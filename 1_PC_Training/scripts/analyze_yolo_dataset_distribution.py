import os
from collections import Counter
from pathlib import Path
# python analyze_yolo_dataset_distribution.py
def analyze_yolo_dataset_distribution(yolo_root_path):
    """
    遍历 YOLO 格式的数据集标签，统计各个类别的目标总数。
    """
    yolo_root = Path(yolo_root_path)
    if not yolo_root.exists():
        print(f"错误：找不到目录 {yolo_root}。请确保已完成数据清洗。")
        return
    
    # 类别 ID 到名称的映射，方便阅读结果
    class_names = {
        '0': 'blue          (蓝牌)',
        '1': 'green         (绿牌)',
        '2': 'yellow_single (单行黄牌)',
        #'3': 'yellow_double (双行黄牌)',
        '3': 'other         (白/黑/其他)'
    }
    
    # 使用 Counter 统计类别分布
    train_counter = Counter()
    val_counter = Counter()
    test_counter = Counter()
    
    # 统计函数
    def count_labels(label_dir, counter):
        if not label_dir.exists():
            return
        for txt_file in label_dir.glob("*.txt"):
            with open(txt_file, 'r', encoding='utf-8') as f:
                for line in f:
                    parts = line.strip().split()
                    if parts:
                        class_id = parts[0]
                        counter[class_id] += 1

    # 执行统计
    count_labels(yolo_root / "labels" / "train", train_counter)
    count_labels(yolo_root / "labels" / "val", val_counter)
    count_labels(yolo_root / "labels" / "test", test_counter)
    # 合并总计
    total_counter = train_counter + val_counter + test_counter

    # 打印格式化报告
    print("=" * 50)
    print("YOLO 数据集类别分布报告")
    print("=" * 50)
    print(f"{'Class ID':<10} | {'Class Name':<28} | {'Train':<8} | {'Val':<8} | {'Test':<8} | {'Total':<8}")
    print("-" * 50)
    
    # 确保按 0-3 的顺序输出
    for class_id in [str(i) for i in range(4)]: #range(5)
        name = class_names.get(class_id, "Unknown")
        train_count = train_counter.get(class_id, 0)
        val_count = val_counter.get(class_id, 0)
        test_count = test_counter.get(class_id, 0)
        total_count = total_counter.get(class_id, 0)
        print(f"{class_id:<10} | {name:<28} | {train_count:<8} | {val_count:<8} | {test_count:<8} | {total_count:<8}")
    
    print("=" * 50)
    total_boxes = sum(total_counter.values())
    print(f"总计 Bounding Boxes: {total_boxes}")
if __name__ == "__main__":
# 执行分析
    analyze_yolo_dataset_distribution("../datasets/yolo_format")

'''
==================================================
CRPD_YOLO数据集类别分布报告
==================================================
Class ID   | Class Name                   | Train    | Val      | Test     | Total   
--------------------------------------------------
0          | blue          (蓝牌)           | 11521    | 2935     | 2898     | 17354   
1          | green         (绿牌)           | 0        | 0        | 0        | 0       
2          | yellow_single (单行黄牌)         | 4638     | 1452     | 256      | 6346    
3          | yellow_double (双行黄牌)         | 119      | 24       | 17       | 160     
4          | other         (白/黑/其他)       | 422      | 7        | 4        | 433     
==================================================
总计 Bounding Boxes: 24293
'''


'''
==================================================
CRPD_YOLO转移到yolo_format后数据集类别分布报告
==================================================
Class ID   | Class Name                   | Train    | Val      | Test     | Total   
--------------------------------------------------
0          | blue          (蓝牌)           | 3276     | 797      | 909      | 4982    
1          | green         (绿牌)           | 0        | 0        | 0        | 0       
2          | yellow_single (单行黄牌)         | 4637     | 1452     | 255      | 6344    
3          | other         (白/黑/其他)       | 422      | 7        | 4        | 433     
==================================================
总计 Bounding Boxes: 11759
'''

'''
==================================================
yolo_format最终数据集类别分布报告
==================================================
Class ID   | Class Name                   | Train    | Val      | Test     | Total   
--------------------------------------------------
0          | blue          (蓝牌)           | 5516     | 1757     | 2509     | 9782    
1          | green         (绿牌)           | 3500     | 1001     | 2000     | 6501    
2          | yellow_single (单行黄牌)         | 4637     | 1452     | 255      | 6344    
3          | other         (白/黑/其他)       | 422      | 7        | 4        | 433     
==================================================
总计 Bounding Boxes: 23060
'''

