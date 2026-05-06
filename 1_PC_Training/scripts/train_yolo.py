"""
使用YOLOv8训练车牌检测模型（支持蓝牌/绿牌分类）
基于ultralytics库实现，在原有模型基础上微调
"""
# python train_yolo.py
import os
import argparse
import time
from ultralytics import YOLO
from tqdm import tqdm


def get_parser():
    parser = argparse.ArgumentParser(description='训练YOLO车牌检测模型')
    parser.add_argument('--model', default='D:\\Yolo_LPR_RK3568_FPGA_Project\\1_PC_Training\\scripts\\runs\\train_results\\yolov8n_multi_class\\weights\\last.pt', help='预训练模型路径')
    parser.add_argument('--config', default='../configs/yolo_config.yaml', help='YOLO配置文件路径')
    # 微调场景下调整默认参数
    parser.add_argument('--epochs', default=50, type=int, help='训练轮数，微调可适当减少')
    parser.add_argument('--batch_size', default=16, type=int, help='批次大小')
    parser.add_argument('--img_size', default=640, type=int, help='输入图像大小')
    parser.add_argument('--lr0', default=0.001, type=float, help='初始学习率，微调建议调低')
    parser.add_argument('--device', default='0', help='训练设备')
    parser.add_argument('--name', default='yolov8n_multi_class', help='训练结果保存名称')
    parser.add_argument('--project', default='../train_results', help='训练结果保存路径')

    
    return parser


def main():
    # 解析命令行参数
    args = get_parser().parse_args()
    
    print(f"开始在原有模型基础上微调蓝绿牌分类模型...")
    print(f"配置参数:\n" \
          f"  基础模型: {args.model}\n" \
          f"  配置文件: {args.config}\n" \
          f"  训练轮数: {args.epochs}\n" \
          f"  批次大小: {args.batch_size}\n" \
          f"  图像大小: {args.img_size}\n" \
          f"  初始学习率: {args.lr0}\n" \
          f"  保存名称: {args.name}\n" \
          f"  保存路径: {args.project}")
    
    # 初始化YOLO模型，自动加载原有权重，自动适配新的类别数
    model = YOLO(args.model)
    
    # 训练模型
    try:
        # 记录训练开始时间
        train_start_time = time.time()
        print(f"\n开始训练，共{args.epochs}轮，请等待...")
        
        # 训练模型（ultralytics原生会输出每轮进度和损失）
        results = model.train(
            data=args.config,
            epochs=args.epochs,
            batch=args.batch_size,
            imgsz=args.img_size,
            lr0=args.lr0,
            device=args.device,
            name=args.name,
            project=args.project,
            exist_ok=True , # 如果保存目录已存在，继续训练
            resume=True  # 从上次训练中断的地方继续训练
        )
        
        # 训练耗时统计
        train_duration = time.time() - train_start_time
        print(f"\n训练完成，总耗时: {train_duration/60:.2f} 分钟 ({train_duration:.2f} 秒)")
        
        # 验证模型（添加进度条）
        print("\n开始验证模型...")
        with tqdm(total=1, desc='验证进度', bar_format='{l_bar}{bar}| {n_fmt}/{total_fmt} [{elapsed}<{remaining}]') as pbar:
            metrics = model.val()
            pbar.update(1)
        
        print(f"模型验证结果:\n" \
              f"  mAP50: {metrics.box.map50}\n" \
              f"  mAP50-95: {metrics.box.map}")
        
        # 导出模型（添加进度条）
        print("\n开始导出模型为torchscript格式...")
        with tqdm(total=1, desc='导出进度', bar_format='{l_bar}{bar}| {n_fmt}/{total_fmt} [{elapsed}<{remaining}]') as pbar:
            model.export(format='torchscript')
            pbar.update(1)
        
        
    except Exception as e:
        print(f"训练过程中出现错误: {str(e)}")
        return False
    
    return True


if __name__ == '__main__':
    main()