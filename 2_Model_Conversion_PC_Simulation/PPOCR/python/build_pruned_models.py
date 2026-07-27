#!/usr/bin/env python3
"""Build the P0 FP16 and P1 MMSE INT8 pruned RKNN models."""

from __future__ import annotations

import argparse
import tempfile
from pathlib import Path
from typing import Any

from convert import (
    DEFAULT_CHARACTER_DICT,
    INPUT_SHAPE,
    MODEL_DIR,
    OUTPUT_SHAPE,
    QUANT_SAMPLE_RANGES,
    SUPPORTED_PLATFORMS,
    check_return_code,
    load_characters,
    validate_dataset,
    validate_onnx,
)


P0_LABEL = "P0 pruned FP16"
P1_LABEL = "P1 pruned MMSE INT8"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Build two losslessly pruned RKNN models from the fixed "
            "[1,3,48,160] PP-OCRv4 ONNX model: P0 FP16 and P1 MMSE INT8."
        )
    )
    parser.add_argument("onnx_model", type=Path)
    parser.add_argument("platform", choices=SUPPORTED_PLATFORMS)
    parser.add_argument(
        "--dataset",
        type=Path,
        required=True,
        help="Calibration dataset.txt used by P1 MMSE INT8.",
    )
    parser.add_argument(
        "--character-dict",
        type=Path,
        default=DEFAULT_CHARACTER_DICT,
        help="73-character dictionary used to verify the 74-class CTC output.",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=MODEL_DIR,
        help="Directory for both generated RKNN files (default: PPOCR/model).",
    )
    parser.add_argument(
        "--validate-only",
        action="store_true",
        help="Validate all inputs without importing RKNN Toolkit2 or building models.",
    )
    parser.add_argument(
        "--overwrite",
        action="store_true",
        help="Allow replacing the exact P0/P1 output files if they already exist.",
    )
    parser.add_argument("--verbose", action="store_true")
    return parser.parse_args()


def output_paths(
    onnx_model: Path, platform: str, output_dir: Path
) -> tuple[Path, Path]:
    stem = onnx_model.stem
    p0 = output_dir / f"{stem}_{platform}_fp16_pruned.rknn"
    p1 = output_dir / f"{stem}_{platform}_mmse_i8_pruned.rknn"
    return p0.resolve(), p1.resolve()


def check_output_targets(paths: tuple[Path, Path], overwrite: bool) -> None:
    if paths[0] == paths[1]:
        raise ValueError("P0 and P1 output paths must be different")
    for path in paths:
        if path.suffix.lower() != ".rknn":
            raise ValueError(f"RKNN output must use the .rknn suffix: {path}")
        if path.exists() and not path.is_file():
            raise IsADirectoryError(f"RKNN output target is not a file: {path}")
        if path.exists() and not overwrite:
            raise FileExistsError(
                f"RKNN output already exists: {path}; pass --overwrite to replace it"
            )


def write_resolved_dataset(path: Path, images: list[Path]) -> None:
    with path.open("w", encoding="utf-8", newline="\n") as file_handle:
        file_handle.writelines(f"{image.as_posix()}\n" for image in images)


def build_variant(
    rknn_class: Any,
    *,
    label: str,
    onnx_model: Path,
    platform: str,
    output_path: Path,
    verbose: bool,
    dataset: Path | None,
) -> Path:
    quantized = dataset is not None
    rknn = rknn_class(verbose=verbose)
    try:
        config: dict[str, Any] = {
            "target_platform": platform,
            "mean_values": [[127.5, 127.5, 127.5]],
            "std_values": [[127.5, 127.5, 127.5]],
            "quant_img_RGB2BGR": True,
            "model_pruning": True,
        }
        if quantized:
            config.update(
                quantized_algorithm="mmse",
                quantized_method="channel",
            )

        print(f"\n==> {label}: config")
        check_return_code("rknn.config", rknn.config(**config))

        print(f"==> {label}: load ONNX")
        check_return_code(
            "rknn.load_onnx", rknn.load_onnx(model=str(onnx_model))
        )

        print(f"==> {label}: build")
        build_options: dict[str, Any] = {"do_quantization": quantized}
        if dataset is not None:
            build_options["dataset"] = str(dataset)
        check_return_code("rknn.build", rknn.build(**build_options))

        output_path.parent.mkdir(parents=True, exist_ok=True)
        print(f"==> {label}: export")
        check_return_code("rknn.export_rknn", rknn.export_rknn(str(output_path)))
    finally:
        rknn.release()

    if not output_path.is_file() or output_path.stat().st_size == 0:
        raise RuntimeError(f"RKNN export did not create a valid file: {output_path}")
    print(f"done: {output_path}")
    return output_path


def import_rknn() -> Any:
    try:
        from rknn.api import RKNN
    except ImportError as exc:
        raise RuntimeError(
            "RKNN Toolkit2 is unavailable. Run this script in the Ubuntu v2.3.0 "
            "conversion environment."
        ) from exc
    return RKNN


def main() -> None:
    args = parse_args()
    onnx_model = args.onnx_model.resolve()
    character_dict = args.character_dict.resolve()
    dataset_file = args.dataset.resolve()
    output_dir = args.output_dir.resolve()

    characters = load_characters(character_dict)
    input_name, output_name = validate_onnx(onnx_model, len(characters))
    dataset_images = validate_dataset(dataset_file)
    outputs = output_paths(onnx_model, args.platform, output_dir)
    check_output_targets(outputs, args.overwrite)

    minimum, maximum = QUANT_SAMPLE_RANGES["mmse"]
    if not minimum <= len(dataset_images) <= maximum:
        print(
            f"WARNING: MMSE is normally calibrated with {minimum}-{maximum} images; "
            f"current dataset has {len(dataset_images)}."
        )

    print(f"ONNX: {onnx_model}")
    print(f"input: {input_name} {INPUT_SHAPE}")
    print(f"output: {output_name} {OUTPUT_SHAPE}")
    print(f"dictionary: {character_dict} ({len(characters)} characters)")
    print(f"calibration: {dataset_file} ({len(dataset_images)} images, MMSE/channel)")
    print(f"P0 output: {outputs[0]}")
    print(f"P1 output: {outputs[1]}")

    if args.validate_only:
        print("Validation passed; P0/P1 RKNN construction was not started.")
        return

    rknn_class = import_rknn()
    with tempfile.TemporaryDirectory(prefix="ppocr_pruning_dataset_") as temp_dir:
        resolved_dataset = Path(temp_dir) / "dataset.txt"
        write_resolved_dataset(resolved_dataset, dataset_images)

        build_variant(
            rknn_class,
            label=P0_LABEL,
            onnx_model=onnx_model,
            platform=args.platform,
            output_path=outputs[0],
            verbose=args.verbose,
            dataset=None,
        )
        build_variant(
            rknn_class,
            label=P1_LABEL,
            onnx_model=onnx_model,
            platform=args.platform,
            output_path=outputs[1],
            verbose=args.verbose,
            dataset=resolved_dataset,
        )

    print("\nP0/P1 pruning build completed.")
    print(f"P0: {outputs[0]}")
    print(f"P1: {outputs[1]}")


if __name__ == "__main__":
    main()
