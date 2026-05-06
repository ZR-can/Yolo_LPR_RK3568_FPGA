import argparse
import os
import random
import shutil
from collections import defaultdict
from pathlib import Path

import cv2
import numpy as np


LPRNET_WIDTH = 94
LPRNET_HEIGHT = 24

PROJECT_ROOT = Path(r"D:\Yolo_LPR_RK3568_FPGA_Project")
SOURCE_DATA_ROOT = PROJECT_ROOT / "1_PC_Training" / "datasets" / "lprnet_7char"
SOURCE_TXT_DIR = SOURCE_DATA_ROOT / "classified_txts"
LPRNET_TIGHT_TRAIN = SOURCE_DATA_ROOT / "lprnet_7char_tight_train"
LPRNET_TIGHT_VAL = SOURCE_DATA_ROOT / "lprnet_7char_tight_val"
YOLO_CROPS_BLUE_TRAIN = PROJECT_ROOT / "1_PC_Training" / "datasets" / "yolo_lprnet_crops" / "blue" / "train"
RAW_IMAGE_DIR = PROJECT_ROOT / "2_Model_Conversion_PC_Simulation" / "LPRNet" / "model" / "lprnet7_raw_images"
QUANT_DATASET_DIR = PROJECT_ROOT / "2_Model_Conversion_PC_Simulation" / "LPRNet" / "model" / "lprnet7_quant_dataset"
DATASET_TXT = PROJECT_ROOT / "2_Model_Conversion_PC_Simulation" / "LPRNet" / "model" / "lprnet7_dataset.txt"

DEFAULT_PER_CLASS = 100
DEFAULT_YOLO_CROPS_COUNT = 100
DEFAULT_SEED = 42

IMG_SUFFIX = {".jpg", ".jpeg", ".png", ".bmp", ".JPG", ".JPEG", ".PNG", ".BMP"}


def cv_imread(file_path):
    """支持中文路径的图像读取。"""
    try:
        return cv2.imdecode(np.fromfile(str(file_path), dtype=np.uint8), cv2.IMREAD_COLOR)
    except Exception as exc:
        print(f"读取异常 {file_path}: {exc}")
        return None


def cv_imwrite(file_path, img):
    """支持中文路径的图像写入。"""
    try:
        ext = os.path.splitext(str(file_path))[1]
        cv2.imencode(ext, img)[1].tofile(str(file_path))
        return True
    except Exception as exc:
        print(f"写入异常 {file_path}: {exc}")
        return False


def build_plate_type_mapping(txt_dir):
    """从 type_*_train.txt 中构建车牌号->类别的映射。"""
    plate_to_type = {}

    if not txt_dir.exists():
        print(f"警告: 分类目录不存在: {txt_dir}")
        return plate_to_type

    for txt_path in sorted(txt_dir.glob("type_*_train.txt")):
        with open(txt_path, "r", encoding="utf-8") as file_handle:
            for line in file_handle:
                parts = line.strip().split()
                if len(parts) < 3:
                    continue
                plate_number, plate_type = parts[1], parts[2]
                plate_to_type[plate_number] = plate_type

    return plate_to_type


def read_classified_samples(txt_dir):
    """从 type_*_train.txt 中读取样本，并按类别分组。"""
    grouped = defaultdict(list)

    if not txt_dir.exists():
        raise FileNotFoundError(f"分类目录不存在: {txt_dir}")

    for txt_path in sorted(txt_dir.glob("type_*_train.txt")):
        # 只读 train 集
        with open(txt_path, "r", encoding="utf-8") as file_handle:
            for line in file_handle:
                parts = line.strip().split()
                if len(parts) < 3:
                    continue

                rel_path, plate_number, plate_type = parts[0], parts[1], parts[2]
                img_path = SOURCE_DATA_ROOT / rel_path
                if img_path.exists() and img_path.suffix in IMG_SUFFIX:
                    grouped[plate_type].append((img_path, plate_number, plate_type, rel_path))

    return grouped


def read_direct_samples(img_dir, category_label="default"):
    """从目录直接读取图片，不依赖分类文件。"""
    samples = []

    if not img_dir.exists():
        print(f"警告: 目录不存在: {img_dir}")
        return samples

    for img_path in sorted(img_dir.iterdir()):
        if img_path.is_file() and img_path.suffix in IMG_SUFFIX:
            rel_path = img_path.name
            plate_number = img_path.stem
            samples.append((img_path, plate_number, category_label, rel_path))

    return samples


def read_lprnet_tight_samples(train_dir, val_dir, plate_to_type, per_class, seed):
    """从 lprnet_7char_tight_train 和 _val 目录读取图片，用 plate_to_type 映射获取类别标签。"""
    grouped = defaultdict(list)
    
    # 读训练集
    if train_dir.exists():
        for img_path in sorted(train_dir.iterdir()):
            if img_path.is_file() and img_path.suffix in IMG_SUFFIX:
                plate_number = img_path.stem
                plate_type = plate_to_type.get(plate_number, "unknown")
                if plate_type != "unknown":
                    grouped[plate_type].append((img_path, plate_number, plate_type, img_path.name))
    
    # 读验证集
    if val_dir.exists():
        for img_path in sorted(val_dir.iterdir()):
            if img_path.is_file() and img_path.suffix in IMG_SUFFIX:
                plate_number = img_path.stem
                plate_type = plate_to_type.get(plate_number, "unknown")
                if plate_type != "unknown":
                    grouped[plate_type].append((img_path, plate_number, plate_type, img_path.name))
    
    if not grouped:
        return [], {}
    
    # 按类别均衡抽样
    rng = random.Random(seed)
    selected = []
    summary = {}
    
    for plate_type in sorted(grouped.keys()):
        samples = list(grouped[plate_type])
        rng.shuffle(samples)
        count = min(per_class, len(samples))
        picked = samples[:count]
        selected.extend(picked)
        summary[plate_type] = {
            "available": len(samples),
            "selected": count,
        }
    
    rng.shuffle(selected)
    return selected, summary



def choose_balanced_samples(grouped_samples, per_class, seed):
    """每类抽取固定数量，类别不足时取全部。"""
    rng = random.Random(seed)
    selected = []
    summary = {}

    for plate_type in sorted(grouped_samples.keys()):
        samples = list(grouped_samples[plate_type])
        rng.shuffle(samples)
        count = min(per_class, len(samples))
        picked = samples[:count]
        selected.extend(picked)
        summary[plate_type] = {
            "available": len(samples),
            "selected": count,
        }

    rng.shuffle(selected)
    return selected, summary


def move_or_copy_samples(samples, raw_dir, mode="copy"):
    """把抽样图片复制/移动到 raw_images，并生成新的文件名。"""
    raw_dir.mkdir(parents=True, exist_ok=True)

    for existing_file in raw_dir.iterdir():
        if existing_file.is_file() and existing_file.suffix.lower() in {".jpg", ".jpeg", ".png", ".bmp"}:
            existing_file.unlink()

    manifest = []
    for index, (src_path, plate_number, category_label, rel_path) in enumerate(samples, start=1):
        save_name = f"{category_label}_{index:04d}_{src_path.stem}{src_path.suffix.lower()}"
        dst_path = raw_dir / save_name

        if mode == "move":
            shutil.move(str(src_path), str(dst_path))
        else:
            shutil.copy2(str(src_path), str(dst_path))

        manifest.append((dst_path, category_label, plate_number, rel_path))

    return manifest


def generate_lprnet_quant_dataset(
    input_img_dir=RAW_IMAGE_DIR,
    output_dir=QUANT_DATASET_DIR,
    txt_path=DATASET_TXT,
):
    """读取 raw_images，缩放到 LPRNet 输入尺寸并生成 RKNN dataset.txt。"""
    output_dir.mkdir(parents=True, exist_ok=True)
    for existing_file in output_dir.iterdir():
        if existing_file.is_file() and existing_file.suffix.lower() in {".jpg", ".jpeg", ".png", ".bmp"}:
            existing_file.unlink()

    if not input_img_dir.exists():
        print(f"输入目录 {input_img_dir} 不存在。")
        return 0

    img_list = [p for p in sorted(input_img_dir.iterdir()) if p.is_file() and p.suffix in IMG_SUFFIX]
    if not img_list:
        print(f"目录 {input_img_dir} 中未找到有效图像文件。")
        return 0

    written = 0
    with open(txt_path, "w", encoding="utf-8") as file_handle:
        for index, img_path in enumerate(img_list, start=1):
            print(f"处理 {index}/{len(img_list)}: {img_path.name}")

            img = cv_imread(img_path)
            if img is None:
                print(f"警告: 无法解码图像 {img_path}，已跳过。")
                continue

            img = cv2.resize(img, (LPRNET_WIDTH, LPRNET_HEIGHT), interpolation=cv2.INTER_LINEAR)
            save_name = f"lprnet_quant_{written:03d}.jpg"
            save_path = output_dir / save_name
            if cv_imwrite(save_path, img):
                file_handle.write(f"./lprnet7_quant_dataset/{save_name}\n")
                written += 1

    print("\nLPRNet 量化数据集生成完成")
    print(f"量化图片输出目录: {output_dir}")
    print(f"量化配置 txt 路径: {txt_path}")
    return written


def build_quant_calibration_set(per_class=DEFAULT_PER_CLASS, yolo_crops_count=DEFAULT_YOLO_CROPS_COUNT, seed=DEFAULT_SEED, mode="copy", dry_run=False):
    """组合来自 lprnet_tight 和 yolo_crops 的样本，进行均衡抽样。"""
    selected_samples = []
    summary = {}
    
    # 构建车牌号->类别映射
    plate_to_type = build_plate_type_mapping(SOURCE_TXT_DIR)
    print(f"已加载 {len(plate_to_type)} 个车牌号的类别映射")
    
    # 从 lprnet_tight_train 和 _val 读取样本，用映射获取类别
    tight_samples, tight_summary = read_lprnet_tight_samples(
        LPRNET_TIGHT_TRAIN, LPRNET_TIGHT_VAL,
        plate_to_type=plate_to_type, per_class=per_class, seed=seed
    )
    selected_samples.extend(tight_samples)
    summary.update(tight_summary)
    
    # 从 yolo_crops 读取样本
    yolo_crops_samples = read_direct_samples(YOLO_CROPS_BLUE_TRAIN, category_label="yolo_blue")
    if yolo_crops_samples:
        rng = random.Random(seed)
        rng.shuffle(yolo_crops_samples)
        yolo_count = min(yolo_crops_count, len(yolo_crops_samples))
        selected_yolo = yolo_crops_samples[:yolo_count]
        selected_samples.extend(selected_yolo)
        
        summary["yolo_blue"] = {
            "available": len(yolo_crops_samples),
            "selected": yolo_count,
        }

    if not selected_samples:
        print("没有可用的抽样图片。")
        return 0

    print("\n数据源抽样统计:")
    for source_type in sorted(summary.keys()):
        info = summary[source_type]
        print(f"- {source_type}: 可用 {info['available']}，选取 {info['selected']}")
    print(f"总计选取: {len(selected_samples)} 张")

    if dry_run:
        return len(selected_samples)

    manifest = move_or_copy_samples(selected_samples, RAW_IMAGE_DIR, mode=mode)
    generated = generate_lprnet_quant_dataset(RAW_IMAGE_DIR, QUANT_DATASET_DIR, DATASET_TXT)

    raw_count = sum(1 for p in RAW_IMAGE_DIR.iterdir() if p.is_file() and p.suffix in IMG_SUFFIX)
    print(f"\nraw_images 实际文件数: {raw_count}")
    print(f"已处理并写入量化图片数: {generated}")
    print(f"已迁移/复制清单数: {len(manifest)}")
    return generated


def parse_args():
    parser = argparse.ArgumentParser(description="为 LPRNet 生成 RKNN PTQ 量化数据集")
    parser.add_argument("--per-class", type=int, default=DEFAULT_PER_CLASS, help="每个类别抽取的图片数量")
    parser.add_argument("--yolo-crops-count", type=int, default=DEFAULT_YOLO_CROPS_COUNT, help="从 yolo_crops 目录抽取的图片总数")
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED, help="抽样随机种子")
    parser.add_argument("--mode", choices=("move", "copy"), default="copy", help="把图片移动还是复制到 raw_images")
    parser.add_argument("--dry-run", action="store_true", help="只统计抽样数量，不实际迁移或生成文件")
    return parser.parse_args()


if __name__ == "__main__":
    args = parse_args()
    build_quant_calibration_set(per_class=args.per_class, yolo_crops_count=args.yolo_crops_count, seed=args.seed, mode=args.mode, dry_run=args.dry_run)