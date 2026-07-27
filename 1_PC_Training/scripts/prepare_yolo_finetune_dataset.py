#!/usr/bin/env python3
"""构建不复制图片数据块的 YOLO 特殊车牌微调数据集。"""

from __future__ import annotations

# %% Imports and constants
import argparse
import csv
import json
import os
import random
from collections import Counter
from dataclasses import dataclass
from pathlib import Path

from analyze_yolo_finetune_dataset import (
    CLASS_NAMES,
    DEFAULT_SPECIAL_ROOT,
    DEFAULT_YOLO_ROOT,
    SPECIAL_GROUPS,
    Box,
    collect_images,
    collect_labels,
    parse_label,
    validate_pairing,
)


SCRIPT_DIR = Path(__file__).resolve().parent
TRAINING_ROOT = SCRIPT_DIR.parent
DEFAULT_AUDIT_JSON = (
    TRAINING_ROOT / "datasets" / "yolo_finetune_audit" / "audit.json"
)
DEFAULT_OUTPUT_ROOT = TRAINING_ROOT / "datasets" / "yolo_finetune_special"
SPLITS = ("train", "val", "test")
SPLIT_PRIORITY = {"train": 0, "val": 1, "test": 2}
SPECIAL_SPLIT_RATIOS = {"train": 0.80, "val": 0.10, "test": 0.10}
DUPLICATE_BOX_IOU = 0.95


# %% Data structures
@dataclass(frozen=True)
class BuildItem:
    split: str
    source_kind: str
    source_partition: str
    source_image: Path
    source_label: Path
    output_stem: str
    repeat_index: int


@dataclass(frozen=True)
class WrittenItem:
    split: str
    source_kind: str
    source_partition: str
    source_image: Path
    source_label: Path
    output_image: Path
    output_label: Path
    repeat_index: int
    boxes: tuple[Box, ...]


# %% CLI
def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "从现有 yolo_format 与特殊车牌构建派生微调集；图片使用 NTFS 硬链接，"
            "特殊车牌局部类别 0 自动重映射为全局 other=3。"
        )
    )
    parser.add_argument("--yolo-root", type=Path, default=DEFAULT_YOLO_ROOT)
    parser.add_argument("--special-root", type=Path, default=DEFAULT_SPECIAL_ROOT)
    parser.add_argument("--audit-json", type=Path, default=DEFAULT_AUDIT_JSON)
    parser.add_argument("--output-root", type=Path, default=DEFAULT_OUTPUT_ROOT)
    parser.add_argument("--seed", type=int, default=20260726)
    parser.add_argument(
        "--special-train-repeats",
        type=int,
        default=3,
        help="新增特殊车牌训练样本的总出现次数（默认：3）",
    )
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--dry-run", action="store_true")
    action.add_argument("--apply", action="store_true")
    return parser.parse_args()


# %% Geometry and labels
def box_iou(first: Box, second: Box) -> float:
    first_x1 = first.x_center - first.width / 2
    first_y1 = first.y_center - first.height / 2
    first_x2 = first.x_center + first.width / 2
    first_y2 = first.y_center + first.height / 2
    second_x1 = second.x_center - second.width / 2
    second_y1 = second.y_center - second.height / 2
    second_x2 = second.x_center + second.width / 2
    second_y2 = second.y_center + second.height / 2
    intersection_width = max(0.0, min(first_x2, second_x2) - max(first_x1, second_x1))
    intersection_height = max(
        0.0,
        min(first_y2, second_y2) - max(first_y1, second_y1),
    )
    intersection = intersection_width * intersection_height
    union = (
        first.width * first.height
        + second.width * second.height
        - intersection
    )
    return intersection / union if union > 0 else 0.0


def merge_duplicate_boxes(boxes: tuple[Box, ...]) -> tuple[Box, ...]:
    merged: list[Box] = []
    merge_counts: list[int] = []
    for box in boxes:
        duplicate_index = next(
            (
                index
                for index, previous in enumerate(merged)
                if previous.class_id == box.class_id
                and box_iou(previous, box) >= DUPLICATE_BOX_IOU
            ),
            None,
        )
        if duplicate_index is None:
            merged.append(box)
            merge_counts.append(1)
            continue
        previous = merged[duplicate_index]
        count = merge_counts[duplicate_index]
        merged[duplicate_index] = Box(
            class_id=box.class_id,
            x_center=(previous.x_center * count + box.x_center) / (count + 1),
            y_center=(previous.y_center * count + box.y_center) / (count + 1),
            width=(previous.width * count + box.width) / (count + 1),
            height=(previous.height * count + box.height) / (count + 1),
        )
        merge_counts[duplicate_index] = count + 1
    return tuple(merged)


def clip_box(box: Box) -> Box:
    x1 = max(0.0, box.x_center - box.width / 2)
    y1 = max(0.0, box.y_center - box.height / 2)
    x2 = min(1.0, box.x_center + box.width / 2)
    y2 = min(1.0, box.y_center + box.height / 2)
    if x2 <= x1 or y2 <= y1:
        raise ValueError(f"裁剪后框面积为 0：{box}")
    return Box(
        class_id=box.class_id,
        x_center=(x1 + x2) / 2,
        y_center=(y1 + y2) / 2,
        width=x2 - x1,
        height=y2 - y1,
    )


def write_label(path: Path, boxes: tuple[Box, ...]) -> None:
    if not boxes:
        raise ValueError(f"拒绝写入无目标标签：{path}")
    lines = [
        (
            f"{box.class_id} {box.x_center:.6f} {box.y_center:.6f} "
            f"{box.width:.6f} {box.height:.6f}"
        )
        for box in boxes
    ]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def load_output_boxes(item: BuildItem) -> tuple[Box, ...]:
    allowed_classes = set(CLASS_NAMES) if item.source_kind == "old" else {0}
    source_boxes, issues = parse_label(item.source_label, allowed_classes)
    non_boundary_issues = [
        issue for issue in issues if "框越出归一化图像边界" not in issue
    ]
    if non_boundary_issues:
        raise ValueError("；".join(non_boundary_issues))
    if item.source_kind == "special":
        return merge_duplicate_boxes(
            tuple(
                Box(3, box.x_center, box.y_center, box.width, box.height)
                for box in source_boxes
            )
        )
    return tuple(clip_box(box) for box in source_boxes)


# %% Source selection and deterministic split
def detect_split(path: Path) -> str:
    parts = {part.lower() for part in path.parts}
    matches = [split for split in SPLITS if split in parts]
    if len(matches) != 1:
        raise ValueError(f"无法从路径唯一确定 train/val/test：{path}")
    return matches[0]


def load_duplicate_exclusions(audit_json: Path) -> set[Path]:
    report = json.loads(audit_json.read_text(encoding="utf-8"))
    if report["special"]["issues"]:
        raise ValueError(
            "新增特殊车牌审计仍有异常，拒绝构建："
            + "；".join(report["special"]["issues"])
        )
    exclusions: set[Path] = set()
    for group in report["cross_partition_exact_duplicate_groups"]:
        paths = [Path(raw_path).resolve() for raw_path in group]
        keeper = max(
            paths,
            key=lambda path: (SPLIT_PRIORITY[detect_split(path)], str(path)),
        )
        exclusions.update(path for path in paths if path != keeper)
    return exclusions


def collect_old_items(
    yolo_root: Path,
    exclusions: set[Path],
) -> list[BuildItem]:
    items: list[BuildItem] = []
    for split in SPLITS:
        images = collect_images(yolo_root / "images" / split)
        labels = collect_labels(yolo_root / "labels" / split)
        validate_pairing(images, labels, f"旧数据集/{split}")
        for stem, image_path in images.items():
            if image_path.resolve() in exclusions:
                continue
            items.append(
                BuildItem(
                    split=split,
                    source_kind="old",
                    source_partition=split,
                    source_image=image_path.resolve(),
                    source_label=labels[stem].resolve(),
                    output_stem=stem,
                    repeat_index=0,
                )
            )
    return items


def deterministic_special_split(
    stems: list[str],
    seed: int,
    group: str,
) -> dict[str, list[str]]:
    shuffled = sorted(stems)
    random.Random(f"{seed}:{group}").shuffle(shuffled)
    validation_count = round(len(shuffled) * SPECIAL_SPLIT_RATIOS["val"])
    test_count = round(len(shuffled) * SPECIAL_SPLIT_RATIOS["test"])
    return {
        "val": shuffled[:validation_count],
        "test": shuffled[validation_count : validation_count + test_count],
        "train": shuffled[validation_count + test_count :],
    }


def collect_special_items(
    special_root: Path,
    seed: int,
    train_repeats: int,
) -> list[BuildItem]:
    items: list[BuildItem] = []
    for image_group, (label_group, english_group) in SPECIAL_GROUPS.items():
        images = collect_images(special_root / "image" / image_group)
        labels = collect_labels(special_root / "txt" / label_group)
        validate_pairing(images, labels, f"新增数据/{image_group}")
        split_stems = deterministic_special_split(
            list(images),
            seed=seed,
            group=image_group,
        )
        for split in SPLITS:
            repeats = train_repeats if split == "train" else 1
            for stem in split_stems[split]:
                for repeat_index in range(repeats):
                    suffix = (
                        f"__r{repeat_index + 1:02d}" if repeats > 1 else ""
                    )
                    items.append(
                        BuildItem(
                            split=split,
                            source_kind="special",
                            source_partition=image_group,
                            source_image=images[stem].resolve(),
                            source_label=labels[stem].resolve(),
                            output_stem=(
                                f"special_{english_group}_{stem}{suffix}"
                            ),
                            repeat_index=repeat_index,
                        )
                    )
    return items


# %% Build and verification
def hardlink_image(source: Path, destination: Path) -> None:
    try:
        os.link(source, destination)
    except OSError as exc:
        raise OSError(
            f"创建图片硬链接失败（源和目标必须在同一 NTFS 卷）：{source} -> {destination}"
        ) from exc


def materialize_item(item: BuildItem, stage_root: Path) -> WrittenItem:
    output_image = (
        stage_root
        / "images"
        / item.split
        / f"{item.output_stem}{item.source_image.suffix.lower()}"
    )
    output_label = (
        stage_root / "labels" / item.split / f"{item.output_stem}.txt"
    )
    hardlink_image(item.source_image, output_image)
    boxes = load_output_boxes(item)
    write_label(output_label, boxes)
    return WrittenItem(
        split=item.split,
        source_kind=item.source_kind,
        source_partition=item.source_partition,
        source_image=item.source_image,
        source_label=item.source_label,
        output_image=output_image,
        output_label=output_label,
        repeat_index=item.repeat_index,
        boxes=boxes,
    )


def write_manifest(items: list[WrittenItem], stage_root: Path) -> None:
    manifest_path = stage_root / "dataset_manifest.tsv"
    with manifest_path.open("w", encoding="utf-8", newline="") as file:
        writer = csv.writer(file, delimiter="\t", lineterminator="\n")
        writer.writerow(
            (
                "split",
                "source_kind",
                "source_partition",
                "source_image",
                "source_label",
                "output_image",
                "repeat_index",
                "box_count",
                "class_ids",
            )
        )
        for item in items:
            writer.writerow(
                (
                    item.split,
                    item.source_kind,
                    item.source_partition,
                    item.source_image,
                    item.source_label,
                    item.output_image.relative_to(stage_root),
                    item.repeat_index,
                    len(item.boxes),
                    ",".join(str(box.class_id) for box in item.boxes),
                )
            )


def create_summary(
    items: list[WrittenItem],
    exclusions: set[Path],
    seed: int,
    train_repeats: int,
) -> dict[str, object]:
    by_split: dict[str, object] = {}
    for split in SPLITS:
        split_items = [item for item in items if item.split == split]
        class_counts = Counter(
            box.class_id for item in split_items for box in item.boxes
        )
        by_split[split] = {
            "images": len(split_items),
            "boxes": sum(len(item.boxes) for item in split_items),
            "class_counts": {
                CLASS_NAMES[class_id]: class_counts[class_id]
                for class_id in CLASS_NAMES
            },
            "old_images": sum(
                item.source_kind == "old" for item in split_items
            ),
            "special_images_with_repeats": sum(
                item.source_kind == "special" for item in split_items
            ),
            "special_unique_source_images": len(
                {
                    item.source_image
                    for item in split_items
                    if item.source_kind == "special"
                }
            ),
        }
    special_unique_by_partition = Counter(
        (item.split, item.source_partition, item.source_image)
        for item in items
        if item.source_kind == "special"
    )
    special_split_group_counts = Counter(
        (split, partition)
        for split, partition, _ in special_unique_by_partition
    )
    return {
        "seed": seed,
        "special_train_repeats": train_repeats,
        "special_split_ratios": SPECIAL_SPLIT_RATIOS,
        "old_cross_split_duplicate_files_excluded": len(exclusions),
        "by_split": by_split,
        "special_unique_images_by_split_and_group": {
            split: {
                group: special_split_group_counts[(split, group)]
                for group in SPECIAL_GROUPS
            }
            for split in SPLITS
        },
    }


def verify_output(items: list[WrittenItem], stage_root: Path) -> None:
    expected_images = Counter(item.split for item in items)
    for split in SPLITS:
        images = collect_images(stage_root / "images" / split)
        labels = collect_labels(stage_root / "labels" / split)
        validate_pairing(images, labels, f"派生数据集/{split}")
        if len(images) != expected_images[split]:
            raise OSError(
                f"{split} 输出图片数 {len(images)} != 预期 {expected_images[split]}"
            )
        for label_path in labels.values():
            _, issues = parse_label(label_path, set(CLASS_NAMES))
            if issues:
                raise ValueError("；".join(issues))


def print_summary(summary: dict[str, object]) -> None:
    print(
        f"排除旧数据跨划分重复文件："
        f"{summary['old_cross_split_duplicate_files_excluded']:,}"
    )
    for split in SPLITS:
        stats = summary["by_split"][split]
        counts = stats["class_counts"]
        print(
            f"{split}: 图片 {stats['images']:,}，框 {stats['boxes']:,}；"
            f"blue={counts['blue']:,}, green={counts['green']:,}, "
            f"yellow_single={counts['yellow_single']:,}, other={counts['other']:,}；"
            f"特殊车牌唯一源图 {stats['special_unique_source_images']:,}，"
            f"计重复训练样本 {stats['special_images_with_repeats']:,}"
        )
    print("新增特殊车牌唯一源图分层划分：")
    for split in SPLITS:
        group_counts = summary["special_unique_images_by_split_and_group"][split]
        print(
            f"  {split}: "
            + ", ".join(
                f"{group}={group_counts[group]}" for group in SPECIAL_GROUPS
            )
        )


# %% Main
def main() -> int:
    args = parse_args()
    if args.special_train_repeats < 1:
        raise ValueError("--special-train-repeats 必须 >= 1")
    yolo_root = args.yolo_root.resolve()
    special_root = args.special_root.resolve()
    audit_json = args.audit_json.resolve()
    output_root = args.output_root.resolve()
    if not yolo_root.is_dir():
        raise FileNotFoundError(f"找不到旧 YOLO 数据集：{yolo_root}")
    if not special_root.is_dir():
        raise FileNotFoundError(f"找不到特殊车牌数据集：{special_root}")
    if not audit_json.is_file():
        raise FileNotFoundError(f"找不到审计 JSON：{audit_json}")
    if output_root.exists():
        raise FileExistsError(f"输出目录已存在，拒绝覆盖：{output_root}")

    exclusions = load_duplicate_exclusions(audit_json)
    build_items = collect_old_items(yolo_root, exclusions)
    build_items.extend(
        collect_special_items(
            special_root,
            seed=args.seed,
            train_repeats=args.special_train_repeats,
        )
    )

    if args.dry_run:
        preview_items = [
            WrittenItem(
                split=item.split,
                source_kind=item.source_kind,
                source_partition=item.source_partition,
                source_image=item.source_image,
                source_label=item.source_label,
                output_image=Path(
                    item.output_stem + item.source_image.suffix.lower()
                ),
                output_label=Path(item.output_stem + ".txt"),
                repeat_index=item.repeat_index,
                boxes=load_output_boxes(item),
            )
            for item in build_items
        ]
        dry_summary = create_summary(
            preview_items,
            exclusions,
            seed=args.seed,
            train_repeats=args.special_train_repeats,
        )
        print_summary(dry_summary)
        print("Dry-run 完成：未创建派生数据集。")
        return 0

    print(
        f"准备构建 {len(build_items):,} 个样本；"
        f"排除旧数据跨划分重复文件 {len(exclusions):,} 个。"
    )

    stage_root = output_root.with_name(f".{output_root.name}.tmp")
    if stage_root.exists():
        raise FileExistsError(f"临时输出目录已存在：{stage_root}")
    for split in SPLITS:
        (stage_root / "images" / split).mkdir(parents=True)
        (stage_root / "labels" / split).mkdir(parents=True)

    written_items: list[WrittenItem] = []
    try:
        for index, item in enumerate(build_items, start=1):
            written_items.append(materialize_item(item, stage_root))
            if index % 2_000 == 0 or index == len(build_items):
                print(f"已构建 {index:,}/{len(build_items):,}")
        write_manifest(written_items, stage_root)
        summary = create_summary(
            written_items,
            exclusions,
            seed=args.seed,
            train_repeats=args.special_train_repeats,
        )
        (stage_root / "build_summary.json").write_text(
            json.dumps(summary, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        verify_output(written_items, stage_root)
        stage_root.rename(output_root)
    except Exception:
        print(f"构建失败，临时目录保留用于排查：{stage_root}")
        raise

    print_summary(summary)
    print(f"派生数据集构建完成：{output_root}")
    print("原图片未复制数据块；派生 images 使用 NTFS 硬链接。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
