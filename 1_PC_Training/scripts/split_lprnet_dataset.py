import os
import shutil
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor, as_completed

# ================= 配置区 =================
# 原始数据集根目录
SOURCE_ROOT = Path(r"D:\Yolo_LPR_RK3568_FPGA_Project\1_PC_Training\datasets\lprnet_format")
SOURCE_TXT_DIR = SOURCE_ROOT / "classified_txts"

# 目标数据集根目录
TARGET_7CHAR = Path(r"D:\Yolo_LPR_RK3568_FPGA_Project\1_PC_Training\datasets\lprnet_7char")
TARGET_8CHAR = Path(r"D:\Yolo_LPR_RK3568_FPGA_Project\1_PC_Training\datasets\lprnet_8char")

def setup_dirs(base_path):
    """创建目标目录结构"""
    (base_path / "classified_txts").mkdir(parents=True, exist_ok=True)
    (base_path / "train").mkdir(parents=True, exist_ok=True)
    (base_path / "val").mkdir(parents=True, exist_ok=True)

def split_lpr_dataset():
    # 1. 初始化目录
    setup_dirs(TARGET_7CHAR)
    setup_dirs(TARGET_8CHAR)
    
    # 2. 遍历所有的 type_*.txt 文件
    txt_files = list(SOURCE_TXT_DIR.glob("type_*.txt"))
    txt_files = [f for f in txt_files if f.name.endswith(("_train.txt", "_val.txt"))]
    print(f"找到 {len(txt_files)} 个标签文件，准备开始分流...")

    stats = {"7char": 0, "8char": 0}
    copy_jobs = []
    output_cache_7char = {}
    output_cache_8char = {}

    for txt_path in txt_files:
        # 判断是 train 还是 val
        split_type = "train" if "train" in txt_path.name else "val"
        
        new_7char_lines = []
        new_8char_lines = []

        with open(txt_path, 'r', encoding='utf-8') as f:
            for line in f:
                parts = line.strip().split()
                if len(parts) < 2: continue
                
                rel_path = parts[0]   # 原始路径通常包含子目录名，如 train_lprnet/xxx.jpg
                plate_num = parts[1]
                plate_type = parts[2] if len(parts) > 2 else ""
                
                # 获取原始图片的绝对路径
                src_img_path = SOURCE_ROOT / rel_path
                if not src_img_path.exists():
                    continue

                # 根据长度判定去向
                if len(plate_num) == 7:
                    target_base = TARGET_7CHAR
                    stats["7char"] += 1
                    target_list = new_7char_lines
                elif len(plate_num) == 8:
                    target_base = TARGET_8CHAR
                    stats["8char"] += 1
                    target_list = new_8char_lines
                else:
                    continue # 异常长度忽略

                # 复制图片到目标文件夹 (直接放在 train 或 val 下)
                dest_img_path = target_base / split_type / src_img_path.name
                copy_jobs.append((src_img_path, dest_img_path))
                
                # 重写标签行：新的相对路径为 "train/文件名" 或 "val/文件名"
                new_rel_path = f"{split_type}/{src_img_path.name}"
                target_list.append(f"{new_rel_path} {plate_num} {plate_type}\n")

        # 暂存新的 txt 内容，等复制完成后统一写入
        if new_7char_lines:
            output_cache_7char[txt_path.name] = new_7char_lines
        if new_8char_lines:
            output_cache_8char[txt_path.name] = new_8char_lines

    # 3. 并发复制图片并展示进度
    total_jobs = len(copy_jobs)
    if total_jobs > 0:
        max_workers = min(32, max(4, (os.cpu_count() or 4) * 2))
        print(f"开始复制 {total_jobs} 张图片，使用 {max_workers} 个线程...")

        progress_step = max(1, total_jobs // 100)  # 约每 1% 刷新一次
        copied = 0

        with ThreadPoolExecutor(max_workers=max_workers) as executor:
            futures = [executor.submit(shutil.copy2, src, dst) for src, dst in copy_jobs]
            for future in as_completed(futures):
                future.result()
                copied += 1
                if copied % progress_step == 0 or copied == total_jobs:
                    percent = copied * 100.0 / total_jobs
                    print(f"\r复制进度: {copied}/{total_jobs} ({percent:.1f}%)", end="", flush=True)
        print()
    else:
        print("没有可复制的图片任务。")

    # 4. 写入新的 txt 文件
    for file_name, lines in output_cache_7char.items():
        with open(TARGET_7CHAR / "classified_txts" / file_name, 'w', encoding='utf-8') as f:
            f.writelines(lines)

    for file_name, lines in output_cache_8char.items():
        with open(TARGET_8CHAR / "classified_txts" / file_name, 'w', encoding='utf-8') as f:
            f.writelines(lines)

    print("-" * 50)
    print("数据集迁移完成！")
    print(f"7位车牌总计: {stats['7char']} 张 -> {TARGET_7CHAR}")
    print(f"8位车牌总计: {stats['8char']} 张 -> {TARGET_8CHAR}")
    print("-" * 50)

if __name__ == "__main__":
    split_lpr_dataset()

'''
找到 12 个标签文件，准备开始分流...
开始复制 289704 张图片，使用 32 个线程...
复制进度: 289704/289704 (100.0%)
--------------------------------------------------
数据集迁移完成！
7位车牌总计: 158129 张 -> D:\Yolo_LPR_RK3568_FPGA_Project\1_PC_Training\datasets\lprnet_7char
8位车牌总计: 131575 张 -> D:\Yolo_LPR_RK3568_FPGA_Project\1_PC_Training\datasets\lprnet_8char
'''