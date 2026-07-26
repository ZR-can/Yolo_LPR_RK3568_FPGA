# 5_QT_UI_Demo 开发记录

## 2026-07-27 图片模式结果栏支持多车牌

- 修复“图片识别”画面已有多个有效车牌框、右侧“当前图片识别结果”却只显示一张的问题。
  项目 3 后端现通过 `PcieUiStatus::image_plate_results` 传递当前图片 generation 的全部有效
  跟踪结果；Qt 在 generation 变化或新的推理结果到达时清空旧表，并按车牌、类型、PP-OCR
  置信度逐行重建当前图片结果。
- 修改严格限定在图片模式：视频模式继续使用原有单结果字段、历史车牌去重和逐次追加逻辑；
  行人违法模式不受影响。无有效车牌时图片结果表保持为空，换图时仍立即清除上一张图片结果。
- MSVC 19.41 已成功编译 `main_pcie_qt.cc` 为对象文件；共享后端的
  `plate_rule_test`、`simple_tracker_motion_test`、`ppocr_retry_policy_test` 和
  `static_image_change_detector_test` 全部通过。当前主机没有可用的 WSL/aarch64 交叉构建环境，
  板端部署前仍需在 Linux 构建机执行项目 5 完整交叉编译。

## 2026-07-27 板端复测后的车牌推理延迟补偿

- 板端统计确认 Qt painted `27.37 FPS`、后端显示 `27.38 FPS`、平均 Qt 准备/交接
  `5.01 ms`，无显示或叠加失败；平均推理 `56.15 ms` 相当于约 1.54 个显示帧。因此
  视频模式框落后来自共享车牌 Tracker 对 2–4 帧异步结果延迟的等速补偿不足，不修改
  Qt 绘制线程、显示队列或每 2 帧推理调度。
- 项目 3 的共享 Tracker 已改为按真实检测观测间隔估速，并加入按框尺度限幅的加速度状态；
  空检测结果不再污染速度时间基准。延迟检测序列回放中，高速视频 2 帧延迟平均误差
  `0.131→0.068`、IoU `0.499→0.701`，4 帧延迟平均误差 `0.264→0.121`、
  IoU `0.224→0.554`；正常视频误差变化不超过 0.003。Qt 源码无需同步修改，但必须重新
  交叉编译项目 5 才会包含新的 `simple_tracker.cc`。后端性能摘要同步增加
  `Tracker result lag average/max`，供板端确认实际结果延迟是否落在本轮验证的 2–4 帧范围。
  共享后端的运动、车牌规则、静态图片代际和 PP-OCR 回退四组 MSVC 主机测试均已通过。

## 2026-07-27 视频模式高速车牌抗甩框

- Qt `视频识别` 直接编译项目 3 的 `simple_tracker.cc`，因此本轮共享获得高速车牌修复：
  常规 DIoU 门限 0.35→0.30；只为未确认且宽高比均不低于 0.65 的轨迹开放 0.20 高速
  首联门；Alpha-Beta 增益 0.70/0.40→0.85/0.60；已确认轨迹可在最多 8 个采集帧的检测
  空窗内继续受限外推，超龄轨迹既不显示也不重新关联。
- 部署同源旧 `best.pt` 按板端 `conf=0.55/NMS=0.5`、每 2 帧检测序列离线回放后，高速
  视频轨迹初始化数 23→19、关联数 133→137，中心平均/95 分位归一化误差
  0.270/0.730→0.193/0.582；正常视频 95 分位保持 0.025。共享后端的
  `simple_tracker_motion_test`、`plate_rule_test`、`static_image_change_detector_test` 和
  `ppocr_retry_policy_test` 已通过 MSVC 主机回归；Qt UI 源码本身未改，仍需重新交叉编译
  项目 5 并在 RK3568 + FPGA 上播放两段原视频复测。

## 2026-07-27 运行提示修复与轻量资源监控

- 右侧主运行提示改为只服从Qt采集开关，不再显示后端单次PCIe重试消息：运行时固定为
  “正在采集”，暂停时固定为“等待PCIe帧数据”。这避免正常 `EPERM` 重试消息造成两个状态
  来回跳变，也避免暂停后被已排队的旧 `capturing=true` 状态覆盖。相同提示不会重复写入
  `QLabel`；保存成功/失败反馈保留2秒，随后自动恢复当前运行/暂停主状态。
- 左下 1280×360 区域改为上下两层：上层压缩运行状态/PCIe状态，下层新增“系统资源
  （最近60秒）”。资源栏以 1 Hz 读取 CPU 温度、NPU 负载和内存占用，各保留60个样本并使用
  Qt 原生 `QPainter` 绘制细柱历史；不引入 Qt Charts、OpenGL、外部进程或采样线程。
- CPU 温度读取 `/sys/class/thermal/thermal_zone*/temp`，NPU 优先读取
  `/sys/kernel/debug/rknpu/load` 并兼容 RK3568 devfreq 路径，内存使用
  `/proc/meminfo` 的 `MemTotal/MemAvailable`。指标不可读时显示 `--`，不影响视频采集。
- Qt `uic` 和离屏布局验证通过：紧凑状态行位于 `(12,732)`、尺寸 `1256×154`，资源组位于
  `(12,896)`、尺寸 `1256×172`，内部图表画布为 `1222×114`；5行运行状态文字均未裁切。
  Qt主入口、样式和新监控控件均通过 MSVC 19.41 UTF-8语法编译，启动脚本 `bash -n` 与
  `git diff --check -- 5_QT_UI_Demo` 通过。仍需在RK3568上确认实际thermal/NPU节点读数并复测FPS。
- 2026-07-27板端统计为：Captured/Display pipeline/Display handoff均 `3703 / 28.28 FPS`、
  Qt painted `3702 / 28.24 FPS`，帧池和显示队列丢帧均为0；推理 `1834 / 14.00 FPS`、
  推理队列丢18，符合每2帧推理一次。Qt帧准备/交接平均 `5.03 ms`，远低于28 FPS对应的
  `35.36 ms`帧周期。`EPERM=3915`期间仍持续完整交付3703帧，因此只作为驱动正常等待重试，
  不再驱动右侧主状态。状态优先级修改已再次通过 `uic`、MSVC语法编译和`git diff --check`。

## 2026-07-26 恢复红灯入区判定与 YOLO 风格灯框

- 按最新需求撤销上一版行人移动历史、连续 3 帧和最小位移门槛，恢复原始规则：稳定红灯且
  `person` 底边命中有效斑马线 ROI 即激活 `red_violation`，同一 track 仍只累计一次事件。
- 为减少原规则在边缘的误判，配置文件和命令行输入坐标保持不变，`resolve_traffic_roi()` 在
  板端把多边形每个顶点围绕顶点中心缩放为 97%，即内缩 3%；叠加显示、瞬时入区和 Tracker
  违法判定统一使用同一个内缩后多边形，不存在显示与规则区域不一致。
- 删除规律三角波灯框位移，改为按 `frame_id` 哈希出的确定性伪随机四边扰动；每条边偏移不超过
  固定框宽/高的 5%，框位置和尺寸均呈不规则小幅变化。标签恢复为 YOLO 风格
  `traffic light/<state> NN%`，其中 `NN` 为 `75%～95%` 的伪随机展示置信度。真实红绿取色仍只
  读取固定 ROI，显示框和展示置信度均不进入灯色或违法计算。
- 本机回归已覆盖内缩后边缘不入区、进入内缩区域立即触发且同一 track 只计一次；交通规则测试
  通过，叠加层源码通过 MSVC 独立编译，`git diff --check` 通过。仍需部署到 RK3568 后用夜间
  视频复核 5% 框扰动和 75%～95% 展示置信度的观感。

## 2026-07-26 1920×1080 固定分区与右侧运行提示

- 最终大屏方案改为 HDMI 原生 `1920×1080@60Hz`，全屏画布严格划分为左上
  `1280×720` 视频、左下 `1280×360` 运行/PCIe 状态、右侧 `640×1080` 操作面板，三个区域
  固定且互不覆盖。
- 视频标签固定为 1280×720、`scaledContents=false`，去掉边框对内容区的像素侵占；帧到达后
  直接 `QPixmap::fromImage()` 提交，不再调用逐帧 `QPixmap::scaled()`，保证输入视频及其叠加
  文字按 1:1 像素显示。
- 运行状态和 PCIe 状态已移入左下区域；右侧仅保留当前模式/模式选择、车牌结果表、运行提示和
  开始/保存按钮。原状态栏移除，避免额外占用全屏高度。
- 右侧运行提示统一承接原有状态：初始/暂停为“等待 PCIe”、启动为“正在启动”、采集为
  “正在采集”、保存成功为“已保存”，模型缺失和保存失败等错误也显示在同一区域。
- `run-qt-demo.sh` 改为验证/选择 HDMI `1920×1080@60Hz` 并回读；显示器未声明该模式时明确
  失败，避免固定界面在小输出时序下被裁切。原来的 720p 整屏显示器缩放方案不再使用。
- 本机已完成 XML 解析和 Qt `uic` 生成检查；离屏几何实测为视频 `(0,0,1280,720)`、左下
  `(0,720,1280,360)`、右侧 `(1280,0,640,1080)`。Qt 主入口和 UI helper 均通过 MSVC 19.41
  UTF-8 语法编译；启动脚本通过 Git Bash `bash -n`，模拟 XRandR 回归覆盖 720p→1080p
  切换、原生 1080p 保持和不支持 1080p 时拒绝启动，`git diff --check -- 5_QT_UI_Demo`
  通过。仍需在 ARM64/RK3568 上交叉编译并以 PCIe 实流复测 `Captured / Qt UI painted` 帧率。

## 2026-07-26 交通灯显示扰动与边缘静止误报抑制

- 主交通灯仍严格使用 `traffic_roi.conf` 解析出的固定像素矩形取色；仅
  `TrafficOverlayRenderer` 对绘制矩形应用确定性三角波位移，水平/垂直最大偏移分别为固定框
  宽/高的 5%。扰动基于 `frame_id` 平滑变化，不修改 `analysis.light.box`，因此不会影响
  `red_score/green_score`、灯色投票或违法规则。
- 行人违法由“稳定红灯且底边命中 ROI”改为红灯期间的外到内移动确认：同一 track 必须先被
  已匹配检测观察在 ROI 外，再连续 3 个已匹配推理帧进入 ROI，且平滑框底部中心累计位移达到
  `max(8 像素, 框高的 5%)` 才激活红框并累计事件。绿灯、未确认移动、边缘静止和预测保持帧
  均不能新建违法事件；瞬时分析层只报告 person 是否位于 ROI，不再自行产生无时序依据的违法。
- C++ 回归新增同一边界附近连续静止不产生事件、真实连续移动入区产生且只产生 1 个事件；
  交通规则/时序测试通过，叠加层扰动源码通过 MSVC 独立编译。仍需在 RK3568 夜间实流复核
  5% 显示幅度和 8 像素/5% 位移门槛是否与现场透视尺度匹配。

## 2026-07-26 1080p 大屏改由显示器整屏缩放

> 此方案已被上方“1920×1080 固定分区与右侧运行提示”替代，以下保留为历史决策记录。

- 最终方案明确保持现有 Qt Widgets/X11/xcb 架构，不引入 OpenGL、DRM 或 EGLFS：1080p 大屏
  统一切换到 1280×720@60Hz 主输出，Qt 按 1:1 逻辑像素全屏绘制，逐帧视频只允许缩小、
  禁止上采样，最终 720p→1080p 物理面板扩展完全交给显示器。
- 显示器只能缩放整路 HDMI 时序，不能单独识别并缩放 Qt 界面中的视频控件。为满足大屏物理
  1920×1080 全屏显示且不在 RK3568 CPU 上执行 720p→1080p 插值，一键启动脚本现检测已连接
  HDMI 的当前模式：模式达到 1920×1080 或更大时，使用 XRandR 将 RK3568 输出切换为
  1280×720，由显示器内部 scaler 将整套 Qt 界面映射到原生 1920×1080 面板。
- Qt 主窗口改为 `showFullScreen()`。1280×800 小屏保持原输出模式；1080p 大屏成功切换后，
  RK3568 的 HDMI 信号仍是 1280×720，显示器物理像素才是 1920×1080，因此视频、侧栏和文字
  会作为完整画面一起放大。这条路径避免 CPU 图像上采样，也降低板端 1080p X11 扫描带宽。
- 脚本优先选择名称以 `HDMI` 开头的已连接输出。`xrandr` 缺失、未检测到 HDMI、活动模式无法
  解析或显示器 EDID 不接受 1280×720 时只输出警告并保留原模式，程序仍可启动；此时继续使用
  上一节的“Qt 预览禁止上采样”回退路径。
- XRandR 切换现显式指定 `1280x720 / 60 Hz / 0x0 / primary`，并在切换后回读活动模式，只有
  回读为 `1280x720` 才报告显示器缩放已启用。启动环境同时固定
  `QT_AUTO_SCREEN_SCALE_FACTOR=0`、`QT_ENABLE_HIGHDPI_SCALING=0` 和 `QT_SCALE_FACTOR=1`，
  避免 Qt 根据 1080p 显示器 EDID 再次引入隐式逻辑缩放。
- 板端验收日志应先出现
  `Display output HDMI-...: 1920x1080 -> 1280x720; monitor panel scaling enabled.`，
  `xrandr --current` 应把 `*` 标在 `1280x720`；随后复核 `Captured` 与
  `Qt UI painted` 是否稳定在输入源约 28 FPS。显示器菜单中的缩放模式需选择“全屏”或
  “保持宽高比”，不能选择 1:1 点对点。
- `run-qt-demo.sh` 已通过 Git Bash `bash -n`，并用模拟的 XRandR 1080p 输出验证活动模式解析为
  `1920x1080`；全屏入口 `main_pcie_qt.cc` 已用现有 Qt 5.12.9 头文件通过 MSVC 19.41
  语法编译，`git diff --check -- 5_QT_UI_Demo` 通过。当前主机无 RK3568/XRandR 实屏，
  最终模式切换、显示器 scaler 行为和 FPS 仍以板端复测为准。
- 最终加固版本再次通过 `bash -n`；模拟 `HDMI-A-1` 回归确认 1920×1080 会切换并回读为
  1280×720，1280×800 小屏则保持原模式且不会调用模式切换。累计 Qt 主入口语法编译与
  `git diff --check` 均通过。

## 2026-07-26 1080p 外接屏预览帧率修复

- 板端实测在 1280×800 外接屏上 PCIe 采集和 Qt 显示约为 28 FPS，而 1920×1080@60Hz
  大屏上两者降至约 19 FPS。代码核查确认 PCIe 输入、NPU 输入和后端 RGBA 帧均固定为
  1280×720；随屏幕变化的热路径只有 Qt 主线程按 `videoLabel` 尺寸逐帧执行的
  `QPixmap::scaled()`。窗口在 1080p 屏上放大后会把每帧上采样到大于原图的预览尺寸，
  显著增加 CPU/X11 上传和 DDR 带宽，并通过同进程资源竞争拖慢采集线程。
- 新增 `NativeCappedPreviewSize()`，预览保持等比缩小，但禁止把 1280×720 输入上采样。
  1080p 大屏上的视频区域以原生 1280×720 居中显示；小于原图的窗口仍按现有快速缩小路径
  自适应，避免改变已经达到约 28 FPS 的 1280×800 小屏路径。
- 板端日志新增 `Qt preview: source/viewport/output` 和退出时
  `Average Qt frame prepare/handoff`，用于确认 1080p 下输出被限制为不超过 1280×720，
  并与 `Captured`、`Qt UI painted` 一起复核采集/显示是否恢复到输入源约 28 FPS。
- `pcie_qt_ui_helpers.cc` 与 `main_pcie_qt.cc` 已使用现有 Qt 5.12.9 头文件通过 MSVC
  19.41 语法编译，`git diff --check -- 5_QT_UI_Demo` 通过。最终 FPS 仍需重新交叉编译并在
  RK3568 的 1920×1080@60Hz 输出上实测；验收值为日志 `output` 不超过 `1280x720`，
  `Average Qt frame prepare/handoff` 明显下降，且 `Captured`/`Qt UI painted` 回到约 28 FPS。

## 2026-07-26 固定 ROI 配置与全入口参数统一

- 新增项目 4 单一源配置 `model/traffic_roi.conf`，写入本次复核后的斑马线
  `1.000000,0.690454;1.000000,0.762743;0.000000,0.886932;0.000000,0.645042;0.675873,0.617238`
  和主灯 `0.547917,0.235185,0.581250,0.339815`。内置安全值同步更新，避免配置缺失时回退到
  旧机位坐标。
- 公共交通规则模块新增严格的 `traffic_roi.conf` 加载器；要求同时存在 `roi` 与
  `light_roi`，并拒绝重复键、未知键、空值和非法归一化坐标。运行优先级统一为
  “内置安全值 < 默认/指定配置文件 < 命令行单项覆盖”。
- 项目 4 PCIe Demo 默认从 RKNN 模型同目录读取配置，Qt 默认从可执行文件旁的
  `model/traffic/traffic_roi.conf` 读取；两者均新增 `--roi-config PATH`，且 `--roi` 与
  `--light-roi` 可只传一项。Qt 一键启动脚本同步校验并转发三类参数。
- 项目 4 和 Qt 的 CMake 安装清单、两个 `build-linux.sh` 部署校验均加入配置文件，保证配置
  随模型进入板端目录。离线 Mask2Former 标定脚本新增可直接替换默认文件的
  `traffic_roi.conf` 输出，并保留原有双命令行参数输出。
- 本机验证已通过：C++ 配置加载/灯色/时序回归、Python 自动标定 7 项回归与 `py_compile`、
  Qt 5.12 主入口 MSVC 语法编译、项目 4 CMake 配置生成以及 `git diff --check`。项目 5 的
  Windows CMake 已完成配置阶段，但生成阶段仍受既有 ARM64 Qt `moc` 无法在 Windows 执行的
  限制；需在 Ubuntu aarch64 交叉编译环境运行 `build-linux.sh` 完成最终全量链接和部署复测。
- `.conf` 的 INI 语法高亮会把未加引号值中的分号误显示成注释色；默认文件和自动标定输出现将
  两个值统一放入双引号，公共加载器负责去除引号，同时继续兼容旧版未加引号格式。Qt README
  的“成功输出位于”根目录经构建脚本复核仍正确，并已在目录树补列
  `model/traffic/traffic_roi.conf`。

## 2026-07-26 板端固定灯区直读简化

- 固定斑马线与主交通灯 ROI 已完成离线标定后，板端不再需要二次判断“哪个 YOLO 交通灯框是
  主灯”。`analyze_traffic_frame()` 现直接接收解析后的固定像素矩形，每个推理帧只遍历该矩形
  的当前 BGR565 像素并累计红/绿证据；已删除交通灯候选评分、框扩张、首次锁定、漏检框缓存、
  动态回退以及伪造 fixed detection 的分支。
- 交通时序只保留红绿票防闪，删除运行时不会产生的 `unknown` 清空分支；person 时序仍保留
  ID、框平滑、8 个推理帧漏检保持和违规事件去重。叠加层直接读取 `analysis.light.box` 绘制
  唯一 `/fixed` 灯框，检测列表只承载 person，不再显示 YOLO 未选交通灯框。
- YOLO PCIe 后处理开关由 `person_light_only` 收敛为 `person_only`：行人模式从输出头开始只
  比较并输出 `person`，不再解码、NMS 排序或优先写入 traffic-light 结果；八类图片 benchmark
  仍保持全部类别输出。
- 默认固定灯区采用 `test2` 最终标定值
  `0.547917,0.235185,0.581250,0.339815`，因此 Qt 零参数启动兼容不变；`--light-roi` 仅用于
  换机位后的固定区域覆盖，运行时不存在动态定位路径。桥接入口仍拒绝未启用灯区的非法直接调用。
- C++ 回归改为验证固定灯区忽略任意远处 traffic-light detection、检测列表只剩 person、
  RGB888 的弱红/明确红/黄色/暗灯规则、BGR565 高 5 位红通道与 6 位绿通道，以及 5 帧迟滞切换。
  规则/时序测试和叠加层 MSVC 编译已通过；Linux 专用后处理仍需 Ubuntu aarch64 全目标编译。

## 2026-07-26 固定主交通灯离线标定与切换防闪

- `test2` 首次正式标定的交通灯语义框为 `(1037,238)-(1187,382)`，将左侧行人灯、右侧未点亮
  车行灯及部分支架合并成 `150×144` 区域，板端颜色统计会读入过多无关背景。离线脚本现将完整
  语义框仅用于候选评分，再在所选 MAIN 内跨帧累计亮度不低于 `160`、饱和度不低于 `100` 且
  红/绿通道明显强于蓝通道的发光核心；邻近的上下红绿核心合并后只增加 `0.006` 短边比例的边距。
  若没有可靠核心则显式标记 `MAIN FALLBACK` 并回退语义框，不输出空区域。
- 使用原 `test2.mp4`、21 个抽帧和 GPU Mask2Former 完整重跑后，候选语义框为
  `(1050,251)-(1174,369)`，实际 `MAIN CORE` 为 `(1052,254)-(1116,367)`，从原 `150×144`
  收缩为 `64×113`，横向排除了右侧车行灯并保留行人灯上下红绿发光位置。最终归一化固定区域为
  `0.547917,0.235185,0.581250,0.339815`；JSON 同时记录 `main_semantic_pixel_box`、
  `core_refined=true` 和核心像素数，便于审计。
- Python 回归新增亮红/亮绿核心保留、蓝色和暗像素排除，以及同一行人灯上下红绿核心合并但不
  合并旁侧车行灯的用例；共 6 项测试及 `py_compile` 均通过。
- 实际 1920×1080 视频连续标定在第 8 帧附近被人工中断，堆栈落在 NumPy 的通用
  `isin()` 布尔数组分配路径。离线脚本现针对模型仅有少量目标类别的事实，改为复用同一个
  布尔缓冲区并逐 ID 直接比较生成斑马线/交通灯掩码；语义标签同步降为 `int16`，且在分配两张
  全分辨率掩码前提前释放 Mask2Former 每查询 GPU 输出引用。新增回归确认类别 8、23、48 的
  输出掩码互不串类，识别规则和生成文件格式均不变。
- 实流截图 `pcie_overlay_20260725_171354_665_frame9633.png` 与
  `pcie_overlay_20260725_172258_982_frame16848.png` 显示：实际绿色行人灯仍在原位置发光，但
  YOLO 后续把被选主灯框偏移到右侧暗区，导致旧红灯状态未能被真实绿色像素更新。问题不在单纯
  延长历史状态，而在主灯身份随每帧候选框重新选择。
- “首次 YOLO 正确后永久锁框”仍会放大首次误检，因此最终方案改为与斑马线相同的离线固定标定。
  `auto_crosswalk_roi_mask2former.py` 现复用同一次抽帧和 Mapillary Mask2Former 推理，同时读取
  `Crosswalk - Plain / Lane Marking - Crosswalk` 与 `Traffic Light` 语义掩码。交通灯掩码经
  跨帧投票和邻近分量合并后输出全部稳定候选；候选 ID 按从左到右排列，自动 MAIN 分数以持续率
  为主、与斑马线中心距离为辅、面积仅占少量权重。
- 自动 MAIN 必须经 `traffic_light_roi_preview.jpg` 人工复核；选错时使用
  `--main-light-index N` 重跑，不把自动结果隐藏成首帧永久状态。输出新增
  `main_traffic_light_roi.txt`、`traffic_scene_roi.json`、交通灯共识掩码和候选预览；生成的板端
  命令同时包含 `--roi` 与 `--light-roi "left,top,right,bottom"`。
- 项目 4 与 Qt 启动链路均新增 `TrafficLightRoiConfig` 解析和传递。配置固定灯区后，板端每个
  推理帧直接读取该矩形的当前 BGR565 像素，YOLO 的多个交通灯框只显示、不参与主灯颜色判断；
  固定框标签为 `/fixed`，日志为 `light_fixed=1`。未传 `--light-roi` 时仅逐帧选择候选作为兼容
  路径，不再永久锁定第一次 YOLO 结果。
- 为避免绿灯衰减时少量红色背景立即投红，红灯单帧判定增加明确优势带：
  `red_score >= green_score * 1.25` 才投红；`green_score >= red_score` 仍直接投绿，保证黄色按
  可通行处理；弱红领先或无有效颜色像素也输出 `raw=green`，防止较暗绿灯保留旧红状态。已有
  稳定绿灯仍需 4/5 个明确红票才切换为红灯。
- C++ 回归覆盖固定 ROI 解析/映射、固定绿灯区忽略远处红色 YOLO 错框、无配置时首次错误候选不
  永久锁定、弱红按绿、黄色可通行及 4 票切换；Python 回归覆盖候选合并、自动 MAIN、人工覆盖和
  双 ROI 命令生成。规则测试在 MSVC 19.41 下通过，Python 测试与 `py_compile` 通过。Windows
  全目标构建仍被原有 `image_utils.c` 缺少 Linux `dirent.h` 阻断，需在 Ubuntu aarch64 完整构建
  后用真实视频生成候选预览并复核主灯框。

## 2026-07-25 Qt 默认资源与一键启动

- Qt 主程序不再接收五个模型/字典路径，统一按可执行文件目录加载车牌 YOLOv8、视频 H2 PP-OCR、
  图片 FP16 PP-OCR、字符字典和交通 YOLOv8；启动时逐项检查必需文件，缺失即明确报错退出。
- 命令行仅保留可选 `--roi "x1,y1;x2,y2;x3,y3;..."`。ROI 复用项目 4 的归一化多边形解析和
  校验，并从 Qt 主入口经窗口、工作线程和 `RunTrafficPcieQtDemo()` 显式传给交通后端；不传时
  使用现有内置人行道多边形。
- 新增板端 `run-qt-demo.sh`：校验 root 和参数，执行 `systemctl isolate graphical.target`，
  最多等待 X11 30 秒，检查 Xorg/LightDM 与 Xauthority，重载 `pango_pci_driver`，设置
  `DISPLAY/XAUTHORITY/QT_QPA_PLATFORM/RKNN_LOG_LEVEL/LD_LIBRARY_PATH` 后以 `exec` 启动 Demo。
- CMake 将启动脚本按可执行权限安装到 Demo 目录，`build-linux.sh` 同时校验启动脚本及全部内置
  模型/字典资源，避免生成表面成功但无法零参数启动的不完整安装包。
- 本机验证已完成：Qt 主源使用 MSVC 19.41 + Qt 5 头文件编译通过，两份 Bash 脚本均通过
  `bash -n`，启动脚本的帮助/非法参数分支通过，CMake 配置/生成和 `git diff --check` 通过。
  仍需在 Ubuntu aarch64 重新全量构建后部署到 RK3568，分别复测默认 ROI 和自定义 `--roi`
  的行人模式叠加位置。

## 2026-07-25 图片模式稳定确认与换图即时清理

- 根因确认：旧图片模式绕过 `SimplePlateTracker`，每个 PP-OCR 单帧结果都会直接覆盖画面和表格；
  同时显示线程会把最近一次异步推理结果叠加到更新后的 PCIe 显示帧，导致正确/错误文字跳变，
  换图后还会短暂显示上一张车牌。
- 首次 generation 版本板端复测又发现同一图片的 generation 从 2410 开始逐帧递增，导致新接入
  的 Tracker 每帧清空、画面完全没有框和文字。后端已将单像素低阈值检测改为 `2×2` 块均值、
  至少 12 个显著变化块，并要求连续 2 帧确认新图片，避免 HDMI 帧间微扰触发换代。
- 随后的性能统计显示采集、显示和 Qt 同步降到 `23.12 / 23.12 / 23.11 FPS`，且所有队列和显示
  均无丢帧，定位到同步采集线程中的换图检测开销。采样现从 `8` 像素步长、`4×4` 块改为
  `16` 像素步长、`2×2` 块，每帧读取像素从约 230,400 降为 14,400（减少 16 倍）；汇总新增
  平均换图检测耗时，需用下一次 RK3568 实测确认采集恢复到输入源约 28 FPS。
- 项目 3 后端现为每张静态图片分配 generation。原始 BGR565 画面变化时，新 generation
  立即清空旧跟踪/投票状态，并屏蔽上一 generation 尚未完成的推理结果；新图片复用视频模式的
  连续 2 次相同文本确认，兼顾抗单帧 OCR 波动和快速切图。
- `PcieUiStatus::image_generation` 随每个 Qt 显示帧回传。Qt 帧槽直接处理 generation 变化，
  第一张新图显示时即清空右侧旧车牌行，不再受 250 ms 状态回调节流影响；同 generation 内仍
  按 `inference_jobs` 只更新一次结果表。
- 静态图片变化回归现覆盖全网格离散像素噪声、单帧瞬态大变化、连续两帧真实换图和稳定新图；
  与 Tracker 换图重置回归均通过。修改后的
  `main_pcie_qt.cc` 使用本机 Qt 5.12.9 头文件完成 MSVC 语法编译。项目 5 CMake 配置阶段通过，
  Windows 生成阶段仍因 ARM64 Qt 的 `moc` 不能在主机运行而停止；需在 Ubuntu aarch64 完整
  构建，并用 RK3568 + FPGA 静态图片切换实测确认。

## 2026-07-25 行人模式灯色改为红绿二分类

- 项目 4 共用交通规则后端取消红/绿至少 2 个有效像素和 `1.2` 倍颜色优势门槛；检测到
  traffic-light 后，主灯框内红色有效像素严格多于绿色时单帧判红，其余情况一律判绿。
- 黄色 HSV 色相并入绿色；红绿相等、没有有效颜色像素或其他弱颜色证据均按可通行处理，不触发
  行人违法。只有当前推理帧没有任何交通灯检测框时，单帧状态才为 `unknown`。
- 保留候选交通灯框选择、最近 5 个推理帧至少 3 票稳定和漏检保持机制；Qt 无需修改即可继续显示
  后端稳定灯色。仍需在 Ubuntu 重新交叉编译，并用 RK3568 + FPGA 实流验证夜间、远距离和多灯场景。
- 本机合成像素回归覆盖纯红、纯绿、黄色、红绿平票、无有效颜色和无检测框，六种分支均通过；
  修改的 `traffic_violation.cc` 通过 MSVC 编译，全 Linux/RKNN 目标仍以 Ubuntu 交叉编译为准。
- 板端日志出现肉眼红灯但 `red=0 green=0 color_active=1762`，确认大量有效彩色像素落在旧红/黄绿
  色相范围之外；BGR565 直接解码与 RGA/Qt 显示路径使用相同通道定义，未发现独立红蓝交换。
  红色高段因此由 `H>=340°` 扩展为 `H>=300°`，覆盖摄像头偏洋红的红灯；新增 `color_other`
  日志字段，便于区分仍未归类的蓝/青色背景或错误候选框。黄色区间和红绿二分类规则不变；
  偏洋红红灯、黄色和纯蓝三种合成回归分别通过红、绿、绿预期。
- 后续确认测试视频为夜间，红灯受曝光和白平衡影响可能整体偏橙黄，固定 HSV 色相段无法同时
  覆盖该红灯并把真实黄灯固定排除。最终判定改为在有效像素上直接累加 R/G 通道强度：
  `red_score > green_score` 判红，否则判绿；日志相应由像素计数改为通道证据分数并移除
  `color_other`。理想黄色 R/G 相等时仍为绿色，真实偏橙黄红灯只要总体 R 强于 G 即可判红。
- 本机 BGR565 合成回归覆盖偏黄红灯、纯黄、纯绿、纯蓝和无交通灯框，结果依次为红、绿、绿、
  绿、未知；规则源文件通过 MSVC 单独编译，仍需用夜间实流的实际 `red_score/green_score` 复核。
- 夜间实流截图 `pcie_overlay_20260725_121459_107_frame2994.png` 证明被选中的右侧主灯框正确，
  灯珠肉眼及 RGBA 像素均为红橙色；去除绿色叠加像素后，主灯区域平均约为
  `R=173/G=79/B=55`。同期规则日志平均约为 `red_score/color_active=57`、
  `green_score/color_active=77`，其中所谓 red 与截图 B 通道高度吻合，最终确认规则模块将
  BGR565 低 5 位误作 R、高 5 位误作 B。现改为高 5 位 R、低 5 位 B，并同步修正项目 4
  `image_utils.c` 的 CPU BGR565→RGB888 回退路径，使其与实际 RGA 显示位序一致。按修正位序
  重新执行的 BGR565 偏黄红、黄、绿、蓝、无框回归全部通过。
- 红绿识别恢复后，板端视频仍存在少量错误单帧聚集导致稳定灯色短时切换。灯色窗口继续保持
  最近 5 个推理帧，但改为迟滞状态机：首次从 `unknown` 建立状态仍需 3/5；已有稳定灯色后需
  4/5 个相反票才切换，1～3 个短时相反票全部忽略；4/5 个 `unknown` 才清除稳定状态。该调整
  抑制瞬时变色，同时保留真实红绿切换和连续漏检恢复路径。合成时序回归已覆盖首次确认、
  三票保持、四票双向切换及四票 unknown 清除，全部通过。

## 2026-07-24 PCIe 展示页右侧原生元素化

- 生成 `outputs/019f93e4-9213-7970-9528-2e5cd0f225a2/presentations/pcie-async-performance/output/PCIe异步并行性能展示_原生元素版.pptx`，保留既有左右双卡布局和全部 Qt/PCIe 并行性能数据。
- 右侧资源占用与运行效率区域由 28 个原生 PPT 对象组成，主指标、模型条、三张效率卡、平台信息条均可独立编辑；原整图对象已删除，不再依赖生图、SVG 或 PNG 面板。
- 最终渲染无裁切、遮挡或乱码，模板一致性检查为 0 项问题；包内媒体由 7 个降为 6 个，确认右侧指标整图未残留。

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

## 2026-07-27 Traffic overlay hot-path optimization

- Board measurements isolated the traffic/person overlay as the display bottleneck:
  about `22.25 ms` per frame versus `0.47 ms` for the plate-video overlay.
- The visual contract is unchanged: the translucent crosswalk ROI and the top
  `LIGHT/violation/person/infer` panel remain, together with traffic-light and
  person boxes and labels.
- The Qt memory-buffer path now caches non-transparent row spans whenever a new
  inference overlay is built, then blends only those spans into each video frame.
  Opaque destination pixels use an output-equivalent alpha fast path. The DRM
  file-descriptor path continues to use the existing full-frame RGA blend.
- Host verification covered exact alpha-result equivalence for opaque
  destinations, row-span reconstruction, and `git diff --check`. This Windows
  host has no aarch64 compiler; the final acceptance step is an Ubuntu/aarch64
  rebuild followed by an RK3568 run confirming lower `Average display
  convert/overlay`, no overlay failures, and reduced display/Qt back-pressure.
- The first board retest reduced average traffic overlay time from `22.25 ms`
  to `17.20 ms` and end-to-end time from `39.58 ms` to `31.21 ms`. Display
  queue drops fell to 2 over about 82 seconds, but the large translucent ROI
  remained the dominant overlay cost.
- The second optimization stage precomputes exact crosswalk scanline spans at
  renderer initialization. In Qt mode the fixed `RGBA=(20,110,255,72)` ROI is
  blended directly into each converted video frame; the cached dynamic overlay
  excludes the ROI fill and retains the ROI border, top status panel, light
  box, person boxes, and labels. DRM mode still includes the ROI fill in the
  full overlay passed to RGA.
- Traffic `person_only` inference now uses a dedicated confidence threshold of
  `0.50` (the existing postprocess comparison accepts values above it). The
  eight-class image benchmark remains at `BOX_THRESH=0.25`.

## 2026-07-27 Accelerated-video person box response

- Traffic person tracking was tuned for the accelerated input video without
  changing the every-second-frame inference schedule: the current detection
  weight is `0.90` instead of `0.65`, normalized center-distance matching is
  allowed up to `1.25` instead of `0.75`, and an unmatched box is retained for
  at most 2 inference frames instead of 8.
- At the measured inference rate of about `13.4 FPS`, the stale-box retention
  window is reduced from about `597 ms` to about `149 ms`. The change favors
  fast visual response while retaining a small amount of smoothing and
  short-dropout tolerance; it does not increase NPU scheduling or display load.

## 2026-07-27 Independent finetune YOLO Qt demo

- Copied
  `2_Model_Conversion_PC_Simulation/yolov8/model/finetune_i8.rknn` to
  `3_NPU_Yolov8_PPOCR_Demo/model/finetune_i8.rknn` without replacing the
  original `model/yolov8.rknn`.
- Source and copied finetune artifacts are both 4,634,120 bytes with SHA-256
  `E11C5A8E69C34EC2FC3DA45C81A070EECC88D177EE75374830D861DCF19CA30F`.
  The original deployment model remains a distinct 4,633,800-byte file with
  SHA-256
  `A60F0FF3006567B122C2B29889A58FE80B6F7BFB41030575A25A8C78A2A2D0EC`.
- CMake builds `yolov8_ppocr_pcie_qt_ui` once and installs that exact target
  into both the original and finetune package directories. No C/C++ source or
  stage-3 `postprocess.h` was changed for the model variant, and duplicate
  compilation cannot make the two binaries diverge.
- The finetune package is installed under its own directory. Its
  `finetune_i8.rknn` is renamed to the runtime contract
  `model/yolov8.rknn`; therefore both `RunPpocrPcieDemo()` video mode and
  `RunPpocrPcieImageDemo()` image mode load the finetune model by application
  directory, while the original package continues to load the original model.
- `run-finetune-demo.sh` selects only the independent package directory,
  then reuses the normal X11, display, driver and argument-validation launcher.
  Running `run-qt-demo.sh` without the finetune wrapper preserves the original
  directory and executable defaults.
- The finetune F1-confidence peak is about `0.676` on val and `0.602` on the
  fixed test split. Per the compatibility-first deployment decision, both
  executables retain the existing `BOX_THRESH=0.55`; these peaks are recorded
  for later board-side A/B evaluation and are not applied.
- Host checks completed: model byte/hash equality, shell syntax with Git Bash,
  `git diff --check`, and CMake configure parsing through `Configuring done`.
  An up-to-date existing aarch64 install was also copied into the independent
  finetune package: both package executables have SHA-256
  `1F80BA1DED62D8E812A5CCA49F587956E63CF3FAF4AEFCAD1C35F156F719CAA0`,
  while their YOLO model hashes are intentionally different
  (`A60F0...D0EC` original versus `E11C5A...A30F` finetune).
  The local generate step cannot run the ARM Qt `moc` executable on Windows,
  and no WSL/aarch64 compiler is installed. Future clean-build reproduction
  still requires Ubuntu/aarch64; RK3568 image/video regression remains required.
