from __future__ import annotations

import csv
import hashlib
import json
import random
from collections import Counter
from dataclasses import dataclass
from pathlib import Path

import cv2
import numpy as np


SEED = 20260726
IMAGE_SIZE = 640
SAMPLES_PER_CORE_CLASS = 600
CORE_CLASSES = ((0, "blue"), (1, "green"), (2, "yellow_single"))

SCRIPT_DIR = Path(__file__).resolve().parent
WORKSPACE_ROOT = SCRIPT_DIR.parents[2]
MODEL_DIR = SCRIPT_DIR.parent / "model"
SOURCE_ROOT = (
    WORKSPACE_ROOT / "1_PC_Training" / "datasets" / "yolo_finetune_special"
)
SOURCE_MANIFEST = SOURCE_ROOT / "dataset_manifest.tsv"
OUTPUT_DIR = MODEL_DIR / "finetune_quant_dataset"
DATASET_PATH = MODEL_DIR / "finetune_quant_dataset.txt"
OUTPUT_MANIFEST = MODEL_DIR / "finetune_quant_manifest.tsv"
SUMMARY_PATH = MODEL_DIR / "finetune_quant_summary.json"


@dataclass(frozen=True)
class SourceImage:
    path: Path
    split: str
    source_kind: str
    source_partition: str
    class_ids: tuple[int, ...]


def load_unique_sources() -> list[SourceImage]:
    if not SOURCE_MANIFEST.is_file():
        raise FileNotFoundError(f"找不到数据集 manifest：{SOURCE_MANIFEST}")

    unique: dict[str, SourceImage] = {}
    with SOURCE_MANIFEST.open("r", encoding="utf-8", newline="") as file:
        for row in csv.DictReader(file, delimiter="\t"):
            source = Path(row["source_image"]).resolve()
            key = str(source).casefold()
            item = SourceImage(
                path=source,
                split=row["split"],
                source_kind=row["source_kind"],
                source_partition=row["source_partition"],
                class_ids=tuple(
                    sorted(int(value) for value in row["class_ids"].split(","))
                ),
            )
            previous = unique.get(key)
            if previous is not None and previous != item:
                raise ValueError(f"同一源图的 manifest 信息不一致：{source}")
            unique[key] = item

    missing = [item.path for item in unique.values() if not item.path.is_file()]
    if missing:
        raise FileNotFoundError(f"存在 {len(missing)} 张缺失源图，首张：{missing[0]}")
    return sorted(unique.values(), key=lambda item: str(item.path).casefold())


def select_sources(sources: list[SourceImage]) -> list[tuple[str, SourceImage]]:
    selected: list[tuple[str, SourceImage]] = []
    selected_paths: set[str] = set()

    other = [item for item in sources if 3 in item.class_ids]
    if not other:
        raise ValueError("没有找到 other 类源图")
    for item in other:
        selected.append(("other_all", item))
        selected_paths.add(str(item.path).casefold())

    for class_id, class_name in CORE_CLASSES:
        candidates = [
            item
            for item in sources
            if item.split == "train"
            and class_id in item.class_ids
            and str(item.path).casefold() not in selected_paths
        ]
        random.Random(f"{SEED}:{class_name}").shuffle(candidates)
        if len(candidates) < SAMPLES_PER_CORE_CLASS:
            raise ValueError(
                f"{class_name} 可用 train 唯一源图不足："
                f"{len(candidates)} < {SAMPLES_PER_CORE_CLASS}"
            )
        for item in candidates[:SAMPLES_PER_CORE_CLASS]:
            selected.append((class_name, item))
            selected_paths.add(str(item.path).casefold())

    random.Random(SEED).shuffle(selected)
    return selected


def letterbox_bgr(image):
    height, width = image.shape[:2]
    scale = min(IMAGE_SIZE / height, IMAGE_SIZE / width)
    resized_width = int(round(width * scale))
    resized_height = int(round(height * scale))
    resized = cv2.resize(
        image,
        (resized_width, resized_height),
        interpolation=cv2.INTER_LINEAR,
    )
    dw = (IMAGE_SIZE - resized_width) / 2
    dh = (IMAGE_SIZE - resized_height) / 2
    left, right = int(round(dw - 0.1)), int(round(dw + 0.1))
    top, bottom = int(round(dh - 0.1)), int(round(dh + 0.1))
    letterboxed = cv2.copyMakeBorder(
        resized,
        top,
        bottom,
        left,
        right,
        cv2.BORDER_CONSTANT,
        value=(0, 0, 0),
    )
    return letterboxed, (
        width,
        height,
        scale,
        left,
        top,
    )


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for block in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest().upper()


def read_image(path: Path):
    encoded = np.fromfile(path, dtype=np.uint8)
    image = cv2.imdecode(encoded, cv2.IMREAD_COLOR)
    if image is None:
        raise ValueError(f"图片解码失败：{path}")
    return image


def ensure_outputs_absent(stage_dir: Path, temporary_files: list[Path]) -> None:
    conflicts = [
        path
        for path in [OUTPUT_DIR, DATASET_PATH, OUTPUT_MANIFEST, SUMMARY_PATH]
        if path.exists()
    ]
    conflicts.extend(path for path in [stage_dir, *temporary_files] if path.exists())
    if conflicts:
        raise FileExistsError(
            "拒绝覆盖已有输出：" + "；".join(str(path) for path in conflicts)
        )


def main() -> None:
    sources = load_unique_sources()
    selected = select_sources(sources)
    expected_total = SAMPLES_PER_CORE_CLASS * len(CORE_CLASSES) + sum(
        3 in item.class_ids for item in sources
    )
    if len(selected) != expected_total:
        raise AssertionError(f"选择数量异常：{len(selected)} != {expected_total}")

    stage_dir = MODEL_DIR / ".finetune_quant_dataset.tmp"
    temporary_files = [
        path.with_name(f".{path.name}.tmp")
        for path in (DATASET_PATH, OUTPUT_MANIFEST, SUMMARY_PATH)
    ]
    ensure_outputs_absent(stage_dir, temporary_files)
    stage_dir.mkdir(parents=False)

    dataset_tmp, manifest_tmp, summary_tmp = temporary_files
    dataset_lines: list[str] = []
    manifest_rows: list[dict[str, object]] = []
    bucket_counts: Counter[str] = Counter()
    class_presence: Counter[int] = Counter()
    other_source_kind: Counter[str] = Counter()
    other_split: Counter[str] = Counter()
    other_partition: Counter[str] = Counter()

    try:
        for index, (bucket, item) in enumerate(selected):
            image = read_image(item.path)
            prepared, info = letterbox_bgr(image)
            output_name = f"quant_{index:04d}.jpg"
            output_path = stage_dir / output_name
            if not cv2.imwrite(str(output_path), prepared):
                raise OSError(f"量化图片写入失败：{output_path}")
            if prepared.shape != (IMAGE_SIZE, IMAGE_SIZE, 3):
                raise AssertionError(f"量化图片尺寸异常：{output_path}")

            width, height, scale, pad_left, pad_top = info
            relative_output = f"finetune_quant_dataset/{output_name}"
            dataset_lines.append(f"./{relative_output}")
            manifest_rows.append(
                {
                    "index": index,
                    "selection_bucket": bucket,
                    "source_kind": item.source_kind,
                    "source_split": item.split,
                    "source_partition": item.source_partition,
                    "class_ids": ",".join(map(str, item.class_ids)),
                    "source_image": str(item.path),
                    "output_image": relative_output,
                    "source_sha256": sha256(item.path),
                    "source_width": width,
                    "source_height": height,
                    "scale": f"{scale:.12g}",
                    "pad_left": pad_left,
                    "pad_top": pad_top,
                }
            )
            bucket_counts[bucket] += 1
            class_presence.update(item.class_ids)
            if 3 in item.class_ids:
                other_source_kind[item.source_kind] += 1
                other_split[item.split] += 1
                other_partition[item.source_partition] += 1

        dataset_tmp.write_text(
            "\n".join(dataset_lines) + "\n",
            encoding="utf-8",
            newline="\n",
        )
        with manifest_tmp.open("w", encoding="utf-8", newline="") as file:
            writer = csv.DictWriter(
                file,
                fieldnames=list(manifest_rows[0]),
                delimiter="\t",
                lineterminator="\n",
            )
            writer.writeheader()
            writer.writerows(manifest_rows)

        summary = {
            "status": "VALID",
            "seed": SEED,
            "image_size": IMAGE_SIZE,
            "pad_color": [0, 0, 0],
            "channel_preparation": (
                "normal-color JPEG: cv2 BGR letterbox written directly by cv2.imwrite"
            ),
            "total_images": len(selected),
            "selection_bucket_counts": dict(sorted(bucket_counts.items())),
            "class_presence_counts": {
                str(key): value for key, value in sorted(class_presence.items())
            },
            "other_unique_images": sum(3 in item.class_ids for item in sources),
            "other_source_kind_counts": dict(sorted(other_source_kind.items())),
            "other_split_counts": dict(sorted(other_split.items())),
            "other_partition_counts": dict(sorted(other_partition.items())),
            "source_manifest": str(SOURCE_MANIFEST),
        }
        summary_tmp.write_text(
            json.dumps(summary, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
            newline="\n",
        )

        stage_dir.rename(OUTPUT_DIR)
        dataset_tmp.rename(DATASET_PATH)
        manifest_tmp.rename(OUTPUT_MANIFEST)
        summary_tmp.rename(SUMMARY_PATH)
    except Exception:
        print(f"生成未完成，临时目录保留用于检查：{stage_dir}")
        raise

    print(f"量化图片：{OUTPUT_DIR}")
    print(f"RKNN dataset：{DATASET_PATH}")
    print(f"明细 manifest：{OUTPUT_MANIFEST}")
    print(f"统计：{SUMMARY_PATH}")
    print(json.dumps(summary, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
