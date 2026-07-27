#!/usr/bin/env python3
"""在固定 YOLO 数据划分上输出可机器读取的逐类检测指标。"""

from __future__ import annotations

# %% Imports and defaults
import argparse
import json
from pathlib import Path

import numpy as np
from ultralytics import YOLO


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
DEFAULT_PROJECT = SCRIPT_DIR / "runs" / "finetune_evaluation"


# %% CLI
def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="使用完整 PR 曲线评估 YOLO，并保存 aggregate/per_class JSON。"
    )
    parser.add_argument("--model", type=Path, default=DEFAULT_MODEL)
    parser.add_argument("--data", type=Path, default=DEFAULT_DATA)
    parser.add_argument("--split", choices=("val", "test"), default="val")
    parser.add_argument("--project", type=Path, default=DEFAULT_PROJECT)
    parser.add_argument("--name", default=None)
    parser.add_argument("--device", default="0")
    parser.add_argument("--batch", type=int, default=16)
    parser.add_argument("--imgsz", type=int, default=640)
    parser.add_argument("--workers", type=int, default=8)
    return parser.parse_args()


def require_file(path: Path, description: str) -> Path:
    resolved = path.expanduser().resolve()
    if not resolved.is_file():
        raise FileNotFoundError(f"找不到{description}：{resolved}")
    return resolved


# %% Metrics
def class_name(names: dict[int, str] | list[str], class_id: int) -> str:
    return str(names[class_id])


def metrics_summary(
    metrics: object,
    names: dict[int, str] | list[str],
) -> dict[str, object]:
    class_ids = np.asarray(metrics.box.ap_class_index, dtype=np.int64)
    all_ap = np.asarray(metrics.box.all_ap, dtype=np.float64)
    precisions = np.asarray(metrics.box.p, dtype=np.float64)
    recalls = np.asarray(metrics.box.r, dtype=np.float64)
    f1_scores = np.asarray(metrics.box.f1, dtype=np.float64)
    per_class: dict[str, object] = {}
    for row, class_id in enumerate(class_ids):
        name = class_name(names, int(class_id))
        per_class[name] = {
            "class_id": int(class_id),
            "precision": float(precisions[row]),
            "recall": float(recalls[row]),
            "f1": float(f1_scores[row]),
            "ap50": float(all_ap[row, 0]),
            "ap50_95": float(all_ap[row].mean()),
            "ap_by_iou_50_to_95": [float(value) for value in all_ap[row]],
        }
    return {
        "aggregate": {
            "precision_mean": float(metrics.box.mp),
            "recall_mean": float(metrics.box.mr),
            "map50": float(metrics.box.map50),
            "map50_95": float(metrics.box.map),
        },
        "per_class": per_class,
    }


# %% Main
def main() -> int:
    args = parse_args()
    model_path = require_file(args.model, "模型")
    data_path = require_file(args.data, "数据 YAML")
    project = args.project.expanduser().resolve()
    run_name = args.name or f"{model_path.stem}_{args.split}"

    model = YOLO(str(model_path))
    metrics = model.val(
        data=str(data_path),
        split=args.split,
        imgsz=args.imgsz,
        batch=args.batch,
        device=args.device,
        workers=args.workers,
        project=str(project),
        name=run_name,
        exist_ok=True,
        plots=True,
        # 不设置高 conf；Ultralytics 验证默认值用于构建完整 PR 曲线。
    )
    summary = {
        "model": str(model_path),
        "data": str(data_path),
        "split": args.split,
        **metrics_summary(metrics, model.names),
    }
    output_path = project / run_name / "evaluation_summary.json"
    output_path.write_text(
        json.dumps(summary, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    print(f"评估摘要：{output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
