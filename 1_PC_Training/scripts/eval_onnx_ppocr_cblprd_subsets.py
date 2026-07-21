#!/usr/bin/env python3
"""Evaluate a fixed-shape PP-OCR ONNX recognizer on CBLPRD subsets."""

from __future__ import annotations

import argparse
import csv
import importlib.util
import math
import sys
import time
from dataclasses import dataclass
from pathlib import Path

import cv2
import numpy as np
import onnxruntime as ort


IMAGE_SHAPE = (3, 48, 160)
SUBSETS = (
    ("basic", "val_basic.txt"),
    ("hard", "val_hard.txt"),
    ("使", "val_special_使.txt"),
    ("学", "val_special_学.txt"),
    ("港", "val_special_港.txt"),
    ("澳", "val_special_澳.txt"),
    ("警", "val_special_警.txt"),
    ("领", "val_special_领.txt"),
)


@dataclass
class AccuracyStats:
    samples: int = 0
    raw_correct: int = 0
    rule_correct: int = 0
    changed: int = 0
    fixed: int = 0
    harmed: int = 0

    def update(self, label: str, raw_prediction: str,
               rule_prediction: str) -> tuple[bool, bool]:
        raw_correct = raw_prediction == label
        rule_correct = rule_prediction == label
        self.samples += 1
        self.raw_correct += int(raw_correct)
        self.rule_correct += int(rule_correct)
        self.changed += int(raw_prediction != rule_prediction)
        self.fixed += int(not raw_correct and rule_correct)
        self.harmed += int(raw_correct and not rule_correct)
        return raw_correct, rule_correct

    def merge(self, other: "AccuracyStats") -> None:
        for field in ("samples", "raw_correct", "rule_correct", "changed",
                      "fixed", "harmed"):
            setattr(self, field, getattr(self, field) + getattr(other, field))


def find_paddleocr_root() -> Path:
    script_parent = Path(__file__).resolve().parents[1]
    candidates = (script_parent, script_parent / "PaddleOCR")
    for candidate in candidates:
        if (candidate / "ppocr" / "metrics" / "plate_rule.py").is_file():
            return candidate
    checked = ", ".join(str(candidate) for candidate in candidates)
    raise FileNotFoundError(f"PaddleOCR root not found; checked: {checked}")


def parse_args() -> argparse.Namespace:
    paddleocr_root = find_paddleocr_root()
    parser = argparse.ArgumentParser(
        description="Evaluate PP-OCR ONNX raw and plate-rule accuracies.")
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--manifest-dir", type=Path, required=True)
    parser.add_argument(
        "--data-dir",
        type=Path,
        required=True,
        help="Base directory joined with image paths stored in manifests.")
    parser.add_argument(
        "--character-dict",
        type=Path,
        default=paddleocr_root / "ppocr" / "utils" /
        "cblprd_plate_dict.txt")
    parser.add_argument(
        "--provider", choices=("auto", "cpu", "cuda"), default="auto")
    parser.add_argument("--progress-step", type=int, default=500)
    parser.add_argument(
        "--save-details",
        type=Path,
        help="Optional TSV containing every label and decoded prediction.")
    args = parser.parse_args()
    if args.progress_step < 0:
        parser.error("--progress-step must be zero or greater")
    return args


def load_plate_rule():
    rule_path = find_paddleocr_root() / "ppocr" / "metrics" / "plate_rule.py"
    spec = importlib.util.spec_from_file_location("cblprd_plate_rule", rule_path)
    if spec is None or spec.loader is None:
        raise ImportError(f"Cannot load plate rule module: {rule_path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.normalize_plate_prediction, module.NORMALIZATION_VERSION


def load_characters(path: Path) -> list[str]:
    characters = path.read_text(encoding="utf-8").splitlines()
    if not characters or any(not character for character in characters):
        raise ValueError(f"Invalid character dictionary: {path}")
    if len(set(characters)) != len(characters):
        raise ValueError(f"Duplicate characters in dictionary: {path}")
    return characters


def select_providers(name: str) -> list[str]:
    available = ort.get_available_providers()
    if name == "cpu":
        return ["CPUExecutionProvider"]
    if name == "cuda":
        if "CUDAExecutionProvider" not in available:
            raise RuntimeError(
                f"CUDAExecutionProvider is unavailable; available={available}")
        return ["CUDAExecutionProvider", "CPUExecutionProvider"]
    if "CUDAExecutionProvider" in available:
        return ["CUDAExecutionProvider", "CPUExecutionProvider"]
    return ["CPUExecutionProvider"]


def create_session(model_path: Path, provider: str) -> ort.InferenceSession:
    if not model_path.is_file():
        raise FileNotFoundError(f"ONNX model not found: {model_path}")
    session = ort.InferenceSession(
        str(model_path), providers=select_providers(provider))
    if len(session.get_inputs()) != 1 or len(session.get_outputs()) != 1:
        raise ValueError("Expected exactly one ONNX input and one output")
    input_shape = session.get_inputs()[0].shape
    if input_shape != list((1,) + IMAGE_SHAPE):
        raise ValueError(
            f"Expected input shape [1, 3, 48, 160], got {input_shape}")
    return session


def read_image(path: Path) -> np.ndarray:
    encoded = np.fromfile(path, dtype=np.uint8)
    image = cv2.imdecode(encoded, cv2.IMREAD_COLOR)
    if image is None:
        raise ValueError(f"Cannot decode image: {path}")
    return image


def preprocess(image: np.ndarray) -> np.ndarray:
    channels, target_height, target_width = IMAGE_SHAPE
    height, width = image.shape[:2]
    if height <= 0 or width <= 0 or image.ndim != 3 or image.shape[2] != 3:
        raise ValueError(f"Invalid BGR image shape: {image.shape}")
    resized_width = min(target_width,
                        int(math.ceil(target_height * width / float(height))))
    resized = cv2.resize(
        image, (resized_width, target_height), interpolation=cv2.INTER_LINEAR)
    normalized = resized.astype(np.float32).transpose((2, 0, 1)) / 255.0
    normalized = (normalized - 0.5) / 0.5
    padded = np.zeros((channels, target_height, target_width), dtype=np.float32)
    padded[:, :, :resized_width] = normalized
    return padded[np.newaxis, ...]


def decode_ctc(prediction: np.ndarray,
               characters: list[str]) -> tuple[str, float]:
    if prediction.ndim != 2:
        raise ValueError(f"Expected [time, classes], got {prediction.shape}")
    expected_classes = len(characters) + 1
    if prediction.shape[1] != expected_classes:
        raise ValueError(
            f"Expected {expected_classes} classes, got {prediction.shape[1]}")

    indices = prediction.argmax(axis=1)
    probabilities = prediction.max(axis=1)
    decoded = []
    confidence = []
    previous = None
    for index, probability in zip(indices.tolist(), probabilities.tolist()):
        if index != previous and index != 0:
            decoded.append(characters[index - 1])
            confidence.append(probability)
        previous = index
    return "".join(decoded), float(np.mean(confidence)) if confidence else 0.0


def read_manifest(path: Path, data_dir: Path):
    if not path.is_file():
        raise FileNotFoundError(f"Validation manifest not found: {path}")
    for line_number, line in enumerate(
            path.read_text(encoding="utf-8").splitlines(), start=1):
        if not line:
            continue
        fields = line.split("\t", 1)
        if len(fields) != 2 or not fields[0] or not fields[1]:
            raise ValueError(f"Invalid manifest row {path}:{line_number}")
        image_path = Path(fields[0])
        if not image_path.is_absolute():
            image_path = data_dir / image_path
        if not image_path.is_file():
            raise FileNotFoundError(
                f"Image not found at {path}:{line_number}: {image_path}")
        yield fields[0], image_path, fields[1]


def print_header() -> None:
    print(f"{'subset':<12} {'samples':>10} {'raw_correct':>12} "
          f"{'raw_accuracy':>14} {'rule_correct':>13} "
          f"{'rule_accuracy':>14} {'changed':>8} {'fixed':>8} {'harmed':>8}")
    print(f"{'------------':<12} {'----------':>10} {'------------':>12} "
          f"{'--------------':>14} {'-------------':>13} "
          f"{'--------------':>14} {'--------':>8} {'--------':>8} "
          f"{'--------':>8}")


def print_stats(name: str, stats: AccuracyStats) -> None:
    raw_accuracy = stats.raw_correct / stats.samples if stats.samples else 0.0
    rule_accuracy = stats.rule_correct / stats.samples if stats.samples else 0.0
    print(f"{name:<12} {stats.samples:>10d} {stats.raw_correct:>12d} "
          f"{raw_accuracy:>13.4%} {stats.rule_correct:>13d} "
          f"{rule_accuracy:>13.4%} {stats.changed:>8d} {stats.fixed:>8d} "
          f"{stats.harmed:>8d}")


def main() -> None:
    args = parse_args()
    normalize_plate_prediction, rule_version = load_plate_rule()
    characters = load_characters(args.character_dict.resolve())
    session = create_session(args.model.resolve(), args.provider)
    input_name = session.get_inputs()[0].name
    total = AccuracyStats()
    started = time.perf_counter()

    details_file = None
    details_writer = None
    if args.save_details:
        args.save_details.parent.mkdir(parents=True, exist_ok=True)
        details_file = args.save_details.open("w", encoding="utf-8", newline="")
        details_writer = csv.writer(details_file, delimiter="\t")
        details_writer.writerow((
            "subset", "image", "label", "raw_prediction", "rule_prediction",
            "confidence", "raw_correct", "rule_correct"))

    print(f"model: {args.model.resolve()}")
    print(f"provider: {session.get_providers()[0]}")
    print(f"rule: {rule_version}")
    print_header()
    try:
        for subset, manifest_name in SUBSETS:
            stats = AccuracyStats()
            manifest = args.manifest_dir.resolve() / manifest_name
            for relative_path, image_path, label in read_manifest(
                    manifest, args.data_dir.resolve()):
                input_tensor = preprocess(read_image(image_path))
                output = session.run(None, {input_name: input_tensor})[0]
                if output.shape[0] != 1:
                    raise ValueError(f"Expected output batch 1, got {output.shape}")
                raw_prediction, confidence = decode_ctc(output[0], characters)
                rule_prediction = normalize_plate_prediction(raw_prediction)
                raw_correct, rule_correct = stats.update(
                    label, raw_prediction, rule_prediction)
                if details_writer:
                    details_writer.writerow((
                        subset, relative_path, label, raw_prediction,
                        rule_prediction, f"{confidence:.8f}", int(raw_correct),
                        int(rule_correct)))
                if args.progress_step and stats.samples % args.progress_step == 0:
                    print(
                        f"[{subset}] processed {stats.samples}", file=sys.stderr)
            print_stats(subset, stats)
            total.merge(stats)
        print_stats("TOTAL", total)
    finally:
        if details_file:
            details_file.close()

    elapsed = time.perf_counter() - started
    fps = total.samples / elapsed if elapsed else 0.0
    print(f"elapsed: {elapsed:.2f}s, end-to-end throughput: {fps:.2f} images/s")
    if args.save_details:
        print(f"details: {args.save_details.resolve()}")


if __name__ == "__main__":
    main()
