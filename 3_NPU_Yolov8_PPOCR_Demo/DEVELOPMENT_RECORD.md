# 3_NPU_Yolov8_PPOCR_Demo 开发记录

## 2026-08-23 图片模式按 generation 与稳定投票抑制 OCR retry

- 仅修改图片 OBB 入口的 PP-OCR 回退决策；视频模式 `process_ppocr_pipeline()` 的主识别、
  无效结果回退、单帧一次 retry 预算和统计口径保持原样。
- 图片 Tracker 新增独立的 retry 判断计数，不改变现有按检测置信度累计的显示投票：同一轨迹的
  候选至少获得 3 个有效计数、占全部有效计数的严格多数，且与当前显示投票赢家一致时，才发布
  稳定区域。图片 generation 变化仍通过原 `tracker.reset()` 同步清空该状态。
- 推理线程只对与稳定区域 IoU 不低于 0.50 的无效主 OCR 候选跳过扩框 retry；同一图片中其他
  未稳定车牌仍保留回退，已抑制候选也不消耗该帧的一次 retry 预算。性能汇总新增
  `Image retries suppressed by stable vote`，逐候选日志使用 `suppressed-stable-vote`。
- `plate_rule_test` 新增三票门槛、generation reset 和视频模式隔离回归；
  `ppocr_retry_policy_test` 新增同区域、不同区域、空状态及无效稳定项回归。
- 两项测试均已用 Visual Studio 2022 x64 编译运行通过：
  `plate_rule_test: all cases passed (ga36_plate_type_v3)`、
  `ppocr_retry_policy_test: all cases passed`。OBB pipeline 的 Windows 对象编译仍受仓库现有
  RKNN 头文件与 `ZERO_COPY` tensor memory 字段不一致限制；最终以 Linux/aarch64 构建为准。

## 2026-08-23 无驱动源码条件下的 read 路径确认

- 当前 `pango_pci_driver.ko` 不是本项目自行编译，仓库、全部 Git 历史及本机相关资料均没有
  匹配的 `pango_pci_driver.c`；模块 DWARF 记录的原始路径为
  `/userdata/car_plate/pango_driver/pango_pci_driver.c`，当前也没有可访问的板端源码副本。
- 对未剥离的 AArch64 模块完成符号、重定位和函数反汇编：`pango_cdev_read()` 只有MMIO完成
  状态读取与一次 `__arch_copy_to_user(buf, dma_buffer, count)`，状态为0时直接返回 `-1`；成功
  路径没有等待循环。模块为多个缓冲分别执行4 MiB `dma_alloc_attrs(..., GFP_KERNEL, attrs=0)`，
  现有 `file_operations` 没有 `mmap` 回调。
- 因缺少源码，本轮不反汇编重造内核模块；Qt工具目录新增只读 ftrace function-graph 脚本，供
  板端在不替换 `.ko` 的情况下拆分成功 `pango_cdev_read`、`copy_to_user` 和剩余驱动时间。

## 2026-08-23 PCIe 采集时序诊断统计

- 在 PCIe 采集线程增加只写型 `steady_clock` 时序累加器；退出汇总现在分别输出成功
  `ReadFrame()` 调用的平均/最大耗时、相邻完整帧读取完成时刻的平均/最大间隔。
- 图片模式额外统计静态变化检测完成到下一次 `ReadFrame()` 开始之间的平均/最大延迟，用于
  隔离变化检测后的队列提交、循环调度和线程抢占是否挤占下一次读取时机。
- 三项统计不改变驱动重试、6 槽帧池、显示/推理队列或每 2 帧推理一次的策略；成功读取统计也
  包含暂停期间为保持 PCIe 消费而读入 drain buffer 的完整帧，`Captured` 原有口径保持不变。
- 四个相关源码/记录文件已通过定向 `git diff --check` 和统计路径源码核对；当前 Windows 环境
  没有 C/C++ 编译器，WSL 也未安装发行版，仍需在 Linux/aarch64 构建环境完成交叉编译和板端
  图片空画面、静态车牌图片、视频模式三组复测。

## 2026-08-23 图片结果快照与实际叠加帧对齐

- 图片模式传给 Qt 的 `image_plate_results` 改为取当前显示线程实际送入叠加器的全部非空车牌
  文字，而不是只传 GA 36 有效结果；未通过校验但已在画面显示的诊断文字同步使用
  `RAW <类型>`，使结果表与同一 RGBA 帧上的可见车牌标签保持一致。
- 视频模式原有单个有效车牌状态、Tracker 投票、图片 generation、推理频率和画面叠加逻辑
  均保持不变；本次只调整图片模式的 Qt 结果快照语义。
- 相关源码和记录已通过定向 `git diff --check`；当前 Windows 验证环境不能编译包含
  `pthread/unistd` 的 PCIe 后端，仍需在既有 Linux/aarch64 环境完成整包交叉编译和板端复测。

## 2026-08-21 答辩总体架构页美化方案启动

- 已审计根目录 `答辩v6.pptx`：16:9、32 页，当前视觉身份以 `#0068E3`、浅蓝和深蓝灰为主，
  中文主体字体为微软雅黑。已抽取源稿文字和图片清单，原始演示文稿未修改。
- 本轮仅重构“总体架构设计”单页的信息层级与版式：保留顶部章节导航；将内容收束为
  “HDMI 输入 → FPGA 流处理 → RK3568 智能识别 → UI 显示”的单向链路，并以简约、写实的
  工程线条替代当前多重描边和分散箭头。最终配色与可编辑输出待方案确认后生成。
- 已确认目标为第 5 页：FPGA 侧包含解码、可调预处理、FIFO、DMA 分包与 MWR 传输；RK3568
  侧包含帧缓冲、RGA、YOLOv8n、ROI 裁剪与 PP-OCRv4。建议以四段横向流程和两段设备容器呈现，
  仅保留一次 PCIe 跨域箭头，右侧成果图作为最终 UI 节点的真实输出证据。
- 已收到生成确认并新建独立 Quick 工作区
  `outputs/architecture-flow-slide_ppt169_20260821/`。输出保持为单页可编辑 PPTX；截图中的
  顶部章节栏逐字保留，正文按单向工程数据流重绘，右侧使用项目既有的高清 UI 实拍作为成果图层。
- 已完成单页交付：`FPGA` 与 `RK3568` 改为浅色设备边界，核心路径依次为 HDMI 输入、MS7200
  解码、预处理/旁路、FIFO/DMA/MWR、PCIe、帧缓冲/RGA、YOLOv8n、ROI/PP-OCR 和 UI 显示。
  导出文件为 `outputs/architecture-flow-slide_ppt169_20260821/exports/architecture-flow-slide_20260821_144957.pptx`。
- QA：Quick SVG 质量检查无阻断错误，导出 postflight 为 `passed-with-warnings`（仅为多行文字框的
  保守估算提示）；已用本地 PowerPoint 16.0 渲染为 PNG 目检，未见裁切、遮挡或空白图层。
  最终文件为 1 页、8 个顶层可编辑对象，SHA-256 为
  `A38E91B8EFEE2E92737341A8994AB1EEAD35BBF4177C4804115F8E51F6FC2241`。

## 2026-08-20 形成 OBB 训练、转换与部署综合报告

- 新增 `YOLOV8_OBB_TRAINING_CONVERSION_DEPLOYMENT_REPORT_20260820.md`，以当前训练
  记录、转换脚本、生产 pipeline 和 RK3568 原始性能日志为依据，统一记录 epoch 54 冻结测试
  指标、PT/ONNX/RKNN 产物、800 张量化校准集、W8A8 零拷贝部署和 OBB 到 PP-OCR 输入的
  完整处理流程。
- 报告明确生产 PP-OCR 输入为 `UINT8 BGR NHWC [1,48,160,3]`，`[1,20,74]` 为输出；
  当前显示仍使用 OBB 四角点 `min/max` 外接矩形，不把内部四角点支持误写为显示层多边形框。
- RK3568 level 0 实测口径为纯 NPU `31.7786 ms / 31.468 FPS`、完整 pre-OCR
  `38.6083 ms / 25.901 FPS`；另行标明生产阈值 `0.55/0.55` 与项目 6 早期性能日志
  `0.25/0.40` 的差异，不以性能基准替代精度结论。
- 交付检查使用 MSVC `/utf-8 /std:c++17` 从当前源码重新编译并直接运行 6 个主机测试；
  车牌规则、视频 Tracker、PP-OCR retry、静态换图检测、OBB 后处理和静态 Tracker 全部通过。
  OBB 回归输出 2 个跨颜色候选、`48x160x3` BGR 输入和有效宽度 96；静态 Tracker 最终
  保持 1 条轨迹，左边界波动范围为 1 px。

## 2026-08-20 答辩第 10 页更新为图片模式 YOLOv8-OBB 流程

- 基于答辩单页源文件 `tmp-ppt.pptx` 原位改写第 10 页流程图，保持原有电路背景、顶部导航、
  四条泳道、配色、圆角节点和箭头版式不变；输出为
  `outputs/manual-20260820-obb-slide10/presentations/defense-obb-flow/output/答辩第10页_YOLOv8-OBB流程.pptx`。
- 检测泳道已更新为 Letterbox、YOLOv8n-OBB RKNN、native INT8 解包/反量化、DFL/角度解码、
  旋转 NMS 和四角点输出；识别泳道已更新为四角回映射、旋转矫正、`48×160` 输入构建和
  蓝绿重叠候选颜色证据裁决。
- 后处理泳道明确 GA36 的 7/8 位结构校验、最多一次异常扩框回退，以及 OBB 类别与尾字符联合
  判型；显示泳道改为图片模式的 OBB 外接矩形、IoU `0.65` 候选合并、`2 px` 死区与 `0.15`
  平滑，不再展示视频 Alpha-Beta 运动预测。
- 最终文件为 1 页、1,211,168 字节，媒体空文件数和空占位符数均为 0；模板保真检查通过且
  artifact-tool 全尺寸渲染无可见越界或遮挡。SHA-256 为
  `F68CC4927F2FF7A17A325E040A2D62B13D932DA7F1379FA00923E4A72B454B97`。

## 2026-08-19 删除颜色不明确时的 OCR 后兜底

- 按用户要求，蓝绿冲突进入像素颜色裁决后，若蓝/绿证据未达到 1.25 倍明确门限，则两个候选
  同时标记为 suppressed，不再送入 PP-OCR、GA 36、Tracker 或显示层；终端输出
  `OBB color conflict dropped ... selected=none`。
- 删除静态 Tracker 中“检测分差 `<=0.10` 且蓝绿结构均合法时优先绿色 8 位”的特殊规则；
  Tracker 恢复仅按合法性、OCR 置信度和检测置信度处理其他普通重叠结果。
- 删除后 `yolo_ppocr_pipeline.cc` 编译通过，完整 6 项 CTest 全部通过。

## 2026-08-19 蓝绿颜色冲突分差门限最终放宽至 0.10

- 按用户最终要求，像素颜色裁决和 OCR 后 Tracker 兜底的蓝绿检测分差门限均由 `0.05`
  放宽为 `0.10`。IoU `0.65`、颜色证据 1.25 倍、OBB `conf/NMS=0.55` 均不变。
- 边界回归同步改为分差 `0.09` 必须进入裁决、分差 `0.11` 不得强制绿色优先。
- `yolo_ppocr_pipeline.cc` MSVC 编译通过，完整 6 项 CTest 全部通过。

## 2026-08-19 蓝绿颜色冲突分差门限放宽至 0.05

- 按用户要求，蓝/绿高重叠候选触发颜色像素裁决的检测分差由 `<=0.01` 放宽为 `<=0.05`；
  OCR 后 Tracker 结构兜底同步使用 `0.05`，避免颜色证据不明确时两个相邻 INT8 量化档位
  再次建立双轨迹。
- IoU `>=0.65`、颜色证据 1.25 倍门限、OBB `conf/NMS=0.55` 与视频模式参数均不变。
- 边界回归使用检测分差 `0.04` 验证进入绿色 8 位裁决，使用分差 `0.06` 验证不触发强制绿色
  优先；完整 6 项 CTest 全部通过。

## 2026-08-19 蓝绿近重叠候选增加矫正图颜色裁决

- 按板端实测要求，蓝/绿框轴对齐 IoU `>=0.65` 且检测分差 `<=0.05` 时，在 PP-OCR 前统计
  已旋转矫正、未填充区域内的蓝/绿色饱和像素。忽略最大通道低于 50 或色差小于 24 的暗色/
  灰白文字像素；一方累计颜色证据至少为另一方 1.25 倍且达到最低证据量时才选色。
- 颜色证据明确时只保留对应蓝/绿候选进入 PP-OCR；证据不明确时不强判，两个候选继续进入
  OCR 后 Tracker 结构兜底。颜色冲突日志新增 `blue_evidence/green_evidence/selected`，用于板端
  直接核查 BGR565 色序及阈值。
- `green=0.276` 的日志低于当前最终 `conf=0.55`，新构建不会把该候选送入 OCR；若重新部署后
  仍出现该行，应先核对启动日志和可执行文件是否仍为旧 `conf=0.25` 版本。
- 颜色分析的合成绿/蓝/中性输入回归、问题样例两种候选顺序、静态框稳定和视频运动预测等
  6 个 CTest 全部通过；`yolo_ppocr_pipeline.cc` 通过 MSVC 19.41 编译。

## 2026-08-19 INT8 蓝绿等分冲突改为 OCR 后裁决

- 板端问题日志显示同一车牌同时输出 `blue/green=0.955`，旋转角和框几乎相同；蓝分支把
  8 位 OCR 截成 7 位后仍通过 GA 36，绿分支保留正确 8 位 `皖AF00166`，因此两者此前可各自
  建立轨迹和文字板。
- 移除 OCR 前跨颜色 NMS。INT8 把颜色 logits 量化为同分时，前置 NMS 只能按不稳定排序保留
  一个类别，可能直接删掉正确绿色候选；当前只对同类别执行旋转 NMS `0.55`。
- 图片 Tracker 继续以轴对齐 IoU `0.65` 合并物理同框候选；若蓝/绿检测置信度差不超过
  `0.05`、两种长度结构均通过 GA 36，则优先保留合法绿色 8 位结果。非同分候选仍按合法性、
  PP-OCR 置信度和检测置信度选择；蓝先/绿先两种候选顺序均必须得到相同结果。
- 已按问题日志坐标、`0.955` 检测分数及一高一低 OCR 置信度建立回归；蓝先和绿先两种输入
  顺序均只输出 `皖AF00166/绿`。OBB 跨类别保留、静态 Tracker、视频运动 Tracker 等 6 个
  CTest 全部通过。

## 2026-08-19 OBB 部署阈值最终指定为 0.55

- 按用户最终决定，图片 OBB 置信度与同类别旋转 NMS 均由上一版 `0.70` 调整为 `0.55`。
  Tracker 轴对齐 IoU `0.65` 和视频模式参数均保持不变；跨颜色前置 NMS 随后按上一节板端
  等分日志改为 OCR 后裁决。
- epoch 54 测试曲线的 `conf=0.705` 仍作为离线总体 F1 峰值记录保留，不再等同于当前板端
  部署工作点。

## 2026-08-19 OBB 冻结测试集最佳阈值落地

- 最终 epoch 54 的冻结 test 共 9,231 张、11,010 个实例；`BoxF1_curve.png` 明确给出总体
  `F1=0.94 @ confidence=0.705`。图片 OBB 部署置信度由通用默认 `0.25` 调整为四舍五入后的
  `0.70`。
- 冻结测试使用的 NMS IoU 为 `0.70`、`agnostic_nms=False`，因此同类别旋转 NMS 从 `0.40`
  对齐为 `0.70`。跨颜色去重不是训练指标，为只消除近乎同框的互斥颜色候选并减少误删真实
  邻近车牌，阈值由 `0.65` 收紧为 `0.80`；Tracker 的轴对齐 IoU `0.65` 二次兜底不变。
- 本次只修改图片 OBB pipeline；视频微调 YOLO 的 `BOX_THRESH=0.55/NMS_THRESH=0.5` 不变。

## 2026-08-19 图片静态框抖动与重叠双框修复

- 根因一：图片模式虽然按 generation 隔离结果，但此前仍以 2 参数接口调用视频 Tracker，
  OBB 每次推理的轻微坐标差会进入速度/加速度估计和显示外推，固定车牌因此仍可能抖动。
- 根因二：OBB 的四个类别是同一物理车牌的互斥颜色标签，原旋转 NMS 却只抑制同类别候选；
  同一位置若同时命中两个颜色类别，会留下两个结果并各自建立 Tracker/文字结果。
- Tracker 已接通现有测试所期望的 `static_image_mode` 参数：图片模式关闭运动外推、忽略显示帧
  间隔寿命、以 `2 px` 死区和 `0.15` 增益平滑固定框，并在建轨前以轴对齐 IoU `0.65`
  去除近重叠检测；视频模式继续使用原速度、加速度和 8 帧寿命参数。
- OBB 旋转 NMS 已增加跨类别 IoU `0.65` 去重；同类别仍使用原 `0.4` NMS，避免为了去除颜色
  重复框而过度合并邻近的不同车牌。
- 新增静态专用回归以两组重叠检测和 `-6～+6 px` 连续抖动输入验证：最终始终只有 1 条轨迹，
  确认后的左边界波动范围为 1 px。包括视频高速初联/短空窗/加速度预测在内的 6 个 CTest
  全部通过，原先 3 参数静态测试与 2 参数 API 不一致的问题也随静态模式接口接通而闭合。

## 2026-08-19 图片模式 OBB 接入方案确认

- 已核对项目 6 的板端验证逻辑与项目 3 当前 PCIe pipeline：图片模式将独立使用
  `yolov8_obb_i8.rknn` 的四路原生 INT8 输出，复用零拷贝输入、native output 解包、
  64 通道 DFL、角度解码和旋转 IoU NMS；视频模式继续使用现有微调 YOLO 与轴对齐后处理。
- PCIe 输入仍为 `1280×720` 小端 BGR565。OBB 四角点映射回原图后先旋转矫正为 PP-OCR
  所需的 `48×160`、右侧填充 128 的连续 BGR 输入，再直接送入 PP-OCR，避免把倾斜车牌重新
  压回普通轴对齐 ROI。
- 当前 `SimplePlateTracker`、`PipelineResult` 和 `RgaOverlayRenderer` 都只接受轴对齐矩形；
  本次显示采用 OBB 四角点的 `floor(min)/ceil(max)` 半开包围框，继续复用现有矩形描边与水平
  文字背景板。这样不增加逐框多边形栅格化和旋转文字板混合开销，且不改变视频显示热路径。
- 已新增生产侧 `obb_postprocess`、图片 OBB pipeline 与 prepared-BGR PP-OCR 入口；OBB 初始化
  会强制校验输入 `640×640×3`、三路 `[1,68,80/40/20,80/40/20]` 和角度
  `[1,1,8400]` 的 INT8/非对称量化契约，避免普通 YOLO 或错误输出顺序误入图片路径。
- `main_ppocr.cc` 仅在 `image_mode=true` 时初始化、执行、释放 OBB pipeline；视频分支原
  `init/process/release_ppocr_pipeline` 调用保持不变。当前进入主机编译与合成 tensor 回归阶段。
- 合成回归已通过：覆盖 67,200 字节 native NC1HWC2 角度缓冲解包为逻辑 8,400 字节、两个
  重叠候选的旋转 NMS、PCIe BGR565 到 `48×160×3` BGR 输入（有效宽 96、右侧填充）以及
  OBB 四角最大包围框 `(68,76,100,92)`。`plate_rule_test`、`ppocr_retry_policy_test` 和
  `static_image_change_detector_test` 也通过。
- MSVC 19.41 已分别编译 `obb_postprocess.cc`、`ppocr_rec.cc` 和
  `yolo_ppocr_pipeline.cc`；项目 3/5 CMake 在 `TARGET_SOC=rk356x` 下配置和生成通过。
  现有 `simple_tracker_motion_test` 自身仍以 3 参数调用当前 2 参数 Tracker API，构建失败与本次
  OBB 改动无关，未越界修改 Tracker；Linux aarch64 全目标编译和 RK3568 实流仍待执行。
- 性能决策沿用项目 6 的 level 0 实测：OBB NPU `31.78 ms`、native 解包 `2.61 ms`、
  解码/旋转 NMS `2.92 ms`、OCR 旋转矫正 `1.30 ms`，pre-OCR 合计 `38.61 ms`。显示仍走既有
  轴对齐矩形叠加，因此不会引入多边形栅格化或旋转文字板的额外热路径。

## 2026-08-17 微调 YOLO RKNN 改为项目 5 唯一部署模型

- 板端测试确认 `model/finetune_i8.rknn` 可直接用于 Qt 图片/视频识别；项目 5 已取消原版与
  微调版双包，唯一包安装时把该文件重命名为运行时契约 `model/yolov8.rknn`。
- 本目录原 `model/yolov8.rknn` 保留给项目 3 独立命令行 Demo，项目 5 构建系统不再引用；
  两个模型文件本身及项目 3 推理源码均未改动。

操作和部署步骤见 [README.md](README.md)。

## 2026-07-27 图片模式多车牌结果传递

- 修复图片模式画面可叠加多个有效车牌框、但 Qt 右侧“当前图片识别结果”始终只有一行的问题。
  根因是 `PcieUiStatus` 只有单个 `plate_text/plate_type/plate_confidence`，显示线程遍历
  `display_results` 时取到第一个有效车牌后立即结束，后续有效跟踪结果未进入 Qt 状态。
- 新增 `PcieUiPlateResult` 和仅供图片模式使用的 `image_plate_results`；图片模式现在把当前
  generation 内所有经过 Tracker 连续 2 次确认且通过 GA 36 校验的结果一起交给 Qt。原单结果
  字段继续保留并只取第一个有效结果，确保视频模式现有去重和结果表行为不变；YOLO、PP-OCR、
  Tracker 参数及图片换代隔离逻辑均未调整。
- MSVC 19.41 已完成项目 5 `main_pcie_qt.cc` 对象编译；`plate_rule_test`、
  `simple_tracker_motion_test`、`ppocr_retry_policy_test` 和
  `static_image_change_detector_test` 全部通过。当前 Windows 环境没有 WSL/aarch64 交叉工具链，
  仍需在 Linux 构建机重新交叉编译项目 5，并在 RK3568 图片模式用多车牌静态画面复测结果行数。

## 2026-07-27 板端复测后的推理延迟运动补偿

- 板端长时统计为采集/显示 `27.41/27.38 FPS`、推理 `13.60 FPS`、平均推理
  `56.15 ms`，显示转换与叠加仅 `7.42/0.31 ms`，无显示、叠加或推理失败；推理队列
  丢 46/6165（0.75%）。因此框仍落后不是 Qt/显示吞吐问题：56.15 ms 相当于 1.54 个
  显示帧，结果通常由约 2 帧后的显示画面消费，偶发排队时接近 4 帧。
- 上一版离线回放在检测结果产生帧立即更新 Tracker，低估了板端异步延迟。改为分别延迟
  2/4 帧重放后，原等速模型在高速视频的中心平均/95 分位归一化误差为
  `0.131/0.362` 和 `0.264/0.607`，平均框 IoU 仅 `0.499/0.224`，与板端“定位跟不上”
  一致。
- Tracker 现以每条轨迹最近两次真实检测观测的帧号和框状态直接计算观测速度；空检测结果
  只推进运动状态，不再改变速度采样时间基准。在位置/速度之外增加受限加速度，按
  `position=0.90`、`velocity=0.80`、`acceleration=0.35` 融合，并把各轴加速度限制为
  当前框尺度的 12%/帧²。显示外推和检测关联统一使用同一常加速度状态，原高速首联和
  8 帧最大寿命保持不变。
- 同一延迟回放中，高速视频 2 帧延迟误差降到 `0.068/0.202`、平均 IoU 提高到 `0.701`；
  4 帧延迟误差降到 `0.121/0.346`、平均 IoU 提高到 `0.554`。正常视频对应误差仅由
  `0.006/0.024` 到 `0.007/0.026`、由 `0.011/0.042` 到 `0.014/0.044`。新增
  2/4 帧加速目标显示预测回归。性能摘要新增 `Tracker result lag average/max`，仅在显示线程
  首次消费新推理结果时统计结果帧与显示帧差，用于板端直接验证常态及峰值延迟。MSVC 19.41
  主机回归通过：`simple_tracker_motion_test`、`plate_rule_test`、
  `static_image_change_detector_test`、`ppocr_retry_policy_test` 全部通过；没有新增编译错误，
  仍需重新交叉编译项目 5 并在 RK3568 上复测。

## 2026-07-27 视频高速车牌跟踪抗甩框

- 使用 `D:\ffmpeg-8.1-essentials_build\bin\ffmpeg.exe`/`ffprobe.exe` 对比两段
  回归视频：`test/testvideo/testvideo1.mp4` 为 640×362、固定 24 FPS；
  `test/testvideo/other/testvideo1.mp4` 为 3840×2160、约 29.69 FPS。后者为车辆
  朝镜头接近的俯视道路画面，车牌中心下移和框尺寸增长均明显快于前者。
- 使用部署同源旧 `best.pt`、板端一致 `conf=0.55/NMS=0.5`，按每 2 帧一次并统一映射到
  1280×720 后离线回放：高速视频 520 个采样帧中 142 帧有检测，存在连续检测空窗；典型
  高速首联中心跨度为 64 px 和 133 px，对应原归一化 DIoU 得分 0.343 和 0.227，低于
  原固定门限 0.35。原 `predict()` 又只在 `time_since_update == 0` 时输出，因此首次漏检
  就会隐藏已确认框，`max_age_frames_` 没有真正用于短时预测显示。
- `SimplePlateTracker` 的常规 DIoU 门限由 0.35 调到 0.30；只对尚未达到 2 次命中且宽高
  比均不低于 0.65 的年轻轨迹开放 0.20 首联门，避免全局放宽后把远处另一辆车接入旧轨迹。
  Alpha-Beta 增益由 0.70/0.40 调到 0.85/0.60，短空窗预测寿命由 5 帧调到 8 帧，并按
  `time_since_update + display_dt` 限制实际显示年龄；超过寿命的轨迹不再参与当前帧关联，
  宽高预测增加 1 px 下限，防止过期轨迹复活或外推产生反向框。
- 新增独立 `simple_tracker_motion_test`，覆盖高速大跨度首联、8 帧短检测空窗继续预测、
  超龄隐藏，以及尺寸差异过大的新目标不得借用高速首联门。MSVC 19.41 主机回归通过：
  `simple_tracker_motion_test: all cases passed`、`plate_rule_test: all cases passed
  (ga36_plate_type_v3)`、`static_image_change_detector_test: all cases passed`、
  `ppocr_retry_policy_test: all cases passed`；没有新增编译错误，仅保留
  `simple_tracker.cc` 原有整数转浮点告警。RK3568 + FPGA 两段实流仍待重新交叉编译部署后
  复测。
- 用相同 2 帧采样检测序列离线重放前后 Tracker：正常视频轨迹初始化数 9→8、预测中心
  平均/95 分位归一化误差 0.007/0.025→0.010/0.025，未出现尾部误差退化；高速视频轨迹
  初始化数 23→19、成功关联 133→137，预测中心平均误差 0.270→0.193（降低 28.5%）、
  95 分位 0.730→0.582（降低 20.3%）。高速视频有框的 2 帧采样时刻由 121→187，其中
  61 个是检测空窗内的受限预测，不是降低 YOLO 置信度得到的新检测。

## 2026-07-25 静态图片代际隔离与两次确认

- 图片模式原先直接复用最近一次单帧推理结果：显示帧已切换到新图片时，异步推理结果仍可能属于
  上一张图片；同一静态图片的单帧 PP-OCR 波动也会立即覆盖当前文字，分别造成旧车牌短暂残留和
  正确/错误结果跳变。
- 首次板端日志出现 `Static image generation: 2410...2416` 逐帧递增，但 PP-OCR 连续稳定输出
  同一合法车牌；由此确认原“4 个单像素采样点变化即换图”的阈值会把 HDMI/BGR565 帧间微扰
  误判为换图，Tracker 因每帧重置而永远不能达到连续 2 次命中。
- `StaticImageChangeDetector` 已改为仅在图片模式下按 16 像素网格提取 `2×2` BGR565 块均值，
  以块均值颜色距离过滤离散像素抖动；显著变化块必须达到至少 12 个（更大输入按全部采样块的
  0.2% 上调），且候选新图连续 2 帧彼此一致，才递增图片 generation。显示线程随后重置
  `SimplePlateTracker`，并拒绝 generation 不匹配的旧推理结果。
- 首次抗噪版本实测图片模式只有 `23.12 FPS`，而显示和 Qt 均无丢帧，确认瓶颈位于同步采集路径
  中每帧约 230,400 像素的 `8/4` 块采样。现改为 `16/2`，每帧只读取约 14,400 像素，理论工作量
  降低 16 倍；性能汇总新增 `Average static image change detection`，用于板端直接核对检测开销。
- 图片模式改为复用视频模式的连续 2 次相同文本确认和合法结果投票，但每次图片 generation
  变化都会清空投票池。因此单帧错误不再造成跳变，上一张车牌也不会依靠历史累计票数滞留；
  新图合法车牌在两次独立推理一致后显示。
- `PcieUiStatus` 新增 `image_generation`，供 Qt 在新图首个显示帧到达时立即清空上一张图片的
  结果表，不再等待下一次推理或 250 ms 状态节流。新增静态图片变化检测回归和 Tracker
  generation 重置回归；原无状态 `build_single_inference_plate_results()` 已随图片模式重新
  接入 Tracker 而删除。
- 本机 MSVC 回归通过：
  `static_image_change_detector_test: all cases passed`、
  `plate_rule_test: all cases passed (ga36_plate_type_v3)`、
  `ppocr_retry_policy_test: all cases passed`。完整 Linux/RKNN 后端仍需在 Ubuntu aarch64
  交叉编译，并在 RK3568 + FPGA 上确认同一图片 generation 保持不变、真实换图只递增一次，
  再实测两次确认延迟和长时间稳定性。

## 2026-07-24 准确度展示新增 PP-OCR FP16 分组表

- 基于 `results/ga36_plate_type_v3_replay_report.md` 的 `fold affine FP16` 分组明细，生成 `outputs/019f93e4-9213-7970-9528-2e5cd0f225a2/presentations/pcie-async-performance/output/PCIe准确度展示_含PPOCR表.pptx`。保留左侧 YOLO 准确度指标，在右侧新增 11×7 原生 PowerPoint 表格，覆盖 basic/hard 绿牌与非绿牌以及使、学、港、澳、警、领共 10 个子集/类型。
- PP-OCR 总体 v3 后正确率采用报告实测 `90.597453%`，展示为 `90.6%`；各行百分比四舍五入到 1 位，样本数、改对、误改和改后仍错保持原始整数。
- 警牌报告实测为原始 `59.090909%`、v3 后 `54.545455%`、误改 2，未将实测值伪造为约 80%；页面另设“优化目标 ≈80%（非实测）”说明，以区分当前证据和后续目标。
- 模板一致性检查通过；最终包包含 1 个 slide XML、2 个原生表格 graphicFrame、1 个非空媒体和 0 个空页级占位符。

## 2026-07-24 PCIe 性能展示右侧改为原生 PPT 元素

- 新增 `outputs/019f93e4-9213-7970-9528-2e5cd0f225a2/presentations/pcie-async-performance/output/PCIe异步并行性能展示_原生元素版.pptx`。右侧资源/效率面板不再使用整张 PNG/SVG，而是在原 449×331 内容边界内用 28 个可编辑 PowerPoint 文本框、圆角矩形和分隔线重建。
- 保留 NPU 71% 运行快照、模型文件 7.07 MiB、内存 41.0%、显示链路达成 99.7%、AI 调度达成 99.1%、H2 准确率 84.21% 与吞吐率 101.96 牌/s等全部既定口径；左侧异步并行关系和模型耗时未修改。
- 对象检查确认右侧 449×331 整图已删除，最终仅保留 6 个模板/图标图片对象；模板一致性检查通过，PPTX 包为 1 个 slide XML、6 个非空媒体、0 个空页级占位符。

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

## 2026-07-27 微调 YOLO RKNN 独立副本

> 此处记录原双版本方案；项目 5 当前部署方式已由上方 2026-08-17 单版本方案取代。

- 将
  `../2_Model_Conversion_PC_Simulation/yolov8/model/finetune_i8.rknn`
  复制为 `model/finetune_i8.rknn`，文件大小为 4,634,120 字节，SHA-256 为
  `E11C5A8E69C34EC2FC3DA45C81A070EECC88D177EE75374830D861DCF19CA30F`。
- 原 `model/yolov8.rknn` 未覆盖，仍为独立的 4,633,800 字节文件；项目 3 命令行
  demo 的默认模型与构建安装行为不变。
- 新文件仅由项目 5 的
  `yolov8_ppocr_pcie_qt_ui_finetune_demo` 独立安装包使用；安装时改名为该目录内的
  `model/yolov8.rknn`，使图片和视频模式均按既有应用目录契约加载微调模型。
- 新旧 Qt demo 均保持 `BOX_THRESH=0.55`，未修改项目 3 的 C/C++ 源文件或
  `include/postprocess.h`。

## 2026-08-17 main_ui 图片识别处理逻辑回退

- 在 `main_ui` 分支反向应用提交 `0ae6e20`，撤销静态图片模式专用的 Tracker
  计龄、运动外推与结果保留逻辑；`simple_tracker.h`、`simple_tracker.cc` 和
  `main_ppocr.cc` 的处理代码恢复为 `origin/main` 当前版本。
- 回退范围仅限上述图片识别/追踪处理逻辑；答辩材料提交保持不变，本地
  `yolo_obb` 分支的 OBB 模型训练脚本、配置、数据集记录及未提交工作区均未改动。
- 使用 Visual Studio 2022 x64 工具链重新编译并运行远程分支实际跟踪的
  `plate_rule_test`，结果为 `all cases passed (ga36_plate_type_v3)`；三个处理代码
  文件与 `origin/main` 的逐文件差异为零。

## 2026-08-21 车牌叠加标签精简

- 车牌叠加标签不再绘制置信度百分比，仅保留车牌号、类型与既有的 `RAW` 有效性标识；
  标签缓存键同步移除置信度，避免同一文本因置信度变化重复生成精灵。
- RGA/Qt 实时标签与直接图像绘制路径的字号统一上调：车牌号从 28 px 提至 32 px，
  类型从 22 px 提至 26 px；小分辨率下原有最低 8 px 的自适应缩小逻辑保持不变。
- 已通过源码检索和 `git diff --check` 验证：标签格式化字符串不再包含置信度百分比，两个
  绘制路径均使用 32/26 px。当前 Windows 工作区没有 CMake、C/C++ 编译器或既有 CMake
  构建目录，未执行交叉编译；需在 RK3568 的既有 Ubuntu 构建环境中完成最终运行验证。
