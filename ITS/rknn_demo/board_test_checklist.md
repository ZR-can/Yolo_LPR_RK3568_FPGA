# RK3568 Benchmark 测试检查清单

## 1. 上板前确认

- [ ] 已准备 COCO RKNN 模型，例如 `yolov8n_coco.rknn`。
- [ ] `model/labels_list.txt` 已替换为 COCO 80 类。
- [ ] `OBJ_CLASS_NUM` 已从 4 改为 80。
- [ ] 已准备 `./test_images` 测试图片文件夹。
- [ ] 第一轮不启动 UI、不启动 DRM 显示、不进入 LPRNet。

## 2. 运行命令

```bash
./yolov8_traffic_benchmark ./model/yolov8n_coco.rknn ./test_images 20 3
```

## 3. 需要记录的性能数据

- 平均推理耗时：
- 平均推理 FPS：
- ROI/规则耗时：
- 推理加规则 FPS：
- 是否有内存错误或崩溃：

## 4. 需要记录的效果数据

对每张图记录：

- `person` 数量是否合理。
- `car` 数量是否合理。
- `motorcycle` 数量是否合理。
- `bus` 数量是否合理。
- `truck` 数量是否合理。
- `traffic light` 是否能识别。
- ROI 告警是否符合画面。

建议使用：

```text
manual_eval_template.csv
```

## 5. 判定建议

可以继续接 UI / 视频 / PCIe 的条件：

- 模型能稳定加载和重复推理。
- FPS 达到答辩演示可接受水平。
- 典型图片中 `person` 和主要车辆类别识别稳定。
- ROI 规则耗时远小于模型推理耗时。

需要回退或换模型的条件：

- COCO 模型无法加载。
- 输出类别明显错乱。
- `OBJ_CLASS_NUM=80` 和标签替换后仍然识别异常。
- FPS 明显低于演示要求。

