import os
import random
import shutil
from pathlib import Path

# 保证每次采样的结果一致
random.seed(42)

# ================= 配置区 =================
CRPD_DIR = Path("../datasets/CRPD_YOLO")
FINAL_YOLO_DIR = Path("../datasets/yolo_format")

# 蓝牌降采样率：如果一张图只有蓝牌，只有 25% 的概率保留它
CRPD_ONLY_BLUE_KEEP_RATE = 0.25

def setup_directories(base_dir):
    """创建标准的 YOLO 目录结构"""
    for split in ['train', 'val', 'test']:
        (base_dir / 'images' / split).mkdir(parents=True, exist_ok=True)
        (base_dir / 'labels' / split).mkdir(parents=True, exist_ok=True)

def migrate_and_filter_crpd():
    setup_directories(FINAL_YOLO_DIR)
    
    total_processed = 0
    total_saved = 0
    discarded_due_to_double_yellow = 0 # 统计因双黄牌丢弃的图片数
    
    for split in ['train', 'val', 'test']:
        img_dir = CRPD_DIR / 'images' / split
        lbl_dir = CRPD_DIR / 'labels' / split
        
        if not lbl_dir.exists():
            continue
            
        for txt_file in lbl_dir.glob("*.txt"):
            total_processed += 1
            
            with open(txt_file, 'r', encoding='utf-8') as f:
                lines = f.readlines()
                
            new_lines = []
            has_rare_class = False
            contains_double_yellow = False # 核心标志位
            
            for line in lines:
                parts = line.strip().split()
                if not parts:
                    continue
                    
                class_id = parts[0]
                
                # 触发“毒苹果”逻辑：发现双行黄牌，整图标记为丢弃
                if class_id == '3':
                    contains_double_yellow = True
                    break # 没必要继续读了，直接跳出内层循环
                    
                # 将 other (原类别 4) 映射为新类别 3
                if class_id == '4':
                    parts[0] = '3'
                    has_rare_class = True
                    
                # 记录是否包含单行黄牌 (原类别 2，保持 2 不变)
                if class_id == '2':
                    has_rare_class = True
                    
                new_lines.append(" ".join(parts) + "\n")
                
            # 执行“同生共死”丢弃逻辑
            if contains_double_yellow:
                discarded_due_to_double_yellow += 1
                continue
                
            if not new_lines:
                continue
                
            # 降采样逻辑：如果没有单行黄牌或白/黑牌(即全是蓝牌)，进行随机丢弃
            if not has_rare_class:
                if random.random() > CRPD_ONLY_BLUE_KEEP_RATE:
                    continue
            
            # 文件拷贝与重命名 (加前缀 crpd_)
            img_file = img_dir / (txt_file.stem + ".jpg")
            if img_file.exists():
                new_basename = f"crpd_{txt_file.name}"
                new_img_name = f"crpd_{img_file.name}"
                
                # 写入更新后的标签
                with open(FINAL_YOLO_DIR / 'labels' / split / new_basename, 'w', encoding='utf-8') as f:
                    f.writelines(new_lines)
                    
                # 拷贝图像
                shutil.copy(str(img_file), str(FINAL_YOLO_DIR / 'images' / split / new_img_name))
                total_saved += 1
                
    return total_processed, total_saved, discarded_due_to_double_yellow

# 执行 CRPD 迁移
print("开始过滤并迁移 CRPD 数据 (启动毒苹果丢弃策略)...")
p, s, d = migrate_and_filter_crpd()
print(f"CRPD 处理完成!")
print(f"扫描文件: {p}")
print(f"最终保留: {s}")
print(f"因包含双行黄牌被直接丢弃的整图数量: {d}")

'''
开始过滤并迁移 CRPD 数据 (启动毒苹果丢弃策略)...
CRPD 处理完成!
扫描文件: 15718
最终保留: 8918
因包含双行黄牌被直接丢弃的整图数量: 160
'''