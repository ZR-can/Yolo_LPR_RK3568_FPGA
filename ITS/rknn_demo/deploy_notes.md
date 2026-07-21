# RK3568 接入步骤草案

## 1. 复制文件

把本目录下这些文件复制到板端工程：

```text
traffic_scene.h -> 3_NPU_Yolov8_LPR_Demo/include/traffic_scene.h
traffic_scene.cc -> 3_NPU_Yolov8_LPR_Demo/src/traffic_scene.cc
coco_80_labels_list.txt -> 3_NPU_Yolov8_LPR_Demo/model/labels_list.txt
traffic_roi.json -> 3_NPU_Yolov8_LPR_Demo/model/traffic_roi.json
```

注意：第一版 `traffic_scene.cc` 内置了归一化 ROI，并没有解析 `traffic_roi.json`。JSON 先作为 PC/RK 两侧对齐记录，后续需要动态配置时再加解析。

## 2. 修改类别数

文件：

```text
3_NPU_Yolov8_LPR_Demo/include/postprocess.h
```

将：

```c
#define OBJ_CLASS_NUM 4
```

改为：

```c
#define OBJ_CLASS_NUM 80
```

否则 COCO 模型输出的 80 类分数会被错误解释。

## 3. CMake 增加源文件

文件：

```text
3_NPU_Yolov8_LPR_Demo/CMakeLists.txt
```

在 `DEMO_COMMON_SRCS` 中加入：

```cmake
src/traffic_scene.cc
```

## 4. 第一轮先做纯命令行 benchmark

第一轮先不要并入 UI，也不要先接 DRM 显示。

推荐先新增 benchmark target：

```text
yolov8_traffic_benchmark
```

它只做：

```text
read_image -> inference_yolov8_model -> analyze_traffic_scene -> printf
```

草案入口：

```text
traffic_benchmark_main.cc
```

详细计划见：

```text
benchmark_plan.md
```

## 5. 后续再新增 traffic 模式入口

不要复用 `process_pipeline()`，它会把 YOLO 检测框继续送入 LPRNet。

推荐在图片 demo 或 PCIe demo 中增加类似逻辑：

```c++
object_detect_result_list od_results;
TrafficSceneResult traffic_result;

int ret = inference_yolov8_model(&yolo_ctx, &src_image, &od_results);
if (ret == 0) {
    analyze_traffic_scene(&od_results, src_image.width, src_image.height, &traffic_result);
    draw_traffic_scene_overlay(&src_image, &od_results, &traffic_result);
}
```

## 6. 模型文件

当前 `model/yolov8.rknn` 是车牌检测模型。交通版需要替换为 COCO 模型，例如：

```text
model/yolov8n_coco.rknn
```

命令行或代码里要加载新的 COCO RKNN 模型。

## 7. 第一轮验证

先用命令行 benchmark 验证：

- 能加载 COCO RKNN。
- 能识别 person/car/bus/truck/motorcycle/traffic light。
- 终端计数正常。
- ROI 告警正常。
- 平均推理 FPS 和后处理耗时可接受。

确认 benchmark 正常后，再接图片显示、视频、UI 或 PCIe 帧。
