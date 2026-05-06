import cv2
import numpy as np

def split_double_layer_plate(image):
    """
    按双层车牌标准比例切割：
    - 高度上，上层:下层 = 4:7
    - 宽度上，上层仅占整牌居中 1/2，下层占整牌全宽
    """
    h, w = image.shape[:2]

    # 高度比例：4/11 与 7/11（对应总高度 220 时的 80 和 140）
    top_h = int(round(h * (4.0 / 11.0)))
    top_h = max(1, min(top_h, h - 1))

    # 上层宽度为整牌的一半，且居中
    top_w = int(round(w * 0.6))
    top_w = max(1, min(top_w, w))
    top_x0 = (w - top_w) // 2
    top_x1 = top_x0 + top_w

    # 按模板区域裁剪
    top_half = image[:top_h, top_x0:top_x1]
    bottom_half = image[top_h:, :]
    
    return top_half, bottom_half

def process_and_flatten_plate(image):
    """
    按比例裁剪后拼接为单行车牌，最终输出固定尺寸 94x24
    """
    top_half, bottom_half = split_double_layer_plate(image)

    target_w, target_h = 94, 24

    # 展平后左右宽度比例：上层:下层 = 1:2
    top_w = int(round(target_w * (1.0 / 3.0)))
    bottom_w = target_w - top_w

    # 左右两部分都拉伸到同一高度 target_h，避免字符上下不齐
    top_resized = cv2.resize(top_half, (top_w, target_h), interpolation=cv2.INTER_LINEAR)
    bottom_resized = cv2.resize(bottom_half, (bottom_w, target_h), interpolation=cv2.INTER_LINEAR)

    # 水平拼接，得到严格 94x24
    flattened_plate = np.hstack((top_resized, bottom_resized))

    return flattened_plate



if __name__ == "__main__":
    # 示例用法
    input_image_path = r""  # 替换为实际图像路径
    output_image_path = "./flattened_plate.jpg"
    
    # 读取图像
    image = cv2.imdecode(np.fromfile(input_image_path, dtype=np.uint8), cv2.IMREAD_COLOR)
     
    if image is None:
        print(f"无法读取图像: {input_image_path}")
        exit(1)
    
    # 处理并扁平化车牌
    flattened_plate = process_and_flatten_plate(image)
    # 保存结果
    cv2.imwrite(output_image_path, flattened_plate)
    print(f"处理完成，结果保存为: {output_image_path}")