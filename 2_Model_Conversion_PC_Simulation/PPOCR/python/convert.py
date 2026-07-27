#!/usr/bin/env python3
"""Convert the project PP-OCRv4 recognition ONNX model to RKNN."""

from __future__ import annotations

import argparse
import tempfile
from pathlib import Path
from typing import Optional, Tuple, Union


INPUT_SHAPE = (1, 3, 48, 160)
OUTPUT_SHAPE = (1, 20, 74)
CHARACTER_COUNT = 73
SUPPORTED_PLATFORMS = ("rk3562", "rk3566", "rk3568", "rk3576", "rk3588")
QUANT_SAMPLE_RANGES = {
    "normal": (20, 100),
    "kl_divergence": (20, 100),
    "mmse": (20, 50),
}

SCRIPT_DIR = Path(__file__).resolve().parent
MODEL_DIR = SCRIPT_DIR.parent / "model"
DEFAULT_CHARACTER_DICT = MODEL_DIR / "cblprd_plate_dict.txt"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Convert the fixed [1,3,48,160] PP-OCRv4 CTC ONNX model to "
            "RKNN Toolkit2 v2.3.0 format."
        )
    )
    parser.add_argument("onnx_model", type=Path)
    parser.add_argument("platform", choices=SUPPORTED_PLATFORMS)
    parser.add_argument(
        "dtype",
        nargs="?",
        choices=("fp", "i8"),
        default="fp",
        help="fp builds the FP16 baseline; i8 enables W8A8 PTQ (default: fp).",
    )
    parser.add_argument(
        "output_rknn",
        nargs="?",
        type=Path,
        help="Output path; defaults to PPOCR/model/<name>_<platform>_<dtype>.rknn.",
    )
    parser.add_argument(
        "--character-dict",
        type=Path,
        default=DEFAULT_CHARACTER_DICT,
        help="73-character dictionary used to verify the 74-class CTC output.",
    )
    parser.add_argument(
        "--dataset",
        type=Path,
        help="RKNN calibration dataset.txt; required for i8.",
    )
    parser.add_argument(
        "--quantized-algorithm",
        choices=tuple(QUANT_SAMPLE_RANGES),
        default="normal",
    )
    parser.add_argument(
        "--quantized-method", choices=("channel", "layer"), default="channel"
    )
    parser.add_argument(
        "--accuracy-analysis-input",
        type=Path,
        help="One preprocessed 48x160 image for optional i8 layer error analysis.",
    )
    parser.add_argument(
        "--accuracy-analysis-output",
        type=Path,
        help="Snapshot directory for --accuracy-analysis-input.",
    )
    parser.add_argument(
        "--validate-only",
        action="store_true",
        help="Validate ONNX, dictionary, and dataset without importing RKNN Toolkit2.",
    )
    parser.add_argument(
        "--overwrite",
        action="store_true",
        help="Allow replacing an existing generated RKNN output.",
    )
    parser.add_argument("--verbose", action="store_true")
    return parser.parse_args()


def tensor_shape(value_info) -> Tuple[Optional[Union[int, str]], ...]:
    dimensions = []
    for dimension in value_info.type.tensor_type.shape.dim:
        if dimension.HasField("dim_value"):
            dimensions.append(dimension.dim_value)
        elif dimension.HasField("dim_param"):
            dimensions.append(dimension.dim_param)
        else:
            dimensions.append(None)
    return tuple(dimensions)


def load_characters(path: Path) -> list[str]:
    path = path.resolve()
    if not path.is_file():
        raise FileNotFoundError(f"Character dictionary not found: {path}")
    characters = path.read_text(encoding="utf-8-sig").splitlines()
    if len(characters) != CHARACTER_COUNT:
        raise ValueError(
            f"Expected {CHARACTER_COUNT} dictionary entries, got {len(characters)}: {path}"
        )
    if any(not character for character in characters):
        raise ValueError(f"Character dictionary contains an empty entry: {path}")
    if len(set(characters)) != len(characters):
        raise ValueError(f"Character dictionary contains duplicate entries: {path}")
    return characters


def validate_onnx(path: Path, character_count: int) -> tuple[str, str]:
    try:
        import onnx
        from onnx import TensorProto
    except ImportError as exc:
        raise RuntimeError("The conversion environment must provide the 'onnx' package.") from exc

    path = path.resolve()
    if not path.is_file():
        raise FileNotFoundError(f"ONNX model not found: {path}")
    model = onnx.load(str(path))
    onnx.checker.check_model(model)

    initializer_names = {initializer.name for initializer in model.graph.initializer}
    inputs = [item for item in model.graph.input if item.name not in initializer_names]
    outputs = list(model.graph.output)
    if len(inputs) != 1 or len(outputs) != 1:
        raise ValueError(
            f"Expected one ONNX input and one output, got {len(inputs)} and {len(outputs)}"
        )

    input_info, output_info = inputs[0], outputs[0]
    input_shape = tensor_shape(input_info)
    output_shape = tensor_shape(output_info)
    if input_shape != INPUT_SHAPE:
        raise ValueError(f"Expected ONNX input shape {INPUT_SHAPE}, got {input_shape}")
    if output_shape != OUTPUT_SHAPE:
        raise ValueError(f"Expected ONNX output shape {OUTPUT_SHAPE}, got {output_shape}")
    if input_info.type.tensor_type.elem_type != TensorProto.FLOAT:
        raise ValueError("Expected a FLOAT ONNX input tensor")
    if output_info.type.tensor_type.elem_type != TensorProto.FLOAT:
        raise ValueError("Expected a FLOAT ONNX output tensor")
    if output_shape[-1] != character_count + 1:
        raise ValueError(
            f"Output classes {output_shape[-1]} do not match dict + CTC blank "
            f"({character_count + 1})"
        )
    return input_info.name, output_info.name


def validate_dataset(path: Path) -> list[Path]:
    path = path.resolve()
    if not path.is_file():
        raise FileNotFoundError(f"Calibration dataset not found: {path}")

    images: list[Path] = []
    for line_number, raw_line in enumerate(
        path.read_text(encoding="utf-8-sig").splitlines(), start=1
    ):
        line = raw_line.strip()
        if not line:
            continue
        fields = line.split()
        if len(fields) != 1:
            raise ValueError(
                f"Expected one input path at {path}:{line_number}, got {len(fields)}"
            )
        image = Path(fields[0]).expanduser()
        if not image.is_absolute():
            image = path.parent / image
        image = image.resolve()
        if not image.is_file():
            raise FileNotFoundError(
                f"Calibration image not found at {path}:{line_number}: {image}"
            )
        if " " in str(image):
            raise ValueError(f"RKNN dataset paths cannot contain spaces: {image}")
        images.append(image)

    if not images:
        raise ValueError(f"Calibration dataset is empty: {path}")
    return images


def check_return_code(operation: str, return_code) -> None:
    if return_code not in (None, 0):
        raise RuntimeError(f"{operation} failed with return code {return_code}")


def default_output_path(onnx_model: Path, platform: str, dtype: str) -> Path:
    suffix = "i8" if dtype == "i8" else "fp16"
    return MODEL_DIR / f"{onnx_model.stem}_{platform}_{suffix}.rknn"


def convert(args: argparse.Namespace, dataset_images: list[Path]) -> Path:
    try:
        from rknn.api import RKNN
    except ImportError as exc:
        raise RuntimeError(
            "RKNN Toolkit2 is unavailable. Run conversion in the Ubuntu v2.3.0 environment."
        ) from exc

    output_path = (
        args.output_rknn.resolve()
        if args.output_rknn
        else default_output_path(args.onnx_model, args.platform, args.dtype).resolve()
    )
    if output_path.exists() and not args.overwrite:
        raise FileExistsError(
            f"RKNN output already exists: {output_path}; pass --overwrite to replace it"
        )
    output_path.parent.mkdir(parents=True, exist_ok=True)

    rknn = RKNN(verbose=args.verbose)
    try:
        config = {
            "target_platform": args.platform,
            "mean_values": [[127.5, 127.5, 127.5]],
            "std_values": [[127.5, 127.5, 127.5]],
            # The training and deployment input is BGR. This option only affects
            # image decoding during calibration/accuracy analysis.
            "quant_img_RGB2BGR": True,
        }
        if args.dtype == "i8":
            config.update(
                quantized_algorithm=args.quantized_algorithm,
                quantized_method=args.quantized_method,
            )

        print("--> Config model")
        check_return_code("rknn.config", rknn.config(**config))
        print("done")

        print("--> Load ONNX")
        check_return_code(
            "rknn.load_onnx", rknn.load_onnx(model=str(args.onnx_model.resolve()))
        )
        print("done")

        print("--> Build RKNN")
        if args.dtype == "i8":
            with tempfile.TemporaryDirectory(prefix="ppocr_rknn_dataset_") as temp_dir:
                resolved_dataset = Path(temp_dir) / "dataset.txt"
                with resolved_dataset.open(
                    "w", encoding="utf-8", newline="\n"
                ) as file_handle:
                    file_handle.writelines(
                        f"{image.as_posix()}\n" for image in dataset_images
                    )
                check_return_code(
                    "rknn.build",
                    rknn.build(
                        do_quantization=True, dataset=str(resolved_dataset)
                    ),
                )
        else:
            check_return_code("rknn.build", rknn.build(do_quantization=False))
        print("done")

        if args.accuracy_analysis_input:
            analysis_input = args.accuracy_analysis_input.resolve()
            analysis_output = (
                args.accuracy_analysis_output.resolve()
                if args.accuracy_analysis_output
                else output_path.with_name(f"{output_path.stem}_snapshot")
            )
            analysis_output.mkdir(parents=True, exist_ok=True)
            print("--> Accuracy analysis")
            check_return_code(
                "rknn.accuracy_analysis",
                rknn.accuracy_analysis(
                    inputs=[str(analysis_input)], output_dir=str(analysis_output)
                ),
            )
            print(f"done: {analysis_output}")

        print("--> Export RKNN")
        check_return_code("rknn.export_rknn", rknn.export_rknn(str(output_path)))
        print(f"done: {output_path}")
    finally:
        rknn.release()
    return output_path


def main() -> None:
    args = parse_args()
    args.onnx_model = args.onnx_model.resolve()
    args.character_dict = args.character_dict.resolve()

    characters = load_characters(args.character_dict)
    input_name, output_name = validate_onnx(args.onnx_model, len(characters))

    dataset_images: list[Path] = []
    if args.dtype == "i8":
        if args.dataset is None:
            raise ValueError("--dataset is required when dtype is i8")
        dataset_images = validate_dataset(args.dataset)
        minimum, maximum = QUANT_SAMPLE_RANGES[args.quantized_algorithm]
        if not minimum <= len(dataset_images) <= maximum:
            print(
                f"WARNING: {args.quantized_algorithm} is normally calibrated with "
                f"{minimum}-{maximum} images; current dataset has {len(dataset_images)}."
            )
    elif args.dataset is not None:
        raise ValueError("--dataset is only valid when dtype is i8")

    if args.accuracy_analysis_input:
        if args.dtype != "i8":
            raise ValueError("--accuracy-analysis-input requires dtype i8")
        if not args.accuracy_analysis_input.resolve().is_file():
            raise FileNotFoundError(
                f"Accuracy-analysis input not found: {args.accuracy_analysis_input.resolve()}"
            )

    print(f"ONNX: {args.onnx_model}")
    print(f"input: {input_name} {INPUT_SHAPE}")
    print(f"output: {output_name} {OUTPUT_SHAPE}")
    print(f"dictionary: {args.character_dict} ({len(characters)} characters)")
    print(f"target: {args.platform}, dtype: {'W8A8' if args.dtype == 'i8' else 'FP16'}")
    if dataset_images:
        print(
            f"calibration: {len(dataset_images)} images, "
            f"{args.quantized_algorithm}/{args.quantized_method}"
        )

    if args.validate_only:
        print("Validation passed; RKNN conversion was not started.")
        return
    convert(args, dataset_images)


if __name__ == "__main__":
    main()
