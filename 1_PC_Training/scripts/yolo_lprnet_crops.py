import os
import cv2
import random
from pathlib import Path
from ultralytics import YOLO
import numpy as np

def cv_imread(file_path):
    """读取包含中文路径的图片"""
    try:
        # np.fromfile 读取为二进制，imdecode 解码为图像矩阵
        img = cv2.imdecode(np.fromfile(file_path, dtype=np.uint8), cv2.IMREAD_COLOR)
        return img
    except Exception as e:
        print(f"读取图片失败 {file_path}: {e}")
        return None

def cv_imwrite(file_path, img):
    """将图片保存到包含中文的路径"""
    try:
        # imencode 编码为指定格式，tofile 写入物理文件
        # 注意：这里需要提取文件后缀来决定编码格式
        ext = file_path.suffix if hasattr(file_path, 'suffix') else '.' + str(file_path).split('.')[-1]
        cv2.imencode(ext, img)[1].tofile(file_path)
        return True
    except Exception as e:
        print(f"保存图片失败 {file_path}: {e}")
        return False
# ================= 1. 全局配置区 =================
# 输入目录
CCPD_BLUE_DIR = Path(r"D:\Yolo_LPR_RK3568_FPGA_Project\1_PC_Training\datasets\CCPD2019\ccpd_base")
CCPD_GREEN_DIR = Path(r"D:\Yolo_LPR_RK3568_FPGA_Project\1_PC_Training\datasets\CCPD2020\ccpd_green")

# 输出目录 (将自动构建 train/val/test 和 txt 文件)
OUT_ROOT = Path(r"D:\Yolo_LPR_RK3568_FPGA_Project\1_PC_Training\datasets\yolo_lprnet_crops")

# YOLO 模型路径 (请替换为你目前效果最好的 YOLOv8 权重)
YOLO_WEIGHT_PATH = r"D:\Yolo_LPR_RK3568_FPGA_Project\1_PC_Training\scripts\runs\train_results\yolov8n_multi_class\weights\best.pt"

# 蓝牌随机划分比例 (Train:Val:Test = 8:1:1)
SPLIT_RATIOS = [0.8, 0.1, 0.1]

# 预设抽取数量 (为了防止几十万张跑太久，建议蓝绿各抽取1.5万张用于微调。设为 None 则全量跑)
MAX_BLUE_SAMPLES = 15000 
MAX_GREEN_SAMPLES = 15000

# ================= 2. CCPD 字典与解码器 =================
PROVINCES = ["皖", "沪", "津", "渝", "冀", "晋", "蒙", "辽", "吉", "黑", "苏", "浙", "京", "闽", "赣", "鲁", "豫", "鄂", "湘", "粤", "桂", "琼", "川", "贵", "云", "藏", "陕", "甘", "青", "宁", "新", "警", "学", "O"]
ALPHABETS = ['A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'J', 'K', 'L', 'M', 'N', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W',
             'X', 'Y', 'Z', 'O']
ADS = ['A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'J', 'K', 'L', 'M', 'N', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X',
       'Y', 'Z', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'O']
def decode_ccpd_filename(filename):
    """
    带防错机制的 CCPD 文件名解码器
    """
    parts = filename.split('-')
    if len(parts) < 5:
        return None
        
    plate_code_part = parts[4]
    indices = plate_code_part.split('_')
    
    # 车牌最少包含 7 位字符
    if len(indices) < 7:
        return None
        
    try:
        prov_idx = int(indices[0])
        alpha_idx = int(indices[1])
        
        # 验证省份和字母索引范围
        if not (0 <= prov_idx < len(PROVINCES) and 0 <= alpha_idx < len(ALPHABETS)):
            return None
            
        plate_chars = PROVINCES[prov_idx] + ALPHABETS[alpha_idx]
        
        # 验证后续字符索引范围
        for code in indices[2:]:
            idx = int(code)
            if 0 <= idx < len(ADS):
                plate_chars += ADS[idx]
            else:
                return None  # 存在非法索引，抛弃该样本
                
        # 最终产物必须严格为 7 位或 8 位
        if len(plate_chars) not in [7, 8]:
            return None
            
        return plate_chars
        
    except ValueError:
        # 捕捉 int() 转换异常
        return None

# ================= 3. 核心处理逻辑 =================
DEVICE = 0
model = YOLO(YOLO_WEIGHT_PATH)  # 加载模型

def setup_directories():
    for color in ["blue", "green"]:
        for split in ["train", "val", "test"]:
            (OUT_ROOT / color / split).mkdir(parents=True, exist_ok=True)
    (OUT_ROOT / "classified_txts").mkdir(parents=True, exist_ok=True)

def process_and_crop(img_path, out_dir, plate_text, plate_type):
    """
    运行 YOLO，裁剪目标，强行 Resize，并以车牌直接命名保存 (支持中文路径)
    """
    # 替换为支持中文的读图函数
    img = cv_imread(str(img_path))
    if img is None:
        return None
    
    results = model.predict(img, conf=0.5, verbose=False, device=DEVICE)
    
    if len(results[0].boxes) == 0:
        return None
    
    best_box = results[0].boxes[0]
    x1, y1, x2, y2 = map(int, best_box.xyxy[0].cpu().numpy())
    
    crop_img = img[y1:y2, x1:x2]
    if crop_img.size == 0:
        return None
        
    resized_img = cv2.resize(crop_img, (94, 24))
    
    # 纯车牌命名，结合重名冲突防御机制
    new_filename = f"{plate_text}.jpg"
    save_path = out_dir / new_filename
    
    counter = 1
    while save_path.exists():
        new_filename = f"{plate_text}_{counter}.jpg"
        save_path = out_dir / new_filename
        counter += 1
        
    # 替换为支持中文的写图函数
    cv_imwrite(save_path, resized_img)
    
    rel_path = f"{out_dir.parent.name}/{out_dir.name}/{new_filename}"
    return f"{rel_path} {plate_text} {plate_type}\n"

# ================= 4. 任务调度 =================
def build_dataset():
    setup_directories()
    
    # --- 处理蓝牌 (未划分，需随机切分) ---
    print("开始处理蓝牌 (CCPD2019)...")
    blue_images = list(CCPD_BLUE_DIR.glob("*.jpg"))
    random.shuffle(blue_images)
    if MAX_BLUE_SAMPLES:
        blue_images = blue_images[:MAX_BLUE_SAMPLES]
        
    train_bound = int(len(blue_images) * SPLIT_RATIOS[0])
    val_bound = train_bound + int(len(blue_images) * SPLIT_RATIOS[1])
    
    txt_lines = {"train": [], "val": [], "test": []}
    
    for idx, img_path in enumerate(blue_images):
        if idx < train_bound:
            split = "train"
        elif idx < val_bound:
            split = "val"
        else:
            split = "test"
            
        plate_text = decode_ccpd_filename(img_path.name)
        if not plate_text or len(plate_text) != 7:
            continue
            
        out_dir = OUT_ROOT / "blue" / split
        line = process_and_crop(img_path, out_dir, plate_text, "普通蓝牌")
        if line:
            txt_lines[split].append(line)
            
    # --- 处理绿牌 (已划分，遍历目录) ---
    print("开始处理绿牌 (CCPD2020)...")
    for split in ["train", "val", "test"]:
        green_split_dir = CCPD_GREEN_DIR / split
        if not green_split_dir.exists():
            continue
            
        green_images = list(green_split_dir.glob("*.jpg"))
        random.shuffle(green_images)
        if MAX_GREEN_SAMPLES:
            # 按比例缩减抽取量，防止全部从 train 抽
            limit = int(MAX_GREEN_SAMPLES * (SPLIT_RATIOS[0] if split == "train" else SPLIT_RATIOS[1]))
            green_images = green_images[:limit]
            
        for img_path in green_images:
            plate_text = decode_ccpd_filename(img_path.name)
            if not plate_text or len(plate_text) != 8:
                continue
                
            out_dir = OUT_ROOT / "green" / split
            line = process_and_crop(img_path, out_dir, plate_text, "新能源小型车")
            if line:
                txt_lines[split].append(line)

    # --- 保存标签文件 ---
    print("正在生成 txt 标签文件...")
    txt_dir = OUT_ROOT / "classified_txts"
    for split in ["train", "val", "test"]:
        if txt_lines[split]:
            with open(txt_dir / f"yolo_crops_{split}.txt", "w", encoding="utf-8") as f:
                f.writelines(txt_lines[split])
                
    print(f"数据生成完毕！文件保存在: {OUT_ROOT}")

if __name__ == "__main__":
    build_dataset()