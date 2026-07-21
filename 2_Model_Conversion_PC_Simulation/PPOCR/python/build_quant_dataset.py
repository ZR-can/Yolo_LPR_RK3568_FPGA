#!/usr/bin/env python3
"""Build a balanced PP-OCRv4 RKNN post-training calibration set."""

from __future__ import annotations

import argparse
import math
import random
import re
from dataclasses import dataclass
from pathlib import Path

import cv2
import numpy as np


IMAGE_HEIGHT = 48
IMAGE_WIDTH = 160
PAD_VALUE = 128
SPECIAL_TYPES = ("使", "学", "港", "澳", "警", "领")

SCRIPT_DIR = Path(__file__).resolve().parent
MODEL_DIR = SCRIPT_DIR.parent / "model"
DEFAULT_OUTPUT_DIR = MODEL_DIR / "quant_images"
DEFAULT_DATASET = MODEL_DIR / "quant_dataset.txt"
DEFAULT_AUDIT_MANIFEST = MODEL_DIR / "quant_manifest.tsv"


@dataclass(frozen=True)
class Sample:
    subset: str
    image_path: Path
    label: str


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Sample all eight CBLPRD plate subsets and generate lossless 48x160 "
            "BGR calibration images for RKNN Toolkit2."
        )
    )
    parser.add_argument(
        "--manifest-dir",
        type=Path,
        required=True,
        help="Directory containing train_basic.txt, train_hard.txt, and train_special_*.txt.",
    )
    parser.add_argument(
        "--data-dir",
        type=Path,
        help="Base directory for image paths in manifests (default: manifest directory).",
    )
    parser.add_argument("--split", choices=("train", "val"), default="train")
    parser.add_argument(
        "--samples",
        type=int,
        default=80,
        help="Total samples balanced across eight subsets (default: 80).",
    )
    parser.add_argument("--seed", type=int, default=20260720)
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT_DIR)
    parser.add_argument("--dataset", type=Path, default=DEFAULT_DATASET)
    parser.add_argument(
        "--audit-manifest", type=Path, default=DEFAULT_AUDIT_MANIFEST
    )
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument(
        "--overwrite",
        action="store_true",
        help="Replace only quant_*.png files previously generated in output-dir.",
    )
    args = parser.parse_args()
    if args.samples <= 0:
        parser.error("--samples must be greater than zero")
    return args


def subset_manifests(manifest_dir: Path, split: str) -> list[tuple[str, Path]]:
    names = [("basic", f"{split}_basic.txt"), ("hard", f"{split}_hard.txt")]
    names.extend((plate_type, f"{split}_special_{plate_type}.txt") for plate_type in SPECIAL_TYPES)
    manifests = [(subset, manifest_dir / name) for subset, name in names]
    missing = [path for _, path in manifests if not path.is_file()]
    if missing:
        raise FileNotFoundError(
            "Missing subset manifests:\n" + "\n".join(str(path) for path in missing)
        )
    return manifests


def parse_manifest_row(raw_line: str, manifest: Path, line_number: int) -> tuple[str, str]:
    fields = raw_line.rstrip("\r\n").split("\t")
    if len(fields) != 2 or not fields[0] or not fields[1]:
        raise ValueError(f"Invalid path<TAB>label row at {manifest}:{line_number}")
    return fields[0], fields[1]


def reservoir_sample(
    manifest: Path,
    subset: str,
    data_dir: Path,
    count: int,
    rng: random.Random,
) -> tuple[list[Sample], int]:
    selected: list[Sample] = []
    available = 0
    with manifest.open("r", encoding="utf-8-sig") as file_handle:
        for line_number, raw_line in enumerate(file_handle, start=1):
            if not raw_line.strip():
                continue
            relative_path, label = parse_manifest_row(raw_line, manifest, line_number)
            # Resolve and stat only the final reservoir, not every row in the
            # 250k-image training manifests.
            sample = Sample(subset, data_dir / relative_path, label)
            available += 1
            if len(selected) < count:
                selected.append(sample)
            else:
                replacement = rng.randrange(available)
                if replacement < count:
                    selected[replacement] = sample

    if available < count:
        raise ValueError(
            f"Subset {subset} has {available} rows, fewer than requested quota {count}"
        )
    resolved_samples: list[Sample] = []
    for sample in selected:
        image_path = sample.image_path.resolve()
        if not image_path.is_file():
            raise FileNotFoundError(
                f"Selected image from {manifest} does not exist: {image_path}"
            )
        resolved_samples.append(Sample(sample.subset, image_path, sample.label))
    return resolved_samples, available


def read_bgr(path: Path) -> np.ndarray:
    encoded = np.fromfile(path, dtype=np.uint8)
    image = cv2.imdecode(encoded, cv2.IMREAD_COLOR)
    if image is None:
        raise ValueError(f"Cannot decode image: {path}")
    return image


def resize_and_pad_bgr(image: np.ndarray) -> tuple[np.ndarray, int]:
    if image.ndim != 3 or image.shape[2] != 3 or min(image.shape[:2]) <= 0:
        raise ValueError(f"Invalid BGR image shape: {image.shape}")
    height, width = image.shape[:2]
    resized_width = min(
        IMAGE_WIDTH, int(math.ceil(IMAGE_HEIGHT * width / float(height)))
    )
    resized = cv2.resize(
        image, (resized_width, IMAGE_HEIGHT), interpolation=cv2.INTER_LINEAR
    )
    padded = np.full(
        (IMAGE_HEIGHT, IMAGE_WIDTH, 3), PAD_VALUE, dtype=np.uint8
    )
    padded[:, :resized_width] = resized
    return padded, resized_width


def write_png(path: Path, image: np.ndarray) -> None:
    success, encoded = cv2.imencode(".png", image)
    if not success:
        raise RuntimeError(f"Failed to encode PNG: {path}")
    encoded.tofile(path)


def prepare_output(output_dir: Path, overwrite: bool) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    existing = list(output_dir.iterdir())
    if not existing:
        return
    unexpected = [
        path
        for path in existing
        if not (path.is_file() and re.fullmatch(r"quant_\d{3}\.png", path.name))
    ]
    if unexpected:
        raise FileExistsError(
            f"Output directory contains non-generated files and will not be changed: {output_dir}"
        )
    if not overwrite:
        raise FileExistsError(
            f"Output directory is not empty: {output_dir}; pass --overwrite to regenerate"
        )
    for path in existing:
        path.unlink()


def main() -> None:
    args = parse_args()
    manifest_dir = args.manifest_dir.resolve()
    data_dir = args.data_dir.resolve() if args.data_dir else manifest_dir
    manifests = subset_manifests(manifest_dir, args.split)
    if args.samples < len(manifests):
        raise ValueError(
            f"--samples must be at least {len(manifests)} to cover every subset"
        )

    quota, remainder = divmod(args.samples, len(manifests))
    rng = random.Random(args.seed)
    selected: list[Sample] = []
    summary: list[tuple[str, int, int]] = []
    for index, (subset, manifest) in enumerate(manifests):
        subset_count = quota + int(index < remainder)
        samples, available = reservoir_sample(
            manifest, subset, data_dir, subset_count, rng
        )
        selected.extend(samples)
        summary.append((subset, available, len(samples)))
    rng.shuffle(selected)

    print(f"split: {args.split}, seed: {args.seed}, selected: {len(selected)}")
    for subset, available, count in summary:
        print(f"  {subset}: available={available}, selected={count}")
    if args.dry_run:
        print("Dry run passed; no calibration files were written.")
        return

    output_dir = args.output_dir.resolve()
    dataset_path = args.dataset.resolve()
    audit_path = args.audit_manifest.resolve()
    prepare_output(output_dir, args.overwrite)
    dataset_path.parent.mkdir(parents=True, exist_ok=True)
    audit_path.parent.mkdir(parents=True, exist_ok=True)

    dataset_rows: list[str] = []
    audit_rows = ["quant_image\tsubset\tresized_width\tsource\tlabel\n"]
    for index, sample in enumerate(selected):
        image = read_bgr(sample.image_path)
        prepared, resized_width = resize_and_pad_bgr(image)
        output_path = output_dir / f"quant_{index:03d}.png"
        if " " in str(output_path):
            raise ValueError(f"RKNN dataset paths cannot contain spaces: {output_path}")
        write_png(output_path, prepared)
        dataset_rows.append(f"{output_path.as_posix()}\n")
        audit_rows.append(
            f"{output_path.as_posix()}\t{sample.subset}\t{resized_width}\t"
            f"{sample.image_path.as_posix()}\t{sample.label}\n"
        )

    with dataset_path.open("w", encoding="utf-8", newline="\n") as file_handle:
        file_handle.writelines(dataset_rows)
    with audit_path.open("w", encoding="utf-8", newline="\n") as file_handle:
        file_handle.writelines(audit_rows)
    print(f"calibration images: {output_dir}")
    print(f"RKNN dataset: {dataset_path}")
    print(f"audit manifest: {audit_path}")


if __name__ == "__main__":
    main()
