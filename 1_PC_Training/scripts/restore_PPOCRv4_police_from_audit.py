#!/usr/bin/env python3
"""从两次 CBLPRD 审计删除备份中恢复警牌到 special/警。"""

from __future__ import annotations

import argparse
import csv
import json
import os
import random
import shutil
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path, PurePosixPath


SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPT_DIR.parents[1]
DEFAULT_DATASET_ROOT = SCRIPT_DIR.parent / "datasets" / "CBLPRD-330k"
DEFAULT_AUDIT_ROOT = PROJECT_ROOT / "CBLPRD-330k_audit" / "output" / "backups"
DEFAULT_SEED = 20260717
EXPECTED_BACKUP_CANDIDATES = 62
SPLITS = ("train", "val")
POLICE_SUFFIX = "警"


@dataclass(frozen=True)
class PoliceRecord:
    name: str
    label: str
    image_path: Path
    current_split: str | None
    source_backup: str
    source_relative_path: str


@dataclass(frozen=True)
class Assignment:
    record: PoliceRecord
    destination_split: str


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "保留审计删除备份，通过 NTFS 硬链接恢复其中的警牌，并与 special/警 "
            "现有样本合并后使用固定随机种子重新做 8:2 划分。"
        )
    )
    parser.add_argument(
        "--dataset-root",
        type=Path,
        default=DEFAULT_DATASET_ROOT,
        help=f"CBLPRD 数据集根目录（默认：{DEFAULT_DATASET_ROOT}）",
    )
    parser.add_argument(
        "--audit-backup-root",
        type=Path,
        default=DEFAULT_AUDIT_ROOT,
        help=f"审计备份根目录（默认：{DEFAULT_AUDIT_ROOT}）",
    )
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED, help="固定随机种子")
    parser.add_argument(
        "--apply",
        action="store_true",
        help="实际建立硬链接并更新标签；默认只预演",
    )
    return parser.parse_args()


def parse_label_file(label_file: Path, expected_split: str) -> list[tuple[str, str]]:
    rows: list[tuple[str, str]] = []
    with label_file.open("r", encoding="utf-8-sig", newline="") as handle:
        for line_number, raw_line in enumerate(handle, start=1):
            line = raw_line.rstrip("\r\n")
            if not line or line.count("\t") != 1:
                raise ValueError(
                    f"{label_file}:{line_number} 必须严格为 路径<TAB>标签"
                )
            path_text, label = line.split("\t")
            relative_path = PurePosixPath(path_text.replace("\\", "/"))
            if (
                len(relative_path.parts) != 2
                or relative_path.parts[0] != expected_split
            ):
                raise ValueError(
                    f"{label_file}:{line_number} 路径必须为 {expected_split}/文件名"
                )
            if not label or label != label.strip() or any(ch.isspace() for ch in label):
                raise ValueError(f"{label_file}:{line_number} 标签含空白或为空")
            rows.append((relative_path.as_posix(), label))
    return rows


def read_current_police(police_root: Path) -> list[PoliceRecord]:
    records: list[PoliceRecord] = []
    seen_names: set[str] = set()
    label_paths: set[str] = set()
    image_paths: set[str] = set()

    for split in SPLITS:
        label_file = police_root / f"{split}.txt"
        image_dir = police_root / split
        if not label_file.is_file() or not image_dir.is_dir():
            raise FileNotFoundError(f"警牌数据集布局不完整：{police_root}")

        for relative_path, label in parse_label_file(label_file, split):
            name = PurePosixPath(relative_path).name
            image_path = image_dir / name
            if POLICE_SUFFIX not in label:
                raise ValueError(f"警牌目录中存在非警牌标签：{label}")
            if name in seen_names:
                raise ValueError(f"警牌目录中存在重复文件名：{name}")
            if not image_path.is_file():
                raise FileNotFoundError(f"标签对应图片不存在：{image_path}")
            seen_names.add(name)
            label_paths.add(relative_path)
            records.append(
                PoliceRecord(
                    name=name,
                    label=label,
                    image_path=image_path,
                    current_split=split,
                    source_backup="current",
                    source_relative_path=relative_path,
                )
            )

        for image_path in image_dir.iterdir():
            if not image_path.is_file():
                raise ValueError(f"警牌图片目录中存在非文件项目：{image_path}")
            image_paths.add(f"{split}/{image_path.name}")

    if label_paths != image_paths:
        raise ValueError(
            f"当前警牌图片/标签不一致：缺图 {len(label_paths - image_paths)}，"
            f"孤立图片 {len(image_paths - label_paths)}"
        )
    return records


def read_backup_police(audit_backup_root: Path) -> list[PoliceRecord]:
    sources = (
        ("20260717_173909", "main_labels"),
        ("20260717_203319", ""),
    )
    records: list[PoliceRecord] = []
    seen_names: set[str] = set()

    for backup_name, label_subdir in sources:
        backup_root = audit_backup_root / backup_name
        for split in SPLITS:
            label_file = backup_root / label_subdir / f"{split}.txt"
            deleted_root = backup_root / "deleted_images"
            for relative_path, label in parse_label_file(label_file, split):
                if POLICE_SUFFIX not in label:
                    continue
                relative = PurePosixPath(relative_path)
                image_path = deleted_root.joinpath(*relative.parts)
                if not image_path.is_file():
                    continue
                if relative.name in seen_names:
                    raise ValueError(f"两个删除备份中存在同名警牌：{relative.name}")
                seen_names.add(relative.name)
                records.append(
                    PoliceRecord(
                        name=relative.name,
                        label=label,
                        image_path=image_path,
                        current_split=None,
                        source_backup=backup_name,
                        source_relative_path=relative_path,
                    )
                )

    if len(records) != EXPECTED_BACKUP_CANDIDATES:
        raise ValueError(
            f"预期找到 {EXPECTED_BACKUP_CANDIDATES} 张警牌，实际找到 {len(records)} 张"
        )
    return records


def select_new_candidates(
    current: list[PoliceRecord], backup: list[PoliceRecord]
) -> tuple[list[PoliceRecord], int]:
    current_by_name = {record.name: record for record in current}
    new_records: list[PoliceRecord] = []
    already_restored = 0

    for candidate in backup:
        existing = current_by_name.get(candidate.name)
        if existing is None:
            new_records.append(candidate)
            continue
        if existing.label != candidate.label or not os.path.samefile(
            existing.image_path, candidate.image_path
        ):
            raise FileExistsError(
                f"警牌文件名与现有数据冲突，且不是同一个硬链接：{candidate.name}"
            )
        already_restored += 1
    return new_records, already_restored


def build_assignments(records: list[PoliceRecord], seed: int) -> list[Assignment]:
    ordered = sorted(records, key=lambda record: (record.name, record.label))
    random.Random(f"{seed}:{POLICE_SUFFIX}").shuffle(ordered)
    train_count = int(len(ordered) * 0.8 + 0.5)
    return [
        Assignment(record=record, destination_split="train" if index < train_count else "val")
        for index, record in enumerate(ordered)
    ]


def atomic_write_text(path: Path, text: str) -> None:
    temporary = path.with_name(f".{path.name}.tmp")
    temporary.write_bytes(text.encode("utf-8"))
    os.replace(temporary, path)


def write_manifest(path: Path, assignments: list[Assignment]) -> None:
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle, delimiter="\t", lineterminator="\n")
        writer.writerow(
            (
                "filename",
                "label",
                "source_backup",
                "source_relative_path",
                "previous_split",
                "destination_split",
            )
        )
        for assignment in assignments:
            record = assignment.record
            writer.writerow(
                (
                    record.name,
                    record.label,
                    record.source_backup,
                    record.source_relative_path,
                    record.current_split or "",
                    assignment.destination_split,
                )
            )


def validate_final_police(police_root: Path, expected_count: int) -> None:
    records = read_current_police(police_root)
    if len(records) != expected_count:
        raise ValueError(f"恢复后警牌应为 {expected_count} 张，实际为 {len(records)} 张")


def update_split_report(
    special_root: Path, backup_root: Path, train_count: int, val_count: int
) -> None:
    report_path = special_root / "split_report.json"
    if not report_path.is_file():
        return
    shutil.copy2(report_path, backup_root / "split_report.before_police_restore.json")
    report = json.loads(report_path.read_text(encoding="utf-8"))
    report.setdefault("categories", {})[POLICE_SUFFIX] = {
        "total": train_count + val_count,
        "train": train_count,
        "val": val_count,
    }
    report["current_total"] = sum(
        int(category["total"]) for category in report["categories"].values()
    )
    report.setdefault("updates", []).append(
        {
            "type": "restore_police_from_audit_backups",
            "applied_at": datetime.now().isoformat(timespec="seconds"),
            "restored": EXPECTED_BACKUP_CANDIDATES,
            "train": train_count,
            "val": val_count,
            "backup_root": str(backup_root),
        }
    )
    atomic_write_text(
        report_path, json.dumps(report, ensure_ascii=False, indent=2) + "\n"
    )


def apply_restore(
    dataset_root: Path,
    police_root: Path,
    assignments: list[Assignment],
    new_candidates: list[PoliceRecord],
    seed: int,
) -> Path:
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    backup_root = (
        dataset_root / ".special_migration_backups" / f"{timestamp}_police_restore"
    )
    backup_root.mkdir(parents=True, exist_ok=False)
    for split in SPLITS:
        shutil.copy2(police_root / f"{split}.txt", backup_root / f"{split}.txt")
    write_manifest(backup_root / "police_restore_manifest.tsv", assignments)

    assignment_by_name = {assignment.record.name: assignment for assignment in assignments}
    new_names = {record.name for record in new_candidates}
    created_links: list[Path] = []
    moved_existing: list[tuple[Path, Path]] = []

    try:
        for candidate in new_candidates:
            destination = police_root / assignment_by_name[candidate.name].destination_split / candidate.name
            if destination.exists():
                raise FileExistsError(f"恢复目标已存在：{destination}")
            os.link(candidate.image_path, destination)
            created_links.append(destination)

        for assignment in assignments:
            record = assignment.record
            if record.name in new_names or record.current_split == assignment.destination_split:
                continue
            source = record.image_path
            destination = police_root / assignment.destination_split / record.name
            if destination.exists():
                raise FileExistsError(f"重分割目标已存在：{destination}")
            os.replace(source, destination)
            moved_existing.append((source, destination))

        for split in SPLITS:
            lines = sorted(
                (
                    f"{split}/{assignment.record.name}\t{assignment.record.label}"
                    for assignment in assignments
                    if assignment.destination_split == split
                ),
                key=lambda line: line.split("\t", 1)[0],
            )
            atomic_write_text(police_root / f"{split}.txt", "".join(f"{line}\n" for line in lines))

        validate_final_police(police_root, len(assignments))
        train_count = sum(a.destination_split == "train" for a in assignments)
        val_count = len(assignments) - train_count
        update_split_report(dataset_root / "special", backup_root, train_count, val_count)

    except Exception:
        for split in SPLITS:
            shutil.copy2(backup_root / f"{split}.txt", police_root / f"{split}.txt")
        for source, destination in reversed(moved_existing):
            if destination.exists() and not source.exists():
                os.replace(destination, source)
        for link_path in reversed(created_links):
            if link_path.exists():
                link_path.unlink()
        old_report = backup_root / "split_report.before_police_restore.json"
        if old_report.exists():
            shutil.copy2(old_report, dataset_root / "special" / "split_report.json")
        raise

    report = {
        "applied_at": datetime.now().isoformat(timespec="seconds"),
        "seed": seed,
        "restored_from_backups": len(new_candidates),
        "total": len(assignments),
        "train": sum(a.destination_split == "train" for a in assignments),
        "val": sum(a.destination_split == "val" for a in assignments),
        "backup_root": str(backup_root),
    }
    report_text = json.dumps(report, ensure_ascii=False, indent=2) + "\n"
    atomic_write_text(backup_root / "police_restore_report.json", report_text)
    atomic_write_text(dataset_root / "special" / "police_restore_report.json", report_text)
    return backup_root


def main() -> None:
    args = parse_args()
    dataset_root = args.dataset_root.resolve()
    audit_backup_root = args.audit_backup_root.resolve()
    police_root = dataset_root / "special" / POLICE_SUFFIX

    current = read_current_police(police_root)
    backup = read_backup_police(audit_backup_root)
    new_candidates, already_restored = select_new_candidates(current, backup)
    combined = current + new_candidates
    assignments = build_assignments(combined, args.seed)
    train_count = sum(a.destination_split == "train" for a in assignments)
    val_count = len(assignments) - train_count
    moves = sum(
        a.record.current_split is not None
        and a.record.current_split != a.destination_split
        for a in assignments
    )

    print("警牌恢复预演")
    print("-" * 56)
    print(f"当前警牌：          {len(current):>6,}")
    print(f"删除备份候选：      {len(backup):>6,}")
    print(f"本次新增恢复：      {len(new_candidates):>6,}")
    print(f"已经恢复：          {already_restored:>6,}")
    print(f"恢复后总数：        {len(assignments):>6,}")
    print(f"目标 train/val：    {train_count:,} / {val_count:,}")
    print(f"现有图片需换分组：  {moves:>6,}")

    if not args.apply:
        print("\n当前为只读预演；确认后添加 --apply 执行。")
        return
    if not new_candidates and moves == 0:
        print("\n警牌已经全部恢复且分割一致，无需修改。")
        return

    backup_root = apply_restore(
        dataset_root, police_root, assignments, new_candidates, args.seed
    )
    print(f"\n警牌恢复完成：{police_root}")
    print(f"恢复记录与旧标签备份：{backup_root}")


if __name__ == "__main__":
    main()
