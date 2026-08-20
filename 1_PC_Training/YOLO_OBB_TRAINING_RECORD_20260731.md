# YOLOv8n-OBB 训练记录（2026-07-31）

## 训练目标

- 任务：4 类车牌 OBB 检测。
- 类别：`blue=0`、`green=1`、`yellow_single=2`、`other=3`。
- 初始权重：`pretrained/yolov8n-obb.pt`。
- 数据配置：`configs/yolo_obb_config.yaml`。
- 训练配置：`configs/yolo_obb_train.yaml`。
- 运行环境：Conda `YOLOv8n_LPRNet`，Python 对应
  `C:/Users/ZR/.conda/envs/YOLOv8n_LPRNet/python.exe`。
- 软件与硬件：PyTorch 2.1.0+cu121、Ultralytics 8.4.33、
  NVIDIA GeForce RTX 4060 Laptop GPU（8,188 MiB）。

## 关键参数

- `imgsz=640`、`batch=16`、`epochs=100`、`patience=20`。
- `optimizer=AdamW`、`lr0=0.001`、`cos_lr=true`、`amp=true`。
- `device=0`、`workers=8`、`seed=20260731`、`deterministic=true`。
- 验证入口：`datasets/yolo_obb_640/val_balanced.txt`，共 2,918 张。
- 训练集：40,921 张；完整 val：9,021 张；test：9,231 张。

## 训练终态与测试集评估

### 训练终态

- 用户于 2026-07-31 约 17:59 确认停止训练；当时 epoch 54 已完整结束，
  epoch 55 正运行至约 `956/2558` batch（37%）。
- Windows 普通进程树终止被数据加载子进程拒绝，核对 PID `4032` 及其
  24 个直接子进程均属于本次训练后，对该训练进程树执行强制终止。
- 终止后已复核：不存在 `train_yolo.py` 训练进程，GPU 显存回落至约
  50 MiB；未完成的 epoch 55 未写入 `results.csv`，不作为有效结果。
- 最终完整轮及最佳轮均为 epoch 54：Precision=0.98237、
  Recall=0.97433、mAP50=0.98846、mAP50-95=0.93734。
- epoch 54 训练损失：box=0.39434、cls=0.28121、dfl=0.86866、
  angle=0.00299；验证损失：box=0.35534、cls=0.33232、
  dfl=0.82138、angle=0.00111。
- 最终 `best.pt` 与 `last.pt` 文件大小均为 18,995,977 字节，SHA-256
  均为 `559EEFAD166B618D7C032F05E5C92942DDEC0268D0F5ED15D829DC1050084A0A`，
  表明二者均对应完整保存的 epoch 54 checkpoint。

### 冻结测试集评估

- 评估时间：2026-07-31 18:00–18:03。
- AP@80 与 Macro-F1 补充评估时间：2026-08-20；复用同一 epoch 54
  `best.pt`、同一冻结 test 集及原评估配置，原总体指标复现一致。
- 模型：最终 epoch 54 `best.pt`；任务为 OBB，`split=test`、
  `imgsz=640`、`batch=16`、`device=0`。
- 测试集：9,231 张图片、11,010 个车牌实例、62 张背景图，`corrupt=0`。

| 类别 | 图片 | 实例 | Precision | Recall | F1 | AP@50 | AP@80 | AP@50-95 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `blue` | 4,064 | 5,711 | 0.974 | 0.906 | 0.939 | 0.987 | 0.955 | 0.932 |
| `green` | 5,006 | 5,006 | 1.000 | 0.984 | 0.992 | 0.995 | 0.994 | 0.937 |
| `yellow_single` | 249 | 256 | 0.958 | 0.918 | 0.937 | 0.952 | 0.892 | 0.860 |
| `other` | 37 | 37 | 0.902 | 0.919 | 0.910 | 0.948 | 0.938 | 0.924 |
| **总体/宏平均** | **9,231** | **11,010** | **0.958** | **0.932** | **0.944** | **0.970** | **0.945** | **0.913** |

- 测试集总体 AP@80 为 0.945、mAP50-95 为 0.913；后者低于平衡验证集
  epoch 54 的 0.93734。AP 与 F1 均按四个类别宏平均，且
  `yellow_single` 与 `other` 样本明显偏少。
- F1 曲线峰值为 Macro-F1=0.944（图中四舍五入为 0.94），对应
  `confidence=0.705`；该值是离线测试最优工作点，不等同于当前部署阈值。
- 测试集主要短板为 `yellow_single`（AP@80=0.892、mAP50-95=0.860）；`blue` 的主要
  问题表现为 Recall=0.906，说明漏检风险高于其定位精度所反映的水平。
- `other` 仅 37 个测试实例，其 0.924 指标统计不稳定；应结合真实视频
  与更多独立特殊车牌样本复核，不能仅据该数值判断泛化能力。
- 推理速度：单图预处理 0.7 ms、模型推理 3.2 ms、后处理 2.0 ms；这是
  RTX 4060 Laptop GPU 上的 PC 评估速度，不代表 RK3568 板端速度。
- 评估输出：
  `scripts/runs/obb_test_results/yolov8n_plate_obb_640_test_epoch54`。
- 标准输出：`logs/yolo_obb_test_epoch54_20260731.stdout.log`；标准错误为空：
  `logs/yolo_obb_test_epoch54_20260731.stderr.log`。
- 测试生成了 PR/F1/P/R 曲线、混淆矩阵及 3 组标签/预测可视化，评估进程
  正常退出，未发现 OOM、Traceback、RuntimeError、NaN/Inf 或非零
  corrupt 计数。
