#!/usr/bin/env python3
"""将官方 PP-OCRv4 Student 输出头映射到 73 字符车牌字典。"""

from pathlib import Path

import paddle


SCRIPT_DIR = Path(__file__).resolve().parent
TRAIN_ROOT = SCRIPT_DIR.parent
PADDLEOCR_ROOT = TRAIN_ROOT / "PaddleOCR"

SOURCE_PATH = (
    PADDLEOCR_ROOT
    / "pretrain_models"
    / "ch_PP-OCRv4_rec_train"
    / "student.pdparams"
)
OUTPUT_PATH = SOURCE_PATH.with_name("student_cblprd73.pdparams")

OLD_DICT_PATH = PADDLEOCR_ROOT / "ppocr" / "utils" / "ppocr_keys_v1.txt"
NEW_DICT_PATH = PADDLEOCR_ROOT / "ppocr" / "utils" / "cblprd_plate_dict.txt"

CTC_WEIGHT = "head.ctc_head.fc.weight"
CTC_BIAS = "head.ctc_head.fc.bias"
NRTR_EMBEDDING = "head.gtc_head.embedding.embedding.weight"
NRTR_PROJECTION = "head.gtc_head.tgt_word_prj.weight"


def read_characters(path: Path) -> list[str]:
    return path.read_text(encoding="utf-8-sig").splitlines()


def main() -> None:
    paddle.set_device("cpu")

    if not SOURCE_PATH.is_file():
        raise FileNotFoundError(f"找不到官方权重：{SOURCE_PATH}")
    if OUTPUT_PATH.exists():
        raise FileExistsError(f"目标文件已经存在，拒绝覆盖：{OUTPUT_PATH}")

    # 官方 ch_PP-OCRv4 使用 use_space_char=True，
    # 因此 ppocr_keys_v1.txt 后还要追加一个空格。
    old_characters = read_characters(OLD_DICT_PATH) + [" "]
    new_characters = read_characters(NEW_DICT_PATH)

    if len(new_characters) != 73 or len(set(new_characters)) != 73:
        raise ValueError("车牌字典必须严格包含 73 个不重复字符")

    old_positions: dict[str, int] = {}
    for character in new_characters:
        positions = [
            index
            for index, old_character in enumerate(old_characters)
            if old_character == character
        ]
        if len(positions) != 1:
            raise ValueError(
                f"字符 {character!r} 在原字典中出现 {len(positions)} 次"
            )
        old_positions[character] = positions[0]

    state_dict = paddle.load(str(SOURCE_PATH))

    expected_shapes = {
        CTC_WEIGHT: [120, 6625],
        CTC_BIAS: [6625],
        NRTR_EMBEDDING: [6629, 384],
        NRTR_PROJECTION: [384, 6629],
    }
    for key, expected_shape in expected_shapes.items():
        if key not in state_dict:
            raise KeyError(f"官方权重缺少参数：{key}")
        actual_shape = list(state_dict[key].shape)
        if actual_shape != expected_shape:
            raise ValueError(
                f"{key} 形状异常：期望 {expected_shape}，实际 {actual_shape}"
            )

    # CTC 顺序：
    #   旧：[blank] + 6623 个字典字符 + [space]
    #   新：[blank] + 73 个车牌字符
    ctc_source_indices = [0] + [
        1 + old_positions[character] for character in new_characters
    ]

    # NRTR 顺序：
    #   [blank/pad, unk, bos, eos] + 字典字符 + Transformer额外输出位
    nrtr_source_indices = (
        [0, 1, 2, 3]
        + [4 + old_positions[character] for character in new_characters]
        + [4 + len(old_characters)]
    )

    ctc_indices = paddle.to_tensor(ctc_source_indices, dtype="int64")
    nrtr_indices = paddle.to_tensor(nrtr_source_indices, dtype="int64")

    state_dict[CTC_WEIGHT] = paddle.index_select(
        state_dict[CTC_WEIGHT], ctc_indices, axis=1
    )
    state_dict[CTC_BIAS] = paddle.index_select(
        state_dict[CTC_BIAS], ctc_indices, axis=0
    )
    state_dict[NRTR_EMBEDDING] = paddle.index_select(
        state_dict[NRTR_EMBEDDING], nrtr_indices, axis=0
    )
    state_dict[NRTR_PROJECTION] = paddle.index_select(
        state_dict[NRTR_PROJECTION], nrtr_indices, axis=1
    )

    expected_output_shapes = {
        CTC_WEIGHT: [120, 74],
        CTC_BIAS: [74],
        NRTR_EMBEDDING: [78, 384],
        NRTR_PROJECTION: [384, 78],
    }
    for key, expected_shape in expected_output_shapes.items():
        actual_shape = list(state_dict[key].shape)
        if actual_shape != expected_shape:
            raise RuntimeError(
                f"{key} 映射失败：期望 {expected_shape}，实际 {actual_shape}"
            )

    paddle.save(state_dict, str(OUTPUT_PATH))

    # 写入后重新加载，防止生成损坏的 checkpoint。
    verified = paddle.load(str(OUTPUT_PATH))
    for key, expected_shape in expected_output_shapes.items():
        if list(verified[key].shape) != expected_shape:
            raise RuntimeError(f"写入后校验失败：{key}")

    print(f"映射完成：{OUTPUT_PATH}")
    for key, shape in expected_output_shapes.items():
        print(f"  {key}: {shape}")


if __name__ == "__main__":
    main()