"""Build a hard-linked, class-balanced RKNN calibration set for plate OBB."""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import random
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path

from PIL import Image


CLASS_NAMES = ("blue", "green", "yellow_single", "other")
SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPT_DIR.parents[2]
DEFAULT_DATASET_ROOT = PROJECT_ROOT / "1_PC_Training/datasets/yolo_obb_640"
DEFAULT_OUTPUT_DIR = SCRIPT_DIR.parent / "model"


@dataclass(frozen=True)
class ClassGeometry:
    short_side_640: float
    tilt_deg: float


@dataclass(frozen=True)
class Sample:
    source_kind: str
    source_partition: str
    source_image: Path
    image_path: Path
    label_path: Path
    class_ids: tuple[int, ...]
    box_count: int
    geometry: dict[int, ClassGeometry]


@dataclass(frozen=True)
class Selection:
    sample: Sample
    primary_class: int
    size_quartile: int
    angle_bin: str


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Select existing OBB train images for RKNN calibration and expose "
            "them through hard links without copying image data."
        )
    )
    parser.add_argument("--dataset-root", type=Path, default=DEFAULT_DATASET_ROOT)
    parser.add_argument("--manifest", type=Path, default=None)
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT_DIR)
    parser.add_argument("--per-class", type=int, default=200)
    parser.add_argument("--seed", type=int, default=20260818)
    parser.add_argument("--force", action="store_true")
    return parser.parse_args()


def label_path_for(dataset_root: Path, image_relative: str) -> Path:
    parts = Path(image_relative.replace("\\", "/")).parts
    if not parts or parts[0] != "images":
        raise ValueError(f"Unexpected output_image path: {image_relative}")
    return (dataset_root / "labels" / Path(*parts[1:])).with_suffix(".txt")


def normalized_tilt_deg(dx: float, dy: float) -> float:
    angle = abs(math.degrees(math.atan2(dy, dx))) % 180.0
    return 180.0 - angle if angle > 90.0 else angle


def read_geometry(
    image_path: Path, label_path: Path
) -> tuple[tuple[int, ...], dict[int, ClassGeometry]]:
    with Image.open(image_path) as image:
        width, height = image.size
    scale_640 = min(640.0 / width, 640.0 / height)

    by_class: dict[int, list[tuple[float, float]]] = defaultdict(list)
    for line_number, raw_line in enumerate(
        label_path.read_text(encoding="utf-8").splitlines(), start=1
    ):
        fields = raw_line.split()
        if not fields:
            continue
        if len(fields) != 9:
            raise ValueError(
                f"{label_path}:{line_number}: expected 9 fields, got {len(fields)}"
            )
        class_id = int(fields[0])
        if not 0 <= class_id < len(CLASS_NAMES):
            raise ValueError(f"{label_path}:{line_number}: invalid class {class_id}")
        coords = [float(value) for value in fields[1:]]
        points = [
            (coords[index] * width, coords[index + 1] * height)
            for index in range(0, 8, 2)
        ]
        edges = []
        for index, point in enumerate(points):
            next_point = points[(index + 1) % 4]
            dx = next_point[0] - point[0]
            dy = next_point[1] - point[1]
            edges.append((math.hypot(dx, dy), dx, dy))
        short_side = min(length for length, _, _ in edges) * scale_640
        _, long_dx, long_dy = max(edges, key=lambda edge: edge[0])
        by_class[class_id].append(
            (short_side, normalized_tilt_deg(long_dx, long_dy))
        )

    geometry = {
        class_id: ClassGeometry(
            short_side_640=min(value[0] for value in values),
            tilt_deg=max(value[1] for value in values),
        )
        for class_id, values in by_class.items()
    }
    return tuple(sorted(by_class)), geometry


def load_samples(dataset_root: Path, manifest_path: Path) -> list[Sample]:
    if not dataset_root.is_dir():
        raise FileNotFoundError(f"Dataset root not found: {dataset_root}")
    if not manifest_path.is_file():
        raise FileNotFoundError(f"Dataset manifest not found: {manifest_path}")

    samples: list[Sample] = []
    seen_sources: set[str] = set()
    with manifest_path.open(encoding="utf-8", newline="") as file:
        for row in csv.DictReader(file, delimiter="\t"):
            if row["split"] != "train" or int(row["repeat_index"]) != 0:
                continue
            if int(row["box_count"]) <= 0:
                continue
            source_key = os.path.normcase(os.path.normpath(row["source_image"]))
            if source_key in seen_sources:
                continue

            image_relative = row["output_image"].replace("\\", "/")
            image_path = dataset_root / Path(image_relative)
            label_path = label_path_for(dataset_root, image_relative)
            if not image_path.is_file() or not label_path.is_file():
                raise FileNotFoundError(
                    f"Missing train pair: image={image_path}, label={label_path}"
                )
            class_ids, geometry = read_geometry(image_path, label_path)
            if not class_ids:
                continue
            manifest_classes = tuple(
                sorted({int(value) for value in row["class_ids"].split(",") if value})
            )
            if class_ids != manifest_classes:
                raise ValueError(
                    f"Class mismatch for {image_path}: label={class_ids}, "
                    f"manifest={manifest_classes}"
                )
            samples.append(
                Sample(
                    source_kind=row["source_kind"],
                    source_partition=row["source_partition"],
                    source_image=Path(row["source_image"]),
                    image_path=image_path.resolve(),
                    label_path=label_path.resolve(),
                    class_ids=class_ids,
                    box_count=int(row["box_count"]),
                    geometry=geometry,
                )
            )
            seen_sources.add(source_key)
    return samples


def angle_bin(angle: float) -> str:
    if angle < 5.0:
        return "0-5"
    if angle < 15.0:
        return "5-15"
    if angle < 30.0:
        return "15-30"
    return "30-90"


def size_quartiles(samples: list[Sample], class_id: int) -> dict[Path, int]:
    ordered = sorted(
        samples,
        key=lambda sample: (
            sample.geometry[class_id].short_side_640,
            sample.image_path.as_posix(),
        ),
    )
    count = len(ordered)
    return {
        sample.image_path: min(3, index * 4 // count)
        for index, sample in enumerate(ordered)
    }


def choose_for_class(
    candidates: list[Sample], class_id: int, quota: int, rng: random.Random
) -> list[Selection]:
    if len(candidates) < quota:
        raise ValueError(
            f"Class {CLASS_NAMES[class_id]} has only {len(candidates)} unique "
            f"unselected train images; requested {quota}"
        )
    quartiles = size_quartiles(candidates, class_id)
    strata: dict[tuple[str, str, int, str, bool], list[Sample]] = defaultdict(list)
    for sample in candidates:
        geometry = sample.geometry[class_id]
        key = (
            sample.source_kind,
            sample.source_partition,
            quartiles[sample.image_path],
            angle_bin(geometry.tilt_deg),
            sample.box_count > 1,
        )
        strata[key].append(sample)

    for values in strata.values():
        values.sort(key=lambda sample: sample.image_path.as_posix())
        rng.shuffle(values)

    selected: list[Selection] = []
    keys = sorted(strata)
    while len(selected) < quota:
        progressed = False
        for key in keys:
            if not strata[key]:
                continue
            sample = strata[key].pop()
            selected.append(
                Selection(
                    sample=sample,
                    primary_class=class_id,
                    size_quartile=quartiles[sample.image_path],
                    angle_bin=angle_bin(sample.geometry[class_id].tilt_deg),
                )
            )
            progressed = True
            if len(selected) == quota:
                break
        if not progressed:
            raise RuntimeError(f"Unable to fill class quota for {CLASS_NAMES[class_id]}")
    return selected


def select_samples(samples: list[Sample], per_class: int, seed: int) -> list[Selection]:
    rng = random.Random(seed)
    selected: list[Selection] = []
    used_images: set[Path] = set()
    # Reserve multi-class samples for rare classes first, while keeping exactly one
    # primary-class quota assignment per selected image.
    for class_id in (3, 2, 1, 0):
        candidates = [
            sample
            for sample in samples
            if class_id in sample.class_ids and sample.image_path not in used_images
        ]
        chosen = choose_for_class(candidates, class_id, per_class, rng)
        selected.extend(chosen)
        used_images.update(item.sample.image_path for item in chosen)
    return sorted(selected, key=lambda item: (item.primary_class, item.sample.image_path.as_posix()))


def ensure_writable_outputs(paths: list[Path], force: bool) -> None:
    existing = [path for path in paths if path.exists()]
    if existing and not force:
        joined = "\n  ".join(str(path) for path in existing)
        raise FileExistsError(f"Output files already exist; pass --force:\n  {joined}")
    for path in paths:
        path.parent.mkdir(parents=True, exist_ok=True)


def reset_generated_link_dir(path: Path, force: bool) -> None:
    if not path.exists():
        path.mkdir(parents=True)
        return
    if not force:
        raise FileExistsError(f"Calibration link directory exists; pass --force: {path}")
    if path.is_symlink() or not path.is_dir():
        raise RuntimeError(f"Refusing to replace non-directory calibration path: {path}")
    for child in path.iterdir():
        if (
            child.is_symlink()
            or not child.is_file()
            or not child.name.startswith("quant_")
        ):
            raise RuntimeError(f"Refusing to remove unexpected calibration entry: {child}")
        child.unlink()
    path.rmdir()
    path.mkdir(parents=True)


def write_outputs(
    selections: list[Selection], output_dir: Path, per_class: int, seed: int, force: bool
) -> tuple[Path, Path, Path]:
    total = per_class * len(CLASS_NAMES)
    list_path = output_dir / f"obb_quant_dataset_{total}.txt"
    manifest_path = output_dir / f"obb_quant_manifest_{total}.tsv"
    summary_path = output_dir / f"obb_quant_summary_{total}.json"
    link_dir = output_dir / f"obb_quant_dataset_{total}"
    ensure_writable_outputs([list_path, manifest_path, summary_path], force)
    reset_generated_link_dir(link_dir, force)

    hardlinks: list[Path] = []
    for index, item in enumerate(selections):
        suffix = item.sample.image_path.suffix.lower()
        destination = link_dir / f"quant_{index:04d}{suffix}"
        os.link(item.sample.image_path, destination)
        if not os.path.samefile(item.sample.image_path, destination):
            raise RuntimeError(
                f"Hard-link verification failed: {item.sample.image_path} -> {destination}"
            )
        hardlinks.append(destination)

    list_lines = [
        f"./{link_dir.name}/{path.name}"
        for path in hardlinks
    ]
    if len(list_lines) != len(set(line.casefold() for line in list_lines)):
        raise RuntimeError("Calibration list contains duplicate image paths")
    list_path.write_text("\n".join(list_lines) + "\n", encoding="utf-8", newline="\n")

    with manifest_path.open("w", encoding="utf-8", newline="") as file:
        fieldnames = (
            "selection_index",
            "primary_class_id",
            "primary_class",
            "class_ids",
            "classes",
            "source_kind",
            "source_partition",
            "source_image",
            "dataset_image",
            "calibration_hardlink",
            "label_path",
            "box_count",
            "short_side_640",
            "tilt_deg",
            "size_quartile",
            "angle_bin",
        )
        writer = csv.DictWriter(file, fieldnames=fieldnames, delimiter="\t", lineterminator="\n")
        writer.writeheader()
        for index, (item, hardlink) in enumerate(zip(selections, hardlinks)):
            sample = item.sample
            geometry = sample.geometry[item.primary_class]
            writer.writerow(
                {
                    "selection_index": index,
                    "primary_class_id": item.primary_class,
                    "primary_class": CLASS_NAMES[item.primary_class],
                    "class_ids": ",".join(map(str, sample.class_ids)),
                    "classes": ",".join(CLASS_NAMES[value] for value in sample.class_ids),
                    "source_kind": sample.source_kind,
                    "source_partition": sample.source_partition,
                    "source_image": sample.source_image,
                    "dataset_image": sample.image_path,
                    "calibration_hardlink": hardlink,
                    "label_path": sample.label_path,
                    "box_count": sample.box_count,
                    "short_side_640": f"{geometry.short_side_640:.6f}",
                    "tilt_deg": f"{geometry.tilt_deg:.6f}",
                    "size_quartile": item.size_quartile,
                    "angle_bin": item.angle_bin,
                }
            )

    primary_counts = Counter(item.primary_class for item in selections)
    coverage_counts = Counter(
        class_id for item in selections for class_id in item.sample.class_ids
    )
    summary = {
        "status": "VALID",
        "seed": seed,
        "per_class": per_class,
        "total_unique_images": len(selections),
        "storage_mode": "hardlinks_to_existing_train_images_no_pixel_copy",
        "hardlink_count": len(hardlinks),
        "hardlink_directory": link_dir.name,
        "primary_class_counts": {
            CLASS_NAMES[class_id]: primary_counts[class_id]
            for class_id in range(len(CLASS_NAMES))
        },
        "image_class_coverage": {
            CLASS_NAMES[class_id]: coverage_counts[class_id]
            for class_id in range(len(CLASS_NAMES))
        },
        "primary_angle_bins": {
            CLASS_NAMES[class_id]: dict(
                sorted(
                    Counter(
                        item.angle_bin
                        for item in selections
                        if item.primary_class == class_id
                    ).items()
                )
            )
            for class_id in range(len(CLASS_NAMES))
        },
        "primary_size_quartiles": {
            CLASS_NAMES[class_id]: {
                str(quartile): sum(
                    item.primary_class == class_id and item.size_quartile == quartile
                    for item in selections
                )
                for quartile in range(4)
            }
            for class_id in range(len(CLASS_NAMES))
        },
        "source_kinds": dict(
            sorted(Counter(item.sample.source_kind for item in selections).items())
        ),
        "source_partitions": dict(
            sorted(
                Counter(
                    f"{item.sample.source_kind}/{item.sample.source_partition}"
                    for item in selections
                ).items()
            )
        ),
        "multi_plate_images": sum(item.sample.box_count > 1 for item in selections),
        "list_file": list_path.name,
        "manifest_file": manifest_path.name,
    }
    summary_path.write_text(
        json.dumps(summary, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
        newline="\n",
    )
    return list_path, manifest_path, summary_path


def main() -> int:
    args = parse_args()
    if args.per_class <= 0:
        raise ValueError("--per-class must be positive")
    dataset_root = args.dataset_root.resolve()
    manifest_path = (
        args.manifest.resolve()
        if args.manifest is not None
        else dataset_root / "dataset_manifest.tsv"
    )
    samples = load_samples(dataset_root, manifest_path)
    selections = select_samples(samples, args.per_class, args.seed)
    outputs = write_outputs(
        selections, args.output_dir.resolve(), args.per_class, args.seed, args.force
    )
    print(f"Validated unique train candidates: {len(samples)}")
    print(f"Selected calibration images: {len(selections)}")
    for class_id, class_name in enumerate(CLASS_NAMES):
        count = sum(item.primary_class == class_id for item in selections)
        print(f"  {class_name}: {count}")
    for path in outputs:
        print(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
