#!/usr/bin/env python3
"""审计现有 YOLO 数据集和待追加的特殊车牌数据集。"""

from __future__ import annotations

# %% Imports and constants
import argparse
import hashlib
import json
import math
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable

from PIL import Image, ImageDraw, ImageFont


SCRIPT_DIR = Path(__file__).resolve().parent
TRAINING_ROOT = SCRIPT_DIR.parent
DEFAULT_YOLO_ROOT = TRAINING_ROOT / "datasets" / "yolo_format"
DEFAULT_SPECIAL_ROOT = TRAINING_ROOT / "datasets" / "特殊车牌"
DEFAULT_OUTPUT_ROOT = TRAINING_ROOT / "datasets" / "yolo_finetune_audit"
IMAGE_SUFFIXES = {".jpg", ".jpeg", ".png", ".bmp", ".webp"}
CLASS_NAMES = {
    0: "blue",
    1: "green",
    2: "yellow_single",
    3: "other",
}
SPECIAL_GROUPS = {
    "教练车牌": ("学", "learner"),
    "香港出入境车牌": ("港", "hongkong"),
    "澳门出入境车牌": ("澳", "macau"),
    "警用车牌": ("警", "police"),
}


# %% Data structures
@dataclass(frozen=True)
class Box:
    class_id: int
    x_center: float
    y_center: float
    width: float
    height: float


@dataclass(frozen=True)
class ImageInfo:
    path: Path
    width: int
    height: int
    error: str | None


@dataclass(frozen=True)
class Sample:
    source: str
    split_or_group: str
    image_path: Path
    label_path: Path
    boxes: tuple[Box, ...]
    image_width: int
    image_height: int


# %% CLI
def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="审计原 YOLO 数据集与新增特殊车牌数据，输出 JSON、Markdown 和标注预览图。"
    )
    parser.add_argument("--yolo-root", type=Path, default=DEFAULT_YOLO_ROOT)
    parser.add_argument("--special-root", type=Path, default=DEFAULT_SPECIAL_ROOT)
    parser.add_argument("--output-root", type=Path, default=DEFAULT_OUTPUT_ROOT)
    parser.add_argument(
        "--workers",
        type=int,
        default=8,
        help="图片校验并发数（默认：8）",
    )
    return parser.parse_args()


# %% File and label parsing
def collect_images(root: Path) -> dict[str, Path]:
    images: dict[str, Path] = {}
    for path in sorted(root.rglob("*")):
        if not path.is_file() or path.suffix.lower() not in IMAGE_SUFFIXES:
            continue
        previous = images.get(path.stem)
        if previous is not None:
            raise ValueError(f"图片主文件名重复：{previous} / {path}")
        images[path.stem] = path
    return images


def collect_labels(root: Path) -> dict[str, Path]:
    labels: dict[str, Path] = {}
    for path in sorted(root.rglob("*.txt")):
        if path.name.lower() == "classes.txt":
            continue
        previous = labels.get(path.stem)
        if previous is not None:
            raise ValueError(f"标签主文件名重复：{previous} / {path}")
        labels[path.stem] = path
    return labels


def parse_label(
    path: Path,
    allowed_class_ids: set[int],
) -> tuple[tuple[Box, ...], list[str]]:
    boxes: list[Box] = []
    issues: list[str] = []
    with path.open("r", encoding="utf-8-sig") as file:
        for line_no, raw_line in enumerate(file, start=1):
            stripped = raw_line.strip()
            if not stripped:
                continue
            fields = stripped.split()
            if len(fields) != 5:
                issues.append(
                    f"{path}:{line_no} 应为 5 列，实际为 {len(fields)} 列"
                )
                continue
            try:
                class_id = int(fields[0])
                values = tuple(float(value) for value in fields[1:])
            except ValueError:
                issues.append(f"{path}:{line_no} 含非数值字段")
                continue
            if class_id not in allowed_class_ids:
                issues.append(
                    f"{path}:{line_no} 类别 ID {class_id} 不在 {sorted(allowed_class_ids)}"
                )
                continue
            if not all(math.isfinite(value) for value in values):
                issues.append(f"{path}:{line_no} 含 NaN 或 Inf")
                continue
            x_center, y_center, width, height = values
            if width <= 0 or height <= 0:
                issues.append(f"{path}:{line_no} 框宽高必须大于 0")
                continue
            boxes.append(Box(class_id, x_center, y_center, width, height))
            x1 = x_center - width / 2
            y1 = y_center - height / 2
            x2 = x_center + width / 2
            y2 = y_center + height / 2
            if min(x1, y1) < -1e-6 or max(x2, y2) > 1 + 1e-6:
                issues.append(f"{path}:{line_no} 框越出归一化图像边界")
    if not boxes:
        issues.append(f"无有效框：{path}")
    return tuple(boxes), issues


# %% Image validation
def inspect_image(path: Path) -> ImageInfo:
    try:
        with Image.open(path) as image:
            width, height = image.size
            image.verify()
        if width <= 0 or height <= 0:
            raise ValueError("宽高必须大于 0")
        return ImageInfo(path, width, height, None)
    except Exception as exc:  # Pillow raises multiple decoder-specific exceptions.
        return ImageInfo(path, 0, 0, str(exc))


def inspect_images(
    paths: Iterable[Path], workers: int
) -> dict[Path, ImageInfo]:
    unique_paths = sorted(set(paths))
    with ThreadPoolExecutor(max_workers=workers) as executor:
        infos = list(executor.map(inspect_image, unique_paths))
    return {info.path: info for info in infos}


# %% Dataset collection
def validate_pairing(
    images: dict[str, Path],
    labels: dict[str, Path],
    dataset_name: str,
) -> None:
    missing_labels = sorted(images.keys() - labels.keys())
    missing_images = sorted(labels.keys() - images.keys())
    if missing_labels or missing_images:
        lines = [f"{dataset_name} 图片/标签不一一对应"]
        if missing_labels:
            lines.append(f"缺标签：{missing_labels[:20]}")
        if missing_images:
            lines.append(f"缺图片：{missing_images[:20]}")
        raise ValueError("\n".join(lines))


def collect_old_samples(
    yolo_root: Path,
    image_infos: dict[Path, ImageInfo],
) -> tuple[list[Sample], list[str]]:
    samples: list[Sample] = []
    issues: list[str] = []
    for split in ("train", "val", "test"):
        images = collect_images(yolo_root / "images" / split)
        labels = collect_labels(yolo_root / "labels" / split)
        validate_pairing(images, labels, f"旧数据集/{split}")
        for stem, image_path in images.items():
            info = image_infos[image_path]
            if info.error is not None:
                issues.append(f"图片无法解码：{image_path}: {info.error}")
                continue
            boxes, label_issues = parse_label(labels[stem], set(CLASS_NAMES))
            issues.extend(label_issues)
            samples.append(
                Sample(
                    source="old",
                    split_or_group=split,
                    image_path=image_path,
                    label_path=labels[stem],
                    boxes=boxes,
                    image_width=info.width,
                    image_height=info.height,
                )
            )
    return samples, issues


def collect_special_samples(
    special_root: Path,
    image_infos: dict[Path, ImageInfo],
) -> tuple[list[Sample], list[str]]:
    samples: list[Sample] = []
    issues: list[str] = []
    for image_group, (label_group, _) in SPECIAL_GROUPS.items():
        images = collect_images(special_root / "image" / image_group)
        labels = collect_labels(special_root / "txt" / label_group)
        validate_pairing(images, labels, f"新增数据/{image_group}")
        for stem, image_path in images.items():
            info = image_infos[image_path]
            if info.error is not None:
                issues.append(f"图片无法解码：{image_path}: {info.error}")
                continue
            local_boxes, label_issues = parse_label(labels[stem], {0})
            issues.extend(label_issues)
            samples.append(
                Sample(
                    source="special",
                    split_or_group=image_group,
                    image_path=image_path,
                    label_path=labels[stem],
                    # 特殊车牌各目录的局部 0 类在全局四分类中对应 other=3。
                    boxes=tuple(
                        Box(3, box.x_center, box.y_center, box.width, box.height)
                        for box in local_boxes
                    ),
                    image_width=info.width,
                    image_height=info.height,
                )
            )
    return samples, issues


# %% Statistics and duplicate checks
def percentile(values: list[float], fraction: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def describe(values: Iterable[float]) -> dict[str, float]:
    materialized = list(values)
    return {
        "min": min(materialized, default=0.0),
        "p10": percentile(materialized, 0.10),
        "median": percentile(materialized, 0.50),
        "p90": percentile(materialized, 0.90),
        "max": max(materialized, default=0.0),
    }


def summarize_samples(samples: list[Sample]) -> dict[str, object]:
    boxes = [box for sample in samples for box in sample.boxes]
    return {
        "images": len(samples),
        "boxes": len(boxes),
        "class_counts": {
            CLASS_NAMES[class_id]: sum(box.class_id == class_id for box in boxes)
            for class_id in CLASS_NAMES
        },
        "boxes_per_image": describe(float(len(sample.boxes)) for sample in samples),
        "image_width_px": describe(float(sample.image_width) for sample in samples),
        "image_height_px": describe(float(sample.image_height) for sample in samples),
        "bbox_width_norm": describe(box.width for box in boxes),
        "bbox_height_norm": describe(box.height for box in boxes),
        "bbox_area_norm": describe(box.width * box.height for box in boxes),
        "bbox_width_px": describe(
            box.width * sample.image_width for sample in samples for box in sample.boxes
        ),
        "bbox_height_px": describe(
            box.height * sample.image_height
            for sample in samples
            for box in sample.boxes
        ),
    }


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def find_exact_duplicates(samples: list[Sample]) -> list[list[str]]:
    by_size: dict[int, list[Sample]] = defaultdict(list)
    for sample in samples:
        by_size[sample.image_path.stat().st_size].append(sample)
    duplicate_groups: list[list[str]] = []
    for same_size in by_size.values():
        if len(same_size) < 2:
            continue
        by_hash: dict[str, list[Sample]] = defaultdict(list)
        for sample in same_size:
            by_hash[sha256(sample.image_path)].append(sample)
        for same_hash in by_hash.values():
            locations = {
                f"{sample.source}/{sample.split_or_group}" for sample in same_hash
            }
            if len(same_hash) > 1 and len(locations) > 1:
                duplicate_groups.append(
                    [str(sample.image_path) for sample in same_hash]
                )
    return sorted(duplicate_groups)


def find_stem_overlaps(samples: list[Sample]) -> dict[str, list[str]]:
    by_stem: dict[str, list[Sample]] = defaultdict(list)
    for sample in samples:
        by_stem[sample.image_path.stem].append(sample)
    return {
        stem: [str(sample.image_path) for sample in matching]
        for stem, matching in sorted(by_stem.items())
        if len({f"{sample.source}/{sample.split_or_group}" for sample in matching}) > 1
    }


# %% Preview and reports
def load_font(size: int) -> ImageFont.ImageFont:
    candidates = (
        Path(r"C:\Windows\Fonts\msyh.ttc"),
        Path(r"C:\Windows\Fonts\arial.ttf"),
    )
    for path in candidates:
        if path.is_file():
            return ImageFont.truetype(str(path), size=size)
    return ImageFont.load_default()


def create_special_preview(samples: list[Sample], output_path: Path) -> None:
    cell_width, cell_height = 360, 270
    columns = 4
    selected: list[Sample] = []
    for group in SPECIAL_GROUPS:
        group_samples = [
            sample for sample in samples if sample.split_or_group == group
        ]
        if len(group_samples) < columns:
            raise ValueError(f"{group} 少于 {columns} 张，无法生成预览")
        step = max(1, len(group_samples) // columns)
        selected.extend(group_samples[index * step] for index in range(columns))
    rows = math.ceil(len(selected) / columns)
    canvas = Image.new("RGB", (columns * cell_width, rows * cell_height), "white")
    font = load_font(18)
    for index, sample in enumerate(selected):
        with Image.open(sample.image_path) as source:
            image = source.convert("RGB")
        draw = ImageDraw.Draw(image)
        for box in sample.boxes:
            x1 = (box.x_center - box.width / 2) * image.width
            y1 = (box.y_center - box.height / 2) * image.height
            x2 = (box.x_center + box.width / 2) * image.width
            y2 = (box.y_center + box.height / 2) * image.height
            line_width = max(3, round(max(image.width, image.height) / 400))
            draw.rectangle((x1, y1, x2, y2), outline="red", width=line_width)
        image.thumbnail((cell_width, cell_height - 28), Image.Resampling.LANCZOS)
        cell = Image.new("RGB", (cell_width, cell_height), "#202020")
        x_offset = (cell_width - image.width) // 2
        y_offset = 28 + (cell_height - 28 - image.height) // 2
        cell.paste(image, (x_offset, y_offset))
        english_group = SPECIAL_GROUPS[sample.split_or_group][1]
        ImageDraw.Draw(cell).text(
            (8, 4),
            f"{english_group}: {sample.image_path.stem}",
            fill="white",
            font=font,
        )
        canvas.paste(
            cell,
            ((index % columns) * cell_width, (index // columns) * cell_height),
        )
    output_path.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(output_path, quality=92)


def markdown_stats(stats: dict[str, object]) -> str:
    class_counts = stats["class_counts"]
    return (
        f"{stats['images']:,} | {stats['boxes']:,} | "
        f"{class_counts['blue']:,} | {class_counts['green']:,} | "
        f"{class_counts['yellow_single']:,} | {class_counts['other']:,}"
    )


def create_report(
    old_samples: list[Sample],
    special_samples: list[Sample],
    old_issues: list[str],
    special_issues: list[str],
    exact_duplicates: list[list[str]],
    stem_overlaps: dict[str, list[str]],
) -> dict[str, object]:
    old_by_split = {
        split: summarize_samples(
            [sample for sample in old_samples if sample.split_or_group == split]
        )
        for split in ("train", "val", "test")
    }
    special_by_group = {
        group: summarize_samples(
            [sample for sample in special_samples if sample.split_or_group == group]
        )
        for group in SPECIAL_GROUPS
    }
    return {
        "old": {
            "by_split": old_by_split,
            "total": summarize_samples(old_samples),
            "issues": old_issues,
        },
        "special": {
            "by_group": special_by_group,
            "total": summarize_samples(special_samples),
            "source_local_class_id": 0,
            "target_global_class_id": 3,
            "issues": special_issues,
        },
        "cross_partition_exact_duplicate_groups": exact_duplicates,
        "cross_partition_stem_overlaps": stem_overlaps,
    }


def write_markdown(report: dict[str, object], output_path: Path) -> None:
    old = report["old"]
    special = report["special"]
    old_total = old["total"]
    special_total = special["total"]
    lines = [
        "# YOLO 特殊车牌微调数据审计",
        "",
        "## 旧数据集",
        "",
        "| 划分 | 图片 | 框 | blue | green | yellow_single | other |",
        "|---|---:|---:|---:|---:|---:|---:|",
    ]
    for split in ("train", "val", "test"):
        lines.append(f"| {split} | {markdown_stats(old['by_split'][split])} |")
    lines.extend(
        [
            f"| total | {markdown_stats(old_total)} |",
            "",
            "## 新增特殊车牌",
            "",
            "| 子类 | 图片 | 框 | blue | green | yellow_single | other（重映射后） |",
            "|---|---:|---:|---:|---:|---:|---:|",
        ]
    )
    for group in SPECIAL_GROUPS:
        lines.append(f"| {group} | {markdown_stats(special['by_group'][group])} |")
    lines.extend(
        [
            f"| total | {markdown_stats(special_total)} |",
            "",
            "- 源标签采用各子目录局部类别 `0=other`。",
            "- 合并进现有四分类数据时必须重映射为全局类别 `3=other`。",
            (
                "- 跨来源/划分的完全相同图片组："
                f"{len(report['cross_partition_exact_duplicate_groups'])}。"
            ),
            (
                "- 跨来源/划分的同名图片："
                f"{len(report['cross_partition_stem_overlaps'])}。"
            ),
            f"- 旧数据异常：{len(old['issues'])}。",
            f"- 新增数据异常：{len(special['issues'])}。",
            "",
            "## 尺寸与框尺度（中位数 / P10–P90）",
            "",
            "| 数据 | 图像宽×高(px) | 框宽×高(px) | 归一化框面积 |",
            "|---|---:|---:|---:|",
        ]
    )
    for name, stats in (("旧数据", old_total), ("新增数据", special_total)):
        lines.append(
            "| "
            f"{name} | "
            f"{stats['image_width_px']['median']:.0f}×"
            f"{stats['image_height_px']['median']:.0f} "
            f"({stats['image_width_px']['p10']:.0f}–"
            f"{stats['image_width_px']['p90']:.0f} × "
            f"{stats['image_height_px']['p10']:.0f}–"
            f"{stats['image_height_px']['p90']:.0f}) | "
            f"{stats['bbox_width_px']['median']:.1f}×"
            f"{stats['bbox_height_px']['median']:.1f} "
            f"({stats['bbox_width_px']['p10']:.1f}–"
            f"{stats['bbox_width_px']['p90']:.1f} × "
            f"{stats['bbox_height_px']['p10']:.1f}–"
            f"{stats['bbox_height_px']['p90']:.1f}) | "
            f"{stats['bbox_area_norm']['median']:.6f} "
            f"({stats['bbox_area_norm']['p10']:.6f}–"
            f"{stats['bbox_area_norm']['p90']:.6f}) |"
        )
    output_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


# %% Main
def main() -> int:
    args = parse_args()
    yolo_root = args.yolo_root.resolve()
    special_root = args.special_root.resolve()
    output_root = args.output_root.resolve()
    if not yolo_root.is_dir():
        raise FileNotFoundError(f"找不到旧 YOLO 数据集：{yolo_root}")
    if not special_root.is_dir():
        raise FileNotFoundError(f"找不到新增特殊车牌数据集：{special_root}")

    old_image_paths = [
        path
        for split in ("train", "val", "test")
        for path in collect_images(yolo_root / "images" / split).values()
    ]
    special_image_paths = [
        path
        for group in SPECIAL_GROUPS
        for path in collect_images(special_root / "image" / group).values()
    ]
    print(f"校验图片：旧数据 {len(old_image_paths):,}，新增数据 {len(special_image_paths):,}")
    image_infos = inspect_images(
        [*old_image_paths, *special_image_paths],
        workers=max(1, args.workers),
    )
    old_samples, old_issues = collect_old_samples(yolo_root, image_infos)
    special_samples, special_issues = collect_special_samples(
        special_root,
        image_infos,
    )
    all_samples = [*old_samples, *special_samples]

    print("检查跨来源/划分的同名和完全相同图片...")
    stem_overlaps = find_stem_overlaps(all_samples)
    exact_duplicates = find_exact_duplicates(all_samples)
    report = create_report(
        old_samples,
        special_samples,
        old_issues,
        special_issues,
        exact_duplicates,
        stem_overlaps,
    )

    output_root.mkdir(parents=True, exist_ok=True)
    json_path = output_root / "audit.json"
    markdown_path = output_root / "audit.md"
    preview_path = output_root / "special_bbox_preview.jpg"
    json_path.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    write_markdown(report, markdown_path)
    create_special_preview(special_samples, preview_path)

    print(f"审计完成：{markdown_path}")
    print(f"详细数据：{json_path}")
    print(f"标注预览：{preview_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
