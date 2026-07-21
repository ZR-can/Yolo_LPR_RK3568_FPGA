# rknn_demo 源码交接说明

更新时间：2026-07-21

`rknn_demo` 是 ITS 项目当前真正需要接手的源码目录。它独立于原 Qt/UI 工程，用于先验证 RK3568 板端 YOLOv8n COCO RKNN 模型的检测速度、检测效果和红灯行人规则逻辑。

## 1. 当前主程序

最重要的板端程序：

```text
redlight_violation_frame_main.cc
```

编译后可执行文件：

```text
yolov8_redlight_violation_frame
```

作用：

```text
JPG 帧文件夹
-> RKNN YOLOv8n COCO 推理
-> 只保留 person / traffic light
-> 在 traffic light 框内统计红色和绿色像素
-> 用固定斑马线 ROI 判断 person 框底边是否进入斑马线
-> 红灯且行人在斑马线 ROI 内则标记 red_violation
-> 输出 result_*.jpg、redlight_violation.csv、summary.txt
```

## 2. 当前斑马线坐标

当前代码使用 test5 和 test8 两个 LabelImg 框的平均值。

原始 LabelImg 坐标：

```text
test5: 0 0.503539 0.777667 0.992922 0.289133
test8: 0 0.381818 0.684739 0.762121 0.210843
```

平均 bbox：

```text
cx cy w h = 0.442679 0.731203 0.877522 0.249988
```

板端 C++ 里实际写成矩形 polygon：

```cpp
static const float kCrosswalkPolyNorm[][2] = {
    {0.003918f, 0.606209f},
    {0.881440f, 0.606209f},
    {0.881440f, 0.856197f},
    {0.003918f, 0.856197f},
};
```

PC 配置中对应：

```json
"crosswalk_polygon_norm": [],
"crosswalk_bbox_norm": [0.442679, 0.731203, 0.877522, 0.249988]
```

**重要：换视频或换图片必须重新标定这里。**

## 3. 核心文件作用

`CMakeLists.txt`

定义独立 CMake 工程和三个板端目标：

```text
yolov8_traffic_benchmark
yolov8_person_light_video
yolov8_redlight_violation_frame
```

`build-linux-traffic.sh`

交叉编译脚本。依赖外部原始 YOLO/LPR demo 工程：

```text
YOLO_LPR_DEMO_ROOT=/mnt/hgfs/3_NPU_Yolov8_LPR_Demo-2
```

`redlight_violation_frame_main.cc`

当前最终演示最重要的板端 C++ 源码。只读 JPG 帧文件夹，不直接读 mp4。

`person_light_video_main.cc`

板端 person / traffic light 检测验证程序。主要用于先看 YOLO 能不能检测人和红绿灯，不做闯红灯规则。

`traffic_benchmark_main.cc`

早期图片 benchmark 程序。用于更基础的交通类别检测和速度测试。

`pc_redlight_violation.py`

PC/Ubuntu 上用 RKNN Toolkit2 模拟器跑红灯规则的视频脚本。适合先看逻辑和可视化结果，但 PC 模拟器帧率不代表 RK3568 NPU。

`pc_rknn_person_light.py`

PC/Ubuntu 上测试 person / traffic light 检测的脚本。

`run_pc_redlight_violation.sh`

PC 红灯规则脚本的 shell 包装。默认使用：

```text
redlight_violation_config.json
```

`redlight_violation_config.json`

PC 脚本使用的红灯规则配置，包含斑马线 bbox、颜色阈值等。

`model_convert/yolov8n_coco_fp.rknn`

当前板端使用的 FP RKNN 模型。

`model_convert/yolov8n_coco.onnx`

用于 PC RKNN Toolkit2 build/simulator 的 ONNX 模型。

`yolov8_fp_fallback.cc`

FP RKNN 输出兼容逻辑。当前 FP 模型需要保留。

`postprocess.h`

YOLO 后处理声明。

`coco_80_labels_list.txt`

COCO 80 类标签表。当前主要用到 `person` 和 `traffic light`。

## 4. 输出文件说明

板端 `yolov8_redlight_violation_frame` 每次运行会在输出目录生成：

```text
result_000001.jpg ...
redlight_violation.csv
summary.txt
```

`result_*.jpg`

叠加后的结果帧，包括 person 框、traffic light 框、斑马线 ROI 和规则状态文字。

`redlight_violation.csv`

逐帧逐检测框记录，包括红绿灯状态、是否 warning、检测框坐标、置信度、推理耗时等。

`summary.txt`

本次运行摘要，包括：

```text
inference_fps
end_to_end_fps
avg_confidence_person
avg_confidence_traffic_light
red_frames
green_frames
unknown_light_frames
warning_frames
```

`inference_fps` 是模型推理速度，最有参考价值。

`end_to_end_fps` 包含读 JPG、推理、画框、写 JPG，所以比纯推理帧率低。

## 5. 已完成

- 已建立独立 CMake 工程。
- 已可交叉编译到 RK3568 Linux aarch64。
- 已可在板端运行 YOLOv8n COCO FP RKNN。
- 已完成 person / traffic light 过滤。
- 已完成基于 YOLO traffic light 框的红/绿颜色统计。
- 已完成基于人工斑马线 ROI 的行人闯红灯规则判断。
- 已完成叠加图、CSV、summary 输出。
- 已记录并使用 test5/test8 平均斑马线坐标。

## 6. 未完成

- 剪枝未做。
- INT8 量化未做。
- 没有训练专门的违法行为模型。
- 没有训练或接入斑马线检测模型。
- 没有实现板端 mp4 直接解码。
- 没有并入最终 UI。
- 红绿灯检测在部分视频里不稳定，需要选择合适素材或增加固定红绿灯 ROI 兜底。

## 7. 常用命令

抽帧：

```bash
cd /mnt/hgfs/ITS/rknn_demo
rm -rf test_frames
mkdir -p test_frames/test5
ffmpeg -y -i /mnt/hgfs/ITS/video/test5.mp4 -vf fps=5 test_frames/test5/frame_%06d.jpg
tar -czf test_frames.tar.gz test_frames
```

交叉编译：

```bash
cd /mnt/hgfs/ITS/rknn_demo
export YOLO_LPR_DEMO_ROOT=/mnt/hgfs/3_NPU_Yolov8_LPR_Demo-2
export GCC_COMPILER=/home/gyn/gcc-linaro-6.3.1-2017.05-x86_64_aarch64-linux-gnu/bin/aarch64-linux-gnu
sed -i 's/\r$//' build-linux-traffic.sh
chmod +x build-linux-traffic.sh
./build-linux-traffic.sh -t rk3568 -a aarch64
```

板端运行：

```bash
cd /userdata/its_traffic_benchmark/yolov8_traffic_benchmark
chmod +x yolov8_redlight_violation_frame
export LD_LIBRARY_PATH=/userdata/its_traffic_benchmark/lib:$LD_LIBRARY_PATH
rm -rf test_frames outputs_redlight
tar -xzf test_frames.tar.gz
mkdir -p outputs_redlight/test5
./yolov8_redlight_violation_frame ./model/yolov8n_coco_fp.rknn ./test_frames/test5 ./outputs_redlight/test5 0 1
cat ./outputs_redlight/test5/summary.txt
```

## 8. 修改斑马线坐标的方法

1. 从目标视频截一帧。
2. 用 LabelImg 框出斑马线。
3. 记录 YOLO 格式：

```text
class cx cy w h
```

4. 如果改 PC 脚本，修改 `redlight_violation_config.json`：

```json
"crosswalk_polygon_norm": [],
"crosswalk_bbox_norm": [cx, cy, w, h]
```

5. 如果改板端 C++，把 bbox 转成矩形 polygon：

```text
left = cx - w / 2
top = cy - h / 2
right = cx + w / 2
bottom = cy + h / 2
```

然后修改 `redlight_violation_frame_main.cc`：

```cpp
static const float kCrosswalkPolyNorm[][2] = {
    {left, top},
    {right, top},
    {right, bottom},
    {left, bottom},
};
```

6. 重新交叉编译并推送板端。

## 9. 最终演示提醒

优先选择 `test5` 或从 `test5` 截取的稳定片段。不要临时换视频，除非已经重新标定斑马线并确认红绿灯能被检测到。

当前方案适合答辩时描述为：

```text
基于 RK3568 NPU 与 YOLOv8n COCO 的行人和交通灯检测，结合人工标定斑马线 ROI 与交通灯颜色判断，实现固定路口视角下的行人闯红灯疑似违规标记。
```
