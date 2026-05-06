import os
from PIL import Image, ImageDraw, ImageFont
#中文字体大小: 18
#ASCII 字体大小: 36

def generate_c_font_array(font_path, output_file):
    # 车牌字符集映射顺序 (必须与 C 代码中的 plate_chinese_map 完全一致)
    chars = [
        "京", "沪", "津", "渝", "冀", "晋", "蒙", "辽", "吉", "黑",
        "苏", "浙", "皖", "闽", "赣", "鲁", "豫", "鄂", "湘", "粤",
        "桂", "琼", "川", "贵", "云", "藏", "陕", "甘", "青", "宁",
        "新", "港", "澳", "警", "学", "领", "使", "蓝", "绿", "黄", 
        "白", "黑",
        "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
        "A", "B", "C", "D", "E", "F", "G", "H", "J", "K",
        "L", "M", "N", "P", "Q", "R", "S", "T", "U", "V",
        "W", "X", "Y", "Z", "O", "I", ".", "%", " "
    ]

    width, height = 20, 40
    padding_x, padding_y = 1, 2

    chinese_chars = [ch for ch in chars if len(ch.encode('utf-8')) > 1]
    ascii_chars = [ch for ch in chars if len(ch.encode('utf-8')) == 1]

    def fits(font, target_chars):
        test_img = Image.new('L', (width, height), color=0)
        test_draw = ImageDraw.Draw(test_img)
        for ch in target_chars:
            bbox = test_draw.textbbox((0, 0), ch, font=font)
            ch_w = bbox[2] - bbox[0]
            ch_h = bbox[3] - bbox[1]
            if ch_w > width - 2 * padding_x or ch_h > height - 2 * padding_y:
                return False
        return True

    def pick_font_for(target_chars):
        selected_font = None
        selected_size = None
        for size in range(40, 9, -1):
            candidate = ImageFont.truetype(font_path, size)
            if fits(candidate, target_chars):
                selected_font = candidate
                selected_size = size
                break
        if selected_font is None:
            selected_size = 10
            selected_font = ImageFont.truetype(font_path, selected_size)
        return selected_font, selected_size

    # 分别为中文和 ASCII 选择各自最大可用字号，减少字符周围空白
    try:
        cn_font, cn_font_size = pick_font_for(chinese_chars)
        ascii_font, ascii_font_size = pick_font_for(ascii_chars)
    except IOError:
        print(f"找不到字体文件: {font_path}，请修改路径")
        return

    print(f"中文字体大小: {cn_font_size}")
    print(f"ASCII 字体大小: {ascii_font_size}")

    with open(output_file, 'w', encoding='utf-8') as f:
            f.write("#ifndef __PLATE_FONT_DATA_H__\n")
            f.write("#define __PLATE_FONT_DATA_H__\n\n")
            
            # 1. 自动生成 C 语言映射表结构
            f.write("typedef struct {\n")
            f.write("    char name[4];\n")
            f.write("    int index;\n")
            f.write("    int bytes; // 1 for ASCII, 3 for UTF-8 Chinese\n")
            f.write("} PlateCharMap;\n\n")
            
            f.write(f"static const PlateCharMap plate_char_map[{len(chars)}] = {{\n")
            for i, char in enumerate(chars):
                bytes_len = 3 if len(char.encode('utf-8')) > 1 else 1
                f.write(f'    {{"{char}", {i}, {bytes_len}}}')
                if i < len(chars) - 1:
                    f.write(",")
                if (i + 1) % 5 == 0:
                    f.write("\n")
            f.write("\n};\n\n")

            # 2. 生成字模数据数组
            f.write(f"extern const unsigned char plate_font_data[{len(chars)}][{width * height}];\n\n")
            f.write(f"const unsigned char plate_font_data[{len(chars)}][{width * height}] = {{\n")

            for idx, char in enumerate(chars):
                img = Image.new('L', (width, height), color=0)
                draw = ImageDraw.Draw(img)

                font = cn_font if len(char.encode('utf-8')) > 1 else ascii_font
                bbox = font.getbbox(char)
                text_w = bbox[2] - bbox[0]
                text_h = bbox[3] - bbox[1]
                x = (width - text_w) // 2 - bbox[0]
                y = (height - text_h) // 2 - bbox[1]

                draw.text((x, y), char, font=font, fill=255)
                pixels = list(img.getdata())

                f.write(f"    // Index {idx}: '{char}'\n")
                f.write("    {\n        ")
                for i, val in enumerate(pixels):
                    f.write(f"{val:>3}, ")
                    if (i + 1) % width == 0 and i != len(pixels) - 1:
                        f.write("\n        ")
                f.write("\n    }")
                
                if idx != len(chars) - 1:
                    f.write(",\n")
                else:
                    f.write("\n")

            f.write("};\n\n")
            f.write("#endif // __PLATE_FONT_DATA_H__\n")
            print(f"Unified font data generated successfully: {output_file}")

if __name__ == "__main__":
    font_file = "C:/Windows/Fonts/simhei.ttf" 
    output_h_file = r"D:\Yolo_LPR_RK3568_FPGA_Project\3_NPU_Yolov8_LPR_Demo\utils\plate_font.h"
    generate_c_font_array(font_file, output_h_file)