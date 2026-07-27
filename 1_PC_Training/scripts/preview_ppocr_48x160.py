"""Preview the PP-OCR recognition preprocessing used by the RKNN C++ demo."""

from __future__ import annotations

import argparse
import math
import os
import random
import shutil
from pathlib import Path

import cv2
import numpy as np


IMAGE_SUFFIXES = {".jpg", ".jpeg", ".png", ".bmp"}
TARGET_HEIGHT = 48
TARGET_WIDTH = 160


def parse_args() -> argparse.Namespace:
    project_dir = Path(__file__).resolve().parents[1]
    dataset_dir = project_dir / "datasets" / "CBLPRD-330k"

    parser = argparse.ArgumentParser(
        description="Randomly preview 48x160 PP-OCR resize, normalization and right padding."
    )
    parser.add_argument(
        "--input-dir",
        type=Path,
        default=dataset_dir / "train",
        help="Directory containing source plate images.",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=dataset_dir / "PP-OCRv4_48x160_preview",
        help="Directory for copied originals and processed previews.",
    )
    parser.add_argument("--count", type=int, default=6, help="Number of images to sample.")
    parser.add_argument("--seed", type=int, default=20260716, help="Random seed.")
    return parser.parse_args()


def read_bgr(path: Path) -> np.ndarray:
    encoded = np.fromfile(path, dtype=np.uint8)
    image = cv2.imdecode(encoded, cv2.IMREAD_COLOR)
    if image is None:
        raise ValueError(f"Failed to decode image: {path}")
    return image


def write_image(path: Path, image: np.ndarray) -> None:
    suffix = path.suffix or ".png"
    success, encoded = cv2.imencode(suffix, image)
    if not success:
        raise ValueError(f"Failed to encode image: {path}")
    encoded.tofile(path)


def sample_images(input_dir: Path, count: int, seed: int) -> list[Path]:
    """Uniformly sample files without loading every dataset path into memory."""
    rng = random.Random(seed)
    selected: list[Path] = []
    image_count = 0

    with os.scandir(input_dir) as entries:
        for entry in entries:
            if not entry.is_file() or Path(entry.name).suffix.lower() not in IMAGE_SUFFIXES:
                continue

            image_count += 1
            path = Path(entry.path)
            if len(selected) < count:
                selected.append(path)
                continue

            replacement = rng.randrange(image_count)
            if replacement < count:
                selected[replacement] = path

    if image_count < count:
        raise ValueError(f"Requested {count} images, but only found {image_count} in {input_dir}")
    return selected


def preprocess_bgr(image: np.ndarray) -> tuple[np.ndarray, int]:
    """Return the normalized CHW tensor and its valid resized width."""
    source_height, source_width = image.shape[:2]
    resized_width = min(
        math.ceil(TARGET_HEIGHT * source_width / float(source_height)),
        TARGET_WIDTH,
    )

    resized = cv2.resize(
        image,
        (resized_width, TARGET_HEIGHT),
        interpolation=cv2.INTER_LINEAR,
    )
    normalized = (resized.astype(np.float32) - 127.5) / 127.5

    padded = np.zeros((TARGET_HEIGHT, TARGET_WIDTH, 3), dtype=np.float32)
    padded[:, :resized_width, :] = normalized
    tensor = padded.transpose(2, 0, 1)

    if resized_width < TARGET_WIDTH:
        assert np.count_nonzero(tensor[:, :, resized_width:]) == 0
    return tensor, resized_width


def tensor_to_bgr_preview(tensor: np.ndarray) -> np.ndarray:
    image = tensor.transpose(1, 2, 0) * 127.5 + 127.5
    return np.clip(np.rint(image), 0, 255).astype(np.uint8)


def make_contact_sheet(rows: list[tuple[np.ndarray, np.ndarray, str]]) -> np.ndarray:
    scale = 3
    gap = 16
    label_height = 30
    panel_width = TARGET_WIDTH * scale
    panel_height = TARGET_HEIGHT * scale
    row_height = label_height + panel_height + gap
    sheet = np.full(
        (row_height * len(rows), panel_width * 2 + gap * 3, 3),
        245,
        dtype=np.uint8,
    )

    for index, (original, processed, name) in enumerate(rows):
        y0 = index * row_height
        original_canvas = np.full((TARGET_HEIGHT, TARGET_WIDTH, 3), 255, dtype=np.uint8)
        source_height, source_width = original.shape[:2]
        display_width = min(
            math.ceil(TARGET_HEIGHT * source_width / float(source_height)),
            TARGET_WIDTH,
        )
        original_canvas[:, :display_width] = cv2.resize(
            original, (display_width, TARGET_HEIGHT), interpolation=cv2.INTER_LINEAR
        )

        left = cv2.resize(original_canvas, (panel_width, panel_height), interpolation=cv2.INTER_NEAREST)
        right = cv2.resize(processed, (panel_width, panel_height), interpolation=cv2.INTER_NEAREST)
        left_x = gap
        right_x = panel_width + gap * 2
        image_y = y0 + label_height
        sheet[image_y:image_y + panel_height, left_x:left_x + panel_width] = left
        sheet[image_y:image_y + panel_height, right_x:right_x + panel_width] = right
        padding_boundary_x = right_x + display_width * scale
        if display_width < TARGET_WIDTH:
            cv2.line(
                sheet,
                (padding_boundary_x, image_y),
                (padding_boundary_x, image_y + panel_height - 1),
                (0, 0, 255),
                2,
            )

        cv2.putText(
            sheet,
            f"{index + 1:02d} original: {name}",
            (left_x, y0 + 21),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.48,
            (25, 25, 25),
            1,
            cv2.LINE_AA,
        )
        cv2.putText(
            sheet,
            f"RKNN/Paddle: 48x160, right pad {TARGET_WIDTH - display_width}px",
            (right_x, y0 + 21),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.48,
            (25, 25, 25),
            1,
            cv2.LINE_AA,
        )

    return sheet


def main() -> None:
    args = parse_args()
    if args.count <= 0:
        raise ValueError("--count must be greater than zero")
    if not args.input_dir.is_dir():
        raise FileNotFoundError(f"Input directory does not exist: {args.input_dir}")

    original_dir = args.output_dir / "original"
    processed_dir = args.output_dir / "processed"
    original_dir.mkdir(parents=True, exist_ok=True)
    processed_dir.mkdir(parents=True, exist_ok=True)

    selected = sample_images(args.input_dir, args.count, args.seed)
    manifest_lines = ["index\tfilename\tsource_size\tresized_size\tright_padding"]
    contact_rows: list[tuple[np.ndarray, np.ndarray, str]] = []

    for index, source_path in enumerate(selected, start=1):
        prefix = f"{index:02d}_"
        copied_path = original_dir / f"{prefix}{source_path.name}"
        processed_path = processed_dir / f"{prefix}{source_path.stem}.png"
        shutil.copy2(source_path, copied_path)

        image = read_bgr(source_path)
        tensor, resized_width = preprocess_bgr(image)
        preview = tensor_to_bgr_preview(tensor)
        write_image(processed_path, preview)

        height, width = image.shape[:2]
        manifest_lines.append(
            f"{index}\t{source_path.name}\t{width}x{height}\t"
            f"{resized_width}x{TARGET_HEIGHT}\t{TARGET_WIDTH - resized_width}"
        )
        contact_rows.append((image, preview, source_path.name))

    manifest_path = args.output_dir / "manifest.tsv"
    manifest_path.write_text("\n".join(manifest_lines) + "\n", encoding="utf-8")
    contact_sheet_path = args.output_dir / "contact_sheet.png"
    write_image(contact_sheet_path, make_contact_sheet(contact_rows))

    print(f"Sampled {len(selected)} images with seed {args.seed}")
    print(f"Output directory: {args.output_dir.resolve()}")
    print(f"Contact sheet: {contact_sheet_path.resolve()}")


if __name__ == "__main__":
    main()
