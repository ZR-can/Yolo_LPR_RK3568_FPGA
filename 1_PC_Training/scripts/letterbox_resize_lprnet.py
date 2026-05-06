import cv2
import numpy as np
import torch

def letterbox_resize(img, target_size=(94, 24)):
    """
    保持宽高比的车牌完美缩放 (Letterboxing)
    img: 输入图像 (H, W, C)
    target_size: (目标宽, 目标高) -> 默认 (94, 24)
    """
    h, w = img.shape[:2]
    target_w, target_h = target_size

    # 计算缩放比例 (取宽和高缩放比例中较小的那个)
    # 这样能保证图像按照原比例缩放后，长边刚好贴合目标尺寸，短边留出空隙
    scale = min(target_w / w, target_h / h)
    new_w, new_h = int(w * scale), int(h * scale)

    # 第一步：等比例缩放图像 (此时字符不会变形，只是变小了)
    # INTER_AREA 在缩小时效果最好，能有效抗锯齿
    resized_img = cv2.resize(img, (new_w, new_h), interpolation=cv2.INTER_AREA)

    # 第二步：计算上下左右需要填充的像素量，使其精确达到 94x24
    top = (target_h - new_h) // 2
    bottom = target_h - new_h - top
    left = (target_w - new_w) // 2
    right = target_w - new_w - left

    # 第三步：执行边缘像素复制填充 (强烈推荐用于车牌)
    # 它会将车牌最外侧的颜色平滑向外延伸，防止 CNN 误提取硬边缘特征
    padded_img = cv2.copyMakeBorder(
        resized_img, 
        top, bottom, left, right, 
        cv2.BORDER_REPLICATE
    )

    return padded_img

# ================= 在你的 Dataset 中的应用方式 =================
# img = cv2.imread(img_path)
# img = cv2.cvtColor(img, cv2.COLOR_BGR2RGB)

# 替换原先的普通 cv2.resize(img, (94, 24))
# img = letterbox_resize(img, target_size=(94, 24))

# img = img.astype('float32') / 255.0
# img = torch.from_numpy(img).permute(2, 0, 1)
if __name__ == "__main__":
    # 测试代码，验证 letterbox_resize 的效果
    test_img_path = r"D:\Yolo_LPR_RK3568_FPGA_Project\1_PC_Training\datasets\CBLPRD-330k\CBLPRD-330k\000000023.jpg"
    
    img = cv2.imread(test_img_path)
    resized_img = letterbox_resize(img, target_size=(94, 24))
    resized_img1 = cv2.resize(img, (94, 24), interpolation=cv2.INTER_AREA)
    # 显示原图和处理后的图像
    cv2.imshow("Original Image", img)
    cv2.imshow("Letterbox Resized Image", resized_img)
    cv2.imshow("Regular Resized Image", resized_img1)
    cv2.waitKey(0)
    cv2.destroyAllWindows()

'''
import os
import cv2
import numpy as np
from pathlib import Path

# ================= 配置区 =================
# CRPD 原始数据集路径 (必须是包含多边形坐标和真实字符的原始 txt)
CRPD_ROOT = Path("../datasets/CRPD")
# 裁剪后警车/白牌图片的保存目录
OUT_IMG_DIR = Path("../datasets/CBLPRD-330k/crpd_white_plates")
# LPRNet 格式的 txt 标签追加目录
OUT_TXT_PATH = Path("../datasets/CBLPRD-330k/classified_txts/type_白色车牌_train.txt")

TARGET_W, TARGET_H = 94, 24


def order_quad_points(pts):
    """将四边形点排序为: 左上、右上、右下、左下。"""
    pts = np.asarray(pts, dtype=np.float32)
    s = pts.sum(axis=1)
    diff = np.diff(pts, axis=1).reshape(-1)

    ordered = np.zeros((4, 2), dtype=np.float32)
    ordered[0] = pts[np.argmin(s)]      # 左上
    ordered[2] = pts[np.argmax(s)]      # 右下
    ordered[1] = pts[np.argmin(diff)]   # 右上
    ordered[3] = pts[np.argmax(diff)]   # 左下
    return ordered


def warp_plate_to_94x24(img, coords):
    """按四边形透视矫正车牌并缩放到 94x24。"""
    src = order_quad_points(coords)
    dst = np.array([
        [0, 0],
        [TARGET_W - 1, 0],
        [TARGET_W - 1, TARGET_H - 1],
        [0, TARGET_H - 1],
    ], dtype=np.float32)

    matrix = cv2.getPerspectiveTransform(src, dst)
    return cv2.warpPerspective(
        img,
        matrix,
        (TARGET_W, TARGET_H),
        flags=cv2.INTER_CUBIC,
        borderMode=cv2.BORDER_REPLICATE,
    )

def crop_white_plates_from_crpd():
    OUT_IMG_DIR.mkdir(parents=True, exist_ok=True)
    
    saved_count = 0
    labels_to_write = []

    # 遍历 CRPD 所有子集 (单车、双车、多车)
    for subset in ["CRPD_single", "CRPD_double", "CRPD_multi"]:
        subset_path = CRPD_ROOT / subset
        if not subset_path.exists():
            continue

        for split in ["train", "val", "test"]:
            img_dir = subset_path / split / "images"
            lbl_dir = subset_path / split / "labels"
            
            if not lbl_dir.exists():
                continue

            for txt_file in lbl_dir.glob("*.txt"):
                with open(txt_file, 'r', encoding='utf-8') as f:
                    lines = f.readlines()

                img_path = img_dir / (txt_file.stem + ".jpg")
                img = None # 延迟读取，只有遇到白牌才读图

                for idx, line in enumerate(lines):
                    parts = line.strip().split()
                    if len(parts) < 10:
                        continue
                    
                    plate_type = parts[8]
                    plate_text = parts[9]

                    # 目标锁定：类型 3 (白牌)
                    if plate_type == '3':
                        if img is None:
                            img = cv2.imread(str(img_path))
                            if img is None:
                                break

                        # 提取四角点坐标
                        coords = np.array(list(map(float, parts[:8])), dtype=np.float32).reshape(4, 2)

                        # 按四边形进行透视矫正，并输出为 94x24
                        crop_img = warp_plate_to_94x24(img, coords)
                        if crop_img.size == 0:
                            continue

                        # 保存裁剪图像
                        new_img_name = f"crpd_white_{subset}_{split}_{txt_file.stem}_{idx}.jpg"
                        cv2.imwrite(str(OUT_IMG_DIR / new_img_name), crop_img)
                        
                        # 构建 LPRNet 格式的标签: 相对路径 车牌号 车牌类型
                        rel_path = f"crpd_white_plates/{new_img_name}"
                        labels_to_write.append(f"{rel_path} {plate_text} 白色车牌\n")
                        saved_count += 1

    # 将提取的标签写入 txt
    with open(OUT_TXT_PATH, 'a', encoding='utf-8') as f:
        f.writelines(labels_to_write)

    print(f"提取完成！共从 CRPD 中裁剪出 {saved_count} 张白牌/警车，标签已追加至 {OUT_TXT_PATH.name}")

if __name__ == "__main__":
    crop_white_plates_from_crpd()
'''