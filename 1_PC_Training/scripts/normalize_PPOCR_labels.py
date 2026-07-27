#!/usr/bin/env python3
"""将 PP-OCR train.txt/val.txt 原子规范化为 路径.jpg<TAB>标签。"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_DATASET_ROOT = SCRIPT_DIR.parent / "datasets" / "CBLPRD-330k"
SPLITS = ("train", "val")
EXACT_PATTERN = re.compile(r"^(\S+\.[Jj][Pp][Gg])\t(\S+)$")
RECOVERABLE_PATTERN = re.compile(
    r"^(\S+\.[Jj][Pp][Gg])[ \t]+(\S+?)[ \t]*$"
)


@dataclass(frozen=True)
class NormalizedLabels:
    path: Path
    lines: list[str]
    changed_count: int
    original_size: int
    original_mtime_ns: int


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "检查并规范化 train.txt/val.txt，每行最终严格为 "
            "<相对图片路径.jpg><TAB><车牌号>。"
        )
    )
    parser.add_argument(
        "--dataset-root",
        type=Path,
        default=DEFAULT_DATASET_ROOT,
        help=f"PP-OCR 数据集根目录（默认：{DEFAULT_DATASET_ROOT}）",
    )
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument(
        "--dry-run",
        action="store_true",
        help="只检查和统计，不修改标签文件",
    )
    action.add_argument(
        "--apply",
        action="store_true",
        help="确认原子替换存在可恢复格式问题的标签文件",
    )
    return parser.parse_args()


def normalize_label_file(dataset_root: Path, split: str) -> NormalizedLabels:
    label_file = dataset_root / f"{split}.txt"
    if not label_file.is_file():
        raise FileNotFoundError(f"找不到标签文件：{label_file}")

    before = label_file.stat()
    normalized_lines: list[str] = []
    changed_count = 0
    seen_paths: dict[str, int] = {}

    with label_file.open("r", encoding="utf-8-sig") as file:
        for line_no, raw_line in enumerate(file, start=1):
            line = raw_line.rstrip("\r\n")
            exact_match = EXACT_PATTERN.fullmatch(line)
            match = exact_match or RECOVERABLE_PATTERN.fullmatch(line)
            if match is None:
                raise ValueError(
                    f"{label_file}:{line_no} 无法安全解析为 图片.jpg + 标签：{line!r}"
                )
            relative_path, label = match.groups()
            if (
                not relative_path.startswith(f"{split}/")
                or "\\" in relative_path
                or any(part in ("", ".", "..") for part in relative_path.split("/"))
            ):
                raise ValueError(
                    f"{label_file}:{line_no} 路径不属于 {split}：{relative_path}"
                )
            if relative_path in seen_paths:
                raise ValueError(
                    f"{label_file}:{line_no} 路径重复，首次位于第 "
                    f"{seen_paths[relative_path]} 行：{relative_path}"
                )
            seen_paths[relative_path] = line_no

            normalized = f"{relative_path}\t{label}"
            if line != normalized:
                changed_count += 1
            normalized_lines.append(normalized)

    after = label_file.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise OSError(f"检查期间标签文件被外部修改，请重试：{label_file}")
    return NormalizedLabels(
        path=label_file,
        lines=normalized_lines,
        changed_count=changed_count,
        original_size=before.st_size,
        original_mtime_ns=before.st_mtime_ns,
    )


def apply_normalization(result: NormalizedLabels) -> None:
    if result.changed_count == 0:
        return
    current = result.path.stat()
    if (current.st_size, current.st_mtime_ns) != (
        result.original_size,
        result.original_mtime_ns,
    ):
        raise OSError(f"写入前标签文件被外部修改，请重试：{result.path}")

    temporary = result.path.with_name(f".{result.path.name}.normalize.tmp")
    if temporary.exists():
        raise FileExistsError(f"临时标签文件已存在，请人工检查：{temporary}")
    with temporary.open("w", encoding="utf-8", newline="\n") as file:
        for line in result.lines:
            file.write(f"{line}\n")
    temporary.replace(result.path)


def main() -> int:
    args = parse_args()
    dataset_root = args.dataset_root.expanduser().resolve()
    try:
        if not dataset_root.is_dir():
            raise FileNotFoundError(f"找不到数据集根目录：{dataset_root}")
        results = {
            split: normalize_label_file(dataset_root, split) for split in SPLITS
        }
        for split in SPLITS:
            result = results[split]
            print(
                f"{split}.txt：{len(result.lines):,} 行，"
                f"需要规范化 {result.changed_count:,} 行"
            )

        if args.dry_run:
            print("Dry-run 完成：未修改标签文件。")
            return 0

        for split in SPLITS:
            apply_normalization(results[split])
        verified = {
            split: normalize_label_file(dataset_root, split) for split in SPLITS
        }
        if any(result.changed_count for result in verified.values()):
            raise OSError("规范化后复核失败")
        print("规范化完成：所有行均为 图片.jpg<TAB>车牌号。")
        return 0
    except (OSError, ValueError) as exc:
        print(f"错误：{exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
