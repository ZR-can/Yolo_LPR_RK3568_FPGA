"""Convert the project four-class YOLOv8-OBB raw-head ONNX to RKNN."""

from __future__ import annotations

import argparse
from pathlib import Path

import onnx


SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_DATASET_PATH = SCRIPT_DIR.parent / "model/obb_quant_dataset_800.txt"
PLATFORMS = (
    "rk3562",
    "rk3566",
    "rk3568",
    "rk3576",
    "rk3588",
    "rk1808",
    "rv1109",
    "rv1126",
)
EXPECTED_INPUT = (1, 3, 640, 640)
EXPECTED_OUTPUTS = (
    (1, 68, 80, 80),
    (1, 68, 40, 40),
    (1, 68, 20, 20),
    (1, 1, 8400),
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("onnx_model", type=Path)
    parser.add_argument("platform", choices=PLATFORMS)
    parser.add_argument("dtype", nargs="?", choices=("i8", "u8", "fp"), default="i8")
    parser.add_argument("output_rknn", nargs="?", type=Path)
    parser.add_argument(
        "--dataset",
        type=Path,
        default=DEFAULT_DATASET_PATH,
        help="RKNN calibration list; required only for i8/u8 conversion",
    )
    parser.add_argument("--check-only", action="store_true")
    parser.add_argument("--verbose", action="store_true")
    return parser.parse_args()


def tensor_shape(value_info: onnx.ValueInfoProto) -> tuple[int, ...]:
    shape = []
    for dimension in value_info.type.tensor_type.shape.dim:
        if not dimension.HasField("dim_value"):
            raise ValueError(f"Dynamic tensor dimension is not supported: {value_info.name}")
        shape.append(dimension.dim_value)
    return tuple(shape)


def validate_onnx(model_path: Path) -> None:
    if not model_path.is_file():
        raise FileNotFoundError(f"ONNX model not found: {model_path}")
    model = onnx.load(model_path)
    onnx.checker.check_model(model)
    if len(model.graph.input) != 1:
        raise ValueError(f"Expected one ONNX input, found {len(model.graph.input)}")
    input_shape = tensor_shape(model.graph.input[0])
    output_shapes = tuple(tensor_shape(output) for output in model.graph.output)
    if input_shape != EXPECTED_INPUT:
        raise ValueError(f"Unexpected input shape: {input_shape}, expected {EXPECTED_INPUT}")
    if output_shapes != EXPECTED_OUTPUTS:
        raise ValueError(
            f"Unexpected OBB output shapes: {output_shapes}, expected {EXPECTED_OUTPUTS}"
        )
    opsets = {item.domain or "ai.onnx": item.version for item in model.opset_import}
    if opsets.get("ai.onnx") != 12:
        raise ValueError(f"Expected ONNX opset 12, found {opsets.get('ai.onnx')}")
    print(f"ONNX signature valid: input={input_shape}, outputs={output_shapes}")


def validate_dataset(dataset_path: Path) -> int:
    if not dataset_path.is_file():
        raise FileNotFoundError(f"Calibration list not found: {dataset_path}")
    entries = [
        line.strip()
        for line in dataset_path.read_text(encoding="utf-8").splitlines()
        if line.strip()
    ]
    if not entries:
        raise ValueError(f"Calibration list is empty: {dataset_path}")
    if len(entries) != len({entry.casefold() for entry in entries}):
        raise ValueError(f"Calibration list contains duplicate paths: {dataset_path}")
    missing = [
        entry
        for entry in entries
        if not (dataset_path.parent / Path(entry)).is_file()
    ]
    if missing:
        preview = "\n  ".join(missing[:5])
        raise FileNotFoundError(
            f"Calibration list contains {len(missing)} missing files:\n  {preview}"
        )
    print(f"Calibration list valid: {len(entries)} unique images")
    return len(entries)


def default_output_path(model_path: Path, platform: str, dtype: str) -> Path:
    suffix = "fp16" if dtype == "fp" else dtype
    return model_path.with_name(f"{model_path.stem}_{platform}_{suffix}.rknn")


def main() -> int:
    args = parse_args()
    modern_platforms = {"rk3562", "rk3566", "rk3568", "rk3576", "rk3588"}
    if args.platform in modern_platforms and args.dtype == "u8":
        raise ValueError(f"{args.platform} supports i8 or fp, not u8")
    if args.platform not in modern_platforms and args.dtype == "i8":
        raise ValueError(f"{args.platform} supports u8 or fp, not i8")
    model_path = args.onnx_model.resolve()
    dataset_path = args.dataset.resolve()
    output_path = (
        args.output_rknn.resolve()
        if args.output_rknn is not None
        else default_output_path(model_path, args.platform, args.dtype)
    )
    do_quantization = args.dtype in {"i8", "u8"}

    validate_onnx(model_path)
    if do_quantization:
        validate_dataset(dataset_path)
    if args.check_only:
        print("Preflight checks passed; RKNN build skipped")
        return 0

    try:
        from rknn.api import RKNN
    except ImportError as error:
        raise RuntimeError(
            "RKNN Toolkit2 is unavailable. Run conversion in the Ubuntu "
            "RKNN Toolkit2 2.3.0 environment."
        ) from error

    output_path.parent.mkdir(parents=True, exist_ok=True)
    rknn = RKNN(verbose=args.verbose)
    try:
        print("--> Config model")
        ret = rknn.config(
            mean_values=[[0, 0, 0]],
            std_values=[[255, 255, 255]],
            target_platform=args.platform,
        )
        if ret != 0:
            raise RuntimeError(f"rknn.config failed: {ret}")

        print("--> Loading ONNX")
        ret = rknn.load_onnx(model=str(model_path))
        if ret != 0:
            raise RuntimeError(f"rknn.load_onnx failed: {ret}")

        print("--> Building RKNN")
        build_args = {"do_quantization": do_quantization}
        if do_quantization:
            build_args["dataset"] = str(dataset_path)
        ret = rknn.build(**build_args)
        if ret != 0:
            raise RuntimeError(f"rknn.build failed: {ret}")

        print("--> Exporting RKNN")
        ret = rknn.export_rknn(str(output_path))
        if ret != 0:
            raise RuntimeError(f"rknn.export_rknn failed: {ret}")
    finally:
        rknn.release()

    print(f"RKNN model saved: {output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
