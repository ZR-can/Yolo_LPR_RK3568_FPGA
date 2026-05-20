# 1_PC_Training

## 功能说明

该目录用于完成训练前与训练中的 PC 侧工作，主要包括：

- 数据集整理、筛选、格式转换
- YOLOv8 车牌检测模型训练与验证
- LPRNet 车牌字符识别模型训练与验证
- 训练结果、权重与测试输出管理

## 目录结构

- `requirements.txt`
  Python 依赖列表
- `configs/`
  YOLO 训练配置，例如 `yolo_config.yaml`
- `datasets/`
  本地训练与验证数据集
- `scripts/`
  YOLO 数据处理、统计、训练与验证脚本
- `LPRNet_Pytorch/`
  LPRNet 训练、测试、模型与权重

## 运行环境

- 默认使用 conda 环境：`YOLOv8n_LPRNet`
- 如需补装依赖，可在本目录执行：

```bash
pip install -r requirements.txt
```

## 常用使用方法

### 1. 数据处理

`scripts/` 中包含多种数据处理脚本，例如：

- `process_ccpd.py`
- `process_crpd.py`
- `process_cblprd.py`
- `split_lprnet_dataset.py`
- `yolo_lprnet_crops.py`

这些脚本用于生成 YOLO 检测数据、LPRNet 识别数据，或把 YOLO 检测结果裁剪成 LPRNet 训练样本。

### 2. 训练 YOLOv8 车牌检测模型

```bash
cd 1_PC_Training/scripts
python train_yolo.py --config ../configs/yolo_config.yaml
```

说明：

- 默认入口脚本为 `train_yolo.py`
- 默认配置文件为 `../configs/yolo_config.yaml`
- 训练结果通常保存在脚本指定的 `runs/` 或输出目录中

### 3. 训练 LPRNet 字符识别模型

```bash
cd 1_PC_Training/LPRNet_Pytorch
python train_LPRNet.py
```

说明：

- 默认训练参数写在 `train_LPRNet.py` 中
- 当前脚本中包含本地路径默认值，若目录调整，需要同步修改
- 训练权重保存在 `LPRNet_Pytorch/weights/`

### 4. 测试 LPRNet 模型

```bash
cd 1_PC_Training/LPRNet_Pytorch
python test_LPRNet.py
```

## 注意事项

- 本目录中的训练脚本大量使用本地绝对路径默认值，迁移环境后应先检查路径参数。
- `datasets/`、训练输出、临时结果图不应作为常规源码改动提交。
- 若训练后需要上板，请将最终模型转交给 `2_Model_Conversion_PC_Simulation` 做 ONNX / RKNN 处理。
