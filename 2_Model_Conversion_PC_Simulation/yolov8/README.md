# yolov8

## Table of contents

- [1. Description](#1-description)
- [2. Current Support Platform](#2-current-support-platform)
- [3. Pretrained Model](#3-pretrained-model)
- [4. Convert to RKNN](#4-convert-to-rknn)
- [5. Python Demo](#5-python-demo)
- [6. Android Demo](#6-android-demo)
  - [6.1 Compile and Build](#61-compile-and-build)
  - [6.2 Push demo files to device](#62-push-demo-files-to-device)
  - [6.3 Run demo](#63-run-demo)
- [7. Linux Demo](#7-linux-demo)
  - [7.1 Compile and Build](#71-compile-and-build)
  - [7.2 Push demo files to device](#72-push-demo-files-to-device)
  - [7.3 Run demo](#73-run-demo)
- [8. Expected Results](#8-expected-results)



## 1. Description

The model used in this example comes from the following open source projects:  

https://github.com/airockchip/ultralytics_yolov8



## 2. Current Support Platform

RK3562, RK3566, RK3568, RK3576, RK3588, RV1126B, RV1109, RV1126, RK1808, RK3399PRO


## 3. Pretrained Model

Download link: 

[./yolov8n.onnx](https://ftrg.zbox.filez.com/v2/delivery/data/95f00b0fc900458ba134f8b180b3f7a1/examples/yolov8/yolov8n.onnx)<br />[./yolov8s.onnx](https://ftrg.zbox.filez.com/v2/delivery/data/95f00b0fc900458ba134f8b180b3f7a1/examples/yolov8/yolov8s.onnx)<br />[./yolov8m.onnx](https://ftrg.zbox.filez.com/v2/delivery/data/95f00b0fc900458ba134f8b180b3f7a1/examples/yolov8/yolov8m.onnx)

Download with shell command:

```
cd model
./download_model.sh
```

**Note**: The model provided here is an optimized model, which is different from the official original model. Take yolov8n.onnx as an example to show the difference between them.
1. The comparison of their output information is as follows. The left is the official original model, and the right is the optimized model. As shown in the figure, the original one output is divided into three groups. For example, in the set of outputs ([1,64,80,80],[1,80,80,80],[1,1,80,80]), [1,64,80,80] is the coordinate of the box, [1,80,80,80] is the confidence of the box corresponding to the 80 categories, and [1,1,80,80] is the sum of the confidence of the 80 categories.

<div align=center>
  <img src="./model_comparison/yolov8_output_comparison.jpg" alt="Image">
</div>

2. Taking the the set of outputs ([1,64,80,80],[1,80,80,80],[1,1,80,80]) as an example, we remove the subgraphs behind the two convolution nodes in the model, keep the outputs of these two convolutions ([1,64,80,80],[1,80,80,80]), and add a reducesum+clip branch for calculating the sum of the confidence of the 80 categories ([1,1,80,80]).

<div align=center>
  <img src="./model_comparison/yolov8_graph_comparison.jpg" alt="Image">
</div>


## 4. Convert to RKNN

*Usage:*

```shell
cd python
python convert.py <onnx_model> <TARGET_PLATFORM> <dtype(optional)> <output_rknn_path(optional)>

# such as: 
python convert.py ../model/yolov8n.onnx rk3588
# output model will be saved as ../model/yolov8.rknn
```

*Description:*

- `<onnx_model>`: Specify ONNX model path.
- `<TARGET_PLATFORM>`: Specify NPU platform name. Such as 'rk3588'.
- `<dtype>(optional)`: Specify as `i8`, `u8` or `fp`. `i8`/`u8` for doing quantization, `fp` for no quantization. Default is `i8`.
- `<output_rknn_path>(optional)`: Specify save path for the RKNN model, default save in the same directory as ONNX model with name `yolov8.rknn`

### 4.1 RK3568 traffic INT8 quantization dataset

Windows 下从 `ITS/videoready` 的视频均匀抽帧：

```powershell
cd D:\Yolo_LPR_RK3568_FPGA_Project
powershell -ExecutionPolicy Bypass -File `
  .\2_Model_Conversion_PC_Simulation\yolov8\python\prepare_traffic_quant_dataset.ps1
```

默认处理 8 个视频，每个视频均匀抽取 25 帧，并按 YOLO letterbox 方式缩放/填充为
`640x640`。输出：

```text
model/traffic_quant_dataset/*.jpg
model/traffic_dataset.txt
```

重复执行脚本会重建专用的 `traffic_quant_dataset` 目录，不会覆盖原有的
`rknn_quant_dataset` 和 `dataset.txt`。可调整参数：

```powershell
.\python\prepare_traffic_quant_dataset.ps1 -FramesPerVideo 25 -ImageSize 640
```

转换八类交通 ONNX 前，需要把 `python/convert.py` 第 4 行改为：

```python
DATASET_PATH = '../model/traffic_dataset.txt'
```

若希望不传输出路径时也使用交通模型名称，可把第 5 行同步改为：

```python
DEFAULT_RKNN_PATH = '../model/yolov8_traffic_i8.rknn'
```

`DEFAULT_QUANT = True` 和 `mean_values/std_values` 不需要修改。Ubuntu x86_64 的
RKNN Toolkit2 环境中执行：

```bash
cd /path/to/Yolo_LPR_RK3568_FPGA_Project/2_Model_Conversion_PC_Simulation/yolov8/python
python3 convert.py ../model/yolov8_traffic.onnx rk3568 i8 ../model/yolov8_traffic_i8.rknn
```

输出文件为：

```text
model/yolov8_traffic_i8.rknn
```

2026-07-22 已完成该 INT8 RKNN 导出，并将同一模型复制到
`../../4_NPU_Yolov8_Traffic_Demo/model/yolov8_traffic_i8.rknn`，供 RK3568 最小图片检测与
性能测试使用。两份文件的 SHA256 均为
`FCC7F014246352EC9E4A69EA577F454251F73CFC8393E4C2B856E5BFB58F7A98`。

若先验证非量化模型，把命令中的 `i8` 改成 `fp`，并将输出名改成
`yolov8_traffic_fp.rknn`。FP 模式不会使用量化数据集。

### 4.2 Finetune 车牌模型 INT8 量化数据集

`python/build_finetune_quant_dataset.py` 从清洗后的
`yolo_finetune_special/dataset_manifest.tsv` 构建车牌专用校准集。固定种子为
`20260726`，blue、green、yellow_single 分别从 train 唯一源图抽取 600 张；
所有包含 `other` 的旧图和新增特殊车牌源图跨 split 全部加入，并按源路径去除
新增 train 的三份重复样本。

当前输出共 2,561 张：

| 选择桶 | 图片 |
|---|---:|
| blue | 600 |
| green | 600 |
| yellow_single | 600 |
| other_all | 761 |

`other_all` 包含旧图 433 张和新增特殊车牌 328 张。761 表示唯一源路径数；旧
train 内存在 2 组完全相同内容的 `other` 图片，因为要求保留全部旧 `other`，
这两组没有删除。

2,561 张应作为可复现的校准候选池，不应直接全部交给 RKNN Toolkit2。官方
2.3.0 文档建议一般校准集使用 20–200 张，其中 KL-Divergence 通常使用
20–100 张；继续增加图片会增加耗时和内存，并不保证提高精度。2026-07-26 首次
使用全量 2,561 张执行 KL INT8 时，模型图优化完成，但进程在
`Quantizating 0/160` 被 Linux 直接杀死，没有 Python 异常，按系统行为判定为
内存不足。正式量化应从该候选池建立分层的 100–200 条子清单。

旧 4,215 张数据能够完成量化不能直接证明当前 KL 配置也应成功。Git 历史显示，
旧 `yolov8.rknn` 生成时的 `convert.py` 只配置 `mean/std/target_platform`，
实际使用 Toolkit 默认 `normal + channel`，且没有启用 `model_pruning`；直到
2026-07-23 才改成 `kl_divergence + model_pruning=True`。新旧 ONNX 图结构相同，
本次内存差异主要来自量化算法和剪枝配置，而不是微调模型变大。建议先使用
`normal + channel + model_pruning=False` 和全量候选池复现旧量化口径；若精度
不足，再使用 100–200 张分层子清单单独比较 KL，避免同时改变算法、剪枝和数据量。

2026-07-26 通道审计发现首版生成脚本在 `cv2.imwrite()` 前执行 BGR 到 RGB
转换，导致磁盘 JPEG 红蓝互换。当前版本已去掉该转换，并使用完全相同的 2,561
条选择名单原位重建。全量图片均可解码且为 `640x640x3`；输出与正常 BGR
letterbox 的平均绝对像素误差为 `0.90524`，与红蓝交换输入的误差为 `6.32248`，
通道方向验证通过。当前 `finetune_quant_summary.json` 状态为 `VALID`，可用于
RKNN Toolkit2 默认 `quant_img_RGB2BGR=False` 的正式量化。

首次生成：

```powershell
cd D:\Yolo_LPR_RK3568_FPGA_Project
C:\Users\ZR\.conda\envs\YOLOv8n_LPRNet\python.exe `
  .\2_Model_Conversion_PC_Simulation\yolov8\python\build_finetune_quant_dataset.py
```

脚本拒绝覆盖已有输出。生成文件：

```text
model/finetune_quant_dataset/*.jpg
model/finetune_quant_dataset.txt
model/finetune_quant_manifest.tsv
model/finetune_quant_summary.json
```

量化 `finetune.onnx` 前，应先建立 100–200 张的分层子清单，再将
`python/convert.py` 中的校准集和默认输出改为对应子清单。例如生成
`finetune_quant_dataset_200.txt` 后使用：

```python
DATASET_PATH = '../model/finetune_quant_dataset_200.txt'
DEFAULT_RKNN_PATH = '../model/finetune_i8.rknn'
```

不要修改 `DEFAULT_QUANT = True`、`mean_values/std_values`、
`quantized_algorithm='kl_divergence'` 或 `model_pruning=True`。建议始终显式传入
输出路径，避免覆盖旧模型：

```bash
cd /path/to/Yolo_LPR_RK3568_FPGA_Project/2_Model_Conversion_PC_Simulation/yolov8/python
python3 convert.py ../model/finetune.onnx rk3568 i8 ../model/finetune_i8.rknn
```

若先建立非量化转换基线，使用：

```bash
python3 convert.py ../model/finetune.onnx rk3568 fp ../model/finetune_fp.rknn
```



## 5. Python Demo

*Usage:*

```shell
cd python
# Inference with PyTorch model or ONNX model
python yolov8.py --model_path <pt_model/onnx_model> --img_show

# Inference with RKNN model
python yolov8.py --model_path <rknn_model> --target <TARGET_PLATFORM> --img_show
```

*Description:*

- `<TARGET_PLATFORM>`: Specify NPU platform name. Such as 'rk3588'.

- `<pt_model / onnx_model / rknn_model>`: Specify the model path.



## 6. Android Demo

**Note: RK1808, RV1109, RV1126 does not support Android.**

#### 6.1 Compile and Build

Please refer to the [Compilation_Environment_Setup_Guide](../../docs/Compilation_Environment_Setup_Guide.md#android-platform) document to setup a cross-compilation environment and complete the compilation of C/C++ Demo.  
**Note: Please replace the model name with `yolov8`.**

#### 6.2 Push demo files to device

With device connected via USB port, push demo files to devices:

```shell
adb root
adb remount
adb push install/<TARGET_PLATFORM>_android_<ARCH>/rknn_yolov8_demo/ /data/
```

#### 6.3 Run demo

```sh
adb shell
cd /data/rknn_yolov8_demo

export LD_LIBRARY_PATH=./lib
./rknn_yolov8_demo model/yolov8.rknn model/bus.jpg
```

- After running, the result was saved as `out.png`. To check the result on host PC, pull back result referring to the following command: 

  ```sh
  adb pull /data/rknn_yolov8_demo/out.png
  ```

- Output result refer [Expected Results](#8-expected-results).



## 7. Linux Demo

#### 7.1 Compile and Build

Please refer to the [Compilation_Environment_Setup_Guide](../../docs/Compilation_Environment_Setup_Guide.md#linux-platform) document to setup a cross-compilation environment and complete the compilation of C/C++ Demo.
**Note: Please replace the model name with `yolov8`.**

#### 7.2 Push demo files to device

- If device connected via USB port, push demo files to devices:

```shell
adb push install/<TARGET_PLATFORM>_linux_<ARCH>/rknn_yolov8_demo/ /userdata/
```

- For other boards, use `scp` or other approaches to push all files under `install/<TARGET_PLATFORM>_linux_<ARCH>/rknn_yolov8_demo/` to `userdata`.

#### 7.3 Run demo

```sh
adb shell
cd /userdata/rknn_yolov8_demo

export LD_LIBRARY_PATH=./lib
./rknn_yolov8_demo model/yolov8.rknn model/bus.jpg
```

- After running, the result was saved as `out.png`. To check the result on host PC, pull back result referring to the following command: 

  ```
  adb pull /userdata/rknn_yolov8_demo/out.png
  ```

- Output result refer [Expected Results](#8-expected-results).



## 8. Expected Results

This example will print the labels and corresponding scores of the test image detect results, as follows:

```
person @ (211 241 283 507) 0.873
person @ (109 235 225 536) 0.866
person @ (476 222 560 521) 0.863
bus @ (99 136 550 456) 0.859
person @ (80 326 116 513) 0.311
```

<img src="result.png">

- Note: Different platforms, different versions of tools and drivers may have slightly different results.
