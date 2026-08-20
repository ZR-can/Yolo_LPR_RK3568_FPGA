# YOLOv8n-OBB 训练、转换与 RK3568 部署报告

报告日期：2026-08-20  
适用范围：当前仓库图片车牌识别模式  
目标平台：RK3568 Linux + RKNN Runtime 2.3.0  

## 1. 报告结论

当前图片模式已经形成一条独立且闭合的 OBB 检测链路：

```text
PCIe 1280x720 BGR565 原始帧
  -> RGB888 等比例 Letterbox 640x640，填充值 114
  -> YOLOv8n-OBB W8A8 INT8 RKNN 零拷贝推理
  -> 四路 native INT8 输出解包与反量化
  -> 三尺度 DFL、类别和角度解码
  -> 同类别旋转 IoU NMS
  -> 回映射原始帧四角点
  -> 旋转矩形矫正、双线性采样
  -> 48xresized_width BGR 有效图像，右侧填充到 48x160
  -> 蓝绿高重叠候选颜色证据裁决
  -> UINT8 BGR NHWC [1,48,160,3] PP-OCR 输入
```

最终 epoch 54 模型在冻结测试集上的总体指标为：

```text
Precision = 0.958
Recall = 0.932
mAP50 = 0.970
mAP50-95 = 0.913
```

RK3568 正式性能口径采用 `RKNN_LOG_LEVEL=0`。在项目 6 的零拷贝板端实测中，
纯 NPU 平均耗时为 `31.7786 ms`，理论吞吐为 `31.468 FPS`；从 NPU 推理开始，
加上 native 输出解包、OBB 解码、旋转 NMS 和一个车牌的 OCR 输入矫正后，完整
pre-OCR 平均耗时为 `38.6083 ms`，理论吞吐为 `25.901 FPS`。若再串行计入本次
测试的 RGA letterbox，则为约 `42.6506 ms / 23.45 FPS`。

图片模式生产阈值最终固定为 `confidence=0.55`、同类别旋转
`NMS IoU=0.55`。训练测试曲线记录的总体 F1 峰值为
`F1=0.94 @ confidence=0.705`，但部署阈值按当前实车召回与稳定性要求放宽到
`0.55`。项目 6 性能日志使用的是早期 `conf=0.25 / nms=0.40`，只能用于耗时
基线，不能替代当前生产阈值的精度结论。

视频模式不使用本报告的 OBB 检测链路，仍使用当前微调后的轴对齐 YOLO 和原视频
Tracker，未因图片模式接入 OBB 而改变。

## 2. 依据与数据口径

本报告只使用当前仓库代码、训练记录、转换记录和已拉回的 RK3568 日志，不把流程图
中的文字当成独立事实来源。主要依据如下：

- [OBB 训练记录](D:/Yolo_LPR_RK3568_FPGA_Project/1_PC_Training/YOLO_OBB_TRAINING_RECORD_20260731.md)
- [OBB 数据集记录](D:/Yolo_LPR_RK3568_FPGA_Project/1_PC_Training/YOLO_OBB_DATASET_RECORD_20260731.md)
- [OBB 转换说明](D:/Yolo_LPR_RK3568_FPGA_Project/2_Model_Conversion_PC_Simulation/yolov8_obb/README.md)
- [RKNN 转换脚本](D:/Yolo_LPR_RK3568_FPGA_Project/2_Model_Conversion_PC_Simulation/yolov8_obb/python/convert.py)
- [量化集构建脚本](D:/Yolo_LPR_RK3568_FPGA_Project/2_Model_Conversion_PC_Simulation/yolov8_obb/python/build_quant_dataset.py)
- [板端性能记录](D:/Yolo_LPR_RK3568_FPGA_Project/6_Temporary_OBB_Perf_Demo/README.md)
- [RKNN_LOG_LEVEL=0 原始日志](D:/Yolo_LPR_RK3568_FPGA_Project/6_Temporary_OBB_Perf_Demo/result/obb_zero_copy_log_level_0.txt)
- [生产 OBB pipeline](D:/Yolo_LPR_RK3568_FPGA_Project/3_NPU_Yolov8_PPOCR_Demo/src/yolo_ppocr_pipeline.cc)
- [生产 OBB 后处理](D:/Yolo_LPR_RK3568_FPGA_Project/3_NPU_Yolov8_PPOCR_Demo/src/obb_postprocess.cc)
- [PP-OCR 输入实现](D:/Yolo_LPR_RK3568_FPGA_Project/3_NPU_Yolov8_PPOCR_Demo/src/ppocr_rec.cc)

本文中的 FPS 分为三种口径：

1. 实测 FPS：日志直接报告的帧率。
2. 理论 FPS：按 `1000 / 单帧平均耗时(ms)` 换算，假设各阶段完全串行且持续有输入。
3. 应用可见 FPS：还受 PCIe 输入、每两帧调度一次、PP-OCR、Tracker、队列和 Qt 绘制限制，
   不能直接用单模型理论 FPS 代替。

## 3. 数据集与训练配置

### 3.1 任务与类别

任务为四类车牌旋转框检测，类别映射固定为：

| 类别 ID | 类别名 | 主要含义 |
|---:|---|---|
| 0 | `blue` | 蓝牌 |
| 1 | `green` | 绿牌/新能源牌 |
| 2 | `yellow_single` | 单层黄牌 |
| 3 | `other` | 教练、港澳出入境、警用等其他牌型 |

OBB 标签每个目标为 9 列：

```text
class_id x1 y1 x2 y2 x3 y3 x4 y4
```

四角点归一化到 `[0,1]`，并按稳定顺时针顺序保存。训练阶段才执行
`imgsz=640` letterbox，源图没有被预先强制拉伸到 640x640。

### 3.2 数据划分

| 划分 | 图片数 | 说明 |
|---|---:|---|
| train | 40,921 | 正式训练集 |
| 完整 val 审计池 | 9,021 | 不直接作为训练期间验证入口 |
| `val_balanced.txt` | 2,918 | 训练期间平衡验证入口 |
| test | 9,231 | 最终冻结测试集 |

冻结 test 含 11,010 个车牌实例、62 张背景图，`corrupt=0`。数据构建使用固定
随机种子 `20260731`，同一源图跨 train/val/test 泄漏数为 0。

### 3.3 训练参数

| 参数 | 配置 |
|---|---|
| 初始权重 | `yolov8n-obb.pt` |
| 输入 | `640x640` |
| 计划 epoch | 100 |
| batch | 16 |
| optimizer | AdamW |
| 初始学习率 | 0.001 |
| 学习率策略 | cosine |
| AMP | 开启 |
| patience | 20 |
| workers | 8 |
| seed | 20260731 |
| deterministic | 开启 |

训练环境为 PyTorch `2.1.0+cu121`、Ultralytics `8.4.33`、RTX 4060 Laptop
GPU 8,188 MiB。训练在 epoch 55 未完成时由用户停止，epoch 55 未写入有效结果；
最终完整轮和最佳轮均为 epoch 54。

### 3.4 Epoch 54 训练终态

| 指标 | 数值 |
|---|---:|
| Precision | 0.98237 |
| Recall | 0.97433 |
| mAP50 | 0.98846 |
| mAP50-95 | 0.93734 |
| train box loss | 0.39434 |
| train cls loss | 0.28121 |
| train dfl loss | 0.86866 |
| train angle loss | 0.00299 |
| val box loss | 0.35534 |
| val cls loss | 0.33232 |
| val dfl loss | 0.82138 |
| val angle loss | 0.00111 |

`best.pt` 与 `last.pt` 均为 18,995,977 字节，SHA-256 均为：

```text
559EEFAD166B618D7C032F05E5C92942DDEC0268D0F5ED15D829DC1050084A0A
```

这说明两者均对应完整保存的 epoch 54 checkpoint。

## 4. 冻结测试集结果

最终评估参数为 OBB、`split=test`、`imgsz=640`、`batch=16`、`device=0`。

| 类别 | 图片 | 实例 | Precision | Recall | mAP50 | mAP50-95 |
|---|---:|---:|---:|---:|---:|---:|
| `blue` | 4,064 | 5,711 | 0.974 | 0.906 | 0.987 | 0.932 |
| `green` | 5,006 | 5,006 | 1.000 | 0.984 | 0.995 | 0.937 |
| `yellow_single` | 249 | 256 | 0.958 | 0.918 | 0.952 | 0.860 |
| `other` | 37 | 37 | 0.902 | 0.919 | 0.948 | 0.924 |
| **总体** | **9,231** | **11,010** | **0.958** | **0.932** | **0.970** | **0.913** |

结果解读：

- `green` 的检测性能最稳定，Recall 为 `0.984`。
- `blue` 的 Recall 为 `0.906`，主要风险是漏检而不是定位精度不足。
- `yellow_single` 的 mAP50-95 最低，为 `0.860`，是当前主要精度短板。
- `other` 只有 37 个测试实例，`0.924` 的 mAP50-95 统计稳定性不足，必须结合更多
  独立特殊车牌和实板输入判断。
- 总体 mAP50-95 `0.913` 低于平衡验证集 epoch 54 的 `0.93734`，符合冻结测试集
  类别分布更不均衡的情况。

测试曲线记录总体 `F1=0.94 @ confidence=0.705`。当前生产 `confidence=0.55`
是后续按实车召回和用户要求选定的部署工作点，不应写成离线 F1 数学最优阈值。

PC 测试速度为预处理 `0.7 ms`、模型推理 `3.2 ms`、后处理 `2.0 ms`，合计
`5.9 ms/图`，简单换算约 `169.49 图/秒`。该数据来自 RTX 4060 Laptop GPU，
不代表 RK3568，也不包含 PCIe、OCR、Tracker 或显示。

## 5. PT 到 ONNX 转换与验证

### 5.1 模型产物

| 产物 | 大小 | SHA-256 |
|---|---:|---|
| `best.pt` | 18,995,977 B | `559EEFAD...0084A0A` |
| `best.onnx` | 12,359,048 B | `03F65861...A28846` |
| `yolov8_obb_i8.rknn` | 4,361,505 B | `66973743...532950` |

相对 ONNX，最终 RKNN 文件大小减少约 `64.71%`。文件大小变化只能说明部署包体积，
不能单独证明量化精度。

### 5.2 RKNN 友好型 ONNX 契约

当前 `best.onnx` 是固定输入、raw-head 输出模型。ONNX opset 为 12、IR version 为 7，
包含 243 个节点和 144 个 initializer。

| Tensor | FP32 Shape | 含义 |
|---|---|---|
| `images` | `[1,3,640,640]` | 归一化 RGB 输入 |
| output 0 | `[1,68,80,80]` | stride 8，64 DFL + 4 类 |
| output 1 | `[1,68,40,40]` | stride 16，64 DFL + 4 类 |
| output 2 | `[1,68,20,20]` | stride 32，64 DFL + 4 类 |
| output 3 | `[1,1,8400]` | 三尺度拼接后的 angle feature |

图中只保留 RKNN 支持良好的 `Add`、`Concat`、`Constant`、`Conv`、`MaxPool`、
`Mul`、`Reshape`、`Resize`、`Sigmoid` 和 `Split`。DFL 期望值、三角函数、旋转
NMS、原图回映射和车牌矫正均放在模型外的 C++ 后处理中。

### 5.3 ONNX 一致性验证

`onnx.checker.check_model()` 已通过。使用真实图片分别运行 PyTorch RKNN 导出头和
ONNX Runtime，四个输出形状全部一致；各输出平均绝对误差依次为：

```text
4.03e-6, 4.35e-6, 5.74e-6, 2.78e-7
```

四个输出均通过 `numpy.allclose(rtol=1e-4, atol=1e-4)`。angle feature 保持在
`[0,1]`，后处理按下式恢复角度：

```text
theta = (angle_feature - 0.25) * pi
```

每个类别选取一个真实样本进行 PT/ONNX 解码对比后，类别、置信度、角度和多边形一致；
蓝牌样本 ONNX 置信度为 `0.933`，四角点与 Ultralytics 预测相差不超过 1 像素。

## 6. RKNN INT8 量化与部署

### 6.1 校准集

量化校准集使用固定种子 `20260818`，从 train 中确定性选择 800 张唯一图片，每个
主类别 200 张：

| 主类别 | 数量 | 所选图片中包含该类别的图片数 |
|---|---:|---:|
| `blue` | 200 | 337 |
| `green` | 200 | 200 |
| `yellow_single` | 200 | 202 |
| `other` | 200 | 200 |

校准集同时按数据来源、车牌短边尺寸四分位、倾斜角区间和多车牌图片分层；其中
CCPD2019 163 张、CCPD2020 200 张、CRPD 351 张、特殊车牌 86 张，包含 162 张
多车牌图片。800 个条目通过 NTFS 硬链接复用训练源图，不复制像素数据，所有路径唯一且
`os.path.samefile()` 校验通过。

### 6.2 转换配置

正式转换环境为 Ubuntu `toolkit230`，RKNN Toolkit2 `2.3.0`，目标平台
`rk3568`。关键转换配置为：

```python
rknn.config(
    mean_values=[[0, 0, 0]],
    std_values=[[255, 255, 255]],
    target_platform="rk3568",
)
rknn.load_onnx(model="best.onnx")
rknn.build(do_quantization=True, dataset="obb_quant_dataset_800.txt")
rknn.export_rknn("yolov8_obb_i8.rknn")
```

最终模型为 W8A8 INT8。800 张校准图检查、图准备、10 个量化阶段、
`rknn.build()` 和 `rknn.export_rknn()` 均成功。FP16 和 INT8 的 `--check-only`
预检均通过，但当前仓库正式保留并完成板端性能测试的 OBB 部署产物是 INT8 RKNN；
不把 FP16 预检写成 FP16 实板结论。

### 6.3 输入归一化与零拷贝

生产代码查询模型 tensor 属性后，把 native 输入类型设为 `UINT8`，通过
`rknn_create_mem()` 和 `rknn_set_io_mem()` 绑定 640x640x3 输入内存。
RGA/CPU letterbox 直接写入该内存，Runtime/NPU 完成模型内归一化和量化。原始 RGB
像素不得在应用侧再除以 255，否则会发生二次归一化。

逻辑输入与 native 输入的板端属性为：

| 属性 | Shape | Format | Type | zp | scale |
|---|---|---|---|---:|---:|
| logical input | `[1,640,640,3]` | NHWC | INT8 | -128 | 0.00392156886 |
| native input | `[1,640,640,3]` | NHWC | UINT8 | -128 | 0.00392156886 |

### 6.4 Native INT8 输出

四个输出均绑定 native tensor memory，运行阶段不调用
`rknn_outputs_get()/release()`：

| 输出 | 逻辑 Shape | Native Shape/布局 | Native 分配大小 |
|---|---|---|---:|
| stride 8 | `[1,68,80,80]` | `[1,9,80,80,8]` NC1HWC2 | 512,000 B |
| stride 16 | `[1,68,40,40]` | `[1,9,40,40,8]` NC1HWC2 | 128,000 B |
| stride 32 | `[1,68,20,20]` | `[1,9,20,20,8]` NC1HWC2 | 32,000 B |
| angle | `[1,1,8400]` | 内部 8 通道对齐 | 67,200 B |

后处理先从 native 对齐布局解包为逻辑连续 INT8，再按每个输出自己查询到的 `zp` 和
`scale` 反量化：

```text
float_value = (int8_value - zero_point) * scale
```

angle 输出逻辑上只有 8,400 字节，但 Runtime native 内存需要 67,200 字节。当前实现按
native attr 分配，因此已经消除早期 `8400/67200` 大小告警。

## 7. 从 OBB 输入到 PP-OCR 输入的完整处理

### 7.1 PCIe 输入与 Letterbox

FPGA/PCIe 帧接口固定为：

```text
1280 x 720
BGR565
2 bytes/pixel
1,843,200 bytes/frame
```

图片模式把原始 `image_buffer_t` 直接交给 OBB pipeline。预处理建立 RKNN native
`RGB888 640x640` 目标缓冲，执行等比例 letterbox 并填充 `114`。对于标准
1280x720 帧，几何上应得到缩放比例 `0.5`、有效图像 `640x360`、垂直 padding
约 140 像素；实际回映射始终使用 `convert_image_with_letterbox()` 返回的
`scale/x_pad/y_pad`，不依赖硬编码尺寸推导。

### 7.2 OBB 解码

三个检测分支的 stride 分别为 8、16、32，每个位置含 64 个 DFL 通道和 4 个类别
logit。类别置信度为：

```text
score = sigmoid(dequantized_class_logit)
```

生产过滤阈值为 `score >= 0.55`。每条边的 16 个 DFL bin 先执行稳定 softmax，再取
期望值得到 `left/top/right/bottom` 距离。角度按：

```text
theta = (dequantized_angle_feature - 0.25) * pi
```

设网格为 `(x,y)`、stride 为 `s`，则旋转框参数恢复为：

```text
dx = (right - left) / 2
dy = (bottom - top) / 2
cx = (dx*cos(theta) - dy*sin(theta) + x + 0.5) * s
cy = (dx*sin(theta) + dy*cos(theta) + y + 0.5) * s
w  = (left + right) * s
h  = (top + bottom) * s
```

### 7.3 同类别旋转 NMS

候选按置信度从高到低排序。每个 OBB 根据 `cx/cy/w/h/theta` 计算四角点，两个旋转框
通过凸多边形裁剪求真实交集面积，再计算 rotated IoU。仅当候选属于同一类别且
`rotated IoU > 0.55` 时抑制低分框，最终最多保留 128 个结果。

蓝、绿等跨类别候选不会在旋转 NMS 中互相抑制。这样可避免 INT8 将蓝绿 logits 量化为
相同或接近分数时，提前丢掉真实颜色类别；跨颜色冲突在矫正后使用像素证据处理。

### 7.4 回映射原始帧四角点

先撤销 letterbox：

```text
source_cx = (cx - x_pad) / scale
source_cy = (cy - y_pad) / scale
source_w  = w / scale
source_h  = h / scale
```

如果 `source_h > source_w`，交换宽高并令 `theta += pi/2`，随后把角度归一化到
`[-pi/2, pi/2)`，保证送入 OCR 的车牌长边保持水平方向。再由中心、宽高和角度计算
原始帧中的左上、右上、右下、左下四角点。

### 7.5 旋转矫正与双线性采样

OCR 目标高度固定为 48，有效宽度保持车牌宽高比：

```text
resized_width = clamp(ceil(48 * source_w / source_h), 1, 160)
```

先创建 `48x160x3` 的交错 BGR 缓冲并全部填充 128。对于有效区域中的每个目标
像素，以 `(u,v)` 归一化到 `[0,1]`，反向映射到旋转源矩形：

```text
source_point = top_left
             + u * (top_right - top_left)
             + v * (bottom_left - top_left)
```

随后直接从原始 BGR565 或 RGB888 帧进行双线性采样，并按 B、G、R 顺序写入目标。
`resized_width` 右侧的剩余区域继续保持 128。

该实现完成的是旋转矩形的旋转与尺度矫正，不是任意四边形的透视变换。YOLO OBB 输出
本质为 `cx/cy/w/h/theta` 旋转矩形，因此可以校正倾斜角，但无法恢复训练标注中的梯形
透视形变；若未来需要梯形校正，检测模型或额外角点模型必须输出四个独立角点，并改用完整
单应性变换。

### 7.6 蓝绿高重叠候选裁决

每个候选的矫正图生成后、进入 PP-OCR 前，执行蓝绿冲突检查：

1. 只比较一蓝一绿候选。
2. 四角点外接矩形 IoU 必须 `>=0.65`。
3. 两个 OBB 检测置信度差必须 `<=0.10`。
4. 只统计矫正车牌有效宽度内部，并去除左右各 5%、上下各 12.5% 的边缘。
5. 忽略 `max(B,G,R)<50` 或 `max-min<24` 的暗色、低饱和像素。
6. 蓝色证据累加 `B-max(G,R)` 的正值；绿色证据累加 `G-max(B,R)` 的正值。
7. 主导颜色证据至少达到另一颜色的 `1.25` 倍，并通过
   `max(100, colored_pixels*4)` 最低证据门限，才保留对应类别。
8. 颜色证据不明确时同时抑制两个候选，不送 OCR、GA 36、Tracker 或显示层。

这一步解决同一车牌被蓝、绿类别同时检测而产生双框双文字的问题。它不是 OCR 文本规则兜底，
此前的“颜色不明确时优先绿色 8 位或 OCR 高分结果”逻辑已经删除。

### 7.7 PP-OCR 输入接口

生产接口是：

```text
数据类型：UINT8
颜色顺序：BGR
内存布局：NHWC，交错 BGR
Shape：[1,48,160,3]
字节数：48 * 160 * 3 = 23,040
右侧填充：128
```

`inference_ppocr_rec_model_prepared_bgr()` 校验字节数后复制到 PP-OCR 输入缓冲，
`rknn_inputs_set()` 显式声明 `RKNN_TENSOR_UINT8 + RKNN_TENSOR_NHWC`。PP-OCR RKNN
内部归一化为 `(x-127.5)/127.5`。

需要特别纠正两种容易混淆的写法：

- 性能 demo 日志中的 `BGR shape=[1,3,48,160]` 是概念性通道描述，生产运行时实际布局是
  `NHWC [1,48,160,3]`。
- `[1,20,74]` 是 PP-OCR CTC logits 输出，其中 20 为序列长度、74 为 73 字符加
  CTC blank；它不是 OCR 输入 shape。

## 8. 显示框与文字背景板

OBB 四角点当前只用于 rotated IoU、颜色分析和车牌旋转矫正。进入统一
`PipelineResult` 前，代码对四角点分别取：

```text
left   = floor(min(x1,x2,x3,x4))
top    = floor(min(y1,y2,y3,y4))
right  = ceil(max(x1,x2,x3,x4))
bottom = ceil(max(y1,y2,y3,y4))
```

并裁剪到原图边界。因此当前显示层画的是包住整个倾斜车牌的轴对齐外接矩形，不是四点
多边形。现有 Tracker、Qt 结果表、矩形框和文字背景板接口保持不变，文字背景仍按矩形框
上方或附近布局，不需要随 OBB 角度旋转。

这也是当前性能和接口风险较低的实现：四角点计算本身已经发生，若仅增加四条多边形边的
绘制，算术开销通常很小；真正的改动成本在于把四角点穿过 `PipelineResult`、Tracker、
RGA/Qt overlay 和文字布局接口。当前没有这部分接口扩展，报告不把“内部支持四角点”写成
“显示层已经支持多边形框”。

## 9. RK3568 转换后性能

### 9.1 测试环境

| 项目 | 配置 |
|---|---|
| SoC | RK3568 |
| RKNN Runtime/API | 2.3.0 |
| RKNN driver | 0.9.8 |
| 模型 | `yolov8_obb_i8.rknn` |
| 模型精度 | W8A8 INT8 |
| 输入图片 | 真实 720x1160 JPEG |
| Letterbox | 397x640，`scale=0.5517241`，`pad=(122,0)` |
| 预热/正式次数 | 5 / 20 |
| 性能日志阈值 | `conf=0.25, nms=0.40` |
| 检测结果 | 1 个蓝牌，0.947646，-10.037 度 |

性能图片与生产 PCIe 1280x720 BGR565 帧的分辨率、解码来源不同，因此 NPU、输出解包和
CPU OBB 后处理数据可直接作为模型基线；JPEG 解码和 letterbox 时间只能作为该测试样本
基线，不能直接当成生产每帧固定值。

### 9.2 正式 Level 0 实测

| 阶段 | Avg ms | Min ms | P50 ms | P95 ms | Max ms |
|---|---:|---:|---:|---:|---:|
| `rknn_run_wall` | 31.7786 | 31.4695 | 31.7559 | 32.0522 | 32.0715 |
| `rknn_official` | 31.7694 | 31.4600 | 31.7470 | 32.0420 | 32.0610 |
| native output 解包 | 2.6057 | 2.3033 | 2.4019 | 3.1600 | 3.3312 |
| OBB 解码 + rotated NMS | 2.9213 | 2.6428 | 2.7511 | 3.2422 | 3.2991 |
| 一个 OCR 输入矫正 | 1.3027 | 1.1480 | 1.2653 | 1.5730 | 1.5867 |
| 完整 pre-OCR | 38.6083 | 37.6964 | 38.0172 | 39.9204 | 39.9315 |

一次性或测试文件相关耗时：

| 项目 | 耗时 |
|---|---:|
| 模型初始化 + 零拷贝绑定 | 30.3947 ms |
| TurboJPEG 文件解码 | 17.0092 ms |
| 官方 RGA letterbox 到 zero-copy input | 4.0423 ms |

### 9.3 理论吞吐换算

| 口径 | 计算 | 理论 FPS |
|---|---|---:|
| 纯 NPU | `1000 / 31.7786` | 31.468 |
| NPU + 解包 + OBB 后处理 + 1 个 OCR 输入 | `1000 / 38.6083` | 25.901 |
| 上述 pre-OCR + 串行 letterbox | `1000 / 42.6506` | 23.45 |
| JPEG 解码 + letterbox + pre-OCR | `1000 / 59.6598` | 16.76 |

生产 PCIe 帧已经是内存中的 BGR565，不需要 JPEG 文件解码，所以 `16.76 FPS` 不是生产
图片模式上限。生产链路还可通过双缓冲让 RGA、NPU和上一帧 CPU 后处理重叠；在理想流水
条件下，吞吐上限主要受约 `31.78 ms` 的 NPU 阶段限制，而单帧串行延迟仍接近各阶段之和。

当前应用固定每两帧提交一次推理任务。若 PCIe 实际输入约 28 FPS，则调度层最多约
14 次推理/秒，即使 OBB pre-OCR 理论能力为 25.901 FPS；这属于主动调度限制，不是模型
算力不足。完整图片识别还要增加 PP-OCR、可能的一次异常扩框重试、Tracker 和队列耗时，
因此本报告不虚构“OBB+OCR 完整端到端理论 FPS”。

### 9.4 Level 4 仅用于诊断

`RKNN_LOG_LEVEL=4` 下，NPU 平均耗时增至 `49.1599 ms`，完整 pre-OCR 增至
`56.4405 ms`，理论 pre-OCR 吞吐降至 `17.718 FPS`。逐层 profiling 和大量日志会
改变运行时间，所以该结果不能作为发布性能。

Level 4 统计显示 `ConvExSwish` 占 `69.86%`、`Concat` 占 `13.01%`、`Split`
占 `4.42%`；单帧内部读写约 60,723.52 KB、权重读写约 3,363.52 KB、总读写约
64,087.03 KB。

### 9.5 零拷贝改造收益

与改造前标准 RKNN API level 0 基线相比：

| 项目 | 改造前 | 零拷贝后 | 变化 |
|---|---:|---:|---:|
| NPU wall time | 33.1197 ms | 31.7786 ms | -4.05% |
| JPEG 解码 | 47.7037 ms | 17.0092 ms | -64.34% |
| letterbox | 35.7536 ms | 4.0423 ms | -88.69% |
| 单图 pre-OCR 估算 | 119.3311 ms | 59.6598 ms | -50.00% |

新 `25.901 FPS` 已包含输出解包、OBB 解码、旋转 NMS 和 OCR 输入生成；旧
`27.875 FPS` 只包含标准 RKNN API 原始推理。两者处理范围不同，不能仅比较 FPS 数字
判断新链路变慢。

## 10. 当前生产参数与模式边界

| 参数/行为 | 图片模式 | 视频模式 |
|---|---|---|
| 检测模型 | `yolov8_obb.rknn` | `yolov8.rknn`，当前微调模型 |
| 检测框 | OBB，内部四角点；显示外接矩形 | 轴对齐矩形 |
| 置信度 | 0.55 | 保持视频原参数 |
| NMS | 同类别 rotated IoU 0.55 | 保持轴对齐 YOLO 后处理 |
| PP-OCR | 图片 FP16 模型 | 视频 H2 混合量化模型 |
| Tracker | 静态低增益、2 px 死区、同帧 IoU 0.65 去重 | 速度/加速度预测 Tracker |
| 推理调度 | 每两帧一次 | 每两帧一次 |

图片模式的蓝绿冲突门限为：

```text
外接矩形 IoU >= 0.65
检测置信度差 <= 0.10
颜色主证据比 >= 1.25
```

显示启动时应看到：

```text
Image detector: YOLOv8 OBB, conf=0.55, rotated_nms=0.55
```

若新部署包仍输出低于 0.55 的 OBB 候选，例如历史日志中的 `0.276`，说明板端运行的不是
当前二进制或模型资源组合，应先核对安装目录和启动日志，而不是继续修改后处理规则。

## 11. 已完成验证与剩余验证

已完成：

- epoch 54 冻结 test 全量评估，`corrupt=0`。
- PT raw-head 与 ONNX Runtime 四输出一致性验证。
- ONNX 固定 shape/opset/算子白名单检查。
- 800 张校准集唯一性、类别、来源、尺寸、角度和硬链接审计。
- RKNN Toolkit2 2.3.0 正式 INT8 构建。
- RK3568 level 0/4 零拷贝性能实测。
- 两种日志等级下四路输出 checksum 完全一致。
- 两种日志等级生成的 23,040 字节 BGR OCR 输入及 PNG 分别同哈希。
- OBB native 输出解包、同类别旋转 NMS、跨颜色保留、BGR565 到 OCR 输入、显示外接矩形和
  蓝绿颜色证据单元测试。
- 项目 3 当前完整 6 项 CTest 回归通过。

仍需在最终部署包上验证：

- Ubuntu/aarch64 全量交叉编译项目 5，并在 RK3568 上运行当前安装包。
- 使用真实 PCIe 1280x720 图片帧重新测量生产 `conf=0.55 / nms=0.55` 下的 OBB、
  PP-OCR 和完整图片模式端到端耗时。
- 记录不同车牌数量下的 OCR 输入矫正耗时；当前 `1.3027 ms` 对应一个车牌，候选数增加时
  该部分近似按实际矫正数量增长。
- 验证蓝绿冲突明确时只保留一个框，颜色不明确时无框无文字。
- 验证静止车牌的外接矩形、文字背景板和静态 Tracker 不再出现双框、双文字或明显抖动。
- 扩充 `yellow_single` 和 `other` 独立测试样本，避免少样本指标被误解为稳定泛化能力。

## 12. 图示使用建议

当前附图可以作为总体链路概览，但正式文档中建议统一为以下表述：

- “RGA 缩放 640x640”写为“RGB888 等比例 Letterbox 至 640x640，Padding=114”。
- 模型输出先写“四路 raw-head INT8 tensor”，再写“解码为
  `cx/cy/w/h/theta/class/score` 并计算原图四角点”，避免误解为 RKNN 直接输出四角点。
- “建立 48x160 水平目标图”写为“按高度 48 等比例生成有效宽度，再右填充到 160”，
  避免误解为把车牌强制拉伸到 48x160。
- OCR 输入写为 `UINT8 BGR NHWC [1,48,160,3]`。
- PP-OCR 输出写为 `[1,20,74] CTC logits`。
- 在旋转矫正与 PP-OCR 之间补充“蓝绿高重叠候选颜色证据裁决”。

按以上修正后，图示与当前生产代码、RKNN tensor 接口及 PP-OCR 输入要求一致。
