#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
CBLPRD-330k 数据集处理脚本，用于适配 LPRNet 训练
功能：
1. 将原始 128x48 图片 resize 为 LPRNet 要求的 94x24 尺寸
2. 按省份、车牌类别分别生成分类的 txt 标注文件
3. 支持中文路径，处理 33 万张数据带进度条
4. 多线程+GPU加速
"""
import os
import cv2
import numpy as np
from tqdm import tqdm
import torch
import torchvision.transforms.functional as TF

from concurrent.futures import ThreadPoolExecutor, as_completed
from threading import Lock

# -------------------------- 请修改此处的数据集根目录 --------------------------
ROOT_DIR = r"D:\Yolo_LPR_RK3568_FPGA_Project\1_PC_Training\datasets\CBLPRD-330k"
# -----------------------------------------------------------------------------

# ===================== GPU 初始化 =====================
DEVICE = torch.device("cuda" if torch.cuda.is_available() else "cpu")
print(f"使用设备: {DEVICE}")
# 目标尺寸 LPRNet: 宽94, 高24
TARGET_SIZE = (24, 94)  # H, W
MAX_WORKERS = 32
# ======================================================

# 输出目录配置
OUT_IMG_DIR = os.path.join(ROOT_DIR, "CBLPRD-330k_94x24")
OUT_TXT_DIR = os.path.join(ROOT_DIR, "classified_txts")

# 特殊车牌标识（遍历全车牌检测，各自独立分类）
SPECIAL_PLATE_CHARS = {'港', '澳', '挂', '学', '领', '使', '临'}

def process_single_txt(txt_file_name):
    """
    处理单个txt标注文件，处理图片并收集分类数据
    """
    txt_path = os.path.join(ROOT_DIR, txt_file_name)
    if not os.path.exists(txt_path):
        print(f"警告：未找到 {txt_file_name}，跳过该文件")
        return None, None
    
    print(f"\n开始处理 {txt_file_name} ...")
    province_data = {}
    type_data = {}
    lock = Lock() # 线程锁，保证数据安全
    
    # 读取所有行
    with open(txt_path, 'r', encoding='utf-8') as f:
        lines = [line.strip() for line in f.readlines() if line.strip()]

    # ===================== 原图像处理代码已注释，解开即可恢复图片处理 =====================
    # # 单张图片处理函数
    # def process_task(line):
    #     parts = line.split(' ', 2)
    #     if len(parts) != 3:
    #         return None
        
    #     old_img_path, plate, plate_type = parts
    #     old_full_img_path = os.path.join(ROOT_DIR, old_img_path)
    #     img_name = os.path.basename(old_img_path)
    #     new_full_img_path = os.path.join(OUT_IMG_DIR, img_name)
    #     new_img_path = f"CBLPRD-330k_94x24/{img_name}"

    #     try:
    #         img = cv2.imdecode(np.fromfile(old_full_img_path, dtype=np.uint8), cv2.IMREAD_COLOR)
    #         if img is None:
    #             return None
            
    #         # GPU 加速 Resize
    #         img_tensor = torch.from_numpy(img).permute(2, 0, 1).float() / 255.0
    #         img_tensor = img_tensor.to(DEVICE)
    #         img_resized = TF.resize(img_tensor, TARGET_SIZE, antialias=True)
    #         img_resized_np = (img_resized.permute(1, 2, 0).cpu().numpy() * 255).astype(np.uint8)
            
    #         cv2.imencode('.jpg', img_resized_np)[1].tofile(new_full_img_path)
    #         return new_img_path, plate, plate_type, new_full_img_path
    #     except:
    #         return None

    # # ===================== 多线程执行（核心优化）=====================
    # with ThreadPoolExecutor(max_workers=MAX_WORKERS) as executor:
    #     futures = [executor.submit(process_task, line) for line in lines]
    #     for future in tqdm(as_completed(futures), total=len(futures), desc=f"处理 {txt_file_name} 图片"):
    #         result = future.result()
    #         if result is None:
    #             continue
            
    #         new_img_path, plate, plate_type, _ = result
    #         new_line = f"{new_img_path} {plate} {plate_type}"
            
    #         # 线程安全写入数据
    #         with lock:
    #             if len(plate) > 0:
    #                 province = plate[0]
    #                 if province not in province_data:
    #                     province_data[province] = []
    #                 province_data[province].append(new_line)
                
    #             if plate_type not in type_data:
    #                 type_data[plate_type] = []
    #             type_data[plate_type].append(new_line)
    # ==================================================================================

    # ===================== 纯文本分类逻辑：特殊字符全车牌检测，各自独立分类 =====================
    for line in tqdm(lines, desc=f"处理 {txt_file_name} 标注"):
        parts = line.split(' ', 2)
        if len(parts) != 3:
            continue
        
        old_img_path, plate, plate_type = parts
        img_name = os.path.basename(old_img_path)
        new_img_path = f"CBLPRD-330k_94x24/{img_name}"
        new_line = f"{new_img_path} {plate} {plate_type}"

        if len(plate) > 0:
            # 遍历车牌，查找是否包含特殊字符
            province = None
            for c in plate:
                if c in SPECIAL_PLATE_CHARS:
                    province = c
                    break  # 找到第一个特殊字符即作为分类
            # 无特殊字符则用首字符作为省份分类
            if province is None:
                province = plate[0]
            
            if province not in province_data:
                province_data[province] = []
            province_data[province].append(new_line)
        
        # 车牌类型分类不变
        if plate_type not in type_data:
            type_data[plate_type] = []
        type_data[plate_type].append(new_line)
    # ==========================================================================================
    
    return province_data, type_data

def generate_txt_only(txt_file_name):
    """
    仅生成分类标注文件，不处理图片（避免重复处理）
    """
    txt_path = os.path.join(ROOT_DIR, txt_file_name)
    if not os.path.exists(txt_path):
        print(f"警告：未找到 {txt_file_name}，跳过该文件")
        return None, None
    
    print(f"\n生成 {txt_file_name} 分类标注（不处理图片）...")
    province_data = {}
    type_data = {}
    
    with open(txt_path, 'r', encoding='utf-8') as f:
        lines = [line.strip() for line in f.readlines() if line.strip()]
    
    for line in tqdm(lines, desc=f"生成 {txt_file_name} 标注"):
        parts = line.split(' ', 2)
        if len(parts) != 3:
            continue
        
        old_img_path, plate, plate_type = parts
        img_name = os.path.basename(old_img_path)
        new_img_path = f"CBLPRD-330k_94x24/{img_name}"
        new_line = f"{new_img_path} {plate} {plate_type}"
        
        if len(plate) > 0:
            # 全车牌检测特殊字符，独立分类
            province = None
            for c in plate:
                if c in SPECIAL_PLATE_CHARS:
                    province = c
                    break
            if province is None:
                province = plate[0]
            
            if province not in province_data:
                province_data[province] = []
            province_data[province].append(new_line)
        
        if plate_type not in type_data:
            type_data[plate_type] = []
        type_data[plate_type].append(new_line)
    
    return province_data, type_data

def save_classified_txts(province_data, type_data, suffix=""):
    """
    将分类后的数据保存为txt文件
    """
    print(f"\n保存{suffix}省份分类文件...")
    for province, lines in province_data.items():
        safe_province = province.replace(' ', '_')
        txt_name = f"province_{safe_province}{suffix}.txt"
        txt_path = os.path.join(OUT_TXT_DIR, txt_name)
        with open(txt_path, 'w', encoding='utf-8') as f:
            f.write('\n'.join(lines))
        print(f"  分类 {province}: {len(lines)} 条样本，已保存到 {txt_name}")
    
    print(f"\n保存{suffix}车牌类型分类文件...")
    for plate_type, lines in type_data.items():
        safe_type = plate_type.replace(' ', '_')
        txt_name = f"type_{safe_type}{suffix}.txt"
        txt_path = os.path.join(OUT_TXT_DIR, txt_name)
        with open(txt_path, 'w', encoding='utf-8') as f:
            f.write('\n'.join(lines))
        print(f"  类型 {plate_type}: {len(lines)} 条样本，已保存到 {txt_name}")

def main():

    os.makedirs(OUT_IMG_DIR, exist_ok=True)
    os.makedirs(OUT_TXT_DIR, exist_ok=True)
    
    print("="*60)
    print("CBLPRD-330k -> LPRNet 数据处理工具")
    print(f"数据集根目录: {ROOT_DIR}")
    print(f"处理后图片目录: {OUT_IMG_DIR}")
    print(f"分类标注目录: {OUT_TXT_DIR}")
    print("="*60)
    
    # 1. 处理 data.txt：生成图片 + 分类标注
    province_data, type_data = process_single_txt("data.txt")
    save_classified_txts(province_data, type_data, suffix="_data")
    # 2. 处理 train.txt：仅生成分类标注，不处理图片
    train_province, train_type = generate_txt_only("train.txt")
    save_classified_txts(train_province, train_type, suffix="_train")
    # 3. 处理 val.txt：仅生成分类标注，不处理图片
    val_province, val_type = generate_txt_only("val.txt")
    save_classified_txts(val_province, val_type, suffix="_val")

    print("\n" + "="*60)
    print("所有处理完成！")
    print(f"1. 处理后的 94x24 图片已保存到: {OUT_IMG_DIR}")
    print(f"2. 分类后的标注文件已保存到: {OUT_TXT_DIR}")
    print("   - 省份/特殊车牌分类文件: province_京_data.txt、province_港_data.txt 等")
    print("   - 车牌类型分类文件前缀: type_xxx_xxx.txt")
    print("="*60)

if __name__ == "__main__":
    main()

'''
保存_data省份分类文件...
  分类 学: 12566 条样本，已保存到 province_学_data.txt
  分类 湘: 9504 条样本，已保存到 province_湘_data.txt
  分类 晋: 9496 条样本，已保存到 province_晋_data.txt
  分类 鄂: 9538 条样本，已保存到 province_鄂_data.txt
  分类 闽: 9553 条样本，已保存到 province_闽_data.txt
  分类 挂: 9893 条样本，已保存到 province_挂_data.txt
  分类 云: 9667 条样本，已保存到 province_云_data.txt
  分类 辽: 9539 条样本，已保存到 province_辽_data.txt
  分类 藏: 9590 条样本，已保存到 province_藏_data.txt
  分类 黑: 9596 条样本，已保存到 province_黑_data.txt
  分类 豫: 9453 条样本，已保存到 province_豫_data.txt
  分类 使: 6651 条样本，已保存到 province_使_data.txt
  分类 桂: 9605 条样本，已保存到 province_桂_data.txt
  分类 新: 9697 条样本，已保存到 province_新_data.txt
  分类 粤: 9579 条样本，已保存到 province_粤_data.txt
  分类 澳: 3269 条样本，已保存到 province_澳_data.txt
  分类 陕: 9621 条样本，已保存到 province_陕_data.txt
  分类 苏: 9535 条样本，已保存到 province_苏_data.txt
  分类 贵: 9554 条样本，已保存到 province_贵_data.txt
  分类 甘: 9659 条样本，已保存到 province_甘_data.txt
  分类 宁: 9435 条样本，已保存到 province_宁_data.txt
  分类 川: 9568 条样本，已保存到 province_川_data.txt
  分类 津: 9615 条样本，已保存到 province_津_data.txt
  分类 领: 6565 条样本，已保存到 province_领_data.txt
  分类 冀: 9633 条样本，已保存到 province_冀_data.txt
  分类 皖: 9495 条样本，已保存到 province_皖_data.txt
  分类 渝: 9393 条样本，已保存到 province_渝_data.txt
  分类 临: 3793 条样本，已保存到 province_临_data.txt
  分类 吉: 9575 条样本，已保存到 province_吉_data.txt
  分类 沪: 9593 条样本，已保存到 province_沪_data.txt
  分类 鲁: 9526 条样本，已保存到 province_鲁_data.txt
  分类 赣: 9466 条样本，已保存到 province_赣_data.txt
  分类 京: 9616 条样本，已保存到 province_京_data.txt
  分类 浙: 9578 条样本，已保存到 province_浙_data.txt
  分类 青: 9494 条样本，已保存到 province_青_data.txt
  分类 蒙: 9555 条样本，已保存到 province_蒙_data.txt
  分类 港: 3264 条样本，已保存到 province_港_data.txt
  分类 琼: 9381 条样本，已保存到 province_琼_data.txt

保存_data车牌类型分类文件...
  类型 单层黄牌: 52630 条样本，已保存到 type_单层黄牌_data.txt
  类型 普通蓝牌: 78960 条样本，已保存到 type_普通蓝牌_data.txt
  类型 黑色车牌: 26315 条样本，已保存到 type_黑色车牌_data.txt
  类型 双层黄牌: 26315 条样本，已保存到 type_双层黄牌_data.txt
  类型 新能源小型车: 78945 条样本，已保存到 type_新能源小型车_data.txt
  类型 新能源大型车: 52630 条样本，已保存到 type_新能源大型车_data.txt
  类型 拖拉机绿牌: 26315 条样本，已保存到 type_拖拉机绿牌_data.txt

保存_train省份分类文件...
  分类 粤: 9100 条样本，已保存到 province_粤_train.txt
  分类 藏: 9097 条样本，已保存到 province_藏_train.txt
  分类 晋: 9021 条样本，已保存到 province_晋_train.txt
  分类 苏: 9032 条样本，已保存到 province_苏_train.txt
  分类 冀: 9144 条样本，已保存到 province_冀_train.txt
  分类 京: 9123 条样本，已保存到 province_京_train.txt
  分类 云: 9201 条样本，已保存到 province_云_train.txt
  分类 学: 11917 条样本，已保存到 province_学_train.txt
  分类 青: 9042 条样本，已保存到 province_青_train.txt
  分类 甘: 9150 条样本，已保存到 province_甘_train.txt
  分类 湘: 9022 条样本，已保存到 province_湘_train.txt
  分类 桂: 9082 条样本，已保存到 province_桂_train.txt
  分类 浙: 9082 条样本，已保存到 province_浙_train.txt
  分类 黑: 9127 条样本，已保存到 province_黑_train.txt
  分类 吉: 9088 条样本，已保存到 province_吉_train.txt
  分类 挂: 9395 条样本，已保存到 province_挂_train.txt
  分类 琼: 8935 条样本，已保存到 province_琼_train.txt
  分类 澳: 3108 条样本，已保存到 province_澳_train.txt
  分类 沪: 9110 条样本，已保存到 province_沪_train.txt
  分类 闽: 9101 条样本，已保存到 province_闽_train.txt
  分类 贵: 9084 条样本，已保存到 province_贵_train.txt
  分类 辽: 9083 条样本，已保存到 province_辽_train.txt
  分类 川: 9071 条样本，已保存到 province_川_train.txt
  分类 蒙: 9105 条样本，已保存到 province_蒙_train.txt
  分类 使: 6286 条样本，已保存到 province_使_train.txt
  分类 豫: 8950 条样本，已保存到 province_豫_train.txt
  分类 鲁: 9039 条样本，已保存到 province_鲁_train.txt
  分类 皖: 9019 条样本，已保存到 province_皖_train.txt
  分类 新: 9238 条样本，已保存到 province_新_train.txt
  分类 赣: 9014 条样本，已保存到 province_赣_train.txt
  分类 领: 6243 条样本，已保存到 province_领_train.txt
  分类 陕: 9132 条样本，已保存到 province_陕_train.txt
  分类 港: 3108 条样本，已保存到 province_港_train.txt
  分类 临: 3602 条样本，已保存到 province_临_train.txt
  分类 渝: 8951 条样本，已保存到 province_渝_train.txt
  分类 鄂: 9090 条样本，已保存到 province_鄂_train.txt
  分类 宁: 8945 条样本，已保存到 province_宁_train.txt
  分类 津: 9168 条样本，已保存到 province_津_train.txt

保存_train车牌类型分类文件...
  类型 新能源大型车: 50024 条样本，已保存到 type_新能源大型车_train.txt
  类型 新能源小型车: 74893 条样本，已保存到 type_新能源小型车_train.txt
  类型 单层黄牌: 50065 条样本，已保存到 type_单层黄牌_train.txt
  类型 普通蓝牌: 75060 条样本，已保存到 type_普通蓝牌_train.txt
  类型 双层黄牌: 25002 条样本，已保存到 type_双层黄牌_train.txt
  类型 拖拉机绿牌: 24983 条样本，已保存到 type_拖拉机绿牌_train.txt
  类型 黑色车牌: 24978 条样本，已保存到 type_黑色车牌_train.txt

保存_val省份分类文件...
  分类 学: 649 条样本，已保存到 province_学_val.txt
  分类 湘: 482 条样本，已保存到 province_湘_val.txt
  分类 晋: 475 条样本，已保存到 province_晋_val.txt
  分类 鄂: 448 条样本，已保存到 province_鄂_val.txt
  分类 闽: 452 条样本，已保存到 province_闽_val.txt
  分类 挂: 498 条样本，已保存到 province_挂_val.txt
  分类 云: 466 条样本，已保存到 province_云_val.txt
  分类 辽: 456 条样本，已保存到 province_辽_val.txt
  分类 藏: 493 条样本，已保存到 province_藏_val.txt
  分类 黑: 469 条样本，已保存到 province_黑_val.txt
  分类 豫: 503 条样本，已保存到 province_豫_val.txt
  分类 使: 365 条样本，已保存到 province_使_val.txt
  分类 桂: 523 条样本，已保存到 province_桂_val.txt
  分类 新: 459 条样本，已保存到 province_新_val.txt
  分类 粤: 479 条样本，已保存到 province_粤_val.txt
  分类 澳: 161 条样本，已保存到 province_澳_val.txt
  分类 陕: 489 条样本，已保存到 province_陕_val.txt
  分类 苏: 503 条样本，已保存到 province_苏_val.txt
  分类 贵: 470 条样本，已保存到 province_贵_val.txt
  分类 甘: 509 条样本，已保存到 province_甘_val.txt
  分类 宁: 490 条样本，已保存到 province_宁_val.txt
  分类 川: 497 条样本，已保存到 province_川_val.txt
  分类 津: 447 条样本，已保存到 province_津_val.txt
  分类 领: 322 条样本，已保存到 province_领_val.txt
  分类 冀: 489 条样本，已保存到 province_冀_val.txt
  分类 皖: 476 条样本，已保存到 province_皖_val.txt
  分类 渝: 442 条样本，已保存到 province_渝_val.txt
  分类 临: 191 条样本，已保存到 province_临_val.txt
  分类 吉: 487 条样本，已保存到 province_吉_val.txt
  分类 沪: 483 条样本，已保存到 province_沪_val.txt
  分类 鲁: 487 条样本，已保存到 province_鲁_val.txt
  分类 赣: 452 条样本，已保存到 province_赣_val.txt
  分类 京: 493 条样本，已保存到 province_京_val.txt
  分类 浙: 496 条样本，已保存到 province_浙_val.txt
  分类 青: 452 条样本，已保存到 province_青_val.txt
  分类 蒙: 450 条样本，已保存到 province_蒙_val.txt
  分类 港: 156 条样本，已保存到 province_港_val.txt
  分类 琼: 446 条样本，已保存到 province_琼_val.txt

保存_val车牌类型分类文件...
  类型 单层黄牌: 2565 条样本，已保存到 type_单层黄牌_val.txt
  类型 普通蓝牌: 3900 条样本，已保存到 type_普通蓝牌_val.txt
  类型 黑色车牌: 1337 条样本，已保存到 type_黑色车牌_val.txt
  类型 双层黄牌: 1313 条样本，已保存到 type_双层黄牌_val.txt
  类型 新能源小型车: 4052 条样本，已保存到 type_新能源小型车_val.txt
  类型 新能源大型车: 2606 条样本，已保存到 type_新能源大型车_val.txt
  类型 拖拉机绿牌: 1332 条样本，已保存到 type_拖拉机绿牌_val.txt  
'''