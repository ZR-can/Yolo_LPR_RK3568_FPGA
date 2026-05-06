import os

def rename_jpg_files():
    """
    固定路径批量重命名JPG文件，并生成指定路径的test_dataset.txt
    图片文件夹：../model/test_imgs
    输出txt：../model/test_dataset.txt
    """
    # ====================== 固定路径配置 ======================
    # 目标图片文件夹（相对路径）
    folder_path = os.path.abspath("../model/test_imgs")
    # 生成的txt文件路径（相对路径）
    txt_save_path = os.path.abspath("../model/test_dataset.txt")
    # ========================================================

    # 1. 校验图片文件夹是否存在
    if not os.path.isdir(folder_path):
        print(f"错误：图片文件夹不存在 -> {folder_path}")
        print("请确认目录结构：当前目录下存在 model/test_imgs 文件夹")
        return

    # 2. 筛选所有 jpg / JPG 文件
    jpg_files = []
    for filename in os.listdir(folder_path):
        if filename.lower().endswith(".jpg"):
            jpg_files.append(filename)

    # 3. 文件排序
    jpg_files.sort()

    # 4. 无文件则退出
    if not jpg_files:
        print("提示：文件夹中未找到任何jpg格式文件！")
        return

    print(f"找到 {len(jpg_files)} 个jpg文件，开始处理...")
    txt_lines = []

    # 5. 批量重命名
    for index, old_name in enumerate(jpg_files):
        # 生成8位数字文件名
        new_name = f"{index:08d}.jpg"
        old_path = os.path.join(folder_path, old_name)
        new_path = os.path.join(folder_path, new_name)

        os.rename(old_path, new_path)
        print(f"重命名：{old_name} -> {new_name}")

        # 生成TXT内容（严格按你要求的格式）
        txt_lines.append(f"./test_imgs/{new_name}")

    # 6. 写入 test_dataset.txt
    # 自动创建model文件夹（如果不存在）
    os.makedirs(os.path.dirname(txt_save_path), exist_ok=True)
    with open(txt_save_path, "w", encoding="utf-8") as f:
        f.write("\n".join(txt_lines))

    print("\n处理完成！")
    print(f"图片重命名路径：{folder_path}")
    print(f"生成的列表文件：{txt_save_path}")

# 命令行直接运行
if __name__ == "__main__":
    input("请确认已备份文件，按回车键开始执行...")
    rename_jpg_files()