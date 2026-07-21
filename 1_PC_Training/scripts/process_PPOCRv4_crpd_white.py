#!/usr/bin/env python3
"""从 CRPD 类别 3 中匹配可信警牌，并追加到 PP-OCRv4 数据集。"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import sys
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

import cv2
import numpy as np


SCRIPT_DIR = Path(__file__).resolve().parent
DATASETS_ROOT = SCRIPT_DIR.parent / "datasets"
DEFAULT_CRPD_ROOT = DATASETS_ROOT / "CRPD"
DEFAULT_TRUSTED_ROOT = DATASETS_ROOT / "lprnet_7char"
DEFAULT_PPOCR_ROOT = DATASETS_ROOT / "CBLPRD-330k"

SUBSETS = ("CRPD_single", "CRPD_double", "CRPD_multi")
SOURCE_SPLITS = ("train", "val", "test")
TARGET_SPLITS = ("train", "val")
WHITE_PLATE_TYPE = "白色车牌"
POLICE_SUFFIX = "警"
CRPD_WHITE_CLASS = "3"
OUTPUT_SIZE = (128, 48)
MAX_MATCH_MSE = 100.0
MIN_MATCH_GAP = 100.0
REJECTED_MANIFEST_NAME = "crpd_white_rejected.txt"


@dataclass(frozen=True)
class TrustedPlate:
    target_split: str
    label: str
    image_path: Path


@dataclass(frozen=True)
class CandidatePlate:
    subset: str
    source_split: str
    image_stem: str
    line_index: int
    label: str
    coordinates: np.ndarray
    image_path: Path

    @property
    def source_id(self) -> str:
        return (
            f"{self.subset}/{self.source_split}/{self.image_stem}:"
            f"{self.line_index}"
        )


@dataclass(frozen=True)
class PlateMatch:
    trusted: TrustedPlate
    candidate: CandidatePlate
    best_mse: float
    second_mse: float
    output_name: str

    @property
    def target_relative_path(self) -> str:
        return f"{self.trusted.target_split}/{self.output_name}"


@dataclass(frozen=True)
class TargetLabels:
    path: Path
    raw_lines: list[str]
    labels_by_path: dict[str, str]
    original_size: int
    original_mtime_ns: int


def positive_int(value: str) -> int:
    parsed = int(value)
    if parsed < 1:
        raise argparse.ArgumentTypeError("必须是大于 0 的整数")
    return parsed


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "遍历 CRPD 全部子集的类别 3 标注，通过可信 LPR 警牌裁剪图进行 "
            "一一匹配，并将 128x48 警牌裁剪追加到现有 PP-OCRv4 数据集。"
        )
    )
    parser.add_argument(
        "--crpd-root",
        type=Path,
        default=DEFAULT_CRPD_ROOT,
        help=f"CRPD 根目录（默认：{DEFAULT_CRPD_ROOT}）",
    )
    parser.add_argument(
        "--trusted-root",
        type=Path,
        default=DEFAULT_TRUSTED_ROOT,
        help=f"包含可信白牌清单和图片的根目录（默认：{DEFAULT_TRUSTED_ROOT}）",
    )
    parser.add_argument(
        "--ppocr-root",
        type=Path,
        default=DEFAULT_PPOCR_ROOT,
        help=f"待追加的 PP-OCR 数据集根目录（默认：{DEFAULT_PPOCR_ROOT}）",
    )
    parser.add_argument(
        "--workers",
        type=positive_int,
        default=min(16, (os.cpu_count() or 4) * 2),
        help="并行读取 CRPD 小标签文件的线程数（默认：最多 16）",
    )
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument(
        "--dry-run",
        action="store_true",
        help="完成全部配对和冲突检查，但不生成图片或修改标签",
    )
    action.add_argument(
        "--apply",
        action="store_true",
        help="确认生成警牌裁剪并追加 train.txt/val.txt",
    )
    return parser.parse_args()


def ensure_inside_root(path: Path, root: Path, description: str) -> None:
    try:
        path.relative_to(root)
    except ValueError as exc:
        raise ValueError(f"{description} 越过数据集根目录：{path}") from exc


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


def order_quad_points(points: np.ndarray) -> np.ndarray:
    sums = points.sum(axis=1)
    differences = np.diff(points, axis=1).reshape(-1)
    ordered = np.zeros((4, 2), dtype=np.float32)
    ordered[0] = points[np.argmin(sums)]
    ordered[2] = points[np.argmax(sums)]
    ordered[1] = points[np.argmin(differences)]
    ordered[3] = points[np.argmax(differences)]
    return ordered


def warp_plate(
    image: np.ndarray, coordinates: np.ndarray, size: tuple[int, int]
) -> np.ndarray:
    width, height = size
    destination = np.array(
        [[0, 0], [width - 1, 0], [width - 1, height - 1], [0, height - 1]],
        dtype=np.float32,
    )
    transform = cv2.getPerspectiveTransform(
        order_quad_points(coordinates), destination
    )
    return cv2.warpPerspective(
        image,
        transform,
        (width, height),
        flags=cv2.INTER_CUBIC,
        borderMode=cv2.BORDER_REPLICATE,
    )


def load_trusted_plates(trusted_root: Path) -> list[TrustedPlate]:
    classified_root = trusted_root / "classified_txts"
    trusted: list[TrustedPlate] = []
    seen_paths: set[Path] = set()

    for split in TARGET_SPLITS:
        label_file = classified_root / f"type_{WHITE_PLATE_TYPE}_{split}.txt"
        if not label_file.is_file():
            raise FileNotFoundError(f"找不到可信白牌清单：{label_file}")

        with label_file.open("r", encoding="utf-8-sig") as file:
            for line_no, raw_line in enumerate(file, start=1):
                stripped = raw_line.strip()
                if not stripped:
                    continue
                fields = stripped.split(maxsplit=2)
                if len(fields) != 3:
                    raise ValueError(
                        f"{label_file}:{line_no} 不是‘图片路径 标签 类型’格式"
                    )
                relative_text, label, plate_type = fields
                if plate_type != WHITE_PLATE_TYPE:
                    raise ValueError(
                        f"{label_file}:{line_no} 类型不是 {WHITE_PLATE_TYPE}"
                    )
                if not label.endswith(POLICE_SUFFIX):
                    raise ValueError(
                        f"{label_file}:{line_no} 可信白牌标签不以“警”结尾：{label}"
                    )

                relative_path = Path(relative_text)
                if not relative_path.parts or relative_path.parts[0] != split:
                    raise ValueError(
                        f"{label_file}:{line_no} 路径不属于 {split}：{relative_text}"
                    )
                image_path = (trusted_root / relative_path).resolve()
                ensure_inside_root(image_path, trusted_root, "可信图片路径")
                if not image_path.is_file():
                    raise FileNotFoundError(f"找不到可信图片：{image_path}")
                if image_path in seen_paths:
                    raise ValueError(f"可信图片被重复引用：{image_path}")
                seen_paths.add(image_path)
                trusted.append(
                    TrustedPlate(
                        target_split=split,
                        label=label,
                        image_path=image_path,
                    )
                )

    if not trusted:
        raise ValueError("可信白牌清单为空")
    return trusted


def parse_crpd_label_file(
    task: tuple[str, str, Path, Path]
) -> list[CandidatePlate]:
    subset, split, label_file, image_root = task
    image_path = image_root / f"{label_file.stem}.jpg"
    candidates: list[CandidatePlate] = []
    with label_file.open("r", encoding="utf-8-sig") as file:
        for line_index, raw_line in enumerate(file):
            fields = raw_line.split()
            if len(fields) < 10 or fields[8] != CRPD_WHITE_CLASS:
                continue
            try:
                coordinates = np.array(
                    [float(value) for value in fields[:8]],
                    dtype=np.float32,
                ).reshape(4, 2)
            except ValueError as exc:
                raise ValueError(
                    f"{label_file}:{line_index + 1} 四点坐标非法"
                ) from exc
            if not np.isfinite(coordinates).all():
                raise ValueError(
                    f"{label_file}:{line_index + 1} 四点坐标含非有限值"
                )
            candidates.append(
                CandidatePlate(
                    subset=subset,
                    source_split=split,
                    image_stem=label_file.stem,
                    line_index=line_index,
                    label=fields[9],
                    coordinates=coordinates,
                    image_path=image_path,
                )
            )
    return candidates


def load_crpd_candidates(
    crpd_root: Path, workers: int
) -> list[CandidatePlate]:
    tasks: list[tuple[str, str, Path, Path]] = []
    for subset in SUBSETS:
        for split in SOURCE_SPLITS:
            split_root = crpd_root / subset / split
            label_root = split_root / "labels"
            image_root = split_root / "images"
            if not label_root.exists():
                continue
            tasks.extend(
                (subset, split, label_file, image_root)
                for label_file in sorted(label_root.glob("*.txt"))
            )

    candidates: list[CandidatePlate] = []
    with ThreadPoolExecutor(max_workers=workers) as executor:
        for parsed in executor.map(parse_crpd_label_file, tasks):
            candidates.extend(parsed)

    for image_path in {candidate.image_path for candidate in candidates}:
        if not image_path.is_file():
            raise FileNotFoundError(
                f"类别 3 标注对应原图不存在：{image_path}"
            )

    if not candidates:
        raise ValueError("未在 CRPD 中找到类别 3 候选")
    return candidates


def build_output_name(candidate: CandidatePlate) -> str:
    safe_stem = re.sub(r"[^A-Za-z0-9_-]+", "_", candidate.image_stem)
    safe_stem = safe_stem.strip("_")[:40] or "image"
    digest = hashlib.sha1(candidate.source_id.encode("utf-8")).hexdigest()[:8]
    subset_name = candidate.subset.removeprefix("CRPD_")
    return (
        f"crpd_white_{subset_name}_{candidate.source_split}_{safe_stem}_"
        f"{candidate.line_index}_{digest}.jpg"
    )


def match_plates(
    trusted: list[TrustedPlate], candidates: list[CandidatePlate]
) -> list[PlateMatch]:
    trusted_images = [read_image(item.image_path) for item in trusted]
    trusted_shapes = {image.shape for image in trusted_images}
    if len(trusted_shapes) != 1:
        raise ValueError(f"可信图片尺寸不统一：{sorted(trusted_shapes)}")
    height, width, channels = trusted_images[0].shape
    if channels != 3:
        raise ValueError("可信图片必须是三通道彩色图")

    image_cache: dict[Path, np.ndarray] = {}
    candidate_crops: list[np.ndarray] = []
    for candidate in candidates:
        image = image_cache.get(candidate.image_path)
        if image is None:
            image = read_image(candidate.image_path)
            image_cache[candidate.image_path] = image
        candidate_crops.append(
            warp_plate(image, candidate.coordinates, (width, height))
        )
    candidate_stack = np.stack(candidate_crops).astype(np.float32)

    matches: list[PlateMatch] = []
    used_candidates: dict[int, TrustedPlate] = {}
    for item, trusted_image in zip(trusted, trusted_images):
        squared_error = (
            candidate_stack - trusted_image.astype(np.float32)
        ) ** 2
        scores = np.mean(squared_error, axis=(1, 2, 3))
        ranked = np.argsort(scores)
        best_index = int(ranked[0])
        second_mse = float(scores[int(ranked[1])]) if len(ranked) > 1 else float("inf")
        best_mse = float(scores[best_index])

        if best_mse > MAX_MATCH_MSE:
            raise ValueError(
                f"可信警牌 {item.label} 无可靠 CRPD 匹配：best MSE={best_mse:.2f}"
            )
        if second_mse - best_mse < MIN_MATCH_GAP:
            raise ValueError(
                f"可信警牌 {item.label} 匹配不唯一：best={best_mse:.2f}, "
                f"second={second_mse:.2f}"
            )
        if best_index in used_candidates:
            previous = used_candidates[best_index]
            raise ValueError(
                f"两个可信条目匹配到同一 CRPD 框：{previous.label} / {item.label}"
            )
        used_candidates[best_index] = item

        candidate = candidates[best_index]
        matches.append(
            PlateMatch(
                trusted=item,
                candidate=candidate,
                best_mse=best_mse,
                second_mse=second_mse,
                output_name=build_output_name(candidate),
            )
        )

    output_names = [match.target_relative_path for match in matches]
    if len(output_names) != len(set(output_names)):
        raise ValueError("生成的警牌输出文件名发生冲突")
    return matches


def read_target_labels(ppocr_root: Path, split: str) -> TargetLabels:
    image_root = ppocr_root / split
    label_file = ppocr_root / f"{split}.txt"
    if not image_root.is_dir():
        raise FileNotFoundError(f"PP-OCR 图片目录不存在：{image_root}")
    if not label_file.is_file():
        raise FileNotFoundError(f"PP-OCR 标签不存在：{label_file}")

    before = label_file.stat()
    raw_lines: list[str] = []
    labels_by_path: dict[str, str] = {}
    with label_file.open("r", encoding="utf-8-sig") as file:
        for line_no, raw_line in enumerate(file, start=1):
            stripped = raw_line.rstrip("\r\n")
            if not stripped:
                continue
            fields = stripped.split("\t", maxsplit=1)
            if len(fields) != 2:
                raise ValueError(
                    f"{label_file}:{line_no} 不是 PaddleOCR 的 路径<TAB>标签 格式"
                )
            relative_text, label = fields
            if not relative_text.lower().endswith(".jpg"):
                raise ValueError(
                    f"{label_file}:{line_no} 图片路径不是 .jpg：{relative_text}"
                )
            if not label or any(character.isspace() for character in label):
                raise ValueError(
                    f"{label_file}:{line_no} 车牌标签为空或含空白字符：{label!r}"
                )
            path_parts = relative_text.split("/")
            if (
                not relative_text.startswith(f"{split}/")
                or "\\" in relative_text
                or any(part in ("", ".", "..") for part in path_parts)
            ):
                raise ValueError(
                    f"{label_file}:{line_no} 图片路径不属于 {split}：{relative_text}"
                )
            if relative_text in labels_by_path:
                raise ValueError(
                    f"{label_file}:{line_no} 图片路径重复：{relative_text}"
                )
            labels_by_path[relative_text] = label
            raw_lines.append(stripped)
    after = label_file.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise OSError(f"检查期间标签文件被外部修改，请重试：{label_file}")
    return TargetLabels(
        path=label_file,
        raw_lines=raw_lines,
        labels_by_path=labels_by_path,
        original_size=before.st_size,
        original_mtime_ns=before.st_mtime_ns,
    )


def read_rejected_manifest(ppocr_root: Path) -> dict[str, str]:
    manifest = ppocr_root / REJECTED_MANIFEST_NAME
    if not manifest.exists():
        return {}

    rejected: dict[str, str] = {}
    with manifest.open("r", encoding="utf-8-sig") as file:
        for line_no, raw_line in enumerate(file, start=1):
            line = raw_line.rstrip("\r\n")
            fields = line.split("\t")
            if len(fields) != 2:
                raise ValueError(
                    f"{manifest}:{line_no} 不是 路径<TAB>标签 格式"
                )
            relative_path, label = fields
            if (
                not relative_path.lower().endswith(".jpg")
                or not relative_path.startswith(("train/", "val/"))
                or "\\" in relative_path
                or not label
                or any(character.isspace() for character in label)
            ):
                raise ValueError(f"{manifest}:{line_no} 拒绝项非法：{line!r}")
            if relative_path in rejected:
                raise ValueError(
                    f"{manifest}:{line_no} 拒绝路径重复：{relative_path}"
                )
            rejected[relative_path] = label
    return rejected


def plan_additions(
    ppocr_root: Path,
    matches: list[PlateMatch],
    target_labels: dict[str, TargetLabels],
    rejected: dict[str, str],
) -> tuple[list[PlateMatch], list[PlateMatch], int, int]:
    images_to_generate: list[PlateMatch] = []
    labels_to_append: list[PlateMatch] = []
    existing_count = 0
    rejected_count = 0
    for match in matches:
        relative_path = match.target_relative_path
        rejected_label = rejected.get(relative_path)
        if rejected_label is not None:
            if rejected_label != match.trusted.label:
                raise ValueError(
                    f"人工拒绝标签冲突：{relative_path} 记录为 {rejected_label}"
                )
            if (ppocr_root / relative_path).exists():
                raise FileExistsError(
                    f"人工拒绝图片仍然存在，请删除或移出数据集：{relative_path}"
                )
            rejected_count += 1
            continue
        existing_label = target_labels[match.trusted.target_split].labels_by_path.get(
            relative_path
        )
        destination = ppocr_root / Path(*PurePosixPath(relative_path).parts)
        if existing_label is not None:
            if existing_label != match.trusted.label:
                raise ValueError(
                    f"目标标签冲突：{relative_path} 已标为 {existing_label}"
                )
            if not destination.is_file():
                images_to_generate.append(match)
                continue
            existing_count += 1
            continue
        if destination.exists():
            raise FileExistsError(f"警牌图片已存在但标签中没有记录：{destination}")
        images_to_generate.append(match)
        labels_to_append.append(match)
    return images_to_generate, labels_to_append, existing_count, rejected_count


def print_summary(
    trusted: list[TrustedPlate],
    candidates: list[CandidatePlate],
    matches: list[PlateMatch],
    images_to_generate: list[PlateMatch],
    labels_to_append: list[PlateMatch],
    existing_count: int,
    rejected_count: int,
) -> None:
    trusted_counts = Counter(item.target_split for item in trusted)
    source_counts = Counter(
        (match.candidate.subset, match.candidate.source_split) for match in matches
    )
    corrections = [
        match for match in matches if match.candidate.label != match.trusted.label
    ]
    max_mse = max(match.best_mse for match in matches)
    min_gap = min(match.second_mse - match.best_mse for match in matches)

    print(f"CRPD 类别 3 候选：{len(candidates):,}")
    print(
        f"可信警牌：{len(trusted):,}（train {trusted_counts['train']:,}，"
        f"val {trusted_counts['val']:,}）"
    )
    print(f"一一匹配：{len(matches):,}；最大 best MSE：{max_mse:.2f}")
    print(f"最小 best/second 间隔：{min_gap:.2f}")
    for (subset, split), count in sorted(source_counts.items()):
        print(f"  {subset}/{split}: {count:,}")

    print(f"\n采用可信标签纠正 CRPD 文字：{len(corrections)} 条")
    for match in corrections:
        print(
            f"  {match.candidate.label} -> {match.trusted.label} "
            f"({match.candidate.source_id})"
        )
    print(
        f"\n目标中完整存在：{existing_count:,}；"
        f"人工拒绝：{rejected_count:,}；"
        f"待生成/修复图片：{len(images_to_generate):,}；"
        f"待追加标签：{len(labels_to_append):,}；"
        f"输出尺寸：{OUTPUT_SIZE[0]}x{OUTPUT_SIZE[1]}"
    )


def apply_additions(
    ppocr_root: Path,
    images_to_generate: list[PlateMatch],
    labels_to_append: list[PlateMatch],
    target_labels: dict[str, TargetLabels],
) -> None:
    if not images_to_generate and not labels_to_append:
        print("所有可信警牌均已存在，无需修改。")
        return

    if labels_to_append:
        for split in TARGET_SPLITS:
            labels = target_labels[split]
            current = labels.path.stat()
            if (current.st_size, current.st_mtime_ns) != (
                labels.original_size,
                labels.original_mtime_ns,
            ):
                raise OSError(
                    f"配对期间标签文件被外部修改，请重新执行：{labels.path}"
                )

    stage_root = ppocr_root / ".crpd_white_stage"
    if stage_root.exists():
        raise FileExistsError(f"临时目录已存在，请先人工检查：{stage_root}")
    for split in TARGET_SPLITS:
        (stage_root / split).mkdir(parents=True, exist_ok=True)

    image_cache: dict[Path, np.ndarray] = {}
    for match in images_to_generate:
        source_image = image_cache.get(match.candidate.image_path)
        if source_image is None:
            source_image = read_image(match.candidate.image_path)
            image_cache[match.candidate.image_path] = source_image
        crop = warp_plate(
            source_image, match.candidate.coordinates, OUTPUT_SIZE
        )
        write_jpeg(
            stage_root / match.trusted.target_split / match.output_name, crop
        )

    staged_label_files: dict[str, Path] = {}
    if labels_to_append:
        for split in TARGET_SPLITS:
            staged_label = ppocr_root / f".{split}.crpd_white.tmp"
            if staged_label.exists():
                raise FileExistsError(
                    f"临时标签已存在，请先人工检查：{staged_label}"
                )
            additions = [
                match
                for match in labels_to_append
                if match.trusted.target_split == split
            ]
            with staged_label.open("w", encoding="utf-8", newline="\n") as file:
                for line in target_labels[split].raw_lines:
                    file.write(f"{line}\n")
                for match in additions:
                    file.write(
                        f"{match.target_relative_path}\t{match.trusted.label}\n"
                    )
            staged_label_files[split] = staged_label

    for match in images_to_generate:
        staged_image = (
            stage_root / match.trusted.target_split / match.output_name
        )
        destination = ppocr_root / match.target_relative_path
        staged_image.rename(destination)

    for split, staged_label in staged_label_files.items():
        staged_label.replace(ppocr_root / f"{split}.txt")

    for split in TARGET_SPLITS:
        (stage_root / split).rmdir()
    stage_root.rmdir()


def main() -> int:
    args = parse_args()
    crpd_root = args.crpd_root.expanduser().resolve()
    trusted_root = args.trusted_root.expanduser().resolve()
    ppocr_root = args.ppocr_root.expanduser().resolve()

    try:
        for path, description in (
            (crpd_root, "CRPD 根目录"),
            (trusted_root, "可信 LPR 数据集根目录"),
            (ppocr_root, "PP-OCR 数据集根目录"),
        ):
            if not path.is_dir():
                raise FileNotFoundError(f"{description}不存在：{path}")

        print("读取可信白牌清单...", flush=True)
        trusted = load_trusted_plates(trusted_root)
        print("并行扫描 CRPD 类别 3 标注...", flush=True)
        candidates = load_crpd_candidates(crpd_root, args.workers)
        print("执行可信裁剪与 CRPD 候选的全局图像匹配...", flush=True)
        matches = match_plates(trusted, candidates)
        print("检查现有 PP-OCR 标签和目标路径...", flush=True)
        target_labels = {
            split: read_target_labels(ppocr_root, split) for split in TARGET_SPLITS
        }
        rejected = read_rejected_manifest(ppocr_root)
        (
            images_to_generate,
            labels_to_append,
            existing_count,
            rejected_count,
        ) = plan_additions(
            ppocr_root, matches, target_labels, rejected
        )
        print_summary(
            trusted,
            candidates,
            matches,
            images_to_generate,
            labels_to_append,
            existing_count,
            rejected_count,
        )

        if args.dry_run:
            print("\nDry-run 完成：未生成图片，也未修改 PP-OCR 标签。")
            return 0

        apply_additions(
            ppocr_root,
            images_to_generate,
            labels_to_append,
            target_labels,
        )
        for split in TARGET_SPLITS:
            read_target_labels(ppocr_root, split)
        print(f"\n警牌追加完成：{ppocr_root}")
        print("train.txt/val.txt 严格格式校验通过：.jpg<TAB>车牌号")
        return 0
    except (OSError, ValueError) as exc:
        print(f"错误：{exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
