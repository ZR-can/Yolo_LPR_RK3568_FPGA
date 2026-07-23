# RK3568 PCIe LPR Qt UI 设计说明

## 当前结构

Qt UI 的有效源码已经放到主项目 `src` 目录，避免交叉编译时同时维护两套入口：

- `../src/main_pcie_qt.cc`：Qt 主入口、窗口、工作线程、统计显示。
- `../src/mainwindow.ui`：Qt Designer 界面文件，由 CMake `AUTOUIC` 生成 `ui_mainwindow.h`。
- `../src/pcie_frame_source.cc`：复用现有 PCIe BGR565 取帧逻辑。
- `../src/yolo_lpr_pipeline.cc`：复用现有 YOLOv8 + LPRNet 推理与画框代码。
- `../src/simple_tracker.cc`：复用现有车牌结果跟踪逻辑。
- `../model/simhei.ttf`：Qt 界面中文字体，安装到板端 `model/` 目录。

`qt_pcie_lpr_ui` 目录只作为构建入口保留：

- `CMakeLists.txt`
- `build-qt-linux.sh`
- `install/`

它不再使用旧的 `qt_pcie_lpr_ui/src/main_qt_pcie.cc` 或 `qt_pcie_lpr_ui/forms/mainwindow.ui`。

## UI 目标

Qt 只负责交互界面，不重新实现检测框绘制：

1. `PcieFrameSource` 从 PCIe 驱动读取 FPGA 送来的 BGR565 图像。
2. `YOLOLPRPipeline` 跑 YOLOv8 和 LPRNet。
3. `SimplePlateTracker` 平滑车牌框和车牌文本。
4. `draw_pipeline_result_overlay()` 在原始图像上画框和文字。
5. Qt 把已经画好的图像显示到 `videoLabel`，并在右侧显示板子状态、PCIe 状态和统计信息。

## 中文显示说明

`ppocr_keys_v1.txt` 是 OCR 解码字符表，不是界面字体。参考工程真正用于显示中文的是 `simhei.ttf`。

当前 Qt 程序启动时会优先加载：

```text
程序目录/simhei.ttf
程序目录/model/simhei.ttf
```

本项目已经把参考工程的 `simhei.ttf` 复制到：

```text
model/simhei.ttf
```

CMake 安装时会把它放到板端：

```text
/userdata/yolov8_lpr_pcie_qt_ui/yolov8_lpr_pcie_qt_ui/model/simhei.ttf
```

因此右侧中文界面、状态栏中文、统计信息中文和车牌中文显示都优先使用该字体。

## Stop 按钮语义

界面里的 `Stop` 不是 Linux `kill`，也不是关闭 PCIe。

点击 `Stop` 后：

- Qt 工作线程保持存在。
- RKNN pipeline 保持初始化状态。
- `PcieFrameSource` 保持 open。
- PCIe driver fd、BAR 映射、DMA 配置不主动释放。
- 采集循环进入暂停状态，不继续调用 `ReadFrame()`。

再次点击 `Start` 会恢复采集和显示。

只有关闭 UI 窗口或程序退出时，才会走 `RequestExit()`，随后调用 `source.Close()` 做完整清理。

## 手动结束程序命令

正常调试时优先用 UI 窗口关闭程序。需要在 ADB shell 里手动结束时，用下面的温和退出：

```sh
pidof yolov8_lpr_pcie_qt_ui
kill -TERM $(pidof yolov8_lpr_pcie_qt_ui)
```

等待 2 秒后确认：

```sh
pidof yolov8_lpr_pcie_qt_ui || echo "qt ui exited"
```

如果程序卡死才用强制退出：

```sh
kill -9 $(pidof yolov8_lpr_pcie_qt_ui)
```

不要把这些命令放到 UI 的 `Stop` 按钮里。`Stop` 只做暂停，避免反复关闭 PCIe 后上电识别状态无法恢复。

## 统计显示

右侧 `Statistics` 文本框对应 `main_lpr.cc` 里的 `PrintPerformance()` 思路，显示：

- Captured FPS 和 pool drops
- Displayed、queue drops、display failures、overlay failures
- Inference、queue drops、failures、plate results
- Average inference pipeline
- Average display convert/overlay/present/end-to-end
- Driver retries: zero、EPERM、interrupted/EAGAIN、fatal

Qt 版本目前是单工作线程结构，没有 `main_lpr.cc` 的 display/inference 队列，因此 queue drops 和 pool drops 通常为 0。保留这些字段是为了界面格式和板子状态观察方式与原 printf 版本一致。

## 构建方式

在 Ubuntu 虚拟机中：

```bash
cd /mnt/hgfs/3_NPU_Yolov8_PPOCR_Demo/qt_pcie_lpr_ui
sed -i 's/\r$//' build-qt-linux.sh
chmod +x build-qt-linux.sh

GCC_COMPILER=/usr/bin/aarch64-linux-gnu \
QT_ARM64_PREFIX=/home/gyn/Qt-5.12.9-arm64 \
./build-qt-linux.sh
```

CMake 当前会从主项目 `src` 目录编译 `main_pcie_qt.cc` 和 `mainwindow.ui`。

主项目 `CMakeLists.txt` 也已经加入可选目标：

```bash
-DENABLE_QT_UI=ON
```

这个选项默认是 `OFF`，所以不会影响原来的 `yolov8_lpr_pcie_demo`。开启后会生成：

```text
yolov8_lpr_pcie_qt_ui
```

## 板端运行环境

当前板端验证通过的是 X11/xcb：

```sh
systemctl isolate graphical.target

cd /userdata/yolov8_lpr_pcie_qt_ui/yolov8_lpr_pcie_qt_ui

export DISPLAY=:0
export XAUTHORITY=/var/run/lightdm/root/:0
export QT_QPA_PLATFORM=xcb
export LD_LIBRARY_PATH=/userdata/yolov8_lpr_pcie_qt_ui/lib:$LD_LIBRARY_PATH

./yolov8_lpr_pcie_qt_ui \
  ./model/yolov8.rknn \
  ./model/lprnet7repair_i8.rknn \
  ./model/lprnet8repair_i8.rknn
```
