#!/usr/bin/env python3
"""从原始车牌数据构建独立的 YOLOv8-OBB 数据集。"""

from __future__ import annotations

# %% Imports and constants
import argparse
import csv
import hashlib
import json
import math
import os
import random
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

from PIL import Image


SCRIPT_DIR = Path(__file__).resolve().parent
TRAINING_ROOT = SCRIPT_DIR.parent
DATASETS_ROOT = TRAINING_ROOT / "datasets"
DEFAULT_CCPD2019_ROOT = DATASETS_ROOT / "CCPD2019"
DEFAULT_CCPD2020_ROOT = DATASETS_ROOT / "CCPD2020" / "ccpd_green"
DEFAULT_CRPD_ROOT = DATASETS_ROOT / "CRPD"
DEFAULT_SPECIAL_ROOT = DATASETS_ROOT / "特殊车牌"
DEFAULT_OUTPUT_ROOT = DATASETS_ROOT / "yolo_obb_640"

SPLITS = ("train", "val", "test")
IMAGE_SUFFIXES = {".jpg", ".jpeg", ".png", ".bmp", ".webp"}
CLASS_NAMES = {
    0: "blue",
    1: "green",
    2: "yellow_single",
    3: "other",
}
CRPD_CLASS_MAPPING = {
    "0": 0,
    "1": 2,
    "3": 3,
}
SPECIAL_GROUPS = {
    "教练车牌": ("学", "learner"),
    "香港出入境车牌": ("港", "hongkong"),
    "澳门出入境车牌": ("澳", "macau"),
    "警用车牌": ("警", "police"),
}
CCPD2019_TRAIN_QUOTAS = {
    "ccpd_base": 2_000,
    "ccpd_blur": 1_000,
    "ccpd_challenge": 2_000,
    "ccpd_db": 1_000,
    "ccpd_fn": 2_000,
    "ccpd_rotate": 2_000,
    "ccpd_tilt": 2_500,
    "ccpd_weather": 500,
}
CCPD2019_NEGATIVE_TRAIN_QUOTA = 500
CRPD_SINGLE_TRAIN_TARGET = 10_000
TRAIN_YELLOW_SINGLE_REPEATS = 2
TRAIN_OTHER_REPEATS = 4
SELECTION_SPLIT_RATIOS = {
    "train": 0.80,
    "val": 0.10,
    "test": 0.10,
}
SPECIAL_SPLIT_RATIOS = {
    "train": 0.60,
    "val": 0.30,
    "test": 0.10,
}
SPECIAL_BASELINE_VAL_RATIO = 0.10
BALANCED_VAL_TARGETS = {
    0: 1_000,
    1: 1_000,
    2: 1_000,
}
CCPD2019_INVALID_NEGATIVE_STEMS = frozenset(
    {
        "1982",
        "2705",
        "3213",
        "3922",
        "4205",
        "4626",
        "5830",
    }
)
KNOWN_BAD_SOURCE_IMAGES = frozenset(
    {
        ("crpd", "CRPD_multi", "train", "44_0191"),
        ("crpd", "CRPD_multi", "train", "44_1333"),
        ("crpd", "CRPD_multi", "train", "45_0089"),
        ("crpd", "CRPD_multi", "train", "45_1404"),
        ("crpd", "CRPD_single", "val", "48_0951"),
    }
)


# %% Data structures
Point = tuple[float, float]


@dataclass(frozen=True)
class ObbLabel:
    class_id: int
    points: tuple[Point, Point, Point, Point]
    original_type: str


@dataclass(frozen=True)
class SourceSample:
    split: str
    source_kind: str
    source_partition: str
    source_image: Path
    source_label: Path | None
    output_stem: str
    labels: tuple[ObbLabel, ...]
    original_types: tuple[str, ...]
    short_side_640: float | None
    repeat_in_train: bool
    negative_reason: str
    other_in_train: bool = False
    dropped_type2_objects: int = 0
    missing_content_objects: int = 0
    repaired_degenerate_objects: int = 0


@dataclass(frozen=True)
class BuildItem:
    sample: SourceSample
    repeat_index: int
    output_stem: str


@dataclass(frozen=True)
class WrittenItem:
    item: BuildItem
    output_image: Path
    output_label: Path


# %% CLI
def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "从 CCPD2019、CCPD2020、CRPD 和现有特殊车牌构建独立的 "
            "YOLOv8-OBB 数据集；图片使用同卷 NTFS 硬链接。"
        )
    )
    parser.add_argument("--ccpd2019-root", type=Path, default=DEFAULT_CCPD2019_ROOT)
    parser.add_argument("--ccpd2020-root", type=Path, default=DEFAULT_CCPD2020_ROOT)
    parser.add_argument("--crpd-root", type=Path, default=DEFAULT_CRPD_ROOT)
    parser.add_argument("--special-root", type=Path, default=DEFAULT_SPECIAL_ROOT)
    parser.add_argument("--output-root", type=Path, default=DEFAULT_OUTPUT_ROOT)
    parser.add_argument("--seed", type=int, default=20260731)
    parser.add_argument("--workers", type=int, default=16)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--dry-run", action="store_true")
    action.add_argument("--apply", action="store_true")
    return parser.parse_args()


# %% Deterministic selection
def stable_digest(seed: int, namespace: str, value: str) -> bytes:
    payload = f"{seed}:{namespace}:{value}".encode("utf-8")
    return hashlib.sha256(payload).digest()


def stable_split(seed: int, namespace: str, path: Path) -> str:
    bucket = int.from_bytes(
        stable_digest(seed, f"{namespace}:split", path.as_posix())[:8],
        "big",
    ) % 10_000
    if bucket < 8_000:
        return "train"
    if bucket < 9_000:
        return "val"
    return "test"


def stable_sort(
    paths: Iterable[Path],
    seed: int,
    namespace: str,
) -> list[Path]:
    return sorted(
        paths,
        key=lambda path: stable_digest(
            seed,
            f"{namespace}:order",
            path.as_posix(),
        ),
    )


def select_product_split(
    paths: list[Path],
    train_quota: int,
    seed: int,
    namespace: str,
) -> dict[str, list[Path]]:
    quotas = {
        "train": train_quota,
        "val": max(1, round(train_quota / 8)),
        "test": max(1, round(train_quota / 8)),
    }
    buckets = {
        split: stable_sort(
            (
                path
                for path in paths
                if stable_split(seed, namespace, path) == split
            ),
            seed,
            f"{namespace}:{split}",
        )
        for split in SPLITS
    }
    for split in SPLITS:
        if len(buckets[split]) < quotas[split]:
            raise ValueError(
                f"{namespace}/{split} 候选 {len(buckets[split])} "
                f"少于配额 {quotas[split]}"
            )
    return {
        split: buckets[split][: quotas[split]]
        for split in SPLITS
    }


# %% Geometry and label helpers
def polygon_area(points: tuple[Point, ...]) -> float:
    return abs(
        sum(
            points[index][0] * points[(index + 1) % len(points)][1]
            - points[(index + 1) % len(points)][0] * points[index][1]
            for index in range(len(points))
        )
    ) / 2


def order_clockwise(points: Iterable[Point]) -> tuple[Point, Point, Point, Point]:
    unique = list(dict.fromkeys(points))
    if len(unique) != 4:
        raise ValueError(f"OBB 必须包含 4 个不同角点：{unique}")
    center_x = sum(point[0] for point in unique) / 4
    center_y = sum(point[1] for point in unique) / 4
    ordered = sorted(
        unique,
        key=lambda point: math.atan2(
            point[1] - center_y,
            point[0] - center_x,
        ),
    )
    first_index = min(
        range(4),
        key=lambda index: (
            ordered[index][0] + ordered[index][1],
            ordered[index][1],
            ordered[index][0],
        ),
    )
    rotated = ordered[first_index:] + ordered[:first_index]
    result = tuple(rotated)
    if polygon_area(result) <= 1e-9:
        raise ValueError(f"OBB 四边形面积为 0：{result}")
    return result  # type: ignore[return-value]


def clip_and_normalize_points(
    points: Iterable[Point],
    width: int,
    height: int,
) -> tuple[Point, Point, Point, Point]:
    if width <= 0 or height <= 0:
        raise ValueError(f"非法图片尺寸：{width}x{height}")
    normalized = [
        (
            min(max(x, 0.0), float(width)) / width,
            min(max(y, 0.0), float(height)) / height,
        )
        for x, y in points
    ]
    return order_clockwise(normalized)


def convex_hull(points: Iterable[Point]) -> list[Point]:
    unique = sorted(set(points))
    if len(unique) <= 1:
        return unique

    def cross(origin: Point, first: Point, second: Point) -> float:
        return (
            (first[0] - origin[0]) * (second[1] - origin[1])
            - (first[1] - origin[1]) * (second[0] - origin[0])
        )

    lower: list[Point] = []
    for point in unique:
        while (
            len(lower) >= 2
            and cross(lower[-2], lower[-1], point) <= 0
        ):
            lower.pop()
        lower.append(point)
    upper: list[Point] = []
    for point in reversed(unique):
        while (
            len(upper) >= 2
            and cross(upper[-2], upper[-1], point) <= 0
        ):
            upper.pop()
        upper.append(point)
    return lower[:-1] + upper[:-1]


def minimum_rectangle_short_side(points: Iterable[Point]) -> float:
    hull = convex_hull(points)
    if len(hull) < 3:
        raise ValueError(f"无法计算最小旋转矩形：{hull}")
    best_area = math.inf
    best_short_side = 0.0
    for index, point in enumerate(hull):
        next_point = hull[(index + 1) % len(hull)]
        angle = math.atan2(
            next_point[1] - point[1],
            next_point[0] - point[0],
        )
        cosine = math.cos(angle)
        sine = math.sin(angle)
        rotated_x = [
            x * cosine + y * sine
            for x, y in hull
        ]
        rotated_y = [
            -x * sine + y * cosine
            for x, y in hull
        ]
        rectangle_width = max(rotated_x) - min(rotated_x)
        rectangle_height = max(rotated_y) - min(rotated_y)
        area = rectangle_width * rectangle_height
        if area < best_area:
            best_area = area
            best_short_side = min(rectangle_width, rectangle_height)
    return best_short_side


def minimum_rectangle_points(
    points: Iterable[Point],
) -> tuple[Point, Point, Point, Point]:
    hull = convex_hull(points)
    if len(hull) < 3:
        raise ValueError(f"无法补全退化 OBB：{hull}")
    best: tuple[float, float, float, float, float, float, float] | None = None
    for index, point in enumerate(hull):
        next_point = hull[(index + 1) % len(hull)]
        angle = math.atan2(
            next_point[1] - point[1],
            next_point[0] - point[0],
        )
        cosine = math.cos(angle)
        sine = math.sin(angle)
        rotated_x = [x * cosine + y * sine for x, y in hull]
        rotated_y = [-x * sine + y * cosine for x, y in hull]
        min_x, max_x = min(rotated_x), max(rotated_x)
        min_y, max_y = min(rotated_y), max(rotated_y)
        area = (max_x - min_x) * (max_y - min_y)
        candidate = (
            area,
            angle,
            min_x,
            max_x,
            min_y,
            max_y,
            cosine,
        )
        if best is None or area < best[0]:
            best = candidate
    if best is None or best[0] <= 0:
        raise ValueError(f"退化 OBB 无法形成正面积矩形：{hull}")
    _, angle, min_x, max_x, min_y, max_y, cosine = best
    sine = math.sin(angle)
    rotated_corners = (
        (min_x, min_y),
        (max_x, min_y),
        (max_x, max_y),
        (min_x, max_y),
    )
    restored = [
        (
            rotated_x * cosine - rotated_y * sine,
            rotated_x * sine + rotated_y * cosine,
        )
        for rotated_x, rotated_y in rotated_corners
    ]
    return order_clockwise(restored)


def projected_short_side_640(
    points: Iterable[Point],
    image_width: int,
    image_height: int,
) -> float:
    return (
        minimum_rectangle_short_side(points)
        * 640
        / max(image_width, image_height)
    )


def parse_ccpd_points(image_path: Path) -> tuple[Point, Point, Point, Point]:
    fields = image_path.stem.split("-")
    if len(fields) < 4:
        raise ValueError(f"CCPD 文件名不含四角点：{image_path}")
    raw_points = fields[3].split("_")
    if len(raw_points) != 4:
        raise ValueError(f"CCPD 四角点数量不是 4：{image_path}")
    points = [
        tuple(float(value) for value in raw_point.split("&"))
        for raw_point in raw_points
    ]
    return order_clockwise(points)


def write_obb_label(path: Path, labels: tuple[ObbLabel, ...]) -> None:
    lines = []
    for label in labels:
        coordinates = " ".join(
            f"{coordinate:.6f}"
            for point in label.points
            for coordinate in point
        )
        lines.append(f"{label.class_id} {coordinates}")
    content = "\n".join(lines)
    if content:
        content += "\n"
    path.write_text(content, encoding="utf-8", newline="\n")


# %% File helpers
def list_images_flat(root: Path) -> list[Path]:
    if not root.is_dir():
        raise FileNotFoundError(f"找不到图片目录：{root}")
    return [
        Path(entry.path)
        for entry in os.scandir(root)
        if entry.is_file()
        and Path(entry.name).suffix.lower() in IMAGE_SUFFIXES
    ]


def collect_images_by_stem(root: Path) -> dict[str, Path]:
    images: dict[str, Path] = {}
    for current_root, _, file_names in os.walk(root):
        for file_name in file_names:
            path = Path(current_root, file_name)
            if path.suffix.lower() not in IMAGE_SUFFIXES:
                continue
            if path.stem in images:
                raise ValueError(
                    f"图片主文件名重复：{images[path.stem]} / {path}"
                )
            images[path.stem] = path.resolve()
    return images


def collect_labels_by_stem(root: Path) -> dict[str, Path]:
    labels: dict[str, Path] = {}
    for current_root, _, file_names in os.walk(root):
        for file_name in file_names:
            path = Path(current_root, file_name)
            if path.suffix.lower() != ".txt" or path.name == "classes.txt":
                continue
            if path.stem in labels:
                raise ValueError(
                    f"标签主文件名重复：{labels[path.stem]} / {path}"
                )
            labels[path.stem] = path.resolve()
    return labels


def image_size(path: Path) -> tuple[int, int]:
    with Image.open(path) as image:
        width, height = image.size
    if width <= 0 or height <= 0:
        raise ValueError(f"非法图片尺寸：{path} -> {width}x{height}")
    return width, height


# %% CCPD collection
def build_ccpd_sample(
    image_path: Path,
    split: str,
    source_kind: str,
    partition: str,
    class_id: int,
) -> SourceSample:
    width, height = 720, 1160
    pixel_points = parse_ccpd_points(image_path)
    normalized_points = clip_and_normalize_points(
        pixel_points,
        width,
        height,
    )
    label = ObbLabel(class_id, normalized_points, str(class_id))
    return SourceSample(
        split=split,
        source_kind=source_kind,
        source_partition=partition,
        source_image=image_path,
        source_label=None,
        output_stem=f"{source_kind}_{partition}_{image_path.stem}",
        labels=(label,),
        original_types=(str(class_id),),
        short_side_640=projected_short_side_640(
            pixel_points,
            width,
            height,
        ),
        repeat_in_train=False,
        negative_reason="",
    )


def collect_ccpd2019_samples(
    root: Path,
    seed: int,
) -> list[SourceSample]:
    samples: list[SourceSample] = []
    for partition, train_quota in CCPD2019_TRAIN_QUOTAS.items():
        paths = list_images_flat(root / partition)
        selected = select_product_split(
            paths,
            train_quota=train_quota,
            seed=seed,
            namespace=f"ccpd2019:{partition}",
        )
        for split in SPLITS:
            samples.extend(
                build_ccpd_sample(
                    image_path,
                    split,
                    "ccpd2019",
                    partition,
                    class_id=0,
                )
                for image_path in selected[split]
            )

    all_negative_paths = list_images_flat(root / "ccpd_np")
    available_negative_stems = {
        image_path.stem
        for image_path in all_negative_paths
    }
    missing_invalid_stems = (
        CCPD2019_INVALID_NEGATIVE_STEMS - available_negative_stems
    )
    if missing_invalid_stems:
        missing_text = ", ".join(sorted(missing_invalid_stems))
        raise ValueError(f"未找到配置的 CCPD2019 异常负样本：{missing_text}")
    negative_paths = [
        image_path
        for image_path in all_negative_paths
        if image_path.stem not in CCPD2019_INVALID_NEGATIVE_STEMS
    ]
    negative_selected = select_product_split(
        negative_paths,
        train_quota=CCPD2019_NEGATIVE_TRAIN_QUOTA,
        seed=seed,
        namespace="ccpd2019:ccpd_np",
    )
    for split in SPLITS:
        for image_path in negative_selected[split]:
            samples.append(
                SourceSample(
                    split=split,
                    source_kind="ccpd2019",
                    source_partition="ccpd_np",
                    source_image=image_path,
                    source_label=None,
                    output_stem=f"ccpd2019_ccpd_np_{image_path.stem}",
                    labels=(),
                    original_types=(),
                    short_side_640=None,
                    repeat_in_train=False,
                    negative_reason="no_plate",
                )
            )
    return samples


def collect_ccpd2020_samples(root: Path) -> list[SourceSample]:
    samples: list[SourceSample] = []
    for split in SPLITS:
        for image_path in list_images_flat(root / split):
            samples.append(
                build_ccpd_sample(
                    image_path,
                    split,
                    "ccpd2020",
                    "ccpd_green",
                    class_id=1,
                )
            )
    return samples


# %% CRPD collection
def parse_crpd_source_sample(
    label_path: Path,
    image_path: Path,
    subset: str,
    split: str,
) -> SourceSample:
    width, height = image_size(image_path)
    labels: list[ObbLabel] = []
    original_types: list[str] = []
    short_sides: list[float] = []
    dropped_type2 = 0
    missing_content = 0
    repaired_degenerate = 0
    with label_path.open("r", encoding="utf-8-sig", errors="replace") as file:
        for line_number, raw_line in enumerate(file, start=1):
            fields = raw_line.strip().split()
            if not fields:
                continue
            if len(fields) < 9:
                raise ValueError(
                    f"{label_path}:{line_number} 应至少为 9 列，"
                    f"实际为 {len(fields)} 列"
                )
            if len(fields) == 9:
                missing_content += 1
            try:
                coordinates = tuple(float(value) for value in fields[:8])
            except ValueError as exc:
                raise ValueError(
                    f"{label_path}:{line_number} 四角点含非数值字段"
                ) from exc
            original_type = fields[8]
            if original_type not in {"0", "1", "2", "3"}:
                raise ValueError(
                    f"{label_path}:{line_number} 未知 CRPD type={original_type}"
                )
            original_types.append(original_type)
            if original_type == "2":
                dropped_type2 += 1
                continue
            raw_points = list(zip(coordinates[0::2], coordinates[1::2]))
            if len(set(raw_points)) == 4:
                pixel_points = order_clockwise(raw_points)
            else:
                try:
                    pixel_points = minimum_rectangle_points(raw_points)
                except ValueError as exc:
                    raise ValueError(
                        f"{label_path}:{line_number} 退化四角点无法修复："
                        f"{raw_points}"
                    ) from exc
                repaired_degenerate += 1
            labels.append(
                ObbLabel(
                    class_id=CRPD_CLASS_MAPPING[original_type],
                    points=clip_and_normalize_points(
                        pixel_points,
                        width,
                        height,
                    ),
                    original_type=original_type,
                )
            )
            short_sides.append(
                projected_short_side_640(
                    pixel_points,
                    width,
                    height,
                )
            )
    repeat_in_train = any(
        original_type in {"1", "3"}
        for original_type in original_types
    )
    other_in_train = "3" in original_types
    if not original_types:
        negative_reason = "raw_empty"
    elif not labels:
        negative_reason = "type2_filtered"
    else:
        negative_reason = ""
    return SourceSample(
        split=split,
        source_kind="crpd",
        source_partition=subset,
        source_image=image_path,
        source_label=label_path,
        output_stem=f"crpd_{subset}_{split}_{image_path.stem}",
        labels=tuple(labels),
        original_types=tuple(original_types),
        short_side_640=min(short_sides) if short_sides else None,
        repeat_in_train=repeat_in_train,
        negative_reason=negative_reason,
        other_in_train=other_in_train,
        dropped_type2_objects=dropped_type2,
        missing_content_objects=missing_content,
        repaired_degenerate_objects=repaired_degenerate,
    )


def collect_crpd_partition(
    root: Path,
    subset: str,
    split: str,
    workers: int,
) -> list[SourceSample]:
    label_root = root / subset / split / "labels"
    image_root = root / subset / split / "images"
    label_paths = sorted(label_root.glob("*.txt"))

    def parse(label_path: Path) -> SourceSample:
        image_path = image_root / f"{label_path.stem}.jpg"
        if not image_path.is_file():
            raise FileNotFoundError(
                f"CRPD 标签缺少同名图片：{label_path} -> {image_path}"
            )
        return parse_crpd_source_sample(
            label_path,
            image_path,
            subset,
            split,
        )

    with ThreadPoolExecutor(max_workers=workers) as executor:
        return list(executor.map(parse, label_paths))


def select_remote_blue_single_samples(
    samples: list[SourceSample],
    target: int,
    seed: int,
) -> list[SourceSample]:
    if target < 0:
        raise ValueError(f"CRPD_single 蓝牌补充配额不能为负：{target}")
    bins: dict[str, list[SourceSample]] = {
        "lt12": [],
        "12to16": [],
        "ge16": [],
    }
    for sample in samples:
        if sample.short_side_640 is None:
            continue
        if sample.short_side_640 < 12:
            bins["lt12"].append(sample)
        elif sample.short_side_640 < 16:
            bins["12to16"].append(sample)
        else:
            bins["ge16"].append(sample)

    quotas = {
        "lt12": round(target * 0.50),
        "12to16": round(target * 0.30),
    }
    quotas["ge16"] = target - quotas["lt12"] - quotas["12to16"]
    selected: list[SourceSample] = []
    selected_paths: set[Path] = set()
    for bin_name in ("lt12", "12to16", "ge16"):
        ordered = sorted(
            bins[bin_name],
            key=lambda sample: stable_digest(
                seed,
                f"crpd_single:{bin_name}",
                sample.source_image.as_posix(),
            ),
        )
        quota = quotas[bin_name]
        for sample in ordered[:quota]:
            selected.append(sample)
            selected_paths.add(sample.source_image)

    if len(selected) < target:
        remaining = sorted(
            (
                sample
                for sample in samples
                if sample.source_image not in selected_paths
            ),
            key=lambda sample: stable_digest(
                seed,
                "crpd_single:remainder",
                sample.source_image.as_posix(),
            ),
        )
        selected.extend(remaining[: target - len(selected)])
    if len(selected) != target:
        raise ValueError(
            f"CRPD_single 蓝牌候选不足：需要 {target}，实际 {len(selected)}"
        )
    return selected


def collect_crpd_samples(
    root: Path,
    seed: int,
    workers: int,
) -> list[SourceSample]:
    by_partition: dict[tuple[str, str], list[SourceSample]] = {}
    for subset in ("CRPD_single", "CRPD_double", "CRPD_multi"):
        for split in SPLITS:
            by_partition[(subset, split)] = collect_crpd_partition(
                root,
                subset,
                split,
                workers,
            )

    selected: list[SourceSample] = []
    for subset in ("CRPD_double", "CRPD_multi"):
        for split in SPLITS:
            selected.extend(by_partition[(subset, split)])

    single_train = [
        sample
        for sample in by_partition[("CRPD_single", "train")]
        if sample.labels
    ]
    special_single_train = [
        sample
        for sample in single_train
        if sample.repeat_in_train
    ]
    blue_single_train = [
        sample
        for sample in single_train
        if not sample.repeat_in_train
        and set(sample.original_types) == {"0"}
    ]
    blue_target = CRPD_SINGLE_TRAIN_TARGET - len(special_single_train)
    selected.extend(special_single_train)
    selected.extend(
        select_remote_blue_single_samples(
            blue_single_train,
            target=blue_target,
            seed=seed,
        )
    )
    for split in ("val", "test"):
        selected.extend(
            sample
            for sample in by_partition[("CRPD_single", split)]
            if sample.labels
        )
    return selected


# %% Existing special-plate collection
def box_iou(first: tuple[float, float, float, float], second: tuple[float, float, float, float]) -> float:
    first_x, first_y, first_w, first_h = first
    second_x, second_y, second_w, second_h = second
    first_x1 = first_x - first_w / 2
    first_y1 = first_y - first_h / 2
    first_x2 = first_x + first_w / 2
    first_y2 = first_y + first_h / 2
    second_x1 = second_x - second_w / 2
    second_y1 = second_y - second_h / 2
    second_x2 = second_x + second_w / 2
    second_y2 = second_y + second_h / 2
    intersection_width = max(
        0.0,
        min(first_x2, second_x2) - max(first_x1, second_x1),
    )
    intersection_height = max(
        0.0,
        min(first_y2, second_y2) - max(first_y1, second_y1),
    )
    intersection = intersection_width * intersection_height
    union = first_w * first_h + second_w * second_h - intersection
    return intersection / union if union > 0 else 0.0


def merge_duplicate_hbbs(
    boxes: list[tuple[float, float, float, float]],
) -> list[tuple[float, float, float, float]]:
    merged: list[tuple[float, float, float, float]] = []
    counts: list[int] = []
    for box in boxes:
        duplicate_index = next(
            (
                index
                for index, previous in enumerate(merged)
                if box_iou(previous, box) >= 0.95
            ),
            None,
        )
        if duplicate_index is None:
            merged.append(box)
            counts.append(1)
            continue
        previous = merged[duplicate_index]
        count = counts[duplicate_index]
        merged[duplicate_index] = tuple(
            (previous[index] * count + box[index]) / (count + 1)
            for index in range(4)
        )
        counts[duplicate_index] = count + 1
    return merged


def parse_special_labels(label_path: Path) -> tuple[ObbLabel, ...]:
    boxes: list[tuple[float, float, float, float]] = []
    with label_path.open("r", encoding="utf-8-sig") as file:
        for line_number, raw_line in enumerate(file, start=1):
            fields = raw_line.strip().split()
            if not fields:
                continue
            if len(fields) != 5:
                raise ValueError(
                    f"{label_path}:{line_number} 应为 5 列，"
                    f"实际为 {len(fields)} 列"
                )
            class_id = int(fields[0])
            if class_id != 0:
                raise ValueError(
                    f"{label_path}:{line_number} 局部类别应为 0，"
                    f"实际为 {class_id}"
                )
            x_center, y_center, width, height = (
                float(value)
                for value in fields[1:]
            )
            x1 = max(0.0, x_center - width / 2)
            y1 = max(0.0, y_center - height / 2)
            x2 = min(1.0, x_center + width / 2)
            y2 = min(1.0, y_center + height / 2)
            if x2 <= x1 or y2 <= y1:
                raise ValueError(
                    f"{label_path}:{line_number} 裁剪后框面积为 0"
                )
            boxes.append(
                (
                    (x1 + x2) / 2,
                    (y1 + y2) / 2,
                    x2 - x1,
                    y2 - y1,
                )
            )
    merged = merge_duplicate_hbbs(boxes)
    if not merged:
        raise ValueError(f"特殊车牌标签无有效框：{label_path}")
    labels = []
    for x_center, y_center, width, height in merged:
        x1 = x_center - width / 2
        y1 = y_center - height / 2
        x2 = x_center + width / 2
        y2 = y_center + height / 2
        labels.append(
            ObbLabel(
                class_id=3,
                points=order_clockwise(
                    ((x1, y1), (x2, y1), (x2, y2), (x1, y2))
                ),
                original_type="special",
            )
        )
    return tuple(labels)


def deterministic_special_split(
    stems: list[str],
    seed: int,
    group: str,
) -> dict[str, list[str]]:
    shuffled = sorted(stems)
    random.Random(f"{seed}:{group}").shuffle(shuffled)
    baseline_validation_count = round(
        len(shuffled) * SPECIAL_BASELINE_VAL_RATIO
    )
    test_count = round(len(shuffled) * SPECIAL_SPLIT_RATIOS["test"])
    validation_count = round(
        len(shuffled) * SPECIAL_SPLIT_RATIOS["val"]
    )
    additional_validation_count = (
        validation_count - baseline_validation_count
    )
    test_start = baseline_validation_count
    train_start = test_start + test_count
    additional_validation_end = (
        train_start + additional_validation_count
    )
    return {
        "val": (
            shuffled[:baseline_validation_count]
            + shuffled[train_start:additional_validation_end]
        ),
        "test": shuffled[test_start:train_start],
        "train": shuffled[additional_validation_end:],
    }


def collect_special_samples(
    root: Path,
    seed: int,
) -> list[SourceSample]:
    samples: list[SourceSample] = []
    for image_group, (label_group, english_group) in SPECIAL_GROUPS.items():
        images = collect_images_by_stem(root / "image" / image_group)
        labels = collect_labels_by_stem(root / "txt" / label_group)
        if set(images) != set(labels):
            missing_labels = sorted(set(images) - set(labels))
            missing_images = sorted(set(labels) - set(images))
            raise ValueError(
                f"特殊车牌 {image_group} 图像标签不配对；"
                f"缺标签={missing_labels[:5]}，缺图片={missing_images[:5]}"
            )
        split_stems = deterministic_special_split(
            list(images),
            seed,
            image_group,
        )
        for split in SPLITS:
            for stem in split_stems[split]:
                width, height = image_size(images[stem])
                obb_labels = parse_special_labels(labels[stem])
                short_sides = [
                    minimum_rectangle_short_side(
                        (
                            (x * width, y * height)
                            for x, y in label.points
                        )
                    )
                    * 640
                    / max(width, height)
                    for label in obb_labels
                ]
                samples.append(
                    SourceSample(
                        split=split,
                        source_kind="special",
                        source_partition=image_group,
                        source_image=images[stem],
                        source_label=labels[stem],
                        output_stem=f"special_{english_group}_{stem}",
                        labels=obb_labels,
                        original_types=("special",),
                        short_side_640=min(short_sides),
                        repeat_in_train=True,
                        negative_reason="",
                        other_in_train=True,
                    )
                )
    return samples


# %% Build item expansion and summary
def expand_build_items(samples: list[SourceSample]) -> list[BuildItem]:
    items: list[BuildItem] = []
    output_stems: set[tuple[str, str]] = set()
    for sample in samples:
        repeats = 1
        if sample.split == "train":
            if sample.other_in_train:
                repeats = TRAIN_OTHER_REPEATS
            elif sample.repeat_in_train:
                repeats = TRAIN_YELLOW_SINGLE_REPEATS
        for repeat_index in range(repeats):
            suffix = (
                f"__r{repeat_index + 1:02d}"
                if repeats > 1
                else ""
            )
            output_stem = f"{sample.output_stem}{suffix}"
            key = (sample.split, output_stem)
            if key in output_stems:
                raise ValueError(f"输出文件名重复：{key}")
            output_stems.add(key)
            items.append(
                BuildItem(
                    sample=sample,
                    repeat_index=repeat_index,
                    output_stem=output_stem,
                )
            )
    return sorted(
        items,
        key=lambda item: (
            SPLITS.index(item.sample.split),
            item.output_stem,
        ),
    )


def select_balanced_validation_items(
    items: list[BuildItem],
    seed: int,
) -> list[BuildItem]:
    validation_items = [
        item
        for item in items
        if item.sample.split == "val"
    ]
    selected: dict[str, BuildItem] = {}

    def add(item: BuildItem) -> None:
        selected[item.output_stem] = item

    for item in validation_items:
        class_ids = {
            label.class_id
            for label in item.sample.labels
        }
        if 3 in class_ids or not class_ids:
            add(item)

    for class_id in (1, 2, 0):
        target = BALANCED_VAL_TARGETS[class_id]
        candidates = sorted(
            (
                item
                for item in validation_items
                if item.output_stem not in selected
                and any(
                    label.class_id == class_id
                    for label in item.sample.labels
                )
            ),
            key=lambda item: stable_digest(
                seed,
                f"balanced_val:{class_id}",
                item.sample.source_image.as_posix(),
            ),
        )
        current = sum(
            label.class_id == class_id
            for item in selected.values()
            for label in item.sample.labels
        )
        for item in candidates:
            if current >= target:
                break
            add(item)
            current += sum(
                label.class_id == class_id
                for label in item.sample.labels
            )
        if current < target:
            raise ValueError(
                f"平衡验证集 class={class_id} 目标 {target}，"
                f"实际仅 {current}"
            )
    return sorted(
        selected.values(),
        key=lambda item: item.output_stem,
    )


def create_summary(items: list[BuildItem], seed: int) -> dict[str, object]:
    by_split: dict[str, object] = {}
    for split in SPLITS:
        split_items = [
            item
            for item in items
            if item.sample.split == split
        ]
        class_counts = Counter(
            label.class_id
            for item in split_items
            for label in item.sample.labels
        )
        source_counts = Counter(
            item.sample.source_kind
            for item in split_items
        )
        partition_counts = Counter(
            f"{item.sample.source_kind}/{item.sample.source_partition}"
            for item in split_items
        )
        unique_sources = {
            item.sample.source_image
            for item in split_items
        }
        by_split[split] = {
            "images_with_repeats": len(split_items),
            "unique_source_images": len(unique_sources),
            "boxes_with_repeats": sum(
                len(item.sample.labels)
                for item in split_items
            ),
            "negative_images": sum(
                not item.sample.labels
                for item in split_items
            ),
            "special_repeat_copies": sum(
                item.repeat_index > 0
                for item in split_items
            ),
            "dropped_crpd_type2_objects": sum(
                item.sample.dropped_type2_objects
                for item in split_items
                if item.repeat_index == 0
            ),
            "missing_crpd_content_objects": sum(
                item.sample.missing_content_objects
                for item in split_items
                if item.repeat_index == 0
            ),
            "repaired_crpd_degenerate_objects": sum(
                item.sample.repaired_degenerate_objects
                for item in split_items
                if item.repeat_index == 0
            ),
            "class_counts": {
                CLASS_NAMES[class_id]: class_counts[class_id]
                for class_id in CLASS_NAMES
            },
            "source_counts": dict(sorted(source_counts.items())),
            "partition_counts": dict(sorted(partition_counts.items())),
        }
    balanced_validation = select_balanced_validation_items(items, seed)
    balanced_class_counts = Counter(
        label.class_id
        for item in balanced_validation
        for label in item.sample.labels
    )
    return {
        "seed": seed,
        "train_yellow_single_total_repeats": TRAIN_YELLOW_SINGLE_REPEATS,
        "train_other_total_repeats": TRAIN_OTHER_REPEATS,
        "ccpd2019_train_quotas": CCPD2019_TRAIN_QUOTAS,
        "ccpd2019_negative_train_quota": CCPD2019_NEGATIVE_TRAIN_QUOTA,
        "excluded_invalid_ccpd2019_negative_images": [
            f"ccpd2019/ccpd_np/{stem}.jpg"
            for stem in sorted(CCPD2019_INVALID_NEGATIVE_STEMS)
        ],
        "crpd_single_train_target": CRPD_SINGLE_TRAIN_TARGET,
        "crpd_type2_policy": (
            "drop type=2 objects; exclude empty CRPD_single images; "
            "retain CRPD_multi/CRPD_double except known bad source images"
        ),
        "excluded_known_bad_source_images": [
            "/".join(key)
            for key in sorted(KNOWN_BAD_SOURCE_IMAGES)
        ],
        "balanced_validation": {
            "list_file": "val_balanced.txt",
            "images": len(balanced_validation),
            "boxes": sum(
                len(item.sample.labels)
                for item in balanced_validation
            ),
            "negative_images": sum(
                not item.sample.labels
                for item in balanced_validation
            ),
            "class_counts": {
                CLASS_NAMES[class_id]: balanced_class_counts[class_id]
                for class_id in CLASS_NAMES
            },
        },
        "by_split": by_split,
    }


def print_summary(summary: dict[str, object]) -> None:
    print(
        "CRPD type=2：删除目标；single 删除空图；"
        "multi/double 保留图片，但排除已确认的异常源图。"
    )
    for split in SPLITS:
        stats = summary["by_split"][split]
        class_counts = stats["class_counts"]
        print(
            f"{split}: 图片 {stats['images_with_repeats']:,}，"
            f"唯一源图 {stats['unique_source_images']:,}，"
            f"框 {stats['boxes_with_repeats']:,}，"
            f"负样本 {stats['negative_images']:,}，"
            f"特殊车牌额外副本 {stats['special_repeat_copies']:,}；"
            f"blue={class_counts['blue']:,}, "
            f"green={class_counts['green']:,}, "
            f"yellow_single={class_counts['yellow_single']:,}, "
            f"other={class_counts['other']:,}"
        )
        print(
            "  来源："
            + ", ".join(
                f"{name}={count:,}"
                for name, count in stats["source_counts"].items()
            )
        )
        print(
            "  子集："
            + ", ".join(
                f"{name}={count:,}"
                for name, count in stats["partition_counts"].items()
            )
        )
        print(
            f"  CRPD审计：删除type=2目标 "
            f"{stats['dropped_crpd_type2_objects']:,}，"
            f"content缺失目标 {stats['missing_crpd_content_objects']:,}，"
            f"退化四点修复 {stats['repaired_crpd_degenerate_objects']:,}"
        )
    balanced = summary["balanced_validation"]
    balanced_classes = balanced["class_counts"]
    print(
        f"平衡验证清单：图片 {balanced['images']:,}，"
        f"框 {balanced['boxes']:,}，"
        f"负样本 {balanced['negative_images']:,}；"
        f"blue={balanced_classes['blue']:,}, "
        f"green={balanced_classes['green']:,}, "
        f"yellow_single={balanced_classes['yellow_single']:,}, "
        f"other={balanced_classes['other']:,}"
    )


# %% Materialization and verification
def hardlink_image(source: Path, destination: Path) -> None:
    try:
        os.link(source, destination)
    except OSError as exc:
        raise OSError(
            "创建图片硬链接失败，源与目标必须位于同一支持硬链接的卷："
            f"{source} -> {destination}"
        ) from exc


def materialize_item(item: BuildItem, stage_root: Path) -> WrittenItem:
    output_image = (
        stage_root
        / "images"
        / item.sample.split
        / f"{item.output_stem}{item.sample.source_image.suffix.lower()}"
    )
    output_label = (
        stage_root
        / "labels"
        / item.sample.split
        / f"{item.output_stem}.txt"
    )
    hardlink_image(item.sample.source_image, output_image)
    write_obb_label(output_label, item.sample.labels)
    return WrittenItem(item, output_image, output_label)


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
                "original_types",
                "short_side_640",
                "negative_reason",
                "dropped_type2_objects",
                "missing_content_objects",
                "repaired_degenerate_objects",
            )
        )
        for written in items:
            sample = written.item.sample
            writer.writerow(
                (
                    sample.split,
                    sample.source_kind,
                    sample.source_partition,
                    sample.source_image,
                    sample.source_label or "",
                    written.output_image.relative_to(stage_root),
                    written.item.repeat_index,
                    len(sample.labels),
                    ",".join(
                        str(label.class_id)
                        for label in sample.labels
                    ),
                    ",".join(sample.original_types),
                    (
                        f"{sample.short_side_640:.6f}"
                        if sample.short_side_640 is not None
                        else ""
                    ),
                    sample.negative_reason,
                    sample.dropped_type2_objects,
                    sample.missing_content_objects,
                    sample.repaired_degenerate_objects,
                )
            )


def balanced_validation_lines(
    items: list[WrittenItem],
    stage_root: Path,
    seed: int,
) -> list[str]:
    selected = select_balanced_validation_items(
        [written.item for written in items],
        seed,
    )
    written_by_stem = {
        written.item.output_stem: written
        for written in items
        if written.item.sample.split == "val"
    }
    return [
        "./" + written_by_stem[item.output_stem].output_image.relative_to(
            stage_root
        ).as_posix()
        for item in selected
    ]


def write_balanced_validation_list(
    items: list[WrittenItem],
    stage_root: Path,
    seed: int,
) -> None:
    lines = balanced_validation_lines(items, stage_root, seed)
    (stage_root / "val_balanced.txt").write_text(
        "\n".join(lines) + "\n",
        encoding="utf-8",
        newline="\n",
    )


def validate_label_file(path: Path) -> tuple[int, Counter[int]]:
    box_count = 0
    class_counts: Counter[int] = Counter()
    with path.open("r", encoding="utf-8") as file:
        for line_number, raw_line in enumerate(file, start=1):
            fields = raw_line.strip().split()
            if not fields:
                continue
            if len(fields) != 9:
                raise ValueError(
                    f"{path}:{line_number} OBB 标签应为 9 列，"
                    f"实际为 {len(fields)}"
                )
            class_id = int(fields[0])
            if class_id not in CLASS_NAMES:
                raise ValueError(
                    f"{path}:{line_number} 非法类别 {class_id}"
                )
            values = tuple(float(value) for value in fields[1:])
            if not all(math.isfinite(value) for value in values):
                raise ValueError(
                    f"{path}:{line_number} 含 NaN 或 Inf"
                )
            if min(values) < 0 or max(values) > 1:
                raise ValueError(
                    f"{path}:{line_number} 坐标越出 [0, 1]"
                )
            points = tuple(zip(values[0::2], values[1::2]))
            if polygon_area(points) <= 1e-9:
                raise ValueError(
                    f"{path}:{line_number} 四边形面积为 0"
                )
            box_count += 1
            class_counts[class_id] += 1
    return box_count, class_counts


def verify_output(
    items: list[WrittenItem],
    stage_root: Path,
    workers: int,
    seed: int,
) -> None:
    def verify_written(
        written: WrittenItem,
    ) -> tuple[Path, str]:
        sample = written.item.sample
        if not written.output_image.is_file():
            raise FileNotFoundError(f"输出图片缺失：{written.output_image}")
        if not written.output_label.is_file():
            raise FileNotFoundError(f"输出标签缺失：{written.output_label}")
        if not os.path.samefile(sample.source_image, written.output_image):
            raise OSError(
                f"输出图片不是源图硬链接："
                f"{sample.source_image} -> {written.output_image}"
            )
        box_count, _ = validate_label_file(written.output_label)
        if box_count != len(sample.labels):
            raise ValueError(
                f"输出标签框数 {box_count} != 预期 {len(sample.labels)}："
                f"{written.output_label}"
            )
        return sample.source_image, sample.split

    source_splits: dict[Path, set[str]] = defaultdict(set)
    with ThreadPoolExecutor(max_workers=workers) as executor:
        for source_image, split in executor.map(verify_written, items):
            source_splits[source_image].add(split)

    leaked = {
        str(path): sorted(splits)
        for path, splits in source_splits.items()
        if len(splits) > 1
    }
    if leaked:
        first_path, first_splits = next(iter(leaked.items()))
        raise ValueError(
            f"同一源图跨划分：{first_path} -> {first_splits}"
        )

    expected_counts = Counter(
        written.item.sample.split
        for written in items
    )
    for split in SPLITS:
        output_images = list_images_flat(stage_root / "images" / split)
        output_labels = list(
            (stage_root / "labels" / split).glob("*.txt")
        )
        if len(output_images) != expected_counts[split]:
            raise ValueError(
                f"{split} 输出图片 {len(output_images)} != "
                f"预期 {expected_counts[split]}"
            )
        if len(output_labels) != expected_counts[split]:
            raise ValueError(
                f"{split} 输出标签 {len(output_labels)} != "
                f"预期 {expected_counts[split]}"
            )
    expected_validation_lines = balanced_validation_lines(
        items,
        stage_root,
        seed,
    )
    validation_list_path = stage_root / "val_balanced.txt"
    if not validation_list_path.is_file():
        raise FileNotFoundError(f"平衡验证清单缺失：{validation_list_path}")
    actual_validation_lines = validation_list_path.read_text(
        encoding="utf-8"
    ).splitlines()
    if actual_validation_lines != expected_validation_lines:
        raise ValueError("平衡验证清单与确定性选择结果不一致")
    if len(actual_validation_lines) != len(set(actual_validation_lines)):
        raise ValueError("平衡验证清单含重复图片")
    for relative_path in actual_validation_lines:
        if not relative_path.startswith("./images/val/"):
            raise ValueError(f"平衡验证清单含非 val 路径：{relative_path}")
        if not (stage_root / relative_path[2:]).is_file():
            raise FileNotFoundError(
                f"平衡验证图片不存在：{relative_path}"
            )


# %% Main
def validate_source_roots(args: argparse.Namespace) -> None:
    for name in (
        "ccpd2019_root",
        "ccpd2020_root",
        "crpd_root",
        "special_root",
    ):
        path = getattr(args, name).resolve()
        if not path.is_dir():
            raise FileNotFoundError(f"{name} 不存在：{path}")
    if args.workers < 1:
        raise ValueError("--workers 必须 >= 1")


def exclude_known_bad_samples(
    samples: list[SourceSample],
) -> list[SourceSample]:
    kept: list[SourceSample] = []
    excluded: set[tuple[str, str, str, str]] = set()
    for sample in samples:
        key = (
            sample.source_kind,
            sample.source_partition,
            sample.split,
            sample.source_image.stem,
        )
        if key in KNOWN_BAD_SOURCE_IMAGES:
            excluded.add(key)
            continue
        kept.append(sample)
    missing = KNOWN_BAD_SOURCE_IMAGES - excluded
    if missing:
        missing_text = ", ".join("/".join(key) for key in sorted(missing))
        raise ValueError(f"未找到配置的异常源图：{missing_text}")
    return kept


def collect_all_samples(args: argparse.Namespace) -> list[SourceSample]:
    ccpd2019_root = args.ccpd2019_root.resolve()
    ccpd2020_root = args.ccpd2020_root.resolve()
    crpd_root = args.crpd_root.resolve()
    special_root = args.special_root.resolve()
    samples = collect_ccpd2019_samples(ccpd2019_root, args.seed)
    samples.extend(collect_ccpd2020_samples(ccpd2020_root))
    samples.extend(
        collect_crpd_samples(
            crpd_root,
            seed=args.seed,
            workers=args.workers,
        )
    )
    samples.extend(collect_special_samples(special_root, args.seed))
    return exclude_known_bad_samples(samples)


def main() -> int:
    args = parse_args()
    validate_source_roots(args)
    output_root = args.output_root.resolve()
    if output_root.exists():
        raise FileExistsError(f"输出目录已存在，拒绝覆盖：{output_root}")

    print("正在收集并解析原始四点标注...")
    samples = collect_all_samples(args)
    items = expand_build_items(samples)
    summary = create_summary(items, args.seed)
    print_summary(summary)

    if args.dry_run:
        print("Dry-run 完成：未创建数据集。")
        return 0

    stage_root = output_root.with_name(f".{output_root.name}.tmp")
    if stage_root.exists():
        raise FileExistsError(f"临时目录已存在：{stage_root}")
    for split in SPLITS:
        (stage_root / "images" / split).mkdir(parents=True)
        (stage_root / "labels" / split).mkdir(parents=True)

    written_items: list[WrittenItem] = []
    try:
        for index, item in enumerate(items, start=1):
            written_items.append(materialize_item(item, stage_root))
            if index % 2_000 == 0 or index == len(items):
                print(f"已构建 {index:,}/{len(items):,}")
        write_manifest(written_items, stage_root)
        write_balanced_validation_list(
            written_items,
            stage_root,
            args.seed,
        )
        (stage_root / "build_summary.json").write_text(
            json.dumps(summary, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        verify_output(
            written_items,
            stage_root,
            args.workers,
            args.seed,
        )
        stage_root.rename(output_root)
    except Exception:
        print(f"构建失败，临时目录保留用于排查：{stage_root}")
        raise

    print(f"OBB 数据集构建完成：{output_root}")
    print("全部派生图片均已验证为源图的 NTFS 硬链接。")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
