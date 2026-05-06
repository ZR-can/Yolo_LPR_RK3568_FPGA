#python quant_dataset.py

import os
import cv2
import numpy as np

IMG_SIZE = (640, 640)  # 模型输入尺寸
pad_color = (0, 0, 0)  # 黑色填充，必须和推理一致

def letter_box(im, new_shape, pad_color=(0,0,0), info_need=False):
    # Resize and pad image while meeting stride-multiple constraints
    shape = im.shape[:2]  # current shape [height, width]
    if isinstance(new_shape, int):
        new_shape = (new_shape, new_shape)

    # Scale ratio
    r = min(new_shape[0] / shape[0], new_shape[1] / shape[1])

    # Compute padding
    ratio = r  # width, height ratios
    new_unpad = int(round(shape[1] * r)), int(round(shape[0] * r))
    dw, dh = new_shape[1] - new_unpad[0], new_shape[0] - new_unpad[1]  # wh padding

    dw /= 2  # divide padding into 2 sides
    dh /= 2

    if shape[::-1] != new_unpad:  # resize
        im = cv2.resize(im, new_unpad, interpolation=cv2.INTER_LINEAR)
    top, bottom = int(round(dh - 0.1)), int(round(dh + 0.1))
    left, right = int(round(dw - 0.1)), int(round(dw + 0.1))
    im = cv2.copyMakeBorder(im, top, bottom, left, right, cv2.BORDER_CONSTANT, value=pad_color)  # add border
    
    if info_need is True:
        return im, ratio, (dw, dh)
    else:
        return im

# ===================== 生成 RKNN 量化数据集 =====================
def generate_rknn_quant_dataset(
        input_img_dir="../model/raw_quant_images",    # 原始图片文件夹
        output_dir="../model/rknn_quant_dataset", # 输出预处理后量化用图片
        txt_path="../model/dataset.txt"           # 给convert.py用的量化txt
):
    os.makedirs(output_dir, exist_ok=True)

    # 支持的图片格式
    img_suffix = [".jpg", ".jpeg", ".png", ".bmp", ".JPG", ".JPEG", ".PNG", ".BMP"]
    img_list = [f for f in os.listdir(input_img_dir) if any(f.endswith(s) for s in img_suffix)]

    with open(txt_path, "w") as f:
        for idx, img_name in enumerate(img_list):
            print(f"处理 {idx+1}/{len(img_list)}: {img_name}")

            # 1. 读图
            img_path = os.path.join(input_img_dir, img_name)
            img = cv2.imread(img_path)
            if img is None:
                continue

            # 2. letter_box
            img = letter_box(img, new_shape=IMG_SIZE, pad_color=pad_color)

            # 3. BGR -> RGB
            img = cv2.cvtColor(img, cv2.COLOR_BGR2RGB)

            # 4. 保存预处理后的图片
            save_filename = f"quant_{idx:03d}.jpg"
            save_path = os.path.join(output_dir, save_filename)
            cv2.imwrite(save_path, img)

            # 5. 写入 dataset.txt（每行一张图路径）
            f.write(f"./rknn_quant_dataset/{save_filename}\n")

    print(f"\n生成完成")
    print(f"量化图片: {output_dir}")
    print(f"数据集txt: {txt_path}")


if __name__ == "__main__":
    generate_rknn_quant_dataset()