#!/usr/bin/env python3
"""为当前 CBLPRD 嵌套目录生成可直接用于 PaddleOCR 的清单。"""

from __future__ import annotations

import argparse
import os
from collections import Counter
from pathlib import Path, PurePosixPath


SCRIPT_DIR = Path(__file__).resolve().parent
DATASETS_DIR = SCRIPT_DIR.parent / "datasets"
DEFAULT_DICT_PATH = (
    SCRIPT_DIR.parent / "PaddleOCR" / "ppocr" / "utils" / "cblprd_plate_dict.txt"
)
SOURCE_DIR_NAME = "CBLPRD"
SPLITS = ("train", "val")
DATASETS = (
    ("basic", Path("basic"), "basic"),
    ("hard", Path("hard"), "hard"),
    ("使", Path("special") / "使", "special_使"),
    ("学", Path("special") / "学", "special_学"),
    ("港", Path("special") / "港", "special_港"),
    ("澳", Path("special") / "澳", "special_澳"),
    ("警", Path("special") / "警", "special_警"),
    ("领", Path("special") / "领", "special_领"),
)


def default_dataset_root() -> Path:
    """兼容本地旧目录名；新目录名 CBLPRD 优先。"""
    candidates = (
        DATASETS_DIR / "CBLPRD",
        DATASETS_DIR / "CBLPRD-330k",
    )
    for candidate in candidates:
        if (candidate / SOURCE_DIR_NAME).is_dir():
            return candidate
    return candidates[0]


def parse_args() -> argparse.Namespace:
    default_root = default_dataset_root()
    parser = argparse.ArgumentParser(
        description=(
            "读取 <dataset-root>/CBLPRD/basic、hard、special 下的源标签，"
            "在 dataset-root 生成 8 份 train、8 份 val 和 2 份 all 清单。"
        )
    )
    parser.add_argument(
        "--dataset-root",
        type=Path,
        default=default_root,
        help=f"外层数据集根目录（默认：{default_root}）",
    )
    parser.add_argument(
        "--dict-path",
        type=Path,
        default=DEFAULT_DICT_PATH,
        help=f"73 字符字典（默认：{DEFAULT_DICT_PATH}）",
    )
    parser.add_argument(
        "--apply",
        action="store_true",
        help="通过全部检查后原子写入清单；默认仅预演",
    )
    return parser.parse_args()


def read_charset(dict_path: Path) -> set[str]:
    if not dict_path.is_file():
        raise FileNotFoundError(f"找不到字符字典：{dict_path}")
    characters = dict_path.read_text(encoding="utf-8-sig").splitlines()
    if len(characters) != 73 or any(len(character) != 1 for character in characters):
        raise ValueError(f"字符字典必须严格包含 73 个单字符：{dict_path}")
    if len(set(characters)) != len(characters):
        raise ValueError(f"字符字典存在重复字符：{dict_path}")
    return set(characters)


def image_names(image_dir: Path) -> set[str]:
    if not image_dir.is_dir():
        raise FileNotFoundError(f"找不到图片目录：{image_dir}")

    names: set[str] = set()
    unexpected: list[Path] = []
    for entry in image_dir.iterdir():
        if entry.is_dir() or not entry.is_file() or entry.suffix.lower() != ".jpg":
            unexpected.append(entry)
            continue
        names.add(entry.name)
    if unexpected:
        preview = ", ".join(str(path) for path in unexpected[:5])
        raise ValueError(f"图片目录包含非 JPG 文件或子目录：{preview}")
    return names


def read_source_lines(
    dataset_root: Path,
    relative_root: Path,
    split: str,
    charset: set[str],
) -> list[str]:
    source_root = dataset_root / SOURCE_DIR_NAME / relative_root
    label_file = source_root / f"{split}.txt"
    if not label_file.is_file():
        raise FileNotFoundError(f"找不到源标签：{label_file}")

    output_lines: list[str] = []
    label_image_names: set[str] = set()
    with label_file.open("r", encoding="utf-8-sig", newline="") as handle:
        for line_number, raw_line in enumerate(handle, start=1):
            line = raw_line.rstrip("\r\n")
            if not line or line.count("\t") != 1:
                raise ValueError(
                    f"{label_file}:{line_number} 必须严格为 路径<TAB>标签"
                )

            path_text, label = line.split("\t")
            if path_text != path_text.strip() or label != label.strip():
                raise ValueError(f"{label_file}:{line_number} 含首尾空白字符")

            source_relative = PurePosixPath(path_text.replace("\\", "/"))
            if (
                len(source_relative.parts) != 2
                or source_relative.parts[0] != split
                or source_relative.suffix.lower() != ".jpg"
            ):
                raise ValueError(
                    f"{label_file}:{line_number} 路径必须为 {split}/文件名.jpg"
                )
            if source_relative.name in label_image_names:
                raise ValueError(
                    f"{label_file}:{line_number} 重复图片：{source_relative.name}"
                )
            if len(label) not in (7, 8):
                raise ValueError(
                    f"{label_file}:{line_number} 标签长度不是 7 或 8：{label}"
                )
            unknown = sorted(set(label) - charset)
            if unknown:
                raise ValueError(
                    f"{label_file}:{line_number} 标签含字典外字符：{unknown}"
                )

            label_image_names.add(source_relative.name)
            merged_relative = PurePosixPath(
                SOURCE_DIR_NAME,
                *relative_root.parts,
                *source_relative.parts,
            ).as_posix()
            output_lines.append(f"{merged_relative}\t{label}")

    actual_image_names = image_names(source_root / split)
    missing_images = sorted(label_image_names - actual_image_names)
    missing_labels = sorted(actual_image_names - label_image_names)
    if missing_images or missing_labels:
        raise ValueError(
            f"{source_root / split} 图片与标签不一一对应："
            f"缺图 {len(missing_images)}，缺标签 {len(missing_labels)}；"
            f"示例缺图 {missing_images[:3]}，示例缺标签 {missing_labels[:3]}"
        )
    return output_lines


def build_manifests(
    dataset_root: Path, charset: set[str]
) -> tuple[dict[str, list[str]], dict[str, Counter[str]]]:
    source_root = dataset_root / SOURCE_DIR_NAME
    if not source_root.is_dir():
        raise FileNotFoundError(
            f"当前结构要求存在内层数据目录：{source_root}"
        )

    manifests: dict[str, list[str]] = {}
    counts = {split: Counter() for split in SPLITS}
    split_paths: dict[str, set[str]] = {}

    for split in SPLITS:
        all_lines: list[str] = []
        seen_paths: set[str] = set()
        for display_name, relative_root, manifest_suffix in DATASETS:
            lines = read_source_lines(
                dataset_root, relative_root, split, charset
            )
            for line in lines:
                relative_path = line.split("\t", 1)[0]
                if relative_path in seen_paths:
                    raise ValueError(f"{split} 清单出现重复路径：{relative_path}")
                seen_paths.add(relative_path)
            manifests[f"{split}_{manifest_suffix}.txt"] = lines
            counts[split][display_name] = len(lines)
            all_lines.extend(lines)

        manifests[f"{split}_all.txt"] = all_lines
        split_paths[split] = seen_paths

    overlap = split_paths["train"] & split_paths["val"]
    if overlap:
        raise ValueError(f"train/val 路径交叉：{sorted(overlap)[:5]}")
    return manifests, counts


def atomic_write(path: Path, lines: list[str]) -> None:
    temporary = path.with_name(f".{path.name}.tmp")
    temporary.write_bytes("".join(f"{line}\n" for line in lines).encode("utf-8"))
    os.replace(temporary, path)


def print_report(
    dataset_root: Path,
    manifests: dict[str, list[str]],
    counts: dict[str, Counter[str]],
) -> None:
    print("PP-OCRv4 CBLPRD 清单预演")
    print(f"外层根目录：{dataset_root}")
    print(f"内层数据目录：{dataset_root / SOURCE_DIR_NAME}")
    print("-" * 62)
    print(f"{'数据集':<12}{'train':>12}{'val':>12}{'合计':>12}")
    for display_name, _, _ in DATASETS:
        train_count = counts["train"][display_name]
        val_count = counts["val"][display_name]
        print(
            f"{display_name:<12}{train_count:>12,}{val_count:>12,}"
            f"{train_count + val_count:>12,}"
        )
    train_total = len(manifests["train_all.txt"])
    val_total = len(manifests["val_all.txt"])
    print("-" * 62)
    print(f"{'合计':<12}{train_total:>12,}{val_total:>12,}{train_total + val_total:>12,}")

    missing = sorted(name for name in manifests if not (dataset_root / name).is_file())
    if missing:
        print("\n当前缺失的输出清单：")
        for name in missing:
            print(f"  - {name}")
    else:
        print("\n18 份输出清单当前均已存在；--apply 会按源标签重建。")


def main() -> None:
    args = parse_args()
    dataset_root = args.dataset_root.resolve()
    charset = read_charset(args.dict_path.resolve())
    manifests, counts = build_manifests(dataset_root, charset)
    print_report(dataset_root, manifests, counts)

    if not args.apply:
        print("\n当前为只读预演；确认后添加 --apply 原子重建全部清单。")
        return

    for name, lines in manifests.items():
        atomic_write(dataset_root / name, lines)
    for name, lines in manifests.items():
        expected = "".join(f"{line}\n" for line in lines).encode("utf-8")
        if (dataset_root / name).read_bytes() != expected:
            raise RuntimeError(f"写入后校验失败：{dataset_root / name}")
    print(f"\n已原子重建并校验 18 份清单：{dataset_root}")


if __name__ == "__main__":
    main()
