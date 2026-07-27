#!/usr/bin/env python3
"""Generate fixed crosswalk and main traffic-light ROIs with Mask2Former."""

from __future__ import annotations

import argparse
import json
import math
import shlex
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Any


MASK2FORMER_MODEL = "facebook/mask2former-swin-large-mapillary-vistas-semantic"
CROSSWALK_LABEL_NAMES = frozenset({"crosswalk - plain", "lane marking - crosswalk"})
TRAFFIC_LIGHT_LABEL_NAMES = frozenset({"traffic light"})


@dataclass(frozen=True)
class LightCandidate:
    index: int
    pixel_box: tuple[int, int, int, int]
    normalized_box: tuple[float, float, float, float]
    support_frames: int
    persistence: float
    area_ratio: float
    crosswalk_distance: float
    score: float


@dataclass(frozen=True)
class MainLightSelection:
    candidate: LightCandidate
    pixel_box: tuple[int, int, int, int]
    normalized_box: tuple[float, float, float, float]
    core_refined: bool
    core_pixels: int


class RoiToolError(RuntimeError):
    """Expected command-line or inference failure."""


def build_label_mask(labels: Any, label_ids: list[int], scratch: Any, np: Any) -> Any:
    mask = np.zeros(labels.shape, dtype=np.uint8)
    for label_id in label_ids:
        np.equal(labels, label_id, out=scratch)
        np.copyto(mask, 255, where=scratch)
    return mask


def log(message: str) -> None:
    print(message, file=sys.stderr, flush=True)


def ratio(value: str) -> float:
    parsed = float(value)
    if not 0.0 < parsed <= 1.0:
        raise argparse.ArgumentTypeError("value must be in (0, 1]")
    return parsed


def positive(value: str) -> float:
    parsed = float(value)
    if parsed <= 0.0:
        raise argparse.ArgumentTypeError("value must be greater than 0")
    return parsed


def byte_value(value: str) -> int:
    parsed = int(value)
    if not 0 <= parsed <= 255:
        raise argparse.ArgumentTypeError("value must be in [0, 255]")
    return parsed


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Use local FFmpeg and Mapillary Mask2Former segmentation to generate a "
            "normalized crosswalk ROI, a reviewed main traffic-light ROI, and the "
            "RK3568 demo command."
        )
    )
    parser.add_argument("--video", required=True, type=Path, help="input fixed-camera video")
    parser.add_argument("--ffmpeg", default="ffmpeg", help="ffmpeg executable or full path")
    parser.add_argument(
        "--sample-every", type=positive, default=2.0, metavar="SECONDS",
        help="extract one frame every N seconds (default: 2)",
    )
    parser.add_argument(
        "--max-frames", type=int, default=50,
        help="maximum number of extracted frames (default: 50)",
    )
    parser.add_argument(
        "--start", type=float, default=0.0, metavar="SECONDS",
        help="skip the beginning of the video (default: 0)",
    )
    parser.add_argument("--device", default=None, help="PyTorch device, e.g. 0, cuda:0 or cpu")
    parser.add_argument(
        "--min-valid-ratio", type=ratio, default=0.40,
        help="minimum fraction of sampled frames with a mask (default: 0.40)",
    )
    parser.add_argument(
        "--vote-threshold", type=ratio, default=0.60,
        help="pixel vote threshold among valid frames (default: 0.60)",
    )
    parser.add_argument(
        "--close-ratio", type=ratio, default=0.050,
        help="morphological closing kernel relative to short image edge",
    )
    parser.add_argument(
        "--min-area-ratio", type=ratio, default=0.005,
        help="minimum final ROI area relative to the image",
    )
    parser.add_argument(
        "--epsilon-ratio", type=ratio, default=0.012,
        help="polygon simplification tolerance relative to perimeter",
    )
    parser.add_argument(
        "--max-points", type=int, default=8,
        help="target maximum polygon points (default: 8)",
    )
    parser.add_argument(
        "--light-vote-threshold", type=ratio, default=0.10,
        help="minimum sampled-frame support for a traffic-light candidate (default: 0.10)",
    )
    parser.add_argument(
        "--light-close-ratio", type=ratio, default=0.008,
        help="traffic-light mask closing kernel relative to short image edge",
    )
    parser.add_argument(
        "--light-merge-ratio", type=ratio, default=0.025,
        help="merge nearby traffic-light mask components within this short-edge ratio",
    )
    parser.add_argument(
        "--light-padding-ratio", type=ratio, default=0.006,
        help="padding around the refined traffic-light core",
    )
    parser.add_argument(
        "--light-core-min-value", type=byte_value, default=160,
        help="minimum RGB value for an illuminated traffic-light core pixel",
    )
    parser.add_argument(
        "--light-core-min-saturation", type=byte_value, default=100,
        help="minimum 0..255 saturation for an illuminated core pixel",
    )
    parser.add_argument(
        "--light-min-area-ratio", type=ratio, default=0.000002,
        help="minimum traffic-light mask area relative to the image",
    )
    parser.add_argument(
        "--light-max-area-ratio", type=ratio, default=0.020,
        help="maximum traffic-light mask area relative to the image",
    )
    parser.add_argument(
        "--main-light-index", type=int, default=None,
        help="override the automatic main-light candidate index shown in the preview",
    )
    parser.add_argument("--interval", type=int, default=2, help="board inference interval")
    parser.add_argument(
        "--demo", default="./yolov8_traffic_pcie_demo",
        help="board demo executable used in generated command",
    )
    parser.add_argument(
        "--board-model", default="./model/yolov8_traffic_i8.rknn",
        help="board RKNN model used in generated command",
    )
    parser.add_argument(
        "--output-dir", type=Path, default=None,
        help="preview/JSON output directory (default: demo results/mask2former_roi/<video>)",
    )
    args = parser.parse_args()
    if args.max_frames < 3:
        parser.error("--max-frames must be at least 3")
    if args.start < 0.0:
        parser.error("--start must be non-negative")
    if args.max_points < 3 or args.interval < 1:
        parser.error("--max-points must be >= 3 and --interval must be >= 1")
    if args.light_min_area_ratio >= args.light_max_area_ratio:
        parser.error("--light-min-area-ratio must be smaller than --light-max-area-ratio")
    if args.main_light_index is not None and args.main_light_index < 0:
        parser.error("--main-light-index must be non-negative")
    return args


def require_file(path: Path, label: str) -> Path:
    resolved = path.expanduser().resolve()
    if not resolved.is_file():
        raise RoiToolError(f"{label} does not exist: {resolved}")
    return resolved


def resolve_executable(command: str) -> str:
    candidate = Path(command).expanduser()
    if candidate.parent != Path(".") or candidate.is_absolute():
        resolved = candidate.resolve()
        if not resolved.is_file():
            raise RoiToolError(f"FFmpeg executable does not exist: {resolved}")
        return str(resolved)
    located = shutil.which(command)
    if located is None:
        raise RoiToolError("FFmpeg was not found; pass its full path with --ffmpeg")
    return located


def extract_frames(args: argparse.Namespace, ffmpeg: str, video: Path, directory: Path) -> list[Path]:
    output_pattern = directory / "frame_%06d.jpg"
    command = [ffmpeg, "-hide_banner", "-loglevel", "error", "-y"]
    if args.start > 0.0:
        command += ["-ss", f"{args.start:.3f}"]
    command += [
        "-i", str(video),
        "-vf", f"fps=1/{args.sample_every:.6f}",
        "-frames:v", str(args.max_frames),
        "-q:v", "2",
        str(output_pattern),
    ]
    completed = subprocess.run(command, capture_output=True, text=True, check=False)
    if completed.returncode != 0:
        detail = completed.stderr.strip() or f"exit code {completed.returncode}"
        raise RoiToolError(f"FFmpeg frame extraction failed: {detail}")
    frames = sorted(directory.glob("frame_*.jpg"))
    if len(frames) < 3:
        raise RoiToolError(f"FFmpeg produced only {len(frames)} frame(s); at least 3 are required")
    return frames


def load_inference_runtime() -> tuple[Any, Any, Any, Any, Any, Any]:
    try:
        import cv2
        import numpy as np
        import torch
        from PIL import Image
        from transformers import AutoImageProcessor, Mask2FormerForUniversalSegmentation
    except ImportError as exc:
        raise RoiToolError(
            "Missing PC dependencies; run: pip install -r pc_tools/requirements-mask2former.txt"
        ) from exc
    return cv2, np, torch, Image, AutoImageProcessor, Mask2FormerForUniversalSegmentation


def resolve_device(requested: str | None, torch: Any) -> str:
    if requested is None:
        return "cuda:0" if torch.cuda.is_available() else "cpu"
    return f"cuda:{requested}" if requested.isdigit() else requested


class Mask2FormerSegmenter:
    def __init__(
        self,
        device: str,
        cv2: Any,
        np: Any,
        torch: Any,
        image_type: Any,
        processor_type: Any,
        model_type: Any,
    ) -> None:
        self.device = device
        self.cv2 = cv2
        self.np = np
        self.torch = torch
        self.image_type = image_type
        dtype = torch.float16 if device.startswith("cuda") else torch.float32
        log(f"[Mask2Former] loading {MASK2FORMER_MODEL} on {device}")
        self.processor = processor_type.from_pretrained(MASK2FORMER_MODEL)
        self.model = model_type.from_pretrained(
            MASK2FORMER_MODEL, use_safetensors=True, torch_dtype=dtype
        ).to(device)
        self.model.eval()
        self.dtype = dtype
        self.id2label = {
            int(label_id): name for label_id, name in self.model.config.id2label.items()
        }
        self.crosswalk_ids = sorted(
            label_id
            for label_id, name in self.id2label.items()
            if name.casefold() in CROSSWALK_LABEL_NAMES
        )
        self.traffic_light_ids = sorted(
            label_id
            for label_id, name in self.id2label.items()
            if name.casefold() in TRAFFIC_LIGHT_LABEL_NAMES
        )
        if not self.crosswalk_ids:
            raise RoiToolError("Mask2Former model does not contain Mapillary crosswalk labels")
        if not self.traffic_light_ids:
            raise RoiToolError("Mask2Former model does not contain the Mapillary Traffic Light label")
        crosswalk_labels = [self.id2label[label_id] for label_id in self.crosswalk_ids]
        light_labels = [self.id2label[label_id] for label_id in self.traffic_light_ids]
        log(
            "[Mask2Former] crosswalk labels: "
            f"{dict(zip(self.crosswalk_ids, crosswalk_labels))}"
        )
        log(
            "[Mask2Former] traffic-light labels: "
            f"{dict(zip(self.traffic_light_ids, light_labels))}"
        )

    def segment(self, image: Any) -> tuple[Any, Any]:
        rgb = self.cv2.cvtColor(image, self.cv2.COLOR_BGR2RGB)
        pil_image = self.image_type.fromarray(rgb)
        inputs = self.processor(images=pil_image, return_tensors="pt")
        inputs = {name: tensor.to(self.device) for name, tensor in inputs.items()}
        inputs["pixel_values"] = inputs["pixel_values"].to(dtype=self.dtype)
        with self.torch.inference_mode():
            outputs = self.model(**inputs)
        # Transformers 4.38 combines softmax FP32 class scores with FP16 mask
        # scores in post-processing. Normalize both tensors to FP32 first.
        outputs.class_queries_logits = outputs.class_queries_logits.float()
        outputs.masks_queries_logits = outputs.masks_queries_logits.float()
        height, width = image.shape[:2]
        semantic = self.processor.post_process_semantic_segmentation(
            outputs, target_sizes=[(height, width)]
        )[0]
        labels = semantic.detach().to(
            device="cpu", dtype=self.torch.int16
        ).numpy()
        # The CPU label map no longer depends on the large per-query GPU tensors.
        # Release their references before allocating both full-resolution masks.
        del semantic, outputs, inputs, pil_image, rgb

        scratch = self.np.empty(labels.shape, dtype=self.np.bool_)
        crosswalk = build_label_mask(
            labels, self.crosswalk_ids, scratch, self.np
        )
        traffic_lights = build_label_mask(
            labels, self.traffic_light_ids, scratch, self.np
        )
        return crosswalk, traffic_lights


def close_mask(mask: Any, close_ratio: float, cv2: Any) -> Any:
    short_edge = min(mask.shape[:2])
    kernel_size = max(3, int(round(short_edge * close_ratio)))
    if kernel_size % 2 == 0:
        kernel_size += 1
    kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (kernel_size, kernel_size))
    return cv2.morphologyEx(mask, cv2.MORPH_CLOSE, kernel)


def build_light_core_mask(
    image: Any, light_mask: Any, args: argparse.Namespace, np: Any
) -> Any:
    pixels = image.astype(np.int32)
    blue = pixels[:, :, 0]
    green = pixels[:, :, 1]
    red = pixels[:, :, 2]
    maximum = pixels.max(axis=2)
    minimum = pixels.min(axis=2)
    saturation = (maximum - minimum) * 255 // np.maximum(maximum, 1)
    red_or_green_over_blue = np.maximum(red, green) - blue
    return (
        (light_mask > 0)
        & (maximum >= args.light_core_min_value)
        & (saturation >= args.light_core_min_saturation)
        & (red_or_green_over_blue >= 20)
    )


def collect_votes(
    frames: list[Path], segmenter: Mask2FormerSegmenter, args: argparse.Namespace, cv2: Any, np: Any
) -> tuple[Any, Any, Any, Any, int, int, int, int]:
    crosswalk_votes = None
    light_votes = None
    light_core_votes = None
    preview = None
    width = 0
    height = 0
    crosswalk_valid_frames = 0
    light_valid_frames = 0

    for index, frame in enumerate(frames, start=1):
        image = cv2.imread(str(frame), cv2.IMREAD_COLOR)
        if image is None:
            raise RoiToolError(f"cannot read extracted frame: {frame}")
        frame_height, frame_width = image.shape[:2]
        if crosswalk_votes is None:
            width, height = frame_width, frame_height
            crosswalk_votes = np.zeros((height, width), dtype=np.uint16)
            light_votes = np.zeros((height, width), dtype=np.uint16)
            light_core_votes = np.zeros((height, width), dtype=np.uint16)
            preview = image.copy()
        elif (frame_width, frame_height) != (width, height):
            raise RoiToolError("extracted frames do not have a consistent resolution")

        crosswalk_mask, light_mask = segmenter.segment(image)
        crosswalk_detected = bool(np.any(crosswalk_mask))
        light_detected = bool(np.any(light_mask))
        if crosswalk_detected:
            crosswalk_mask = close_mask(crosswalk_mask, args.close_ratio, cv2)
            crosswalk_votes += (crosswalk_mask > 0).astype(np.uint16)
            crosswalk_valid_frames += 1
        if light_detected:
            light_core_votes += build_light_core_mask(
                image, light_mask, args, np
            ).astype(np.uint16)
            light_mask = close_mask(light_mask, args.light_close_ratio, cv2)
            light_votes += (light_mask > 0).astype(np.uint16)
            light_valid_frames += 1
        log(
            f"[Mask2Former] frame {index}/{len(frames)}: "
            f"crosswalk={'yes' if crosswalk_detected else 'no'}, "
            f"traffic_light={'yes' if light_detected else 'no'}"
        )

    if (
        crosswalk_votes is None
        or light_votes is None
        or light_core_votes is None
        or preview is None
    ):
        raise RoiToolError("Mask2Former did not find a crosswalk in any sampled frame")
    if crosswalk_valid_frames == 0:
        raise RoiToolError("Mask2Former did not find a crosswalk in any sampled frame")
    if light_valid_frames == 0:
        raise RoiToolError("Mask2Former did not find a traffic light in any sampled frame")
    return (
        crosswalk_votes,
        light_votes,
        light_core_votes,
        preview,
        width,
        height,
        crosswalk_valid_frames,
        light_valid_frames,
    )


def build_consensus_mask(
    votes: Any,
    total_frames: int,
    valid_frames: int,
    args: argparse.Namespace,
    cv2: Any,
    np: Any,
) -> tuple[Any, int]:
    valid_ratio = valid_frames / total_frames
    if valid_ratio < args.min_valid_ratio:
        raise RoiToolError(
            f"only {valid_frames}/{total_frames} frames contain a mask; "
            f"required ratio is {args.min_valid_ratio:.2f}"
        )
    required_votes = max(1, math.ceil(valid_frames * args.vote_threshold))
    consensus = np.where(votes >= required_votes, 255, 0).astype(np.uint8)
    consensus = close_mask(consensus, args.close_ratio, cv2)
    small_kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (3, 3))
    consensus = cv2.morphologyEx(consensus, cv2.MORPH_OPEN, small_kernel)
    if not np.any(consensus):
        raise RoiToolError("mask voting produced an empty ROI; lower --vote-threshold if appropriate")
    return consensus, required_votes


def extract_polygon(mask: Any, args: argparse.Namespace, cv2: Any) -> list[tuple[int, int]]:
    contours, _ = cv2.findContours(mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
    if not contours:
        raise RoiToolError("no contour was found in the consensus mask")
    contour = max(contours, key=cv2.contourArea)
    image_area = mask.shape[0] * mask.shape[1]
    area_ratio = cv2.contourArea(contour) / image_area
    if area_ratio < args.min_area_ratio:
        raise RoiToolError(
            f"largest ROI area ratio {area_ratio:.4f} is below {args.min_area_ratio:.4f}"
        )

    # Semantic masks describe painted stripes, while the rule needs the full
    # walkable band including gaps between stripes. The hull is a stable,
    # non-self-intersecting ROI and avoids following narrow connecting pixels.
    contour = cv2.convexHull(contour)
    perimeter = cv2.arcLength(contour, True)
    epsilon = args.epsilon_ratio
    polygon = None
    for _ in range(16):
        candidate = cv2.approxPolyDP(contour, epsilon * perimeter, True).reshape(-1, 2)
        if len(candidate) >= 3:
            polygon = candidate
        if 3 <= len(candidate) <= args.max_points:
            polygon = candidate
            break
        epsilon *= 1.25
    if polygon is None:
        raise RoiToolError("the consensus contour cannot form a valid polygon")
    return [(int(point[0]), int(point[1])) for point in polygon]


def normalized_roi(polygon: list[tuple[int, int]], width: int, height: int) -> tuple[str, list[list[float]]]:
    normalized = [
        [x / max(1, width - 1), y / max(1, height - 1)] for x, y in polygon
    ]
    text = ";".join(f"{x:.6f},{y:.6f}" for x, y in normalized)
    return text, normalized


def boxes_are_near(
    first: tuple[int, int, int, int],
    second: tuple[int, int, int, int],
    gap: int,
) -> bool:
    return not (
        first[2] + gap < second[0]
        or second[2] + gap < first[0]
        or first[3] + gap < second[1]
        or second[3] + gap < first[1]
    )


def merge_nearby_boxes(
    boxes: list[tuple[int, int, int, int]], gap: int
) -> list[tuple[int, int, int, int]]:
    merged = list(boxes)
    changed = True
    while changed:
        changed = False
        for first_index in range(len(merged)):
            for second_index in range(first_index + 1, len(merged)):
                if not boxes_are_near(merged[first_index], merged[second_index], gap):
                    continue
                first = merged[first_index]
                second = merged.pop(second_index)
                merged[first_index] = (
                    min(first[0], second[0]),
                    min(first[1], second[1]),
                    max(first[2], second[2]),
                    max(first[3], second[3]),
                )
                changed = True
                break
            if changed:
                break
    return merged


def normalized_light_box(
    box: tuple[int, int, int, int], width: int, height: int
) -> tuple[float, float, float, float]:
    return (
        box[0] / width,
        box[1] / height,
        box[2] / width,
        box[3] / height,
    )


def build_light_candidates(
    votes: Any,
    total_frames: int,
    polygon: list[tuple[int, int]],
    args: argparse.Namespace,
    cv2: Any,
    np: Any,
) -> tuple[Any, list[LightCandidate], int]:
    height, width = votes.shape[:2]
    required_votes = max(1, math.ceil(total_frames * args.light_vote_threshold))
    consensus = np.where(votes >= required_votes, 255, 0).astype(np.uint8)
    if not np.any(consensus):
        raise RoiToolError(
            "traffic-light voting produced no stable candidate; "
            "lower --light-vote-threshold if the preview frames contain a light"
        )

    component_count, _, stats, _ = cv2.connectedComponentsWithStats(
        consensus, connectivity=8
    )
    image_area = width * height
    raw_boxes: list[tuple[int, int, int, int]] = []
    for component in range(1, component_count):
        area_ratio = int(stats[component, cv2.CC_STAT_AREA]) / image_area
        if not args.light_min_area_ratio <= area_ratio <= args.light_max_area_ratio:
            continue
        left = int(stats[component, cv2.CC_STAT_LEFT])
        top = int(stats[component, cv2.CC_STAT_TOP])
        component_width = int(stats[component, cv2.CC_STAT_WIDTH])
        component_height = int(stats[component, cv2.CC_STAT_HEIGHT])
        raw_boxes.append(
            (left, top, left + component_width, top + component_height)
        )
    if not raw_boxes:
        raise RoiToolError(
            "all stable traffic-light components were rejected by the area limits"
        )

    merge_gap = max(1, round(min(width, height) * args.light_merge_ratio))
    boxes = merge_nearby_boxes(raw_boxes, merge_gap)
    polygon_moments = cv2.moments(np.asarray(polygon, dtype=np.float32))
    if abs(polygon_moments["m00"]) > 1e-6:
        crosswalk_x = polygon_moments["m10"] / polygon_moments["m00"]
        crosswalk_y = polygon_moments["m01"] / polygon_moments["m00"]
    else:
        crosswalk_x = sum(point[0] for point in polygon) / len(polygon)
        crosswalk_y = sum(point[1] for point in polygon) / len(polygon)
    diagonal = math.hypot(width, height)

    scored: list[tuple[tuple[int, int, int, int], int, float, float, float, float]] = []
    for box in boxes:
        support_frames = int(votes[box[1]:box[3], box[0]:box[2]].max())
        mask_area = int(np.count_nonzero(consensus[box[1]:box[3], box[0]:box[2]]))
        area_ratio = mask_area / image_area
        center_x = (box[0] + box[2]) * 0.5
        center_y = (box[1] + box[3]) * 0.5
        crosswalk_distance = math.hypot(
            center_x - crosswalk_x, center_y - crosswalk_y
        ) / diagonal
        persistence = support_frames / total_frames
        proximity = max(0.0, 1.0 - crosswalk_distance)
        area_score = min(1.0, math.sqrt(max(0.0, area_ratio)) / 0.04)
        score = 0.60 * persistence + 0.35 * proximity + 0.05 * area_score
        scored.append(
            (
                box,
                support_frames,
                persistence,
                area_ratio,
                crosswalk_distance,
                score,
            )
        )

    scored.sort(key=lambda item: ((item[0][0] + item[0][2]) * 0.5, item[0][1]))
    candidates = [
        LightCandidate(
            index=index,
            pixel_box=item[0],
            normalized_box=normalized_light_box(item[0], width, height),
            support_frames=item[1],
            persistence=item[2],
            area_ratio=item[3],
            crosswalk_distance=item[4],
            score=item[5],
        )
        for index, item in enumerate(scored)
    ]
    return consensus, candidates, required_votes


def refine_main_light(
    candidate: LightCandidate,
    core_votes: Any,
    args: argparse.Namespace,
    cv2: Any,
    np: Any,
) -> MainLightSelection:
    height, width = core_votes.shape[:2]
    left, top, right, bottom = candidate.pixel_box
    active = (core_votes[top:bottom, left:right] > 0).astype(np.uint8)
    component_count, _, stats, _ = cv2.connectedComponentsWithStats(
        active, connectivity=8
    )
    candidate_area = max(1, (right - left) * (bottom - top))
    minimum_component_area = max(4, round(candidate_area * 0.0004))
    core_boxes: list[tuple[int, int, int, int]] = []
    for component in range(1, component_count):
        if int(stats[component, cv2.CC_STAT_AREA]) < minimum_component_area:
            continue
        component_left = left + int(stats[component, cv2.CC_STAT_LEFT])
        component_top = top + int(stats[component, cv2.CC_STAT_TOP])
        component_width = int(stats[component, cv2.CC_STAT_WIDTH])
        component_height = int(stats[component, cv2.CC_STAT_HEIGHT])
        core_boxes.append(
            (
                component_left,
                component_top,
                component_left + component_width,
                component_top + component_height,
            )
        )

    if not core_boxes:
        selected_box = candidate.pixel_box
        core_refined = False
        core_pixels = 0
    else:
        merge_gap = max(2, round(min(width, height) * args.light_close_ratio))
        core_groups = merge_nearby_boxes(core_boxes, merge_gap)
        selected_box = max(
            core_groups,
            key=lambda box: int(
                np.count_nonzero(core_votes[box[1]:box[3], box[0]:box[2]])
            ),
        )
        core_refined = True
        core_pixels = int(
            np.count_nonzero(
                core_votes[
                    selected_box[1]:selected_box[3],
                    selected_box[0]:selected_box[2],
                ]
            )
        )

    padding = max(2, round(min(width, height) * args.light_padding_ratio))
    selected_box = (
        max(0, selected_box[0] - padding),
        max(0, selected_box[1] - padding),
        min(width, selected_box[2] + padding),
        min(height, selected_box[3] + padding),
    )
    return MainLightSelection(
        candidate=candidate,
        pixel_box=selected_box,
        normalized_box=normalized_light_box(selected_box, width, height),
        core_refined=core_refined,
        core_pixels=core_pixels,
    )


def select_main_light(
    candidates: list[LightCandidate], override_index: int | None
) -> LightCandidate:
    if not candidates:
        raise RoiToolError("no traffic-light candidate is available")
    if override_index is not None:
        for candidate in candidates:
            if candidate.index == override_index:
                return candidate
        raise RoiToolError(
            f"--main-light-index {override_index} is outside the candidate range "
            f"0..{len(candidates) - 1}"
        )
    return max(candidates, key=lambda candidate: candidate.score)


def light_roi_text(selection: MainLightSelection) -> str:
    return ",".join(f"{value:.6f}" for value in selection.normalized_box)


def traffic_roi_config_text(roi_text: str, light_text: str) -> str:
    return (
        "# Fixed-camera normalized regions. Command-line values override these defaults.\n"
        f'roi="{roi_text}"\n'
        f'light_roi="{light_text}"\n'
    )


def board_command(args: argparse.Namespace, roi_text: str, light_text: str) -> str:
    return (
        f"{shlex.quote(args.demo)} {shlex.quote(args.board_model)} \\\n"
        f"  --interval {args.interval} \\\n"
        f'  --roi "{roi_text}" \\\n'
        f'  --light-roi "{light_text}"'
    )


def save_outputs(
    output_dir: Path,
    preview: Any,
    mask: Any,
    light_mask: Any,
    polygon: list[tuple[int, int]],
    light_candidates: list[LightCandidate],
    main_light: MainLightSelection,
    metadata: dict[str, Any],
    cv2: Any,
    np: Any,
) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    overlay = preview.copy()
    points = np.asarray(polygon, dtype=np.int32)
    filled = preview.copy()
    cv2.fillPoly(filled, [points], (255, 96, 32))
    cv2.addWeighted(filled, 0.28, overlay, 0.72, 0.0, overlay)
    cv2.polylines(overlay, [points], True, (255, 160, 32), 4, cv2.LINE_AA)
    label_x = max(12, int(points[:, 0].min()) + 12)
    label_y = max(30, int(points[:, 1].min()) - 12)
    cv2.putText(
        overlay, "MASK2FORMER CROSSWALK ROI", (label_x, label_y), cv2.FONT_HERSHEY_SIMPLEX,
        0.8, (255, 255, 255), 2, cv2.LINE_AA,
    )
    if not cv2.imwrite(str(output_dir / "crosswalk_roi_preview.jpg"), overlay):
        raise RoiToolError("failed to write crosswalk_roi_preview.jpg")
    if not cv2.imwrite(str(output_dir / "crosswalk_consensus_mask.png"), mask):
        raise RoiToolError("failed to write crosswalk_consensus_mask.png")
    light_overlay = overlay.copy()
    for candidate in light_candidates:
        left, top, right, bottom = candidate.pixel_box
        selected = candidate.index == main_light.candidate.index
        color = (0, 165, 255) if selected else (0, 210, 255)
        thickness = 2
        cv2.rectangle(
            light_overlay, (left, top), (right - 1, bottom - 1),
            color, thickness, cv2.LINE_AA,
        )
        label = (
            f"LIGHT #{candidate.index}"
            f"{' SELECTED' if selected else ''} score={candidate.score:.3f}"
        )
        cv2.putText(
            light_overlay, label, (left, max(24, top - 8)),
            cv2.FONT_HERSHEY_SIMPLEX, 0.62, color, 2, cv2.LINE_AA,
        )
    left, top, right, bottom = main_light.pixel_box
    cv2.rectangle(
        light_overlay, (left, top), (right - 1, bottom - 1),
        (32, 32, 255), 4, cv2.LINE_AA,
    )
    core_label = (
        f"MAIN CORE #{main_light.candidate.index}"
        if main_light.core_refined
        else f"MAIN FALLBACK #{main_light.candidate.index}"
    )
    cv2.putText(
        light_overlay, core_label, (left, max(24, bottom + 24)),
        cv2.FONT_HERSHEY_SIMPLEX, 0.62, (32, 32, 255), 2, cv2.LINE_AA,
    )
    if not cv2.imwrite(
        str(output_dir / "traffic_light_roi_preview.jpg"), light_overlay
    ):
        raise RoiToolError("failed to write traffic_light_roi_preview.jpg")
    if not cv2.imwrite(
        str(output_dir / "traffic_light_consensus_mask.png"), light_mask
    ):
        raise RoiToolError("failed to write traffic_light_consensus_mask.png")
    metadata_text = json.dumps(metadata, ensure_ascii=False, indent=2) + "\n"
    (output_dir / "traffic_scene_roi.json").write_text(
        metadata_text, encoding="utf-8"
    )
    (output_dir / "main_traffic_light_roi.txt").write_text(
        light_roi_text(main_light) + "\n", encoding="utf-8"
    )
    (output_dir / "traffic_roi.conf").write_text(
        traffic_roi_config_text(
            metadata["crosswalk"]["roi"],
            metadata["traffic_lights"]["main_roi"],
        ),
        encoding="utf-8",
    )
    # Keep the original filename for existing calibration workflows.
    (output_dir / "crosswalk_roi.json").write_text(metadata_text, encoding="utf-8")


def run(args: argparse.Namespace) -> str:
    video = require_file(args.video, "video")
    ffmpeg = resolve_executable(args.ffmpeg)
    output_dir = args.output_dir
    if output_dir is None:
        demo_root = Path(__file__).resolve().parents[1]
        output_dir = demo_root / "results" / "mask2former_roi" / video.stem
    output_dir = output_dir.expanduser().resolve()

    cv2, np, torch, image_type, processor_type, model_type = load_inference_runtime()
    device = resolve_device(args.device, torch)
    segmenter = Mask2FormerSegmenter(
        device, cv2, np, torch, image_type, processor_type, model_type
    )
    with tempfile.TemporaryDirectory(prefix="mask2former_crosswalk_roi_") as temp:
        frames = extract_frames(args, ffmpeg, video, Path(temp))
        log(f"[FFmpeg] extracted {len(frames)} frame(s)")
        (
            crosswalk_votes,
            light_votes,
            light_core_votes,
            preview,
            width,
            height,
            valid_frames,
            light_valid_frames,
        ) = collect_votes(frames, segmenter, args, cv2, np)
        mask, required_votes = build_consensus_mask(
            crosswalk_votes, len(frames), valid_frames, args, cv2, np
        )

    polygon = extract_polygon(mask, args, cv2)
    roi_text, normalized = normalized_roi(polygon, width, height)
    light_mask, light_candidates, light_required_votes = build_light_candidates(
        light_votes, len(frames), polygon, args, cv2, np
    )
    main_candidate = select_main_light(light_candidates, args.main_light_index)
    main_light = refine_main_light(
        main_candidate, light_core_votes, args, cv2, np
    )
    main_light_text = light_roi_text(main_light)
    command = board_command(args, roi_text, main_light_text)
    metadata = {
        "video": str(video),
        "mask2former_model": MASK2FORMER_MODEL,
        "crosswalk_label_ids": segmenter.crosswalk_ids,
        "traffic_light_label_ids": segmenter.traffic_light_ids,
        "source_size": [width, height],
        "sampled_frames": len(frames),
        "crosswalk": {
            "valid_frames": valid_frames,
            "required_pixel_votes": required_votes,
            "vote_threshold": args.vote_threshold,
            "pixel_polygon": [[x, y] for x, y in polygon],
            "normalized_polygon": normalized,
            "roi": roi_text,
        },
        "traffic_lights": {
            "valid_frames": light_valid_frames,
            "required_pixel_votes": light_required_votes,
            "vote_threshold": args.light_vote_threshold,
            "selection": (
                "manual_override" if args.main_light_index is not None else "automatic"
            ),
            "automatic_score_weights": {
                "persistence": 0.60,
                "crosswalk_proximity": 0.35,
                "area": 0.05,
            },
            "candidates": [
                {
                    "index": candidate.index,
                    "pixel_box": list(candidate.pixel_box),
                    "normalized_box": list(candidate.normalized_box),
                    "support_frames": candidate.support_frames,
                    "persistence": candidate.persistence,
                    "area_ratio": candidate.area_ratio,
                    "crosswalk_distance": candidate.crosswalk_distance,
                    "score": candidate.score,
                    "selected": candidate.index == main_light.candidate.index,
                }
                for candidate in light_candidates
            ],
            "main_index": main_light.candidate.index,
            "main_semantic_pixel_box": list(main_light.candidate.pixel_box),
            "main_pixel_box": list(main_light.pixel_box),
            "main_normalized_box": list(main_light.normalized_box),
            "main_roi": main_light_text,
            "core_refined": main_light.core_refined,
            "core_pixels": main_light.core_pixels,
            "core_min_value": args.light_core_min_value,
            "core_min_saturation": args.light_core_min_saturation,
        },
        "board_command": command,
    }
    save_outputs(
        output_dir,
        preview,
        mask,
        light_mask,
        polygon,
        light_candidates,
        main_light,
        metadata,
        cv2,
        np,
    )
    log(f"[Output] {output_dir}")
    log(
        f"[Main light] candidate #{main_light.candidate.index}: "
        f"{main_light_text} (score={main_light.candidate.score:.3f}, "
        f"core_refined={'yes' if main_light.core_refined else 'no'})"
    )
    return command


def main() -> int:
    try:
        print(run(parse_args()))
        return 0
    except RoiToolError as exc:
        log(f"ERROR: {exc}")
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
