# 5_QT_UI_Demo 操作手册

## 当前状态

本目录承载 RK3568 + FPGA PCIe 智能交通 Qt 5 界面，提供三种可选模式：

- `视频识别`：当前生产链路，使用 YOLOv8 + PP-OCRv4 识别车牌。
- `图片识别`：读取 FPGA/PCIe 输入的静态画面，使用 YOLOv8-OBB 定位并旋转矫正车牌，再送入
  FP16 PP-OCR；每次换图先隔离上一图片 generation，再复用 Tracker 的连续 2 次确认与合法投票。
- `行人违法检测`：接入项目 4 的 person 检测、固定交通灯 ROI 取色、灯色投票、斑马线 ROI 和
  闯红灯事件去重链路。

Qt 目标直接复用第 3 阶段的 `main_ppocr.cc`、PCIe 采集、YOLO 检测、PP-OCR 识别、车牌规则、
Tracker 和 RGA 叠加代码，同时复用第 4 阶段的交通后处理、时序 Tracker、违法规则和叠加层。
右侧面板提供 FPGA BAR0 预处理控制，支持旁路、亮度、对比度和 ROI 放大预览；控制配置独立于
三种识别模式，模型和规则始终处理完整 `1280×720` PCIe 帧。
视频识别的共享车牌 Tracker 已加入高速首联尺寸门、真实观测速度、受限加速度和最多 8 帧
短检测空窗预测，用于补偿约 2–4 帧异步推理延迟；该变化只作用于项目 3/项目 5 的车牌
跟踪，不改变行人违法检测 Tracker。
当前仍需在 Ubuntu 20.04 交叉编译，并在 RK3568 + FPGA 实链路复测。

## 视频/图片检测模型分流

板端测试已确认微调模型可直接用于视频模式，因此项目 5 只生成一个
`yolov8_ppocr_pcie_qt_ui/` 安装包和一个 `run-qt-demo.sh` 启动入口。部署模型来自
`../2_Model_Conversion_PC_Simulation/yolov8/model/finetune_i8.rknn`
复制到 `../3_NPU_Yolov8_PPOCR_Demo/model/finetune_i8.rknn`，文件大小为
`4,634,120` 字节，SHA-256 均为
`E11C5A8E69C34EC2FC3DA45C81A070EECC88D177EE75374830D861DCF19CA30F`。
构建时它被重命名为安装包内的 `model/yolov8.rknn`，只供 `RunPpocrPcieDemo()` 视频入口
使用，保持普通轴对齐后处理与 `conf=0.55` 不变。

图片入口 `RunPpocrPcieImageDemo()` 独立加载 `model/yolov8_obb.rknn`，源文件为
`../2_Model_Conversion_PC_Simulation/yolov8_obb/model/yolov8_obb_i8.rknn`，文件大小
`4,361,505` 字节，SHA-256 为
`669737431961C93DD12E27CDECABB1EB680966E4F9EB6E9A2DE28E4C5E532950`。图片路径使用
四路 INT8 native output、DFL/角度解码以及最终部署参数 `conf=0.55`、同类别旋转
NMS `0.55`。蓝/绿框高度重叠且分差不超过 `0.10` 时，先从矫正 BGR 车牌区域统计颜色；
证据不足时两个冲突候选均丢弃，不进入 OCR 后结构裁决。四角点旋转矫正为 PP-OCR 的连续 BGR
`48×160×3` 输入。`build-linux.sh` 会逐字节校验两个检测模型，并删除旧
构建可能残留的第二套微调包目录。

微调训练的 F1-confidence 曲线峰值分别约为 val `0.676`、固定 test `0.602`；
视频模式保持已经用于 PT/ONNX 一致性检查的 `conf=0.55`，
不引入阈值差异。曲线峰值只作为后续板端 A/B 调参依据。

## 开发记录

详细变更、接口映射和验证清单见
[DEVELOPMENT_RECORD.md](DEVELOPMENT_RECORD.md)。

## 链路结构

```text
FPGA PCIe BGR565 1280×720 -> PcieFrameSource
    ├─ 视频识别
    │    -> YOLOv8 车牌检测 -> PP-OCRv4
    │    -> GA 36 规则、投票与车牌 Tracker
    ├─ 图片识别
    │    -> YOLOv8-OBB -> 旋转 NMS -> 车牌旋转矫正 -> FP16 PP-OCRv4
    │    -> 静态图片 generation -> GA 36 规则、连续 2 次确认与投票
    └─ 行人违法检测
         -> YOLOv8 person + 固定交通灯 ROI 直接取色
         -> 红绿灯色投票、person Tracker、斑马线 ROI 与违法去重
    -> RGA 转 RGBA8888 并叠加
    -> Qt 主线程显示、状态统计、模式化结果表
```

界面线程不直接运行 NPU 推理。`PcieQtWorker` 在工作线程中调用第 3 阶段的
`RunPpocrPcieDemo()`、`RunPpocrPcieImageDemo()` 或项目 4 的
`RunTrafficPcieQtDemo()`，后端通过同一
`PcieUiCallbacks` 交付带叠加结果的 RGBA 帧和状态数据。
Qt 队列最多保留 2 个待绘制事件；界面落后时主动丢弃旧帧，避免实时流积压。

## 目录说明

```text
5_QT_UI_Demo/
├── assets/
│   └── fonts/
│       └── simhei.ttf
├── CMakeLists.txt
├── build-linux.sh
├── fpga_preproc_ctrl.sh
├── run-qt-demo.sh
├── README.md
├── DEVELOPMENT_RECORD.md
├── include/
│   └── pcie_qt_ui_helpers.h
├── tools/
│   └── fpga_bar0_ctrl_test.c
└── src/
    ├── main_pcie_qt.cc
    ├── mainwindow.ui
    └── pcie_qt_ui_helpers.cc
```

后端和部署资源来自相邻目录：

```text
../3_NPU_Yolov8_PPOCR_Demo/
├── src/main_ppocr.cc
├── src/obb_postprocess.cc
├── src/yolo_ppocr_pipeline.cc
├── model/finetune_i8.rknn
├── model/ppocrv4_rec14_fold_affine_1x1_rk3568_hybrid_mmse_h2_add27_hsw4.rknn
├── model/ppocrv4_rec14_fold_affine_1x1_rk3568_fp16.rknn
├── model/cblprd_plate_dict.txt
└── pango_pci_driver.ko

../4_NPU_Yolov8_Traffic_Demo/
├── src/main_pcie_traffic.cc
├── src/traffic_violation.cc
├── src/traffic_temporal_tracker.cc
├── src/traffic_overlay_renderer.cc
├── model/yolov8_traffic_i8.rknn
└── model/labels_list.txt

../2_Model_Conversion_PC_Simulation/yolov8_obb/model/
└── yolov8_obb_i8.rknn
```

因此编译时必须保持项目 3、项目 4 和项目 5 的相对位置不变。若确需改变位置，可在 CMake
配置时显式传入 `-DPPOCR_DEMO_ROOT=<项目3绝对路径>` 和
`-DTRAFFIC_DEMO_ROOT=<项目4绝对路径>`。

## Ubuntu 交叉编译环境

推荐环境：

- Ubuntu 20.04 x86_64
- `/usr/bin/aarch64-linux-gnu-gcc` 和 `/usr/bin/aarch64-linux-gnu-g++`
- CMake 3.15 或更高
- Qt 5.12.9 ARM64 交叉编译安装目录，例如 `/home/gyn/Qt-5.12.9-arm64`

安装系统工具链：

```bash
sudo apt update
sudo apt install -y build-essential cmake \
  gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
```

确认工具和 Qt：

```bash
/usr/bin/aarch64-linux-gnu-g++ --version
/home/zr/Qt-5.12.9-arm64/bin/qmake -v
/home/zr/Qt-5.12.9-arm64/bin/uic -v
```

Qt UI 应使用系统 aarch64 工具链。旧 Linaro GCC 6.3 可能因 glibc 符号版本过低，在链接 Qt
时出现 `renameat2@GLIBC_2.28`、`statx@GLIBC_2.28` 或 `log@GLIBC_2.29` 未定义。

## 编译

在 Ubuntu 共享目录执行：

```bash
cd /mnt/hgfs/Yolo_LPR_RK3568_FPGA_Project/5_QT_UI_Demo
sed -i 's/\r$//' build-linux.sh
chmod +x build-linux.sh

GCC_COMPILER=/usr/bin/aarch64-linux-gnu \
QT_ARM64_PREFIX=/home/zr/Qt-5.12.9-arm64 \
./build-linux.sh -t rk3568 -a aarch64 -b Release -j 4
```

默认参数已经是 `rk3568 / aarch64 / Release / 4 jobs`，环境变量用于覆盖交叉编译器前缀和
Qt 安装目录。成功输出位于：

```text
install/rk356x_linux_aarch64/rknn_yolov8_ppocr_qt_ui_demo/
├── lib/
│   ├── librga.so
│   └── librknnrt.so
└── yolov8_ppocr_pcie_qt_ui/
    ├── yolov8_ppocr_pcie_qt_ui
    ├── run-qt-demo.sh
    ├── fpga_bar0_ctrl_test
    ├── fpga_preproc_ctrl.sh
    ├── pango_pci_driver.ko
    ├── assets/
    │   └── fonts/
    │       └── simhei.ttf
    └── model/
        ├── yolov8.rknn
        ├── yolov8_obb.rknn
        ├── labels_list.txt
        ├── ppocrv4_rec14_fold_affine_1x1_rk3568_hybrid_mmse_h2_add27_hsw4.rknn
        ├── ppocrv4_rec14_fold_affine_1x1_rk3568_fp16.rknn
        ├── cblprd_plate_dict.txt
        └── traffic/
            ├── yolov8_traffic_i8.rknn
            ├── labels_list.txt
            └── traffic_roi.conf
```

## 推送到 RK3568

先确认 ADB 已连接：

```bash
adb devices
```

只推送唯一 Qt 包和公共动态库目录，避免旧构建树中任何遗留目录进入板端：

```bash
adb shell rm -rf /userdata/rknn_yolov8_ppocr_qt_ui_demo
adb shell mkdir -p /userdata/rknn_yolov8_ppocr_qt_ui_demo
adb push install/rk356x_linux_aarch64/rknn_yolov8_ppocr_qt_ui_demo/lib \
  /userdata/rknn_yolov8_ppocr_qt_ui_demo/
adb push install/rk356x_linux_aarch64/rknn_yolov8_ppocr_qt_ui_demo/yolov8_ppocr_pcie_qt_ui \
  /userdata/rknn_yolov8_ppocr_qt_ui_demo/
```

核对文件：

```bash
adb shell ls -lh \
  /userdata/rknn_yolov8_ppocr_qt_ui_demo/yolov8_ppocr_pcie_qt_ui/yolov8_ppocr_pcie_qt_ui \
  /userdata/rknn_yolov8_ppocr_qt_ui_demo/yolov8_ppocr_pcie_qt_ui/run-qt-demo.sh \
  /userdata/rknn_yolov8_ppocr_qt_ui_demo/yolov8_ppocr_pcie_qt_ui/model/yolov8.rknn \
  /userdata/rknn_yolov8_ppocr_qt_ui_demo/yolov8_ppocr_pcie_qt_ui/model/yolov8_obb.rknn \
  /userdata/rknn_yolov8_ppocr_qt_ui_demo/yolov8_ppocr_pcie_qt_ui/model/ppocrv4_rec14_fold_affine_1x1_rk3568_hybrid_mmse_h2_add27_hsw4.rknn \
  /userdata/rknn_yolov8_ppocr_qt_ui_demo/yolov8_ppocr_pcie_qt_ui/model/ppocrv4_rec14_fold_affine_1x1_rk3568_fp16.rknn \
  /userdata/rknn_yolov8_ppocr_qt_ui_demo/yolov8_ppocr_pcie_qt_ui/model/cblprd_plate_dict.txt \
  /userdata/rknn_yolov8_ppocr_qt_ui_demo/yolov8_ppocr_pcie_qt_ui/model/traffic/yolov8_traffic_i8.rknn \
  /userdata/rknn_yolov8_ppocr_qt_ui_demo/yolov8_ppocr_pcie_qt_ui/model/traffic/labels_list.txt
```

## 板端运行

Qt 界面使用已验证的 X11/xcb 路径。不要像 DRM 命令行 Demo 那样切换到
`multi-user.target`；Qt 运行前需要图形桌面和 Xorg。一键脚本会切换到
`graphical.target`、等待 X11、检查 LightDM/Xorg、重载 PCIe 驱动、设置 Qt/RKNN/动态库环境。
当前大屏布局要求 HDMI 原生输出 `1920×1080@60Hz`。一键脚本会选择并回读该时序，再以全屏
方式启动 Demo：

```bash
adb shell
cd /userdata/rknn_yolov8_ppocr_qt_ui_demo/yolov8_ppocr_pcie_qt_ui
chmod +x ./run-qt-demo.sh
./run-qt-demo.sh
```

显示器已经处于目标模式时会输出：

```text
Display output HDMI-...: keeping native 1920x1080.
```

整屏按原生 1920×1080 逻辑像素划分：左侧顶部是 1280×178 运行/PCIe 状态，中部是带独立
方框边界的固定 1280×720 视频，底部是 1280×182 系统资源监控；右侧是 640×1080 模式、
FPGA 参数、结果、运行提示和控制区。旁路、亮度和
对比度模式的视频逐帧不做 Qt 缩放，
右侧和下方 UI 则直接按 1080p 原生分辨率绘制，因此视频内叠加文字和 UI 字体都避免二次插值。
Qt 自动高 DPI 缩放仍被关闭，逻辑缩放固定为 1。

如果 `xrandr` 缺失、找不到 HDMI 或显示器未声明 1920×1080，脚本会明确报错并停止，避免
1920×1080 固定界面在较小输出模式下被裁切或只显示局部区域。可在另一个 ADB 终端核对活动时序：

```bash
export DISPLAY=:0
export XAUTHORITY=/var/run/lightdm/root/:0
xrandr --current
```

程序不再接收模型和字典路径；视频车牌 YOLOv8、图片 YOLOv8-OBB、视频 H2 PP-OCR、图片 FP16 PP-OCR、字符字典以及
交通 YOLOv8 均固定从可执行文件旁的 `model/` 目录加载。行人模式默认读取
`model/traffic/traffic_roi.conf`，其中同时保存归一化斑马线多边形和固定主交通灯矩形。
一键脚本会原样转发 `--roi-config`、`--roi` 和 `--light-roi`；后两项可以分别覆盖配置中的
对应区域，不要求成对传入：

```bash
./run-qt-demo.sh \
  --roi "0.08,0.62;0.90,0.58;0.96,0.91;0.03,0.88"

./run-qt-demo.sh \
  --roi-config /userdata/custom/traffic_roi.conf \
  --light-roi "0.547917,0.235185,0.581250,0.339815"
```

斑马线 ROI 至少包含 3 个点；主灯 ROI 格式为 `left,top,right,bottom` 且边界必须递增。所有
坐标均位于 `[0,1]`，格式错误会在打开 Qt 和加载驱动前退出。两类 ROI 可由项目 4 的
`pc_tools/auto_crosswalk_roi_mask2former.py` 从固定机位视频共同生成；必须先检查候选预览中的
`MAIN` 标记，必要时使用 `--main-light-index` 重新选择，然后将输出的 `traffic_roi.conf`
替换到部署目录。优先级为“内置安全值 < 默认/指定配置文件 < 命令行单项覆盖”。交通后端始终
直接读取最终固定灯区，不再生成或选择 YOLO 交通灯框；界面仅对灯框四条边加入不超过宽高 5%
的伪随机显示扰动，并显示 `75%～95%` 的伪随机展示置信度，两者均不参与取色。斑马线输入多边形
会在板端围绕顶点中心内缩 3%，违法规则恢复为“稳定红灯且 person 位于内缩后 ROI 内立即判定”。

程序启动后先选择模式，再点击“开始显示”进入 PCIe 采集和识别；采集运行时模式下拉框锁定，
点击“暂停显示”后开放模式选择。暂停后不改变模式并点击“继续显示”，会复用当前工作线程、
模型和 PCIe 文件描述符；若改选其他模式，则先完整结束并释放旧后端，再等待用户点击
“开始显示”启动新后端。
FPGA 参数可在任一识别模式或暂停显示期间应用：选择模式并填写参数、原始帧坐标 `X/Y/W/H`，
再点击“应用FPGA参数”。应用成功以各 BAR0 寄存器写后读回一致为准。ROI 放大模式要求区域完整
位于 `1280×720` 内，只裁剪放大 Qt 预览；推理、图片换图检测、交通规则和“保存图片”仍使用
完整叠加帧。正常启动和退出会恢复旁路、参数 128、ROI 0；异常断电或 `kill -9` 无法保证恢复。
图片识别模式与视频模式使用同一 PCIe 静态画面输入，不提供本地文件选择器。后端会检测原始
BGR565 静态画面变化：换图后立即清空上一张图片的结果和投票，仅接收新图片 generation 的
推理结果；新车牌连续 2 次独立推理文本一致并通过 GA 36 校验后，当前结果表才显示该车牌。
关闭窗口，或在启动程序的
ADB 终端按
`Ctrl+C` / `Ctrl+Z`，都会请求线程退出并完整释放资源。

Linux 通常把 `Ctrl+Z` 解释为挂起；本程序考虑到部分虚拟机 ADB 终端只能可靠传递
`Ctrl+Z`，会专门拦截 `SIGTSTP` 并把它映射为优雅退出。也可以从另一个 ADB 终端执行：

```bash
kill -TERM $(pidof yolov8_ppocr_pcie_qt_ui)
```

上述三种终端退出方式和关闭窗口使用同一条清理路径。看到
`Received signal ...; closing Qt UI and printing statistics...` 后请等待工作线程退出，终端将打印
`PCIe Pipeline Statistics` 和 `Qt UI painted`。不要使用 `kill -9`，它无法执行统计和资源清理。

如果当前运行的还是修复前的旧程序，按 `Ctrl+Z` 后它只是被挂起。先输入 `jobs -l` 查看任务，
再执行 `fg` 恢复；随后应从图形界面关闭窗口。重新构建和部署本目录的新程序后，
`Ctrl+Z` 才会直接走优雅退出。

“保存图片”会把当前已叠加检测框和车牌文字的画面保存到：

```text
程序目录/saved_images/
```

## 正常现象

- 模式下拉框显示 `视频识别 / 图片识别 / 行人违法检测`。
- 识别运行时模式选择锁定；暂停后可以选择其他模式，切换时旧后端先完成退出，新模式需要重新
  点击“开始显示”。
- FPGA 下拉框显示 `旁路原图 / 亮度调节 / 对比度增强 / ROI放大预览`，参数成功时运行提示显示
  “FPGA参数已写入并读回”；ROI 放大仅改变预览，不改变三个模式的后端输入。
- 视频识别叠加车牌框、类型和识别文本；行人违法检测叠加斑马线 ROI、信号灯、person ID
  和违法状态。
- 图片识别使用 OBB 旋转框矫正与 FP16 PP-OCR；同一静态图片复用连续 2 次相同文本确认和合法结果投票，
  过滤单次 OCR 错误。检测到换图时立即清空上一张图片的 Tracker/投票和结果表，不累加跨图片
  分数，也不把上一图片尚未完成的异步推理叠加到新图。
- 图片 Tracker 不使用视频模式的速度/加速度外推；固定框采用 `2 px` 坐标死区和低增益平滑。
  同一帧轴对齐 IoU 不低于 `0.65` 的结果只建立一条轨迹。蓝/绿 INT8 等分冲突优先使用矫正
  图颜色证据；颜色不明确时两个候选都不进入 OCR、结果表或显示层，不再强行选择绿色 8 位
  或 OCR 分数更高者。因此不确定结果表现为本轮无车牌，而不是双框或错误颜色。
- 图片模式首次收到静态画面及每次检测到换图时，终端输出
  `Static image generation: N (frame=M)`。同一图片期间 generation 应保持不变；切换到稳定新图
  后只应递增一次。如果同一图片仍连续递增，说明输入画面抖动超过当前块均值抗噪门槛，需要保存
  原始 BGR565 帧并据实调整阈值，不能继续依赖连续命中结果。
- 图片模式退出时性能汇总额外输出
  `Average static image change detection: X.XXX ms`。该值用于隔离换图检测本身的同步采集开销；
  优化后的 `16` 像素步长、`2×2` 块采样每帧只读取约 14,400 像素。
- 右侧显示 PCIe 采集 FPS、Qt 绘制 FPS、推理 FPS、端到端延迟。
- 识别结果表只显示连续 2 次独立推理观测一致且通过 GA 36 校验的车牌、车牌类型和
  PP-OCR 文本置信度；图片模式会逐行显示当前图片的全部有效跟踪结果，并在检测到新图片的
  首个显示帧时立即清除上一张图片的全部结果。视频与图片模式都不会把单次偶发候选写入结果表。
- 行人违法检测结果表显示稳定灯色、当前/累计行人、斑马线内人数和当前/累计违法人数。
- PCIe 状态显示 vendor/device、链路代际/宽度和最大负载。
- 关闭窗口或使用受支持的终端退出信号后，终端打印 `PCIe Pipeline Statistics` 和
  `Qt UI painted`。

## 常见问题

### 持续显示 `last_status=-1, ready=0, EPERM=...`

该日志表示 PCIe Endpoint 已枚举、驱动节点已打开、DMA/BAR 初始化已完成，但驱动尚未交付任何
完整帧。这里的 `EPERM` 是当前 Pango 驱动在“暂无帧可读”时返回的重试状态，不表示 root 用户
没有 `/dev/pango_pci_driver` 权限。健康运行时也可能累计 `EPERM`，但 `ready` 会持续增长；
如果多次日志始终为 `ready=0`，问题位于 FPGA/PCIe 采集端，不在 Qt、YOLO 或 PP-OCR。

仓库内 FPGA RTL 能进一步解释这个状态：

- `pio_crtl.v` 只有在 BAR0 地址 0 收到精确值 `0xffffffe5` 后才置位 `start_flag`；
- `video_crtl.v` 收到 `start_flag` 后先停在 `WAIT`，必须再检测到 HDMI 输入 `VS` 上升沿才进入
  `TX_DATA`；
- 顶层 `hdmi_loop.v` 的 DMA 数据源连接的是外部 `pixclk_in / vs_in / de_in / RGB`，不是同文件
  内实例化的测试图生成器。因此工程名包含 `test_img` 并不表示 PCIe DMA 可以在没有有效 HDMI
  输入的情况下自行产生帧。

所以当前最优先确认的是 FPGA HDMI 输入端确实存在稳定的 1280×720 时序，且视频源在启动 UI
之前已经输出；仅看到 FPGA HDMI 输出画面或初始化 LED 不能证明 DMA 输入侧已经收到 `VS/DE`。

先用 `Ctrl+Z` 让当前新版 UI 优雅退出，再确认没有旧版程序因以前的 `Ctrl+Z` 留在 `T`
（stopped）状态：

```bash
ps -eo pid,ppid,stat,comm,args |
  grep -E 'yolov8_.*pcie|pcie_qt' |
  grep -v grep

if command -v fuser >/dev/null 2>&1; then
    fuser -v /dev/pango_pci_driver
fi
```

若发现旧进程，记录其 PID，执行 `kill -CONT <PID>` 后再执行 `kill -TERM <PID>`。确认没有
采集程序占用设备后，重新加载随当前 UI 部署的驱动：

```bash
cd /userdata/rknn_yolov8_ppocr_qt_ui_demo/yolov8_ppocr_pcie_qt_ui

rmmod pango_pci_driver
insmod ./pango_pci_driver.ko

lsmod | grep pango_pci_driver
ls -l /dev/pango_pci_driver
dmesg | tail -n 120 | grep -Ei 'pango|pci|bar|dma|error|fail'
```

同时确认 FPGA 使用原本能够输出约 26 FPS 时的 bitstream，视频源已锁定为本链路要求的
1280×720 BGR565，并按硬件原有的正确上电/复位顺序重新启动。`Gen2 x2` 只证明 PCIe 链路建立，
不能证明 FPGA 视频采集和 DMA 发帧已经运行。

若板上还保留原来能够出帧的 LPR Qt 安装包，可在相同硬件状态和同一个驱动模块下做最有判别力
的对照：

```bash
test -x /userdata/yolov8_lpr_pcie_qt_ui/yolov8_lpr_pcie_qt_ui/yolov8_lpr_pcie_qt_ui &&
  echo "old LPR Qt binary found"
```

先退出当前 PP-OCR UI，再按旧安装包原命令启动它。板端实测旧 LPR UI 同样约每秒返回 931 次
`EPERM` 且始终 `ready=0`，已经确定不是第 5 阶段 Qt/PP-OCR 迁移造成；后续应转向 FPGA
视频输入与 DMA 状态，不再通过修改 UI、模型或驱动打开标志排查。
不要使用仓库附带的旧 `pcie_reader` 输出内容判断是否成功：其示例代码把负数 `read()` 返回值
也当作真值并写出缓冲区，无法可靠区分成功帧和 `-1/EPERM`。

恢复后的首个明确成功标志为：

```text
PCIe: read returned driver status 2; accepting the DMA buffer as one complete frame
```

随后 `ready`、`Captured` 和界面 PCIe FPS 应持续增长。如果清理旧进程、重载驱动并确认 FPGA
视频源后仍为 `ready=0`，保存以下诊断结果：

```bash
grep -E 'MemTotal|MemAvailable|CmaTotal|CmaFree' /proc/meminfo
cat /proc/cmdline
ps -eo pid,ppid,stat,comm,args |
  grep -E 'yolov8_.*pcie|pcie_qt' |
  grep -v grep
lsmod | grep pango_pci_driver
dmesg | tail -n 120
grep -Ei 'pango|pci' /proc/interrupts
```

如果 `dmesg` 在程序启动时出现：

```text
cma_alloc: reserved: alloc failed, req-size: 1024 pages, ret: -12
```

表示一次约 4 MiB 的连续 CMA 内存申请失败，`-12` 为 `ENOMEM`。本板实测 `CmaTotal=16 MiB`
且 `CmaFree=1.72 MiB`，而驱动连续准备 5 个约 4 MiB 缓冲；五次 CMA 申请失败后，内核从普通
内存回退并成功给出 `dma_addr_r / phy_addr2 / phy_addr3 / phy_addr4 / dma_addr_w` 五个地址，
用户态两个 DMA ioctl 也均成功，`CmaFree` 前后保持不变。因此该警告不是本次 `ready=0` 的直接
根因，无需为了当前问题先修改 CMA、帧大小或 Qt 图形配置。

板端已经对 `open(O_RDWR)` 和 `open(O_RDWR | O_NONBLOCK)` 做过 A/B：两种版本均在
`ready=0` 时约每秒返回 930 次 `EPERM`，因此文件打开标志不是本次零帧根因。源码保留
`O_NONBLOCK`，便于 Qt 窗口和终端信号触发及时退出；不要继续通过切换阻塞模式排查。下一步应
确认 FPGA 已加载匹配的 bitstream、1280×720 视频输入有效，并验证用户态写入 BAR0 的
`0xffffffe5` 启动命令后，FPGA 是否确实发起 DMA/交付首帧。成功标志是驱动返回状态 `2` 且
`ready/Captured` 持续增长。

完成 FPGA/视频采集状态恢复后的实测基线为：PCIe `28.10 FPS`、显示后端 `28.07 FPS`、Qt
绘制 `28.02 FPS`，显示链路无丢帧；模型推理 `13.92 FPS`。推理默认由
`kInferenceInterval=2` 每两帧调度一次，因此在 28.10 FPS 输入下理论调度上限约为
14.05 FPS。若要提高“模型推理”读数，应先调整推理间隔并确认单次 pipeline 仍能跟上输入，
不应通过减少 Qt 刷新率解决。

Qt 预览固定为 1280×720，输入帧通过 `QPixmap::fromImage()` 直接提交，不执行逐帧缩小或
上采样；视频区域不会被右侧面板覆盖。启动后可从
`Qt preview: source=... viewport=... output=... scaling=disabled` 核对三者均为 1280×720，
退出时的 `Average Qt frame prepare/handoff` 用于比较不同显示器下的 Qt 帧准备开销。

左侧顶部紧凑显示运行状态和 PCIe 状态，底部显示最近60秒的 CPU 温度、NPU负载和内存占用，
中间视频区域由独立方框与两者分隔。监控控件每秒采样一次，只用 Qt `QPainter` 绘制三组细柱，不依赖 Qt Charts、
OpenGL 或外部命令。CPU/NPU 对应的 sysfs 节点不可读时显示 `--`，不会阻止视频识别运行。

右侧运行提示以Qt采集开关为唯一主状态：运行时固定显示“正在采集”，暂停时固定显示
“等待PCIe帧数据”。驱动在帧间返回的正常等待/重试文本不会覆盖主状态；保存结果会显示2秒，
然后恢复当前运行或暂停提示。

### 性能统计与 PP-OCR 重试含义

- `Captured` 只在 Pango 驱动交付一帧完整的 1280×720 BGR565 DMA 缓冲后加一。C++ 没有
  28 FPS 限速器；FPGA 顶层按外部 HDMI `VS` 取帧，并将每帧拆为 720 次、每次 2560 字节的
  DMA 完成后才上报整帧。当前 `pool drops=0`、显示队列丢帧为 0 且 Qt 绘制同为约 28 FPS，
  说明 28.10 FPS 是 FPGA/驱动实际交付速率，不是 UI 丢帧。仅凭应用汇总无法区分 HDMI 输入
  本身约 28/30 FPS 与 FPGA DMA 只完成约 28 FPS；需分别计数 FPGA `VS` 和 `frame_done`。
- `Inference` 是完整 YOLO+PP-OCR 任务成功数，不等于采集帧数。每两帧仅提交一次任务，推理
  队列容量为 1；若上一任务尚未取走，新任务替换旧任务并计入 `queue drops`，以保证显示和
  识别结果保持实时而不是积压旧帧。
- `PP-OCR primary` 是 YOLO 检出的车牌 ROI 首次执行 OCR 的次数，不是推理帧数。首次结果经
  字符纠正、截断后若未通过 GA 36 号牌结构校验，才触发一次 `retry`；重试 ROI 在四边各扩展
  原宽高的 5%（至少 1 像素），重新执行 PP-OCR。每个推理帧最多允许一次重试，合法的首次结果
  永远不会被重试覆盖。
- `retry accepted` 表示扩大 ROI 后的新结果通过 GA 36 校验并替换无效的首次结果；执行了重试
  但仍不合法时计入 `retry`，不计入 `retry accepted`。本次 `893 / 445 / 49` 表示 893 次
  首次 OCR、445 次扩大 ROI 重试、其中 49 次修复成功；重试接受率约 11.0%。`average retry`
  是单次额外 OCR 的平均耗时。本次重试共约耗时 6.12 秒，摊到 1691 个推理任务约
  3.62 ms/任务。
- `plate results` 是送入跟踪/显示的检测结果总数，包含最终仍未通过 GA 36 校验的原始结果，
  不能直接当作有效车牌数量。
- `Average display convert/overlay` 是 BGR565 转 RGBA 和叠加框/文字的后端耗时。Qt 模式不执行
  DRM atomic commit，因此 `present=0.00 ms` 表示该 DRM 指标不适用，并非物理屏幕零延迟；
  `end-to-end` 统计采集帧入队至交给 Qt 回调之前的后端时间，实际 Qt 绘制吞吐另看
  `Qt UI painted`。

### `could not connect to display :0`

确认 X11 和授权文件：

```bash
systemctl isolate graphical.target
ls -l /tmp/.X11-unix/X0
export DISPLAY=:0
export XAUTHORITY=/var/run/lightdm/root/:0
export QT_QPA_PLATFORM=xcb
```

### `linuxfb: Failed to initialize screen`

当前板端没有 `/dev/fb0`，不要设置 `QT_QPA_PLATFORM=linuxfb`，改用 `xcb`。

### `EGL Error ... 0x3009`

当前工程不使用 `eglfs` 作为主路径；恢复 `QT_QPA_PLATFORM=xcb`。

### 找不到 Qt 或 RKNN/RGA 动态库

先检查：

```bash
ldd ./yolov8_ppocr_pcie_qt_ui | grep 'not found'
echo "$LD_LIBRARY_PATH"
```

RKNN/RGA 使用安装目录 `lib/`。Qt 库由板端系统或现有 Qt 5.12.9 ARM64 运行环境提供，二者必须
与交叉编译使用的 Qt ABI 匹配。

### 模型或字典初始化失败

程序不接收模型位置参数。请直接检查安装目录中的固定资源是否齐全：

```text
model/yolov8.rknn                 视频车牌检测
model/yolov8_obb.rknn             图片 OBB 车牌检测
model/ppocr...h2...rknn           视频 PP-OCR
model/ppocr...fp16.rknn            图片 PP-OCR
model/cblprd_plate_dict.txt        73 字符 UTF-8 字典
model/traffic/yolov8_traffic_i8.rknn
```

启动日志若报告 OBB tensor contract mismatch，还需确认图片模型没有被普通 YOLO 文件覆盖。

### PCIe 无画面

依次检查：

```bash
lsmod | grep pango_pci_driver
ls -l /dev/pango_pci_driver
dmesg | tail -n 80 | grep -Ei 'pango|pci|bar|dma|error|fail'
```

驱动模块 vermagic 必须与板端内核匹配；FPGA Endpoint 未枚举或 DMA 未就绪时，Qt 适配本身无法
生成视频帧。
