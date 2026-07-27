#!/usr/bin/env python3
"""将审阅通过的特殊车牌按类别 8:2 追加到 CBLPRD PP-OCR 数据集。"""

from __future__ import annotations

import argparse
import math
import os
import random
import shutil
import sys
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path

import cv2
import numpy as np


SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_REVIEW_ROOT = (
    SCRIPT_DIR.parent / "datasets" / "特殊车牌" / "PP-OCRv4_format"
)
DEFAULT_DATASET_ROOT = SCRIPT_DIR.parent / "datasets" / "CBLPRD-330k"
CATEGORIES = ("学", "港", "澳", "警")
SPLITS = ("train", "val")
TRAIN_RATIO = 0.8
DEFAULT_SEED = 20260716
EXPECTED_IMAGE_SHAPE = (48, 128, 3)
STAGE_NAME = ".append_special_plates.tmp"


@dataclass(frozen=True)
class Sample:
    plate_text: str
    category: str
    image_path: Path


@dataclass(frozen=True)
class LabelSnapshot:
    path: Path
    raw: bytes
    size: int
    mtime_ns: int
    paths: frozenset[str]
    labels: frozenset[str]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "按学、港、澳、警四类分别以固定随机种子做 8:2 划分，"
            "并追加到 CBLPRD-330k/train、val。"
        )
    )
    parser.add_argument(
        "--review-root",
        type=Path,
        default=DEFAULT_REVIEW_ROOT,
        help=f"已审阅特殊车牌目录（默认：{DEFAULT_REVIEW_ROOT}）",
    )
    parser.add_argument(
        "--dataset-root",
        type=Path,
        default=DEFAULT_DATASET_ROOT,
        help=f"目标 PP-OCR 数据集目录（默认：{DEFAULT_DATASET_ROOT}）",
    )
    parser.add_argument(
        "--seed",
        type=int,
        default=DEFAULT_SEED,
        help=f"固定划分随机种子（默认：{DEFAULT_SEED}）",
    )
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--dry-run", action="store_true", help="只检查和预览划分")
    action.add_argument("--apply", action="store_true", help="执行图片复制和标签追加")
    return parser.parse_args()


def decode_image(path: Path) -> np.ndarray:
    try:
        encoded = np.fromfile(path, dtype=np.uint8)
        image = cv2.imdecode(encoded, cv2.IMREAD_COLOR)
    except OSError as exc:
        raise OSError(f"读取图片失败：{path}") from exc
    if image is None:
        raise OSError(f"图片无法解码：{path}")
    return image


def read_review_samples(review_root: Path) -> list[Sample]:
    image_root = review_root / "images"
    label_file = review_root / "labels.txt"
    if not image_root.is_dir() or not label_file.is_file():
        raise FileNotFoundError(
            f"审阅集必须包含 images/ 和 labels.txt：{review_root}"
        )

    samples: list[Sample] = []
    seen_paths: set[str] = set()
    with label_file.open("r", encoding="utf-8-sig") as file:
        for line_no, raw_line in enumerate(file, start=1):
            line = raw_line.rstrip("\r\n")
            if not line:
                continue
            if line.count("\t") != 1:
                raise ValueError(
                    f"{label_file}:{line_no} 不是 图片路径<TAB>车牌号：{line!r}"
                )
            relative_path, plate_text = line.split("\t")
            relative = Path(relative_path)
            if (
                relative_path != f"images/{relative.name}"
                or relative.suffix.lower() != ".jpg"
                or relative.stem != plate_text
            ):
                raise ValueError(
                    f"{label_file}:{line_no} 路径、文件名和车牌号不一致：{line!r}"
                )
            if relative_path in seen_paths:
                raise ValueError(f"{label_file}:{line_no} 路径重复：{relative_path}")
            if not plate_text or plate_text[-1] not in CATEGORIES:
                raise ValueError(
                    f"{label_file}:{line_no} 车牌不属于学/港/澳/警：{plate_text}"
                )

            image_path = review_root / relative_path
            if not image_path.is_file():
                raise FileNotFoundError(f"标签对应图片不存在：{image_path}")
            image = decode_image(image_path)
            if image.shape != EXPECTED_IMAGE_SHAPE:
                raise ValueError(
                    f"审阅图片不是 128x48 三通道：{image_path} / {image.shape}"
                )
            seen_paths.add(relative_path)
            samples.append(
                Sample(
                    plate_text=plate_text,
                    category=plate_text[-1],
                    image_path=image_path,
                )
            )

    actual_images = {path.name for path in image_root.glob("*.jpg")}
    labeled_images = {sample.image_path.name for sample in samples}
    if actual_images != labeled_images:
        missing = sorted(labeled_images - actual_images)
        orphaned = sorted(actual_images - labeled_images)
        raise ValueError(
            f"审阅图片与标签不一一对应：缺图 {missing[:5]}，"
            f"无标签图片 {orphaned[:5]}"
        )
    if not samples:
        raise ValueError("审阅集为空")
    return samples


def split_samples(
    samples: list[Sample], seed: int
) -> dict[str, list[Sample]]:
    grouped: dict[str, list[Sample]] = defaultdict(list)
    for sample in samples:
        grouped[sample.category].append(sample)
    missing_categories = [category for category in CATEGORIES if not grouped[category]]
    if missing_categories:
        raise ValueError(f"审阅集缺少类别：{missing_categories}")

    randomizer = random.Random(seed)
    result = {split: [] for split in SPLITS}
    for category in CATEGORIES:
        category_samples = sorted(
            grouped[category], key=lambda sample: sample.plate_text
        )
        randomizer.shuffle(category_samples)
        train_count = math.floor(len(category_samples) * TRAIN_RATIO + 0.5)
        if not 0 < train_count < len(category_samples):
            raise ValueError(f"类别 {category} 数量不足以划分 train/val")
        result["train"].extend(category_samples[:train_count])
        result["val"].extend(category_samples[train_count:])
    return result


def read_target_labels(dataset_root: Path, split: str) -> LabelSnapshot:
    label_path = dataset_root / f"{split}.txt"
    image_root = dataset_root / split
    if not label_path.is_file() or not image_root.is_dir():
        raise FileNotFoundError(
            f"目标数据集必须包含 {split}/ 和 {split}.txt：{dataset_root}"
        )

    before = label_path.stat()
    raw = label_path.read_bytes()
    after = label_path.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise OSError(f"读取期间标签被外部修改，请重试：{label_path}")
    if raw.startswith(b"\xef\xbb\xbf") or b"\r" in raw:
        raise ValueError(f"目标标签必须为 UTF-8 无 BOM、LF 换行：{label_path}")
    if raw and not raw.endswith(b"\n"):
        raise ValueError(f"目标标签文件末尾缺少 LF：{label_path}")

    paths: set[str] = set()
    labels: set[str] = set()
    text = raw.decode("utf-8")
    for line_no, line in enumerate(text.splitlines(), start=1):
        if line.count("\t") != 1:
            raise ValueError(
                f"{label_path}:{line_no} 不是 图片路径<TAB>车牌号：{line!r}"
            )
        relative_path, label = line.split("\t")
        if (
            not relative_path.startswith(f"{split}/")
            or "\\" in relative_path
            or not relative_path.lower().endswith(".jpg")
            or not label
        ):
            raise ValueError(f"{label_path}:{line_no} 标签格式错误：{line!r}")
        if relative_path in paths:
            raise ValueError(f"{label_path}:{line_no} 路径重复：{relative_path}")
        paths.add(relative_path)
        labels.add(label)
    return LabelSnapshot(
        path=label_path,
        raw=raw,
        size=before.st_size,
        mtime_ns=before.st_mtime_ns,
        paths=frozenset(paths),
        labels=frozenset(labels),
    )


def verify_snapshot(snapshot: LabelSnapshot) -> None:
    current = snapshot.path.stat()
    if (current.st_size, current.st_mtime_ns) != (
        snapshot.size,
        snapshot.mtime_ns,
    ):
        raise OSError(f"写入前标签被外部修改，请重试：{snapshot.path}")


def validate_destinations(
    dataset_root: Path,
    assignments: dict[str, list[Sample]],
    snapshots: dict[str, LabelSnapshot],
) -> int:
    all_paths = snapshots["train"].paths | snapshots["val"].paths
    existing_labels = snapshots["train"].labels | snapshots["val"].labels
    repeated_text_count = 0
    for split in SPLITS:
        for sample in assignments[split]:
            train_path = f"train/{sample.image_path.name}"
            val_path = f"val/{sample.image_path.name}"
            if train_path in all_paths or val_path in all_paths:
                raise FileExistsError(
                    f"目标标签已存在同名特殊车牌：{sample.image_path.name}"
                )
            if (
                (dataset_root / train_path).exists()
                or (dataset_root / val_path).exists()
            ):
                raise FileExistsError(
                    f"目标目录已存在同名特殊车牌：{sample.image_path.name}"
                )
            if sample.plate_text in existing_labels:
                repeated_text_count += 1
    return repeated_text_count


def appended_label_bytes(
    snapshot: LabelSnapshot, split: str, samples: list[Sample]
) -> bytes:
    additions = "".join(
        f"{split}/{sample.image_path.name}\t{sample.plate_text}\n"
        for sample in samples
    ).encode("utf-8")
    return snapshot.raw + additions


def apply_append(
    dataset_root: Path,
    assignments: dict[str, list[Sample]],
    snapshots: dict[str, LabelSnapshot],
) -> None:
    stage_root = dataset_root / STAGE_NAME
    if stage_root.exists():
        raise FileExistsError(f"追加临时目录已存在，请人工检查：{stage_root}")

    moved_targets: list[Path] = []
    replaced_labels: list[str] = []
    try:
        for split in SPLITS:
            (stage_root / split).mkdir(parents=True, exist_ok=True)
            shutil.copy2(
                snapshots[split].path,
                stage_root / f"{split}.original.txt",
            )
            new_raw = appended_label_bytes(
                snapshots[split], split, assignments[split]
            )
            (stage_root / f"{split}.new.txt").write_bytes(new_raw)
            for sample in assignments[split]:
                staged_image = stage_root / split / sample.image_path.name
                shutil.copy2(sample.image_path, staged_image)
                if (
                    staged_image.stat().st_size != sample.image_path.stat().st_size
                    or decode_image(staged_image).shape != EXPECTED_IMAGE_SHAPE
                ):
                    raise OSError(f"暂存图片校验失败：{staged_image}")

        for snapshot in snapshots.values():
            verify_snapshot(snapshot)
        validate_destinations(dataset_root, assignments, snapshots)

        for split in SPLITS:
            for sample in assignments[split]:
                staged_image = stage_root / split / sample.image_path.name
                target_image = dataset_root / split / sample.image_path.name
                staged_image.replace(target_image)
                moved_targets.append(target_image)

        for split in SPLITS:
            os.replace(
                stage_root / f"{split}.new.txt",
                snapshots[split].path,
            )
            replaced_labels.append(split)
    except Exception:
        for split in reversed(replaced_labels):
            backup = stage_root / f"{split}.original.txt"
            if backup.exists():
                os.replace(backup, snapshots[split].path)
        for target in reversed(moved_targets):
            target.unlink(missing_ok=True)
        raise
    finally:
        if stage_root.exists():
            shutil.rmtree(stage_root)

    for split in SPLITS:
        expected_raw = appended_label_bytes(
            snapshots[split], split, assignments[split]
        )
        if snapshots[split].path.read_bytes() != expected_raw:
            raise OSError(f"追加后标签内容校验失败：{snapshots[split].path}")
        for sample in assignments[split]:
            target = dataset_root / split / sample.image_path.name
            if not target.is_file() or decode_image(target).shape != EXPECTED_IMAGE_SHAPE:
                raise OSError(f"追加后图片校验失败：{target}")


def print_summary(
    assignments: dict[str, list[Sample]],
    seed: int,
    repeated_text_count: int,
) -> None:
    print(f"固定随机种子：{seed}")
    print("各类别独立按最接近 8:2 的整数划分：")
    for category in CATEGORIES:
        train_count = sum(
            sample.category == category for sample in assignments["train"]
        )
        val_count = sum(
            sample.category == category for sample in assignments["val"]
        )
        print(f"  {category}: train {train_count}，val {val_count}")
    print(
        f"合计：train {len(assignments['train'])}，"
        f"val {len(assignments['val'])}"
    )
    print(f"目标数据集中已有相同识别文本但不同文件名：{repeated_text_count}")


def main() -> int:
    args = parse_args()
    review_root = args.review_root.expanduser().resolve()
    dataset_root = args.dataset_root.expanduser().resolve()
    try:
        samples = read_review_samples(review_root)
        assignments = split_samples(samples, args.seed)
        snapshots = {
            split: read_target_labels(dataset_root, split) for split in SPLITS
        }
        repeated_text_count = validate_destinations(
            dataset_root, assignments, snapshots
        )
        print_summary(assignments, args.seed, repeated_text_count)
        if args.dry_run:
            print("Dry-run 完成：未复制图片、未修改标签。")
            return 0

        apply_append(dataset_root, assignments, snapshots)
        print("特殊车牌追加完成。")
        return 0
    except (OSError, UnicodeError, ValueError) as exc:
        print(f"错误：{exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
