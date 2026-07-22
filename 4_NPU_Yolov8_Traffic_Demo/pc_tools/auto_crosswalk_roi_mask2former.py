#!/usr/bin/env python3
"""Generate a stable crosswalk ROI with Mapillary Mask2Former."""

from __future__ import annotations

import argparse
import json
import math
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any


MASK2FORMER_MODEL = "facebook/mask2former-swin-large-mapillary-vistas-semantic"
TARGET_LABEL_NAMES = frozenset({"crosswalk - plain", "lane marking - crosswalk"})


class RoiToolError(RuntimeError):
    """Expected command-line or inference failure."""


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


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Use local FFmpeg and Mapillary Mask2Former segmentation to generate a "
            "normalized crosswalk ROI and the RK3568 demo command."
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
        self.target_ids = sorted(
            label_id
            for label_id, name in self.id2label.items()
            if name.casefold() in TARGET_LABEL_NAMES
        )
        if not self.target_ids:
            raise RoiToolError("Mask2Former model does not contain Mapillary crosswalk labels")
        labels = [self.id2label[label_id] for label_id in self.target_ids]
        log(f"[Mask2Former] target labels: {dict(zip(self.target_ids, labels))}")

    def segment(self, image: Any) -> Any:
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
        labels = semantic.detach().cpu().numpy()
        return self.np.isin(labels, self.target_ids).astype(self.np.uint8) * 255


def close_mask(mask: Any, close_ratio: float, cv2: Any) -> Any:
    short_edge = min(mask.shape[:2])
    kernel_size = max(3, int(round(short_edge * close_ratio)))
    if kernel_size % 2 == 0:
        kernel_size += 1
    kernel = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (kernel_size, kernel_size))
    return cv2.morphologyEx(mask, cv2.MORPH_CLOSE, kernel)


def collect_votes(
    frames: list[Path], segmenter: Mask2FormerSegmenter, args: argparse.Namespace, cv2: Any, np: Any
) -> tuple[Any, Any, int, int, int]:
    votes = None
    preview = None
    width = 0
    height = 0
    valid_frames = 0

    for index, frame in enumerate(frames, start=1):
        image = cv2.imread(str(frame), cv2.IMREAD_COLOR)
        if image is None:
            raise RoiToolError(f"cannot read extracted frame: {frame}")
        frame_height, frame_width = image.shape[:2]
        if votes is None:
            width, height = frame_width, frame_height
            votes = np.zeros((height, width), dtype=np.uint16)
        elif (frame_width, frame_height) != (width, height):
            raise RoiToolError("extracted frames do not have a consistent resolution")

        mask = segmenter.segment(image)
        detected = bool(np.any(mask))
        if detected:
            mask = close_mask(mask, args.close_ratio, cv2)
            votes += (mask > 0).astype(np.uint16)
            valid_frames += 1
            if preview is None:
                preview = image.copy()
        log(f"[Mask2Former] frame {index}/{len(frames)}: {'mask' if detected else 'no mask'}")

    if votes is None or preview is None:
        raise RoiToolError("Mask2Former did not find a crosswalk in any sampled frame")
    return votes, preview, width, height, valid_frames


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


def board_command(args: argparse.Namespace, roi_text: str) -> str:
    return (
        f"{shlex.quote(args.demo)} {shlex.quote(args.board_model)} \\\n"
        f"  --interval {args.interval} \\\n"
        f'  --roi "{roi_text}"'
    )


def save_outputs(
    output_dir: Path,
    preview: Any,
    mask: Any,
    polygon: list[tuple[int, int]],
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
    (output_dir / "crosswalk_roi.json").write_text(
        json.dumps(metadata, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )


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
        votes, preview, width, height, valid_frames = collect_votes(
            frames, segmenter, args, cv2, np
        )
        mask, required_votes = build_consensus_mask(
            votes, len(frames), valid_frames, args, cv2, np
        )

    polygon = extract_polygon(mask, args, cv2)
    roi_text, normalized = normalized_roi(polygon, width, height)
    command = board_command(args, roi_text)
    metadata = {
        "video": str(video),
        "mask2former_model": MASK2FORMER_MODEL,
        "target_label_ids": segmenter.target_ids,
        "source_size": [width, height],
        "sampled_frames": len(frames),
        "valid_frames": valid_frames,
        "required_pixel_votes": required_votes,
        "vote_threshold": args.vote_threshold,
        "pixel_polygon": [[x, y] for x, y in polygon],
        "normalized_polygon": normalized,
        "roi": roi_text,
        "board_command": command,
    }
    save_outputs(output_dir, preview, mask, polygon, metadata, cv2, np)
    log(f"[Output] {output_dir}")
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
