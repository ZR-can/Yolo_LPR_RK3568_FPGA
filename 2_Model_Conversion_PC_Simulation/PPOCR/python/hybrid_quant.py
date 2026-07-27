#!/usr/bin/env python3
"""Run the two-stage RKNN Toolkit2 hybrid quantization workflow."""

from __future__ import annotations

import argparse
import os
import shutil
from pathlib import Path

from convert import (
    DEFAULT_CHARACTER_DICT,
    QUANT_SAMPLE_RANGES,
    SUPPORTED_PLATFORMS,
    check_return_code,
    load_characters,
    validate_dataset,
    validate_onnx,
)


SCRIPT_DIR = Path(__file__).resolve().parent
MODEL_DIR = SCRIPT_DIR.parent / "model"


def add_common_options(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--verbose", action="store_true")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Generate a manually configured hybrid-quantized RKNN model using "
            "RKNN Toolkit2 v2.3.0."
        )
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    step1 = subparsers.add_parser(
        "step1",
        help="Generate .model, .data, and editable quantization configuration files.",
    )
    step1.add_argument("onnx_model", type=Path)
    step1.add_argument("platform", choices=SUPPORTED_PLATFORMS)
    step1.add_argument("--dataset", type=Path, required=True)
    step1.add_argument(
        "--work-dir",
        type=Path,
        help="Must be absent or empty; defaults to model/<onnx>_<platform>_hybrid.",
    )
    step1.add_argument(
        "--character-dict",
        type=Path,
        default=DEFAULT_CHARACTER_DICT,
    )
    step1.add_argument(
        "--quantized-algorithm",
        choices=tuple(QUANT_SAMPLE_RANGES),
        default="normal",
    )
    step1.add_argument(
        "--quantized-method",
        choices=("channel", "layer"),
        default="channel",
    )
    add_common_options(step1)

    step2 = subparsers.add_parser(
        "step2",
        help="Build and export RKNN from the Step1 artifacts and an edited config.",
    )
    step2.add_argument("work_dir", type=Path)
    step2.add_argument("output_rknn", type=Path)
    step2.add_argument(
        "--config",
        type=Path,
        help="Defaults to the Step1-generated *.quantization.manual.cfg.",
    )
    step2.add_argument(
        "--accuracy-analysis-input",
        type=Path,
        help="Optional preprocessed 48x160 image for layer error analysis.",
    )
    step2.add_argument(
        "--accuracy-analysis-output",
        type=Path,
        help="Defaults to <work-dir>/<output-stem>_accuracy_analysis.",
    )
    step2.add_argument(
        "--overwrite",
        action="store_true",
        help="Allow replacing only the requested RKNN output file.",
    )
    add_common_options(step2)
    return parser.parse_args()


def import_rknn():
    try:
        from rknn.api import RKNN
    except ImportError as exc:
        raise RuntimeError(
            "RKNN Toolkit2 is unavailable. Run this script in the Ubuntu v2.3.0 environment."
        ) from exc
    return RKNN


def require_empty_work_dir(path: Path) -> Path:
    path = path.resolve()
    if path.exists() and not path.is_dir():
        raise NotADirectoryError(f"Hybrid work path is not a directory: {path}")
    if path.exists() and any(path.iterdir()):
        raise FileExistsError(
            f"Hybrid work directory is not empty: {path}; use a new directory "
            "to avoid mixing partial Toolkit artifacts"
        )
    path.mkdir(parents=True, exist_ok=True)
    return path


def write_resolved_dataset(work_dir: Path, images: list[Path]) -> Path:
    output = work_dir / "calibration_dataset.resolved.txt"
    with output.open("w", encoding="utf-8", newline="\n") as file_handle:
        file_handle.writelines(f"{image.as_posix()}\n" for image in images)
    return output


def run_step1(args: argparse.Namespace) -> None:
    onnx_model = args.onnx_model.resolve()
    character_dict = args.character_dict.resolve()
    dataset = args.dataset.resolve()

    characters = load_characters(character_dict)
    input_name, output_name = validate_onnx(onnx_model, len(characters))
    dataset_images = validate_dataset(dataset)
    work_dir = require_empty_work_dir(
        args.work_dir
        if args.work_dir is not None
        else MODEL_DIR / f"{onnx_model.stem}_{args.platform}_hybrid"
    )
    minimum, maximum = QUANT_SAMPLE_RANGES[args.quantized_algorithm]
    if not minimum <= len(dataset_images) <= maximum:
        print(
            f"WARNING: {args.quantized_algorithm} is normally calibrated with "
            f"{minimum}-{maximum} images; current dataset has {len(dataset_images)}."
        )
    resolved_dataset = write_resolved_dataset(work_dir, dataset_images)

    print(f"ONNX: {onnx_model}")
    print(f"input: {input_name}")
    print(f"output: {output_name}")
    print(f"dictionary: {character_dict} ({len(characters)} characters)")
    print(f"calibration: {len(dataset_images)} images")
    print(f"hybrid work directory: {work_dir}")
    print("automatic proposal: disabled (manual configuration)")

    RKNN = import_rknn()
    previous_cwd = Path.cwd()
    rknn = RKNN(verbose=args.verbose)
    try:
        check_return_code(
            "rknn.config",
            rknn.config(
                target_platform=args.platform,
                mean_values=[[127.5, 127.5, 127.5]],
                std_values=[[127.5, 127.5, 127.5]],
                quant_img_RGB2BGR=True,
                quantized_algorithm=args.quantized_algorithm,
                quantized_method=args.quantized_method,
            ),
        )
        check_return_code(
            "rknn.load_onnx", rknn.load_onnx(model=str(onnx_model))
        )
        os.chdir(work_dir)
        check_return_code(
            "rknn.hybrid_quantization_step1",
            rknn.hybrid_quantization_step1(
                dataset=str(resolved_dataset), proposal=False
            ),
        )
    finally:
        os.chdir(previous_cwd)
        rknn.release()

    model_path = work_dir / f"{onnx_model.stem}.model"
    data_path = work_dir / f"{onnx_model.stem}.data"
    generated_config = work_dir / f"{onnx_model.stem}.quantization.cfg"
    for artifact in (model_path, data_path, generated_config):
        if not artifact.is_file() or artifact.stat().st_size == 0:
            raise RuntimeError(f"Step1 output is missing or empty: {artifact}")

    editable_config = work_dir / f"{onnx_model.stem}.quantization.manual.cfg"
    shutil.copy2(generated_config, editable_config)
    print("Step1 completed:")
    print(f"  model: {model_path}")
    print(f"  data: {data_path}")
    print(f"  generated config: {generated_config}")
    print(f"  editable config: {editable_config}")


def discover_step1_artifacts(work_dir: Path) -> tuple[Path, Path, Path]:
    models = sorted(work_dir.glob("*.model"))
    if len(models) != 1:
        raise RuntimeError(
            f"Expected exactly one .model file in {work_dir}, found {len(models)}"
        )
    model_path = models[0]
    data_path = model_path.with_suffix(".data")
    manual_config = work_dir / f"{model_path.stem}.quantization.manual.cfg"
    for artifact in (model_path, data_path, manual_config):
        if not artifact.is_file() or artifact.stat().st_size == 0:
            raise FileNotFoundError(f"Required Step1 artifact is missing: {artifact}")
    return model_path, data_path, manual_config


def run_step2(args: argparse.Namespace) -> None:
    work_dir = args.work_dir.resolve()
    if not work_dir.is_dir():
        raise NotADirectoryError(f"Hybrid work directory not found: {work_dir}")
    model_path, data_path, default_config = discover_step1_artifacts(work_dir)
    config_path = args.config.resolve() if args.config else default_config
    if not config_path.is_file() or config_path.stat().st_size == 0:
        raise FileNotFoundError(f"Hybrid quantization config not found: {config_path}")

    output_rknn = args.output_rknn.resolve()
    if output_rknn.exists() and not args.overwrite:
        raise FileExistsError(
            f"RKNN output already exists: {output_rknn}; pass --overwrite to replace it"
        )
    output_rknn.parent.mkdir(parents=True, exist_ok=True)

    analysis_input = None
    analysis_output = None
    if args.accuracy_analysis_input is not None:
        analysis_input = args.accuracy_analysis_input.resolve()
        if not analysis_input.is_file():
            raise FileNotFoundError(f"Accuracy-analysis input not found: {analysis_input}")
        analysis_output = (
            args.accuracy_analysis_output.resolve()
            if args.accuracy_analysis_output is not None
            else work_dir / f"{output_rknn.stem}_accuracy_analysis"
        )
        if analysis_output.exists():
            raise FileExistsError(
                f"Accuracy-analysis output already exists: {analysis_output}; use a new path"
            )
        analysis_output.mkdir(parents=True)
    elif args.accuracy_analysis_output is not None:
        raise ValueError(
            "--accuracy-analysis-output requires --accuracy-analysis-input"
        )

    print(f"model input: {model_path}")
    print(f"data input: {data_path}")
    print(f"quantization config: {config_path}")
    print(f"RKNN output: {output_rknn}")

    RKNN = import_rknn()
    previous_cwd = Path.cwd()
    rknn = RKNN(verbose=args.verbose)
    try:
        os.chdir(work_dir)
        check_return_code(
            "rknn.hybrid_quantization_step2",
            rknn.hybrid_quantization_step2(
                model_input=str(model_path),
                data_input=str(data_path),
                model_quantization_cfg=str(config_path),
            ),
        )
        check_return_code(
            "rknn.export_rknn", rknn.export_rknn(str(output_rknn))
        )
        if analysis_input is not None and analysis_output is not None:
            check_return_code(
                "rknn.accuracy_analysis",
                rknn.accuracy_analysis(
                    inputs=[str(analysis_input)], output_dir=str(analysis_output)
                ),
            )
    finally:
        os.chdir(previous_cwd)
        rknn.release()

    print(f"Step2 completed: {output_rknn}")
    if analysis_output is not None:
        print(f"Accuracy analysis: {analysis_output}")


def main() -> None:
    args = parse_args()
    if args.command == "step1":
        run_step1(args)
    elif args.command == "step2":
        run_step2(args)
    else:
        raise ValueError(f"Unsupported command: {args.command}")


if __name__ == "__main__":
    main()
