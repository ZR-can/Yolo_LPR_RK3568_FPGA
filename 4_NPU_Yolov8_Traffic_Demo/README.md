# RK3568 八类交通 YOLOv8 INT8 / PCIe 闯红灯验证 Demo

更新时间：2026-07-26

项目 5 的 Qt UI 现通过 `RunTrafficPcieQtDemo()` 复用本工程完整的 person 后处理、
灯色时序、斑马线规则和 RGBA 叠加层。Qt 模式不初始化或占用 DRM，而是通过共用
`PcieUiCallbacks` 回传已叠加的 RGBA 帧和实时违法统计；原命令行 PCIe Demo 仍独占 DRM，
两种入口不会同时启动。为允许车牌与交通 YOLO 后端链接到同一 Qt 可执行文件，本工程的 YOLO
和后处理公开函数统一增加 `traffic_` 前缀，RKNN 上下文类型独立为
`traffic_rknn_app_context_t`，算法、阈值与 standalone 调用行为不变。

固定机位运行时，主交通灯区域由 PC 离线标定工具提前生成；当前 `test2` 结果已作为默认值内置，
也可通过 `--light-roi "left,top,right,bottom"` 覆盖。后续每个推理帧都从该固定区域重新读取当前
画面像素，不再依赖首次 YOLO 判断，也不会因 YOLO 漏检、错框或多个候选框切换而沿用旧灯色。
框内通过饱和度和亮度门槛的像素分别累加红、绿通道强度：绿色不少于红色时输出
`raw=green`；红色至少比绿色强 25% 时才输出 `raw=red`；介于两者之间的弱红领先以及没有有效
颜色像素时也输出 `raw=green`。因此主灯定位成功后保持“非明确红即绿”的二分类，理想黄色的
R/G 证据相等，仍按可通行的绿色处理。终端日志中的 `color_active` 表示参与累加的有效像素数，
`active` 表示当前违规行人数。
FPGA 小端 BGR565 与 RGA 的布局统一为高 5 位 R、中间 6 位 G、低 5 位 B；规则模块直接解码和
CPU 回退转换均使用该位序，避免把实际红通道误记为蓝通道。

本工程包含两个相互独立的板端程序，并向项目 5 提供一个 Qt 桥接入口：

- `yolov8_traffic_benchmark`：单图或图片目录检测，用于确认模型结果和 NPU 性能。
- `yolov8_traffic_pcie_demo`：复用 `3_NPU_Yolov8_PPOCR_Demo` 的 PCIe、固定帧槽和 DRM
  显示链路，实时检测 `person`，并直接读取固定交通灯 ROI 执行斑马线闯红灯规则。
- `RunTrafficPcieQtDemo()`：复用同一 PCIe、推理、规则和叠加链路，将 RGBA 帧交给项目 5
  的 Qt 主线程显示，不获取 DRM master。
- `pc_tools/auto_crosswalk_roi_mask2former.py`：PC 端调用本地 FFmpeg 和 Mapillary Vistas
  Mask2Former，从固定机位视频生成斑马线多边形、全部稳定交通灯候选、可复核的主灯矩形及板端
  完整运行命令。

PCIe 后端不依赖 LPRNet、MPP、OpenCV 或 Qt；只包含轻量 person 空间跟踪，不引入 Kalman、ReID
或车牌文字投票。Qt 仅通过无 Qt 类型的回调桥接接收帧。当前已经完成 Windows 工作区代码迁移、
规则单元验证和 C++ 静态语法检查；新增 Qt 桥接后的 CMake 配置/生成以及交通规则、时序 Tracker、
叠加层源文件本机编译已通过。RKNN、RGA 和 Linux PCIe 主程序仍需在 Ubuntu 重新交叉编译，并在
RK3568 + FPGA 实链路复测。

交通 PCIe 信号处理函数会显式保存并忽略 `write()` 返回值，避免 GCC
`-Wunused-result` 告警；仍只调用异步信号安全的 `write()`，不改变停止和资源回收流程。

## 1. 模型与类别

模型来源：

```text
../2_Model_Conversion_PC_Simulation/yolov8/model/yolov8_traffic_i8.rknn
```

INT8 模型的 8 个输出类别已连续重新编号：

```text
0 person
1 bicycle
2 car
3 motorcycle
4 bus
5 truck
6 traffic light
7 stop sign
```

图片 benchmark 后处理阈值为 `BOX_THRESH=0.25`、`NMS_THRESH=0.5`，仍解析并输出全部 8 类；
PCIe 行人模式独立使用 `TRAFFIC_PERSON_BOX_THRESH=0.50`，只保留置信度高于 `0.50` 的
`0 person`，并且从输出头开始不再解码、排序或输出 YOLO traffic-light 框。
交通灯颜色完全来自离线标定的固定像素区域。

## 2. PCIe 实时链路

```text
FPGA 1280x720 小端 BGR565
  -> Pango PCIe DMA 读取
  -> 6 个固定 BGR565 帧槽
  -> 显示队列（容量 2，每帧）
  -> 推理队列（容量 1，默认每 2 帧）
  -> YOLOv8 INT8
  -> person 专用后处理
  -> 固定灯区直接取色 + 5 帧灯色投票
  -> person ID/框平滑/漏检保持
  -> ROI 规则与违规事件去重
  -> RGA BGR565 -> RGBA
  -> 缓存的 RGBA 规则叠加层
  -> DRM/KMS 显示
```

队列满时丢弃旧帧并保留最新帧，PCIe 采集线程不等待显示或 NPU。显示线程在没有新推理结果的
帧上复用最近一次分析结果，保持与项目 3 相同的隔帧推理方式。`Ctrl+C` 的停止信号只在主采集
线程解除屏蔽，使阻塞的驱动读取能够被中断，然后按顺序停止 DMA、唤醒线程并输出统计。

## 3. 闯红灯规则与显示

每个推理帧执行以下规则：

1. 提前解析归一化 `--light-roi`，将其固定映射到当前帧的主交通灯矩形。
2. 每个推理帧始终从固定矩形的当前画面重新取样；YOLO 后处理只保留 person，不生成任何交通灯
   候选。低饱和度、低亮度像素不参与统计，其余像素分别累加 R/G 通道强度。
3. 绿色证据不少于红色时单帧判绿，包含 R/G 相等的黄色；只有
   `red_score >= green_score * 1.25` 才单帧判红。弱红领先或没有有效颜色像素时产生
   绿色可通行票，避免绿灯衰减帧直接投红或保留旧红状态。
4. 最近 5 个推理帧做带迟滞的灯色投票：初次从 `unknown` 建立红/绿状态需要至少 3 票；已有
   稳定灯色后，必须有至少 4 个相反灯色票才允许切换，短时 1～3 帧跳变继续保持原状态。
5. person 使用 IoU + 中心距离匹配 ID。为适配加速视频中的高速行人，匹配中心距离上限为
   `1.25` 个框对角线，框坐标按 `0.90` 新框、`0.10` 旧框平滑；最多保持 2 个连续漏检推理帧，
   在约 13.4 FPS 推理率下旧框停留时间约为 150 ms，避免框明显落后或长时间停在旧位置。
6. 输入斑马线多边形在板端围绕顶点中心统一内缩 3%，叠加显示和入区判断都使用内缩后的区域；
   再在平滑后的 `person` 框底边均匀取 5 个点，至少 1 个点落入有效多边形即记为在 ROI 内。
7. 恢复原违法规则：稳定红灯且人在内缩后的 ROI 内时立即显示 `red_violation`；同一
   `track_id` 在本次运行期间最多累计 1 名违法人员，离开并再次进入不会重复累计。

固定主灯的颜色始终从原始固定 ROI 当前像素读取；只在叠加显示层按推理帧对灯框四条边分别加入
不超过固定框宽高 5% 的伪随机扰动，并显示 `75%～95%` 的伪随机展示置信度，使外观接近 YOLO
原生框。显示置信度不参与取色、灯色投票或违法规则。板端仍不生成 YOLO 交通灯候选；摄像机
位置或画面裁剪变化后必须重新运行 PC 标定。
单帧结果再进入 5 帧迟滞投票；轻量 person 跟踪只服务于显示稳定和事件去重，不做身份识别。

显示约定：

- 斑马线 ROI：半透明蓝色多边形、蓝色粗边框和圆角深色 `CROSSWALK ROI` 背景板。
- `red_violation` 人框：红色；保持红框不等于重复生成事件。
- 绿灯且人框底边位于 ROI 内：绿色，标签为 `person#ID/person/green_pass`。
- 其他 person：黄色。
- 漏检保持的框带 `/hold`；person 标签包含稳定的 `track_id`。
- 固定 traffic light：按投票后红/绿状态显示，框边缘在 5% 范围内伪随机变化并显示展示置信度。
- 左上角状态栏使用较大白字，只显示 `LIGHT=稳定灯色`、`violation=累计违规 ID 数`、
  `person=累计分配 ID 数` 和最近一次 `infer` 耗时；当前存在红框时为红色背景，否则为绿色背景。

叠加层只在获得新推理结果时由 CPU 重建。DRM 路径继续构建包含半透明 ROI 的完整图层，并使用
RGA alpha blend 合成到不可映射的 RGBA buffer。Qt 虚拟内存路径在初始化时预计算斑马线多边形
的逐行跨度，每个显示帧把固定颜色和 Alpha 的 ROI 直接混入目标视频；缓存的动态图层只包含 ROI
边线、左上角状态栏、交通灯框、行人框和文字标签，并且只混合实际非透明跨度。这样既不改变显示
内容，也避免每个推理帧先生成 ROI 图层、每个显示帧再从图层二次读取 ROI。视频帧 Alpha 为 255
时继续使用与通用公式等价的快速混合路径。

## 4. 固定 ROI 配置与命令行覆盖

默认配置文件为模型同目录的 `traffic_roi.conf`。当前
`model/traffic_roi.conf` 已写入新测试视频复核后的两块归一化区域：

```ini
roi="1.000000,0.690454;1.000000,0.762743;0.000000,0.886932;0.000000,0.645042;0.675873,0.617238"
light_roi="0.547917,0.235185,0.581250,0.339815"
```

运行参数优先级为“内置安全值 < 配置文件 < 命令行单项覆盖”。程序默认读取
`<model.rknn 所在目录>/traffic_roi.conf`；`--roi-config PATH` 可切换到另一份完整配置。
`--roi` 和 `--light-roi` 可单独覆盖对应区域，不要求同时传入。例如只临时调整斑马线：

```bash
./yolov8_traffic_pcie_demo ./model/yolov8_traffic_i8.rknn \
  --interval 2 \
  --roi "0.08,0.62;0.90,0.58;0.96,0.91;0.03,0.88"
```

或者基于指定配置仅覆盖主灯：

```bash
./yolov8_traffic_pcie_demo ./model/yolov8_traffic_i8.rknn \
  --roi-config ./model/traffic_roi.conf \
  --light-roi "0.547917,0.235185,0.581250,0.339815"
```

斑马线 `roi` 至少输入 3 个点，点之间用分号分隔；板端解析后会围绕多边形顶点中心内缩 3%。
`light_roi` 使用矩形边界
`left,top,right,bottom`，必须满足 `0 <= left < right <= 1` 和
`0 <= top < bottom <= 1`。配置文件必须同时包含 `roi` 和 `light_roi`，重复键、未知键或非法
坐标都会拒绝启动。默认配置文件缺失时会明确告警并使用同值的内置安全区域；显式指定的
`--roi-config` 不可读时则直接报错。`--light-roi` 只覆盖固定区域，不会启用 YOLO 动态定位。

坐标相对于原始 1280×720 图像归一化。每次改变机位、裁剪或画面比例后都必须重新标定两类 ROI。
多边形可以包含任意数量的顶点（至少 3 个）。person 底边仍独立均匀采样 5 个点，只要其中至少
1 个点位于多边形内，就判定该 person 进入斑马线 ROI；采样点数与多边形顶点数没有对应关系。

## 5. PC 端 Mask2Former 自动 ROI 标定

该工具仅用于 PC 离线标定，不参与板端实时推理，也不需要将 Mask2Former 转换为 ONNX/RKNN。
Mapillary Vistas 模型同时包含斑马线和 `Traffic Light` 语义类别，可在一次抽帧推理中标定路面
多边形与主交通灯矩形。处理链路为：

```text
固定机位视频
  -> 本地 FFmpeg 定时抽帧
  -> Mapillary Vistas Mask2Former 语义分割
  ├─ Crosswalk - Plain / Lane Marking - Crosswalk
  │    -> 多帧像素投票 -> 最大稳定区凸包 -> --roi
  └─ Traffic Light
       -> 多帧像素投票 -> 邻近分量合并 -> 全部稳定候选编号
       -> 持续率 + 与斑马线中心距离 + 面积评分
       -> 自动主灯，可用 --main-light-index 人工覆盖
       -> 主灯语义框内聚合跨帧高亮红/绿核心并加少量边距
       -> --light-roi
```

### 5.1 安装和模型准备

安装 Python 依赖：

```powershell
cd D:\Yolo_LPR_RK3568_FPGA_Project\4_NPU_Yolov8_Traffic_Demo pip install -r .\pc_tools\requirements-mask2former.txt
```

工具固定使用公开模型 `facebook/mask2former-swin-large-mapillary-vistas-semantic`，首次运行由
Transformers 下载约 866 MB 权重并缓存，不需要接受 gated 协议，也没有 `--model` 或 `--prompt`
参数。模型与接口说明见 [Hugging Face 模型页](https://huggingface.co/facebook/mask2former-swin-large-mapillary-vistas-semantic)
和 [Transformers Mask2Former 文档](https://huggingface.co/docs/transformers/model_doc/mask2former)。

### 5.2 自动标定

```powershell
python .\pc_tools\auto_crosswalk_roi_mask2former.py `
  --video "D:\Yolo_LPR_RK3568_FPGA_Project\4_NPU_Yolov8_Traffic_Demo\test\test2.mp4" `
  --ffmpeg "D:\ffmpeg-8.1-essentials_build\bin\ffmpeg.exe" `
  --sample-every 2 `
  --max-frames 50 `
  --device 0 `
  --interval 2
```

斑马线默认要求至少 40% 的抽样帧检测到掩码，并保留在至少 60% 有效帧中出现的像素。交通灯
默认保留至少出现在 10% 全部抽样帧中的稳定区域，将距离较近的语义分量合并为同一个灯体候选。
候选 ID 按画面从左到右排列；自动主灯优先考虑跨帧持续率，其次考虑与斑马线中心的距离，面积只
占少量权重。自动结果不是不可更改的首帧锁定：必须查看
`traffic_light_roi_preview.jpg`，若主灯标记错误，按预览中的编号增加
`--main-light-index N` 重新运行。

Mask2Former 的 `Traffic Light` 语义块可能把并排的行人灯、车行灯和灯罩连成一个候选。确定 MAIN
候选后，工具会在该候选内累计各抽帧的高亮红/绿像素，将距离接近的红灯与绿灯核心合并，再使用
较小的 `--light-padding-ratio` 生成最终固定矩形。预览中的橙框表示用于候选评分的完整语义框，
红框 `MAIN CORE` 才是实际写入 `--light-roi` 的区域。默认核心门槛为亮度 `160`、饱和度 `100`；
找不到可靠核心时显示 `MAIN FALLBACK` 并回退到语义框，避免产生空 ROI。

程序的标准输出最后直接给出可复制的板端命令：

```bash
./yolov8_traffic_pcie_demo ./model/yolov8_traffic_i8.rknn \
  --interval 2 \
  --roi "1.000000,0.690454;1.000000,0.762743;0.000000,0.886932;0.000000,0.645042;0.675873,0.617238" \
  --light-roi "0.547917,0.235185,0.581250,0.339815"
```

默认结果目录为 `results/mask2former_roi/<视频文件名>/`：

```text
crosswalk_roi_preview.jpg       最终多边形叠加预览
crosswalk_consensus_mask.png    多帧投票后的二值掩码
traffic_light_roi_preview.jpg   候选语义框及最终 MAIN CORE 固定框
traffic_light_consensus_mask.png 交通灯多帧投票掩码
main_traffic_light_roi.txt      可直接复制的归一化主灯矩形
traffic_roi.conf                可直接替换板端默认文件的两块固定区域
traffic_scene_roi.json          两类 ROI、候选统计和完整板端命令
crosswalk_roi.json              与旧工作流兼容的同内容 JSON
```

常用调节项：

- 白色条纹没有连成完整区域：适当增大 `--close-ratio`，默认值为 `0.050`。
- 有效帧不足：适当降低 `--min-valid-ratio`，但必须检查预览图是否仍对应目标斑马线。
- 偶发错误掩码进入 ROI：增大 `--vote-threshold`，并检查预览图。
- 交通灯偶发漏检：降低 `--light-vote-threshold`；候选碎裂时增大 `--light-merge-ratio`。
- 自动 MAIN 不是目标行人灯：按预览候选编号传入 `--main-light-index N`，不要修改板端代码。
- `MAIN CORE` 仍偏大：适当提高 `--light-core-min-value` 或
  `--light-core-min-saturation`；框过紧则降低对应门槛或增大 `--light-padding-ratio`。
- 视频开头黑屏或机位尚未稳定：使用 `--start 5` 跳过前 5 秒。

## 6. Ubuntu 交叉编译

```bash
cd /path/to/Yolo_LPR_RK3568_FPGA_Project/4_NPU_Yolov8_Traffic_Demo
export GCC_COMPILER=/path/to/toolchain/bin/aarch64-linux-gnu
sed -i 's/\r$//' build-linux.sh
chmod +x build-linux.sh
./build-linux.sh
```

默认复用 `../3_NPU_Yolov8_PPOCR_Demo/3rdparty` 中的 RKNN Runtime、RGA 和 TurboJPEG。
`librga.a` 依赖 pthread，两个可执行目标都在静态库之后链接 `Threads::Threads`，避免
`pthread_mutexattr_init` / `DSO missing from command line`。

安装布局：

```text
install/rk356x_linux_aarch64/rknn_yolov8_traffic_demo/
├── lib/
├── yolov8_traffic_benchmark/
│   ├── yolov8_traffic_benchmark
│   ├── model/
│   └── test/
└── yolov8_traffic_pcie_demo/
    ├── yolov8_traffic_pcie_demo
    ├── pango_pci_driver.ko
    └── model/
```

## 7. 板端运行

在主机端进入本工程目录，将包含动态库、两个可执行程序、模型和 PCIe 驱动的完整安装目录
推送到板端，并赋予可执行权限：

```bash
adb push install/rk356x_linux_aarch64/rknn_yolov8_traffic_demo/ /userdata/
adb shell "chmod +x \
  /userdata/rknn_yolov8_traffic_demo/yolov8_traffic_benchmark/yolov8_traffic_benchmark \
  /userdata/rknn_yolov8_traffic_demo/yolov8_traffic_pcie_demo/yolov8_traffic_pcie_demo"
```

图片 benchmark：

```bash
cd /userdata/rknn_yolov8_traffic_demo/yolov8_traffic_benchmark
export LD_LIBRARY_PATH=../lib:$LD_LIBRARY_PATH
./yolov8_traffic_benchmark \
  ./model/yolov8_traffic_i8.rknn \
  ./test/traffic_test.png \
  20 3 ./outputs
```

PCIe 版本运行前必须确认驱动模块与板端内核兼容。当前复用模块的记录 vermagic 为
`6.1.99 SMP mod_unload aarch64`，不能用 `insmod -f` 强制加载：

```bash
# 切到命令行模式，立刻关闭3568桌面
sudo systemctl isolate multi-user.target

cd /userdata/rknn_yolov8_traffic_demo/yolov8_traffic_pcie_demo
export LD_LIBRARY_PATH=../lib:$LD_LIBRARY_PATH

uname -r
modinfo ./pango_pci_driver.ko | grep -E 'name|vermagic'
if lsmod | grep -q '^pango_pci_driver '; then
  rmmod pango_pci_driver
fi
insmod ./pango_pci_driver.ko
ls -l /dev/pango_pci_driver

./yolov8_traffic_pcie_demo ./model/yolov8_traffic_i8.rknn --interval 2
```

程序需要独占 DRM master。若日志显示 `cannot acquire master` 或 `Device or resource busy`，应先定位并
停止正在占用 KMS 的桌面显示服务，不能绕过 master 检查。正常退出后检查终端统计和内核日志：

```bash
dmesg | tail -n 80 | grep -Ei 'pango|pci|bar|dma|error|fail'
```

## 8. 已验证性能与待验证项

八类 INT8 图片 benchmark 的首轮 RK3568 实板结果：

```text
images=1
detections=6
warmup_per_image=3
repeat_per_image=20
measured_calls=20
pipeline_average_ms=39.071
pipeline_fps=25.594
npu_samples=20
npu_average_ms=31.126
npu_fps=32.128
```

规则与时序模块已用构造数据验证：默认/自定义固定灯区解析和像素映射、固定灯区忽略任意 YOLO
traffic-light 结果、RGB888/BGR565 红绿通道顺序、弱红按绿处理、5 帧灯色迟滞投票、person ID
保持、框平滑、3% ROI 内缩、红灯入区立即违法和违规 ID 去重均通过；PC 标定工具的邻近候选
合并、自动评分、人工覆盖和双 ROI 命令生成也有独立回归。交通规则、时序和叠加层源文件已通过
MSVC 编译。Windows 全目标构建仍会在既有 `image_utils.c`
的 Linux `dirent.h` 依赖处停止。仍需实板确认：

- Ubuntu aarch64 完整编译与链接。
- FPGA BGR565 实际颜色顺序与灯色阈值。
- 默认/自定义 ROI 的显示位置和 person 底边判定。
- DRM 全屏显示、退出资源回收和长时间运行性能。
- Mask2Former 交通灯候选预览、人工 MAIN 复核，以及红绿完整周期内固定框是否持续覆盖发光区域。
- PC 端曾使用公开的 Mapillary Vistas Mask2Former 语义分割模型。使用 CUDA 对 3840×2160
  抽样帧实测 8/8 帧均得到目标掩码，
  5 票像素共识与最大区域凸包生成以下历史 5 点 ROI：

```text
0.969263,0.721167;
0.225580,1.000000;
0.000000,1.000000;
0.000000,0.842520;
0.684032,0.691061
```

  结果保存在 `results/mask2former_roi/test5/`；当前内置默认值已经改为本节上方的新测试
  人行道 ROI，不再使用这组历史坐标。板端使用前仍应以实际 person 底边落点复核边界。
  早期零样本方案的权重、字节码及其专用 CLIP Python 依赖已清理。
