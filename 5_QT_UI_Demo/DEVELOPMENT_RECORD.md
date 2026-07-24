# 5_QT_UI_Demo 开发记录

## 2026-07-24 Qt/PCIe 并行关系与运行效率展示更新

- 更新 `outputs/019f93e4-9213-7970-9528-2e5cd0f225a2/presentations/pcie-async-performance/output/PCIe异步并行性能展示_并行与资源更新.pptx`，保留原左右拉开式双卡布局。左侧以树状关系表达：完整帧输入 28.10 FPS；显示链路后端 7.89 ms/帧、Qt 绘制 28.02 FPS；AI 链路每 2 帧调度 1 次、57.94 ms/任务、13.92 FPS。
- 显示链路达成率按 `28.02 / 28.10 = 99.7%` 展示；AI 调度达成率按目标 `28.10 / 2 = 14.05 FPS` 与实测 `13.92 FPS` 计算为 99.1%。两者用于说明并行支路运行效率，不与模型调用耗时串行相加。
- 右侧资源卡片注明 NPU 71% 为 2026-07-24 07:48:45 的单次运行监控快照，并列出内存占用 41.0%、CPU 1.416 GHz、DDR 1560 MHz、CPU 温度 53.75 °C 和系统负载 3.02/2.30/2.79。
- 模型区采用 YOLOv8 33.81 ms/帧与 PP-OCR H2 9.30 ms/车牌的独立端到端口径；额外 OCR 调用按多车牌数量解释。模板一致性检查通过，最终单页无裁切、遮挡或乱码。

## 2026-07-24 Qt/PCIe 异步并行性能展示页

- 已生成 `outputs/019f93e4-9213-7970-9528-2e5cd0f225a2/presentations/pcie-async-performance/output/PCIe异步并行性能展示.pptx`。页面将 28.10 FPS 采集、28.02 FPS Qt 实际绘制和 13.92 FPS AI 推理拆成独立泳道，以 F0–F3 说明显示线程不会等待 AI，推理完成后只异步更新后续帧。
- Qt 侧口径明确区分：`35.69 ms/帧` 是 28.02 FPS 的输出节拍，`7.89 ms` 是显示后端处理耗时；二者不是可直接相加的串行链路。稳定性区展示显示丢帧/推理失败为 0，AI 队列替换 17 次对应容量 1 的 latest-frame 防积压策略。
- 模型区保留可审计口径：YOLOv8 `33.81 ms/次` 为 RKNN 图内端到端；PP-OCR `16.16 ms/检出车牌` 为含条件回退的期望端到端，并列出 `9.30 + (445 / 893 × 13.76)` 计算式及 RKNN 官方 `9.023 ms/次`，避免把早先的均摊数误读为单次模型耗时。

## 2026-07-24 FPGA PDS 零帧根因确认

- 对当前 PG2L100H 工程
  `3_NPU_Yolov8_PPOCR_Demo/pcie_720p/pcie_test_img_100h` 的 RTL 与现有综合网表完成核查：
  PIO 的 `0xffffffe5` 会把采集 `start_flag` 保持为 1，但 `pcie_tx_fun.v` 同时在
  `i_start_tx_flag==0` 和互斥的 `else if(i_start_tx_flag)` 路径清零帧计数，并在
  `i_start_tx_flag==1` 时强制清零 `r_frame_done`。所以当前逻辑启动后不可能产生整帧完成。
- 2026-04-27 生成的现有 `.sbit` 综合网表已经将 `o_check_data[0]`、PIO
  `i_wr_frame_done` 及读状态 bit0 优化掉；这与板端 `WR_FRAME_DONE=0x00000000`、
  `ready=0` 和持续 `EPERM` 完全对应。当前首要修复位于 FPGA RTL 和 bitstream，不修改
  Qt 等待循环、PP-OCR/YOLO pipeline 或 RK3568 PCIe 供电配置。
- PDS 配置日志还确认 01:50 成功配置工程 `hdmi_loop.sbit` 后，01:55 和 02:01 的两次
  `cfg_program` 实际配置文件已被外部 Flash 操作切换为 PDS 自带的
  `PG2L100H/3V3_f1.sbit`，随后 Flash 扫描失败。以后不能用单独的
  `cfg_program succeed` 判断用户设计已烧录；必须核对同次 `cfg_assign_file` 路径，并在
  Flash 操作后明确恢复工程 `.sbit`。
- 主逻辑修复并重新综合后，还必须核查 PIO 已配置但未送入 `pcie_tx_fun` 的其余三个 DMA
  缓冲地址，以及外部 `pixclk_in` 缺少真实 720p 时钟/输入延迟约束的问题；否则状态位恢复后
  仍可能出现缓冲索引错误或视频输入域偶发不稳定。当前未修改 RTL、未生成新 `.sbit`。

## 2026-07-24 图片识别模式接入

- 图片模式主标题统一为“图片车牌识别”。
- 图片识别定义为“FPGA/PCIe 静态画面识别”，继续复用 1280×720 BGR565 采集、YOLO 车牌定位、
  PP-OCR ROI 识别、GA 36 校验、RGA 叠加和 Qt 回调，不增加本地文件选择器。
- 项目 3 新增 `RunPpocrPcieImageDemo()`：与视频入口共享同一初始化/采集/释放实现，但显示
  结果直接取最近一次独立推理输出，绕过 `SimplePlateTracker` 的连续 2 次确认、运动平滑和
  全周期 `plate_votes`，避免同一位置更换静态图片后旧车牌文字继续占据最高票。
- 图片模式使用
  `ppocrv4_rec14_fold_affine_1x1_rk3568_fp16.rknn`；视频模式继续使用 H2 混合量化模型。
  两个模型按模式点击“开始”时分别初始化，切换模式会先释放旧模型。FP16 模型已加入 CMake
  必需资源、安装清单和 `build-linux.sh` 安装结果校验。
- 图片结果表不累计历史车牌；`inference_jobs` 每产生一次新推理结果就清空旧行，并只显示本次
  第一个通过 GA 36 校验的车牌。无有效车牌时表格为空，画面仍可用 `RAW` 标签显示本次无效
  OCR 文本，便于诊断。
- 新增 `build_single_inference_plate_results()` 无状态结果转换及回归用例，验证连续输入
  `京A12345 -> 京B12345` 时第二次立即输出京B、不会保留京A；无效当前文本不会继承旧合法结果。
  本机 `plate_rule_test` 全部通过，Qt 主源使用 MSVC 19.41 编译通过，项目 5 CMake
  配置/生成通过；仍需 Ubuntu aarch64 全量编译和 RK3568 静态图片实流复测。

## 2026-07-23 三模式与行人违法检测接入

- 模式下拉框改为 `视频识别 / 图片识别 / 行人违法检测`。视频识别继续调用项目 3 的
  `RunPpocrPcieDemo()`；图片识别在本阶段先作为占位，选择后显示“暂未接入”并禁用开始按钮，
  不创建工作线程、不打开 PCIe 或加载模型；该占位已于 2026-07-24 由本文上一节的静态图片
  即时识别实现替代。
- 行人违法检测通过项目 4 新增的 `RunTrafficPcieQtDemo()` 接入，复用其八类 YOLO 模型中的
  person/traffic-light 专用后处理、5 帧灯色投票、person 时序跟踪、斑马线 ROI、违法事件去重
  和 `TrafficOverlayRenderer`；Qt 模式不初始化 DRM，而是通过原 `PcieUiCallbacks` 接收
  RGBA8888 叠加帧。
- 交通结果表固定显示信号灯、当前/累计行人、斑马线内人数和当前/累计违法人数；模式运行期间
  下拉框锁定，停止后再允许切换，防止界面模式与正在运行的后端不一致。暂停/继续、Qt 背压、
  截图和终端优雅退出继续复用原路径。
- 项目 4 的 YOLO/后处理公开符号统一增加 `traffic_` 前缀，RKNN 上下文类型独立为
  `traffic_rknn_app_context_t`，并作为独立静态后端目标编入 Qt，解决项目 3 与项目 4 同名
  函数的链接冲突及不同结构体定义的 ODR/ABI 隐患。交通模型和八类标签独立安装到
  `model/traffic/`，避免覆盖车牌模型使用的 `model/labels_list.txt`。
- 本机验证已确认 `mainwindow.ui` XML 合法且三项文本、顺序完全匹配；Qt 主入口使用
  Qt 5.12.9 头文件和 MSVC 19.41 通过语法编译；项目 4 的 CMake 配置/生成以及交通规则、
  时序 Tracker、叠加层源文件的本机编译通过。项目 5 的 CMake 已解析至配置完成，但 Windows
  无法执行随 ARM64 Qt 包提供的 `moc`，因此最终全量链接仍以 Ubuntu aarch64 交叉编译为准。
- 待板端验证三种模式的选择与锁定、交通模型/标签加载、FPGA 实流叠加、违法统计更新、
  暂停/继续和退出清理；图片模式应始终不创建线程、不打开 PCIe。
- 修复项目 4 交通 PCIe 信号处理函数中直接 `(void)write(...)` 仍触发 GCC
  `-Wunused-result` 的告警：现与项目 3 保持一致，先保存 `ssize_t` 返回值再显式忽略；信号
  处理函数仍只执行 `sig_atomic_t` 写入、`write()` 和 `errno` 恢复。
- 修复模式只能在首次启动前选择的问题：采集运行时继续锁定下拉框，暂停后开放选择；暂停后
  直接“继续”仍复用当前后端，改选模式则同步结束旧工作线程并释放模型/PCIe，再将按钮恢复为
  “开始”。每次工作线程使用递增 generation 标识，旧线程退出前已排队的帧、状态或完成信号
  会被丢弃，避免污染新模式界面。
- 项目 4 的内置斑马线 ROI 更新为新测试人行道的 5 点归一化坐标
  `1.000000,0.695622;1.000000,0.765115;0.000000,0.886727;0.000000,0.645587;0.661587,0.615705`；
  Qt 行人违法模式调用同一 `default_traffic_roi()`，不需要额外传参即可使用新区域。

## 2026-07-23 YOLOv8 + PP-OCR Qt UI 迁移与适配

- 修复右侧车牌统计表收录中间偶发错误文字的问题：共用 Tracker 现只把在独立推理观测中
  连续 2 次完全一致且通过 GA 36 校验的车牌放入有效投票池；Qt 表继续只消费
  `PcieUiStatus::plate_text`，因此单次偶发的合法形状错误文本不再进入统计。确认逻辑不放在
  Qt 显示帧层，避免同一推理结果被多帧画面复用后误判为连续命中。主机侧新增瞬态错误回归
  用例并通过完整 `plate_rule_test`，仍需重新交叉编译后用 RK3568 PCIe 实流确认 UI 表现。
- 首次新版终端退出实测确认 `Ctrl+Z` 已可触发程序退出；同次板端启动日志显示 PCIe
  `Gen2 x2 / MPS 128`、驱动节点、显示线程和推理线程均正常初始化，但连续三次状态均为
  `last_status=-1 / ready=0`，`EPERM` 分别为 `11146 / 13003 / 14863`，即约每秒 929 次
  无帧重试且尚未收到首帧。历史健康记录同时存在 `Captured=4422` 和 `EPERM=7519`，因此
  `EPERM` 本身不是 Unix 权限故障；本次卡点明确位于 PP-OCR、YOLO 和 Qt 之前的 FPGA/PCIe
  帧交付阶段。待板端排除旧挂起进程占用、重载驱动，并确认 FPGA bitstream、1280×720 视频源及
  上电/复位顺序；不以屏蔽等待日志替代根因处理。
- 后续板端回传确认退出统计完整，`Captured / Display / Inference / Qt painted` 均为 0；
  `ps` 和 `fuser` 未发现残留采集进程，`rmmod/insmod` 成功且新模块引用计数为 0，已排除旧
  `Ctrl+Z` 挂起进程占用。首次运行期间 `dmesg` 连续 5 次报告
  `cma_alloc: reserved: alloc failed, req-size: 1024 pages, ret: -12`，即约 4 MiB 连续 CMA
  申请发生 `ENOMEM`；但其后驱动仍报告非零 `dma_addr_r=0x40800000` 和
  `dma_addr_w=0x41800000`，用户态 DMA 配置 ioctl 也未失败。仓库和公开检索均没有找到与当前
  `.ko` 完全匹配的驱动源码，因此暂不把 CMA 警告直接定为无帧根因；下一验证点为新加载驱动的
  立即复跑、程序启动前后 `CmaTotal/CmaFree` 对比，以及 FPGA 视频/DMA 发帧状态。
- 重载驱动后的第二次实测确认 `CmaTotal=16384 kB / CmaFree=1760 kB` 在应用启动前后不变。
  五次 4 MiB CMA 申请失败后，驱动仍依次获得
  `0x42000000 / 0x42400000 / 0x42800000 / 0x42c00000 / 0x43000000` 五个缓冲地址，
  并成功完成 `current_len=960 dw` 的读写 DMA 映射，说明内核已从普通内存回退；CMA 警告不再
  作为本次零帧根因。`/proc/interrupts` 中 `pcie-sys` 和 `PCIe PME` 均无计数，只能证明当前
  抓取时未见这两类 PCIe 中断活动，不能单独代替 FPGA DMA 状态。
- 已完成 `open(O_RDWR)` 与 `open(O_RDWR | O_NONBLOCK)` 的板端 A/B：阻塞版本仍连续得到
  `ready=0`，`EPERM` 计数为 `1847 / 3704 / 5565 / 7426`，同样约每秒 930 次。说明该驱动在
  当前无帧状态下的 `read()` 返回行为不受 `O_NONBLOCK` 控制，打开标志不是本次回归根因。
  已撤回阻塞读取试验和 Qt `SIGUSR1` 工作线程唤醒代码，恢复较简单的非阻塞读取；保留已经实测
  有效的终端信号到 Qt 关闭流程。下一排查边界收敛到 FPGA 视频输入、DMA 发帧状态以及 BAR0
  `0xffffffe5` 启动命令之后的首帧交付。
- 继续核对仓库 FPGA RTL 与原始 `pcie_reader`：四份 `pango_pci_driver.ko` 的 SHA-256 完全
  一致，项目 3 与项目 4 的 `PcieFrameSource` 也一致；原始 reader 的 open/ioctl/BAR0/DMA
  初始化顺序与当前封装相同。RTL 中 `pio_crtl.v` 用 `0xffffffe5` 置位 `start_flag`，
  `video_crtl.v` 随后必须等到外部 HDMI `VS` 上升沿才进入 `TX_DATA`；顶层实际连接
  `pixclk_in / vs_in / de_in`，内部测试图发生器未接入 DMA 数据路径。由此当前第一硬件检查项
  明确为 FPGA HDMI 输入是否已锁定并持续提供 1280×720 `VS/DE`，其次才是 DMA 请求/完成。
  原始 `pcie_reader` 存在把负数 `read()` 返回值也作为成功条件的示例缺陷，不以其 stdout
  是否有数据作为出帧证据。
- 板端已确认历史 LPR Qt 可执行文件仍位于
  `/userdata/yolov8_lpr_pcie_qt_ui/yolov8_lpr_pcie_qt_ui/yolov8_lpr_pcie_qt_ui`。下一步在不
  重载驱动、不改变 FPGA 和视频源状态的条件下直接启动旧 UI 并点击 `Start`，以旧 UI 是否实际
  出图及 PCIe FPS 是否增长作为软硬件边界 A/B；旧 UI 测试期间不使用其未适配的 `Ctrl+Z`。
- 旧 LPR Qt 已在同一块板、同一 FPGA/视频源状态和同一个已加载驱动下完成 A/B，结果同样为
  `last_status=-1 / ready=0`；20 秒内 `EPERM` 从 `1858` 递增至 `18617`，约每秒 931 次，
  与新版 PP-OCR Qt 完全一致。由此排除第 5 阶段 UI 迁移、PP-OCR 模型、Qt 显示、OpenGL
  软件渲染以及 `O_NONBLOCK` 对零帧故障的影响。后续不再重复软件侧重构或内存检查，直接核查
  FPGA 的外部 HDMI `VS/DE`、BAR0 启动标志、DMA 请求、DMA 完成和整帧完成信号。
- FPGA/视频采集状态恢复后的新版 Qt 实测运行约 121.5 秒：`Captured=3415 (28.10 FPS)`、
  `Display pipeline=3411 (28.07 FPS)`、`Qt UI painted=3410 (28.02 FPS)`，帧池、显示队列、
  显示、叠加均为 0 丢帧/失败，证明 Qt 显示不会拖慢 28 FPS PCIe 采集。模型侧
  `Inference=1691 (13.92 FPS)`、失败 0、推理队列丢 17；该值符合源码
  `kInferenceInterval=2` 的主动隔帧策略，理论调度上限为 `28.10/2=14.05 FPS`，不是 UI
  性能下降。平均完整 YOLO+PP-OCR pipeline 为 `57.94 ms`；893 次主 OCR 中触发 445 次重试，
  49 次重试被接受，重试平均 `13.76 ms`。本次恢复也反证此前 `ready=0` 是 FPGA/视频/DMA
  启动状态问题，而非第 5 阶段软件回归。
- 后续零帧复现时的板端快照显示 RK3568 Root Port `0002:20:00.0 [1d87:3566]` 和 FPGA
  Endpoint `0002:21:00.0 [0755:0755]` 均已枚举，`pango_pci_driver` 已加载且
  `/dev/pango_pci_driver` 存在；过滤后的最近 200 行 `dmesg` 未见 PCIe link、AER、BAR 或
  DMA 错误，仅见上次进程关闭后的 `pango_cdev_release`。`pcie-sys` 与 `PCIe PME` 仍为 0，
  继续不把这两个服务中断当作 DMA 完成证据。此前命令中的 `<FPGA的BDF>` 是说明占位符，
  直接复制会被 Bash 解析成重定向；当前板端应改用实际 BDF `0002:21:00.0`，并在应用保持
  `ready=0` 时同时抓取 Endpoint 与 Root Port 的 `LnkSta/DevSta`，以区分链路/供电问题和
  FPGA 视频/DMA 内部停滞。
- 应用以 PID 3941 持续占用 `/dev/pango_pci_driver` 时完成了上述同步抓取：Endpoint 和
  Root Port 的 `LnkSta` 均稳定为 `5 GT/s x2`，Endpoint 的三个 BAR 已分配且内核驱动绑定为
  `pango_pci_driver`，两端 `DevSta` 均未置位可纠正、不可纠正、致命或不支持请求错误。
  启动日志也明确显示 `PCIe Gen.2 x2 link up`，驱动装载后识别到相同 Link Speed/Width；
  当前零帧状态没有主链路掉线、降速或 BAR 丢失证据。`rockchip,pipe_grf` 警告属于旧 RK3568
  PCIe3 PHY 驱动已知的非致命误查，上游后续改为仅在 RK3588 查询该属性；`invalid prsnt-gpios`
  和 `can't get current limit` 表明运行设备树仍有可清理的可选板级描述，但在固定 FPGA 已
  枚举并稳定保持 Gen2 x2 的本次快照中，不足以解释 `ready=0`。应用本次 DMA 初始化虽再次
  出现五次 4 MiB CMA 申请失败，随后仍获得 `dma_addr_r=0x42400000` 和
  `dma_addr_w=0x43400000` 并完成映射，与既有回退路径一致。下一硬件观察点收敛为 FPGA
  `start_flag`、外部 HDMI `VS/DE`、DMA 请求/完成及 `i_wr_frame_done`；若继续查供电，应优先
  测量 FPGA/HDMI 接收侧电源域和复位时序，而不是先改 RK3568 PCIe 主链路配置。
- 保持采集进程打开、用 `SIGSTOP` 暂停所有用户态线程两秒后，直接读取
  BAR0 `WR_FRAME_DONE`（物理地址 `0xf0200140`）得到 `0x00000000`。暂停期间驱动没有机会
  消费并清除完成位，因此结果表明 FPGA 未锁存 `i_wr_frame_done`，零帧边界已进一步收敛到
  `start_flag` 之后、整帧完成之前。下一最小 A/B 是在进程保持暂停时重新写入 BAR0 地址 0 的
  `0xffffffe5`，等待两秒后再次读取 `0xf0200140`：若完成位出现，则检查原启动写入后 FPGA
  是否又发生复位；若仍为 0，则直接观察 HDMI `VS/DE`、视频控制状态机和 DMA 请求/完成。
- 操作手册补充统计口径：C++ 采集侧没有 28 FPS 限速，`Captured` 由 FPGA/驱动整帧交付决定；
  `Inference` 受每两帧调度一次和容量 1 的最新帧队列控制；PP-OCR 重试仅针对 GA 36 校验失败
  的首次识别，使用四边扩展 5% 的 ROI 且每个推理帧最多一次，只有重试结果转为合法号牌时才计
  `retry accepted`。同时明确 `plate results` 包含最终无效的原始检测结果，不能视为有效车牌数。
  Qt 路径不执行 DRM commit，故 `present=0.00 ms` 是指标不适用；真实绘制吞吐以
  `Qt UI painted` 为准。
- 修复虚拟机 ADB 终端按 `Ctrl+Z` 只挂起进程、无法触发性能汇总的问题：Linux Qt 入口现以
  异步信号安全的 `sig_atomic_t` 标志接收 `SIGINT`、`SIGTERM`、`SIGHUP` 和 `SIGTSTP`，
  再由 Qt 主线程定时检查并调用窗口关闭流程；信号处理函数本身不调用 Qt、分配内存或等待线程。
  `Ctrl+Z` 在本程序中因此被明确映射为优雅退出，而不是系统默认的挂起。
- 修复关闭窗口时 `Qt UI painted` 依赖排队信号、可能在事件循环结束前来不及打印的问题：
  工作线程等待完成后同步补打 UI 统计，并用状态位保证每轮运行只打印一次；后端
  `PCIe Pipeline Statistics` 与 UI 统计均显式刷新 stdout。
- 修复共用后端 `main_ppocr.cc` 中信号处理 `write()` 返回值未使用的 GCC 告警；仅保存并显式忽略返回值，不改变 Qt UI、PCIe、YOLO 或 PP-OCR 热路径，因此不会影响运行性能。

### 目标

- 新建独立的 `5_QT_UI_Demo/`。
- 将原 Qt 窗口、Designer UI 和样式辅助代码从第 3 阶段迁入第 5 阶段。
- 保持原界面的实时显示、状态统计、结果表、暂停/继续和截图行为。
- 将旧 `YOLOv8 + LPRNet` 后端替换为当前 `main_ppocr.cc` 的
  `YOLOv8 + PP-OCRv4` 后端。
- 第 3 阶段继续负责无界面的推理、PCIe、Tracker、RGA/DRM 和模型资源；第 5 阶段只负责 Qt
  前端及其构建/部署入口。

### 接口映射

| 项目 | 适配前 | 适配后 |
| --- | --- | --- |
| 后端入口 | `RunPcieDemo()` | `RunPpocrPcieDemo()` |
| 参数 1 | YOLOv8 RKNN | YOLOv8 RKNN |
| 参数 2 | 7 位 LPRNet RKNN | PP-OCR RKNN |
| 参数 3 | 8 位 LPRNet RKNN | 73 字符字典 |
| Pipeline | `yolo_lpr_pipeline.cc` | `yolo_ppocr_pipeline.cc` |
| 车牌文本置信度 | LPRNet 结果 | PP-OCR CTC 平均字符置信度 |
| UI 模式 | 车牌识别/行人占位 | YOLOv8 + PP-OCR |
| 可执行文件 | `yolov8_lpr_pcie_qt_ui` | `yolov8_ppocr_pcie_qt_ui` |

### 代码边界

第 5 阶段拥有：

- `src/main_pcie_qt.cc`
- `src/mainwindow.ui`
- `src/pcie_qt_ui_helpers.cc`
- `include/pcie_qt_ui_helpers.h`
- `assets/fonts/simhei.ttf`
- Qt 专用 `CMakeLists.txt` 和 `build-linux.sh`

第 3 阶段继续拥有：

- `src/main_ppocr.cc` 和 `RunPpocrPcieDemo()`
- `include/pcie_demo_bridge.h` 中的
  `PcieUiCallbacks` / `PcieUiFrame` / `PcieUiStatus` 桥接契约
- PCIe 帧源、YOLO、PP-OCR、规则、Tracker、RGA/DRM
- RKNN 模型、字典、PCIe 驱动和第三方库

该边界使命令行 PP-OCR Demo 不依赖 Qt，同时 Qt UI 复用同一份生产后端，不维护第二套推理逻辑。

### 本次代码变更

1. Qt 工作线程参数改为 `yolov8_model / ppocr_model / dictionary`。
2. 工作线程改调 `RunPpocrPcieDemo()`，继续使用原 `PcieUiCallbacks` 完成暂停、退出、背压、
   RGBA 帧和状态交付。
3. `include/pcie_demo_bridge.h` 补充 PP-OCR 后端函数声明，使前端和实现的编译期契约闭合。
4. 界面标题和模式标识改为 `YOLOv8 + PP-OCR`，删除未接入的“行人模式”占位。
5. 第 5 阶段 CMake 只列入 PP-OCR 相关后端源文件，不再编译 `lprnet.cc` 或
   `yolo_lpr_pipeline.cc`。
6. 安装规则只部署 YOLO、指定 PP-OCR H2 模型、73 字符字典、标签、中文字体和 PCIe 驱动，
   不再部署 7 位/8 位 LPRNet 模型。
7. 第 3 阶段移除旧 `ENABLE_QT_UI` 构建目标；Qt 编译统一从第 5 阶段进入。
8. `simhei.ttf` 经引用检查确认仅供 Qt 加载，已由项目 3 的 `model/` 迁入项目 5 的
   `assets/fonts/`；项目 3 的 DRM/RGA 文字叠加继续使用内嵌 `utils/plate_font.h`。
9. Qt 入口增加终端停止信号桥接，`Ctrl+C`、`Ctrl+Z`、`SIGTERM`、`SIGHUP` 与关闭窗口统一
   进入 `ShutdownWorker()`，确保 PCIe、显示、推理线程退出后再输出完整统计。

### 保持不变的行为

- FPGA 输入仍为 1280×720 小端 BGR565。
- 仍使用 6 槽帧池、显示队列 2、推理队列 1、每 2 帧推理一次。
- 检测结果仍经 Tracker 平滑后叠加。
- Qt 仍接收已经转换并叠加完成的 RGBA8888 帧。
- Qt 待绘制事件上限仍为 2，背压时丢弃旧显示帧。
- “暂停”不释放模型或 PCIe；关闭窗口或终端停止信号才完整退出。
- 截图仍保存已叠加画面，优先 PNG，失败时回退 BMP。

### 验证记录

已完成：

- [x] Qt 源文件迁入 `5_QT_UI_Demo/src/`。
- [x] UI 前端不再引用 LPRNet 模型参数或 `RunPcieDemo()`。
- [x] CMake 源文件集合不再包含 LPRNet pipeline。
- [x] PP-OCR 模型、字典和字体安装路径已显式检查。
- [x] 第 3 阶段旧 Qt 构建入口已移除，避免同时维护两套目标。
- [x] `mainwindow.ui` XML 解析通过。
- [x] Windows CMake 3.29 配置和生成通过，OpenCV 3.4.5、Qt5 Widgets、
  AUTOUIC/AUTOMOC 目标均成功解析。
- [x] AUTOUIC/AUTOMOC 生成后，`main_pcie_qt.cc` 和 `pcie_qt_ui_helpers.cc`
  使用本机 Qt5 + MSVC 19.41 编译通过。
- [x] `plate_rule_test` 通过：`all cases passed (ga36_plate_type_v3)`。
- [x] `ppocr_retry_policy_test` 通过：`all cases passed`。
- [x] 项目 3 的 CMake/CTest 回归为 `2/2 tests passed`；仅保留
  `simple_tracker.cc` 既有的整数到浮点转换告警。
- [x] 终端信号处理函数仅写入 `sig_atomic_t`，Qt 关闭操作仍在主线程执行。
- [x] 修复后的 `main_pcie_qt.cc` 使用本机 Qt 5.15.15 + MSVC 19.41 编译通过；
  Linux 专用 `sigaction` 分支仍以 Ubuntu aarch64 交叉编译结果为准。

待 Ubuntu/RK3568 验证：

- [ ] Ubuntu 20.04 + Qt 5.12.9 ARM64 全量交叉编译。
- [ ] `ldd` 检查 Qt、RKNN、RGA、OpenCV 依赖闭合。
- [ ] RK3568 X11/xcb 界面启动。
- [ ] FPGA PCIe 1280×720 实流连续显示。
- [ ] YOLO 框、PP-OCR 文本、类型和置信度进入画面及结果表。
- [ ] 暂停/继续不重复初始化模型、不关闭 PCIe。
- [ ] 截图保存、窗口关闭和统计输出正常。
- [ ] ADB 终端分别使用 `Ctrl+C`、`Ctrl+Z` 和 `kill -TERM`，均能打印完整后端/UI 统计后退出。
- [ ] 长时间运行下 Qt 背压无持续内存增长。

### 风险与说明

- 本目录依赖相邻的 `3_NPU_Yolov8_PPOCR_Demo`，单独复制 `5_QT_UI_Demo` 无法构建。
- Qt 运行使用 X11/xcb；当前板端无 `/dev/fb0`，`linuxfb` 不是可用路径，`eglfs` 也未作为主路径。
- 第 3 阶段 `main_ppocr.cc` 的命令行模式走 DRM；第 5 阶段通过非空 Qt callbacks 走 RGBA/Qt
  分支，二者共享推理和采集代码，但显示所有权不同。
- 当前开发机没有 aarch64 Qt 交叉环境，本记录不把源码级检查写成实板验证结论。
- Windows 全目标构建会在第 3 阶段既有 Linux 专用 `dirent.h` 和板端库处停止；该结果不属于
  Qt 适配编译失败。Qt 前端已单独完成编译级检查，最终可执行文件仍必须由 Ubuntu aarch64
  交叉工具链生成。

## 历史 Qt UI 记录（迁移自项目 3）

以下两节记录迁移前 `YOLOv8 + LPRNet` Qt UI 的实板 FPS 和前后端交接实现。数据保留为性能
基线；其中目录、源码位置、`RunPcieDemo()`、`-q` 构建选项和
`yolov8_lpr_pcie_qt_ui` 运行命令均为旧链路，当前 PP-OCR UI 的构建与运行以
[README.md](README.md) 为准。

## PCIe Qt FPS test record - 2026-07-20

Workspace:

`D:\a_fpga\codex\5_12_HDMI_IN_DDR3_HDMI_OUT\else\Yolo_LPR_RK3568_FPGA\3_NPU_Yolov8_PPOCR_Demo`

Test target:

- `src/main_lpr.cc`: LPR reference PCIe capture / display pipeline / inference statistics.
- `src/main_pcie_qt.cc`: Qt UI painted FPS statistics.

Terminal result:

```text
========== PCIe Pipeline Statistics ==========
Captured: 4422 (28.00 fps), pool drops: 0
Display pipeline: 3916 (24.80 fps)
Display presented/handoff: 3916 (24.80 fps), queue drops: 0, display failures: 0, overlay failures: 0
Inference: 2210 (14.00 fps), queue drops: 1, failures: 0, plate results: 0
Average inference pipeline: 47.73 ms
Average display convert/overlay: 7.02 / 0.01 ms
Average display present/end-to-end: 0.00 / 8.12 ms
Driver retries: zero=0 EPERM=7519 interrupted/EAGAIN=0, fatal errors=0
==============================================
Qt UI painted: 3916 (24.78 fps)
```

Random UI screenshot values:

```text
PCIe capture: 29.1 FPS
Screen display: 25.2 FPS
Model inference: 15.5 FPS
End-to-end latency: 8.2 ms
```

Interpretation:

- `Captured` counts successful `PcieFrameSource::ReadFrame()` results. It is
  the PCIe/user-space frame input rate.
- `Display pipeline` counts frames that completed display-side conversion and
  overlay. It is the backend display-processing throughput before UI painting.
- `Display presented/handoff` counts frames accepted by the display backend. In
  DRM mode this means commit success; in Qt mode this means handoff to Qt.
- `Qt UI painted` counts frames after Qt main thread completes
  `QLabel::setPixmap()`. This is the real Qt UI painted-frame rate.
- `Inference` counts successful `process_pipeline()` jobs. With the current
  `kInferenceInterval = 2`, the expected inference FPS is about half of
  captured FPS.

Conclusion for this test:

- Capture average is about 28 FPS.
- Qt UI painted average is about 24.78 FPS, matching display handoff almost
  exactly, so Qt painting itself is not the main loss point in this run.
- Inference average is exactly about half of capture FPS, matching the current
  every-2nd-frame inference policy.
- Display loss is `4422 - 3916 = 506` frames over the run. This is between PCIe
  capture and display pipeline/handoff, not between Qt handoff and Qt painting.

## PCIe Qt UI handoff notes - 2026-07-21

Important source files:

- `src/main_lpr.cc`: shared LPR reference PCIe capture, display conversion/overlay, NPU
  inference, and terminal FPS statistics. It still builds the original
  command-line PCIe demo when `PCIE_QT_UI_BUILD` is not defined.
- `src/main_pcie_qt.cc`: Qt frontend. It calls `RunPcieDemo()` through
  `PcieUiCallbacks`, paints frames on the Qt main thread, and prints
  `Qt UI painted` when the window exits.
- `src/pcie_demo_bridge.h`: bridge structs between the shared PCIe backend and
  the Qt frontend.
- `src/mainwindow.ui` and `src/pcie_qt_ui_helpers.*`: Qt layout and styling.

Build the Qt UI target in the Ubuntu/aarch64 cross-build environment:

```shell
cd /mnt/hgfs/Yolo_LPR_RK3568_FPGA_Project/3_NPU_Yolov8_PPOCR_Demo
export GCC_COMPILER=/usr/bin/aarch64-linux-gnu
export QT_ARM64_PREFIX=/home/gyn/Qt-5.12.9-arm64
./build-linux.sh -t rk3568 -a aarch64 -d yolov8_lpr -q
```

The install output is:

```text
install/rk356x_linux_aarch64/rknn_yolov8_lpr_demo/yolov8_lpr_pcie_qt_ui/
```

Run on RK3568 after pushing the install directory:

```shell
cd /userdata/yolov8_lpr_pcie_qt_ui/yolov8_lpr_pcie_qt_ui
export DISPLAY=:0
export XAUTHORITY=/var/run/lightdm/root/:0
export QT_QPA_PLATFORM=xcb
./yolov8_lpr_pcie_qt_ui ./model/yolov8.rknn ./model/lprnet7repair_i8.rknn ./model/lprnet8repair_i8.rknn
```

Current UI smoothness changes to preserve:

- The Qt UI receives already-converted RGBA frames from the shared PCIe backend;
  the old standalone `pcie_qt_window.*` path was removed.
- `QImage::copy()` was removed from the hot path. The queued Qt frame owns a
  shared pixel buffer until `QLabel::setPixmap()` finishes.
- Qt scaling uses `Qt::FastTransformation` to reduce CPU cost on RK3568.
- Status updates are throttled to about 250 ms and `OnFrameReady()` only paints
  the video frame, avoiding per-frame table/status widget churn.
- Qt back-pressure allows at most two pending UI frame events
  (`kMaxPendingUiFrames = 2`). If Qt is behind, `main_lpr.cc` drops before
  expensive conversion/overlay work.
- `PcieFrameSource` 保持 `O_NONBLOCK` 打开驱动。板端 A/B 已证明改成 `O_RDWR` 阻塞打开后
  仍以相同速率返回 `EPERM` 且 `ready=0`，因此不再用文件打开标志解释 FPGA 首帧缺失。

Saved image flow:

- The Qt UI has a `保存图片` button. It is enabled after the first painted frame.
- The saved file is the latest full-resolution RGBA frame after backend
  conversion and recognition overlay, not the scaled QLabel preview.
- Images are written as PNG files under:

```text
/userdata/yolov8_lpr_pcie_qt_ui/yolov8_lpr_pcie_qt_ui/saved_images/
```

Pull saved images from the upper computer with ADB:

```powershell
D:\adb\bin\adb.exe pull /userdata/yolov8_lpr_pcie_qt_ui/yolov8_lpr_pcie_qt_ui/saved_images .\saved_images
```

The board-side Qt process normally cannot `adb push` to the upper computer by
itself, because ADB is initiated from the upper computer. For automatic transfer
without manual `adb pull`, add a small TCP/HTTP receiver on the upper computer
or use SSH/SCP if the board image has network and credentials configured.

FPS metric meanings:

- `Captured`: successful `PcieFrameSource::ReadFrame()` frames from PCIe into
  userspace.
- `Display pipeline`: frames that completed display conversion and overlay.
- `Display presented/handoff`: DRM mode means successful display commit; Qt mode
  means the backend handed the frame to Qt.
- `Qt UI painted`: frames after the Qt main thread completed
  `QLabel::setPixmap()`. This is the best number for visible UI smoothness.
- `Inference`: successful `process_pipeline()` jobs. With
  `kInferenceInterval = 2`, this is expected to be about half of capture FPS.

If `Display presented/handoff` is close to `Qt UI painted`, Qt painting is not
the main bottleneck. If capture is much higher than display pipeline, focus on
conversion/overlay/back-pressure. If inference is low while display is smooth,
focus on RKNN/NPU/postprocess.
