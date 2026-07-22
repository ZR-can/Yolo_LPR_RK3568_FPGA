# RK3568 八类交通 YOLOv8 INT8 / PCIe 闯红灯验证 Demo

更新时间：2026-07-22

板端小交通灯实测出现 `red=2～3 green=0` 时，旧版 8 像素门槛会错误输出 `raw=unknown`。当前已将
最小红/绿有效像素数调为 2，继续保留 1.2 倍颜色优势约束；终端日志新增 `color_active` 表示通过
饱和度和亮度门槛的颜色有效像素数，原 `active` 仍表示当前违规行人数。

本工程包含两个相互独立的板端程序：

- `yolov8_traffic_benchmark`：单图或图片目录检测，用于确认模型结果和 NPU 性能。
- `yolov8_traffic_pcie_demo`：复用 `3_NPU_Yolov8_LPR_Demo` 的 PCIe、固定帧槽和 DRM
  显示链路，实时检测 `person` / `traffic light`，并执行可配置斑马线 ROI 闯红灯规则。
- `pc_tools/auto_crosswalk_roi_mask2former.py`：PC 端调用本地 FFmpeg 和 Mapillary Vistas
  Mask2Former，从固定机位视频自动生成稳定的斑马线多边形及板端完整运行命令。

PCIe 版本不依赖 LPRNet、MPP、OpenCV 或 Qt；只包含轻量 person 空间跟踪，不引入 Kalman、ReID
或车牌文字投票。当前已经完成 Windows 工作区代码迁移、
规则单元验证和 C++ 静态语法检查，尚需在 Ubuntu 重新交叉编译并在 RK3568 + FPGA 实链路复测。

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

后处理阈值为 `BOX_THRESH=0.25`、`NMS_THRESH=0.5`。图片 benchmark 仍解析并输出全部 8 类；
PCIe 模式从输出头开始只比较 `0 person` 和 `6 traffic light`。NMS 后先向 128 个结果槽写入
traffic light，再写入 person，避免其他六类或密集人框挤掉有效交通灯。

## 2. PCIe 实时链路

```text
FPGA 1280x720 小端 BGR565
  -> Pango PCIe DMA 读取
  -> 6 个固定 BGR565 帧槽
  -> 显示队列（容量 2，每帧）
  -> 推理队列（容量 1，默认每 2 帧）
  -> YOLOv8 INT8
  -> person / traffic light 专用后处理
  -> 5 帧灯色投票 + person ID/框平滑/漏检保持
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

1. 选择红/绿颜色证据最强的 `traffic light` 检测框。
2. 直接解码该框内 BGR565 像素并转为 HSV；低饱和度、低亮度像素不参与统计。
3. 红色或绿色有效像素至少为 2，且相对另一颜色达到 `1.2` 倍时，判定对应灯色；否则为
   `unknown`。
4. 最近 5 个推理帧做灯色投票，至少 3 票确定红/绿；已有稳定状态剩余至少 2 票时短时保持。
5. person 使用 IoU + 中心距离匹配 ID，框坐标按 `0.65` 新框、`0.35` 旧框平滑；最多保持
   8 个连续漏检推理帧，避免隔帧推理时跳动或闪烁。
6. 在平滑后的 `person` 框底边均匀取 5 个点，至少 1 个点落入斑马线多边形即记为在 ROI 内。
7. 稳定红灯且人在 ROI 内时框持续为 `red_violation`；同一 `track_id` 在本次运行期间最多计为
   1 名闯红灯人员，离开并再次进入 ROI 不会重复累计人数。

交通灯检测框也会短时保持 5 个推理帧，用于减少框闪烁。灯色票数不足时状态为 `unknown`，不会
误报闯红灯。轻量 person 跟踪只服务于显示稳定和事件去重，不做身份识别。

显示约定：

- 斑马线 ROI：半透明蓝色多边形、蓝色粗边框和圆角深色 `CROSSWALK ROI` 背景板。
- `red_violation` 人框：红色；保持红框不等于重复生成事件。
- 绿灯且人框底边位于 ROI 内：绿色，标签为 `person#ID/person/green_pass`。
- 其他 person：黄色。
- 漏检保持的框带 `/hold`；person 标签包含稳定的 `track_id`。
- 被选中的 traffic light：按投票后红/绿状态显示；未知或非主灯为黄色。
- 左上角状态栏使用较大白字，只显示 `LIGHT=稳定灯色`、`violation=累计违规 ID 数`、
  `person=累计分配 ID 数` 和最近一次 `infer` 耗时；当前存在红框时为红色背景，否则为绿色背景。

叠加层只在获得新推理结果时由 CPU 重建，随后用 RGA alpha blend 合成到 DRM RGBA buffer，
避免在不可映射的 DRM buffer 上逐像素绘制。

## 4. 斑马线多边形接口

不传 `--roi` 时使用当前验证代码中的默认归一化多边形：

```text
0.969263,0.721167;
0.225580,1.000000;
0.000000,1.000000;
0.000000,0.842520;
0.684032,0.691061
```

板端可用 `--roi` 临时覆盖，至少输入 3 个点，坐标范围必须为 `[0,1]`，点之间用分号分隔：

```bash
./yolov8_traffic_pcie_demo ./model/yolov8_traffic_i8.rknn \
  --interval 2 \
  --roi "0.08,0.62;0.90,0.58;0.96,0.91;0.03,0.88"
```

坐标相对于原始 1280×720 图像归一化。每次改变机位、裁剪或画面比例后都必须重新标定 ROI。
多边形可以包含任意数量的顶点（至少 3 个）。person 底边仍独立均匀采样 5 个点，只要其中至少
1 个点位于多边形内，就判定该 person 进入斑马线 ROI；采样点数与多边形顶点数没有对应关系。

## 5. PC 端 Mask2Former 自动 ROI 标定

该工具仅用于 PC 离线标定，不参与板端实时推理，也不需要将 Mask2Former 转换为 ONNX/RKNN。
Mapillary Vistas 模型直接包含斑马线语义类别，适合从夜间固定机位画面恢复完整路面 ROI。
处理链路为：

```text
固定机位视频
  -> 本地 FFmpeg 定时抽帧
  -> Mapillary Vistas Mask2Former 语义分割
  -> 合并 Crosswalk - Plain / Lane Marking - Crosswalk
  -> 单帧掩码闭运算
  -> 有效帧像素级多数投票
  -> 最大稳定区域凸包和多边形简化
  -> 按 (width-1,height-1) 归一化
  -> 输出 --roi 板端命令
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
  --video "D:\Yolo_LPR_RK3568_FPGA_Project\4_NPU_Yolov8_Traffic_Demo\test\test5.mp4" `
  --ffmpeg "D:\ffmpeg-8.1-essentials_build\bin\ffmpeg.exe" `
  --sample-every 2 `
  --max-frames 50 `
  --device 0 `
  --interval 2
```

默认要求至少 40% 的抽样帧检测到掩码，并保留在至少 60% 有效帧中出现的像素。多帧直接对
原分辨率掩码投票，不平均顶点数量和顺序不一致的单帧多边形。若画面中存在多个斑马线，当前
版本选择投票后面积最大的连通区域，并取凸包覆盖条纹之间的可通行间隙，生成无自交 ROI。

程序的标准输出最后直接给出可复制的板端命令：

```bash
./yolov8_traffic_pcie_demo ./model/yolov8_traffic_i8.rknn \
  --interval 2 \
  --roi "0.969263,0.721167;0.225580,1.000000;0.000000,1.000000;0.000000,0.842520;0.684032,0.691061"
```

默认结果目录为 `results/mask2former_roi/<视频文件名>/`：

```text
crosswalk_roi_preview.jpg       最终多边形叠加预览
crosswalk_consensus_mask.png    多帧投票后的二值掩码
crosswalk_roi.json              坐标、统计参数和完整板端命令
```

常用调节项：

- 白色条纹没有连成完整区域：适当增大 `--close-ratio`，默认值为 `0.050`。
- 有效帧不足：适当降低 `--min-valid-ratio`，但必须检查预览图是否仍对应目标斑马线。
- 偶发错误掩码进入 ROI：增大 `--vote-threshold`，并检查预览图。
- 视频开头黑屏或机位尚未稳定：使用 `--start 5` 跳过前 5 秒。

## 6. Ubuntu 交叉编译

```bash
cd /path/to/Yolo_LPR_RK3568_FPGA_Project/4_NPU_Yolov8_Traffic_Demo
export GCC_COMPILER=/path/to/toolchain/bin/aarch64-linux-gnu
sed -i 's/\r$//' build-linux.sh
chmod +x build-linux.sh
./build-linux.sh
```

默认复用 `../3_NPU_Yolov8_LPR_Demo/3rdparty` 中的 RKNN Runtime、RGA 和 TurboJPEG。
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

规则与时序模块已用构造数据验证：BGR565 红/绿判断、5 帧灯色多数投票、person ID 保持、框平滑、
短时漏检保持和违规 ID 去重均通过；相关源文件和 PCIe 主程序已通过
本地 C++ 静态语法检查。仍需实板确认：

- Ubuntu aarch64 完整编译与链接。
- FPGA BGR565 实际颜色顺序与灯色阈值。
- 默认/自定义 ROI 的显示位置和 person 底边判定。
- DRM 全屏显示、退出资源回收和长时间运行性能。
- 小目标交通灯的检测稳定性；若 5 帧投票和灯框保持仍不足，再增加固定灯区兜底。
- PC 端使用公开的 Mapillary Vistas Mask2Former 语义分割模型。使用 CUDA 对 3840×2160
  抽样帧实测 8/8 帧均得到目标掩码，
  5 票像素共识与最大区域凸包生成以下 5 点 ROI：

```text
0.969263,0.721167;
0.225580,1.000000;
0.000000,1.000000;
0.000000,0.842520;
0.684032,0.691061
```

  结果保存在 `results/mask2former_roi/test5/`，板端使用前应以预览图和实际 person 底边落点复核边界。
  早期零样本方案的权重、字节码及其专用 CLIP Python 依赖已清理。
