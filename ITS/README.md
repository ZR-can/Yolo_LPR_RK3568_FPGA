# ITS 智能交通识别交接文档

更新时间：2026-07-21

本文档说明 `else/ITS` 文件夹当前的整体思路、已经完成的内容、未完成内容、关键文件作用和后续演示注意事项。

## 1. 当前方案一句话

当前 ITS demo 不是重新训练的交通违法行为模型，而是使用现成的 YOLOv8n COCO RKNN 模型检测 `person` 和 `traffic light`，再用人工标定的斑马线区域和红绿灯颜色规则判断行人是否疑似闯红灯。

核心链路：

```text
测试视频
-> ffmpeg 抽成 JPG 帧
-> RK3568 板端读取帧文件夹
-> YOLOv8n COCO RKNN 检测 person / traffic light
-> 在 traffic light 检测框内统计红色和绿色像素
-> 人为指定斑马线 ROI
-> 判断 person 框底边是否落入斑马线 ROI
-> 红灯 + 行人在斑马线 ROI 内 = red_violation
-> 输出叠加结果 JPG / CSV / summary.txt
```

## 2. 最重要的限制

**每换一个测试图片或视频，都必须重新标定斑马线位置坐标。**

原因是：当前斑马线不是模型检测出来的，而是人为规定的固定 ROI。只要视频机位、裁剪范围、画面比例、拍摄角度发生变化，原来的斑马线坐标就可能失效。

最终答辩演示时一定要使用已经调好坐标、效果最稳定的图片或视频。目前主观判断 `test5` 是最适合继续验证和演示的视频。

## 3. 文件夹说明

```text
ITS/
  README.md
  rknn_demo/
  video/
  videoready/
  坐标/
  docs/
  pc_demo/
```

`rknn_demo/` 是当前最重要的源码和板端 demo 工程目录。模型推理、PC 测试脚本、交叉编译脚本、板端 C++ 程序都在这里。

`video/` 放本次实际要测试的视频。当前里面是 `test5.mp4` 和 `test8.mp4`。

`videoready/` 放拍摄得到的所有相对可用视频。`test1` 到 `test4` 是街道路口素材，`test5` 到 `test8` 是后来在珞珈门拍的素材。

`坐标/` 是用 LabelImg 标斑马线坐标的工作目录。里面的 txt 是 YOLO bbox 格式：

```text
class cx cy w h
```

当前关键坐标：

```text
test5: 0 0.503539 0.777667 0.992922 0.289133
test8: 0 0.381818 0.684739 0.762121 0.210843
```

当前代码使用的是二者平均后的斑马线 bbox：

```text
cx cy w h = 0.442679 0.731203 0.877522 0.249988
```

## 4. 已完成工作

1. 找到并转换了可用的 YOLOv8n COCO 模型，当前使用：

```text
rknn_demo/model_convert/yolov8n_coco_fp.rknn
```

2. 建立了独立的 `rknn_demo` CMake 工程，不需要并入原来的 Qt/UI 工程。

3. 完成了 RK3568 板端可运行的帧序列推理程序：

```text
yolov8_redlight_violation_frame
```

4. 板端程序已支持：

- 读取 JPG 帧文件夹
- 检测 `person` 和 `traffic light`
- 在红绿灯检测框内做红/绿颜色统计
- 画出斑马线 ROI
- 判断行人框底边是否进入 ROI
- 输出违规状态
- 输出叠加图片、CSV 和 summary

5. 已经加入每次运行后的统计指标：

```text
avg_inference_ms
inference_fps
end_to_end_fps
detections_total
detections_person
detections_traffic_light
avg_confidence_all
avg_confidence_person
avg_confidence_traffic_light
red_frames / green_frames / unknown_light_frames
warning_frames / warning_ratio_percent
```

6. 当前 `test5` 的一次板端结果参考：

```text
frames_with_inference = 78
avg_inference_ms = 166.331
inference_fps = 6.01
end_to_end_fps = 5.20
detections_person = 6
detections_traffic_light = 1
unknown_light_frames = 77
```

这个结果说明模型速度有参考价值，但 `traffic light` 检测不稳定，红绿灯判断还有改进空间。

## 5. 未完成工作

1. 剪枝和量化还没做。

当前使用的是 FP RKNN 模型，尚未做 INT8 量化、剪枝或模型结构优化。

2. 没有训练专门的斑马线/闯红灯检测模型。

斑马线 ROI 仍然靠 LabelImg 人工标定，并写入代码或配置。

3. 红绿灯检测不够稳定。

YOLO COCO 的 `traffic light` 类在当前视频里可能漏检。如果最终演示需要稳定触发红灯规则，需要选择红绿灯清晰、模型能检出的片段，或者加入固定红绿灯 ROI 颜色判断作为兜底。

4. 没有做实时 mp4 解码板端输入。

因为当前交叉 OpenCV 缺少 `videoio`，板端流程是先在 PC/Ubuntu 把 mp4 抽成 JPG，再推到板端运行。

5. 没有接入最终 UI。

当前保持独立 CMake demo，便于先测帧率、准确率和规则效果。

## 6. 推荐演示策略

最终演示不要随便换视频。建议使用 `test5` 或从 `test5` 中截取效果最好的片段。

演示前必须确认：

- 斑马线 ROI 与视频画面匹配。
- 行人框底边能落入 ROI。
- 红绿灯在若干帧中能被 YOLO 检出。
- `summary.txt` 中 `traffic_light` 不应长期为 0 或 1。
- 输出叠加图中框、斑马线、状态文字视觉上说得通。

如果换新视频，流程是：

```text
1. 从视频中截一张代表性帧
2. 用 LabelImg 框出斑马线
3. 得到 YOLO bbox: class cx cy w h
4. 修改 rknn_demo/redlight_violation_frame_main.cc 中的斑马线 polygon
5. 同步修改 rknn_demo/redlight_violation_config.json
6. 重新交叉编译
7. 抽帧、推板子、运行、拉回检查
```

## 7. 常用运行流程

Ubuntu 抽帧：

```bash
cd /mnt/hgfs/ITS/rknn_demo
rm -rf test_frames
mkdir -p test_frames/test5
ffmpeg -y -i /mnt/hgfs/ITS/video/test5.mp4 -vf fps=5 test_frames/test5/frame_%06d.jpg
tar -czf test_frames.tar.gz test_frames
```

Ubuntu 交叉编译：

```bash
cd /mnt/hgfs/ITS/rknn_demo
export YOLO_LPR_DEMO_ROOT=/mnt/hgfs/3_NPU_Yolov8_LPR_Demo-2
export GCC_COMPILER=/home/gyn/gcc-linaro-6.3.1-2017.05-x86_64_aarch64-linux-gnu/bin/aarch64-linux-gnu
sed -i 's/\r$//' build-linux-traffic.sh
chmod +x build-linux-traffic.sh
./build-linux-traffic.sh -t rk3568 -a aarch64
```

Windows 推送：

```powershell
cd D:\adb\bin
.\adb.exe shell "rm -rf /userdata/its_traffic_benchmark"
.\adb.exe shell "mkdir -p /userdata/its_traffic_benchmark"
.\adb.exe push "D:\a_fpga\codex\5_12_HDMI_IN_DDR3_HDMI_OUT\else\ITS\rknn_demo\install\rk356x_linux_aarch64\its_traffic_benchmark\yolov8_traffic_benchmark" "/userdata/its_traffic_benchmark/"
.\adb.exe push "D:\a_fpga\codex\5_12_HDMI_IN_DDR3_HDMI_OUT\else\ITS\rknn_demo\install\rk356x_linux_aarch64\its_traffic_benchmark\lib" "/userdata/its_traffic_benchmark/"
.\adb.exe push "D:\a_fpga\codex\5_12_HDMI_IN_DDR3_HDMI_OUT\else\ITS\rknn_demo\test_frames.tar.gz" "/userdata/its_traffic_benchmark/yolov8_traffic_benchmark/"
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

Windows 拉回：

```powershell
cd D:\adb\bin
mkdir "D:\a_fpga\codex\5_12_HDMI_IN_DDR3_HDMI_OUT\else\ITS\rknn_demo\board_outputs\redlight_final" -Force
.\adb.exe pull "/userdata/its_traffic_benchmark/yolov8_traffic_benchmark/outputs_redlight" "D:\a_fpga\codex\5_12_HDMI_IN_DDR3_HDMI_OUT\else\ITS\rknn_demo\board_outputs\redlight_final"
```

Ubuntu 合成查看视频：

```bash
cd /mnt/hgfs/ITS/rknn_demo/board_outputs/redlight_final/outputs_redlight
ffmpeg -y -framerate 5 -i test5/result_%06d.jpg -c:v libx264 -pix_fmt yuv420p test5_redlight_board.mp4
```

## 8. 给下一位接手者的提醒

这个 demo 的重点是“用现成轻量模型 + 规则逻辑做智能交通识别展示”。不要把它误解成已经训练好了违法动作识别模型。

如果时间有限，优先保证一条演示视频效果稳定，而不是追求泛化到所有路口。斑马线坐标、人行区域、红绿灯可见性都高度依赖固定机位。
