"""
CCPD测试集评估脚本
功能：加载训练好的模型，评估test集，输出mAP/Precision/Recall
"""
from ultralytics import YOLO

# 1. 配置路径
MODEL_PATH = r"D:\Yolo_LPR_RK3568_FPGA_Project\1_PC_Training\scripts\runs\train_results\yolov8n_multi_class\weights\best.pt"


if __name__ == '__main__':
    # 2. 加载模型
    model = YOLO(MODEL_PATH)

    # 3. 评估测试集（自动对比真实标签，计算所有指标）
    results = model.val(
        data=r"D:\Yolo_LPR_RK3568_FPGA_Project\1_PC_Training\configs\yolo_config.yaml",  # 配置文件中已指定test集路径
        split="test",  # 评估test集
        save_txt=True,      # 保存预测结果为txt文件
        iou=0.65,        # 核心匹配阈值
        conf=0.5,      # 置信度阈值
        device='0',  # 使用GPU进行评估
        plots=True,       # 生成曲线图
        project=r"D:\Yolo_LPR_RK3568_FPGA_Project\1_PC_Training\scripts\runs\test_results" # 评估结果保存路径
    )

    # 4. 打印核心指标
    print("\n" + "="*50)
    print("测试集评估结果")
    print("="*50)
    print(f"mAP@0.5: {results.box.map50:.4f}")
    
    # 整体指标（所有类别平均）
    print(f"精确率(Precision): {results.box.p.mean():.4f}")
    print(f"召回率(Recall): {results.box.r.mean():.4f}")
    
    # 蓝牌
    print(f"蓝牌 AP@0.5: {results.box.ap50[0]:.4f}")
    # 绿牌
    print(f"绿牌 AP@0.5: {results.box.ap50[1]:.4f}")
    # 黄牌
    print(f"单层黄牌 AP@0.5: {results.box.ap50[2]:.4f}")
    # 白牌
    print(f"白牌/其他 AP@0.5: {results.box.ap50[3]:.4f}")

'''
                 Class     Images  Instances      Box(P          R      mAP50  mAP50-95): 100% ━━━━━━━━━━━━ 261/261 9.1it/s 28.6s
                   all       4170       4768       0.86      0.788      0.844      0.739
                  blue       2105       2509      0.974      0.952      0.972      0.838
                 green       2000       2000      0.999      0.985      0.992      0.887
         yellow_single        248        255      0.969      0.965      0.976       0.84
                 other          4          4        0.5       0.25      0.435      0.391
'''