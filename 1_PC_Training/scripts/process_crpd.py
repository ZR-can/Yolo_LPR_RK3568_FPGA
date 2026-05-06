import os
import random
import shutil
import cv2
from pathlib import Path

# python process_crpd.py
random.seed(42)

# ================= 配置区 =================
# CRPD 原始数据集根目录
CRPD_ROOT = Path("../datasets/CRPD")
# YOLO 目标输出根目录
YOLO_ROOT = Path("../datasets/CRPD_YOLO")

# 类别映射规则 (避开绿牌 1)
# CRPD: 0(蓝) -> YOLO: 0
# CRPD: 1(单黄) -> YOLO: 2
# CRPD: 2(双黄) -> YOLO: 3
# CRPD: 3(白) -> YOLO: 4
CLASS_MAPPING = {'0': '0', '1': '2', '2': '3', '3': '4'}

# 采样策略配置
SAMPLING_RATES = {
    "CRPD_multi": 1.0,         # 多车全保留
    "CRPD_double": {
        "keep_all": ['1', '2', '3'], # 双车图片中含单黄、双黄、白牌全保留
        "sample_blue": 0.8           # 双车图片中仅含蓝牌则随机保留 80%
    },
    "CRPD_single": {
        "keep_all": ['1', '2', '3'], # 单车图片中含单黄、双黄、白牌全保留
        "sample_blue": 0.15          # 单车图片中仅含蓝牌则随机保留 15%
    }
}

def process_crpd_to_yolo():
    # 创建 YOLO 目录结构，明确包含 train, val, test 三个独立划分
    for split in ['train', 'val', 'test']:
            (YOLO_ROOT / 'images' / split).mkdir(parents=True, exist_ok=True)
            (YOLO_ROOT / 'labels' / split).mkdir(parents=True, exist_ok=True)

    total_processed = 0
    total_saved = 0

    # 遍历三大子集
    for subset in ["CRPD_multi", "CRPD_double", "CRPD_single"]:
        subset_path = CRPD_ROOT / subset
        if not subset_path.exists():
            continue

        # 遍历数据集划分
        for origin_split in ["train", "val", "test"]:
            split_path = subset_path / origin_split
            if not split_path.exists():
                continue
            
            target_split = origin_split
            
            img_dir = split_path / "images"
            lbl_dir = split_path / "labels"
            
            for txt_file in lbl_dir.glob("*.txt"):
                total_processed += 1
                
                with open(txt_file, 'r', encoding='utf-8') as f:
                    lines = f.readlines()
                
                if not lines:
                    continue

                # ----- 策略过滤逻辑 -----
                keep_file = False
                if subset == "CRPD_multi":
                    keep_file = True
                elif subset == "CRPD_double":
                    has_rare_class = any(line.strip().split()[8] in SAMPLING_RATES["CRPD_double"]["keep_all"] for line in lines if len(line.strip().split()) >= 10)
                    if has_rare_class:
                        keep_file = True
                    else:
                        keep_file = random.random() < SAMPLING_RATES["CRPD_double"]["sample_blue"]
                elif subset == "CRPD_single":
                    has_rare_class = any(line.strip().split()[8] in SAMPLING_RATES["CRPD_single"]["keep_all"] for line in lines if len(line.strip().split()) >= 10)
                    if has_rare_class:
                        keep_file = True
                    else:
                        keep_file = random.random() < SAMPLING_RATES["CRPD_single"]["sample_blue"]

                if not keep_file:
                    continue

                # ----- 图像处理与坐标转换 -----
                img_file = img_dir / (txt_file.stem + ".jpg") 
                if not img_file.exists():
                    continue

                img = cv2.imread(str(img_file))
                if img is None:
                    continue
                img_h, img_w = img.shape[:2]

                yolo_labels = []
                for line in lines:
                    parts = line.strip().split()
                    if len(parts) < 10:
                        continue
                    
                    coords = list(map(float, parts[:8]))
                    x_coords = coords[0::2]
                    y_coords = coords[1::2]
                    
                    x_min, x_max = min(x_coords), max(x_coords)
                    y_min, y_max = min(y_coords), max(y_coords)
                    
                    x_center = max(0, min(1, (x_min + x_max) / 2.0 / img_w))
                    y_center = max(0, min(1, (y_min + y_max) / 2.0 / img_h))
                    w = max(0, min(1, (x_max - x_min) / img_w))
                    h = max(0, min(1, (y_max - y_min) / img_h))
                    
                    crpd_class = parts[8]
                    yolo_class = CLASS_MAPPING.get(crpd_class, '4')
                    
                    yolo_labels.append(f"{yolo_class} {x_center:.6f} {y_center:.6f} {w:.6f} {h:.6f}\n")

                # ----- 文件写入与拷贝 -----
                if yolo_labels:
                    out_lbl_path = YOLO_ROOT / 'labels' / target_split / txt_file.name
                    with open(out_lbl_path, 'w', encoding='utf-8') as f:
                        f.writelines(yolo_labels)
                    
                    out_img_path = YOLO_ROOT / 'images' / target_split / img_file.name
                    shutil.copy(str(img_file), str(out_img_path))
                    
                    total_saved += 1

    return total_processed, total_saved

if __name__ == "__main__":
    print("开始清洗并转换 CRPD 数据集")
    processed, saved = process_crpd_to_yolo()

    print("-" * 30)
    print(f"数据处理完成！")
    print(f"总计扫描 CRPD 标签文件: {processed} 个")
    print(f"应用策略后有效保留并转换: {saved} 个")
    print(f"输出目录: {YOLO_ROOT.absolute()}")
    print("-" * 30)