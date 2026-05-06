import os
import cv2
import numpy as np
from pathlib import Path
from concurrent.futures import ProcessPoolExecutor

# ================= 全局配置区 =================
# 旧数据集路径 (可以是父目录，代码会自动扫描所有子文件夹中的图片)
INPUT_DIR = Path(r"D:\Yolo_LPR_RK3568_FPGA_Project\1_PC_Training\datasets\lprnet_7char\val")
# 紧凑裁剪后的新输出路径
OUTPUT_DIR = Path(r"D:\Yolo_LPR_RK3568_FPGA_Project\1_PC_Training\datasets\lprnet_7char\lprnet_7char_tight_val")

# 安全外扩像素(Padding)。值越大越安全，建议值为 1 到 3
SAFE_PADDING = 1 
# 目标尺寸
TARGET_SIZE = (94, 24)

# ================= 核心视觉算法 =================
def cv_imread(file_path):
    """支持中文路径读取"""
    try:
        return cv2.imdecode(np.fromfile(str(file_path), dtype=np.uint8), cv2.IMREAD_COLOR)
    except Exception:
        return None

def cv_imwrite(file_path, img):
    """支持中文路径写入"""
    try:
        ext = os.path.splitext(str(file_path))[-1]
        cv2.imencode(ext, img)[1].tofile(str(file_path))
        return True
    except Exception:
        return False

def get_tight_crop(img, padding=SAFE_PADDING, target_size=TARGET_SIZE):
    """通过边缘投影算法自适应裁剪车牌留白"""
    # 1. 转换为灰度图
    gray = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
    
    # 2. 边缘检测：使用 Sobel 提取 X 方向（垂直）边缘，这最能反映字符笔画
    sobel_x = cv2.Sobel(gray, cv2.CV_16S, 1, 0, ksize=3)
    abs_x = cv2.convertScaleAbs(sobel_x)
    
    # 3. 大津法二值化，过滤掉微弱的噪点
    _, binary = cv2.threshold(abs_x, 0, 255, cv2.THRESH_BINARY + cv2.THRESH_OTSU)
    
    # 4. 投影计算
    v_proj = np.sum(binary, axis=0) # 垂直投影（寻找左右边界）
    h_proj = np.sum(binary, axis=1) # 水平投影（寻找上下边界）
    
    # 5. 设定阈值（最大波峰的 10%）
    v_threshold = np.max(v_proj) * 0.10
    h_threshold = np.max(h_proj) * 0.10
    
    # 6. 扫描寻找边界
    left, right = 0, len(v_proj) - 1
    for i in range(len(v_proj)):
        if v_proj[i] > v_threshold:
            left = i
            break
    for i in range(len(v_proj)-1, -1, -1):
        if v_proj[i] > v_threshold:
            right = i
            break
            
    top, bottom = 0, len(h_proj) - 1
    for i in range(len(h_proj)):
        if h_proj[i] > h_threshold:
            top = i
            break
    for i in range(len(h_proj)-1, -1, -1):
        if h_proj[i] > h_threshold:
            bottom = i
            break
            
    # 7. 施加绝对安全的 Padding，并防止越界
    H, W = img.shape[:2]
    x1 = max(0, left - padding)
    x2 = min(W, right + padding)
    y1 = max(0, top - padding)
    y2 = min(H, bottom + padding)
    
    # 8. 物理裁剪
    cropped = img[y1:y2, x1:x2]
    
    # 容错：如果图像本身很糊导致没找到边缘，或者切得过小，则退回原图
    if cropped.size == 0 or (x2 - x1) < W * 0.5:
        return cv2.resize(img, TARGET_SIZE)
        
    # 9. 拉伸回标准尺寸
    resized = cv2.resize(cropped, TARGET_SIZE)
    return resized


# ================= 多进程任务调度 =================
def process_single_image(img_path):
    """处理单张图片的任务单元"""
    try:
        # 保持原有的目录结构
        rel_path = img_path.relative_to(INPUT_DIR)
        out_path = OUTPUT_DIR / rel_path
        
        # 避免重复处理
        if out_path.exists():
            return True
            
        out_path.parent.mkdir(parents=True, exist_ok=True)
        
        img = cv_imread(img_path)
        if img is None:
            return False
            
        tight_img = get_tight_crop(img)
        cv_imwrite(out_path, tight_img)
        return True
    except Exception as e:
        print(f"处理失败 {img_path.name}: {e}")
        return False

def main():
    if not INPUT_DIR.exists():
        print(f"输入路径不存在: {INPUT_DIR}")
        return
        
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    
    print("正在扫描图片文件...")
    image_paths = list(INPUT_DIR.rglob("*.jpg"))
    total_imgs = len(image_paths)
    print(f"共发现 {total_imgs} 张图片，开始多进程紧凑重裁...")
    
    success_count = 0
    # 使用所有可用的 CPU 核心进行处理
    with ProcessPoolExecutor() as executor:
        results = executor.map(process_single_image, image_paths, chunksize=100)
        
        for i, res in enumerate(results):
            if res:
                success_count += 1
            if (i + 1) % 5000 == 0:
                print(f"进度: {i + 1} / {total_imgs}")
                
    print(f"处理完成！成功 {success_count} 张，失败 {total_imgs - success_count} 张。")
    print(f"新数据集已保存在: {OUTPUT_DIR}")

if __name__ == "__main__":
    main()