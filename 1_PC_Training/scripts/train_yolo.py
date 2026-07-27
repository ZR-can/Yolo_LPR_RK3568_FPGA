#!/usr/bin/env python3
"""从已有最佳权重开始新的 YOLO 微调，或显式恢复中断的微调任务。"""

from __future__ import annotations

# %% Imports and defaults
import argparse
import hashlib
import json
import time
from pathlib import Path
from typing import Any

from ultralytics import YOLO
from ultralytics.utils import YAML


SCRIPT_DIR = Path(__file__).resolve().parent
TRAINING_ROOT = SCRIPT_DIR.parent
DEFAULT_MODEL = (
    SCRIPT_DIR
    / "runs"
    / "train_results"
    / "yolov8n_multi_class"
    / "weights"
    / "best.pt"
)
DEFAULT_DATA = TRAINING_ROOT / "configs" / "yolo_config.yaml"
DEFAULT_TRAIN_CONFIG = (
    TRAINING_ROOT / "configs" / "yolo_finetune_train.yaml"
)
EXPECTED_CLASSES = {
    0: "blue",
    1: "green",
    2: "yellow_single",
    3: "other",
}


# %% CLI and validation
def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "默认从旧 best.pt 开始新的特殊车牌微调（resume=false）。"
            "只有恢复同一次中断任务时才使用 --resume-checkpoint。"
        )
    )
    parser.add_argument("--model", type=Path, default=DEFAULT_MODEL)
    parser.add_argument("--data", type=Path, default=DEFAULT_DATA)
    parser.add_argument(
        "--train-config",
        type=Path,
        default=DEFAULT_TRAIN_CONFIG,
    )
    parser.add_argument(
        "--resume-checkpoint",
        type=Path,
        default=None,
        help="仅用于恢复本次微调的中断 checkpoint（通常为 last.pt）",
    )
    parser.add_argument("--device", default=None)
    parser.add_argument("--project", type=Path, default=None)
    parser.add_argument("--name", default=None)
    parser.add_argument(
        "--skip-test",
        action="store_true",
        help="训练后不在 test split 上执行最终评估",
    )
    return parser.parse_args()


def require_file(path: Path, description: str) -> Path:
    resolved = path.expanduser().resolve()
    if not resolved.is_file():
        raise FileNotFoundError(f"找不到{description}：{resolved}")
    return resolved


def normalize_names(raw_names: Any) -> dict[int, str]:
    if isinstance(raw_names, list):
        return {index: str(name) for index, name in enumerate(raw_names)}
    if isinstance(raw_names, dict):
        return {int(class_id): str(name) for class_id, name in raw_names.items()}
    raise ValueError("数据 YAML 的 names 必须为列表或映射")


def validate_data_config(data_path: Path) -> None:
    data_config = YAML.load(data_path)
    required_keys = {"path", "train", "val", "test", "names"}
    missing = required_keys - data_config.keys()
    if missing:
        raise ValueError(f"数据 YAML 缺少字段：{sorted(missing)}")
    names = normalize_names(data_config["names"])
    if names != EXPECTED_CLASSES:
        raise ValueError(f"类别映射不一致：{names} != {EXPECTED_CLASSES}")
    dataset_root = Path(data_config["path"])
    if not dataset_root.is_absolute():
        dataset_root = (data_path.parent / dataset_root).resolve()
    for split in ("train", "val", "test"):
        split_path = dataset_root / data_config[split]
        if not split_path.is_dir():
            raise FileNotFoundError(f"{split} 图片目录不存在：{split_path}")


def validate_train_config(config_path: Path) -> dict[str, Any]:
    config = YAML.load(config_path)
    if config.get("resume") not in (False, None):
        raise ValueError(
            "微调配置必须保持 resume: false；中断恢复请使用 --resume-checkpoint"
        )
    if config.get("cache") not in (False, None):
        raise ValueError("当前内存不足以安全缓存全量图像，cache 必须为 false")
    return config


# %% Result recording
def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def metrics_to_dict(metrics: Any) -> dict[str, float]:
    return {
        "precision_mean": float(metrics.box.mp),
        "recall_mean": float(metrics.box.mr),
        "map50": float(metrics.box.map50),
        "map50_95": float(metrics.box.map),
    }


def write_training_summary(
    output_path: Path,
    *,
    initial_model: Path,
    initial_model_sha256: str,
    resume_checkpoint: Path | None,
    data_path: Path,
    train_config_path: Path,
    best_path: Path,
    last_path: Path,
    phase_duration_seconds: float,
    validation_metrics: Any,
    test_metrics: Any | None,
) -> None:
    summary = {
        "initial_model": str(initial_model),
        "initial_model_sha256": initial_model_sha256,
        "resume_checkpoint": (
            str(resume_checkpoint) if resume_checkpoint is not None else None
        ),
        "data": str(data_path),
        "train_config": str(train_config_path),
        "best": str(best_path),
        "last": str(last_path),
        "phase_duration_seconds": phase_duration_seconds,
        "validation": metrics_to_dict(validation_metrics),
        "test": metrics_to_dict(test_metrics) if test_metrics is not None else None,
    }
    output_path.write_text(
        json.dumps(summary, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )


# %% Training
def main() -> int:
    args = parse_args()
    data_path = require_file(args.data, "数据 YAML")
    train_config_path = require_file(args.train_config, "训练参数 YAML")
    validate_data_config(data_path)
    train_config = validate_train_config(train_config_path)

    resume_checkpoint = (
        require_file(args.resume_checkpoint, "续训 checkpoint")
        if args.resume_checkpoint is not None
        else None
    )
    initial_model = require_file(args.model, "初始模型权重")
    model_path = require_file(
        resume_checkpoint if resume_checkpoint is not None else args.model,
        "模型权重",
    )
    initial_model_digest = sha256(initial_model)
    model = YOLO(str(model_path))

    overrides: dict[str, Any] = {
        "cfg": str(train_config_path),
        "data": str(data_path),
        "resume": str(resume_checkpoint) if resume_checkpoint else False,
    }
    if args.device is not None:
        overrides["device"] = args.device
    if args.project is not None:
        overrides["project"] = str(args.project.expanduser().resolve())
    if args.name is not None:
        overrides["name"] = args.name

    mode = "恢复中断训练" if resume_checkpoint else "从旧最佳权重开始新微调"
    print(f"模式：{mode}")
    print(f"模型：{model_path}")
    print(f"数据：{data_path}")
    print(f"参数：{train_config_path}")

    start_time = time.time()
    validation_metrics = model.train(**overrides)
    duration_seconds = time.time() - start_time
    trainer = model.trainer
    best_path = Path(trainer.best).resolve()
    last_path = Path(trainer.last).resolve()
    if not best_path.is_file() or not last_path.is_file():
        raise FileNotFoundError("训练结束但找不到 best.pt 或 last.pt")

    test_metrics = None
    if not args.skip_test:
        print("训练完成，使用 best.pt 评估 test split...")
        best_model = YOLO(str(best_path))
        test_metrics = best_model.val(
            data=str(data_path),
            split="test",
            imgsz=int(train_config["imgsz"]),
            batch=int(train_config["batch"]),
            device=overrides.get("device", train_config.get("device", "0")),
            workers=int(train_config.get("workers", 8)),
            project=str(best_path.parents[2]),
            name=f"{best_path.parents[1].name}_test",
            exist_ok=True,
            plots=True,
        )

    summary_path = best_path.parents[1] / "training_summary.json"
    write_training_summary(
        summary_path,
        initial_model=initial_model,
        initial_model_sha256=initial_model_digest,
        resume_checkpoint=resume_checkpoint,
        data_path=data_path,
        train_config_path=train_config_path,
        best_path=best_path,
        last_path=last_path,
        phase_duration_seconds=duration_seconds,
        validation_metrics=validation_metrics,
        test_metrics=test_metrics,
    )
    print(f"训练耗时：{duration_seconds / 60:.2f} 分钟")
    print(f"最佳权重：{best_path}")
    print(f"训练摘要：{summary_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
