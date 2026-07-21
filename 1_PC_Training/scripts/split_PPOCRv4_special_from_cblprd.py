#!/usr/bin/env python3
"""从 CBLPRD basic/hard 中迁出特殊车牌，并按类别重新做 8:2 划分。"""

from __future__ import annotations

import argparse
import csv
import json
import os
import random
import shutil
from collections import Counter
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path, PurePosixPath


SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_DATASET_ROOT = SCRIPT_DIR.parent / "datasets" / "CBLPRD-330k"
SOURCE_NAMES = ("basic", "hard")
SPLITS = ("train", "val")
CATEGORIES = ("使", "学", "港", "澳", "警", "领")
DEFAULT_SEED = 20260717


@dataclass(frozen=True)
class Record:
    source_name: str
    source_split: str
    relative_path: PurePosixPath
    label: str
    original_line: str
    image_path: Path
    category: str | None


@dataclass(frozen=True)
class Migration:
    record: Record
    destination_split: str
    destination_path: Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "从 basic 和 hard 中识别使/学/港/澳/警/领车牌，按类别独立使用"
            "固定随机种子重新做 train/val=8:2 划分，并移动到 special/<类别>/。"
        )
    )
    parser.add_argument(
        "--dataset-root",
        type=Path,
        default=DEFAULT_DATASET_ROOT,
        help=f"CBLPRD 数据集根目录（默认：{DEFAULT_DATASET_ROOT}）",
    )
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED, help="固定随机种子")
    parser.add_argument(
        "--train-ratio",
        type=float,
        default=0.8,
        help="每个类别的训练集比例（默认：0.8）",
    )
    parser.add_argument(
        "--apply",
        action="store_true",
        help="实际移动图片并更新标签；默认只预演",
    )
    return parser.parse_args()


def classify_label(label: str) -> str | None:
    matched = [category for category in CATEGORIES if category in label]
    if len(matched) > 1:
        raise ValueError(f"标签同时命中多个特殊类别：{label} -> {matched}")
    return matched[0] if matched else None


def read_source_records(source_root: Path, source_name: str) -> list[Record]:
    records: list[Record] = []
    seen_paths: set[str] = set()

    for split in SPLITS:
        label_file = source_root / f"{split}.txt"
        image_dir = source_root / split
        if not label_file.is_file() or not image_dir.is_dir():
            raise FileNotFoundError(f"数据集布局不完整：{source_root}")

        with label_file.open("r", encoding="utf-8-sig", newline="") as handle:
            for line_number, raw_line in enumerate(handle, start=1):
                line = raw_line.rstrip("\r\n")
                if not line:
                    raise ValueError(f"{label_file}:{line_number} 存在空行")
                if line.count("\t") != 1:
                    raise ValueError(
                        f"{label_file}:{line_number} 必须严格为 路径<TAB>标签"
                    )

                path_text, label = line.split("\t")
                relative_path = PurePosixPath(path_text.replace("\\", "/"))
                if (
                    len(relative_path.parts) != 2
                    or relative_path.parts[0] != split
                    or not relative_path.name
                ):
                    raise ValueError(
                        f"{label_file}:{line_number} 路径必须为 {split}/文件名：{path_text}"
                    )
                if not label or label != label.strip() or any(ch.isspace() for ch in label):
                    raise ValueError(f"{label_file}:{line_number} 标签含空白或为空：{label!r}")

                path_key = relative_path.as_posix()
                if path_key in seen_paths:
                    raise ValueError(f"{source_root} 存在重复标签路径：{path_key}")
                seen_paths.add(path_key)

                image_path = source_root.joinpath(*relative_path.parts)
                if not image_path.is_file():
                    raise FileNotFoundError(f"标签对应图片不存在：{image_path}")

                records.append(
                    Record(
                        source_name=source_name,
                        source_split=split,
                        relative_path=relative_path,
                        label=label,
                        original_line=line,
                        image_path=image_path,
                        category=classify_label(label),
                    )
                )

    validate_dataset_bijection(source_root, records)
    return records


def validate_dataset_bijection(root: Path, records: list[Record]) -> None:
    label_paths = {record.relative_path.as_posix() for record in records}
    image_paths: set[str] = set()

    for split in SPLITS:
        image_dir = root / split
        if not image_dir.is_dir():
            raise FileNotFoundError(f"图片目录不存在：{image_dir}")
        for entry in image_dir.iterdir():
            if not entry.is_file():
                raise ValueError(f"图片目录中存在非文件项目：{entry}")
            image_paths.add(f"{split}/{entry.name}")

    missing_images = label_paths - image_paths
    orphan_images = image_paths - label_paths
    if missing_images or orphan_images:
        raise ValueError(
            f"{root} 图片/标签不一致：缺图 {len(missing_images)}，孤立图片 {len(orphan_images)}"
        )


def ensure_special_is_empty(special_root: Path) -> None:
    if special_root.exists() and not special_root.is_dir():
        raise NotADirectoryError(f"special 目标不是目录：{special_root}")
    for category in CATEGORIES:
        category_root = special_root / category
        if category_root.exists() and not category_root.is_dir():
            raise NotADirectoryError(f"类别目标不是目录：{category_root}")
        if category_root.is_dir() and any(category_root.iterdir()):
            raise FileExistsError(
                f"目标类别目录必须为空，避免覆盖已有数据：{category_root}"
            )


def build_migrations(
    all_records: list[Record], special_root: Path, seed: int, train_ratio: float
) -> list[Migration]:
    if not 0.0 < train_ratio < 1.0:
        raise ValueError("--train-ratio 必须位于 0 和 1 之间")

    migrations: list[Migration] = []
    for category in CATEGORIES:
        category_records = sorted(
            (record for record in all_records if record.category == category),
            key=lambda record: (
                record.source_name,
                record.source_split,
                record.relative_path.as_posix(),
            ),
        )
        if not category_records:
            raise ValueError(f"没有找到类别“{category}”的样本")

        name_counts = Counter(record.relative_path.name for record in category_records)
        duplicates = sorted(name for name, count in name_counts.items() if count > 1)
        if duplicates:
            raise ValueError(
                f"类别“{category}”存在目标同名文件，无法安全迁移：{duplicates[:10]}"
            )

        random.Random(f"{seed}:{category}").shuffle(category_records)
        train_count = int(len(category_records) * train_ratio + 0.5)
        for index, record in enumerate(category_records):
            destination_split = "train" if index < train_count else "val"
            destination_path = (
                special_root / category / destination_split / record.relative_path.name
            )
            migrations.append(
                Migration(
                    record=record,
                    destination_split=destination_split,
                    destination_path=destination_path,
                )
            )

    return migrations


def atomic_write_lines(path: Path, lines: list[str]) -> None:
    temporary = path.with_name(f".{path.name}.tmp")
    payload = "".join(f"{line}\n" for line in lines).encode("utf-8")
    temporary.write_bytes(payload)
    os.replace(temporary, path)


def write_manifest(path: Path, migrations: list[Migration]) -> None:
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle, delimiter="\t", lineterminator="\n")
        writer.writerow(
            (
                "source_dataset",
                "source_split",
                "source_relative_path",
                "label",
                "category",
                "destination_split",
                "destination_relative_path",
            )
        )
        for migration in migrations:
            record = migration.record
            writer.writerow(
                (
                    record.source_name,
                    record.source_split,
                    record.relative_path.as_posix(),
                    record.label,
                    record.category,
                    migration.destination_split,
                    f"{record.category}/{migration.destination_split}/{record.relative_path.name}",
                )
            )


def print_plan(all_records: list[Record], migrations: list[Migration]) -> None:
    print("\n特殊车牌迁移预演")
    print("-" * 72)
    print(f"{'类别':<8}{'basic':>10}{'hard':>10}{'合计':>10}{'train':>10}{'val':>10}")
    for category in CATEGORIES:
        basic = sum(
            record.category == category and record.source_name == "basic"
            for record in all_records
        )
        hard = sum(
            record.category == category and record.source_name == "hard"
            for record in all_records
        )
        train = sum(
            migration.record.category == category
            and migration.destination_split == "train"
            for migration in migrations
        )
        val = sum(
            migration.record.category == category
            and migration.destination_split == "val"
            for migration in migrations
        )
        print(f"{category:<8}{basic:>10,}{hard:>10,}{basic + hard:>10,}{train:>10,}{val:>10,}")
    print("-" * 72)
    print(f"待迁移总数：{len(migrations):,}")


def apply_migrations(
    dataset_root: Path,
    records_by_source: dict[str, list[Record]],
    migrations: list[Migration],
    seed: int,
    train_ratio: float,
) -> Path:
    special_root = dataset_root / "special"
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    backup_root = dataset_root / ".special_migration_backups" / timestamp
    backup_root.mkdir(parents=True, exist_ok=False)

    for source_name in SOURCE_NAMES:
        source_root = dataset_root / source_name
        for split in SPLITS:
            shutil.copy2(
                source_root / f"{split}.txt",
                backup_root / f"{source_name}_{split}.txt",
            )
    write_manifest(backup_root / "migration_manifest.tsv", migrations)

    for category in CATEGORIES:
        for split in SPLITS:
            (special_root / category / split).mkdir(parents=True, exist_ok=False)

    moved: list[Migration] = []
    try:
        for migration in migrations:
            os.replace(migration.record.image_path, migration.destination_path)
            moved.append(migration)

        for source_name, records in records_by_source.items():
            source_root = dataset_root / source_name
            for split in SPLITS:
                remaining_lines = [
                    record.original_line
                    for record in records
                    if record.source_split == split and record.category is None
                ]
                atomic_write_lines(source_root / f"{split}.txt", remaining_lines)

        for category in CATEGORIES:
            for split in SPLITS:
                destination_lines = sorted(
                    (
                        f"{split}/{migration.record.relative_path.name}\t{migration.record.label}"
                        for migration in migrations
                        if migration.record.category == category
                        and migration.destination_split == split
                    ),
                    key=lambda line: line.split("\t", 1)[0],
                )
                atomic_write_lines(
                    special_root / category / f"{split}.txt", destination_lines
                )

        for source_name in SOURCE_NAMES:
            remaining = [
                record
                for record in records_by_source[source_name]
                if record.category is None
            ]
            validate_dataset_bijection(dataset_root / source_name, remaining)

        for category in CATEGORIES:
            category_records = [
                Record(
                    source_name="special",
                    source_split=migration.destination_split,
                    relative_path=PurePosixPath(
                        migration.destination_split,
                        migration.record.relative_path.name,
                    ),
                    label=migration.record.label,
                    original_line="",
                    image_path=migration.destination_path,
                    category=category,
                )
                for migration in migrations
                if migration.record.category == category
            ]
            validate_dataset_bijection(special_root / category, category_records)

    except Exception:
        for source_name in SOURCE_NAMES:
            source_root = dataset_root / source_name
            for split in SPLITS:
                backup_label = backup_root / f"{source_name}_{split}.txt"
                if backup_label.exists():
                    shutil.copy2(backup_label, source_root / f"{split}.txt")
        for migration in reversed(moved):
            if migration.destination_path.exists() and not migration.record.image_path.exists():
                os.replace(migration.destination_path, migration.record.image_path)
        for category in CATEGORIES:
            category_root = special_root / category
            for split in SPLITS:
                label_file = category_root / f"{split}.txt"
                if label_file.exists():
                    label_file.unlink()
                image_dir = category_root / split
                if image_dir.exists():
                    image_dir.rmdir()
        raise

    report = {
        "applied_at": datetime.now().isoformat(timespec="seconds"),
        "dataset_root": str(dataset_root),
        "seed": seed,
        "train_ratio": train_ratio,
        "backup_root": str(backup_root),
        "total_migrated": len(migrations),
        "categories": {
            category: {
                "total": sum(m.record.category == category for m in migrations),
                "train": sum(
                    m.record.category == category and m.destination_split == "train"
                    for m in migrations
                ),
                "val": sum(
                    m.record.category == category and m.destination_split == "val"
                    for m in migrations
                ),
            }
            for category in CATEGORIES
        },
        "remaining": {
            source_name: {
                split: sum(
                    record.category is None and record.source_split == split
                    for record in records_by_source[source_name]
                )
                for split in SPLITS
            }
            for source_name in SOURCE_NAMES
        },
    }
    report_text = json.dumps(report, ensure_ascii=False, indent=2) + "\n"
    (backup_root / "split_report.json").write_text(report_text, encoding="utf-8")
    (special_root / "split_report.json").write_text(report_text, encoding="utf-8")
    return backup_root


def main() -> None:
    args = parse_args()
    dataset_root = args.dataset_root.resolve()
    special_root = dataset_root / "special"
    ensure_special_is_empty(special_root)

    records_by_source = {
        source_name: read_source_records(dataset_root / source_name, source_name)
        for source_name in SOURCE_NAMES
    }
    all_records = [
        record
        for source_name in SOURCE_NAMES
        for record in records_by_source[source_name]
    ]
    migrations = build_migrations(
        all_records, special_root, args.seed, args.train_ratio
    )
    print_plan(all_records, migrations)

    if not args.apply:
        print("\n当前为只读预演；确认数量后添加 --apply 执行。")
        return

    backup_root = apply_migrations(
        dataset_root,
        records_by_source,
        migrations,
        args.seed,
        args.train_ratio,
    )
    print(f"\n迁移完成：{special_root}")
    print(f"源标签和迁移清单备份：{backup_root}")


if __name__ == "__main__":
    main()
