# Codex 改版记录

本文件记录 AI/RK3568 仓库中由 Codex 协助完成的代码改版、验证状态和遗留问题。

## 2026-08-14 - Stable FPGA control and complete PCIe preview

Branch: `codex/pio-bar0-debug-20260806`

- Qt FPGA controls support bypass, gain/brightness, and contrast modes through
  the verified BAR0 register interface.
- UI startup and normal window close restore FPGA defaults: mode 0, threshold
  128, and zero ROI parameters. This does not stop PCIe DMA.
- The 1280x720 PCIe frame is scaled to the fixed 16:9 preview viewport instead
  of being clipped at its original pixel size.
- The 1280x800 layout uses an 840x405 video viewport so the lower status area
  remains visible below the desktop title bar.
- The control script restores execute permission on `fpga_bar0_ctrl_test` after
  ADB deployment and retries transient BAR0 accesses up to three times.
- Control failures now print the command, stdout, stderr, and process exit code.
- Board verification passed with the stable Stage 06 FPGA image: PCIe video,
  bypass, brightness/gain, contrast, restart defaults, and Qt display all work.

## 记录规则

- “已验证跑通”必须写明验证时间、验证环境和关键命令/结果。
- 本地只完成静态检查、未上板运行、缺少交叉编译工具链的改动，统一标记为“待验证”。
- 涉及 PDS 编译、bitstream、SFC、Flash 烧录的步骤默认由用户手动执行；Codex 只记录步骤和结果。

## 2026-08-01 17:00 - RK 侧 BAR0 控制/状态通道验证工具

验证状态：已验证跑通基础 RK -> FPGA BAR0 控制/状态闭环。

修改内容：

- 新增 RK3568 用户态测试工具：
  `D:\a_fpga\codex\5_12_HDMI_IN_DDR3_HDMI_OUT\else\1\Yolo_LPR_RK3568_FPGA\4_NPU_Yolov8_Traffic_Demo\tools\fpga_bar0_ctrl_test.c`
- 修改 Traffic Demo 构建脚本：
  `D:\a_fpga\codex\5_12_HDMI_IN_DDR3_HDMI_OUT\else\1\Yolo_LPR_RK3568_FPGA\4_NPU_Yolov8_Traffic_Demo\CMakeLists.txt`
- 修改 Traffic Demo Linux 构建入口：
  `D:\a_fpga\codex\5_12_HDMI_IN_DDR3_HDMI_OUT\else\1\Yolo_LPR_RK3568_FPGA\4_NPU_Yolov8_Traffic_Demo\build-linux.sh`
- 新增只编译 BAR0 小工具的轻量脚本：
  `D:\a_fpga\codex\5_12_HDMI_IN_DDR3_HDMI_OUT\else\1\Yolo_LPR_RK3568_FPGA\4_NPU_Yolov8_Traffic_Demo\build-fpga-bar0-tool.sh`
- 新工具复用现有 `pango_pci_driver` / `pango_pci.h` 流程，通过 `PCI_MAP_BAR0_CMD` 获取 BAR0 物理地址，再 mmap `/dev/mem` 访问 FPGA BAR0。
- 默认读取现有 `BAR0 + 0x140` 帧状态寄存器；可选写 `BAR0 + 0x000` 的 legacy stop/start 命令。
- `--start` 会先调用现有 DMA 配置和地址映射 ioctl，避免在 DMA 基地址未准备好时启动 FPGA MWr；默认轮询后写 stop 并释放 DMA 映射。
- `build-linux.sh` 安装后会检查 `fpga_bar0_ctrl_test` 是否生成到 `yolov8_traffic_pcie_demo` 目录。

保持不变：

- 未修改 FPGA RTL。
- 未修改 PCIe IP、PDS 工程、FDC 约束或 bitstream。
- 不需要重新跑 PDS 编译。

本地检查：

- 已核对 `pango_pci.h` ioctl、BAR0、`/dev/mem` 路径与现有 `pcie_frame_source.cc` 用法一致。
- Codex 本机 PATH 中未找到 `aarch64-linux-gnu-gcc`、`gcc`、`cl`、`cmake`、`ninja` 或 `make`，因此不能在 Windows 侧完成本地编译验证。
- 已根据队友现有虚拟机交叉编译习惯，补充单文件编译脚本；仍需在 Linux 虚拟机中运行验证。

虚拟机交叉编译结果：

- 2026-08-01，用户在 Ubuntu 虚拟机 `/mnt/hgfs/4_NPU_Yolov8_Traffic_Demo` 执行：
  `./build-fpga-bar0-tool.sh`
- 已生成 aarch64 可执行文件：
  `/mnt/hgfs/4_NPU_Yolov8_Traffic_Demo/build/fpga_bar0_tool/fpga_bar0_ctrl_test`
- `file` 识别结果为：
  `ELF 64-bit LSB shared object, ARM aarch64`
- 首次编译出现 `usleep` 隐式声明告警；已改为 `nanosleep` 延时实现，下一次编译应无该告警。

板端运行结果：

- 2026-08-01，用户在 RK3568 板端目录
  `/userdata/rknn_yolov8_ppocr_qt_ui_demo/yolov8_ppocr_pcie_qt_ui`
  运行 `fpga_bar0_ctrl_test`。
- 驱动已加载：
  `pango_pci_driver 49152 0`
- 设备节点存在：
  `/dev/pango_pci_driver`
- `--status` 读到 PCIe 设备信息：
  `vendor=0x0755 device=0x0755 link=gen2 x2 mps=128 mrrs=512`
- BAR0 映射成功：
  `phys=0xf0200000 mapped_len=4096`
- `--status`、`--stop --status`、`--start --poll 30 --delay-ms 100` 均可运行，没有 ioctl/mmap/read/write 报错。
- 无 HDMI 输入时，现有 `BAR0 + 0x140` 返回始终为：
  `0x00000000 frame_done=0 wr_index=0`
- 随后用户再次运行 `--start --poll 30 --delay-ms 100`，`BAR0 + 0x140`
  出现稳定交替：
  `0x00000000 frame_done=0 wr_index=0`
  与
  `0x00000004 frame_done=0 wr_index=2`
- 这说明 RK 侧写 `BAR0 + 0x000 = 0xffffffe5` 后，FPGA 侧状态输出出现可观察变化。

当前判断：

- 已证明 RK 侧可以通过现有 `.ko` 获取设备信息并 mmap BAR0，PCIe 链路为 Gen2 x2。
- 已证明 RK 侧可写 legacy start/stop 控制寄存器，且 start 后 FPGA 状态寄存器有响应。
- 已证明 RK 侧可读现有 `0x140` 状态寄存器，并解析 `wr_index`。
- `frame_done` 仍为 0 是合理的，因为该位依赖真实帧完成事件；当前没有把 HDMI 输入稳定性作为本次验证目标。
- 下一阶段可以在保留 legacy 逻辑的基础上扩展正式控制寄存器，例如 magic/version、scratch、采集开关、预处理模式、阈值、ROI 和调试触发。

预期板端验证命令：

```bash
./fpga_bar0_ctrl_test --status
./fpga_bar0_ctrl_test --stop --status
./fpga_bar0_ctrl_test --start --poll 30 --delay-ms 100
```

下一步判断：

- 如果 `0x140` 可读且 `frame_done/wr_index` 有合理变化，说明现有 RK 读 FPGA 状态通道已打通。
- 如果 `0x140` 一直为 0 或读失败，再进入 FPGA 侧 Debugger，观察 `pio_rd_en`、`pio_rd_addr`、`pio_rd_data` 以及 MRD/CPLD 应答路径。

## 2026-08-01 17:25 - FPGA BAR0 控制寄存器 v1

验证状态：RTL 单文件语法检查通过；待用户手动 PDS 编译/下载后板端验证。

修改内容：

- 修改 FPGA PIO 控制模块：
  `D:\a_fpga\codex\5_12_HDMI_IN_DDR3_HDMI_OUT\else\1\pcie_test_img_100h\src\pcie\pio_crtl.v`
- 在 BAR0 `0x100` 段新增 RK 可见寄存器：
  - `0x100` magic，固定读出 `0x46504331`，即 `FPC1`
  - `0x104` version，固定读出 `0x20260801`
  - `0x108` scratch，可写可读
  - `0x10c` capture control，bit0 控制原 `start_flag`
  - `0x110` preprocessing mode，暂存预处理模式
  - `0x114` threshold，暂存阈值
  - `0x118` ROI XY，暂存 ROI 左上角
  - `0x11c` ROI WH，暂存 ROI 宽高
  - `0x120` debug trigger，暂存调试触发命令
  - `0x124` frame config，暂存帧率/分辨率配置
  - `0x128` control status，返回 `start/frame_done/wr_index/cfg_write_count`
- 保留原有 legacy 控制：
  - `BAR0 + 0x000 = 0xffffffe5` start
  - `BAR0 + 0x000 = 0xffffff00` stop
- 保留原有 DMA 地址寄存器和 `0x140` 帧状态寄存器。
- 本版新增寄存器先用于 RK -> FPGA 读写闭环验证，尚未接入真实预处理/ROI 数据路径。
- 2026-08-01 17:40 修正 `ctrl_status` 拼接位宽为 32 位，避免读回值被截断。

RK 工具同步修改：

- 修改：
  `D:\a_fpga\codex\5_12_HDMI_IN_DDR3_HDMI_OUT\else\1\Yolo_LPR_RK3568_FPGA\4_NPU_Yolov8_Traffic_Demo\tools\fpga_bar0_ctrl_test.c`
- 新增参数：
  - `--regs`
  - `--scratch VALUE`
  - `--control VALUE`
  - `--read OFFSET`
  - `--write OFFSET --value VALUE`

已完成检查：

- ModelSim 单文件编译 `pio_crtl.v`：
  `Errors: 0, Warnings: 0`
- Codex Windows 侧没有 aarch64 交叉编译器，因此 RK 工具需在 Ubuntu 虚拟机重新运行：
  `./build-fpga-bar0-tool.sh`

待验证步骤：

1. 用户在 PDS 中手动重新编译/生成 bitstream/下载 FPGA。
2. 用户在 Ubuntu 虚拟机重新编译并推送 `fpga_bar0_ctrl_test`。
3. 板端运行：

```bash
./fpga_bar0_ctrl_test --regs
./fpga_bar0_ctrl_test --scratch 0x12345678 --regs
./fpga_bar0_ctrl_test --control 1 --regs
./fpga_bar0_ctrl_test --control 0 --regs
```

预期：

- `magic` 读出 `0x46504331`
- `version` 读出 `0x20260801`
- `scratch` 写入后能读回同样的值
- `capture_ctrl` 随 `--control 1/0` 改变
- `ctrl_status` 中 `cfg_writes` 随配置写入增加

## 2026-08-06 - Branch and BAR0 debug note
- Created local branch: codex/pio-bar0-debug-20260806
- No repo source files were changed in this step.
- The working issue remains the FPGA BAR0 readback mismatch: magic is visible, but version/scratch/control registers still read 0.
- Next step is to capture pio_wr_en/pio_wr_addr/pio_wr_data/pio_rd_en/pio_rd_addr/pio_rd_data.
- Strong clue: synthesize/hdmi_loop_syn.fic still shows a 2026-04-27 timestamp while compile and bitstream outputs are from 2026-08-06.
- That makes the active split/FIC the first thing to refresh before blaming RTL.

## 2026-08-10 23:36:11 +08:00 - PDS/FIC regression checkpoint

After the user rebuilt, generated SFC, programmed Flash, and physically
restarted the board, PCIe still enumerated as Gen2 x2, but all aligned BAR0
control registers read zero, including the previously stable magic register.

PDS `device_map/run.log` reported:

`Inserter-0026: Fic file .../hdmi_loop_syn_hdmi_activity.fic is not valid`

The new HDMI activity FIC was not valid for PDS. This does not by itself prove
that the user's SFC/SBIT files were old: the decisive issue is that the PDS
flow using that FIC did not complete a valid Device Map/insertion step. The
PDS project was restored to the known-good `synthesize/hdmi_loop_syn.fic`.
Rebuild from Device Map onward and verify both successful PDS logs and
`magic=0x46504331` before any RK DMA test.

User correction, 2026-08-10: the user confirmed that the SFC and SBIT used for
programming were the latest generated outputs. Keep that fact separate from
the FIC/Device Map failure; do not label those files stale without direct
proof.

## 2026-08-10 19:37:38 +08:00 - Qt FPGA capture start-order safety

现场现象：HDMI 已接入且 PCIe BAR0 可读写；点击 Qt 的 FPGA 采集按钮后，
RK3568 图形界面、鼠标和 ADB 可能同时失去响应，需要重启恢复。

对照结果：当前工程与旧版
`else/pcie_720p/pcie_test_img_100h` 的 `video_crtl.v`、
`ips2l_pcie_dma.v`、`ips2l_pcie_dma_controller.v` 内容一致。当前新增的
FPGA 改动集中在 PIO 控制寄存器和 `video_preproc`，没有重写 PCIe DMA IP。

根因判断：Qt 新按钮曾在 RK 侧 DMA 缓冲尚未完成映射时直接写
`BAR0+0x130=1`。该寄存器连接 FPGA 原有 `start_flag`，可能让 FPGA 在 RK
地址尚未准备好时立即发起 DMA，造成 PCIe Host/ADB 阻塞。

本次修复：

- `pcie_frame_source.cc` 改为先完成 `PCI_MAP_ADDR_CMD`，再写
  `0x130=1` 和 legacy start 命令；关闭时同时写 legacy stop 和 `0x130=0`。
- `main_pcie_qt.cc` 禁止 UI 空闲时单独开启 FPGA 采集；必须先启动已有
  DMA 会话。DMA 会话结束后按钮恢复为“开启FPGA采集”。
- 未修改 FPGA PCIe DMA RTL。

验证边界：本次 RK 端修改需要重新交叉编译并推送后再验证；在此之前不要
点击 Qt 的“开启FPGA采集”。若设备再次无响应，先物理重启，再检查
`lspci`、ADB 和 BAR0，避免重复启动 DMA。
## 2026-08-11 19:22:32 +08:00 - Restore Qt window controls

The 1280x800 Qt application had no minimize, maximize, or close buttons because
`5_QT_UI_Demo/src/main_pcie_qt.cc` explicitly called `window.showFullScreen()`.

Changed the entry point to `window.showMaximized()`. The application will still
open maximized for the 1280x800 display, but it will retain the desktop title
bar and normal window controls. This is an RK Qt-only change; no FPGA/PDS
source was changed by this UI fix.

Verification status: source edit complete. Cross-compilation and board
deployment are required before the change is visible on RK3568.

## 2026-08-11 19:28:39 +08:00 - Qt cross-link compatibility fix

The Linux ARM64 cross-build compiled all sources successfully but failed while
linking the Qt executable because the bundled Qt shared libraries reference
versioned glibc symbols newer than the legacy host cross-linker's sysroot:
`renameat2@GLIBC_2.28`, `statx@GLIBC_2.28`, `getentropy@GLIBC_2.25`,
`log@GLIBC_2.29`, and `pow@GLIBC_2.29`.

The RK3568 board uses glibc 2.36. `5_QT_UI_Demo/CMakeLists.txt` now applies
`-Wl,--allow-shlib-undefined` only to the cross-compiled Qt executable, so
these shared-library symbols are resolved by the board's runtime loader. The
BAR0 control tool and FPGA RTL are unaffected.

Verification status: CMake change applied; rerun `./build-linux.sh -j4` in the
Ubuntu build environment. Deployment remains pending until the final link
completes successfully.

## 2026-08-11 19:36:26 +08:00 - First post-fix PCIe video frame

After the corrected FPGA image was flashed and the rebuilt Qt package was
deployed, the board produced a visible PCIe video stream. The runtime log
confirmed `capture started, BGR565 1280x720`, active PCIe Gen2 x2, and both
inference and display threads running.

BAR0 status during capture was `capture_ctrl=1`, `ctrl_status.start=1`,
`wr_index=3`, and `frame_status=0x6`. The frame status value corresponds to
the current four-buffer write index (`3`) with the sticky frame-done bit clear
at the instant of the register read. The earlier no-frame timeout is therefore
resolved.

The preview showed some visible motion/jitter. No immediate PCIe link failure,
frame starvation, or RTL regression was observed. The current likely causes
are HDMI pixel-clock/frame synchronization or RK Qt display refresh timing;
this is accepted for the current demonstration checkpoint and no new RTL
change was made.

## 2026-08-11 19:42:24 +08:00 - FPGA preprocessing control validation

While the PCIe video stream was active, the RK-side UI successfully applied
the threshold and ROI preprocessing settings. BAR0 readback confirmed
`preproc_mode=2`, `threshold=0x80`, then `preproc_mode=3`,
`roi_xy=0x00640064`, and `roi_wh=0x01000200`; the displayed video changed
accordingly. The FPGA remained in capture state and continued rotating frame
buffers.

The ROI packing was also checked directly: `roi 117 118 512 256` produced
`roi_xy=0x00760075` and `roi_wh=0x01000200`, confirming the format is
`roi_xy={Y[15:0], X[15:0]}` and `roi_wh={H[15:0], W[15:0]}`. The earlier
`0x00740075` therefore represented X=117, Y=116 and was a UI entry value
variation, not an FPGA packing defect.

## 2026-08-11 19:52:03 +08:00 - Qt capture control and 1280x800 window layout

The separate `开启FPGA采集` button was removed from the Qt UI and its handler
was removed from `5_QT_UI_Demo/src/main_pcie_qt.cc`. The `开始显示` session is
now the single owner of the DMA/display lifecycle; the worker maps DMA before
`PcieFrameSource` arms FPGA capture, and closing the worker stops capture.
FPGA preprocessing parameter application remains available through the
dedicated apply button.

The fixed 800-pixel panel heights were changed so the title bar no longer
pushes the lower content below a 1280x800 display. The right control panel is
now a vertically scrollable `QScrollArea`, while the left panel can shrink to
the available client height. The window keeps normal title-bar controls and
starts maximized with a minimum window size of 960x600.

Verification status: UI XML parses successfully and source references are
consistent. Cross-compilation and board deployment are required for visual
verification.

## 2026-08-11 20:49:14 +08:00 - Preprocessing controls and ROI zoom preview

The Qt FPGA control panel now presents mode 1 as `亮度调节`, mode 2 as
`对比度增强`, and mode 3 as `ROI放大预览`. The legacy script aliases remain
accepted for compatibility, while new commands use `brightness`, `contrast`,
and `roi-zoom`.

After a successful FPGA parameter write/readback, the Qt preview stores the
active ROI and crops/scales that rectangle with `Qt::FastTransformation` when
mode 3 is selected. The worker and inference path still receive a full-frame
1280x720 image, with the FPGA preprocessing applied; only the visual preview
is cropped and zoomed. ROI coordinates are pixel
values: X/Y is the top-left corner and W/H is the rectangle size.

The FPGA mode-3 output now dims pixels outside the ROI, preserving scene
context. FPGA mode 0/1/2 data paths and the PCIe frame protocol are unchanged.

## 2026-08-12 - Qt capture startup reverted to legacy control

The current FPGA image still exposes `capture_ctrl` at BAR0+0x130, but the Qt
capture source no longer writes this register. Qt now restores the original
startup protocol: map the RK DMA buffer, write the legacy start command
`0xffffffe5`, and keep reading frames until the user pauses or exits.

The Qt-side one-second first-frame timeout and automatic DMA close were removed.
When no frame is available, `ReadFrame` continues its existing nonblocking retry
behavior, so a late HDMI frame does not terminate the UI session. Shutdown still
writes the legacy stop command `0xffffff00`.

This change is RK-side only. It does not change the FPGA bitstream, PCIe IP,
DMA packet format, BAR0 register map, or preprocessing RTL. Cross-compilation
and board deployment are required before testing.

## 2026-08-12 - UI pause keeps the PCIe session armed

The Qt `开始显示/暂停显示` control is defined as a display-delivery control,
not a hardware capture power switch. While paused, the capture loop continues
reading and draining PCIe frames without forwarding them to the Qt display or
inference queues. This prevents repeated FPGA legacy start/stop writes during
normal UI interaction.

The PCIe/DMA session is started once when the worker begins and is stopped only
when the worker exits, such as application shutdown or an explicit session
reconfiguration. The FPGA `capture_ctrl` register is not toggled by the UI
pause button.

The Qt controls default to the bypass entry. The startup path does not invoke
the BAR0 control script before the Qt event loop is running; FPGA reset remains
the source of the hardware default `preproc_mode=0`. This avoids coupling Qt
construction to a BAR0 subprocess and keeps startup independent of the driver
control helper.

## 2026-08-12 - Remove early BAR0 subprocess from Qt construction

The first attempt to force `preproc_mode=0` at every Qt launch called
`fpga_preproc_ctrl.sh` from the `MainWindow` constructor. On the board this
version exited immediately with a segmentation fault, while the control script
worked correctly when run by itself. The early subprocess call was removed.
The UI still initializes its FPGA mode selector to bypass, and explicit
parameter application continues to use the script after the application is
fully running.

This is RK-side only and requires cross-compilation/deployment; no FPGA rebuild
is required.
