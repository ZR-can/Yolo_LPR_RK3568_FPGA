
# 3_NPU_Yolov8_LPR_Demo用法

## 当前状态

当前工程保留 MPP 视频 demo，并新增独立的 `yolov8_lpr_pcie_demo` 首版代码。PCIe 版本面向 1280×720、每像素 2 字节的小端 BGR565 帧，移除了 MPP/H.264 解码依赖；当前仅完成 Windows 工作区编码与静态检查，尚未在 Ubuntu 交叉编译、推送或 RK3568 板端验证。
`pango_pci_driver.ko` 已随 PCIe demo 安装，但 FPGA 侧视频源仍属于外部前置条件；本工程没有把 FPGA 侧 PCIe 预处理描述为已经集成完成。当前模块的 vermagic 为 `6.1.99 SMP mod_unload aarch64`，板端内核必须兼容。

## 开发记录

### 2026-07-20 PP-OCRv4 FP16 独立板端验证入口

- 新增独立 `ppocr_rec_demo`，使用 `main_ppocr.cc`、`ppocr_rec.cc` 和运行时 UTF-8 字典，不改动现有 YOLO/LPRNet 图片、视频及 PCIe pipeline。
- 初始化阶段严格检查 RKNN 为单输入单输出、输入 `[1,3,48,160]`、输出 `[1,20,74]`，并检查外部字典恰好为 73 个非空且不重复字符；CTC blank 固定为类别 0，字符类别按 `index-1` 查询字典。
- `read_image()` 输出的 RGB888 会先转为 BGR，再按高度 48 等比例缩放并在右侧填充原始像素值 128 到宽度 160；RKNN 输入使用 UINT8 NHWC，依赖转换模型内嵌的 `(x-127.5)/127.5` 归一化。
- demo 支持可选 warmup 和重复次数，分别统计预处理、输入设置、NPU run、输出获取、CTC 解码及端到端平均耗时，并打印 RKNN API/驱动版本和输入输出 tensor 属性。
- CMake 为 PP-OCR 建立独立安装目录，只复制 `ppocrv4_rec14_rk3568_fp16.rknn`、`cblprd_plate_dict.txt` 和 `test_ppocr.jpg`；`build-linux.sh` 增加相应安装产物检查。
- 使用相同 BGR、缩放、右补 128 和归一化流程运行 `ppocrv4_rec14.onnx`，`test_ppocr.jpg` 的 PC 基线结果为 `陕HR7YKA`、平均字符置信度 `0.993716`；该结果仅作为板端 FP16 RKNN 对照，不代表板端已经验证。
- RK3568 首次板端诊断已完成：RKNN Runtime/Toolkit 均为 2.3.0、驱动为 0.9.8，运行时输入属性为等价的 NHWC `[1,48,160,3]`、输出为 `[1,20,74]`，识别结果为 `陕HR7YKA`、平均字符置信度 `0.993373`，与 ONNX 基线一致且日志中没有 RKNN error/warning。该次使用 `RKNN_LOG_LEVEL=4` 且只执行 1 次，`rknn_run=40.211 ms`、端到端 `42.666 ms` 仅用于逐层诊断，正式性能仍需在日志等级 0、定频、warmup 后重复测试。
- 关闭详细 profiling 后，以 10 次 warmup、100 次统计运行得到平均 `rknn_run=19.486 ms`、端到端 `20.785 ms`，对应同步单帧倒数吞吐率 `51.32 FPS` 和端到端 `48.11 FPS`。另一次 `RKNN_LOG_LEVEL=1`、无 warmup 的单次运行中 `rknn_run=19.506 ms`，与正式平均值仅差 `0.020 ms`；其端到端 `21.967 ms` 主要由首轮预处理和输入转换冷启动造成。
- 性能口径进一步改为在 `rknn_outputs_get()` 后查询 `RKNN_QUERY_PERF_RUN`：官方 `run_duration` 作为主要 RKNN 推理时间和吞吐率，同时保留 `rknn_run()` 墙钟时间用于观察 Runtime/驱动开销；性能查询接口自身耗时不计入端到端时间。
- `RKNN_QUERY_PERF_RUN` 板端 A/B 验证表明：日志等级 0 时官方推理时间/墙钟时间为 `19.783/19.799 ms`，日志等级 4 时为 `35.663/35.679 ms`，两种模式均只差 `0.016 ms`。因此 `run_duration` 表示当前 Runtime 模式下的真实同步推理时间，会包含详细 profiling 对执行过程造成的开销；正式性能只能采用日志等级 0、定频且 warmup 后的结果，日志等级 4 仅用于逐层分析。
- 批量评测首图是有效的 128x48 baseline JPEG，单图 `read_image()` 能成功解码并完成 INT8 推理，但板端 OpenCV 3.4.5 `cv::imread()` 返回空图。`main_ppocr_eval.cc` 已改为直接复用 `read_image()` 产生的 RGB888 `image_buffer_t`，并在 warmup、成功推理和错误退出路径释放图像内存；评测脚本会过滤 `read_image()` 每张 JPEG 的尺寸诊断行，保留进度和汇总结果。该修复已通过 Windows 编译级静态检查，仍需重新交叉编译后在板端复测批量评估。
- 新增 `ppocr_rec_eval_demo` 和 `eval_ppocr_rknn_subsets.sh` 板端数据集评估入口。脚本沿用 CBLPRD 的 basic、hard、使、学、港、澳、警、领八份验证清单；模型和字典在单一进程内只初始化一次，统一 warmup 后逐图执行 RKNN 推理，输出各 subset/总计的原始 CTC 整串准确率、官方 RKNN/端到端平均时间以及逐图 UTF-8 TSV。规则修正仍保留在 PC 端 `plate_rule.py`，避免形成两套实现口径。
- 三个模型已在相同 17,357 张 CBLPRD 验证集完成板端对照：基线 FP16 为 88.1777% / 19.948465 ms，折叠 LearnableAffine FP16 为 88.1489% / 17.370938 ms，普通 INT8 为 71.7059% / 9.854101 ms。折叠 FP16 仅净少正确 5 张且官方推理延迟降低 12.92%，作为当前优先 FP16 候选；INT8 虽约 2.02 倍速，但下降 16.4718 个百分点，不满足最终部署精度要求。
- 折叠模型的 `mmse + channel` 全 INT8 与三组手工混合量化已完成相同口径板端验证：全 INT8、H1、H2、H3 分别为 82.9925% / 8.488004 ms、83.8394% / 8.899664 ms、84.2081% / 9.022712 ms、83.4361% / 8.873892 ms。H2（`Add.27 + hardswish_4.tmp_0` 为 FP16）是当前优先量化候选，相对全 INT8 提升 1.2156 个百分点并增加 6.30% 官方推理延迟；相对折叠 FP16 仍低 3.9408 个百分点，但官方推理快 1.925 倍。单张 `accuracy_analysis` 的最终余弦排序与完整板端准确率不一致，后续混合层只能由多张真实错误样本和完整板端 CTC 结果共同决定。

### 2026-07-15 PCIe 720p BGR565 首版

- 新增 `yolov8_lpr_pcie_demo`、独立 `PcieFrameSource` 和 Pango 驱动 ABI 头文件。初始化顺序沿用已验证参考实现：读取设备信息、配置 DMA、查询并映射 BAR0、建立 DMA 映射，再写入 `0xffffffe5` 启动采集；退出时写入 `0xffffff00` 并释放映射。
- Pango 驱动的 `read()` 返回值按驱动状态码解释：正值（板端已观测成功值为 `2`）表示映射缓冲区中已有一帧完整 DMA 图像，不与 `1280*720*2=1843200` 比较；`0`、`EINTR`、`EAGAIN` 和参考实现可能出现的 `EPERM` 进入短暂重试，其他错误才终止采集。
- BGR565 原图通过 6 槽固定帧池在采集、显示和推理线程间共享。显示队列容量为 2，推理待处理队列容量为 1，队列满时替换旧帧；默认每 2 帧提交一次推理，避免显示或 NPU 反压阻塞 PCIe 读取。
- YOLO 路径直接调用现有 `process_pipeline()`：RGA 在 `convert_image_with_letterbox()` 中完成 BGR565 1280×720 到 RGB888 640×640 的格式转换、等比例缩放和上下填充，检测框由后处理映射回 1280×720 原图坐标。
- LPR 路径不生成整帧 RGB888：先在 BGR565 原图上裁剪检测 ROI，再用 OpenCV `COLOR_BGR5652BGR` 转为 BGR、缩放到 94×24，保持现有 LPRNet 输入字节语义。原 Tracker 接收原图坐标，显示时再按 DRM mode 映射坐标。
- PCIe 显示复用现有 DRM UI 双缓冲：RGA 将 BGR565 原图直接转换到 RGBA UI buffer，随后在同一 buffer 上叠加检测框和标签，再执行 UI-only atomic commit。首版尚待 Ubuntu 交叉编译和板端确认颜色顺序、画面完整性、识别坐标、退出回收及性能统计。
- CMake 安装布局会生成独立的 `yolov8_lpr_pcie_demo/` 目录并复制模型；`build-linux.sh` 在安装后同时检查 PCIe 可执行文件是否存在。
- 修复 LPRNet CTC 后处理的字符置信度索引：车牌字符继续使用类别 ID 查询字典，概率改为按解码后的字符位置同步读取，避免把类别 ID 当作概率数组下标造成越界。
- 将工程根目录的 `pango_pci_driver.ko` 安装到 `yolov8_lpr_pcie_demo/`，并在 `build-linux.sh` 中检查驱动模块安装产物。该预编译模块仅确认适配 `6.1.99` aarch64 内核，加载前必须核对板端 `uname -r`。
- 修复 PCIe 运行期间 `Ctrl+C` 可能无法中断主线程驱动读取的问题：创建推理和显示线程前临时屏蔽 `SIGINT/SIGTERM`，工作线程继承屏蔽状态后仅在主采集线程解除屏蔽，使信号能够中断其阻塞 `read()`；信号到达后先输出 `PCIe: stop requested`，随后沿用停止采集、DMA unmap、线程 join 和性能统计的正常回收路径。
- 板端现象“启动时全屏、运行中突然缩至左上角约三分之二区域”确认是 CRTC 从 1280×720 被桌面显示服务重新切回 1920×1080，而 demo 后续只更新 1280×720 plane。DRM 初始化现在必须取得独占 master；若桌面仍占用 KMS，会明确报错并拒绝启动，避免两个显示主体中途竞争。
- CRTC 固定使用已连接显示器 EDID 标记的 preferred/native mode；PCIe UI 双缓冲继续保持 1280×720，RGA 只执行 BGR565→RGBA 转换和 720p overlay，不再生成 1080p/4K framebuffer。atomic plane 使用 1280×720 source、native CRTC destination，由 RK3568 VOP 完成全屏缩放，并在启动时打印 UI framebuffer、实际 mode 和 `VOP plane scaling`。该方案不增加每帧 RGA/CPU 像素处理量，仍需 Ubuntu 交叉编译及 RK3568 板端复测。
- PCIe 显示线程连续 3 次 atomic commit 失败时会向主采集线程发送 `SIGTERM`，沿用正常的 PCIe 停止与 DMA 回收流程退出；不再在 DRM master 丢失或显示链路失效后继续无画面识别。
- 首次板端复测在 `/dev/dri/card0` 和 `card1` 均得到 `Device or resource busy`，说明桌面显示进程仍持有两个 DRM card；demo 按设计在 PCIe 初始化前终止，没有发生 DMA 资源泄漏。不能删除 master 检查，应通过 `fuser` 或 debugfs `clients` 找到实际 master 所属进程，并停止对应 systemd service，防止服务自动拉起后再次竞争。

### 2026-07-12 车牌文字显示校验

- 视频路径的车牌投票校验已补全为逐位规则：普通牌为“省份 + 字母 + 字母/数字”，末位保留既有的“警、学、港、澳、领、使”特殊尾标；绿牌为“省份 + 字母 + 6 位字母/数字”，且第 3 位或末位必须为 `D/F`。因此 `津E黑S使云2` 等含非法中文字符的结果不会进入投票池。
- 跟踪目标未得到有效车牌投票时，RGA 仍绘制检测框，但跳过标签精灵，DRM UI plane 不再显示 `:`、`-` 或原始错误车牌文字。该逻辑尚待 Ubuntu 交叉编译和 RK3568 板端复测确认。

### 2026-07-10 绘制性能回退排查

- 用户反馈三层定位框、半透明圆角文字底板和加粗文字启用后帧率显著下降。
- 静态排查确认：该 overlay 在 DRM 映射帧缓冲上执行大量 CPU 读-改-写。半透明圆角底板逐像素做 alpha 混合，三层框增加描边写入；两段文字各执行三次栅格化，且每次均重新缩放字模并申请释放临时内存。
- 优先恢复单层不透明边框和单次文字栅格化，以现有 `Avg UI Drawing` 与 `System Throughput` 为基线验证；视觉样式需要保留时，再将叠加层改为预生成小型 RGBA overlay 后由 RGA 合成，避免 CPU 直接对 DRM 帧缓冲做 alpha 混合。
- GPU 可参与 UI 渲染，但首选架构是 MPP NV12 DMA-BUF 直接进入 DRM 视频 plane，Mali GPU 通过 GBM + EGL + GLES 渲染 ARGB UI plane，DRM atomic KMS 以 zpos 和 alpha 合成两个 plane。这样 CPU 仅更新目标几何和文本状态，避免 GPU 合成整帧视频。
- 该路线需先在板端确认 atomic plane 同时支持 NV12 和 ARGB8888，以及 `zpos`、`alpha`、fence 属性；MPP 的输出 buffer 必须在对应 DRM page-flip 完成前保持引用，不能在 decoder callback 返回并执行 `mpp_frame_deinit()` 后继续无保护地 scanout。
- 对当前少量车牌框与标签的 UI，RGA 是优先方案：复用已有 DMA-BUF fd 导入能力，以缓存的 ARGB 字形、圆角面板和边框位图作为源，由 RGA 完成填充与 alpha 合成。GPU 仅在后续需要动画、大量动态矢量对象或复杂特效时引入；无论采用 RGA 还是 GPU，最终都应由 DRM 双 plane 合成，避免每帧生成完整的 CPU 可见视频帧。
- 若后续扩展为左侧识别结果、上方状态栏、下方模式选择和右侧视频的交互式界面，渲染职责按 plane 划分：MPP/RGA 只提供视频 plane，Mali GPU 统一渲染整张透明 UI plane（包括视频上的检测框和标签），DRM 完成两层合成。不要让 RGA 与 GPU 分别写相邻或重叠的 UI 区域，以免增加额外 buffer、合成和 fence 同步复杂度。
- 已完成 RGA-only 双 plane 代码路径：视频 target 的 `main()` 位于 `src/main_video.cc`，MPP NV12 DMA-BUF 由 DRM atomic KMS 导入底层 video plane；两个不映射到 CPU 的 ABGR UI buffer 由 RGA 轮换写入上层 plane。
- 新增 `RgaOverlayRenderer`：每帧 RGA 清空透明 UI buffer 并绘制三层框；车牌、类型和置信度构成缓存键，缓存失效时才在普通内存生成小型直 alpha 圆角标签，随后通过位置化 RGA alpha blend 写入 UI DMA-BUF。CPU 不再访问 DRM 帧缓冲的像素。
- MPP decoder callback 现为显示持有额外 `MppBuffer` 引用，当前 scanout 帧在下一次 atomic commit 成功替换后才释放。`save_interval` 在 dual-plane 模式暂时禁用，避免导出不完整的单 plane 图像。
- RK3568 板端已完成一次视频复测：MPP 输出与显示均为 1600 帧、无显示丢帧和 overlay 失败，显示吞吐为 30.07 FPS，NPU 推理为 15.03 FPS。`Avg Decode Call` 为 33.16 ms，其中 32.21 ms 为按 30 FPS 的主动节流；MPP put/get 调用分别为 0.02/0.03 ms，未观察到解码背压。
- 后续连续运行复测出现 4.08 FPS：`MPP Input Retries` 与 `MPP Buffer Full` 同为 294215，平均重试等待 195.85 ms、连续重试峰值 894；MPP 输入/输出错误和 1 秒无进展 stall 均为零。同期 NPU 推理升至 145.72 ms，而 RGA/UI/DRM 仍约为 0.76/4.56/9.96 ms。该现象表明 VPU 在持续取得少量输出但长期受输入背压限制，优先排查残留进程、DDR/VPU/NPU DVFS、温度和内核日志，不应归因于 overlay 绘制。
- 已发现并修复 `MppDecoder::Init()` 的 context 所有权错误：局部变量曾遮蔽类成员 `mpp_ctx`，导致析构函数无法调用 `mpp_destroy()`，`Reset()` 也会向空 context 发命令。该缺口会使多次运行后的 MPP/VPU 资源回收不可靠，与“重启后恢复、连续运行退化”的现象直接相关。现在 context 由类成员持有，初始化失败路径和主函数初始化失败路径均会释放已分配资源。
- 修复后一次重启复测曾在输出 3 帧后遇到 `MPP_ERR_BUFFER_FULL` 持续约 1 秒，旧 watchdog 主动返回 `MPP_ERR_TIMEOUT` 并退出；当时 MPP 输入/输出错误均为零，说明是 watchdog 阈值而非 MPP 致命返回。现已改为 1 秒记录 `MPP Input Stalls` 告警、连续 5 秒无输出才记录 `MPP Input Aborts` 并退出，避免把短暂 VPU 启动背压误判为故障。
- 上述启动背压期间的板端状态为：CPU 51.25°C、CPU 1416 MHz、NPU 负载 3%、可用内存约 1399 MB；DMC 工具报告值为 1560（标签显示 GHz，实际单位需在板端确认）。未见 CPU 温度、NPU 负载或内存压力异常，后续应优先采集 `dmesg` 中 rkvdec/VPU/IOMMU/DMC 相关日志，并检查 raw H.264 parser 状态。
- `dmesg` 已确认启动背压的根因在板端电源管理路径：`mpp_rkvdec2 fdf80200.rkvdec: Cannot set voltage 875000 uV` 与 `rk3x-i2c fdd40000.i2c: timeout` 同时出现；随后 CPU 的 `_set_opp_voltage` 和 `cpufreq` 也以 `-110` 失败。VPU 与 CPU 均无法通过 PMIC I2C 切换 OPP 电压，导致 rkvdec 无法正常提升/切换性能状态，从而持续出现 MPP `BUFFER_FULL`。该问题不属于 demo 用户态代码，需要检查板级供电、PMIC/I2C 总线、内核 DTS regulator/OPP 配置及对应内核驱动。
- 在“放置久后慢帧”的同一时间点再次捕获 `rk3x-i2c fdd40000.i2c: timeout`，紧随其后 CPU `_set_opp_voltage`、regulator 与 `cpufreq` 均以 `-110` 失败，随后 `mpp_rkvdec2` 发起 900000 uV 调压请求。`set voltage` 日志表示请求而非成功确认；结合前后的 I2C timeout 和 MPP `BUFFER_FULL`，可确认空闲后的慢帧与 PMIC I2C/OPP 调压故障直接相关。
- 用户态 VPU 预检已验证可用：将 `fdf80200.rkvdec` runtime-PM 设为 `on` 后无新增调压/I2C 错误；将 devfreq governor 从 `vdec2_ondemand` 切换为 `performance` 后，频率由 297000000 升至 400000000，仍无错误。该设置可作为空闲期临时规避方案，后续需在保持 `power/control=on` 与 `governor=performance` 时完成长时间空闲后的 demo 复测；设置仅在当前开机周期有效。
- 已对照 `Rockchip_Developer_Guide_MPP_CN.pdf` 第 19-25 页：固定长度裸码流读取属于内部分帧，必须在 `mpp_init()` 前启用 `MPP_DEC_SET_PARSER_SPLIT_MODE`；当前代码符合该要求。指南说明非阻塞 `decode_put_packet()` 在内部队列满时应等待重试，默认约可排队 4 个输入包；当前代码的 retry 行为符合规范。指南同时指出内部分帧效率低于外部按完整帧提交，后续可用 Annex-B access-unit 分帧降低 parser/输入队列压力，但无法修复 VPU OPP 调压失败。
- 未修改内核的临时运行适配：新增 `vpu_preflight.sh`，随 video demo 一同安装。它以 root 身份将 `rkvdec` runtime-PM 固定为 `on`、devfreq 固定为 `performance`，等待 2 秒后确认 `active`、400 MHz 且预检期间无新增 PMIC/I2C/OPP 错误；通过时只提示可以启动 demo，并在 demo 退出后保持该锁定至本次开机结束。它不能修复 I2C 调压超时本身，预检失败时不应启动 demo。
- 修复后重启板端首次复测为 30.03 FPS：MPP 输出/显示均为 1600 帧、无显示丢帧和 MPP 输入/输出错误。仅出现 6 次短暂 `MPP_ERR_BUFFER_FULL`，无实际退避等待、无 stall；NPU 36.50 ms、RGA/UI/DRM 为 0.60/4.63/8.69 ms，恢复到正常基线。仍需在不重启的前提下连续多次运行验证 context 回收是否彻底消除退化。
- 未重启条件下第二次连续复测为 30.09 FPS：10 次短暂 `MPP_ERR_BUFFER_FULL`、连续重试峰值 3、无退避等待/stall/MPP 错误；NPU 36.60 ms、RGA/UI/DRM 为 0.61/4.71/8.70 ms。首轮与第二轮无性能退化，表明 MPP context 与显示 buffer 回收已在连续运行间生效。
- 未重启条件下第三次连续复测为 30.08 FPS：13 次短暂 `MPP_ERR_BUFFER_FULL`、连续重试峰值 4、无退避等待/stall/MPP 错误；NPU 36.67 ms、RGA/UI/DRM 为 0.60/4.71/8.26 ms。输入重试计数在每个进程启动时清零，6/10/13 是各次独立运行的瞬时非阻塞队列满次数，并非跨运行累积；三次均保持 30 FPS，完成连续运行稳定性验证。
- 修复 Ubuntu GCC 6.3 构建兼容：图片 demo 不再访问已移除的 CPU 映射 framebuffer；`InferJob` 使用显式构造函数入队，避免旧编译器拒绝花括号初始化。等待 Ubuntu 重新编译。
- 修复 RGA overlay 链接冲突：`plate_font.h` 的字体位图改为仅由 `image_drawing.c` 定义，RGA 渲染模块仅引用该唯一实例，避免 `plate_font_data` 在两个 target 中重复定义。
- 根据板端 `2.43 FPS` 数据修复显示反压：原 `Avg MPP Decode` 计时覆盖整个同步 `decoder->Decode()` 调用，并非纯硬件解码；RGA UI 与 DRM atomic commit 曾直接在该回调中执行。
- 视频显示改为独立线程和容量为 2 的最新帧队列。解码回调只完成 NPU 预处理并移交持有引用的 MPP frame；队列满时丢弃旧帧并立即归还其 `MppBuffer`，避免显示阻塞 MPP 输出。
- DRM 现在按 MPP DMA-BUF fd 与帧布局缓存 GEM handle / framebuffer，消除每帧 PRIME import、`ADDFB2`、`RMFB` 与 `GEM_CLOSE`；分辨率或格式变化后的旧缓存仅在新 atomic commit 成功后清理。
- 默认关闭 MPP 每帧日志，性能统计改为分别报告 MPP 输出、实际送显、显示丢帧和 NPU 任务；尚待 Ubuntu 交叉编译与 RK3568 板端复测。
- 修复 RK3568 RGA overlay 的 `RGA_COLORFILL fail: Invalid argument`：旧 `imrectangle` 会把细边转换为如 `2x6` 的独立 color-fill 目标。现在以完整 UI DMA-BUF 为目标，通过四个裁剪 `imfill` 区域绘制框线，并显式使用 `wrapbuffer_fd_t` 传入完整宽高和 stride。
- 标签面板现按车牌框水平中心对齐，优先完整放置在框上方；上方空间不足时自动切换到框下方。面板坐标始终夹紧至 UI 边界，缓存生成时会按显示分辨率缩小字号，RGA blend 也保留最终裁剪保护，避免文字框越界或被裁切。
- 视频编译入口已迁回 `src/main_video.cc`；`src/main.cc` 保留为空文件且不参与 video target 编译，避免双 main 或额外跳转接口。
- 当前工作区中的 `build_and_push.sh` 已被删除；恢复该脚本前不能使用 README 中的一键构建推送流程。
- 视频性能摘要包含 MPP callback、put/get-frame、节流睡眠和 buffer-group 峰值统计；单个 RGA 框或标签操作失败只跳过该对象，避免逐帧错误刷屏拖慢解码。
- 当前本地版本使用 512 KiB 裸码流读取块，并显式将 MPP 输入和输出配置为非阻塞模式。`decode_put_packet()` 仅对 `MPP_ERR_BUFFER_FULL` 和输入超时重试；无输出进展时以 1 ms 退避，单个输入包持续 1 秒无进展仅记录背压告警，连续 5 秒无进展才停止解码并返回错误。EOS 包会等待最终输出帧，等待超过 1 秒同样作为错误返回。其他输入错误立即终止当前解码，主函数以非零状态退出，不对背压状态盲目 reset 解码器。
- 裸码流读取结束后始终单独提交零长度 EOS 包，不再依赖最后一次 `fread()` 是否触发 `feof()`；这保证文件大小恰好为读取块整数倍时也能完成 VPU drain 和 MPP 资源收尾。
- 性能摘要现记录输入重试次数、buffer-full/输入超时分类、实际退避等待、连续重试峰值、输入背压告警、输入中止和输入/输出侧错误数。这样可区分正常短暂背压、输出 buffer 未归还、VPU/DDR 降速及不可恢复的 MPP 错误。
- MPP 初始化现在在 `mpp_init()` 前显式启用 raw-stream split parser 和 parser fast mode，符合本地 MPP API 的时序要求；若板端仍存在高频输入重试，需要结合 DDR/VPU 频率、温度和 `dmesg` 排查硬件资源状态。

### 2026-07-07 双缓冲与绘制修复

- 本地工作区已从 GitHub `main` 回退后重新开始修复，并新建 `codex/fix-buffer-overlay` 分支；本地 `codex/pcie` 已删除，`git ls-remote --heads origin codex/pcie` 未发现远端同名 head。
- 修复视频异步推理调度：将 `g_infer` 从单槽 ready 状态改为任务队列，避免新帧覆盖尚未被推理线程消费的输入任务。
- 修复 `yolo_input_busy[]` 线程数据竞争：缓冲预约、失败释放、推理完成释放均集中到同一把互斥锁保护的 helper 中。
- 修复 `process_pipeline_preprocessed()` 与双缓冲潜在不一致：预处理 pipeline 现在显式接收当前 RKNN 输入 buffer 对应的 `image_buffer_t`，LPR 裁剪不再固定读取 `input_mems[0]`。
- 修复第二块 YOLO 输入缓冲释放顺序：备用 `rknn_tensor_mem` 在 `release_pipeline()` 销毁 RKNN context 之前释放，释放前先把 RKNN 输入重新绑定回主 buffer。
- 更新车牌 overlay：统一使用洋红色三层定位框、深黑灰半透明圆角文字底板、白色粗体固定字号单行文字，并限制到画面边界内。

## 文件构成介绍

```shell

```

## Linux环境搭建

1. 首先安装虚拟机，Ubuntu20即可，建议分配至少20g，内存6g，处理器2个每个4核，可根据自己电脑性能调整
2. 共享文件夹，将3_NPU_Yolov8_LPR_Demo文件夹共享给虚拟机，共享后文件一般放在在虚拟机的目录：/mnt/hgfs/Yolo_LPR_RK3568_FPGA_Project/3_NPU_Yolov8_LPR_Demo下，其后运行时每次从这里打开终端即可

## 交叉编译环境配置
   目标设备是`Linux`系统时，使用根目录下的`build-linux.sh`脚本编译具体模型的 C/C++ Demo。  
使用该脚本编译C/C++ Demo前需要先下载交叉编译工具，并通过环境变量`GCC_COMPILER`指定交叉编译工具的路径。

1. 不同的系统架构，依赖不同的交叉编译工具。下面给出具体系统架构建议使用的交叉编译工具下载链接：
   - aarch64: https://releases.linaro.org/components/toolchain/binaries/6.3-2017.05/aarch64-linux-gnu/gcc-linaro-6.3.1-2017.05-x86_64_aarch64-linux-gnu.tar.xz
2. 在虚拟机解压缩下载好的交叉编译工具，记住具体的路径，后面在编译时会用到该路径。  
   **这里不要解压到共享文件夹，需要解压到自己的home/目录下，例如我的/home/zr/gcc-linaro-6.3.1-2017.05-x86_64_aarch64-linux-gnu**
3. 每次进行编译前需要运行：export GCC_COMPILER=<GCC_COMPILER_PATH>   
**这里可以将路径写入Linux的环境变量，这样以后每次编译则不用再运行export GCC_COMPILER=<GCC_COMPILER_PATH>命令，例如我的写入/home/zr/gcc-linaro-6.3.1-2017.05-x86_64_aarch64-linux-gnu/bin/aarch64-linux-gnu**

## Compile and Build
以下的命令均以Linux的为例，可直接复制使用。 
建议开两个终端，一个主终端：在/mnt/hgfs/Yolo_LPR_RK3568_FPGA_Project/3_NPU_Yolov8_LPR_Demo交叉编译用，一个从终端：专门登入板端
```shell
cd /mnt/hgfs/Yolo_LPR_RK3568_FPGA_Project/3_NPU_Yolov8_LPR_Demo
export GCC_COMPILER=<GCC_COMPILER_PATH> #配置好后可省略
./build-linux.sh -t rk3568 -a aarch64 -d yolov8_lpr
```

/bin/bash^M 表示 build-linux.sh 被保存为 Windows 的 CRLF 换行。
在 Ubuntu 共享目录执行：
```shell
sed -i 's/\r$//' build-linux.sh
chmod +x build-linux.sh
./build-linux.sh -t rk3568 -a aarch64 -d yolov8_lpr
```

## Push demo files to device

```shell
#主终端使用
#删除原有旧demo
adb shell rm -rf /userdata/rknn_yolov8_lpr_demo
#推送
adb push install/rk356x_linux_aarch64/rknn_yolov8_lpr_demo /userdata/

adb push install/rk356x_linux_aarch64/rknn_yolov8_lpr_demo/yolov8_lpr_picture_demo /userdata/rknn_yolov8_lpr_demo
adb push install/rk356x_linux_aarch64/rknn_yolov8_lpr_demo/yolov8_lpr_video_demo /userdata/rknn_yolov8_lpr_demo
adb push install/rk356x_linux_aarch64/rknn_yolov8_lpr_demo/yolov8_lpr_pcie_demo /userdata/rknn_yolov8_lpr_demo

adb push model/testX.jpg /userdata/rknn_yolov8_lpr_demo/yolov8_lpr_picture_demo/test
adb push model/testvideoX.h264 /userdata/rknn_yolov8_lpr_demo/yolov8_lpr_video_demo/test
```

## Run demo and pull result

```shell
# 切到命令行模式，立刻关闭3568桌面
sudo systemctl isolate multi-user.target
# 直接使用 DRM/KMS 的 demo 必须在桌面服务停止后运行；否则 DRM master 获取会失败并拒绝启动
systemctl is-active display-manager 2>/dev/null || true

# 若仍报告 Device or resource busy，定位两个 card 的实际占用者
sudo fuser -v /dev/dri/card0 /dev/dri/card1 2>/dev/null || true
sudo mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null || true
for clients in /sys/kernel/debug/dri/*/clients; do
    [ -r "$clients" ] || continue
    echo "=== $clients ==="
    cat "$clients"
done
# 使用 ps -fp <PID> 和 systemctl status <PID> 确认所属服务，再用 systemctl stop <实际服务名> 停止；不要只 kill 会被自动拉起的进程

# 恢复桌面
sudo systemctl set-default graphical.target
sudo reboot
```

```shell
#从终端使用
#登入板端
adb shell 
chmod +x /userdata/rknn_yolov8_lpr_demo/yolov8_lpr_picture_demo/yolov8_lpr_picture_demo
chmod +x /userdata/rknn_yolov8_lpr_demo/yolov8_lpr_video_demo/yolov8_lpr_video_demo
chmod +x /userdata/rknn_yolov8_lpr_demo/yolov8_lpr_pcie_demo/yolov8_lpr_pcie_demo
# export LD_LIBRARY_PATH=./lib
```

```shell
#推理单图片
cd /userdata/rknn_yolov8_lpr_demo/yolov8_lpr_picture_demo
./yolov8_lpr_picture_demo ./model/yolov8.rknn ./model/lprnet7repair_fp.rknn ./model/lprnet8repair_fp.rknn ./test/test1.jpg
for img in ./test/test{1..4}.jpg; do ./yolov8_lpr_picture_demo ./model/yolov8.rknn ./model/lprnet7repair_fp.rknn ./model/lprnet8repair_fp.rknn $img; done
```

```shell
#图片推理性能耗时评估

#从终端使用
export RKNN_LOG_LEVEL=4
export RKNN_LOG_LEVEL=0
#以下命令使用不同的main记得更改生成的.log日志文件名，不同的日志结果分开存放并注明测试的内容
#使用test1.jpg存在一张图多车牌，使用test2.jpg为单图绿牌，根据要测试的内容选择不同的test.jpg
cd /userdata/rknn_yolov8_lpr_demo/yolov8_lpr_picture_demo
./yolov8_lpr_picture_demo ./model/yolov8.rknn ./model/lprnet7repair_i8.rknn ./model/lprnet8repair_i8.rknn ./test/test2.jpg > rknn_perf2repair_i8.log 2>&1
#主终端使用
adb pull /userdata/rknn_yolov8_lpr_demo/yolov8_lpr_picture_demo/result ./result/picture ; adb pull /userdata/rknn_yolov8_lpr_demo/yolov8_lpr_picture_demo/rknn_perf2repair_i8.log ./result/log
```

```shell
#推理视频
cd /userdata/rknn_yolov8_lpr_demo/yolov8_lpr_video_demo
./vpu_preflight.sh
#板端 PMIC/I2C 电源管理异常,看到"VPU preflight passed"后,再手动运行yolov8_lpr_video_demo
./yolov8_lpr_video_demo ./model/yolov8.rknn ./model/lprnet7repair_i8.rknn ./model/lprnet8repair_i8.rknn ./test/testvideo1.h264 0
```

```shell
#pcie_demo
1. 检查内核版本和 PCIe 枚举/链路状态。`modinfo` 不存在时，以本文记录的 vermagic 和 `uname -r` 人工比对：

uname -a
uname -r
if command -v modinfo >/dev/null 2>&1; then
    modinfo ./pango_pci_driver.ko | grep -E 'name|vermagic'
fi

if command -v lspci >/dev/null 2>&1; then
    lspci -nn
    lspci -vv | grep -E '^[0-9a-fA-F]+:|LnkCap:|LnkSta:'
fi

# 板端没有 lspci 时使用 sysfs 检查枚举结果
for dev in /sys/bus/pci/devices/*; do
    [ -f "$dev/vendor" ] || continue
    echo "$(basename "$dev") vendor=$(cat "$dev/vendor") device=$(cat "$dev/device")"
    [ -f "$dev/current_link_speed" ] && echo "  speed=$(cat "$dev/current_link_speed") width=$(cat "$dev/current_link_width")"
done

如果 `lspci` 和 `/sys/bus/pci/devices/` 都没有显示 FPGA Endpoint，应先检查 FPGA 上电、PCIe 参考时钟、复位、RK3568 Root Complex 配置和物理链路，不要继续运行 demo。

2. 卸载可能残留的旧模块，再加载随 demo 安装的模块：

cd /userdata/rknn_yolov8_lpr_demo/yolov8_lpr_pcie_demo
if lsmod | grep -q '^pango_pci_driver '; then
    rmmod pango_pci_driver
fi
insmod ./pango_pci_driver.ko

lsmod | grep pango_pci_driver
ls -l /dev/pango_pci_driver
dmesg | tail -n 80 | grep -Ei 'pango|pci|bar|dma|error|fail'

只有模块出现在 `lsmod`、`/dev/pango_pci_driver` 存在且 `dmesg` 无 probe/BAR/DMA 致命错误时，才继续运行。若 `insmod` 返回 `Invalid module format`，应检查 `uname -r`、模块 vermagic 和 `dmesg`，不能使用 `-f` 强制加载。

3. 启动 PCIe demo：

cd /userdata/rknn_yolov8_lpr_demo/yolov8_lpr_pcie_demo
./yolov8_lpr_pcie_demo ./model/yolov8.rknn ./model/lprnet7repair_fp.rknn ./model/lprnet8repair_fp.rknn

正常启动应依次看到 PCIe vendor/device、Link Gen/Width、MPS、BGR565 1280×720 采集启动信息。首次成功读取通常会输出 `read returned driver status 2`；这里的 `2` 是驱动成功状态码，表示 DMA 缓冲区中已有一帧完整的 1843200 字节图像。

4. 使用 `Ctrl+C` 退出。终端应先打印 `PCIe: stop requested`，随后打印 PCIe Pipeline Statistics；同时确认启动日志类似 `DRM: UI framebuffer 1280x720 -> display mode 1920x1080 (preferred), VOP plane scaling`，画面持续覆盖整个显示区域，且退出时没有 DMA unmap 错误。若出现 `cannot acquire master`，必须先停止桌面显示服务，不能让 demo 与桌面同时控制 KMS：

dmesg | tail -n 80 | grep -Ei 'pango|pci|bar|dma|error|fail'
```

```shell
#退出板端终端命令为logout

#主终端使用
#拉取结果
adb pull /userdata/rknn_yolov8_lpr_demo/yolov8_lpr_picture_demo/result ./result/picture
adb pull /userdata/rknn_yolov8_lpr_demo/yolov8_lpr_video_demo/result ./result/video
```

## 性能监控
```shell
cat /sys/devices/system/cpu/cpufreq/policy0/scaling_available_frequencies
cat /sys/class/devfreq/fde40000.npu/available_frequencies
cat /sys/class/devfreq/dmc/available_frequencies
```

```shell
echo "=== CPU 当前频率 ==="
cat /sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq

echo -e "\n=== NPU 当前频率 ==="
cat /sys/class/devfreq/fde40000.npu/cur_freq

echo -e "\n=== DDR 当前频率 ==="
cat /sys/class/devfreq/dmc/cur_freq
```

```shell
# 锁CPU最高
echo performance > /sys/devices/system/cpu/cpufreq/policy0/scaling_governor

# 锁NPU 600MHz
echo userspace > /sys/class/devfreq/fde40000.npu/governor
echo 600000000 > /sys/class/devfreq/fde40000.npu/userspace/set_freq

# 锁DDR 1560MHz
echo userspace > /sys/class/devfreq/dmc/governor
echo 1560000000 > /sys/class/devfreq/dmc/userspace/set_freq

# 查看结果
echo "=== 已锁满性能 ==="
echo "CPU:" $(cat /sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq)
echo "NPU:" $(cat /sys/class/devfreq/fde40000.npu/cur_freq)
echo "DDR:" $(cat /sys/class/devfreq/dmc/cur_freq)

# 性能模式
echo performance | tee $(find /sys/ -name *governor) /dev/null || true
```

```shell
watch -n 1 "
echo '--- [系统时间 & 负载] ---'
uptime
echo ''
echo '--- [CPU 状态: 温度与频率] ---'
echo -n 'CPU Temp: ' && cat /sys/class/thermal/thermal_zone0/temp | awk '{print \$1/1000\"°C\"}'
echo -n 'CPU Freq: ' && cat /sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq | awk '{print \$1/1000\"MHz\"}'
echo ''
echo '--- [NPU 负载] ---'
cat /sys/kernel/debug/rknpu/load
echo ''
echo '--- [DDR/DMC 频率] ---'
cat /sys/class/devfreq/dmc/cur_freq | awk '{print \$1/1000000\"GHz\"}'
echo ''
echo '--- [内存占用 (MB)] ---'
free -m
"
```
