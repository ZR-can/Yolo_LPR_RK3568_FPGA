# 5_QT_UI_Demo 1280x800 稳定版交接说明

> 最后更新：2026-08-14
>
> GitHub 仓库：`https://github.com/ZR-can/Yolo_LPR_RK3568_FPGA.git`
>
> 目标分支：`codex/pio-bar0-debug-20260806`
>
> 已验证组合：RK3568 Qt 1280x800 程序 + FPGA Stage06 contrast 稳定版本

## 1. 文档目的

本文说明当前 `5_QT_UI_Demo` 1280x800 最终工作版本的代码结构、显示适配
方法、运行关系、构建产物和部署流程，供后续维护人员继续修改或重新编译。

需要特别说明：最初的 800P 适配完成后，工程又经过用户后续修改。当前目录
不是最初适配刚完成时的原始快照，也没有一份可用于逐行还原全过程的独立历史
基线。因此本文以“当前已经验证正常的实际代码”为依据，准确说明现在是如何
工作的；对于无法由现有文件可靠证明的内容，不把它写成某一次修改的历史事实。

最初的 800P 适配只调整显示界面及其构建、部署标识，不改变 YOLO、PP-OCR、
交通违法检测等业务算法，不改变 RKNN 模型输入输出协议，也不改变 PCIe 图像
来源。后续版本又增加了 FPGA BAR0 参数控制、PCIe 会话稳定性处理、ROI 预览、
窗口控制和退出恢复默认参数等功能。本文同时记录这些后续改动。

1280x800 版使用独立可执行文件名和独立板端目录，可以与原显示器版本并存。

### 1.1 必须强调的显示器差异

项目原先使用的显示器不是当前 1280x800（800P）显示器。旧界面按更高的纵向
空间设计，直接运行在 800P 屏幕上会出现底部控件超出屏幕、任务栏或文字被挡、
窗口无法完整操作等问题。

当前版本不是简单修改分辨率字符串，而是专门做了以下 800P 适配：

- 独立目标名、可执行文件名和板端部署目录均带 `_1280x800`。
- 主窗口由曾经使用的 `showFullScreen()` 改为 `showMaximized()`，保留桌面标题
  栏、最小化、最大化和关闭按钮。
- 主窗口最小尺寸为 960x600，正常启动时最大化。
- 左侧预览固定为 840x405，状态区为 840x320。
- 右侧操作区使用纵向 `QScrollArea`，标题栏占用高度后仍能访问底部按钮。
- PCIe 原始帧仍是 1280x720 BGR565；840x405 只属于 Qt 预览，不改变模型输入
  和 FPGA/PCIe 帧协议。

## 2. 当前版本位置

Windows 工程目录：

```text
D:\a_fpga\codex\5_12_HDMI_IN_DDR3_HDMI_OUT\else\1\Yolo_LPR_RK3568_FPGA\5_QT_UI_Demo
```

Ubuntu 虚拟机共享目录：

```text
/mnt/hgfs/Yolo_LPR_RK3568_FPGA/5_QT_UI_Demo
```

交叉编译后的安装目录：

```text
5_QT_UI_Demo/install/rk356x_linux_aarch64/rknn_yolov8_ppocr_qt_ui_demo
```

1280x800 程序包目录：

```text
yolov8_ppocr_pcie_qt_ui_1280x800
```

可执行文件：

```text
yolov8_ppocr_pcie_qt_ui_1280x800/yolov8_ppocr_pcie_qt_ui_1280x800
```

板端部署目录：

```text
/userdata/yolov8_lpr_pcie_qt_ui_1280x800
```

## 3. 当前界面结构和适配目标

当前 800P 版保留了原 1080P 版的主要功能分区：

1. 左上区域显示 PCIe 实时图像。
2. 左下区域显示运行状态、PCIe 状态和系统资源监控。
3. 右侧显示标题、模式选择、FPGA 预处理参数、实时识别结果和运行提示。
4. 右下保留“开始显示/暂停显示”和“保存图片”按钮。

1280x800 屏幕的主要限制是纵向只有 800 像素，无法继续照搬 1080P 版的大
视频区和所有固定高度控件。因此适配方法不是删除功能，而是在保持区域顺序
和按钮关系不变的前提下，压缩视频预览和信息区高度，并让右侧在内容超高时
可以纵向滚动。

当前主窗口设计尺寸是 1280x776。剩余高度由桌面窗口边框和标题栏占用，程
序通过 `showMaximized()` 最大化显示，因此不需要停止桌面环境，也不直接抢占
DRM master。

## 4. 当前 UI 布局的具体实现

主要界面文件：

```text
5_QT_UI_Demo/src/mainwindow.ui
```

### 4.1 总体横向分区

界面保持左右两栏：

```text
总宽 1280
├── 左侧主显示区：840
│   ├── 实时图像：840 x 405
│   └── 状态与系统监控区：840 x 320
└── 右侧操作区：440
```

左右两区宽度相加正好为 1280。根布局的四周边距和区域间距均设为 0，防止
额外空白导致右侧控件被挤出屏幕。

### 4.2 实时图像区域

`videoLabel` 固定为 840x405。主程序中也再次调用
`setFixedSize(840, 405)`，保证运行时加载文字、图片或检测结果不会改变布局。

图像显示仍使用 Qt 的 `Qt::KeepAspectRatio`：

- 原图按比例缩放到 840x405 范围内。
- 不拉伸、不改变图像宽高比。
- 如果启用 ROI 预览，先裁剪合法 ROI，再按相同比例缩放。
- 未占满的区域由 `videoLabel` 背景保留，不会裁掉原始画面内容。

因此当前版本没有强制要求实时画面必须占据 1280x720，而是让原始 PCIe 图像
按照比例适配显示区域。这只影响屏幕展示尺寸，不影响送入模型的原始帧数据。

### 4.3 左下状态区

左下区域固定为 840x320，内部仍保留两层：

- 上层 154 像素高：运行状态和 PCIe 状态并排显示。
- 下层：`SystemMonitorWidget` 显示最近 60 秒的系统资源变化。

运行状态没有被删除，仍包括：

- 当前状态
- PCIe 采集状态/速率
- 屏幕显示状态/速率
- 模型推理状态/速率
- 端到端延迟

PCIe 状态仍包括设备、链路和最大负载信息。

系统资源监控由以下文件实现：

```text
5_QT_UI_Demo/include/system_monitor_widget.h
5_QT_UI_Demo/src/system_monitor_widget.cc
```

该控件继续显示 CPU 温度、NPU 负载和内存使用情况，并绘制最近 60 秒的历史
曲线。本次 800P 适配没有用简单文本替换这个原控件。

### 4.4 右侧操作和结果区

右侧固定宽度为 440，外层改为 `QScrollArea`：

- 水平滚动条永久关闭。
- 纵向内容超过当前可用高度时自动提供滚动。
- 在正常 1280x800 桌面尺寸下尽量完整展示；桌面标题栏较高时也不会把底部
  按钮永久挤出屏幕。

右侧控件的顺序保持不变：

1. 系统标题。
2. 模式选择和当前模式标识。
3. FPGA 预处理控制。
4. 识别结果表格。
5. 运行提示。
6. 开始显示和保存图片按钮。

保留的模式包括：

- 视频识别
- 图片识别
- 行人违法检测

保留的 FPGA 控件包括预处理模式、调节参数、ROI 的 X/Y/W/H 和“应用 FPGA
参数”按钮。ROI 参数范围仍按照 1280x720 原始视频坐标定义，而不是按照
840x405 的 UI 预览坐标定义，所以显示缩小不会破坏 FPGA 参数语义。

识别结果表格仍为三列：车牌、类型、置信度。`operationMessageLabel` 继续显
示启动、等待帧、识别和错误提示。

`startButton` 和 `saveButton` 都被保留：

- 开始后按钮文字切换为“暂停显示”。
- 运行时锁定模式选择，避免后端任务中途切换。
- 获得有效图像后才启用“保存图片”。
- 停止后恢复模式选择和“等待图像”状态。

## 5. 当前主程序中的显示适配

主程序文件：

```text
5_QT_UI_Demo/src/main_pcie_qt.cc
```

显示相关的关键实现如下：

1. 初始化时将 `videoLabel` 固定为 840x405，并保持居中对齐。
2. 收到后端图像回调后，将图像或 ROI 图像通过 `scaled(...,
   Qt::KeepAspectRatio, Qt::FastTransformation)` 缩放到标签范围。
3. 缩放结果只交给 `QPixmap` 显示，后端模型仍使用原始图像数据。
4. 主窗口使用 `showMaximized()`，继续运行在桌面 Qt 窗口系统中。
5. 开始、暂停、保存、模式切换和结果表更新的信号槽关系保持原版逻辑。

这里最重要的边界是：840x405 是 UI 预览尺寸，不是 YOLO 或 PP-OCR 的模型
输入尺寸，也不是 PCIe 帧尺寸。模型需要的缩放、量化和推理由原后端继续完成。

## 6. 业务和模型调用边界

当前版本继续复用已有后端，而不是在 `5_QT_UI_Demo` 内重新实现模型算法：

```text
3_NPU_Yolov8_PPOCR_Demo
4_NPU_Yolov8_Traffic_Demo
```

`CMakeLists.txt` 将第 3 阶段的 YOLOv8 + PP-OCR 源文件和第 4 阶段的交通检测
源文件编入同一个 Qt 程序。UI 只通过原有桥接接口启动相应模式和接收回调。

运行时模型路径以可执行文件所在目录为基准：

```text
model/yolov8.rknn
model/labels_list.txt
model/ppocrv4_rec14_fold_affine_1x1_rk3568_hybrid_mmse_h2_add27_hsw4.rknn
model/ppocrv4_rec14_fold_affine_1x1_rk3568_fp16.rknn
model/cblprd_plate_dict.txt
model/traffic/yolov8_traffic_i8.rknn
model/traffic/labels_list.txt
```

因此实际运行模型由安装包内 `model/yolov8.rknn` 决定。根据本项目约定，800P
工作只应涉及 UI，不应改变 RKNN 文件格式、模型输入输出、后处理、跟踪、车牌
规则或交通检测规则。由于当前工程还包含适配完成后的用户修改，后续若要严格
审计业务算法是否与某个历史版本逐行一致，必须再提供那个历史版本作为比较基线；
本文不根据当前 Git 差异对这段历史作无证据推断。

后续维护时，若只是继续调整 UI，禁止顺手改动以下内容：

- `RunPcie...` / `RunTrafficPcieQtDemo` 等后端入口及参数。
- RKNN 模型文件名和加载顺序。
- YOLO/PP-OCR 前处理、后处理和阈值。
- 视频识别、图片识别、行人违法检测的模式分派。
- PCIe 帧格式、ROI 坐标语义和 FPGA BAR0 寄存器协议。

## 7. 构建和版本区分

构建配置文件：

```text
5_QT_UI_Demo/CMakeLists.txt
5_QT_UI_Demo/build-linux.sh
```

为了避免与原 1080P 程序混淆，工程目标名、可执行文件名和安装子目录统一使
用：

```text
yolov8_ppocr_pcie_qt_ui_1280x800
```

CMake 定义 `PCIE_QT_UI_LAYOUT_1280X800`，用于明确标识该构建是 800P 布局
版本。安装阶段同时收集：

- Qt 主程序
- `fpga_bar0_ctrl_test`
- `fpga_preproc_ctrl.sh`
- `pango_pci_driver.ko`
- YOLO、PP-OCR 和交通检测模型/标签
- 中文字体 `simhei.ttf`

`CMAKE_OBJECT_PATH_MAX` 设为 128，且构建目录使用 `/tmp/qtu1280_build`，用于
规避 VMware HGFS 共享目录下 CMake 目标路径过长导致汇编器报
`File name too long` 的问题。构建结果仍安装回共享工程的 `install` 目录。

交叉链接时使用 `-Wl,--allow-shlib-undefined`，原因是随工程提供的 ARM64 Qt
动态库引用的 glibc 符号版本可能比虚拟机交叉工具链 sysroot 新；最终符号在
RK3568 板端运行环境解析。该选项不改变程序业务逻辑。

## 8. 交叉编译

在 Ubuntu 虚拟机中执行：

```bash
cd /mnt/hgfs/Yolo_LPR_RK3568_FPGA/5_QT_UI_Demo

sed -i 's/\r$//' build-linux.sh
chmod +x build-linux.sh

export GCC_COMPILER=aarch64-linux-gnu
./build-linux.sh
```

成功后应检查：

```bash
ls -l install/rk356x_linux_aarch64/rknn_yolov8_ppocr_qt_ui_demo/yolov8_ppocr_pcie_qt_ui_1280x800
ls -l install/rk356x_linux_aarch64/rknn_yolov8_ppocr_qt_ui_demo/lib
```

## 9. 推送到 RK3568

在 Windows PowerShell 中执行：

```powershell
cd D:\adb\bin

.\adb.exe devices

.\adb.exe shell "rm -rf /userdata/yolov8_lpr_pcie_qt_ui_1280x800"
.\adb.exe shell "mkdir -p /userdata/yolov8_lpr_pcie_qt_ui_1280x800"

.\adb.exe push "D:\a_fpga\codex\5_12_HDMI_IN_DDR3_HDMI_OUT\else\1\Yolo_LPR_RK3568_FPGA\5_QT_UI_Demo\install\rk356x_linux_aarch64\rknn_yolov8_ppocr_qt_ui_demo\yolov8_ppocr_pcie_qt_ui_1280x800" "/userdata/yolov8_lpr_pcie_qt_ui_1280x800/"

.\adb.exe push "D:\a_fpga\codex\5_12_HDMI_IN_DDR3_HDMI_OUT\else\1\Yolo_LPR_RK3568_FPGA\5_QT_UI_Demo\install\rk356x_linux_aarch64\rknn_yolov8_ppocr_qt_ui_demo\lib" "/userdata/yolov8_lpr_pcie_qt_ui_1280x800/"
```

也可以运行工程中的自动部署脚本：

```powershell
powershell -ExecutionPolicy Bypass -File "D:\a_fpga\codex\5_12_HDMI_IN_DDR3_HDMI_OUT\else\1\Yolo_LPR_RK3568_FPGA\5_QT_UI_Demo\scripts\deploy_qt_ui_1280x800.ps1"
```

自动脚本会检查 ADB、安装包和必需文件，重建指定的板端目录，推送程序包和
动态库，并恢复可执行权限。

## 10. 重启后一步运行

保持 RK3568 桌面正常运行，不停止桌面显示服务。Windows PowerShell 执行：

```powershell
cd D:\adb\bin

.\adb.exe shell "sh -c 'cd /userdata/yolov8_lpr_pcie_qt_ui_1280x800/yolov8_ppocr_pcie_qt_ui_1280x800 || exit 1; chmod +x ./yolov8_ppocr_pcie_qt_ui_1280x800 ./fpga_bar0_ctrl_test ./fpga_preproc_ctrl.sh; insmod ./pango_pci_driver.ko 2>/dev/null || true; export LD_LIBRARY_PATH=$PWD/../lib:$LD_LIBRARY_PATH; exec ./yolov8_ppocr_pcie_qt_ui_1280x800'"
```

这条命令依次完成：进入程序目录、恢复执行权限、加载 PCIe 驱动、设置动态库
搜索路径并启动 1280x800 Qt 程序。驱动已加载时 `insmod` 的错误会被忽略。

## 11. 后续修改注意事项

1. 调整布局时优先修改 `mainwindow.ui`，不要通过修改模型图像尺寸来迁就屏幕。
2. `main_pcie_qt.cc` 中的 `videoLabel` 固定尺寸必须与 `.ui` 保持一致。
3. 右侧控件较多，继续使用 `QScrollArea`，不要为了省高度删除按钮或状态区。
4. ROI 的最大范围继续对应 1280x720 原始视频，不要改成 840x405 UI 坐标。
5. 不要关闭桌面或改成直接 DRM/KMS 显示；当前版本是桌面内最大化 Qt 窗口。
6. 保持 `_1280x800` 文件名和板端目录，避免覆盖已经存在的另一显示版本。
7. 提交前必须确认分支是 `codex/pio-bar0-debug-20260806`，不要误推到 `main`
   或 `main_ui`。

## 12. 验收标准

重新编译或修改 UI 后至少确认：

- 1280x800 桌面中主窗口最大化后没有横向溢出。
- 左上实时图像完整按比例显示，没有拉伸。
- 左下运行状态、PCIe 状态、CPU 温度、NPU 负载和内存监控存在。
- 右侧三种模式、FPGA 参数、结果表和运行提示存在。
- “开始显示/暂停显示”和“保存图片”按钮可见且行为正常。
- 视频、图片和行人违法检测仍调用原有模型和后端。
- 原 1080P 板端目录没有被覆盖。

## 13. 800P 适配后的后续修改清单

下面按功能列出最初 800P 适配完成后继续加入的修改。维护人员不能把这些功能
误认为单纯的界面尺寸调整。

### 13.1 FPGA BAR0 控制入口

- 新增 `fpga_bar0_ctrl_test` 命令行工具并随 Qt 安装包部署。
- 新增 `fpga_preproc_ctrl.sh`，负责模式、参数和 ROI 的写入及读回确认。
- BAR0 控制寄存器使用 16-byte 对齐地址：

```text
0x100 magic
0x110 version
0x120 scratch
0x130 capture_ctrl
0x140 frame_status
0x150 preproc_mode
0x160 threshold/adjustment
0x170 roi_xy = {Y[15:0], X[15:0]}
0x180 roi_wh = {H[15:0], W[15:0]}
0x190 debug_trig
0x1a0 frame_cfg
0x1b0 ctrl_status
```

- 控制脚本写寄存器后立即读回，失败时最多重试三次。
- 脚本发现 `fpga_bar0_ctrl_test` 没有执行权限时会尝试自动 `chmod +x`。
- Qt 通过 `QProcess` 调用脚本，输出实际命令、标准输出、错误输出和退出码，便于
  从终端定位“参数应用失败”。

### 13.2 FPGA 预处理模式

Qt 当前显示的模式为：

```text
0 bypass       旁路
1 brightness   亮度调节
2 contrast     对比度增强
3 roi-zoom     ROI 放大预览
```

兼容脚本仍接受部分旧别名，例如 `gray/grayscale`、`threshold/binary` 和
`roi/zoom`，但新界面和新命令应使用上面的名称。

ROI 中 X/Y 是左上角坐标，W/H 是区域宽高，坐标基于 1280x720 原始帧。模式 3
下 FPGA 保留场景并压暗 ROI 外部区域，Qt 预览再裁剪 ROI 并放大；推理线程仍然
接收完整 1280x720 帧，不会因为预览放大而改变模型输入语义。

### 13.3 参数应用和退出恢复默认值

- “应用 FPGA 参数”写入模式、调节值、ROI XY 和 ROI WH，并检查每个寄存器
  读回。
- 应用成功后界面才更新当前 ROI 预览状态。
- 程序正常关闭时调用默认恢复：旁路、参数 128、ROI 全部为 0。
- 默认恢复应发生在 Qt 已经正常运行或关闭阶段；不要重新把 BAR0 子进程调用放
  到 `MainWindow` 构造早期。早期调用曾导致板端启动时段错误。
- FPGA 重新上电时硬件默认仍应是 `preproc_mode=0`。程序异常被杀死或系统断电
  时不能保证退出恢复代码执行，因此再次启动前可手动运行状态/旁路命令。

### 13.4 PCIe 启动逻辑恢复为稳定旧协议

- Qt 先通过 `PCI_MAP_ADDR_CMD` 映射 DMA 缓冲区，再发送 legacy start 命令
  `0xffffffe5`。
- Qt 主采集路径不写 `BAR0+0x130 capture_ctrl`。该寄存器保留给独立调试工具，
  不作为日常 UI 的 PCIe 开关。
- 关闭工作线程时发送 legacy stop `0xffffff00` 并解除 DMA 映射。
- 删除“一秒没有首帧就自动停止 DMA”的保护。没有帧时持续非阻塞重试，避免
  HDMI 帧稍晚到达就让整个会话退出。
- 驱动 `read()` 返回正数（现场常见状态 2）时，将当前 DMA 缓冲区接受为一帧。
- `EPERM` 在该驱动协议中作为“当前还没有可取的新帧”重试状态统计，不等同于
  Linux 文件权限配置失败；判断链路是否工作要看 `ready` 是否持续增加。

### 13.5 “开始显示/暂停显示”语义

- 已删除与“开始显示”重复的独立“开始 FPGA 采集”按钮。
- “开始显示”负责创建一次 PCIe/DMA 工作会话。
- “暂停显示”只暂停向 Qt 显示和推理队列投递帧，后台继续读取和排空 PCIe
  帧，避免反复开关 FPGA/DMA。
- PCIe 会话只在线程退出、程序关闭或必须重配后端时真正停止。
- 视频、图片和行人违法检测模式仍应在停止状态下切换，不能在任务运行中途直接
  切换后端。

### 13.6 窗口、信号和退出处理

- `showFullScreen()` 已替换为 `showMaximized()`，恢复右上角窗口控制。
- Qt 进程安装 SIGINT/SIGTERM 停止处理，终端 `Ctrl+C` 后由 Qt 主线程关闭
  窗口、清理工作线程并打印统计信息。
- 右侧参数和结果区域可纵向滚动，避免 800P 标题栏导致底部内容不可见。

### 13.7 构建与部署可靠性

- `QFileInfo::isFile(script_path)` 已修正为 Qt 5.12 兼容写法
  `QFileInfo(script_path).isFile()`。
- 构建目录固定使用 `/tmp/qtu1280_build`，规避 VMware HGFS 路径过长。
- 交叉链接仅对 Qt 程序使用 `-Wl,--allow-shlib-undefined`，由 RK3568 板端较新
  glibc 解析 Qt 动态库符号。
- 安装包必须同时包含 Qt 主程序、驱动、控制工具、控制脚本、模型、标签、字体
  和相邻 `lib` 目录。

## 14. 推荐交叉编译命令

在 Ubuntu 虚拟机执行：

```bash
cd /mnt/hgfs/Yolo_LPR_RK3568_FPGA/5_QT_UI_Demo
sed -i 's/\r$//' build-linux.sh fpga_preproc_ctrl.sh
chmod +x build-linux.sh fpga_preproc_ctrl.sh
export GCC_COMPILER=/usr/bin/aarch64-linux-gnu
export QT_ARM64_PREFIX=$HOME/Qt-5.12.9-arm64
./build-linux.sh -j4
```

必须看到主程序、控制工具和脚本均存在且可执行：

```bash
PKG=install/rk356x_linux_aarch64/rknn_yolov8_ppocr_qt_ui_demo/yolov8_ppocr_pcie_qt_ui_1280x800
ls -l "$PKG/yolov8_ppocr_pcie_qt_ui_1280x800"
ls -l "$PKG/fpga_bar0_ctrl_test"
ls -l "$PKG/fpga_preproc_ctrl.sh"
ls -ld install/rk356x_linux_aarch64/rknn_yolov8_ppocr_qt_ui_demo/lib
```

## 15. 推荐板端部署命令

Windows PowerShell 中执行：

```powershell
cd D:\adb\bin
.\adb.exe devices

.\adb.exe shell "pkill -TERM -f yolov8_ppocr_pcie_qt_ui_1280x800 2>/dev/null || true"
Start-Sleep -Seconds 2

.\adb.exe shell "rm -rf /userdata/yolov8_lpr_pcie_qt_ui_1280x800"
.\adb.exe shell "mkdir -p /userdata/yolov8_lpr_pcie_qt_ui_1280x800"

.\adb.exe push "D:\a_fpga\codex\5_12_HDMI_IN_DDR3_HDMI_OUT\else\1\Yolo_LPR_RK3568_FPGA\5_QT_UI_Demo\install\rk356x_linux_aarch64\rknn_yolov8_ppocr_qt_ui_demo\yolov8_ppocr_pcie_qt_ui_1280x800" "/userdata/yolov8_lpr_pcie_qt_ui_1280x800/"

.\adb.exe push "D:\a_fpga\codex\5_12_HDMI_IN_DDR3_HDMI_OUT\else\1\Yolo_LPR_RK3568_FPGA\5_QT_UI_Demo\install\rk356x_linux_aarch64\rknn_yolov8_ppocr_qt_ui_demo\lib" "/userdata/yolov8_lpr_pcie_qt_ui_1280x800/"
```

也可以使用自动部署脚本：

```powershell
powershell -ExecutionPolicy Bypass -File "D:\a_fpga\codex\5_12_HDMI_IN_DDR3_HDMI_OUT\else\1\Yolo_LPR_RK3568_FPGA\5_QT_UI_Demo\scripts\deploy_qt_ui_1280x800.ps1"
```

注意：不要在旧 UI 进程运行时直接覆盖正在执行的二进制文件。应先停止进程，
再删除并完整推送程序目录和 `lib`。

## 16. 板端运行、检查和停止命令

用户进入 `adb shell` 后，后续命令直接在板端执行，不需要再套一层
`adb shell "..."`。

### 16.1 启动

```bash
cd /userdata/yolov8_lpr_pcie_qt_ui_1280x800/yolov8_ppocr_pcie_qt_ui_1280x800
chmod +x ./yolov8_ppocr_pcie_qt_ui_1280x800 ./fpga_bar0_ctrl_test ./fpga_preproc_ctrl.sh
insmod ./pango_pci_driver.ko 2>/dev/null || true
export LD_LIBRARY_PATH=$PWD/../lib:$LD_LIBRARY_PATH
./yolov8_ppocr_pcie_qt_ui_1280x800
```

原现场使用的一行启动命令也可以继续使用：

```bash
cd /userdata/yolov8_lpr_pcie_qt_ui_1280x800/yolov8_ppocr_pcie_qt_ui_1280x800 && chmod +x ./yolov8_ppocr_pcie_qt_ui_1280x800 ./fpga_bar0_ctrl_test ./fpga_preproc_ctrl.sh && insmod ./pango_pci_driver.ko 2>/dev/null || true; export LD_LIBRARY_PATH=$PWD/../lib:$LD_LIBRARY_PATH; ./yolov8_ppocr_pcie_qt_ui_1280x800
```

建议优先使用分行形式，避免 `&&`、`||` 和 `;` 优先级使工作目录失败后仍继续
运行后半段命令。

### 16.2 启动前控制面检查

```bash
./fpga_bar0_ctrl_test --regs
./fpga_preproc_ctrl.sh status
./fpga_preproc_ctrl.sh apply bypass 128 0 0 0 0
```

控制工具只有在 FPGA 当前固件包含对应 BAR0 寄存器时才有效。纯 PCIe 基线
固件可以正常出图，但不一定提供该控制面。

### 16.3 停止进程

前台运行时先按 `Ctrl+C`。另一个板端终端也可以执行：

```bash
pkill -TERM -f yolov8_ppocr_pcie_qt_ui_1280x800
sleep 2
pidof yolov8_ppocr_pcie_qt_ui_1280x800 || true
```

仅在进程没有响应时最后使用：

```bash
pkill -KILL -f yolov8_ppocr_pcie_qt_ui_1280x800
```

`SIGKILL` 不会执行参数恢复和正常资源清理，所以不应作为日常关闭方式。

## 17. 运行验收流程

1. HDMI 输入稳定后启动程序。
2. 点击一次“开始显示”，确认 `ready` 增长且出现实时图像。
3. 点击“暂停显示”，确认 UI 暂停但程序和 PCIe 会话未退出。
4. 恢复显示，确认无需重新加载驱动或重启 FPGA。
5. 依次测试 bypass、brightness、contrast、roi-zoom。
6. 每次参数应用必须看到寄存器写入、读回以及退出码 0。
7. 关闭窗口或发送 `Ctrl+C`，确认程序打印统计信息并正常退出。
8. 再次读取寄存器，正常关闭后应恢复旁路、参数 128、ROI 0。
9. 不断电再次启动，确认仍能正常显示 PCIe 图像。

## 18. Git 版本和提交边界

当前交接目标是：

```text
仓库：https://github.com/ZR-can/Yolo_LPR_RK3568_FPGA.git
分支：codex/pio-bar0-debug-20260806
当前已推送代码提交：0fa6092 Fix FPGA controls and fit PCIe preview
```

提交 `0fa6092` 明确包含：

- `5_QT_UI_Demo/fpga_preproc_ctrl.sh`
- `5_QT_UI_Demo/src/main_pcie_qt.cc`
- `5_QT_UI_Demo/src/mainwindow.ui`
- `CODEX_CHANGELOG.md`

本地工作区还可能存在其他实验文件、生成目录或队友改动。提交本交接文档时只
暂存 `5_QT_UI_Demo/README_1280X800_HANDOFF.md`，不得使用 `git add .`，不得
顺带提交 `build/`、`install/` 或与本次交接无关的删除和模型文件。

以后若代码继续变化，应先在同一分支追加明确提交，并同步更新本文顶部的提交号
和“后续修改清单”。
