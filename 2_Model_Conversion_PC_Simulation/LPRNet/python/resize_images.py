import cv2
import os
import numpy as np
# ===================== 配置参数（直接改这里）=====================
INPUT_DIR = r"D:\Yolo_LPR_RK3568_FPGA_Project\1_PC_Training\LPRNet_Pytorch\data\lprnet_train\train"    # 你的原始图片文件夹
OUTPUT_DIR = r"D:\Yolo_LPR_RK3568_FPGA_Project\2_Model_Conversion_PC_Simulation\LPRNet\data\test_imgs"  # 缩放后图片保存文件夹（自动创建）
TARGET_WIDTH = 94    # 模型输入宽度
TARGET_HEIGHT = 24   # 模型输入高度
# ===============================================================
# python resize_images.py

# 创建输出文件夹
os.makedirs(OUTPUT_DIR, exist_ok=True)

def resize_image(image_path, save_path):
    # 遍历所有图片
    for img_name in os.listdir(image_path):
        # 过滤图片格式
        if img_name.endswith(('.jpg', '.jpeg', '.png', '.bmp')):
            # 拼接完整路径
            img_path = os.path.join(image_path, img_name)
            out_path = os.path.join(save_path, img_name)
            
            # 读取图片
            img_data = np.fromfile(img_path, dtype=np.uint8)
            # 解码图片
            img = cv2.imdecode(img_data, cv2.IMREAD_COLOR)
            if img is None:
                print(f"跳过损坏图片：{img_name}")
                continue
            
            # 核心：缩放到 94x24（模型输入尺寸）
            resized_img = cv2.resize(img, (TARGET_WIDTH, TARGET_HEIGHT), interpolation=cv2.INTER_LINEAR)
            
            # 保存缩放后的图片（支持中文路径）
            ext = os.path.splitext(img_name)[1] or '.jpg'
            ok, encoded = cv2.imencode(ext, resized_img)
            if not ok:
                print(f"编码失败，跳过：{img_name}")
                continue
            encoded.tofile(out_path)

if __name__ == "__main__":
    print(f"正在处理图片，输入文件夹：{INPUT_DIR}，输出文件夹：{OUTPUT_DIR}")
    resize_image(INPUT_DIR, OUTPUT_DIR)