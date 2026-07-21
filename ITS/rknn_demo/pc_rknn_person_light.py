#!/usr/bin/env python3
"""
PC-side RKNN Toolkit2 YOLOv8 person / traffic-light checker.

This script uses RKNN Toolkit2 runtime simulator on an x86 Ubuntu PC/VM.
It is for functional checking only; FPS does not represent RK3568 NPU speed.

RKNN Toolkit2 1.6.0 cannot run PC simulator inference from an exported .rknn
loaded with load_rknn(). For no-board PC checks, use --onnx so the script runs
load_onnx() -> build() -> init_runtime() -> inference() inside RKNN Toolkit2.
"""

import argparse
import csv
import os
import sys
import time
from pathlib import Path

import cv2
import numpy as np

try:
    from rknn.api import RKNN
except Exception as exc:
    RKNN = None
    RKNN_IMPORT_ERROR = exc
else:
    RKNN_IMPORT_ERROR = None


PERSON_ID = 0
TRAFFIC_LIGHT_ID = 9
KEEP_CLASS_IDS = {PERSON_ID, TRAFFIC_LIGHT_ID}
CLASS_NAMES = {
    PERSON_ID: "person",
    TRAFFIC_LIGHT_ID: "traffic light",
}


def detection_summary_lines(title, model_path, input_path, output_path, csv_path,
                            processed, total_infer, total_detections,
                            total_person, total_light,
                            sum_conf, sum_person_conf, sum_light_conf):
    avg_infer = total_infer / processed if processed else 0.0
    infer_fps = 1000.0 / avg_infer if avg_infer > 0 else 0.0
    avg_conf = sum_conf / total_detections if total_detections else 0.0
    avg_person_conf = sum_person_conf / total_person if total_person else 0.0
    avg_light_conf = sum_light_conf / total_light if total_light else 0.0
    return [
        title,
        f"model={model_path}",
        f"input={input_path}",
        f"output={output_path}",
        f"csv={csv_path}",
        f"frames_with_inference={processed}",
        f"avg_inference_ms={avg_infer:.3f}",
        f"inference_fps={infer_fps:.2f}",
        f"detections_total={total_detections}",
        f"detections_person={total_person}",
        f"detections_traffic_light={total_light}",
        f"avg_confidence_all={avg_conf:.4f}",
        f"avg_confidence_person={avg_person_conf:.4f}",
        f"avg_confidence_traffic_light={avg_light_conf:.4f}",
    ]


def print_and_write_summary(lines, summary_path):
    print("summary:")
    for line in lines:
        print(f"  {line}")
    summary_path = Path(summary_path)
    summary_path.parent.mkdir(parents=True, exist_ok=True)
    summary_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"summary file: {summary_path}")


def sigmoid(x):
    return 1.0 / (1.0 + np.exp(-x))


def softmax(x, axis=0):
    x = x - np.max(x, axis=axis, keepdims=True)
    e = np.exp(x)
    return e / np.sum(e, axis=axis, keepdims=True)


def letterbox_bgr_to_rgb(image_bgr, model_size=(640, 640), pad_value=0):
    src_h, src_w = image_bgr.shape[:2]
    model_w, model_h = model_size
    scale = min(model_w / src_w, model_h / src_h)
    new_w = int(round(src_w * scale))
    new_h = int(round(src_h * scale))
    resized = cv2.resize(image_bgr, (new_w, new_h), interpolation=cv2.INTER_LINEAR)

    canvas = np.full((model_h, model_w, 3), pad_value, dtype=np.uint8)
    x_pad = (model_w - new_w) // 2
    y_pad = (model_h - new_h) // 2
    canvas[y_pad:y_pad + new_h, x_pad:x_pad + new_w] = resized
    rgb = cv2.cvtColor(canvas, cv2.COLOR_BGR2RGB)
    return rgb, {
        "scale": scale,
        "x_pad": x_pad,
        "y_pad": y_pad,
        "src_w": src_w,
        "src_h": src_h,
        "model_w": model_w,
        "model_h": model_h,
    }


def parse_crop_box(crop_text):
    if not crop_text:
        return None
    parts = [p.strip() for p in crop_text.replace(";", ",").split(",")]
    if len(parts) != 4:
        raise ValueError("--crop must be left,top,right,bottom")
    try:
        left, top, right, bottom = [int(round(float(p))) for p in parts]
    except ValueError as exc:
        raise ValueError("--crop values must be numbers") from exc
    if right <= left or bottom <= top:
        raise ValueError("--crop requires right > left and bottom > top")
    return left, top, right, bottom


def crop_image(image_bgr, crop_box):
    if crop_box is None:
        return image_bgr, {"x_offset": 0, "y_offset": 0, "box": None}

    h, w = image_bgr.shape[:2]
    left, top, right, bottom = crop_box
    left = max(0, min(left, w - 1))
    top = max(0, min(top, h - 1))
    right = max(left + 1, min(right, w))
    bottom = max(top + 1, min(bottom, h))
    cropped = image_bgr[top:bottom, left:right]
    return cropped, {"x_offset": left, "y_offset": top, "box": (left, top, right, bottom)}


def shift_detections_to_original(detections, crop_meta):
    x_offset = crop_meta["x_offset"]
    y_offset = crop_meta["y_offset"]
    if x_offset == 0 and y_offset == 0:
        return detections

    shifted = []
    for box, score, cls_id in detections:
        out_box = box.copy().astype(np.float32)
        out_box[[0, 2]] += x_offset
        out_box[[1, 3]] += y_offset
        shifted.append((out_box, score, cls_id))
    return shifted


def dfl_decode(box_tensor):
    dfl_len = box_tensor.shape[0] // 4
    box_tensor = box_tensor.reshape(4, dfl_len, *box_tensor.shape[1:])
    prob = softmax(box_tensor, axis=1)
    bins = np.arange(dfl_len, dtype=np.float32).reshape(1, dfl_len, 1, 1)
    return np.sum(prob * bins, axis=1)


def nms_xyxy(boxes, scores, iou_thres):
    if len(boxes) == 0:
        return []
    boxes = boxes.astype(np.float32)
    scores = scores.astype(np.float32)
    x1, y1, x2, y2 = boxes[:, 0], boxes[:, 1], boxes[:, 2], boxes[:, 3]
    areas = np.maximum(0, x2 - x1) * np.maximum(0, y2 - y1)
    order = scores.argsort()[::-1]
    keep = []
    while order.size > 0:
        i = int(order[0])
        keep.append(i)
        if order.size == 1:
            break
        xx1 = np.maximum(x1[i], x1[order[1:]])
        yy1 = np.maximum(y1[i], y1[order[1:]])
        xx2 = np.minimum(x2[i], x2[order[1:]])
        yy2 = np.minimum(y2[i], y2[order[1:]])
        w = np.maximum(0.0, xx2 - xx1)
        h = np.maximum(0.0, yy2 - yy1)
        inter = w * h
        union = areas[i] + areas[order[1:]] - inter
        iou = np.where(union > 0, inter / union, 0.0)
        order = order[1:][iou <= iou_thres]
    return keep


def map_boxes_to_original(boxes, meta):
    out = boxes.copy().astype(np.float32)
    out[:, [0, 2]] = (out[:, [0, 2]] - meta["x_pad"]) / meta["scale"]
    out[:, [1, 3]] = (out[:, [1, 3]] - meta["y_pad"]) / meta["scale"]
    out[:, [0, 2]] = np.clip(out[:, [0, 2]], 0, meta["src_w"] - 1)
    out[:, [1, 3]] = np.clip(out[:, [1, 3]], 0, meta["src_h"] - 1)
    return out


def normalize_output_array(arr):
    arr = np.asarray(arr)
    if arr.ndim == 4 and arr.shape[0] == 1:
        arr = arr[0]
    if arr.ndim == 3 and arr.shape[0] == 1:
        arr = arr[0]
    return arr.astype(np.float32)


def as_chw_tensor(arr, preferred_channels):
    arr = normalize_output_array(arr)
    if arr.ndim != 3:
        return None
    if arr.shape[0] in preferred_channels:
        return arr
    if arr.shape[2] in preferred_channels:
        return np.transpose(arr, (2, 0, 1))
    return None


def postprocess_single_output(outputs, meta, conf_thres, nms_thres):
    arr = normalize_output_array(outputs[0])
    if arr.ndim != 2:
        return None
    if arr.shape[0] in (84, 85):
        pred = arr.T
    elif arr.shape[1] in (84, 85):
        pred = arr
    else:
        return None

    boxes_xywh = pred[:, :4]
    class_scores = pred[:, 4:84]
    if class_scores.shape[1] < 80:
        return None

    scores_person = class_scores[:, PERSON_ID]
    scores_light = class_scores[:, TRAFFIC_LIGHT_ID]
    cls_ids = np.where(scores_person >= scores_light, PERSON_ID, TRAFFIC_LIGHT_ID)
    scores = np.maximum(scores_person, scores_light)

    mask = scores >= conf_thres
    boxes_xywh = boxes_xywh[mask]
    scores = scores[mask]
    cls_ids = cls_ids[mask]
    if len(scores) == 0:
        return []

    boxes = np.empty_like(boxes_xywh)
    boxes[:, 0] = boxes_xywh[:, 0] - boxes_xywh[:, 2] * 0.5
    boxes[:, 1] = boxes_xywh[:, 1] - boxes_xywh[:, 3] * 0.5
    boxes[:, 2] = boxes_xywh[:, 0] + boxes_xywh[:, 2] * 0.5
    boxes[:, 3] = boxes_xywh[:, 1] + boxes_xywh[:, 3] * 0.5
    boxes = map_boxes_to_original(boxes, meta)

    detections = []
    for cls_id in sorted(KEEP_CLASS_IDS):
        idx = np.where(cls_ids == cls_id)[0]
        keep = nms_xyxy(boxes[idx], scores[idx], nms_thres)
        for local_i in keep:
            real_i = idx[local_i]
            detections.append((boxes[real_i], float(scores[real_i]), int(cls_id)))
    return detections


def postprocess_branch_outputs(outputs, meta, conf_thres, nms_thres):
    if len(outputs) < 6 or len(outputs) % 3 not in (0,):
        # RKNN Model Zoo YOLOv8 usually has 6 outputs: box/score per scale.
        if len(outputs) < 6:
            return None

    outs = [normalize_output_array(x) for x in outputs]
    output_per_branch = len(outs) // 3
    if output_per_branch < 2:
        return None

    all_boxes = []
    all_scores = []
    all_cls_ids = []

    for scale_i in range(3):
        box = as_chw_tensor(outs[scale_i * output_per_branch], {64, 68})
        score = as_chw_tensor(outs[scale_i * output_per_branch + 1], {80})

        if box is None or score is None:
            return None
        if box.shape[0] % 4 != 0:
            return None
        if score.shape[0] < 80:
            return None

        grid_h, grid_w = box.shape[1], box.shape[2]
        stride = meta["model_h"] / grid_h
        dist = dfl_decode(box)

        person_score = score[PERSON_ID]
        light_score = score[TRAFFIC_LIGHT_ID]
        cls_score = np.maximum(person_score, light_score)
        cls_id = np.where(person_score >= light_score, PERSON_ID, TRAFFIC_LIGHT_ID)
        mask = cls_score >= conf_thres
        if not np.any(mask):
            continue

        yy, xx = np.meshgrid(np.arange(grid_h), np.arange(grid_w), indexing="ij")
        x1 = (-dist[0] + xx + 0.5) * stride
        y1 = (-dist[1] + yy + 0.5) * stride
        x2 = (dist[2] + xx + 0.5) * stride
        y2 = (dist[3] + yy + 0.5) * stride
        boxes = np.stack([x1, y1, x2, y2], axis=-1)

        all_boxes.append(boxes[mask])
        all_scores.append(cls_score[mask])
        all_cls_ids.append(cls_id[mask])

    if not all_boxes:
        return []

    boxes = np.concatenate(all_boxes, axis=0)
    scores = np.concatenate(all_scores, axis=0).astype(np.float32)
    cls_ids = np.concatenate(all_cls_ids, axis=0).astype(np.int32)
    boxes = map_boxes_to_original(boxes, meta)

    detections = []
    for cls_id in sorted(KEEP_CLASS_IDS):
        idx = np.where(cls_ids == cls_id)[0]
        keep = nms_xyxy(boxes[idx], scores[idx], nms_thres)
        for local_i in keep:
            real_i = idx[local_i]
            detections.append((boxes[real_i], float(scores[real_i]), int(cls_id)))
    return detections


def postprocess(outputs, meta, conf_thres, nms_thres):
    detections = postprocess_single_output(outputs, meta, conf_thres, nms_thres)
    if detections is not None:
        return detections
    detections = postprocess_branch_outputs(outputs, meta, conf_thres, nms_thres)
    if detections is not None:
        return detections

    shapes = [np.asarray(x).shape for x in outputs]
    raise RuntimeError(f"Unsupported RKNN YOLOv8 output shapes: {shapes}")


def draw_detections(image_bgr, detections, frame_index, infer_ms, crop_meta=None):
    person_count = 0
    light_count = 0
    if crop_meta and crop_meta.get("box"):
        left, top, right, bottom = crop_meta["box"]
        cv2.rectangle(image_bgr, (left, top), (right, bottom), (255, 160, 0), 2)
        cv2.putText(image_bgr, "inference crop", (left + 8, max(24, top + 24)),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.62, (255, 160, 0), 2, cv2.LINE_AA)
    for box, score, cls_id in detections:
        x1, y1, x2, y2 = [int(round(v)) for v in box]
        if x2 <= x1 or y2 <= y1:
            continue
        if cls_id == PERSON_ID:
            color = (0, 255, 255)
            person_count += 1
        else:
            color = (0, 0, 255)
            light_count += 1
        cv2.rectangle(image_bgr, (x1, y1), (x2, y2), color, 2)
        label = f"{CLASS_NAMES[cls_id]} {score:.2f}"
        cv2.putText(image_bgr, label, (x1, max(20, y1 - 8)),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.6, color, 2, cv2.LINE_AA)

    cv2.rectangle(image_bgr, (8, 8), (520, 95), (32, 32, 32), -1)
    cv2.putText(image_bgr, "RKNN YOLOv8: person + traffic light only",
                (18, 32), cv2.FONT_HERSHEY_SIMPLEX, 0.62, (0, 255, 255), 2, cv2.LINE_AA)
    cv2.putText(image_bgr, f"Frame:{frame_index} person:{person_count} traffic_light:{light_count}",
                (18, 58), cv2.FONT_HERSHEY_SIMPLEX, 0.58, (255, 255, 255), 2, cv2.LINE_AA)
    cv2.putText(image_bgr, f"Infer:{infer_ms:.2f} ms",
                (18, 82), cv2.FONT_HERSHEY_SIMPLEX, 0.58, (255, 255, 255), 2, cv2.LINE_AA)
    return image_bgr, person_count, light_count


def iter_image_paths(input_path):
    path = Path(input_path)
    exts = {".jpg", ".jpeg", ".png", ".bmp"}
    if path.is_dir():
        return sorted([p for p in path.iterdir() if p.suffix.lower() in exts])
    if path.suffix.lower() in exts:
        return [path]
    return []


def run_image_folder(rknn, args):
    image_paths = iter_image_paths(args.input)
    if not image_paths:
        raise FileNotFoundError(f"No image files found: {args.input}")

    out_dir = Path(args.output)
    out_dir.mkdir(parents=True, exist_ok=True)
    csv_path = out_dir / "person_light.csv"

    total_infer = 0.0
    total_detections = 0
    total_person = 0
    total_light = 0
    sum_conf = 0.0
    sum_person_conf = 0.0
    sum_light_conf = 0.0
    processed = 0
    with csv_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(["frame_index", "image_path", "cls_id", "class_name",
                         "score", "left", "top", "right", "bottom", "infer_ms"])
        for frame_idx, image_path in enumerate(image_paths, start=1):
            if args.max_frames > 0 and frame_idx > args.max_frames:
                break
            if (frame_idx - 1) % args.frame_stride != 0:
                continue

            image = cv2.imread(str(image_path))
            if image is None:
                print(f"skip unreadable image: {image_path}")
                continue

            model_image, crop_meta = crop_image(image, args.crop_box)
            inp, meta = letterbox_bgr_to_rgb(model_image, (args.img_size, args.img_size))
            t0 = time.perf_counter()
            outputs = rknn.inference(inputs=[inp])
            if args.dump_shapes and processed == 0:
                print("output shapes:", [np.asarray(x).shape for x in outputs])
            infer_ms = (time.perf_counter() - t0) * 1000.0
            detections = postprocess(outputs, meta, args.conf, args.nms)
            detections = shift_detections_to_original(detections, crop_meta)

            vis, persons, lights = draw_detections(image.copy(), detections, frame_idx, infer_ms, crop_meta)
            out_path = out_dir / f"result_{processed + 1:06d}.jpg"
            cv2.imwrite(str(out_path), vis)

            for box, score, cls_id in detections:
                x1, y1, x2, y2 = [int(round(v)) for v in box]
                writer.writerow([frame_idx, str(image_path), cls_id, CLASS_NAMES[cls_id],
                                 f"{score:.6f}", x1, y1, x2, y2, f"{infer_ms:.3f}"])
                total_detections += 1
                sum_conf += score
                if cls_id == PERSON_ID:
                    total_person += 1
                    sum_person_conf += score
                elif cls_id == TRAFFIC_LIGHT_ID:
                    total_light += 1
                    sum_light_conf += score

            total_infer += infer_ms
            processed += 1
            print(f"frame {frame_idx}: person={persons} traffic_light={lights} "
                  f"detections={len(detections)} infer={infer_ms:.2f} ms")

    summary_lines = detection_summary_lines(
        "Person/light image-folder summary",
        args.onnx if args.source != "rknn" else args.rknn,
        args.input,
        out_dir,
        csv_path,
        processed,
        total_infer,
        total_detections,
        total_person,
        total_light,
        sum_conf,
        sum_person_conf,
        sum_light_conf,
    )
    print_and_write_summary(summary_lines, out_dir / "summary.txt")
    print(f"output dir: {out_dir}")
    print(f"csv: {csv_path}")


def run_video(rknn, args):
    cap = cv2.VideoCapture(args.input)
    if not cap.isOpened():
        raise RuntimeError(f"Open video failed: {args.input}")

    out_path = Path(args.output)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    csv_path = out_path.with_suffix(".csv")

    src_fps = cap.get(cv2.CAP_PROP_FPS)
    if src_fps <= 0 or src_fps > 120:
        src_fps = 25.0
    output_fps = args.output_fps if args.output_fps > 0 else src_fps / max(1, args.frame_stride)
    width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    if width <= 0 or height <= 0:
        raise RuntimeError("Invalid video size")

    fourcc = cv2.VideoWriter_fourcc(*"mp4v")
    vw = cv2.VideoWriter(str(out_path), fourcc, output_fps, (width, height))
    if not vw.isOpened():
        raise RuntimeError(f"Open output video failed: {out_path}")
    print(f"video info: width={width} height={height} src_fps={src_fps:.3f} output_fps={output_fps:.3f}")

    total_infer = 0.0
    total_detections = 0
    total_person = 0
    total_light = 0
    sum_conf = 0.0
    sum_person_conf = 0.0
    sum_light_conf = 0.0
    processed = 0
    frame_idx = 0
    interrupted = False
    with csv_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow(["frame_index", "cls_id", "class_name", "score",
                         "left", "top", "right", "bottom", "infer_ms"])
        try:
            while True:
                ok, image = cap.read()
                if not ok:
                    break
                frame_idx += 1
                if args.max_frames > 0 and frame_idx > args.max_frames:
                    break
                if (frame_idx - 1) % args.frame_stride != 0:
                    continue

                model_image, crop_meta = crop_image(image, args.crop_box)
                inp, meta = letterbox_bgr_to_rgb(model_image, (args.img_size, args.img_size))
                t0 = time.perf_counter()
                outputs = rknn.inference(inputs=[inp])
                if args.dump_shapes and processed == 0:
                    print("output shapes:", [np.asarray(x).shape for x in outputs])
                infer_ms = (time.perf_counter() - t0) * 1000.0
                detections = postprocess(outputs, meta, args.conf, args.nms)
                detections = shift_detections_to_original(detections, crop_meta)

                vis, persons, lights = draw_detections(image.copy(), detections, frame_idx, infer_ms, crop_meta)
                vw.write(vis)
                for box, score, cls_id in detections:
                    x1, y1, x2, y2 = [int(round(v)) for v in box]
                    writer.writerow([frame_idx, cls_id, CLASS_NAMES[cls_id],
                                     f"{score:.6f}", x1, y1, x2, y2, f"{infer_ms:.3f}"])
                    total_detections += 1
                    sum_conf += score
                    if cls_id == PERSON_ID:
                        total_person += 1
                        sum_person_conf += score
                    elif cls_id == TRAFFIC_LIGHT_ID:
                        total_light += 1
                        sum_light_conf += score

                total_infer += infer_ms
                processed += 1
                print(f"frame {frame_idx}: person={persons} traffic_light={lights} "
                      f"detections={len(detections)} infer={infer_ms:.2f} ms")
        except KeyboardInterrupt:
            interrupted = True
            print("\nInterrupted by user. Finalizing partial video and CSV...")
        finally:
            f.flush()

    cap.release()
    vw.release()
    summary_lines = detection_summary_lines(
        "Person/light video summary",
        args.onnx if args.source != "rknn" else args.rknn,
        args.input,
        out_path,
        csv_path,
        processed,
        total_infer,
        total_detections,
        total_person,
        total_light,
        sum_conf,
        sum_person_conf,
        sum_light_conf,
    )
    print_and_write_summary(summary_lines, out_path.with_suffix(".summary.txt"))
    if interrupted:
        print("partial output saved")
    print(f"video: {out_path}")
    print(f"csv: {csv_path}")


def parse_args():
    parser = argparse.ArgumentParser(description="PC RKNN person / traffic-light checker")
    parser.add_argument("--rknn", default="model_convert/yolov8n_coco_fp.rknn",
                        help="exported RKNN model path, mainly for board/device runtime")
    parser.add_argument("--onnx", default="model_convert/yolov8n_coco.onnx",
                        help="ONNX model path used by PC simulator")
    parser.add_argument("--source", choices=["auto", "onnx", "rknn"], default="auto",
                        help="model source. For no-board PC simulator, use onnx")
    parser.add_argument("--target-platform", default="rk3568",
                        help="RKNN target platform used when building ONNX")
    parser.add_argument("--input", required=True,
                        help="input image, image folder, or video")
    parser.add_argument("--output", required=True,
                        help="output folder for images, or output mp4 for video")
    parser.add_argument("--mode", choices=["auto", "images", "video"], default="auto")
    parser.add_argument("--img-size", type=int, default=640)
    parser.add_argument("--conf", type=float, default=0.55)
    parser.add_argument("--nms", type=float, default=0.50)
    parser.add_argument("--crop", default="",
                        help="optional inference crop as left,top,right,bottom in original image pixels")
    parser.add_argument("--max-frames", type=int, default=0)
    parser.add_argument("--frame-stride", type=int, default=1)
    parser.add_argument("--output-fps", type=float, default=0.0,
                        help="override output video FPS. Default keeps real time after frame_stride")
    parser.add_argument("--dump-shapes", action="store_true",
                        help="print RKNN output tensor shapes on the first processed frame")
    parser.add_argument("--verbose", action="store_true")
    return parser.parse_args()


def main():
    args = parse_args()
    if args.frame_stride <= 0:
        args.frame_stride = 1
    try:
        args.crop_box = parse_crop_box(args.crop)
    except ValueError as exc:
        print(f"ERROR: {exc}")
        return 2
    if args.crop_box:
        print(f"Use inference crop: {args.crop_box}")

    if RKNN is None:
        print("ERROR: cannot import rknn.api.RKNN")
        print(f"detail: {RKNN_IMPORT_ERROR}")
        print("Install/activate RKNN Toolkit2 in x86 Ubuntu first.")
        return 2

    rknn = RKNN(verbose=args.verbose)

    source = args.source
    onnx_path = Path(args.onnx)
    rknn_path = Path(args.rknn)
    if source == "auto":
        source = "onnx" if onnx_path.exists() else "rknn"

    if source == "onnx":
        if not onnx_path.exists():
            print(f"ERROR: ONNX model not found: {onnx_path}")
            return 2
        print("--> Config RKNN build")
        ret = rknn.config(mean_values=[[0, 0, 0]],
                          std_values=[[255, 255, 255]],
                          target_platform=args.target_platform)
        if ret != 0:
            print("rknn.config failed")
            return ret
        print("done")

        print("--> Load ONNX model for PC simulator")
        ret = rknn.load_onnx(model=str(onnx_path))
        if ret != 0:
            print("load_onnx failed")
            return ret
        print("done")

        print("--> Build RKNN graph in memory, do_quantization=False")
        ret = rknn.build(do_quantization=False)
        if ret != 0:
            print("rknn.build failed")
            return ret
        print("done")
    else:
        if not rknn_path.exists():
            print(f"ERROR: RKNN model not found: {rknn_path}")
            return 2
        print("--> Load exported RKNN model")
        ret = rknn.load_rknn(str(rknn_path))
        if ret != 0:
            print("load_rknn failed")
            return ret
        print("done")
        print("NOTE: exported .rknn usually needs a real target/runtime.")
        print("      If init_runtime fails on PC simulator, rerun with --source onnx.")

    print("--> Init runtime: PC simulator")
    ret = rknn.init_runtime()
    if ret != 0:
        print("init_runtime failed")
        if source == "rknn":
            print("RKNN Toolkit2 cannot PC-simulate this exported .rknn.")
            print("Use --source onnx, or run the .rknn on RK3568 board runtime.")
        rknn.release()
        return ret
    print("done")

    try:
        input_suffix = Path(args.input).suffix.lower()
        mode = args.mode
        if mode == "auto":
            mode = "video" if input_suffix in {".mp4", ".avi", ".mov", ".mkv"} else "images"
        if mode == "video":
            run_video(rknn, args)
        else:
            run_image_folder(rknn, args)
    finally:
        rknn.release()
    return 0


if __name__ == "__main__":
    sys.exit(main())
