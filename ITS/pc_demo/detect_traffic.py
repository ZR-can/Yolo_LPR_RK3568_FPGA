import argparse
import csv
import os
from collections import Counter
from pathlib import Path

import cv2
import numpy as np
import onnxruntime as ort

from roi_rules import annotate_regions, draw_warning_panel, evaluate_rules, load_roi_config


OBJ_THRESH = 0.25
NMS_THRESH = 0.45
IMG_SIZE = (640, 640)

COCO_CLASSES = (
    "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train",
    "truck", "boat", "traffic light", "fire hydrant", "stop sign",
    "parking meter", "bench", "bird", "cat", "dog", "horse", "sheep",
    "cow", "elephant", "bear", "zebra", "giraffe", "backpack",
    "umbrella", "handbag", "tie", "suitcase", "frisbee", "skis",
    "snowboard", "sports ball", "kite", "baseball bat", "baseball glove",
    "skateboard", "surfboard", "tennis racket", "bottle", "wine glass",
    "cup", "fork", "knife", "spoon", "bowl", "banana", "apple",
    "sandwich", "orange", "broccoli", "carrot", "hot dog", "pizza",
    "donut", "cake", "chair", "couch", "potted plant", "bed",
    "dining table", "toilet", "tv", "laptop", "mouse", "remote",
    "keyboard", "cell phone", "microwave", "oven", "toaster", "sink",
    "refrigerator", "book", "clock", "vase", "scissors", "teddy bear",
    "hair drier", "toothbrush",
)

TRAFFIC_CLASSES = {
    "person",
    "car",
    "motorcycle",
    "bus",
    "truck",
    "traffic light",
}

CLASS_COLORS = {
    "person": (0, 220, 255),
    "car": (255, 170, 40),
    "motorcycle": (210, 120, 255),
    "bus": (60, 220, 80),
    "truck": (80, 180, 255),
    "traffic light": (0, 80, 255),
}


def default_model_path():
    this_file = Path(__file__).resolve()
    else_dir = this_file.parents[2]
    return else_dir / "Yolo_LPR_RK3568_FPGA" / "2_Model_Conversion_PC_Simulation" / "yolov8" / "model" / "yolov8n_coco.onnx"


def default_roi_path():
    return Path(__file__).resolve().parent / "configs" / "demo_roi.json"


def letterbox(image, new_shape=(640, 640), pad_color=(0, 0, 0)):
    shape = image.shape[:2]
    ratio = min(new_shape[0] / shape[0], new_shape[1] / shape[1])
    new_unpad = (int(round(shape[1] * ratio)), int(round(shape[0] * ratio)))
    dw = new_shape[1] - new_unpad[0]
    dh = new_shape[0] - new_unpad[1]
    dw /= 2
    dh /= 2

    if shape[::-1] != new_unpad:
        image = cv2.resize(image, new_unpad, interpolation=cv2.INTER_LINEAR)
    top, bottom = int(round(dh - 0.1)), int(round(dh + 0.1))
    left, right = int(round(dw - 0.1)), int(round(dw + 0.1))
    image = cv2.copyMakeBorder(
        image, top, bottom, left, right, cv2.BORDER_CONSTANT, value=pad_color
    )
    return image, ratio, dw, dh


def unletterbox_boxes(boxes, ratio, dw, dh, original_shape):
    out = boxes.copy()
    out[:, [0, 2]] = (out[:, [0, 2]] - dw) / ratio
    out[:, [1, 3]] = (out[:, [1, 3]] - dh) / ratio
    out[:, [0, 2]] = np.clip(out[:, [0, 2]], 0, original_shape[1])
    out[:, [1, 3]] = np.clip(out[:, [1, 3]], 0, original_shape[0])
    return out


def nms_boxes(boxes, scores):
    x1 = boxes[:, 0]
    y1 = boxes[:, 1]
    x2 = boxes[:, 2]
    y2 = boxes[:, 3]
    areas = (x2 - x1) * (y2 - y1)
    order = scores.argsort()[::-1]
    keep = []

    while order.size > 0:
        i = order[0]
        keep.append(i)
        xx1 = np.maximum(x1[i], x1[order[1:]])
        yy1 = np.maximum(y1[i], y1[order[1:]])
        xx2 = np.minimum(x2[i], x2[order[1:]])
        yy2 = np.minimum(y2[i], y2[order[1:]])
        w = np.maximum(0.0, xx2 - xx1)
        h = np.maximum(0.0, yy2 - yy1)
        inter = w * h
        iou = inter / (areas[i] + areas[order[1:]] - inter + 1e-6)
        inds = np.where(iou <= NMS_THRESH)[0]
        order = order[inds + 1]

    return np.array(keep)


def dfl(position):
    n, c, h, w = position.shape
    p_num = 4
    mc = c // p_num
    x = position.reshape(n, p_num, mc, h, w)
    x = x - np.max(x, axis=2, keepdims=True)
    y = np.exp(x)
    y = y / np.sum(y, axis=2, keepdims=True)
    weights = np.arange(mc, dtype=np.float32).reshape(1, 1, mc, 1, 1)
    return np.sum(y * weights, axis=2)


def box_process(position):
    grid_h, grid_w = position.shape[2:4]
    col, row = np.meshgrid(np.arange(grid_w), np.arange(grid_h))
    col = col.reshape(1, 1, grid_h, grid_w)
    row = row.reshape(1, 1, grid_h, grid_w)
    grid = np.concatenate((col, row), axis=1)
    stride = np.array(
        [IMG_SIZE[1] // grid_h, IMG_SIZE[0] // grid_w], dtype=np.float32
    ).reshape(1, 2, 1, 1)

    position = dfl(position)
    box_xy = grid + 0.5 - position[:, 0:2, :, :]
    box_xy2 = grid + 0.5 + position[:, 2:4, :, :]
    return np.concatenate((box_xy * stride, box_xy2 * stride), axis=1)


def flatten_nchw(x):
    ch = x.shape[1]
    return x.transpose(0, 2, 3, 1).reshape(-1, ch)


def filter_and_nms(boxes, class_scores, obj_scores=None):
    if obj_scores is None:
        obj_scores = np.ones((class_scores.shape[0],), dtype=np.float32)
    else:
        obj_scores = obj_scores.reshape(-1)

    class_ids = np.argmax(class_scores, axis=-1)
    class_conf = np.max(class_scores, axis=-1)
    scores = class_conf * obj_scores
    keep = np.where(scores >= OBJ_THRESH)[0]
    if keep.size == 0:
        return None, None, None

    boxes = boxes[keep]
    class_ids = class_ids[keep]
    scores = scores[keep]

    final_boxes, final_classes, final_scores = [], [], []
    for cls in sorted(set(class_ids.tolist())):
        cls_inds = np.where(class_ids == cls)[0]
        cls_keep = nms_boxes(boxes[cls_inds], scores[cls_inds])
        final_boxes.append(boxes[cls_inds][cls_keep])
        final_classes.append(class_ids[cls_inds][cls_keep])
        final_scores.append(scores[cls_inds][cls_keep])

    return (
        np.concatenate(final_boxes),
        np.concatenate(final_classes),
        np.concatenate(final_scores),
    )


def post_process_optimized(outputs):
    boxes, class_scores, obj_scores = [], [], []
    branch_count = 3
    pair_per_branch = len(outputs) // branch_count
    for i in range(branch_count):
        base = pair_per_branch * i
        boxes.append(flatten_nchw(box_process(outputs[base])))
        class_scores.append(flatten_nchw(outputs[base + 1]))
        if pair_per_branch >= 3:
            obj_scores.append(flatten_nchw(outputs[base + 2]))

    boxes = np.concatenate(boxes)
    class_scores = np.concatenate(class_scores)
    obj = np.concatenate(obj_scores) if obj_scores else None
    return filter_and_nms(boxes, class_scores, obj)


def post_process_ultralytics(outputs):
    out = outputs[0]
    if out.ndim == 3:
        out = out[0]
    if out.shape[0] < out.shape[1]:
        out = out.transpose()

    xywh = out[:, :4]
    class_scores = out[:, 4:]
    boxes = np.empty_like(xywh)
    boxes[:, 0] = xywh[:, 0] - xywh[:, 2] / 2
    boxes[:, 1] = xywh[:, 1] - xywh[:, 3] / 2
    boxes[:, 2] = xywh[:, 0] + xywh[:, 2] / 2
    boxes[:, 3] = xywh[:, 1] + xywh[:, 3] / 2
    return filter_and_nms(boxes, class_scores)


def post_process(outputs):
    if len(outputs) in (6, 9):
        return post_process_optimized(outputs)
    if len(outputs) == 1:
        return post_process_ultralytics(outputs)
    raise RuntimeError("Unsupported YOLO output count: {}".format(len(outputs)))


def iter_images(path):
    exts = {".jpg", ".jpeg", ".png", ".bmp"}
    if os.path.isdir(path):
        for name in sorted(os.listdir(path)):
            if os.path.splitext(name)[1].lower() in exts:
                yield os.path.join(path, name)
    else:
        yield path


def build_detections(boxes, classes, scores, traffic_only=True):
    detections = []
    if boxes is None:
        return detections

    order = scores.argsort()[::-1]
    for idx in order:
        cls_id = int(classes[idx])
        cls_name = COCO_CLASSES[cls_id]
        if traffic_only and cls_name not in TRAFFIC_CLASSES:
            continue
        x1, y1, x2, y2 = [float(v) for v in boxes[idx]]
        detections.append(
            {
                "class_id": cls_id,
                "class_name": cls_name,
                "score": float(scores[idx]),
                "box": [x1, y1, x2, y2],
            }
        )
    return detections


def draw_detections(image, detections):
    for det in detections:
        x1, y1, x2, y2 = [int(round(v)) for v in det["box"]]
        name = det["class_name"]
        color = CLASS_COLORS.get(name, (0, 180, 255))
        cv2.rectangle(image, (x1, y1), (x2, y2), color, 2)
        label = "{} {:.2f}".format(name, det["score"])
        cv2.putText(
            image,
            label,
            (x1, max(20, y1 - 6)),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.62,
            color,
            2,
            cv2.LINE_AA,
        )
        if "anchor" in det:
            ax, ay = det["anchor"]
            cv2.circle(image, (int(round(ax)), int(round(ay))), 4, color, -1)


def save_csv(csv_path, rows):
    if not rows:
        return
    with open(csv_path, "w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=[
                "image",
                "class_name",
                "score",
                "x1",
                "y1",
                "x2",
                "y2",
                "warnings",
            ],
        )
        writer.writeheader()
        writer.writerows(rows)


def main():
    parser = argparse.ArgumentParser(
        description="Traffic target detection + ROI risk warning demo."
    )
    parser.add_argument("--img", required=True, help="image file or image folder")
    parser.add_argument(
        "--model-path",
        default=str(default_model_path()),
        help="YOLOv8/YOLO11 ONNX model path",
    )
    parser.add_argument(
        "--save-dir",
        default=str(Path(__file__).resolve().parent / "outputs"),
        help="directory for annotated images and CSV",
    )
    parser.add_argument(
        "--roi-config",
        default=str(default_roi_path()),
        help="ROI JSON config path",
    )
    parser.add_argument("--no-roi", action="store_true", help="disable ROI rules")
    parser.add_argument("--all-classes", action="store_true", help="keep all COCO classes")
    args = parser.parse_args()

    model_path = Path(args.model_path)
    if not model_path.exists():
        raise FileNotFoundError("model not found: {}".format(model_path))

    save_dir = Path(args.save_dir)
    save_dir.mkdir(parents=True, exist_ok=True)

    sess = ort.InferenceSession(str(model_path), providers=["CPUExecutionProvider"])
    input_info = sess.get_inputs()[0]
    output_names = [x.name for x in sess.get_outputs()]

    print("model:", model_path)
    print("input:", input_info.name, input_info.shape, input_info.type)
    print("outputs:", [(x.name, x.shape) for x in sess.get_outputs()])
    print("roi:", "disabled" if args.no_roi else args.roi_config)

    csv_rows = []
    for img_path in iter_images(args.img):
        src = cv2.imread(img_path)
        if src is None:
            print("skip unreadable image:", img_path)
            continue

        padded, ratio, dw, dh = letterbox(src, new_shape=(IMG_SIZE[1], IMG_SIZE[0]))
        rgb = cv2.cvtColor(padded, cv2.COLOR_BGR2RGB)
        input_data = rgb.transpose(2, 0, 1)[None].astype(np.float32) / 255.0

        outputs = sess.run(output_names, {input_info.name: input_data})
        boxes, classes, scores = post_process(outputs)
        if boxes is not None:
            boxes = unletterbox_boxes(boxes, ratio, dw, dh, src.shape[:2])

        detections = build_detections(
            boxes, classes, scores, traffic_only=not args.all_classes
        )
        counts = Counter(det["class_name"] for det in detections)

        regions, rules = [], {}
        warnings = []
        if not args.no_roi:
            regions, rules = load_roi_config(args.roi_config, src.shape)
            _, warnings = evaluate_rules(detections, regions, rules)

        drawn = src.copy()
        if regions:
            annotate_regions(drawn, regions)
        draw_detections(drawn, detections)
        draw_warning_panel(drawn, counts, warnings)

        out_path = save_dir / Path(img_path).name
        cv2.imwrite(str(out_path), drawn)

        warning_text = "; ".join(w["message"] for w in warnings)
        for det in detections:
            x1, y1, x2, y2 = det["box"]
            csv_rows.append(
                {
                    "image": Path(img_path).name,
                    "class_name": det["class_name"],
                    "score": "{:.4f}".format(det["score"]),
                    "x1": "{:.1f}".format(x1),
                    "y1": "{:.1f}".format(y1),
                    "x2": "{:.1f}".format(x2),
                    "y2": "{:.1f}".format(y2),
                    "warnings": warning_text,
                }
            )

        print("\nIMG:", img_path)
        print("saved:", out_path)
        print("counts:", dict(counts))
        if warnings:
            for warning in warnings:
                print("warning:", warning["message"])
        else:
            print("warning: none")

    save_csv(save_dir / "detections.csv", csv_rows)
    print("\nCSV:", save_dir / "detections.csv")


if __name__ == "__main__":
    main()

