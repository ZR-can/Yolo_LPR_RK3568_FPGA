#!/usr/bin/env python3
"""Fold the 12 scalar LearnableAffine blocks before 1x1 Conv nodes."""

from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
import onnx
from onnx import numpy_helper


TARGET_CONVS = (
    "Conv.2",
    "Conv.4",
    "Conv.6",
    "Conv.8",
    "Conv.10",
    "Conv.12",
    "Conv.14",
    "Conv.16",
    "Conv.18",
    "Conv.20",
    "Conv.30",
    "Conv.32",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Fold the 12 scalar Mul/Add LearnableAffine chains into their "
            "following 1x1 Conv weights and biases."
        )
    )
    parser.add_argument("input_onnx", type=Path)
    parser.add_argument("output_onnx", type=Path)
    parser.add_argument(
        "--overwrite",
        action="store_true",
        help="Allow replacing output_onnx; input_onnx is never overwritten.",
    )
    return parser.parse_args()


def node_attributes(node: onnx.NodeProto) -> dict[str, object]:
    return {
        attribute.name: onnx.helper.get_attribute_value(attribute)
        for attribute in node.attribute
    }


def split_constant_input(
    node: onnx.NodeProto,
    tensor_values: dict[str, np.ndarray],
) -> tuple[str, str]:
    constant_inputs = [name for name in node.input if name in tensor_values]
    data_inputs = [name for name in node.input if name not in tensor_values]
    if len(constant_inputs) != 1 or len(data_inputs) != 1:
        raise ValueError(
            f"{node.name or node.op_type} must have one constant and one data input"
        )
    return constant_inputs[0], data_inputs[0]


def fold_affine_blocks(model: onnx.ModelProto) -> list[str]:
    graph = model.graph
    producers = {
        output_name: node for node in graph.node for output_name in node.output
    }
    consumers: dict[str, list[onnx.NodeProto]] = {}
    for node in graph.node:
        for input_name in node.input:
            consumers.setdefault(input_name, []).append(node)

    initializers = {tensor.name: tensor for tensor in graph.initializer}
    initializer_values = {
        name: numpy_helper.to_array(tensor)
        for name, tensor in initializers.items()
    }
    constant_tensors: dict[str, onnx.TensorProto] = {}
    for node in graph.node:
        if node.op_type != "Constant" or len(node.output) != 1:
            continue
        value_attribute = next(
            (attribute for attribute in node.attribute if attribute.name == "value"),
            None,
        )
        if value_attribute is not None:
            constant_tensors[node.output[0]] = value_attribute.t
    constant_values = {
        name: numpy_helper.to_array(tensor)
        for name, tensor in constant_tensors.items()
    }
    tensor_values = {**initializer_values, **constant_values}

    def replace_tensor_value(name: str, array: np.ndarray) -> None:
        if name in initializers:
            initializers[name].CopyFrom(
                numpy_helper.from_array(array, name=name)
            )
            return
        if name in constant_tensors:
            tensor_name = constant_tensors[name].name
            constant_tensors[name].CopyFrom(
                numpy_helper.from_array(array, name=tensor_name)
            )
            return
        raise ValueError(f"tensor {name} is not a constant parameter")

    conv_by_name = {
        node.name: node for node in graph.node if node.name in TARGET_CONVS
    }
    missing = sorted(set(TARGET_CONVS) - set(conv_by_name))
    if missing:
        raise ValueError(f"target Conv nodes are missing: {', '.join(missing)}")

    node_ids_to_remove: set[int] = set()
    initializer_names_to_remove: set[str] = set()
    folded: list[str] = []

    for conv_name in TARGET_CONVS:
        conv = conv_by_name[conv_name]
        if conv.op_type != "Conv" or len(conv.input) != 3:
            raise ValueError(f"{conv_name} must be a Conv with weight and bias")
        if node_attributes(conv).get("group", 1) != 1:
            raise ValueError(f"{conv_name} must use group=1")

        weight_name, conv_bias_name = conv.input[1], conv.input[2]
        if weight_name not in tensor_values or conv_bias_name not in tensor_values:
            raise ValueError(f"{conv_name} weight and bias must be constant tensors")
        if len(consumers.get(weight_name, [])) != 1:
            raise ValueError(f"{conv_name} weight initializer is shared")
        if len(consumers.get(conv_bias_name, [])) != 1:
            raise ValueError(f"{conv_name} bias initializer is shared")

        weight = tensor_values[weight_name]
        conv_bias = tensor_values[conv_bias_name]
        if weight.ndim != 4 or tuple(weight.shape[2:]) != (1, 1):
            raise ValueError(f"{conv_name} is not a 1x1 Conv: {weight.shape}")
        if conv_bias.shape != (weight.shape[0],):
            raise ValueError(
                f"{conv_name} bias shape {conv_bias.shape} does not match "
                f"output channels {weight.shape[0]}"
            )

        add = producers.get(conv.input[0])
        if add is None or add.op_type != "Add":
            raise ValueError(f"{conv_name} input must be produced by affine Add")
        add_constant_name, mul_output_name = split_constant_input(
            add, tensor_values
        )
        mul = producers.get(mul_output_name)
        if mul is None or mul.op_type != "Mul":
            raise ValueError(f"{conv_name} affine Add must be fed by affine Mul")
        mul_constant_name, source_name = split_constant_input(mul, tensor_values)

        if len(consumers.get(mul.output[0], [])) != 1:
            raise ValueError(f"{mul.name} must have exactly one consumer")
        if len(consumers.get(add.output[0], [])) != 1:
            raise ValueError(f"{add.name} must have exactly one consumer")

        scale_array = tensor_values[mul_constant_name]
        affine_bias_array = tensor_values[add_constant_name]
        if scale_array.size != 1 or affine_bias_array.size != 1:
            raise ValueError(f"{conv_name} affine scale and bias must be scalar")
        scale = float(scale_array.reshape(-1)[0])
        affine_bias = float(affine_bias_array.reshape(-1)[0])

        weight_fp64 = weight.astype(np.float64)
        new_weight = (weight_fp64 * scale).astype(weight.dtype)
        new_bias = (
            conv_bias.astype(np.float64)
            + affine_bias * weight_fp64.sum(axis=(1, 2, 3))
        ).astype(conv_bias.dtype)

        replace_tensor_value(weight_name, new_weight)
        replace_tensor_value(conv_bias_name, new_bias)
        conv.input[0] = source_name

        node_ids_to_remove.update((id(mul), id(add)))
        for constant_name in (mul_constant_name, add_constant_name):
            constant_node = producers.get(constant_name)
            if constant_node is not None and constant_node.op_type == "Constant":
                if len(consumers.get(constant_name, [])) != 1:
                    raise ValueError(
                        f"affine constant {constant_name} is shared unexpectedly"
                    )
                node_ids_to_remove.add(id(constant_node))
            elif constant_name in initializers:
                if len(consumers.get(constant_name, [])) != 1:
                    raise ValueError(
                        f"affine initializer {constant_name} is shared unexpectedly"
                    )
                initializer_names_to_remove.add(constant_name)

        folded.append(
            f"{mul.name} -> {add.name} -> {conv_name} "
            f"(scale={scale:.9g}, bias={affine_bias:.9g})"
        )

    kept_nodes = [
        node for node in graph.node if id(node) not in node_ids_to_remove
    ]
    del graph.node[:]
    graph.node.extend(kept_nodes)

    for initializer_name in initializer_names_to_remove:
        graph.initializer.remove(initializers[initializer_name])

    return folded


def main() -> None:
    args = parse_args()
    input_path = args.input_onnx.resolve()
    output_path = args.output_onnx.resolve()
    if not input_path.is_file():
        raise FileNotFoundError(f"input ONNX not found: {input_path}")
    if input_path == output_path:
        raise ValueError("input and output ONNX paths must be different")
    if output_path.exists() and not args.overwrite:
        raise FileExistsError(
            f"output already exists: {output_path}; pass --overwrite to replace it"
        )

    model = onnx.load(str(input_path))
    onnx.checker.check_model(model)
    folded = fold_affine_blocks(model)
    if len(folded) != len(TARGET_CONVS):
        raise RuntimeError(
            f"expected {len(TARGET_CONVS)} folds, completed {len(folded)}"
        )

    model = onnx.shape_inference.infer_shapes(model)
    onnx.checker.check_model(model)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    onnx.save(model, str(output_path))

    print(f"input: {input_path}")
    print(f"output: {output_path}")
    print(f"folded blocks: {len(folded)}")
    for description in folded:
        print(f"  {description}")


if __name__ == "__main__":
    main()
