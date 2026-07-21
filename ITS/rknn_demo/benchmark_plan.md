# RK3568 CLI Benchmark Plan

## 1. Current Strategy

Do not merge into UI first. Do not enable DRM display, LPRNet, video stream or PCIe real-time frames in the first RK3568 test.

First verify this path:

```text
image/folder -> COCO RKNN YOLO -> traffic class filter -> ROI rules -> terminal output
```

Goals:

- Check whether the COCO model runs stably on RK3568 NPU.
- Compare YOLOv8n and YOLO11n if both are available.
- Measure inference FPS.
- Measure ROI/rule post-process time.
- Check whether traffic detection quality is good enough for the demo.

## 2. Metrics

Performance:

- Average inference time in ms.
- Average inference FPS.
- ROI/rule post-process time.
- Inference plus rules FPS.

Effect:

- Whether `person` can be detected.
- Whether `car/bus/truck/motorcycle` can be detected.
- Whether `traffic light` can be detected.
- Whether false positives are acceptable.
- Whether ROI warnings match the image.

Do not force mAP in the first round. mAP needs an annotated dataset. Use manual review on self-collected images first.

## 3. Test Images

Prepare 20-50 images:

- Daytime road scenes.
- Pedestrians on or near crosswalks.
- Vehicles on roads.
- Motorcycles or e-bikes.
- Traffic lights.
- Frames captured from the FPGA/HDMI path.

Suggested board path:

```text
./test_images/
```

## 4. Run Format

After integration into the board project, build:

```text
yolov8_traffic_benchmark
```

Run:

```bash
./yolov8_traffic_benchmark ./model/yolov8n_coco.rknn ./test_images 20 3
```

Arguments:

- `./model/yolov8n_coco.rknn`: COCO RKNN model.
- `./test_images`: image file or image folder.
- `20`: repeat count for timing.
- `3`: warmup count.

## 5. Acceptance

First target:

- COCO RKNN loads successfully.
- Repeated inference does not crash.
- Average FPS is good enough for demo.
- Traffic classes are detected in typical images.
- ROI/rule time is much smaller than model inference time.

