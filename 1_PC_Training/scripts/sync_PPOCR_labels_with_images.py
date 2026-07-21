#!/usr/bin/env python3
"""按现存图片同步 PP-OCR 标签，并记录人工拒绝的 CRPD 警牌。"""

from __future__ import annotations

import argparse
import os
import re
import sys
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_DATASET_ROOT = SCRIPT_DIR.parent / "datasets" / "CBLPRD-330k"
SPLITS = ("train", "val")
REJECTED_MANIFEST_NAME = "crpd_white_rejected.txt"
LABEL_PATTERN = re.compile(r"^(\S+\.[Jj][Pp][Gg])\t(\S+)$")


@dataclass(frozen=True)
class LabelState:
    path: Path
    records: list[tuple[str, str]]
    original_size: int
    original_mtime_ns: int


@dataclass(frozen=True)
class ManifestState:
    path: Path
    records: dict[str, str]
    existed: bool
    original_size: int | None
    original_mtime_ns: int | None


@dataclass(frozen=True)
class DatasetAudit:
    labels: dict[str, LabelState]
    images: dict[str, set[str]]
    missing: dict[str, list[tuple[str, str]]]
    orphaned: dict[str, list[str]]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "以 train/val 中实际存在的 .jpg 图片为准同步 PaddleOCR 标签。"
            "缺图标签会被移除，未标注图片会报错而不会猜测标签。"
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
        help="只统计缺图标签和无标签图片，不修改文件",
    )
    action.add_argument(
        "--apply",
        action="store_true",
        help="确认删除缺图标签并写入人工拒绝清单",
    )
    return parser.parse_args()


def read_label_file(dataset_root: Path, split: str) -> LabelState:
    label_file = dataset_root / f"{split}.txt"
    if not label_file.is_file():
        raise FileNotFoundError(f"找不到标签文件：{label_file}")

    before = label_file.stat()
    records: list[tuple[str, str]] = []
    seen_paths: dict[str, int] = {}
    with label_file.open("r", encoding="utf-8-sig") as file:
        for line_no, raw_line in enumerate(file, start=1):
            line = raw_line.rstrip("\r\n")
            match = LABEL_PATTERN.fullmatch(line)
            if match is None:
                raise ValueError(
                    f"{label_file}:{line_no} 不是 .jpg<TAB>车牌号：{line!r}"
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
            records.append((relative_path, label))

    after = label_file.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise OSError(f"检查期间标签文件被外部修改，请重试：{label_file}")
    return LabelState(
        path=label_file,
        records=records,
        original_size=before.st_size,
        original_mtime_ns=before.st_mtime_ns,
    )


def scan_split_images(dataset_root: Path, split: str) -> set[str]:
    image_root = dataset_root / split
    if not image_root.is_dir():
        raise FileNotFoundError(f"找不到图片目录：{image_root}")

    images: set[str] = set()
    for directory, _, filenames in os.walk(image_root):
        directory_path = Path(directory)
        for filename in filenames:
            if not filename.lower().endswith(".jpg"):
                continue
            relative_path = (directory_path / filename).relative_to(
                dataset_root
            ).as_posix()
            if relative_path in images:
                raise ValueError(f"图片路径重复：{relative_path}")
            images.add(relative_path)
    return images


def audit_dataset(dataset_root: Path) -> DatasetAudit:
    labels = {
        split: read_label_file(dataset_root, split) for split in SPLITS
    }
    with ThreadPoolExecutor(max_workers=2) as executor:
        scanned = executor.map(
            lambda split: scan_split_images(dataset_root, split), SPLITS
        )
        images = dict(zip(SPLITS, scanned))

    missing: dict[str, list[tuple[str, str]]] = {}
    orphaned: dict[str, list[str]] = {}
    for split in SPLITS:
        label_paths = {path for path, _ in labels[split].records}
        missing[split] = [
            record
            for record in labels[split].records
            if record[0] not in images[split]
        ]
        orphaned[split] = sorted(images[split] - label_paths)
    return DatasetAudit(
        labels=labels,
        images=images,
        missing=missing,
        orphaned=orphaned,
    )


def read_manifest(dataset_root: Path) -> ManifestState:
    manifest = dataset_root / REJECTED_MANIFEST_NAME
    if not manifest.exists():
        return ManifestState(
            path=manifest,
            records={},
            existed=False,
            original_size=None,
            original_mtime_ns=None,
        )

    before = manifest.stat()
    records: dict[str, str] = {}
    with manifest.open("r", encoding="utf-8-sig") as file:
        for line_no, raw_line in enumerate(file, start=1):
            line = raw_line.rstrip("\r\n")
            match = LABEL_PATTERN.fullmatch(line)
            if match is None:
                raise ValueError(
                    f"{manifest}:{line_no} 不是 .jpg<TAB>车牌号：{line!r}"
                )
            relative_path, label = match.groups()
            if relative_path in records:
                raise ValueError(
                    f"{manifest}:{line_no} 人工拒绝路径重复：{relative_path}"
                )
            records[relative_path] = label
    after = manifest.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise OSError(f"检查期间拒绝清单被外部修改，请重试：{manifest}")
    return ManifestState(
        path=manifest,
        records=records,
        existed=True,
        original_size=before.st_size,
        original_mtime_ns=before.st_mtime_ns,
    )


def print_audit(audit: DatasetAudit) -> None:
    for split in SPLITS:
        missing = audit.missing[split]
        orphaned = audit.orphaned[split]
        print(
            f"{split}：标签 {len(audit.labels[split].records):,}，"
            f"图片 {len(audit.images[split]):,}，缺图标签 {len(missing):,}，"
            f"无标签图片 {len(orphaned):,}"
        )
        for relative_path, label in missing[:20]:
            print(f"  [删除标签] {relative_path}\t{label}")
        if len(missing) > 20:
            print(f"  ... 其余 {len(missing) - 20:,} 条")
        for relative_path in orphaned[:20]:
            print(f"  [无标签图片] {relative_path}")
        if len(orphaned) > 20:
            print(f"  ... 其余 {len(orphaned) - 20:,} 张")


def verify_unchanged(state: LabelState) -> None:
    current = state.path.stat()
    if (current.st_size, current.st_mtime_ns) != (
        state.original_size,
        state.original_mtime_ns,
    ):
        raise OSError(f"写入前标签文件被外部修改，请重试：{state.path}")


def verify_manifest_unchanged(state: ManifestState) -> None:
    if not state.existed:
        if state.path.exists():
            raise OSError(f"写入前拒绝清单被外部创建，请重试：{state.path}")
        return
    current = state.path.stat()
    if (current.st_size, current.st_mtime_ns) != (
        state.original_size,
        state.original_mtime_ns,
    ):
        raise OSError(f"写入前拒绝清单被外部修改，请重试：{state.path}")


def write_lines(path: Path, lines: list[str]) -> Path:
    temporary = path.with_name(f".{path.name}.sync.tmp")
    if temporary.exists():
        raise FileExistsError(f"临时文件已存在，请人工检查：{temporary}")
    with temporary.open("w", encoding="utf-8", newline="\n") as file:
        for line in lines:
            file.write(f"{line}\n")
    return temporary


def apply_sync(dataset_root: Path, audit: DatasetAudit) -> None:
    orphaned_count = sum(len(items) for items in audit.orphaned.values())
    if orphaned_count:
        raise ValueError(
            f"存在 {orphaned_count} 张无标签图片，无法安全猜测标签，未做修改"
        )
    missing_count = sum(len(items) for items in audit.missing.values())
    if missing_count == 0:
        print("标签和图片已经一一对应，无需修改。")
        return

    manifest = read_manifest(dataset_root)
    rejected = dict(manifest.records)
    for split in SPLITS:
        for relative_path, label in audit.missing[split]:
            if "/crpd_white_" not in relative_path:
                continue
            previous = rejected.get(relative_path)
            if previous is not None and previous != label:
                raise ValueError(
                    f"人工拒绝清单标签冲突：{relative_path} / {previous} / {label}"
                )
            rejected[relative_path] = label

    for split in SPLITS:
        verify_unchanged(audit.labels[split])
    verify_manifest_unchanged(manifest)

    staged_labels: dict[str, Path] = {}
    for split in SPLITS:
        missing_paths = {path for path, _ in audit.missing[split]}
        kept_lines = [
            f"{path}\t{label}"
            for path, label in audit.labels[split].records
            if path not in missing_paths
        ]
        staged_labels[split] = write_lines(
            audit.labels[split].path, kept_lines
        )
    staged_manifest = write_lines(
        manifest.path,
        [f"{path}\t{label}" for path, label in rejected.items()],
    )

    for split in SPLITS:
        verify_unchanged(audit.labels[split])
    verify_manifest_unchanged(manifest)
    for split in SPLITS:
        staged_labels[split].replace(audit.labels[split].path)
    staged_manifest.replace(manifest.path)


def main() -> int:
    args = parse_args()
    dataset_root = args.dataset_root.expanduser().resolve()
    try:
        if not dataset_root.is_dir():
            raise FileNotFoundError(f"找不到数据集根目录：{dataset_root}")
        print("扫描标签和图片...", flush=True)
        audit = audit_dataset(dataset_root)
        print_audit(audit)
        if args.dry_run:
            print("Dry-run 完成：未修改标签或图片。")
            return 0

        apply_sync(dataset_root, audit)
        print("重新执行双向一一对应检查...", flush=True)
        verified = audit_dataset(dataset_root)
        if any(verified.missing.values()) or any(verified.orphaned.values()):
            raise OSError("同步后标签与图片仍未一一对应")
        print_audit(verified)
        print(f"人工拒绝清单：{dataset_root / REJECTED_MANIFEST_NAME}")
        return 0
    except (OSError, ValueError) as exc:
        print(f"错误：{exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
