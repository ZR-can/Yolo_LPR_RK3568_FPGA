# 2_Model_Conversion_PC_Simulation

## 功能说明

该目录用于模型导出、模型转换和 PC 侧仿真验证，主要包括：

- YOLOv8 模型导出与 RKNN 转换
- LPRNet 模型导出与 RKNN 转换
- ONNX / PyTorch / RKNN 的 PC 侧推理验证
- 量化数据集与转换中间产物管理

## 目录结构

- `yolov8/`
  YOLOv8 模型、转换脚本、PC 侧仿真脚本
- `LPRNet/`
  LPRNet 模型、ONNX 导出、RKNN 转换和 PC 侧评估脚本
- `py_utils/`
  YOLOv8 PC 仿真和后处理共用工具
- `ultralytics_yolov8/`
  本地参考或适配代码

## 运行环境

- 默认使用 conda 环境：`YOLOv8n_LPRNet`
- ONNX -> RKNN 转换通常在 Ubuntu 虚拟机中执行
- 需要本地可用的 RKNN Toolkit 环境

## 常用使用方法

### 1. YOLOv8 转换为 RKNN

```bash
cd 2_Model_Conversion_PC_Simulation/yolov8/python
python convert.py ../model/best.onnx rk3568 i8 ../model/yolov8.rknn
```

说明：

- `best.onnx` 为待转换的 YOLOv8 ONNX 模型
- `rk3568` 为目标平台
- `i8` 表示量化模型
- 输出模型通常保存为 `../model/yolov8.rknn`

### 2. YOLOv8 的 PC 侧仿真验证

```bash
cd 2_Model_Conversion_PC_Simulation/yolov8/python
python yolov8_PC.py --model_path ../model/best.onnx --img_folder ../model/test_imgs --img_save
python yolov8_PC.py --model_path ../model/yolov8.rknn --img_folder ../model/test_imgs --img_save
```

说明：

- `yolov8_PC.py` 支持 `.pt`、`.onnx` 和 `.rknn`
- 结果图默认可输出到 `python/result/`

### 3. LPRNet 导出 ONNX

```bash
cd 2_Model_Conversion_PC_Simulation/LPRNet/python
python export_onnx_repair.py
```

说明：

- 当前脚本内部通过常量指定使用的权重文件和导出路径
- 导出结果通常位于 `../model/`

### 4. LPRNet 转换为 RKNN

```bash
cd 2_Model_Conversion_PC_Simulation/LPRNet/python
python convert.py ../model/lprnet7repair.onnx rk3568 i8 ../model/lprnet7repair_i8.rknn
python convert.py ../model/lprnet8repair.onnx rk3568 i8 ../model/lprnet8repair_i8.rknn
```

如果需要浮点模型，可将 `i8` 改为 `fp`。

### 5. LPRNet 的 PC 侧评估

```bash
cd 2_Model_Conversion_PC_Simulation/LPRNet/python
python lprnet_PC_eval.py --onnx_path ../model/lprnet8repair.onnx --quant
```

说明：

- `lprnet_PC_eval.py` 会在 PC 模拟器中构建并评估模型
- 默认目标平台配置为 `rk3568`
- 默认测试目录与量化数据集路径可通过参数覆盖

## 相关子说明

- 更细的 YOLOv8 示例说明见 [yolov8/README.md](yolov8/README.md)
- 更细的 LPRNet 示例说明见 [LPRNet/README.md](LPRNet/README.md)

## 注意事项

- 量化数据集文件路径由脚本内部常量决定，修改模型版本时要同步检查。
- 不同模型版本的输出命名应保持统一，便于 `3_NPU_Yolov8_LPR_Demo` 直接引用。
- 转换结果是否可用，仍需要最终到 `3` 中完成板端验证。
