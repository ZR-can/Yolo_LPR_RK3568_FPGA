"""Run four-class license-plate OBB inference with PT, ONNX or RKNN."""

from __future__ import annotations

import argparse
import math
from dataclasses import dataclass
from pathlib import Path

import cv2
import numpy as np


CLASS_NAMES = ("blue", "green", "yellow_single", "other")
MODEL_SIZE = 640
REG_MAX = 16
EXPECTED_BRANCH_CHANNELS = REG_MAX * 4 + len(CLASS_NAMES)
EXPECTED_GRID_SIZES = (80, 40, 20)


@dataclass(frozen=True)
class Detection:
    class_id: int
    score: float
    center_x: float
    center_y: float
    width: float
    height: float
    angle: float

    def polygon(self) -> np.ndarray:
        half_width = self.width / 2.0
        half_height = self.height / 2.0
        local = np.array(
            [
                [-half_width, -half_height],
                [half_width, -half_height],
                [half_width, half_height],
                [-half_width, half_height],
            ],
            dtype=np.float32,
        )
        cosine = math.cos(self.angle)
        sine = math.sin(self.angle)
        rotation = np.array([[cosine, -sine], [sine, cosine]], dtype=np.float32)
        return local @ rotation.T + np.array(
            [self.center_x, self.center_y], dtype=np.float32
        )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model-path", "--model_path", dest="model_path", type=Path, required=True)
    parser.add_argument("--image-path", "--image_path", dest="image_path", type=Path, required=True)
    parser.add_argument("--output-path", "--output_path", dest="output_path", type=Path, default=Path("result.jpg"))
    parser.add_argument("--target", default="rk3568")
    parser.add_argument("--device-id", "--device_id", dest="device_id", default=None)
    parser.add_argument("--conf-threshold", type=float, default=0.25)
    parser.add_argument("--nms-threshold", type=float, default=0.4)
    parser.add_argument("--max-det", type=int, default=300)
    return parser.parse_args()


def letterbox(image: np.ndarray) -> tuple[np.ndarray, float, int, int]:
    height, width = image.shape[:2]
    scale = min(MODEL_SIZE / width, MODEL_SIZE / height)
    resized_width = round(width * scale)
    resized_height = round(height * scale)
    interpolation = cv2.INTER_AREA if scale < 1.0 else cv2.INTER_LINEAR
    resized = cv2.resize(image, (resized_width, resized_height), interpolation=interpolation)
    canvas = np.full((MODEL_SIZE, MODEL_SIZE, 3), 114, dtype=np.uint8)
    offset_x = (MODEL_SIZE - resized_width) // 2
    offset_y = (MODEL_SIZE - resized_height) // 2
    canvas[
        offset_y : offset_y + resized_height,
        offset_x : offset_x + resized_width,
    ] = resized
    return canvas, scale, offset_x, offset_y


def float_nchw(rgb_image: np.ndarray) -> np.ndarray:
    return np.ascontiguousarray(rgb_image.transpose(2, 0, 1)[None]).astype(np.float32) / 255.0


def infer_onnx(model_path: Path, rgb_image: np.ndarray) -> list[np.ndarray]:
    import onnxruntime as ort

    session = ort.InferenceSession(str(model_path), providers=["CPUExecutionProvider"])
    input_info = session.get_inputs()
    if len(input_info) != 1 or tuple(input_info[0].shape) != (1, 3, 640, 640):
        raise ValueError(f"Unexpected ONNX input signature: {[item.shape for item in input_info]}")
    return session.run(None, {input_info[0].name: float_nchw(rgb_image)})


def infer_pt(model_path: Path, rgb_image: np.ndarray) -> list[np.ndarray]:
    import torch
    from ultralytics import YOLO

    yolo = YOLO(str(model_path))
    model = yolo.model.cpu().float().eval().fuse()
    head = model.model[-1]
    if type(head).__name__ != "OBB" or getattr(head, "nc", None) != len(CLASS_NAMES):
        raise ValueError("PT model is not the project four-class OBB model")
    head.export = True
    head.format = "rknn"
    with torch.no_grad():
        raw = model(torch.from_numpy(float_nchw(rgb_image)))
    if not (
        isinstance(raw, list)
        and len(raw) == 2
        and isinstance(raw[0], list)
        and len(raw[0]) == 3
    ):
        raise RuntimeError(
            "PT raw-head output is unavailable. Run with the local Rockchip "
            "ultralytics_yolov8 fork on PYTHONPATH."
        )
    return [tensor.detach().cpu().numpy() for tensor in raw[0]] + [
        raw[1].detach().cpu().numpy()
    ]


def infer_rknn(
    model_path: Path, rgb_image: np.ndarray, target: str, device_id: str | None
) -> list[np.ndarray]:
    try:
        from rknn.api import RKNN
    except ImportError as error:
        raise RuntimeError("RKNN Toolkit2 is required for .rknn inference") from error

    rknn = RKNN(verbose=False)
    try:
        ret = rknn.load_rknn(str(model_path))
        if ret != 0:
            raise RuntimeError(f"rknn.load_rknn failed: {ret}")
        ret = rknn.init_runtime(target=target, device_id=device_id)
        if ret != 0:
            raise RuntimeError(f"rknn.init_runtime failed: {ret}")
        return rknn.inference(inputs=[rgb_image])
    finally:
        rknn.release()


def run_backend(
    model_path: Path, rgb_image: np.ndarray, target: str, device_id: str | None
) -> list[np.ndarray]:
    suffix = model_path.suffix.lower()
    if suffix == ".onnx":
        return infer_onnx(model_path, rgb_image)
    if suffix == ".pt":
        return infer_pt(model_path, rgb_image)
    if suffix == ".rknn":
        return infer_rknn(model_path, rgb_image, target, device_id)
    raise ValueError(f"Unsupported model extension: {model_path.suffix}")


def normalize_outputs(outputs: list[np.ndarray]) -> tuple[list[np.ndarray], np.ndarray]:
    branches: list[np.ndarray] = []
    angle = None
    for raw in outputs:
        array = np.asarray(raw)
        if array.size == 8400 and array.ndim in (2, 3, 4):
            angle = array.reshape(-1).astype(np.float32, copy=False)
            continue
        if array.ndim != 4:
            raise ValueError(f"Unexpected detection output shape: {array.shape}")
        if array.shape[1] == EXPECTED_BRANCH_CHANNELS:
            branch = array
        elif array.shape[-1] == EXPECTED_BRANCH_CHANNELS:
            branch = array.transpose(0, 3, 1, 2)
        else:
            raise ValueError(f"Unexpected detection channel count: {array.shape}")
        branches.append(branch.astype(np.float32, copy=False))

    branches.sort(key=lambda branch: branch.shape[2], reverse=True)
    shapes = tuple(tuple(branch.shape) for branch in branches)
    expected = tuple(
        (1, EXPECTED_BRANCH_CHANNELS, size, size) for size in EXPECTED_GRID_SIZES
    )
    if shapes != expected or angle is None or angle.shape != (8400,):
        raise ValueError(f"Unexpected OBB outputs: branches={shapes}, angle={None if angle is None else angle.shape}")
    if not all(np.isfinite(branch).all() for branch in branches) or not np.isfinite(angle).all():
        raise ValueError("Model outputs contain NaN or Inf")
    return branches, angle


def sigmoid(values: np.ndarray) -> np.ndarray:
    clipped = np.clip(values, -50.0, 50.0)
    return 1.0 / (1.0 + np.exp(-clipped))


def softmax(values: np.ndarray, axis: int) -> np.ndarray:
    shifted = values - np.max(values, axis=axis, keepdims=True)
    exponent = np.exp(shifted)
    return exponent / np.sum(exponent, axis=axis, keepdims=True)


def decode_outputs(
    branches: list[np.ndarray], angle_feature: np.ndarray, conf_threshold: float
) -> list[Detection]:
    detections: list[Detection] = []
    angle_offset = 0
    bins = np.arange(REG_MAX, dtype=np.float32).reshape(1, REG_MAX, 1, 1)
    for branch, stride in zip(branches, (8, 16, 32)):
        _, _, grid_height, grid_width = branch.shape
        distribution = branch[:, : REG_MAX * 4].reshape(
            4, REG_MAX, grid_height, grid_width
        )
        distances = np.sum(softmax(distribution, axis=1) * bins, axis=1)
        class_scores = sigmoid(branch[0, REG_MAX * 4 :])
        candidates = np.argwhere(class_scores > conf_threshold)
        for class_id, grid_y, grid_x in candidates:
            left, top, right, bottom = distances[:, grid_y, grid_x]
            angle_index = angle_offset + grid_y * grid_width + grid_x
            angle = (float(angle_feature[angle_index]) - 0.25) * math.pi
            cosine = math.cos(angle)
            sine = math.sin(angle)
            delta_x = (right - left) / 2.0
            delta_y = (bottom - top) / 2.0
            center_x = (
                delta_x * cosine - delta_y * sine + grid_x + 0.5
            ) * stride
            center_y = (
                delta_x * sine + delta_y * cosine + grid_y + 0.5
            ) * stride
            detections.append(
                Detection(
                    class_id=int(class_id),
                    score=float(class_scores[class_id, grid_y, grid_x]),
                    center_x=float(center_x),
                    center_y=float(center_y),
                    width=float((left + right) * stride),
                    height=float((top + bottom) * stride),
                    angle=angle,
                )
            )
        angle_offset += grid_height * grid_width
    return detections


def rotated_iou(first: Detection, second: Detection) -> float:
    first_polygon = first.polygon().astype(np.float32)
    second_polygon = second.polygon().astype(np.float32)
    first_area = abs(cv2.contourArea(first_polygon))
    second_area = abs(cv2.contourArea(second_polygon))
    intersection, _ = cv2.intersectConvexConvex(first_polygon, second_polygon)
    union = first_area + second_area - intersection
    return float(intersection / union) if union > 0.0 else 0.0


def rotated_nms(
    detections: list[Detection], threshold: float, max_det: int
) -> list[Detection]:
    kept: list[Detection] = []
    for class_id in range(len(CLASS_NAMES)):
        pending = sorted(
            (item for item in detections if item.class_id == class_id),
            key=lambda item: item.score,
            reverse=True,
        )
        while pending and len(kept) < max_det:
            current = pending.pop(0)
            kept.append(current)
            pending = [
                candidate
                for candidate in pending
                if rotated_iou(current, candidate) <= threshold
            ]
    return sorted(kept, key=lambda item: item.score, reverse=True)[:max_det]


def restore_polygon(
    detection: Detection,
    scale: float,
    offset_x: int,
    offset_y: int,
    image_width: int,
    image_height: int,
) -> np.ndarray:
    polygon = detection.polygon()
    polygon[:, 0] = (polygon[:, 0] - offset_x) / scale
    polygon[:, 1] = (polygon[:, 1] - offset_y) / scale
    polygon[:, 0] = np.clip(polygon[:, 0], 0, image_width - 1)
    polygon[:, 1] = np.clip(polygon[:, 1], 0, image_height - 1)
    return polygon


def draw_results(
    image: np.ndarray,
    detections: list[Detection],
    scale: float,
    offset_x: int,
    offset_y: int,
) -> np.ndarray:
    result = image.copy()
    height, width = result.shape[:2]
    for detection in detections:
        polygon = restore_polygon(
            detection, scale, offset_x, offset_y, width, height
        )
        integer_polygon = np.rint(polygon).astype(np.int32)
        cv2.polylines(result, [integer_polygon], True, (0, 255, 0), 2)
        text_x = int(np.min(integer_polygon[:, 0]))
        text_y = max(16, int(np.min(integer_polygon[:, 1])) - 4)
        text = f"{CLASS_NAMES[detection.class_id]} {detection.score:.3f}"
        cv2.putText(
            result,
            text,
            (text_x, text_y),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.5,
            (0, 0, 255),
            1,
            cv2.LINE_AA,
        )
        print(
            f"{text}, angle={math.degrees(detection.angle):.2f} deg, "
            f"points={integer_polygon.tolist()}"
        )
    return result


def main() -> int:
    args = parse_args()
    model_path = args.model_path.resolve()
    image_path = args.image_path.resolve()
    output_path = args.output_path.resolve()
    if not model_path.is_file():
        raise FileNotFoundError(f"Model not found: {model_path}")
    image = cv2.imread(str(image_path))
    if image is None:
        raise FileNotFoundError(f"Image cannot be decoded: {image_path}")
    if not 0.0 < args.conf_threshold < 1.0:
        raise ValueError("--conf-threshold must be between 0 and 1")
    if not 0.0 < args.nms_threshold < 1.0:
        raise ValueError("--nms-threshold must be between 0 and 1")

    letterboxed, scale, offset_x, offset_y = letterbox(image)
    rgb_image = cv2.cvtColor(letterboxed, cv2.COLOR_BGR2RGB)
    raw_outputs = run_backend(model_path, rgb_image, args.target, args.device_id)
    branches, angle_feature = normalize_outputs(raw_outputs)
    candidates = decode_outputs(branches, angle_feature, args.conf_threshold)
    detections = rotated_nms(candidates, args.nms_threshold, args.max_det)
    result = draw_results(image, detections, scale, offset_x, offset_y)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    if not cv2.imwrite(str(output_path), result):
        raise RuntimeError(f"Failed to save result: {output_path}")
    print(f"Candidates before NMS: {len(candidates)}")
    print(f"Detections after NMS: {len(detections)}")
    print(f"Result saved: {output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
