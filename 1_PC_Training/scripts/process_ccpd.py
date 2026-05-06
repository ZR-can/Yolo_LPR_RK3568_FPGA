"""
CCPD蓝绿牌数据集处理工具
支持限制每个蓝牌子目录的样本数量，避免样本过多
python process_ccpd.py --green_root ../datasets/CCPD2020/ccpd_green --blue_root ../datasets/CCPD2019 --output_dir ../datasets/yolo_format
"""

import os
import argparse
import cv2
import numpy as np
import shutil
from tqdm import tqdm
import random


class CCPDProcessor:
    def __init__(self, green_root, blue_root, output_dir):
        self.green_root = green_root  # 绿牌数据集根目录，包含train/val子目录
        self.blue_root = blue_root    # 蓝牌数据集根目录，包含ccpd_base等子目录
        self.output_dir = output_dir
        self.image_dir = os.path.join(output_dir, 'images')
        self.label_dir = os.path.join(output_dir, 'labels')
    
        # 创建输出目录
        os.makedirs(os.path.join(self.image_dir, 'train'), exist_ok=True)
        os.makedirs(os.path.join(self.image_dir, 'val'), exist_ok=True)
        os.makedirs(os.path.join(self.image_dir, 'test'), exist_ok=True)
        os.makedirs(os.path.join(self.label_dir, 'train'), exist_ok=True)
        os.makedirs(os.path.join(self.label_dir, 'val'), exist_ok=True)
        os.makedirs(os.path.join(self.label_dir, 'test'), exist_ok=True)

    def process(self, blue_train_ratio=0.7, samples_per_blue_dir=400, green_samples=3500):
        """
        处理数据集
        :param blue_train_ratio: 蓝牌数据集的训练集比例
        :param samples_per_blue_dir: 每个蓝牌子目录最多选取的样本数量，避免样本过多
        """
        train_samples = []  # (img_path, class_id)
        val_samples = []    # (img_path, class_id)
        test_samples = []   # (img_path, class_id)
        np.random.seed(42) 
        # -------------------------- 1. 处理绿牌样本：直接沿用已经划分好的train/val/test --------------------------
        print(f"正在收集绿牌样本，训练集保留 {green_samples} 张，验证集保留全部...")
        # 绿牌训练集：随机采样指定数量
        green_train_dir = os.path.join(self.green_root, 'train')
        if os.path.exists(green_train_dir):
            green_train_all = []
            for root, _, files in os.walk(green_train_dir):
                for file in files:
                    if file.endswith('.jpg'):
                        green_train_all.append(os.path.join(root, file))
            
            # 随机打乱后采样
            random.shuffle(green_train_all)
            green_train_selected = green_train_all[:green_samples]
            for img_path in green_train_selected:
                train_samples.append((img_path, 1))  # 绿牌类别ID=1
            print(f"  绿牌训练集：原有 {len(green_train_all)} 张，选取 {len(green_train_selected)} 张")
        
        # 绿牌验证集：保留全部
        green_val_dir = os.path.join(self.green_root, 'val')
        if os.path.exists(green_val_dir):
            green_val_count = 0
            for root, _, files in os.walk(green_val_dir):
                for file in files:
                    if file.endswith('.jpg'):
                        val_samples.append((os.path.join(root, file), 1))
                        green_val_count += 1
            print(f"  绿牌验证集：保留全部 {green_val_count} 张")

        # 绿牌测试集：随机采样指定数量
        green_test_dir = os.path.join(self.green_root, 'test')
        if os.path.exists(green_test_dir):
            green_test_all = []
            for root, _, files in os.walk(green_test_dir):
                for file in files:
                    if file.endswith('.jpg'):
                        green_test_all.append(os.path.join(root, file))
            
            # 随机打乱后采样
            random.shuffle(green_test_all)
            green_test_num = 2000  # 绿牌测试集保留数量
            green_test_selected = green_test_all[:green_test_num]
            for img_path in green_test_selected:
                test_samples.append((img_path, 1))  # 绿牌类别ID=1
            print(f"  绿牌测试集：原有 {len(green_test_all)} 张，选取 {len(green_test_selected)} 张")

        # -------------------------- 2. 处理蓝牌样本：每个子目录最多选N个样本，自动划分 --------------------------
        print(f"正在收集蓝牌样本，每个子目录最多选 {samples_per_blue_dir} 张...")
        blue_all_samples = []
        TEST_SAMPLE_COUNT = 200  # 每个目录剩余图片取200张测试
        # 遍历蓝牌的所有子目录，跳过无用的目录
        skip_dirs = {'ccpd_np', 'splits','ccpd_characters'}  # 跳过无车牌、划分文件、字符样本
        for sub_dir in os.listdir(self.blue_root):
            sub_path = os.path.join(self.blue_root, sub_dir)
            if not os.path.isdir(sub_path):
                continue
            if sub_dir in skip_dirs:
                print(f"  跳过无用目录: {sub_dir}")
                continue
            
            # 收集当前子目录的所有图片
            sub_samples = []
            for root, _, files in os.walk(sub_path):
                for file in files:
                    if file.endswith('.jpg'):
                        sub_samples.append(os.path.join(root, file))
            
            # 随机打乱，然后取前N个，保证每个场景都有覆盖，同时控制数量
            random.shuffle(sub_samples)
            selected_for_train_val = sub_samples[:samples_per_blue_dir]
            remaining_for_test = sub_samples[samples_per_blue_dir:samples_per_blue_dir+TEST_SAMPLE_COUNT]

            # 加入到蓝牌总样本
            for img_path in selected_for_train_val:
                blue_all_samples.append((img_path, 0))  # 蓝牌类别ID=0

            # 加入测试集
            for img_path in remaining_for_test:
                test_samples.append((img_path, 0)) 

            print(f"  处理蓝牌子目录: {sub_dir}, 选取 {len(selected_for_train_val)} 个训练/验证, {len(remaining_for_test)} 个测试")
        
        # 划分蓝牌的训练/验证集
        np.random.shuffle(blue_all_samples)
        split_idx = int(len(blue_all_samples) * blue_train_ratio)
        blue_train = blue_all_samples[:split_idx]
        blue_val = blue_all_samples[split_idx:]
        
        # 合并到总样本列表
        train_samples.extend(blue_train)
        val_samples.extend(blue_val)
        
        # -------------------------- 3. 统计信息 --------------------------
        green_train_count = sum(1 for _ in train_samples if _[1] == 1)
        green_val_count = sum(1 for _ in val_samples if _[1] == 1)
        green_test_count = sum(1 for _ in test_samples if _[1] == 1)
        blue_train_count = sum(1 for _ in train_samples if _[1] == 0)
        blue_val_count = sum(1 for _ in val_samples if _[1] == 0)
        blue_test_count = sum(1 for _ in test_samples if _[1] == 0)
        

        print(f"\n数据集统计:")
        print(f"  训练集总数: {len(train_samples)} 张")
        print(f"    - 蓝牌训练: {blue_train_count} 张")
        print(f"    - 绿牌训练: {green_train_count} 张")
        print(f"  验证集总数: {len(val_samples)} 张")
        print(f"    - 蓝牌验证: {blue_val_count} 张")
        print(f"    - 绿牌验证: {green_val_count} 张")
        print(f"  测试集总数: {len(test_samples)} 张")
        print(f"    - 蓝牌测试: {blue_test_count} 张")
        print(f"    - 绿牌测试: {green_test_count} 张")
        
        # -------------------------- 4. 处理样本 --------------------------
        print("\n开始处理训练集...")
        self._process_samples(train_samples, 'train')
        
        print("开始处理验证集...")
        self._process_samples(val_samples, 'val')

        print("开始处理测试集...")
        self._process_samples(test_samples, 'test')
        
        
        print(f"\n数据集处理完成")
        print(f"  图像保存在: {self.image_dir}")
        print(f"  标注文件保存在: {self.label_dir}")
        
    def _process_samples(self, samples, split):
        """处理指定的样本列表"""
        for img_path, class_id in tqdm(samples, desc=f'处理{split}集'):
            try:
                # 生成唯一文件名，避免重名覆盖
                rel_path = os.path.relpath(img_path, self.blue_root if class_id ==0 else self.green_root)
                prefix = 'ccpd_blue_' if class_id ==0 else 'ccpd_green_'
                new_img_name = prefix + rel_path.replace(os.path.sep, '_')
                
                # 解析文件名获取车牌坐标
                old_img_name = os.path.basename(img_path)
                coords = self._parse_coordinates(old_img_name)
                
                if coords is None:
                    continue
                
                # 读取图像获取尺寸
                img = cv2.imread(img_path)
                if img is None:
                    continue
                
                h, w = img.shape[:2]
                
                # 计算YOLO格式的标注
                yolo_coords = self._convert_to_yolo_format(coords, w, h, class_id)
                
                # 复制图像到目标目录
                dest_img_path = os.path.join(self.image_dir, split, new_img_name)
                shutil.copy(img_path, dest_img_path)
                
                # 创建标注文件
                label_name = os.path.splitext(new_img_name)[0] + '.txt'
                label_path = os.path.join(self.label_dir, split, label_name)
                
                with open(label_path, 'w', encoding='utf-8') as f:
                    f.write(yolo_coords)
                    
            except Exception as e:
                # 静默跳过错误样本，不打断进度条
                continue
        
    def _process_samples(self, samples, split):
        """处理指定的样本列表（原代码完全不变，支持train/val/test）"""
        for img_path, class_id in tqdm(samples, desc=f'处理{split}集'):
            try:
                # 生成唯一文件名，避免重名覆盖
                rel_path = os.path.relpath(img_path, self.blue_root if class_id ==0 else self.green_root)
                prefix = 'ccpd_blue_' if class_id ==0 else 'ccpd_green_'
                new_img_name = prefix + rel_path.replace(os.path.sep, '_')
                
                # 解析文件名获取车牌坐标
                old_img_name = os.path.basename(img_path)
                coords = self._parse_coordinates(old_img_name)
                
                if coords is None:
                    continue
                
                # 读取图像获取尺寸
                img = cv2.imread(img_path)
                if img is None:
                    continue
                
                h, w = img.shape[:2]
                
                # 计算YOLO格式的标注
                yolo_coords = self._convert_to_yolo_format(coords, w, h, class_id)
                
                # 复制图像到目标目录
                dest_img_path = os.path.join(self.image_dir, split, new_img_name)
                shutil.copy(img_path, dest_img_path)
                
                # 创建标注文件
                label_name = os.path.splitext(new_img_name)[0] + '.txt'
                label_path = os.path.join(self.label_dir, split, label_name)
                
                with open(label_path, 'w', encoding='utf-8') as f:
                    f.write(yolo_coords)
                    
            except Exception as e:
                # 静默跳过错误样本，不打断进度条
                continue
           

    def _parse_coordinates(self, img_name):
        """从CCPD文件名解析车牌坐标"""
        # CCPD文件名格式示例: 000257620317-90_113-265&349_447&404-447&404_275&414_265&349_438&339-0_0_22_26_27_20_30-124-23.jpg
        try:
            # 提取坐标部分
            parts = img_name.split('-')
            if len(parts) < 3:
                return None
            
            # 第二个部分是车牌坐标: 265&349_447&404
            coord_part = parts[2]
            points = coord_part.split('_')
            
            if len(points) < 2:
                return None
            
            # 获取四个角点坐标
            # 这里简化处理，使用矩形框而不是四边形
            # 提取左上和右下坐标
            x1, y1 = points[0].split('&')
            x2, y2 = points[1].split('&')
            
            return {
                'x1': int(x1),
                'y1': int(y1),
                'x2': int(x2),
                'y2': int(y2)
            }
            
        except Exception as e:
            print(f"解析文件名 {img_name} 时出错: {str(e)}")
            return None
    
    def _convert_to_yolo_format(self, coords, img_width, img_height, class_id):
        """将坐标转换为YOLO格式"""
        # YOLO格式: class_id x_center y_center width height
        # 所有值都归一化到0-1之间
        x_center = (coords['x1'] + coords['x2']) / 2.0 / img_width
        y_center = (coords['y1'] + coords['y2']) / 2.0 / img_height
        width = (coords['x2'] - coords['x1']) / img_width
        height = (coords['y2'] - coords['y1']) / img_height
        
        return f"{class_id} {x_center:.6f} {y_center:.6f} {width:.6f} {height:.6f}\n"


def get_parser():
    parser = argparse.ArgumentParser(description='CCPD蓝绿牌数据集处理工具')
    parser.add_argument('--green_root', required=True, help='绿牌数据集根目录，需要包含train/val子目录')
    parser.add_argument('--blue_root', required=True, help='蓝牌数据集根目录，包含ccpd_base等子目录')
    parser.add_argument('--output_dir', default='../datasets/yolo_format', help='输出目录')
    parser.add_argument('--blue_train_ratio', default=0.7, type=float, help='蓝牌数据集的训练集比例')
    parser.add_argument('--samples_per_blue_dir', default=400, type=int, help='每个蓝牌子目录最多选取的样本数量')
    
    return parser


def main():
    args = get_parser().parse_args()
    
    processor = CCPDProcessor(args.green_root, args.blue_root, args.output_dir)
    processor.process(
        blue_train_ratio=args.blue_train_ratio,
        samples_per_blue_dir=args.samples_per_blue_dir
    )


if __name__ == '__main__':
    main()
    
'''
正在收集绿牌样本，训练集保留 3500 张，验证集保留全部...
  绿牌训练集：原有 5769 张，选取 3500 张
  绿牌验证集：保留全部 1001 张
  绿牌测试集：原有 5006 张，选取 2000 张
正在收集蓝牌样本，每个子目录最多选 400 张...
  处理蓝牌子目录: ccpd_base, 选取 400 个训练/验证, 200 个测试
  处理蓝牌子目录: ccpd_blur, 选取 400 个训练/验证, 200 个测试
  处理蓝牌子目录: ccpd_challenge, 选取 400 个训练/验证, 200 个测试
  处理蓝牌子目录: ccpd_db, 选取 400 个训练/验证, 200 个测试
  处理蓝牌子目录: ccpd_fn, 选取 400 个训练/验证, 200 个测试
  跳过无用目录: ccpd_np
  处理蓝牌子目录: ccpd_rotate, 选取 400 个训练/验证, 200 个测试
  处理蓝牌子目录: ccpd_tilt, 选取 400 个训练/验证, 200 个测试
  处理蓝牌子目录: ccpd_weather, 选取 400 个训练/验证, 200 个测试
  跳过无用目录: splits

数据集统计:
  训练集总数: 5740 张
    - 蓝牌训练: 2240 张
    - 绿牌训练: 3500 张
  验证集总数: 1961 张
    - 蓝牌验证: 960 张
    - 绿牌验证: 1001 张
  测试集总数: 3600 张
    - 蓝牌测试: 1600 张
    - 绿牌测试: 2000 张
'''