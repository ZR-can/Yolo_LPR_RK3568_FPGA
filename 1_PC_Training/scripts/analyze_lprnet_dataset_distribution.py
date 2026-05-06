import os
from pathlib import Path
from collections import defaultdict

# ================= 配置区 =================
# 与清理脚本保持一致的 txt 目录
TXT_DIR = Path(r"D:\Yolo_LPR_RK3568_FPGA_Project\1_PC_Training\datasets\lprnet_8char\classified_txts")

# 特殊车牌标识（遍历全车牌检测，各自独立分类）
SPECIAL_PLATE_CHARS = {'港', '澳', '挂', '学', '领', '使', '临','警'}


def collect_stats_by_prefix(prefix):
    """
    通用统计逻辑：统计指定前缀文件（如 type_*.txt / province_*.txt）中的 train/val 样本数。
    文件名格式：{prefix}_{category}_{split}.txt
    """
    stats = {
        'train': defaultdict(int),
        'val': defaultdict(int)
    }

    txt_files = [
        p for p in TXT_DIR.glob(f"{prefix}_*.txt")
        if p.name.endswith(("_train.txt", "_val.txt"))
    ]
    if not txt_files:
        return stats, []

    for txt_path in txt_files:
        filename = txt_path.stem
        parts = filename.split('_')

        if len(parts) < 3:
            continue

        split_type = parts[-1]
        if split_type not in ['train', 'val']:
            continue

        category_name = "_".join(parts[1:-1])
        if not category_name:
            continue

        with open(txt_path, 'r', encoding='utf-8') as f:
            count = sum(1 for line in f if line.strip())

        stats[split_type][category_name] = count

    all_categories = set(stats['train'].keys()).union(set(stats['val'].keys()))
    return stats, sorted(all_categories)


def print_stats_table(title, category_header, stats, categories):
    """打印 train/val 统计表。"""
    print("=" * 55)
    print(title)
    print("=" * 55)
    print(f"{category_header:<20} | {'Train 数量':<12} | {'Val 数量':<12}")
    print("-" * 55)

    total_train = 0
    total_val = 0

    for category in categories:
        train_count = stats['train'].get(category, 0)
        val_count = stats['val'].get(category, 0)

        total_train += train_count
        total_val += val_count

        print(f"{category:<20} | {train_count:<12} | {val_count:<12}")

    print("=" * 55)
    print(f"{'总计':<20} | {total_train:<12} | {total_val:<12}")
    print(f"数据集总体规模: {total_train + total_val} 张图像")


def sort_province_categories(categories):
    """
    省份/特殊字符排序：常规省份在前，特殊车牌标识在后，二者均按字典序。
    """
    normal = sorted([c for c in categories if c not in SPECIAL_PLATE_CHARS])
    special = sorted([c for c in categories if c in SPECIAL_PLATE_CHARS])
    return normal + special

def analyze_lprnet_dataset():
    type_stats, type_categories = collect_stats_by_prefix("type")
    province_stats, province_categories = collect_stats_by_prefix("province")

    if not type_categories and not province_categories:
        print("未找到可统计的 type_/province_ train/val 文件。")
        return

    if type_categories:
        print_stats_table(
            title="LPRNet 单行车牌类型分布统计",
            category_header="车牌类型",
            stats=type_stats,
            categories=type_categories,
        )
        print()
    else:
        print("未找到 type_*.txt 的 train/val 统计文件。\n")

    if province_categories:
        ordered_province_categories = sort_province_categories(province_categories)
        print_stats_table(
            title="LPRNet 省份/特殊车牌分布统计",
            category_header="省份/特殊标识",
            stats=province_stats,
            categories=ordered_province_categories,
        )
    else:
        print("未找到 province_*.txt 的 train/val 统计文件。")

if __name__ == "__main__":
    # 执行统计
    analyze_lprnet_dataset()

'''
=======================================================
LPRNet 单行车牌类型分布统计
=======================================================
车牌类型                 | Train 数量     | Val 数量      
-------------------------------------------------------
单层黄牌                 | 50074        | 2568        
新能源大型车               | 50024        | 2606        
新能源小型车               | 74893        | 4052        
普通蓝牌                 | 75060        | 3900        
白色车牌                 | 167          | 45          
黑色车牌                 | 24978        | 1337        
=======================================================
总计                   | 275196       | 14508       
数据集总体规模: 289704 张图像

=======================================================
LPRNet 省份/特殊车牌分布统计
=======================================================
省份/特殊标识              | Train 数量     | Val 数量      
-------------------------------------------------------
云                    | 8142         | 407         
京                    | 8124         | 435         
冀                    | 8082         | 425         
吉                    | 7969         | 439         
宁                    | 7897         | 429         
川                    | 7970         | 450         
新                    | 8064         | 400         
晋                    | 7973         | 423         
桂                    | 8067         | 462         
沪                    | 8068         | 436         
津                    | 8068         | 382         
浙                    | 7967         | 431         
渝                    | 7866         | 392         
湘                    | 7966         | 413         
琼                    | 7869         | 394         
甘                    | 8084         | 454         
皖                    | 7945         | 419         
粤                    | 8067         | 423         
苏                    | 7979         | 447         
蒙                    | 7983         | 401         
藏                    | 8059         | 428         
豫                    | 7891         | 450         
贵                    | 7981         | 411         
赣                    | 7869         | 405         
辽                    | 8049         | 414         
鄂                    | 7993         | 399         
闽                    | 7978         | 394         
陕                    | 8052         | 429         
青                    | 7931         | 395         
鲁                    | 7983         | 425         
黑                    | 8047         | 409         
使                    | 6286         | 365         
学                    | 8301         | 438         
港                    | 3108         | 156         
澳                    | 3108         | 161         
警                    | 167          | 45          
领                    | 6243         | 322         
=======================================================
总计                   | 275196       | 14508       
数据集总体规模: 289704 张图像
'''