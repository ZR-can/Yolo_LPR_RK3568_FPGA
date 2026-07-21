# PC 侧智能交通识别 Demo

这个目录用于先在电脑上验证“交通目标检测 + ROI 区域规则 + 风险告警”的演示效果。

默认复用当前 AI 工程里已经准备好的 COCO ONNX 模型：

```text
D:\a_fpga\codex\5_12_HDMI_IN_DDR3_HDMI_OUT\else\Yolo_LPR_RK3568_FPGA\2_Model_Conversion_PC_Simulation\yolov8\model\yolov8n_coco.onnx
```

## 运行方式

从仓库根目录运行：

```powershell
C:\Users\gaoya\.codex\venvs\yolov8_coco_pc\Scripts\python.exe `
  .\else\ITS\pc_demo\detect_traffic.py `
  --img .\else\Yolo_LPR_RK3568_FPGA\2_Model_Conversion_PC_Simulation\yolov8\model\bus_coco.jpg
```

输入自己的图片：

```powershell
C:\Users\gaoya\.codex\venvs\yolov8_coco_pc\Scripts\python.exe `
  .\else\ITS\pc_demo\detect_traffic.py `
  --img D:\your_image.jpg
```

输入一个文件夹：

```powershell
C:\Users\gaoya\.codex\venvs\yolov8_coco_pc\Scripts\python.exe `
  .\else\ITS\pc_demo\detect_traffic.py `
  --img D:\your_image_folder
```

输出目录默认为：

```text
else\ITS\pc_demo\outputs
```

输出包括：

- 带检测框、ROI 和告警面板的图片。
- `detections.csv` 检测结果表。

## ROI 配置

默认 ROI 配置：

```text
else\ITS\pc_demo\configs\demo_roi.json
```

第一版内置两个区域：

- `road_lane`：机动车道区域。
- `crosswalk`：人行横道或行人区域。

规则：

- `person` 落入 `road_lane`：行人进入机动车道风险。
- `car/motorcycle/bus/truck` 落入 `crosswalk`：车辆进入行人区域风险。
- `crosswalk` 内同时有行人和车辆：人车混行风险。
- `road_lane` 内车辆数超过阈值：车辆密度过高。

如果只想看检测框，不启用 ROI：

```powershell
C:\Users\gaoya\.codex\venvs\yolov8_coco_pc\Scripts\python.exe `
  .\else\ITS\pc_demo\detect_traffic.py `
  --img D:\your_image.jpg `
  --no-roi
```

## 当前类别

默认只保留这些 COCO 交通相关类别：

- `person`
- `car`
- `motorcycle`
- `bus`
- `truck`
- `traffic light`

如果要显示全部 COCO 类别，增加：

```powershell
--all-classes
```

## 快速制作自己的 ROI

先用任意看图工具读出多边形顶点像素坐标。比如道路区域四个点：

```text
100,420;1180,390;1270,720;0,720
```

生成配置：

```powershell
C:\Users\gaoya\.codex\venvs\yolov8_coco_pc\Scripts\python.exe `
  .\else\ITS\pc_demo\make_roi_config.py `
  --image D:\your_image.jpg `
  --road "100,420;1180,390;1270,720;0,720" `
  --crosswalk "420,330;900,330;1080,500;260,520" `
  --output .\else\ITS\pc_demo\configs\your_roi.json
```

预览 ROI：

```powershell
C:\Users\gaoya\.codex\venvs\yolov8_coco_pc\Scripts\python.exe `
  .\else\ITS\pc_demo\roi_preview.py `
  --image D:\your_image.jpg `
  --roi-config .\else\ITS\pc_demo\configs\your_roi.json `
  --output .\else\ITS\pc_demo\outputs\your_roi_preview.jpg `
  --grid
```

用自己的 ROI 跑检测：

```powershell
C:\Users\gaoya\.codex\venvs\yolov8_coco_pc\Scripts\python.exe `
  .\else\ITS\pc_demo\detect_traffic.py `
  --img D:\your_image.jpg `
  --roi-config .\else\ITS\pc_demo\configs\your_roi.json `
  --save-dir .\else\ITS\pc_demo\outputs\your_test
```
