#!/usr/bin/env python3
"""原地整理 CBLPRD-330k，生成单层车牌 PP-OCRv4 识别数据集。"""

from __future__ import annotations

import argparse
import os
import sys
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Sequence


SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_SOURCE_ROOT = SCRIPT_DIR.parent / "datasets" / "CBLPRD-330k"
SPLITS = ("train", "val")
KEEP_PLATE_TYPES = (
    "黑色车牌",
    "单层黄牌",
    "普通蓝牌",
    "新能源大型车",
    "新能源小型车",
)


@dataclass(frozen=True)
class Sample:
    source_path: Path
    target_path: Path
    target_relative_path: Path
    label: str
    plate_type: str
    keep: bool


@dataclass(frozen=True)
class SplitResult:
    samples: list[Sample]
    type_counts: Counter[str]


def positive_int(value: str) -> int:
    parsed = int(value)
    if parsed < 1:
        raise argparse.ArgumentTypeError("必须是大于 0 的整数")
    return parsed


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "按原 train.txt/val.txt 划分原地整理 CBLPRD-330k：保留图片移动到 "
            "train/val，排除图片永久删除，标签转换为 PaddleOCR 标准格式。"
        )
    )
    parser.add_argument(
        "--source-root",
        type=Path,
        default=DEFAULT_SOURCE_ROOT,
        help=f"CBLPRD-330k 根目录（默认：{DEFAULT_SOURCE_ROOT}）",
    )
    parser.add_argument(
        "--workers",
        type=positive_int,
        default=min(8, os.cpu_count() or 1),
        help="并行移动/删除图片的线程数（默认：最多 8）",
    )
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument(
        "--dry-run",
        action="store_true",
        help="只检查并显示计划，不移动、删除或改写任何文件",
    )
    action.add_argument(
        "--apply",
        action="store_true",
        help="确认在源数据集内执行不可逆的移动、删除和标签替换",
    )
    return parser.parse_args()


def ensure_inside_root(path: Path, root: Path, annotation: Path, line_no: int) -> None:
    try:
        path.relative_to(root)
    except ValueError as exc:
        raise ValueError(
            f"{annotation}:{line_no} 图片路径越过数据集根目录：{path}"
        ) from exc


def parse_split(source_root: Path, split: str) -> SplitResult:
    annotation = source_root / f"{split}.txt"
    if not annotation.is_file():
        raise FileNotFoundError(f"找不到原始划分文件：{annotation}")

    samples: list[Sample] = []
    type_counts: Counter[str] = Counter()
    seen_sources: set[Path] = set()
    kept_target_names: dict[str, Path] = {}

    with annotation.open("r", encoding="utf-8-sig") as file:
        for line_no, raw_line in enumerate(file, start=1):
            stripped = raw_line.strip()
            if not stripped:
                continue

            fields = stripped.split(maxsplit=2)
            if len(fields) != 3:
                raise ValueError(
                    f"{annotation}:{line_no} 应为‘图片路径 标签 车牌类型’，实际为：{stripped!r}"
                )

            image_text, label, plate_type = fields
            if not label or any(char.isspace() for char in label):
                raise ValueError(
                    f"{annotation}:{line_no} 车牌标签为空或含空白字符：{label!r}"
                )

            source_path = (source_root / Path(image_text)).resolve()
            ensure_inside_root(source_path, source_root, annotation, line_no)
            if source_path in seen_sources:
                raise ValueError(
                    f"{annotation}:{line_no} 图片被重复标注：{source_path}"
                )
            seen_sources.add(source_path)

            keep = plate_type in KEEP_PLATE_TYPES
            target_relative_path = Path(split) / source_path.name
            target_path = source_root / target_relative_path
            source_exists = source_path.is_file()
            target_exists = target_path.is_file()

            if keep:
                previous_source = kept_target_names.get(source_path.name)
                if previous_source is not None:
                    raise ValueError(
                        f"{annotation}:{line_no} 输出文件名冲突：{previous_source} 与 "
                        f"{source_path} 都将写为 {target_relative_path.as_posix()}"
                    )
                kept_target_names[source_path.name] = source_path

                if source_exists and target_exists:
                    raise FileExistsError(
                        f"原图和目标图同时存在，无法判断应保留哪个：{source_path} / {target_path}"
                    )
                if not source_exists and not target_exists:
                    raise FileNotFoundError(
                        f"{annotation}:{line_no} 找不到待保留图片：{source_path}"
                    )

            type_counts[plate_type] += 1
            samples.append(
                Sample(
                    source_path=source_path,
                    target_path=target_path,
                    target_relative_path=target_relative_path,
                    label=label,
                    plate_type=plate_type,
                    keep=keep,
                )
            )

    return SplitResult(samples=samples, type_counts=type_counts)


def validate_splits(results: dict[str, SplitResult]) -> None:
    train_paths = {sample.source_path for sample in results["train"].samples}
    val_paths = {sample.source_path for sample in results["val"].samples}
    overlap = train_paths & val_paths
    if overlap:
        example = next(iter(overlap))
        raise ValueError(f"train/val 存在 {len(overlap)} 张重复图片，例如：{example}")


def kept_samples(result: SplitResult) -> list[Sample]:
    return [sample for sample in result.samples if sample.keep]


def discarded_samples(result: SplitResult) -> list[Sample]:
    return [sample for sample in result.samples if not sample.keep]


def print_summary(results: dict[str, SplitResult]) -> None:
    print("保留类别：" + "、".join(KEEP_PLATE_TYPES))
    for split in SPLITS:
        result = results[split]
        kept = kept_samples(result)
        discarded = discarded_samples(result)
        kept_counts = Counter(sample.plate_type for sample in kept)
        pending_moves = sum(sample.source_path.is_file() for sample in kept)
        pending_deletes = sum(sample.source_path.is_file() for sample in discarded)

        print(
            f"\n[{split}] 原始 {len(result.samples):,}，保留 {len(kept):,}，"
            f"删除 {len(discarded):,}"
        )
        print(
            f"  待移动 {pending_moves:,}，已移动 {len(kept) - pending_moves:,}，"
            f"待删除 {pending_deletes:,}，已不存在 {len(discarded) - pending_deletes:,}"
        )
        for plate_type in KEEP_PLATE_TYPES:
            print(f"  {plate_type}: {kept_counts[plate_type]:,}")
        for plate_type, count in sorted(result.type_counts.items()):
            if plate_type not in KEEP_PLATE_TYPES:
                print(f"  [删除] {plate_type}: {count:,}")

    kept_total = sum(len(kept_samples(results[split])) for split in SPLITS)
    discarded_total = sum(len(discarded_samples(results[split])) for split in SPLITS)
    print(f"\n合计保留：{kept_total:,}；合计删除：{discarded_total:,}")


def prepare_target_directories(
    source_root: Path, results: dict[str, SplitResult]
) -> None:
    for split in SPLITS:
        target_directory = source_root / split
        expected_names = {
            sample.target_path.name for sample in kept_samples(results[split])
        }
        if target_directory.exists() and not target_directory.is_dir():
            raise NotADirectoryError(f"目标路径不是目录：{target_directory}")
        target_directory.mkdir(exist_ok=True)

        unexpected = [
            entry
            for entry in target_directory.iterdir()
            if not entry.is_file() or entry.name not in expected_names
        ]
        if unexpected:
            raise FileExistsError(
                f"{target_directory} 中存在不属于本次转换的内容，例如：{unexpected[0]}"
            )


def write_staged_label_file(
    source_root: Path, split: str, samples: Sequence[Sample]
) -> Path:
    staged_file = source_root / f".{split}.ppocr.tmp"
    with staged_file.open("w", encoding="utf-8", newline="\n") as file:
        for sample in samples:
            file.write(
                f"{sample.target_relative_path.as_posix()}\t{sample.label}\n"
            )
    return staged_file


def move_sample(sample: Sample) -> bool:
    if sample.target_path.is_file():
        return False
    sample.source_path.rename(sample.target_path)
    return True


def delete_sample(sample: Sample) -> bool:
    if not sample.source_path.exists():
        return False
    sample.source_path.unlink()
    return True


def run_file_actions(
    samples: Sequence[Sample],
    action: Callable[[Sample], bool],
    workers: int,
    action_name: str,
) -> None:
    total = len(samples)
    batch_size = 2048
    processed = 0
    changed = 0

    with ThreadPoolExecutor(max_workers=workers) as executor:
        for start in range(0, total, batch_size):
            batch = samples[start : start + batch_size]
            changed += sum(executor.map(action, batch))
            processed += len(batch)
            if processed == total or processed % (batch_size * 5) == 0:
                print(f"  {action_name} {processed:,}/{total:,}")
    print(f"  实际变更：{changed:,}")


def verify_image_result(results: dict[str, SplitResult]) -> None:
    for split in SPLITS:
        for sample in kept_samples(results[split]):
            if sample.source_path.exists() or not sample.target_path.is_file():
                raise OSError(f"保留图片移动结果异常：{sample.source_path}")
        for sample in discarded_samples(results[split]):
            if sample.source_path.exists():
                raise OSError(f"排除图片删除失败：{sample.source_path}")


def remove_empty_source_directories(
    source_root: Path, results: dict[str, SplitResult]
) -> None:
    directories: set[Path] = set()
    for split in SPLITS:
        for sample in results[split].samples:
            parent = sample.source_path.parent
            while parent != source_root:
                directories.add(parent)
                parent = parent.parent

    for directory in sorted(directories, key=lambda path: len(path.parts), reverse=True):
        if directory.exists():
            try:
                directory.rmdir()
            except OSError as exc:
                raise OSError(f"旧图片目录仍含未记录文件，未替换标签：{directory}") from exc


def finalize_annotations(source_root: Path, staged_files: dict[str, Path]) -> None:
    old_data_file = source_root / "data.txt"
    if old_data_file.exists():
        old_data_file.unlink()

    for split in SPLITS:
        staged_files[split].replace(source_root / f"{split}.txt")


def main() -> int:
    args = parse_args()
    source_root = args.source_root.expanduser().resolve()

    try:
        if not source_root.is_dir():
            raise FileNotFoundError(f"找不到源数据集目录：{source_root}")

        results = {split: parse_split(source_root, split) for split in SPLITS}
        validate_splits(results)
        print_summary(results)

        if args.dry_run:
            print("\nDry-run 完成：未移动、删除或改写任何文件。")
            return 0

        prepare_target_directories(source_root, results)
        staged_files = {
            split: write_staged_label_file(
                source_root, split, kept_samples(results[split])
            )
            for split in SPLITS
        }

        for split in SPLITS:
            print(f"\n移动 {split} 保留图片：")
            run_file_actions(
                kept_samples(results[split]), move_sample, args.workers, "已处理"
            )

        for split in SPLITS:
            print(f"\n删除 {split} 排除图片：")
            run_file_actions(
                discarded_samples(results[split]),
                delete_sample,
                args.workers,
                "已处理",
            )

        verify_image_result(results)
        remove_empty_source_directories(source_root, results)
        finalize_annotations(source_root, staged_files)

        print(f"\n原地整理完成：{source_root}")
        print("图片未复制、未重编码；train.txt/val.txt 已改为 PaddleOCR 标准格式。")
        return 0
    except (OSError, ValueError) as exc:
        print(f"错误：{exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
