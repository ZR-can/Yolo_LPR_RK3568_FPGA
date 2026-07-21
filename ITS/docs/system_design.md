# 系统设计说明

## 1. 总体架构

```text
视频源 / HDMI 输入
        |
        v
FPGA
  - 视频采集
  - RGB888 数据整理
  - PCIe / DDR / FIFO 链路
        |
        v
RK3568
  - 图像帧获取
  - NPU 推理
  - CPU 后处理
        |
        v
显示与告警
  - 检测框
  - 类别计数
  - ROI 风险提示
```

## 2. 软件模块

PC 侧验证模块：

```text
detect_traffic.py
  - ONNX 推理
  - YOLO 后处理
  - 交通类别过滤
  - 结果绘制

roi_rules.py
  - ROI 读取
  - 点在多边形内判断
  - 规则告警
  - 统计面板绘制

make_roi_config.py
  - 像素坐标转归一化 ROI

roi_preview.py
  - ROI 可视化预览
```

板端计划模块：

```text
traffic_scene.cc/h
  - 复用 inference_yolov8_model()
  - 过滤 COCO 交通类别
  - 统计目标数量
  - 执行 ROI 风险规则
  - 绘制检测框和告警文字
```

## 3. 数据结构

检测结果：

```text
class_id
class_name
score
box = x1, y1, x2, y2
anchor = bottom-center or center
```

区域配置：

```text
name
label
points_norm
```

风险输出：

```text
level
code
message
```

## 4. 当前演示闭环

已经完成：

```text
图片输入
  -> YOLOv8n COCO ONNX
  -> person/bus/car/truck/motorcycle/traffic light 筛选
  -> 目标计数
  -> ROI 风险告警
  -> 输出带框图片和 detections.csv
```

待完成：

```text
COCO RKNN 模型
  -> RK3568 NPU 推理
  -> 板端 ROI 告警
  -> FPGA 视频帧接入
```

