#!/usr/bin/env python3
"""
PC-side RKNN Toolkit2 red-light pedestrian violation checker.

This is a rule-based demo for fixed-view intersection videos:
  YOLO person detection + configured crosswalk polygon + light state.

Current COCO YOLOv8n does not contain a crosswalk class, so the crosswalk is a
manual polygon in the config file. Traffic-light state can be manual or derived
from a configured signal ROI by HSV color thresholds.
"""

import argparse
import csv
import json
import sys
import time
from pathlib import Path

import cv2
import numpy as np

from pc_rknn_person_light import (
    RKNN,
    RKNN_IMPORT_ERROR,
    PERSON_ID,
    TRAFFIC_LIGHT_ID,
    CLASS_NAMES,
    letterbox_bgr_to_rgb,
    parse_crop_box,
    crop_image,
    shift_detections_to_original,
    postprocess,
    detection_summary_lines,
    print_and_write_summary,
)


DEFAULT_CONFIG = {
    "crosswalk_mode": "lower_half",
    "crosswalk_polygon": [],
    "crosswalk_polygon_norm": [],
    "crosswalk_bbox_norm": [],
    "crosswalk_y_ratio": 0.50,
    "bottom_line_samples": 5,
    "bottom_line_min_inside": 1,
    "color_min_saturation": 70,
    "color_min_value": 90,
    "color_min_pixels": 8,
    "color_dominance_ratio": 1.20,
    "light_box_expand_ratio": 0.10,
}


def load_config(path):
    if not path:
        return dict(DEFAULT_CONFIG)
    with open(path, "r", encoding="utf-8") as f:
        cfg = json.load(f)
    merged = dict(DEFAULT_CONFIG)
    merged.update(cfg)
    return merged


def resolve_crosswalk_polygon(cfg, width, height):
    polygon = cfg.get("crosswalk_polygon", [])
    if polygon and len(polygon) >= 3:
        return polygon

    polygon_norm = cfg.get("crosswalk_polygon_norm", [])
    if polygon_norm and len(polygon_norm) >= 3:
        resolved = []
        for point in polygon_norm:
            if len(point) != 2:
                raise ValueError("crosswalk_polygon_norm points must be [x, y]")
            x_norm, y_norm = [float(v) for v in point]
            x = int(round(max(0.0, min(x_norm, 1.0)) * (width - 1)))
            y = int(round(max(0.0, min(y_norm, 1.0)) * (height - 1)))
            resolved.append([x, y])
        return resolved

    bbox_norm = cfg.get("crosswalk_bbox_norm", [])
    if bbox_norm and len(bbox_norm) == 4:
        cx, cy, bw, bh = [float(v) for v in bbox_norm]
        left = int(round((cx - bw * 0.5) * width))
        top = int(round((cy - bh * 0.5) * height))
        right = int(round((cx + bw * 0.5) * width))
        bottom = int(round((cy + bh * 0.5) * height))
        left, top, right, bottom = clamp_rect([left, top, right, bottom], width, height)
        return [[left, top], [right - 1, top], [right - 1, bottom - 1], [left, bottom - 1]]

    mode = str(cfg.get("crosswalk_mode", "lower_half")).lower()
    if mode != "lower_half":
        raise ValueError("crosswalk_polygon requires at least 3 points unless crosswalk_mode is lower_half")

    y_ratio = float(cfg.get("crosswalk_y_ratio", 0.50))
    y_ratio = max(0.05, min(y_ratio, 0.95))
    y0 = int(round(height * y_ratio))
    return [[0, y0], [width - 1, y0], [width - 1, height - 1], [0, height - 1]]


def clamp_rect(rect, width, height):
    left, top, right, bottom = [int(round(float(v))) for v in rect]
    left = max(0, min(left, width - 1))
    top = max(0, min(top, height - 1))
    right = max(left + 1, min(right, width))
    bottom = max(top + 1, min(bottom, height))
    return left, top, right, bottom


def expand_box(box, ratio, width, height):
    x1, y1, x2, y2 = [float(v) for v in box]
    bw = max(1.0, x2 - x1)
    bh = max(1.0, y2 - y1)
    pad_x = bw * ratio
    pad_y = bh * ratio
    return clamp_rect([x1 - pad_x, y1 - pad_y, x2 + pad_x, y2 + pad_y], width, height)


def point_in_polygon(point, polygon):
    poly = np.asarray(polygon, dtype=np.float32)
    return cv2.pointPolygonTest(poly, (float(point[0]), float(point[1])), False) >= 0


def bottom_line_inside_crosswalk(box, polygon, sample_count=5, min_inside=1):
    x1, y1, x2, y2 = [float(v) for v in box]
    if sample_count <= 1:
        xs = [(x1 + x2) * 0.5]
    else:
        xs = np.linspace(x1, x2, sample_count)
    inside = 0
    for x in xs:
        if point_in_polygon((x, y2), polygon):
            inside += 1
    return inside >= min_inside, inside, len(xs)


def estimate_light_state(image_bgr, roi, cfg):
    h, w = image_bgr.shape[:2]
    left, top, right, bottom = clamp_rect(roi, w, h)
    patch = image_bgr[top:bottom, left:right]
    if patch.size == 0:
        return "unknown", 0.0, 0.0, 0, 0, 0

    hsv = cv2.cvtColor(patch, cv2.COLOR_BGR2HSV)
    hch = hsv[:, :, 0]
    sch = hsv[:, :, 1]
    vch = hsv[:, :, 2]
    min_s = int(cfg.get("color_min_saturation", 70))
    min_v = int(cfg.get("color_min_value", 90))
    min_pixels = int(cfg.get("color_min_pixels", 8))
    dominance = float(cfg.get("color_dominance_ratio", 1.20))

    # Ignore black/gray/white/background pixels by requiring saturation and brightness.
    active = (sch >= min_s) & (vch >= min_v)
    active_count = int(np.count_nonzero(active))

    red = active & (((hch <= 10) | (hch >= 170)))
    green = active & (hch >= 35) & (hch <= 90)
    red_count = int(np.count_nonzero(red))
    green_count = int(np.count_nonzero(green))
    denom = max(1, red_count + green_count)
    red_ratio = float(red_count) / denom
    green_ratio = float(green_count) / denom

    if red_count >= min_pixels and red_count > green_count * dominance:
        state = "red"
    elif green_count >= min_pixels and green_count > red_count * dominance:
        state = "green"
    else:
        state = "unknown"
    return state, red_ratio, green_ratio, red_count, green_count, active_count


def choose_yolo_light_by_color(image_bgr, detections, cfg):
    lights = [(box, score) for box, score, cls_id in detections if cls_id == TRAFFIC_LIGHT_ID]
    if not lights:
        return None, "unknown", 0.0, 0.0, 0, 0, 0, 0.0, 0

    h, w = image_bgr.shape[:2]
    expand_ratio = float(cfg.get("light_box_expand_ratio", 0.10))
    best = None
    for box, score in lights:
        light_box = expand_box(box, expand_ratio, w, h)
        state, red_ratio, green_ratio, red_count, green_count, active_count = estimate_light_state(
            image_bgr, light_box, cfg
        )
        color_evidence = max(red_count, green_count)
        # Prefer actual color evidence over YOLO confidence; confidence only breaks ties.
        rank = (color_evidence, active_count, float(score))
        if best is None or rank > best["rank"]:
            best = {
                "rank": rank,
                "box": light_box,
                "state": state,
                "red_ratio": red_ratio,
                "green_ratio": green_ratio,
                "red_count": red_count,
                "green_count": green_count,
                "active_count": active_count,
                "score": float(score),
                "num_candidates": len(lights),
            }

    return (
        best["box"], best["state"], best["red_ratio"], best["green_ratio"],
        best["red_count"], best["green_count"], best["active_count"],
        best["score"], best["num_candidates"]
    )


def draw_polygon(image, polygon, color, label):
    pts = np.asarray(polygon, dtype=np.int32)
    cv2.polylines(image, [pts], True, color, 3, cv2.LINE_AA)
    if len(pts) > 0:
        x, y = int(pts[0][0]), int(pts[0][1])
        cv2.putText(image, label, (x + 8, max(24, y - 10)),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.68, color, 2, cv2.LINE_AA)


def draw_overlay(image, detections, cfg, light_state, red_ratio, green_ratio,
                 red_count, green_count, active_count,
                 violators, persons_in_crosswalk, frame_index, infer_ms, yolo_light_box):
    crosswalk = cfg["crosswalk_polygon"]
    draw_polygon(image, crosswalk, (255, 180, 0), "crosswalk ROI")

    if yolo_light_box:
        left, top, right, bottom = clamp_rect(yolo_light_box, image.shape[1], image.shape[0])
        light_color = (0, 0, 255) if light_state == "red" else (0, 255, 0) if light_state == "green" else (0, 255, 255)
        cv2.rectangle(image, (left, top), (right, bottom), light_color, 2)
        cv2.putText(image, "YOLO traffic light", (left + 6, max(22, top - 8)),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.55, light_color, 2, cv2.LINE_AA)

    violator_ids = set(violators)
    person_count = 0
    light_count = 0
    for det_idx, (box, score, cls_id) in enumerate(detections):
        x1, y1, x2, y2 = [int(round(v)) for v in box]
        if cls_id == PERSON_ID:
            person_count += 1
            color = (0, 0, 255) if det_idx in violator_ids else (0, 255, 255)
        elif cls_id == TRAFFIC_LIGHT_ID:
            light_count += 1
            color = (0, 255, 0)
        else:
            continue
        cv2.rectangle(image, (x1, y1), (x2, y2), color, 2)
        cv2.line(image, (x1, y2), (x2, y2), color, 3)
        label = f"{CLASS_NAMES.get(cls_id, cls_id)} {score:.2f}"
        if det_idx in violator_ids:
            label = "VIOLATION " + label
        cv2.putText(image, label, (x1, max(22, y1 - 8)),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.58, color, 2, cv2.LINE_AA)

    warning = light_state == "red" and len(violators) > 0
    if warning:
        rule_status = "red_violation"
    elif light_state == "red":
        rule_status = "red_no_violation"
    elif light_state == "green":
        rule_status = "green_no_violation"
    else:
        rule_status = "unknown"
    banner_color = (0, 0, 180) if warning else (30, 120, 30) if light_state == "green" else (80, 80, 80)
    cv2.rectangle(image, (8, 8), (690, 118), banner_color, -1)
    title = "RED LIGHT: pedestrian violation" if warning else (
        "RED LIGHT: no pedestrian in crosswalk" if light_state == "red" else
        "GREEN LIGHT: no pedestrian violation" if light_state == "green" else
        "LIGHT UNKNOWN: violation not confirmed"
    )
    cv2.putText(image, title, (18, 38), cv2.FONT_HERSHEY_SIMPLEX, 0.78,
                (255, 255, 255), 2, cv2.LINE_AA)
    cv2.putText(image, f"Frame:{frame_index} person:{person_count} in_crosswalk:{persons_in_crosswalk} violators:{len(violators)}",
                (18, 68), cv2.FONT_HERSHEY_SIMPLEX, 0.58, (255, 255, 255), 2, cv2.LINE_AA)
    cv2.putText(image, f"light={light_state} red={red_count} green={green_count} active={active_count} infer={infer_ms:.1f}ms",
                (18, 96), cv2.FONT_HERSHEY_SIMPLEX, 0.55, (255, 255, 255), 2, cv2.LINE_AA)
    return warning, rule_status, person_count, light_count


def init_rknn(args):
    if RKNN is None:
        print("ERROR: cannot import rknn.api.RKNN")
        print(f"detail: {RKNN_IMPORT_ERROR}")
        return None, 2
    onnx_path = Path(args.onnx)
    if not onnx_path.exists():
        print(f"ERROR: ONNX model not found: {onnx_path}")
        return None, 2

    rknn = RKNN(verbose=args.verbose)
    print("--> Config RKNN build")
    ret = rknn.config(mean_values=[[0, 0, 0]], std_values=[[255, 255, 255]],
                      target_platform=args.target_platform)
    if ret != 0:
        return None, ret
    print("done")

    print("--> Load ONNX model for PC simulator")
    ret = rknn.load_onnx(model=str(onnx_path))
    if ret != 0:
        return None, ret
    print("done")

    print("--> Build RKNN graph in memory, do_quantization=False")
    ret = rknn.build(do_quantization=False)
    if ret != 0:
        return None, ret
    print("done")

    print("--> Init runtime: PC simulator")
    ret = rknn.init_runtime()
    if ret != 0:
        return None, ret
    print("done")
    return rknn, 0


def run_video(rknn, args, cfg):
    cap = cv2.VideoCapture(args.input)
    if not cap.isOpened():
        raise RuntimeError(f"Open video failed: {args.input}")

    src_fps = cap.get(cv2.CAP_PROP_FPS)
    if src_fps <= 0 or src_fps > 120:
        src_fps = 25.0
    output_fps = args.output_fps if args.output_fps > 0 else src_fps / max(1, args.frame_stride)
    width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    if width <= 0 or height <= 0:
        raise RuntimeError("Invalid video size")

    out_path = Path(args.output)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    csv_path = out_path.with_suffix(".csv")
    fourcc = cv2.VideoWriter_fourcc(*"mp4v")
    writer_video = cv2.VideoWriter(str(out_path), fourcc, output_fps, (width, height))
    if not writer_video.isOpened():
        raise RuntimeError(f"Open output video failed: {out_path}")

    cfg = dict(cfg)
    cfg["crosswalk_polygon"] = resolve_crosswalk_polygon(cfg, width, height)

    print(f"video info: width={width} height={height} src_fps={src_fps:.3f} output_fps={output_fps:.3f}")
    print(f"crosswalk_polygon={cfg['crosswalk_polygon']}")
    print("signal source=YOLO traffic light detection box")

    crop_box = parse_crop_box(args.crop)
    total_infer = 0.0
    total_detections = 0
    total_person = 0
    total_light = 0
    sum_conf = 0.0
    sum_person_conf = 0.0
    sum_light_conf = 0.0
    processed = 0
    warning_frames = 0
    red_frames = 0
    green_frames = 0
    unknown_light_frames = 0
    red_violation_frames = 0
    red_no_violation_frames = 0
    green_no_violation_frames = 0
    frame_index = 0

    interrupted = False
    with csv_path.open("w", newline="", encoding="utf-8") as f:
        csv_writer = csv.writer(f)
        csv_writer.writerow([
            "frame_index", "light_state", "rule_status", "red_ratio", "green_ratio",
            "red_count", "green_count", "active_color_count", "warning",
            "yolo_light_score", "yolo_light_candidates",
            "yolo_light_left", "yolo_light_top", "yolo_light_right", "yolo_light_bottom",
            "det_index", "class_name", "score", "left", "top", "right", "bottom",
            "bottom_inside", "inside_samples", "sample_count", "infer_ms"
        ])

        try:
            while True:
                ok, frame = cap.read()
                if not ok:
                    break
                frame_index += 1
                if args.max_frames > 0 and frame_index > args.max_frames:
                    break
                if (frame_index - 1) % args.frame_stride != 0:
                    continue

                model_frame, crop_meta = crop_image(frame, crop_box)
                inp, meta = letterbox_bgr_to_rgb(model_frame, (args.img_size, args.img_size))
                t0 = time.perf_counter()
                outputs = rknn.inference(inputs=[inp])
                if args.dump_shapes and processed == 0:
                    print("output shapes:", [np.asarray(x).shape for x in outputs])
                infer_ms = (time.perf_counter() - t0) * 1000.0
                detections = postprocess(outputs, meta, args.conf, args.nms)
                detections = shift_detections_to_original(detections, crop_meta)

                light_state = args.light_state
                red_ratio = 0.0
                green_ratio = 0.0
                red_count = 0
                green_count = 0
                active_count = 0
                yolo_light_score = 0.0
                yolo_light_candidates = 0
                yolo_light_box = None
                if light_state == "auto":
                    (
                        yolo_light_box, light_state, red_ratio, green_ratio,
                        red_count, green_count, active_count,
                        yolo_light_score, yolo_light_candidates
                    ) = choose_yolo_light_by_color(frame, detections, cfg)

                violators = []
                row_infos = []
                persons_in_crosswalk = 0
                for det_idx, (box, score, cls_id) in enumerate(detections):
                    if cls_id != PERSON_ID:
                        row_infos.append((det_idx, cls_id, box, score, False, 0, 0))
                        continue
                    inside, inside_samples, sample_count = bottom_line_inside_crosswalk(
                        box,
                        cfg["crosswalk_polygon"],
                        int(cfg.get("bottom_line_samples", 5)),
                        int(cfg.get("bottom_line_min_inside", 1)),
                    )
                    if inside:
                        persons_in_crosswalk += 1
                    if light_state == "red" and inside:
                        violators.append(det_idx)
                    row_infos.append((det_idx, cls_id, box, score, inside, inside_samples, sample_count))

                vis = frame.copy()
                warning, rule_status, _persons, _lights = draw_overlay(
                    vis, detections, cfg, light_state, red_ratio, green_ratio,
                    red_count, green_count, active_count,
                    violators, persons_in_crosswalk, frame_index, infer_ms, yolo_light_box
                )
                if warning:
                    warning_frames += 1
                if light_state == "red":
                    red_frames += 1
                    if warning:
                        red_violation_frames += 1
                    else:
                        red_no_violation_frames += 1
                elif light_state == "green":
                    green_frames += 1
                    green_no_violation_frames += 1
                else:
                    unknown_light_frames += 1
                writer_video.write(vis)

                for det_idx, cls_id, box, score, inside, inside_samples, sample_count in row_infos:
                    x1, y1, x2, y2 = [int(round(v)) for v in box]
                    if yolo_light_box:
                        sl, st, sr, sb = [int(round(v)) for v in yolo_light_box]
                    else:
                        sl, st, sr, sb = [-1, -1, -1, -1]
                    csv_writer.writerow([
                        frame_index, light_state, rule_status,
                        f"{red_ratio:.6f}", f"{green_ratio:.6f}",
                        red_count, green_count, active_count, int(warning),
                        f"{yolo_light_score:.6f}", yolo_light_candidates,
                        sl, st, sr, sb,
                        det_idx, CLASS_NAMES.get(cls_id, str(cls_id)), f"{score:.6f}",
                        x1, y1, x2, y2, int(inside), inside_samples, sample_count, f"{infer_ms:.3f}"
                    ])
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
                print(f"frame {frame_index}: light={light_state} status={rule_status} "
                      f"in_crosswalk={persons_in_crosswalk} violators={len(violators)} "
                      f"detections={len(detections)} infer={infer_ms:.2f} ms")
        except KeyboardInterrupt:
            interrupted = True
            print("\nInterrupted by user. Finalizing partial video and CSV...")
        finally:
            f.flush()

    cap.release()
    writer_video.release()
    warning_ratio = 100.0 * warning_frames / processed if processed else 0.0
    summary_lines = detection_summary_lines(
        "Red-light violation video summary",
        args.onnx,
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
    summary_lines.extend([
        f"red_frames={red_frames}",
        f"green_frames={green_frames}",
        f"unknown_light_frames={unknown_light_frames}",
        f"red_violation_frames={red_violation_frames}",
        f"red_no_violation_frames={red_no_violation_frames}",
        f"green_no_violation_frames={green_no_violation_frames}",
        f"warning_frames={warning_frames}",
        f"warning_ratio_percent={warning_ratio:.2f}",
    ])
    print_and_write_summary(summary_lines, out_path.with_suffix(".summary.txt"))
    if interrupted:
        print("partial output saved")
    print(f"video: {out_path}")
    print(f"csv: {csv_path}")


def parse_args():
    parser = argparse.ArgumentParser(description="RKNN PC red-light pedestrian violation checker")
    parser.add_argument("--onnx", default="model_convert/yolov8n_coco.onnx")
    parser.add_argument("--target-platform", default="rk3568")
    parser.add_argument("--input", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--config", default="")
    parser.add_argument("--light-state", choices=["auto", "red", "green", "unknown"], default="auto",
                        help="auto uses the YOLO traffic-light box and red/green color count")
    parser.add_argument("--crop", default="")
    parser.add_argument("--img-size", type=int, default=640)
    parser.add_argument("--conf", type=float, default=0.35)
    parser.add_argument("--nms", type=float, default=0.50)
    parser.add_argument("--max-frames", type=int, default=0)
    parser.add_argument("--frame-stride", type=int, default=1)
    parser.add_argument("--output-fps", type=float, default=0.0)
    parser.add_argument("--dump-shapes", action="store_true")
    parser.add_argument("--verbose", action="store_true")
    return parser.parse_args()


def main():
    args = parse_args()
    if args.frame_stride <= 0:
        args.frame_stride = 1
    cfg = load_config(args.config)

    rknn, ret = init_rknn(args)
    if ret != 0:
        return ret
    try:
        run_video(rknn, args, cfg)
    finally:
        rknn.release()
    return 0


if __name__ == "__main__":
    sys.exit(main())
