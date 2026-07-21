#!/usr/bin/env python3
"""按 YOLO 框裁剪特殊车牌，生成独立 PP-OCRv4 审阅数据集。"""

from __future__ import annotations

import argparse
import math
import sys
from collections import Counter
from dataclasses import dataclass
from pathlib import Path

import cv2
import numpy as np


SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_SOURCE_ROOT = SCRIPT_DIR.parent / "datasets" / "特殊车牌"
DEFAULT_OUTPUT_NAME = "PP-OCRv4_format"
OUTPUT_SIZE = (128, 48)
DUPLICATE_IOU_THRESHOLD = 0.95


@dataclass(frozen=True)
class PlateSample:
    plate_text: str
    category: str
    image_path: Path
    label_path: Path
    yolo_box: tuple[float, float, float, float]
    merged_duplicate_count: int


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "按特殊车牌 YOLO 标签裁剪车牌，等比例填充到 128x48，并生成 "
            "images/车牌号.jpg + labels.txt 审阅集。"
        )
    )
    parser.add_argument(
        "--source-root",
        type=Path,
        default=DEFAULT_SOURCE_ROOT,
        help=f"特殊车牌数据根目录（默认：{DEFAULT_SOURCE_ROOT}）",
    )
    parser.add_argument(
        "--output-root",
        type=Path,
        default=None,
        help=(
            "输出审阅集目录（默认：source-root/"
            f"{DEFAULT_OUTPUT_NAME}）"
        ),
    )
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument(
        "--dry-run",
        action="store_true",
        help="只检查图片、标签和裁剪框，不生成输出",
    )
    action.add_argument(
        "--apply",
        action="store_true",
        help="确认生成独立 PP-OCRv4 审阅集",
    )
    return parser.parse_args()


def read_image(path: Path) -> np.ndarray:
    try:
        encoded = np.fromfile(path, dtype=np.uint8)
        image = cv2.imdecode(encoded, cv2.IMREAD_COLOR)
    except OSError as exc:
        raise OSError(f"读取图片失败：{path}") from exc
    if image is None:
        raise OSError(f"图片无法解码：{path}")
    return image


def write_jpeg(path: Path, image: np.ndarray) -> None:
    success, encoded = cv2.imencode(
        ".jpg", image, [cv2.IMWRITE_JPEG_QUALITY, 95]
    )
    if not success:
        raise OSError(f"JPEG 编码失败：{path}")
    encoded.tofile(path)


def collect_unique_files(root: Path, pattern: str) -> dict[str, Path]:
    files: dict[str, Path] = {}
    for path in sorted(root.rglob(pattern)):
        if path.name == "classes.txt":
            continue
        previous = files.get(path.stem)
        if previous is not None:
            raise ValueError(
                f"文件名重复，无法使用车牌号作为唯一图片名：{previous} / {path}"
            )
        files[path.stem] = path
    return files


def yolo_to_corners(
    box: tuple[float, float, float, float]
) -> tuple[float, float, float, float]:
    x_center, y_center, width, height = box
    return (
        x_center - width / 2,
        y_center - height / 2,
        x_center + width / 2,
        y_center + height / 2,
    )


def box_iou(
    first: tuple[float, float, float, float],
    second: tuple[float, float, float, float],
) -> float:
    first_x1, first_y1, first_x2, first_y2 = yolo_to_corners(first)
    second_x1, second_y1, second_x2, second_y2 = yolo_to_corners(second)
    intersection_width = max(0.0, min(first_x2, second_x2) - max(first_x1, second_x1))
    intersection_height = max(0.0, min(first_y2, second_y2) - max(first_y1, second_y1))
    intersection = intersection_width * intersection_height
    first_area = (first_x2 - first_x1) * (first_y2 - first_y1)
    second_area = (second_x2 - second_x1) * (second_y2 - second_y1)
    union = first_area + second_area - intersection
    return intersection / union if union > 0 else 0.0


def read_yolo_box(
    label_path: Path,
) -> tuple[tuple[float, float, float, float], int]:
    boxes: list[tuple[float, float, float, float]] = []
    with label_path.open("r", encoding="utf-8-sig") as file:
        for line_no, raw_line in enumerate(file, start=1):
            stripped = raw_line.strip()
            if not stripped:
                continue
            fields = stripped.split()
            if len(fields) != 5:
                raise ValueError(
                    f"{label_path}:{line_no} 不是 YOLO class xc yc w h 格式"
                )
            if fields[0] != "0":
                raise ValueError(
                    f"{label_path}:{line_no} 类别不是 0：{fields[0]}"
                )
            try:
                box = tuple(float(value) for value in fields[1:])
            except ValueError as exc:
                raise ValueError(
                    f"{label_path}:{line_no} YOLO 坐标不是数值"
                ) from exc
            if (
                len(box) != 4
                or not all(math.isfinite(value) for value in box)
                or not all(0.0 <= value <= 1.0 for value in box)
                or box[2] <= 0.0
                or box[3] <= 0.0
            ):
                raise ValueError(f"{label_path}:{line_no} YOLO 坐标越界：{box}")
            boxes.append(box)  # type: ignore[arg-type]

    if not boxes:
        raise ValueError(f"YOLO 标签为空：{label_path}")
    if len(boxes) == 1:
        return boxes[0], 0

    for index, first in enumerate(boxes):
        for second in boxes[index + 1 :]:
            iou = box_iou(first, second)
            if iou < DUPLICATE_IOU_THRESHOLD:
                raise ValueError(
                    f"{label_path} 含多个不同车牌框，最小 IoU={iou:.4f}"
                )
    merged = tuple(
        sum(box[position] for box in boxes) / len(boxes)
        for position in range(4)
    )
    return merged, len(boxes) - 1  # type: ignore[return-value]


def build_samples(source_root: Path) -> list[PlateSample]:
    image_root = source_root / "image"
    label_root = source_root / "txt"
    if not image_root.is_dir() or not label_root.is_dir():
        raise FileNotFoundError(
            f"源目录必须包含 image/ 和 txt/：{source_root}"
        )

    images = collect_unique_files(image_root, "*.jpg")
    labels = collect_unique_files(label_root, "*.txt")
    missing_labels = sorted(set(images) - set(labels))
    missing_images = sorted(set(labels) - set(images))
    if missing_labels or missing_images:
        raise ValueError(
            f"图片/标签文件名不一一对应：缺标签 {missing_labels[:5]}，"
            f"缺图片 {missing_images[:5]}"
        )

    samples: list[PlateSample] = []
    for plate_text in sorted(images):
        if not plate_text or any(character.isspace() for character in plate_text):
            raise ValueError(f"车牌文件名为空或含空白：{plate_text!r}")
        label_path = labels[plate_text]
        category = label_path.parent.name
        if not plate_text.endswith(category):
            raise ValueError(
                f"车牌名末字与标签类别不一致：{plate_text} / {category}"
            )
        yolo_box, merged_count = read_yolo_box(label_path)
        samples.append(
            PlateSample(
                plate_text=plate_text,
                category=category,
                image_path=images[plate_text],
                label_path=label_path,
                yolo_box=yolo_box,
                merged_duplicate_count=merged_count,
            )
        )
    return samples


def crop_from_yolo(image: np.ndarray, box: tuple[float, float, float, float]) -> np.ndarray:
    image_height, image_width = image.shape[:2]
    x1, y1, x2, y2 = yolo_to_corners(box)
    if x1 < 0.0 or y1 < 0.0 or x2 > 1.0 or y2 > 1.0:
        raise ValueError(f"YOLO 框越过图片边界：{box}")
    left = max(0, math.floor(x1 * image_width))
    top = max(0, math.floor(y1 * image_height))
    right = min(image_width, math.ceil(x2 * image_width))
    bottom = min(image_height, math.ceil(y2 * image_height))
    if right <= left or bottom <= top:
        raise ValueError(f"YOLO 框转换后为空：{box}")
    return image[top:bottom, left:right]


def letterbox_to_output(crop: np.ndarray) -> np.ndarray:
    target_width, target_height = OUTPUT_SIZE
    crop_height, crop_width = crop.shape[:2]
    scale = min(target_width / crop_width, target_height / crop_height)
    resized_width = max(1, round(crop_width * scale))
    resized_height = max(1, round(crop_height * scale))
    resized = cv2.resize(
        crop,
        (resized_width, resized_height),
        interpolation=cv2.INTER_AREA,
    )
    top = (target_height - resized_height) // 2
    bottom = target_height - resized_height - top
    left = (target_width - resized_width) // 2
    right = target_width - resized_width - left
    return cv2.copyMakeBorder(
        resized,
        top,
        bottom,
        left,
        right,
        borderType=cv2.BORDER_REPLICATE,
    )


def validate_samples(samples: list[PlateSample]) -> Counter[str]:
    category_counts: Counter[str] = Counter()
    for sample in samples:
        image = read_image(sample.image_path)
        crop = crop_from_yolo(image, sample.yolo_box)
        output = letterbox_to_output(crop)
        if output.shape != (OUTPUT_SIZE[1], OUTPUT_SIZE[0], 3):
            raise ValueError(
                f"输出尺寸异常：{sample.plate_text} / {output.shape}"
            )
        category_counts[sample.category] += 1
    return category_counts


def generate_dataset(samples: list[PlateSample], output_root: Path) -> None:
    if output_root.exists():
        raise FileExistsError(
            f"输出目录已存在；为避免审阅结果混入，请先确认后移走：{output_root}"
        )
    stage_root = output_root.with_name(f".{output_root.name}.tmp")
    if stage_root.exists():
        raise FileExistsError(f"临时输出目录已存在：{stage_root}")
    image_root = stage_root / "images"
    image_root.mkdir(parents=True)

    label_lines: list[str] = []
    for index, sample in enumerate(samples, start=1):
        image = read_image(sample.image_path)
        crop = crop_from_yolo(image, sample.yolo_box)
        output = letterbox_to_output(crop)
        output_name = f"{sample.plate_text}.jpg"
        write_jpeg(image_root / output_name, output)
        label_lines.append(
            f"images/{output_name}\t{sample.plate_text}"
        )
        if index % 50 == 0 or index == len(samples):
            print(f"  已生成 {index:,}/{len(samples):,}")

    label_file = stage_root / "labels.txt"
    with label_file.open("w", encoding="utf-8", newline="\n") as file:
        for line in label_lines:
            file.write(f"{line}\n")

    output_files = {path.name for path in image_root.glob("*.jpg")}
    expected_files = {f"{sample.plate_text}.jpg" for sample in samples}
    if output_files != expected_files:
        raise OSError("生成图片集合与源车牌集合不一致")
    for sample in samples:
        output = read_image(image_root / f"{sample.plate_text}.jpg")
        if output.shape != (OUTPUT_SIZE[1], OUTPUT_SIZE[0], 3):
            raise OSError(f"生成图片尺寸异常：{sample.plate_text}")
    stage_root.rename(output_root)


def print_summary(samples: list[PlateSample], category_counts: Counter[str]) -> None:
    print(f"有效车牌：{len(samples):,}")
    for category, count in sorted(category_counts.items()):
        print(f"  {category}: {count:,}")
    merged = [sample for sample in samples if sample.merged_duplicate_count]
    print(f"合并重复 YOLO 框：{sum(item.merged_duplicate_count for item in merged):,}")
    for sample in merged:
        print(
            f"  {sample.plate_text}: 合并 {sample.merged_duplicate_count + 1} 个高 IoU 框"
        )
    print(f"输出尺寸：{OUTPUT_SIZE[0]}x{OUTPUT_SIZE[1]}")


def main() -> int:
    args = parse_args()
    source_root = args.source_root.expanduser().resolve()
    output_root = (
        args.output_root.expanduser().resolve()
        if args.output_root is not None
        else source_root / DEFAULT_OUTPUT_NAME
    )
    try:
        if not source_root.is_dir():
            raise FileNotFoundError(f"找不到特殊车牌目录：{source_root}")
        samples = build_samples(source_root)
        category_counts = validate_samples(samples)
        print_summary(samples, category_counts)
        if args.dry_run:
            print("Dry-run 完成：未生成输出目录。")
            return 0

        generate_dataset(samples, output_root)
        print(f"审阅数据集生成完成：{output_root}")
        print("尚未追加到 CBLPRD-330k。")
        return 0
    except (OSError, ValueError) as exc:
        print(f"错误：{exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
