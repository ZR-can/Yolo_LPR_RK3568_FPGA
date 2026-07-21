# RK3568 板端迁移计划

## 1. 当前结论

PC 侧 ITS Demo 已经完成：

- COCO YOLOv8n ONNX 推理。
- 交通类别过滤。
- 类别计数。
- ROI 区域规则告警。
- 带框图片和 CSV 输出。

板端迁移不建议直接改原有车牌识别流水线，也不建议第一步并入 UI。当前
`3_NPU_Yolov8_LPR_Demo` 的 `process_pipeline()` 默认把 YOLO 检测框当作车牌框，再裁剪送入 LPRNet。智能交通检测应该走一条独立分支：

```text
image_buffer_t
  -> inference_yolov8_model()
  -> traffic class filter
  -> ROI rules
  -> draw traffic boxes / warning panel
```

## 2. 迁移风险点

### 2.1 类别数

当前板端后处理：

```c
#define OBJ_CLASS_NUM 4
```

当前 `model/labels_list.txt` 是车牌类别：

```text
blue
green
yellow_single
other
```

如果换成 COCO YOLOv8n / YOLO11n，必须改成 80 类标签，否则后处理会按 4 类读分数，结果一定不对。

建议：

```c
#define OBJ_CLASS_NUM 80
```

并把 `labels_list.txt` 替换为 COCO 80 类。

### 2.2 模型文件

当前板端：

```text
3_NPU_Yolov8_LPR_Demo/model/yolov8.rknn
```

大概率是车牌检测模型，不是 COCO 模型。交通检测版需要放入：

```text
model/yolov8n_coco.rknn
```

或使用 RKNN Model Zoo 的官方 YOLOv8n / YOLO11n COCO RKNN 模型。

### 2.3 流水线入口

不要让 COCO 检测结果继续进入 LPRNet，否则 `person/car/bus` 会被当车牌裁剪识别。

推荐新增：

```c++
int process_traffic_scene(
    rknn_app_context_t* yolo_ctx,
    image_buffer_t* src_image,
    TrafficSceneResult* out_result,
    bool draw_on_image
);
```

保留原有 `process_pipeline()` 给车牌识别使用。

## 3. 推荐新增模块

建议后续在板端工程增加：

```text
include/traffic_scene.h
src/traffic_scene.cc
model/coco_80_labels_list.txt
model/traffic_roi.json
```

第一版已经在 `else/ITS/rknn_demo` 放置草案文件。它暂不解析 JSON，直接在
C++ 里写固定归一化 ROI，减少依赖。

核心结构：

```c++
struct TrafficCounts {
    int person;
    int car;
    int motorcycle;
    int bus;
    int truck;
    int traffic_light;
};

struct TrafficWarning {
    char code[64];
    char message[128];
};
```

## 4. 第一版板端功能

第一版只需要完成：

- 显示 `person/car/motorcycle/bus/truck/traffic light` 检测框。
- 屏幕左上角显示类别计数。
- 输出 `person in road lane risk` 等英文告警。
- 终端打印每帧计数和告警。

中文叠字可以后做，因为当前板端代码里中文编码显示已经有一些历史乱码问题。答辩演示时，英文告警完全可用。

## 5. 最小改动路径

1. 复制一份 COCO RKNN 模型到 `model/`。
2. 替换或新增 COCO 80 类标签。
3. 把 `OBJ_CLASS_NUM` 改为 80。
4. 新增 `traffic_scene.cc/h`，复用 `inference_yolov8_model()` 的结果。
5. 在 main 或 UI 流程里增加 `traffic` 模式，不进入 LPRNet。
6. 验证图片输入。
7. 再接实时视频/PCIe 帧。

最新执行顺序调整：

1. 先做 RK3568 纯命令行 benchmark。
2. 测 YOLOv8n / YOLO11n COCO 的帧率和检测效果。
3. 确认 ROI 后处理耗时和告警稳定性。
4. 再决定是否并入 UI、DRM 显示或 PCIe 实时帧。

## 6. 暂不建议做的事

- 暂不训练闯红灯/逆行模型。
- 暂不在 FPGA 侧实现交通规则。
- 暂不把 LPRNet 和 COCO 检测强行混在一个后处理里。
- 暂不做复杂多目标跟踪，最多做 3-5 帧中心点方向判断。
