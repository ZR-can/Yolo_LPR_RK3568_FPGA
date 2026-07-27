# YOLO 特殊车牌微调记录（2026-07-26）

## 结论

本轮微调已完成并通过独立 test 对比。新 `best.pt` 显著提升 `other`，同时
blue/green/yellow_single 三个核心类别的 AP50-95 整体保持稳定或提升，可作为
替换旧 YOLO 检测权重的候选。

- 新权重 SHA256：
  `54755C4233381AE98FB6A0DB04A67A03FD14E74405F05119D1B8068B09F49AD7`
- 微调前权重 SHA256：
  `906172B90E0EE2347F82C8C90DB8CB86C65A663949C1B347C7266A43623D9F86`
- 最佳轮次：epoch 23/30
- 最佳训练期 val mAP50/mAP50-95：0.98995/0.84711
- 固定 test mAP50/mAP50-95：0.98275/0.85198

## 数据审计

### 原数据 `yolo_format`

| 划分 | 图片 | 框 | blue | green | yellow_single | other |
|---|---:|---:|---:|---:|---:|---:|
| train | 12,252 | 14,075 | 5,516 | 3,500 | 4,637 | 422 |
| val | 3,797 | 4,217 | 1,757 | 1,001 | 1,452 | 7 |
| test | 4,170 | 4,768 | 2,509 | 2,000 | 255 | 4 |
| 合计 | 20,219 | 23,060 | 9,782 | 6,501 | 6,344 | 433 |

全量图片均可解码且图片/标签成对。发现：

- 1 个旧 train 框越出归一化右边界；
- 30 组跨 train/val/test 的完全相同图片；
- 无跨划分同名图片。

### 新增 `特殊车牌`

| 子类 | 图片 | 原始框 |
|---|---:|---:|
| 教练车牌 | 86 | 86 |
| 香港出入境车牌 | 81 | 81 |
| 澳门出入境车牌 | 77 | 78 |
| 警用车牌 | 84 | 84 |
| 合计 | 328 | 329 |

新增图片均为 1600×1200，框中位数约 167×52 px，归一化框面积中位数
0.00455，与旧数据中位数 0.00484 接近。保持 640 输入时典型车牌约
67×21 px，无需改变当前 RK3568 部署输入尺寸。

源标签的四个子目录都采用各自的局部 `0=other`，合并到当前四分类时已重映射为
全局 `3=other`。`粤ZZ473澳` 的两个 IoU≈0.987 重复框已合并，最终为 328 个
有效新增框。

## 派生数据集策略

原 `yolo_format` 保持只读，派生集使用 NTFS 图片硬链接：

- 新增四个子类分别按固定种子 `20260726` 做 8:1:1 分层划分；
- 唯一新增源图 train/val/test 为 262/33/33；
- 只在 train 将新增源图重复为 3 份，使随机增强下的有效 other 比例提高；
- val/test 不重复；
- 旧集完全重复图按 `test > val > train` 保留一份；
- 旧集唯一越界框只在派生标签中裁到边界。

| 划分 | 图片 | 框 | blue | green | yellow_single | other |
|---|---:|---:|---:|---:|---:|---:|
| train | 13,012 | 14,835 | 5,490 | 3,500 | 4,637 | 1,208 |
| val | 3,826 | 4,246 | 1,753 | 1,001 | 1,452 | 40 |
| test | 4,203 | 4,801 | 2,509 | 2,000 | 255 | 37 |

## 配置依据

`yolo_config.yaml` 只保留 Ultralytics 数据描述；训练参数独立放在
`yolo_finetune_train.yaml`，避免旧配置中数据字段与无效训练字段混用。

主要训练参数：

- 输入 640、batch 16、AMP；
- AdamW，`lr0=2e-4`，`lrf=0.05`，cosine LR；
- 30 epochs，patience 10；
- `cache=false`：训练前主机仅约 3.3 GB 可用内存；
- `hsv_h=0.005`、`hsv_s=0.3`、`hsv_v=0.3`、`scale=0.3`；
- `mosaic=0.5`、最后 5 轮关闭 mosaic；
- 从旧 `best.pt` 初始化，`resume=false`；只有中断恢复才显式传
  `--resume-checkpoint`。

降低 HSV 扰动是因为 blue/green/yellow/other 的类别本身依赖颜色语义；
类别不平衡通过数据层过采样处理，不使用旧配置中的无效 `cls_pw`。

## 训练环境与过程

- GPU：NVIDIA GeForce RTX 4060 Laptop GPU，8 GB
- Python：3.10.20
- PyTorch：2.1.0+cu121
- Ultralytics：8.4.33
- 完成轮次：30
- 有效 epoch 训练时间：3,646.21 秒（约 60.77 分钟）
- epoch 20 后做过一次受控断点恢复；checkpoint 含 optimizer，epoch 21 指标与
  前段连续。

最佳 epoch 23：

- train box/cls/dfl loss：0.58150/0.29350/0.88678
- val box/cls/dfl loss：0.63101/0.29557/0.94962
- precision/recall：0.96584/0.96943
- mAP50/mAP50-95：0.98995/0.84711

## 固定 val 对比

| 指标 | 微调前 | 微调后 | 变化 |
|---|---:|---:|---:|
| 总体 Precision | 0.93611 | 0.96594 | +0.02983 |
| 总体 Recall | 0.87354 | 0.96950 | +0.09596 |
| 总体 mAP50 | 0.92065 | 0.98995 | +0.06930 |
| 总体 mAP50-95 | 0.77181 | 0.84952 | +0.07771 |
| blue AP50-95 | 0.82501 | 0.82622 | +0.00122 |
| green AP50-95 | 0.87431 | 0.87959 | +0.00528 |
| yellow_single AP50-95 | 0.83118 | 0.83696 | +0.00578 |
| other Recall | 0.52500 | 0.97500 | +0.45000 |
| other AP50 | 0.71511 | 0.98833 | +0.27322 |
| other AP50-95 | 0.55675 | 0.85530 | +0.29855 |

## 固定 test 对比

| 指标 | 微调前 | 微调后 | 变化 |
|---|---:|---:|---:|
| 总体 Precision | 0.89071 | 0.97262 | +0.08191 |
| 总体 Recall | 0.86486 | 0.95006 | +0.08520 |
| 总体 mAP50 | 0.90993 | 0.98275 | +0.07282 |
| 总体 mAP50-95 | 0.77348 | 0.85198 | +0.07851 |
| blue AP50-95 | 0.84298 | 0.84824 | +0.00526 |
| green AP50-95 | 0.88820 | 0.88748 | -0.00072 |
| yellow_single AP50-95 | 0.78153 | 0.81207 | +0.03053 |
| other Recall | 0.51049 | 0.91892 | +0.40843 |
| other AP50 | 0.72094 | 0.96762 | +0.24668 |
| other AP50-95 | 0.58118 | 0.86015 | +0.27896 |

green 的 AP50-95 变化为 -0.00072，远小于单次评估波动；blue 和
yellow_single 均提升。按 test 结果，新权重满足“提升 other 且不明显损害核心
类别”的采用条件。

## 复现

```bash
cd 1_PC_Training
python scripts/analyze_yolo_finetune_dataset.py
python scripts/prepare_yolo_finetune_dataset.py --dry-run
python scripts/prepare_yolo_finetune_dataset.py --apply

cd scripts
python evaluate_yolo_finetune.py --split val --name before_finetune_val
python evaluate_yolo_finetune.py --split test --name before_finetune_test
python train_yolo.py
python evaluate_yolo_finetune.py \
  --model runs/finetune_results/yolov8n_special_other_20260726/weights/best.pt \
  --split test \
  --name after_finetune_test
```

## ONNX 导出验证（2026-07-26）

- `2_Model_Conversion_PC_Simulation/yolov8/model/finetune.pt` 与本轮最佳
  `best.pt` 的 SHA256 均为
  `54755C4233381AE98FB6A0DB04A67A03FD14E74405F05119D1B8068B09F49AD7`；
- `finetune.onnx` 的 SHA256 为
  `6A0FDC607927EDB7893E165CCBC74DDAED33F8A3ED1680B7873225670E3DEBF7`，
  ONNX full checker 通过；
- 模型为静态 `[1,3,640,640]`、FP32、opset 12，输出为 RKNN 后处理需要的
  3 个尺度 × 3 路共 9 路张量：每尺度依次为 64 通道 DFL、4 通道分类概率和
  1 通道 score sum；输出形状及顺序与旧 `best.onnx` 一致；
- 在固定 test 中抽取 blue、green、yellow_single 以及学、港、澳、警共 7 张图，
  使用相同黑色 letterbox 和 RGB `/255` 输入比较 PT 与 ONNX。板端
  `conf=0.55、NMS=0.5` 口径下共得到 8 个检测，数量和类别全部一致；
- 7 张图的最大解码框坐标差为 `1.14440918e-05 px`，最大置信度差为
  `2.98023224e-07`。九路原始输出的最大绝对差出现在 20×20 DFL 分支，为
  `0.00720214844`，该分支平均绝对差为 `2.86663351e-05`，没有造成解码结果
  变化。

结论：`finetune.onnx` 与本轮微调 PT 权重及现有 RKNN 九输出后处理兼容，可以进入
校准集构建和 RKNN FP/INT8 转换阶段；该结论不等同于 INT8 精度已验证。

## INT8 量化校准集（2026-07-26）

已生成车牌专用 `finetune_quant_dataset`：

- blue、green、yellow_single 分别从派生 train 唯一源图固定抽取 600 张；
- 所有包含 `other` 的源图跨 train/val/test 纳入，按源路径去除新增 train 的
  3 份重复样本后为 761 张；
- `other` 中旧数据 433 张、新增特殊车牌 328 张；新增子类为教练 86、香港 81、
  澳门 77、警用 84；
- 总计 2,561 张，全部经过黑色 letterbox 生成 `640x640` 正常颜色 JPEG；
- 全量验证通过：2,561 张均可解码且为 `640x640x3`，dataset 清单、manifest 和
  实际文件一一对应，无缺失路径；
- 761 张 `other` 是唯一源路径数。旧 train 中有 2 组完全相同内容的
  `other` 图片，因本轮要求保留所有旧 `other` 而继续纳入。

后续通道审计发现上述“BGR 到 RGB 后直接 `cv2.imwrite()`”会使磁盘 JPEG 的红蓝
通道交换，黄牌显示为青蓝色。生成脚本已去掉该转换，并使用完全相同的 2,561
条选择名单原位重建；错误版本临时备份已在新版本验证通过后删除。全量图片均可
解码且为 `640x640x3`，dataset、manifest 和实际文件一一对应。新输出相对正常
BGR letterbox 的平均绝对像素误差为 `0.90524`，相对红蓝交换输入为 `6.32248`，
目视黄牌颜色正常，`finetune_quant_summary.json` 状态为 `VALID`。当前数据集
适配 RKNN Toolkit2 2.3.0 默认 `quant_img_RGB2BGR=False`，可继续正式量化。

输出：

- `2_Model_Conversion_PC_Simulation/yolov8/model/finetune_quant_dataset/`
- `2_Model_Conversion_PC_Simulation/yolov8/model/finetune_quant_dataset.txt`
- `2_Model_Conversion_PC_Simulation/yolov8/model/finetune_quant_manifest.tsv`
- `2_Model_Conversion_PC_Simulation/yolov8/model/finetune_quant_summary.json`

全量 2,561 张数据只作为候选池。转换前必须从中生成 100–200 张的分层量化
子清单，并将 `convert.py` 的 `DATASET_PATH` 从交通数据集切换为该子清单，例如
`../model/finetune_quant_dataset_200.txt`；推荐默认输出名为
`../model/finetune_i8.rknn`。量化算法、`mean/std` 和 `model_pruning` 保持旧车牌
模型配置不变。

## RKNN INT8 首次构建诊断（2026-07-26）

在 RKNN Toolkit2 2.3.0 Ubuntu 环境使用全量 2,561 张清单执行：

```bash
python convert.py ../model/finetune.onnx rk3568 i8 ../model/finetune_i8.rknn
```

ONNX 126 个参数加载和各阶段 OpFusing 均完成；Toolkit 报告
`model.0.conv.weight=-21.157`、`model.22.cv2.2.2.weight=-31.344` 两处权重
outlier，随后进程在 `Quantizating 0/160` 仅输出“已杀死”，没有 Python
traceback。outlier 是非致命精度警告，不是本次中止原因；无异常栈的直接
SIGKILL 与 Linux/虚拟机 OOM 行为一致。

2,561 张 `640x640x3` 图片仅按 uint8 完全解码约占 3.15 GB，若 Toolkit 缓存
float32 输入则约占 12.59 GB，尚未计入模型图、逐层直方图和系统开销。RKNN
Toolkit2 2.3.0 官方建议一般校准集为 20–200 张，KL-Divergence 通常为
20–100 张，并说明增加图片数量不一定提升精度。因此现有 2,561 张数据改作候选
池，下一步从中生成类别和特殊子类均衡的 100–200 条子清单后重新构建；不优先
切换 MMSE，因为其内存和耗时更高。

进一步核对 Git 历史确认：旧 4,215 张校准集生成 `yolov8.rknn` 时，
`convert.py` 尚未配置 `quantized_algorithm` 和 `model_pruning`，因此使用的是
Toolkit 默认 `normal + channel` 且不剪枝；`kl_divergence +
model_pruning=True` 是 2026-07-23 为交通模型转换加入的配置。新旧车牌 ONNX
均为 226 个节点、126 个 initializer 和相同九输出拓扑，微调模型没有结构性
增大。旧全量量化成功与本次 14 GB 下失败的核心差异是算法/剪枝配置，而不是
4,215 对 2,561 的图片数量。后续先用 `normal + channel +
model_pruning=False` 全量复现旧口径，再用 100–200 张分层子清单测试 KL；两项
实验不得混在同一个候选中，以便归因精度、内存和耗时变化。

## 后续边界

当前新增独立 val/test 各只有 33 张，足以验证本轮方向，但不足以覆盖所有现场域。
部署替换前仍应：

1. 使用已生成校准集生成 RKNN FP/INT8 候选，并完成逐级精度对比；
2. 用固定板端图片/视频回归蓝、绿、单黄和四类特殊车牌；
3. 继续收集不同相机、夜间、遮挡、雨雾和更小目标的 `other` 独立测试样本。
