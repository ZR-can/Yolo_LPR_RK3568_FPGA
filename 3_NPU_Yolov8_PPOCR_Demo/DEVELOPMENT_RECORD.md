# 3_NPU_Yolov8_PPOCR_Demo 开发记录

操作和部署步骤见 [README.md](README.md)。

## 2026-07-24 PCIe 异步并行性能展示页数据口径更新

- 在不改变原左右双卡布局的前提下，完成 `outputs/019f93e4-9213-7970-9528-2e5cd0f225a2/presentations/pcie-async-performance/output/PCIe异步并行性能展示_并行与资源更新.pptx`：左侧明确表示 FPGA/PCIe 完整帧 28.10 FPS、显示后端 7.89 ms/帧并以 28.02 FPS 绘制、AI 每 2 帧调度 1 次且实测 57.94 ms/任务、13.92 FPS，避免将异步支路误解为串行耗时相加。
- AI 子链路按独立调用口径展示：YOLOv8 RKNN 图端到端 33.81 ms/帧；PP-OCR H2 `inference_e2e` 9.30 ms/车牌。条件追加 OCR 不再解释成“回退”，而解释为同一画面存在多块车牌：每增加 1 块车牌，追加 1 次 OCR。
- 右侧使用 2026-07-24 07:48:45 的运行时监控快照和 YOLOv8/H2 评测日志：NPU 71%、内存 802/1958 MB（41.0%）、CPU 1.416 GHz、DDR 1560 MHz、CPU 53.75 °C；模型文件共 7.07 MiB（YOLOv8 4.42、PP-OCR H2 2.65），H2 准确率 84.21%、吞吐率 101.96 牌/s。
- 最终模板一致性检查通过，单页渲染无裁切、遮挡或乱码；PPTX 包检查为 1 个 slide XML、7 个非空媒体、0 个空页级占位符。

## 2026-07-24 PCIe 异步并行性能展示页

- 新增单页 PPT `outputs/019f93e4-9213-7970-9528-2e5cd0f225a2/presentations/pcie-async-performance/output/PCIe异步并行性能展示.pptx`，用采集、Qt 显示、AI 推理三条泳道展示 F0–F3：采集与显示按约 35.6 ms/帧连续推进，AI 采用 latest-frame 策略每两帧处理一次，推理结果异步更新到后续显示帧。
- 页面采用原始运行日志口径：采集 28.10 FPS、Qt 实际绘制 28.02 FPS、AI 13.92 FPS、整项推理任务 57.94 ms/任务；未把 35.6 ms/帧伪写为 PCIe 传输延迟，也未把显示后端 7.89 ms 当作整帧系统延迟。
- YOLOv8 指标明确为 RKNN 图内端到端 33.81 ms/次（证据 `33,808 us`），应用预/后处理未独立计时；PP-OCR 指标明确为每个检出车牌的期望端到端 16.16 ms，包含条件回退：`9.30 + (445 / 893 × 13.76) = 16.16 ms`，同时保留 RKNN 官方推理 `9.023 ms/次` 作为可核对的算子证据。
- 页脚注明 57.94 ms/任务、33.81 ms/YOLO 调用和 16.16 ms/检出车牌的分母不同、不可相加。最终 PPT 为 1 页；包检查为 1 个 slide XML、1 个非空媒体、0 个页级占位符；渲染目检无裁切、遮挡或乱码。

## 2026-07-24 LPR 全流程演示页排版优化

- 已解析单页源 PPT，并以人工完成的 YOLO 区域作为版式基准；PP-OCR、POST、DISPLAY
  三条处理链已完成原生形状重排，统一卡片间距、箭头数量、字体与粗细层级。
- 下方密集卡片采用清晰的上下信息层级，同时保留 YOLO 的 Microsoft YaHei 粗体、分区强调色、
  白底浅蓝描边卡片和同轴箭头节奏；PP-OCR、POST、DISPLAY 分别统一为 5、6、8 个连续阶段。
- 最终 PPT 保持单页、全部对象可编辑；模板一致性检查通过，布局检查为 0 个未豁免错误，
  PPTX 包检查为 1 页、0 个空媒体、0 个空白页级占位符。

## 2026-07-24 PG2L100H PDS 零帧根因定位

- 当前板端使用的工程确认是
  `pcie_720p/pcie_test_img_100h/hdmi_loop.pds`，器件为 PG2L100H，工程源文件明确包含
  `src/pcie/pcie_tx_fun.v` 和 `src/pcie/pio_crtl.v`。现有
  `generate_bitstream/hdmi_loop.sbit` 生成于 2026-04-27 16:45:28；2026-07-24 的 PDS
  日志没有新的综合记录。配置日志显示 01:50:51～01:50:55 确实成功配置过工程
  `hdmi_loop.sbit`；但 01:55:53 和 02:01:12 为外部 Flash 操作分配并配置的实际文件已变成
  PDS 自带的 `cfg_fpga_voltage_sbit/Logos2/PG2L100H/3V3_f1.sbit`，随后 Flash 扫描失败。
  因此调试时不能只看 `cfg_program succeed`，每次都必须同时确认紧邻的
  `cfg_assign_file` 是工程 `.sbit`；外部 Flash 操作后应重新配置工程文件或按确定的上电加载
  流程恢复用户逻辑。
- 已定位到确定性的 RTL 条件错误：PIO 写入 `0xffffffe5` 后把 `i_start_tx_flag` 保持为 1，
  但 `pcie_tx_fun.v` 的帧计数逻辑在 `i_start_tx_flag==0` 时清零后，又在互斥的
  `else if(i_start_tx_flag)` 分支继续清零；帧完成逻辑也在 `i_start_tx_flag==1` 时清零，
  却只允许在后续 `else if` 中置位。因此启动状态覆盖 0/1 两种取值时，
  `r_frame_cnt` 都不能累计，`r_frame_done` 永远不能置 1。
- 用当前 `.sbit` 对应的 `synthesize/hdmi_loop_syn.vm` 完成交叉验证：综合后的
  `pcie_tx_fun` 已移除 `o_check_data/o_dma_wr_done/o_dma_wr_index` 端口，综合后的
  `pio_crtl` 也已移除 `i_wr_frame_done` 和锁存器，仅保留 `pio_rd_data[2:1]`；
  `pio_rd_data[0]` 在顶层显示为 floating。该结果与板端暂停用户态竞争后读取
  BAR0 `0xf0200140 == 0x00000000` 完全一致，证明当前 bitstream 在逻辑上不可能返回
  `ready=1`。本次零帧的第一根因不是 Qt、PP-OCR、驱动打开标志或已保持 Gen2 x2 的
  RK3568 PCIe 主链路。
- 最小主修复为删除帧计数器的 `else if(i_start_tx_flag)` 清零分支，并将帧完成清零条件改为
  `!i_pcie_rst_n || !i_start_tx_flag`；修复后必须全量综合，先在综合网表确认
  `o_check_data[0] -> i_wr_frame_done -> pio_rd_data[0]` 链路未被优化，再生成和烧录新
  `.sbit`。当前仅完成诊断和记录，尚未修改 RTL 或覆盖现有 bitstream。
- 修复主阻塞后还需一并审计四缓冲地址：PIO 接收了四个独立 DMA 物理地址，但顶层只把第一个
  地址传入 `pcie_tx_fun`，其余三个端口已被综合优化；同时每帧结束时 `r_dma_addr` 仍回到
  `i_dma_base_addr`，没有使用已计算的 `r_dma_base_addr`。若只修 `frame_done`，可能恢复
  `ready`，但后续索引 1～3 仍会读取错误缓冲。
- 时序约束同样需补齐：`hdmi_loop.fdc` 只约束 PCIe `pclk/pclk_div2/ref_clk`，没有为外部
  `pixclk_in` 建立真实 720p 像素时钟和输入延迟；现有时序报告把它推断为 1 MHz/1000 ns，
  因此“All Constraints Met”不能证明 74.25 MHz HDMI 输入域闭合。该项是主逻辑修复后的
  稳定性风险，不是当前 bit0 恒为 0 的直接原因。

## 2026-07-24 PCIe 静态图片即时结果入口

- 新增 `RunPpocrPcieImageDemo()` 供项目 5 图片模式调用。该入口与
  `RunPpocrPcieDemo()` 共用模型初始化、PCIe 采集、隔帧推理、RGA 叠加和释放路径，只将结果
  策略切换为最近一次独立推理输出。
- 新增无状态 `build_single_inference_plate_results()`：保留 GA 36 类型化合法性校验，但不
  使用连续命中、DIoU 轨迹、位置平滑或累计 `plate_votes`。因此静态画面从上一张车牌换到下一张
  后，新一次推理完成即替换框中文字，不需要新文本累计超过旧投票。
- 视频入口和命令行 `yolov8_ppocr_pcie_demo` 保持原 Tracker 与连续 2 次确认策略不变，继续
  过滤运动视频中的瞬态 OCR 错误。
- `plate_rule_test` 新增前后两张合法车牌及当前无效文本回归，本机 MSVC 编译运行通过：
  `plate_rule_test: all cases passed (ga36_plate_type_v3)`。

## 2026-07-23 YOLOv8 + PP-OCR PCIe 级联适配

- 为第 5 阶段三模式 Qt UI 扩展共用 `PcieUiStatus`：新增稳定灯色、当前/累计行人数、斑马线内
  人数和当前/累计违法人数；项目 3 的 PP-OCR 后端不写这些字段，原车牌回调行为和二进制入口
  不变。项目 4 的 Qt 桥接通过同一状态结构回传交通规则结果，避免 Qt 层维护第二套帧协议。
- 修复 Qt 车牌统计表把单次偶发、但恰好通过 GA 36 格式校验的错误文字计入结果的问题：
  Tracker 现要求同一合法车牌文本在独立推理观测中连续命中 2 次，才允许进入有效投票池；
  单次候选不会再产生 `has_valid_plate_text`，因此不会通过 `PcieUiStatus::plate_text` 进入
  第 5 阶段结果表。确认发生在 Tracker 推理观测层，避免显示线程复用同一推理结果时形成
  “伪连续”。`plate_rule_test` 已新增“高置信度单次错误 -> 连续正确文本”的回归场景；
  本机 MSVC 环境编译并运行通过，输出
  `plate_rule_test: all cases passed (ga36_plate_type_v3)`，仅保留原有的整数转浮点告警。
- 板端已完成 `open(O_RDWR)` 与 `open(O_RDWR | O_NONBLOCK)` 的零帧 A/B；两者都约每秒
  返回 930 次 `EPERM` 且始终 `ready=0`，证明打开标志不是本次首帧缺失根因。阻塞试验已撤回，
  `PcieFrameSource` 恢复 `O_NONBLOCK`，DMA/BAR 初始化和 `0xffffffe5/0xffffff00` 协议不变；
  第 5 阶段 Qt 入口继续使用已经实测有效的终端信号优雅退出流程。
- FPGA RTL 核对确认 `0xffffffe5` 仅置位采集启动标志，数据通路随后必须检测到外部 HDMI
  `VS` 上升沿才开始传输；顶层 DMA 输入连接外部 `pixclk_in / vs_in / de_in`，不是内部测试图
  发生器。当前 `ready=0` 的首要板端验证项因此是 1280×720 HDMI 输入时序与 FPGA DMA 完成
  状态，而不是 PP-OCR、Qt 或文件打开标志。
- 在 Qt 应用持续占用驱动且复现 `ready=0` 时同步读取 PCIe 状态，确认 RK3568 Root Port 与
  FPGA Endpoint 均稳定为 `5 GT/s x2`，Endpoint 三个 BAR、驱动绑定和 DMA 地址映射均有效，
  两端 `DevSta` 无错误位，未见链路掉线或降速。由此进一步排除 RK3568 PCIe 主链路和用户态
  帧源作为首要根因；下一步直接观察 RTL 已标记调试的 `start_flag`、外部 `VS/DE`、
  DMA 请求/完成和 `i_wr_frame_done`。板端旧 PCIe3 PHY 驱动打印的 `pipe_grf` 信息以及
  `prsnt-gpios/current limit` 设备树警告仍应单独整理，但同次启动已成功建立并保持 Gen2 x2，
  现阶段不据此修改采集协议或 PP-OCR 代码。
- 采集进程保持设备打开并暂停两秒后，直接读取 BAR0 `WR_FRAME_DONE` 状态寄存器
  `0xf0200140` 返回 `0x00000000`；由于用户态/驱动暂停期间不会竞争读取并清除该锁存位，
  已确认 FPGA 在观察窗口内没有产生 `i_wr_frame_done`。下一步先重复写入一次
  `0xffffffe5` 排除启动标志被后续复位清除，再按 `start_flag -> HDMI VS/DE ->
  video_crtl WAIT/TX_DATA -> DMA request/done -> i_wr_frame_done` 顺序进行片上调试，不再修改
  PP-OCR、Qt 或 PCIe 读取重试代码。
- 同板同驱动运行历史 LPR Qt 的 A/B 结果仍为 `last_status=-1 / ready=0`，`EPERM` 约每秒
  931 次，与 PP-OCR Qt 一致，进一步排除本阶段 PP-OCR 级联代码导致零帧；采集恢复前不进入
  YOLO/PP-OCR 性能分析。
- 采集恢复后实测 PCIe 为 `28.10 FPS`，PP-OCR pipeline 为 `13.92 FPS`、失败 0，符合
  `main_ppocr.cc` 中 `kInferenceInterval=2` 的主动隔帧设计；平均完整推理耗时 `57.94 ms`，
  不能把约 14 FPS 直接解释为 Qt 或 CPU 显示拖慢。
- `PrintPerformance()` 在完整性能汇总后显式执行 `fflush(stdout)`，确保第 5 阶段 Qt UI
  通过窗口关闭或终端信号优雅退出时，ADB 终端立即收到 `PCIe Pipeline Statistics`。
- `HandleSignal()` 现显式接收异步信号安全函数 `write()` 的返回值，消除 GCC `warn_unused_result` 编译告警；退出信号处理行为保持不变，该路径不在正常采集、推理或显示热路径中。
- 接口已经对齐：`post_process()` 先去除 YOLO 640x640 letterbox 的 padding，再除以缩放系数，将检测框映射回 1280x720 BGR565 原帧坐标；坐标现统一为半开区间 `[left,right)×[top,bottom)`，左/上向下取整、右/下向上取整，右/下最大允许等于原图宽高，避免紧框或贴边车牌再损失一行/列像素。`yolo_ppocr_pipeline.cc` 直接以该坐标裁剪车牌 ROI，不改变 Tracker 和显示端使用的坐标系。
- PP-OCR 输入是单张 48x160 BGR 图像：ROI 按高度 48 等比例缩放，宽度向上取整且不超过 160，右侧以原始像素值 128 填充；RKNN 输入为 UINT8/NHWC，模型内嵌 `(x-127.5)/127.5` 归一化。输出为 `[1,20,74]`，类别 0 是 CTC blank，其余 73 类按 `cblprd_plate_dict.txt` 解码并去除连续重复。
- `ppocr_rec.cc` 新增 BGR565 原帧 ROI 输入，避免为每个框生成整帧 RGB888；RGB888 图片评估接口保持兼容。主 pipeline 的处理顺序为：CTC 解码 -> `ga36_plate_type_v3` 字符修正 -> 按蓝/绿牌目标位数截断 -> GA 36 类型化校验 -> 有效结果投票 -> 连续命中显示。
- `ga36_plate_type_v3` 将发牌机关代号与序号字符规则分离：普通、警、学、港、澳牌第二位按 `A-Z` 发牌机关处理并允许合法 `I/O`，序号禁止 `I/O` 且最多包含 2 位字母；警牌不再允许数字 `0/1` 作为第二位。使馆牌按“6 位数字 + 使”校验，领馆牌按“省份 + 3 位数字机构编号 + 数字开头的 2 位序号 + 领”校验。
- 新能源牌按 GA 36 表 5/表 6 校验：能源字母允许 `A/B/C/D/E/F/G/H/J/K`；小型新能源要求能源字母位于第 3 位、第 4 位可为 1 位序号字母且其余为数字，大型新能源要求能源字母位于末位且前 5 位序号为数字。部署端保留 `0 -> D` OCR 恢复，但仅在小型/大型候选中恰好一种结构合法时改写；已有合法能源位或两种候选都成立时保持原文，避免把合法序号末位或首位数字 `0` 误改为 `D`。按当前项目范围，不增加 `挂/试/超`。
- `normalize_plate_prediction()` 处理完整 7/8 位文本；`correct_plate_prediction_for_pipeline()` 继续支持超长输出先按类型纠正再截断。使馆牌不再要求省份前缀，领馆和使馆数字区域执行 `O/I -> 0/1`，其余号牌的发牌机关位置执行 `0/1 -> O/I`、序号区域执行 `O/I -> 0/1`。
- 工程目录已改名为 `3_NPU_Yolov8_PPOCR_Demo`；原 `main_pcie.cc` 改名为 `main_lpr.cc`，新的 `main_ppocr.cc` 是 PCIe 主入口，`main_video.cc` 和 `main_picture.cc` 已删除。2026-07-23 起 Qt UI 源码和构建入口统一迁入 `5_QT_UI_Demo`。
- 部署模型固定为 `ppocrv4_rec14_fold_affine_1x1_rk3568_hybrid_mmse_h2_add27_hsw4.rknn`；CMake 只向 PP-OCR PCIe 目录安装该模型、YOLO 模型、73 字符字典和 PCIe 驱动。
- `ga36_plate_type_v3` 已完成主机侧 MSVC 编译和 `plate_rule_test` 回归：覆盖发牌机关 `I/O`、警牌数字第二位拒绝、使/领馆独立结构、普通及专用牌序号字母上限、十个新能源能源字母的大小型位置、`0 -> D` 恢复、超长纠正后截断，以及 `挂` 保持不支持。编译仅报告 `simple_tracker.cc` 原有的整数到浮点转换告警。当前环境没有 aarch64 交叉编译器，仍需 Ubuntu 交叉编译及 RK3568 PCIe 实流复测。
- 两份板端 `details.tsv` 已按“字符修正 -> 位数截断 -> 严格相等”重放验证，且将 `basic/hard` 中的 6157 张 8 位新能源牌单独统计。修复后的 v3 使 hybrid 模型从 `84.208100%` 提升到 `86.904419%`，FP16 模型从 `88.148874%` 提升到 `90.597453%`；完整口径、分组结果和异常样本见 [`results/ga36_plate_type_v3_replay_report.md`](results/ga36_plate_type_v3_replay_report.md)。
- PP-OCR 级联现增加同帧条件回退：主识别仍使用原 YOLO ROI 和训练一致的定高 48、宽度上限 160、右填 128 预处理；仅当“v3 字符修正 -> 位数截断 -> GA 36 校验”仍无效时，将原 ROI 四边各扩张 5%（每边至少 1 像素）并在同一 `PcieFrame`、同一 `ppocr_ctx` 上复推。每帧最多执行 1 次回退，二次结果只有通过 v3 才替换无效主结果，任何有效主结果都不会被覆盖；不重新运行 YOLO、不重新进入帧队列，也不产生第二次 Tracker 更新。
- `yolo_lprnet_crops` 的 17031 条原始预测按当前 v3 重放后，严格结构无效触发率为 `403/17031=2.3663%`，蓝/绿牌分别为 `2.1571%/3.5700%`，403 个触发样本均为错误结果。按 H2 单次 PP-OCR 约 `9.30 ms` 估算，平均额外开销约 `0.22 ms/车牌`；实际 H2 + PCIe BGR565 + 实时 YOLO ROI 触发率仍需板端统计确认。
- 新增 `ppocr_retry_policy_test`，覆盖半开 ROI 边界夹紧、5% 扩框、图像边缘保护以及蓝/绿牌 v3 重试判定；与既有 `plate_rule_test` 均已在主机侧 MSVC 通过。PCIe 性能摘要新增主识别次数、回退次数、回退接受次数和平均回退耗时，便于板端确认触发率、额外时延及推理队列丢帧是否变化。
- 修复“终端有 PP-OCR 字符但 DRM 只显示检测框”：有效号牌仍按 GA 36 v3 校验并进入原投票池；没有有效投票时，仅连续 2 次得到完全相同的非空文本才在画面显示，并以 `RAW` 明确标记为未通过校验，该文本不会进入有效投票或 Qt 状态结果。识别日志新增 `ga36_status=valid/invalid`；关闭每帧成功路径的 `rknn_run`、letterbox `scale/fill` 调试输出，并将 PCIe 等待状态统一写入 stdout，避免 `2>&1 | tee` 时跨线程日志插入另一行中间。主机侧 `plate_rule_test` 已新增 `湘VWUJ3N` RAW 回退用例并通过，仍需重新交叉编译后验证 DRM 字体叠加。

## 2026-07-20 PP-OCRv4 FP16 独立板端验证入口

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
# 2026-07-24 PP-OCR 模型介绍页设计进度

- 已完成源演示文稿审计，并以现有 YOLO 模型介绍页作为唯一视觉模板；输出将保留 YOLO 原页并复制生成 PP-OCR 页。
- 已核对 `1_PC_Training/README_CBLPRD_RK3568.md`、`2_Model_Conversion_PC_Simulation/PPOCR/README.md` 和三份板端 `evaluation.log`，统一采用原始整牌准确率、官方 RKNN 延迟与数据集吞吐率口径。
- PP-OCR 页规划为三层：73 类字符 CTC 微调与结果表、算子折叠/H2 混合量化及延迟图、三种 RKNN 模型的准确率/延迟/吞吐对比表；规划与证据记录位于 `outputs/019f934f-e675-7f60-a4ed-51f27f91b32b/presentations/ppocr-model-intro/`。
# 2026-07-24 PP-OCR 演示页模板复制

- 已按模板帧映射把现有 YOLO 页复制为两页起始稿：第 1 页保持原样，第 2 页作为 PP-OCR 原生对象编辑页。
- 第 2 页的数据口径已固定：训练表使用未经过规则修正的整牌准确率，板端表直接使用三份 `evaluation.log` 的准确率、`RKNN_QUERY_PERF_RUN` 官方延迟和数据集吞吐率。
# 2026-07-24 PP-OCR 演示页排版完成

- 已完成 PP-OCR 页的原生文本、表格和图表改写；中区部署标题保持单行，底部三模型对比表采用等高四行，避免新增行挤压。
- 第 2 页当前展示 73 类 CTC 微调提升、12 组算子折叠、H2 混合量化，以及基线 FP16/折叠 FP16/H2 的准确率、官方延迟、吞吐率和 RKNN 文件大小。
- 模板帧映射与模板一致性检查均通过；布局检查在仅豁免原模板背景图/导航重叠后为 0 error / 0 warning。
# 2026-07-24 PP-OCR 演示页交付

- 最终两页演示文稿已生成：第 1 页保留 YOLO 模型介绍，第 2 页为同风格 PP-OCR 模型介绍。
- PP-OCR 页同时体现官方模型到 73 类 CTC-only 模型的差异、7.31 MiB ONNX（相对官方标称 10 MB 约缩减 27%）、折叠 FP16 与 H2 混合量化的板端性能/精度/大小对比。
- 最终文件：`outputs/019f934f-e675-7f60-a4ed-51f27f91b32b/presentations/lpr-pipeline-layout/output/LPR模型介绍_YOLO与PPOCR.pptx`。
- QA：模板帧映射检查通过，模板一致性检查通过，布局检查 0 error / 0 warning；PPTX 包含 2 个 slide XML、51 个 ZIP 条目、0 个零字节条目。
