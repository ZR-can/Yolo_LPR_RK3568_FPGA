
# 3_NPU_Yolov8_PPOCR_Demo 用法

## 当前状态

工程主入口现为 `main_ppocr.cc`，面向 PCIe 输入的 1280×720 小端 BGR565 视频帧，执行 YOLOv8 车牌定位和 PP-OCRv4 字符识别。原 PCIe/LPRNet 入口仅改名为 `main_lpr.cc`，继续构建为 `yolov8_lpr_pcie_demo` 供同源对照；Qt UI 代码本轮不修改，仍连接 LPR 基准入口。

MPP 视频和单图片入口已删除，构建不再生成 `yolov8_lpr_video_demo` 或 `yolov8_lpr_picture_demo`。新的 `yolov8_ppocr_pcie_demo` 继续复用原 PCIe 采集、6 槽帧池、每 2 帧推理一次、DIoU 跟踪、长度截断、逐位合法性校验、有效结果投票、连续 2 次命中后显示、RGA 叠加和 DRM 输出逻辑。当前改动已完成 Windows 工作区编码，尚待 Ubuntu aarch64 交叉编译和 RK3568 + FPGA 实链路验证。

## 开发记录

### 2026-07-23 YOLOv8 + PP-OCR PCIe 级联适配

- 接口已经对齐：`post_process()` 先去除 YOLO 640x640 letterbox 的 padding，再除以缩放系数，将检测框映射回 1280x720 BGR565 原帧坐标；坐标现统一为半开区间 `[left,right)×[top,bottom)`，左/上向下取整、右/下向上取整，右/下最大允许等于原图宽高，避免紧框或贴边车牌再损失一行/列像素。`yolo_ppocr_pipeline.cc` 直接以该坐标裁剪车牌 ROI，不改变 Tracker 和显示端使用的坐标系。
- PP-OCR 输入是单张 48x160 BGR 图像：ROI 按高度 48 等比例缩放，宽度向上取整且不超过 160，右侧以原始像素值 128 填充；RKNN 输入为 UINT8/NHWC，模型内嵌 `(x-127.5)/127.5` 归一化。输出为 `[1,20,74]`，类别 0 是 CTC blank，其余 73 类按 `cblprd_plate_dict.txt` 解码并去除连续重复。
- `ppocr_rec.cc` 新增 BGR565 原帧 ROI 输入，避免为每个框生成整帧 RGB888；RGB888 图片评估接口保持兼容。主 pipeline 的处理顺序为：CTC 解码 -> `ga36_plate_type_v3` 字符修正 -> 按蓝/绿牌目标位数截断 -> GA 36 类型化校验 -> 有效结果投票 -> 连续命中显示。
- `ga36_plate_type_v3` 将发牌机关代号与序号字符规则分离：普通、警、学、港、澳牌第二位按 `A-Z` 发牌机关处理并允许合法 `I/O`，序号禁止 `I/O` 且最多包含 2 位字母；警牌不再允许数字 `0/1` 作为第二位。使馆牌按“6 位数字 + 使”校验，领馆牌按“省份 + 3 位数字机构编号 + 数字开头的 2 位序号 + 领”校验。
- 新能源牌按 GA 36 表 5/表 6 校验：能源字母允许 `A/B/C/D/E/F/G/H/J/K`；小型新能源要求能源字母位于第 3 位、第 4 位可为 1 位序号字母且其余为数字，大型新能源要求能源字母位于末位且前 5 位序号为数字。部署端保留 `0 -> D` OCR 恢复，但仅在小型/大型候选中恰好一种结构合法时改写；已有合法能源位或两种候选都成立时保持原文，避免把合法序号末位或首位数字 `0` 误改为 `D`。按当前项目范围，不增加 `挂/试/超`。
- `normalize_plate_prediction()` 处理完整 7/8 位文本；`correct_plate_prediction_for_pipeline()` 继续支持超长输出先按类型纠正再截断。使馆牌不再要求省份前缀，领馆和使馆数字区域执行 `O/I -> 0/1`，其余号牌的发牌机关位置执行 `0/1 -> O/I`、序号区域执行 `O/I -> 0/1`。
- 工程目录已改名为 `3_NPU_Yolov8_PPOCR_Demo`；原 `main_pcie.cc` 改名为 `main_lpr.cc`，新的 `main_ppocr.cc` 是 PCIe 主入口，`main_video.cc` 和 `main_picture.cc` 已删除。Qt UI 源码未改动。
- 部署模型固定为 `ppocrv4_rec14_fold_affine_1x1_rk3568_hybrid_mmse_h2_add27_hsw4.rknn`；CMake 只向 PP-OCR PCIe 目录安装该模型、YOLO 模型、73 字符字典和 PCIe 驱动。
- `ga36_plate_type_v3` 已完成主机侧 MSVC 编译和 `plate_rule_test` 回归：覆盖发牌机关 `I/O`、警牌数字第二位拒绝、使/领馆独立结构、普通及专用牌序号字母上限、十个新能源能源字母的大小型位置、`0 -> D` 恢复、超长纠正后截断，以及 `挂` 保持不支持。编译仅报告 `simple_tracker.cc` 原有的整数到浮点转换告警。当前环境没有 aarch64 交叉编译器，仍需 Ubuntu 交叉编译及 RK3568 PCIe 实流复测。
- 两份板端 `details.tsv` 已按“字符修正 -> 位数截断 -> 严格相等”重放验证，且将 `basic/hard` 中的 6157 张 8 位新能源牌单独统计。修复后的 v3 使 hybrid 模型从 `84.208100%` 提升到 `86.904419%`，FP16 模型从 `88.148874%` 提升到 `90.597453%`；完整口径、分组结果和异常样本见 [`results/ga36_plate_type_v3_replay_report.md`](results/ga36_plate_type_v3_replay_report.md)。
- PP-OCR 级联现增加同帧条件回退：主识别仍使用原 YOLO ROI 和训练一致的定高 48、宽度上限 160、右填 128 预处理；仅当“v3 字符修正 -> 位数截断 -> GA 36 校验”仍无效时，将原 ROI 四边各扩张 5%（每边至少 1 像素）并在同一 `PcieFrame`、同一 `ppocr_ctx` 上复推。每帧最多执行 1 次回退，二次结果只有通过 v3 才替换无效主结果，任何有效主结果都不会被覆盖；不重新运行 YOLO、不重新进入帧队列，也不产生第二次 Tracker 更新。
- `yolo_lprnet_crops` 的 17031 条原始预测按当前 v3 重放后，严格结构无效触发率为 `403/17031=2.3663%`，蓝/绿牌分别为 `2.1571%/3.5700%`，403 个触发样本均为错误结果。按 H2 单次 PP-OCR 约 `9.30 ms` 估算，平均额外开销约 `0.22 ms/车牌`；实际 H2 + PCIe BGR565 + 实时 YOLO ROI 触发率仍需板端统计确认。
- 新增 `ppocr_retry_policy_test`，覆盖半开 ROI 边界夹紧、5% 扩框、图像边缘保护以及蓝/绿牌 v3 重试判定；与既有 `plate_rule_test` 均已在主机侧 MSVC 通过。PCIe 性能摘要新增主识别次数、回退次数、回退接受次数和平均回退耗时，便于板端确认触发率、额外时延及推理队列丢帧是否变化。
- 修复“终端有 PP-OCR 字符但 DRM 只显示检测框”：有效号牌仍按 GA 36 v3 校验并进入原投票池；没有有效投票时，仅连续 2 次得到完全相同的非空文本才在画面显示，并以 `RAW` 明确标记为未通过校验，该文本不会进入有效投票或 Qt 状态结果。识别日志新增 `ga36_status=valid/invalid`；关闭每帧成功路径的 `rknn_run`、letterbox `scale/fill` 调试输出，并将 PCIe 等待状态统一写入 stdout，避免 `2>&1 | tee` 时跨线程日志插入另一行中间。主机侧 `plate_rule_test` 已新增 `湘VWUJ3N` RAW 回退用例并通过，仍需重新交叉编译后验证 DRM 字体叠加。

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

## 历史资料说明

以下章节保留原 LPR/MPP/Qt 联调记录用于追溯；其中引用 `main_video.cc`、`main_picture.cc`、`main_pcie.cc`、`yolov8_lpr_video_demo` 或 `yolov8_lpr_picture_demo` 的命令已经失效。当前构建、部署和运行以本文前部“当前构建与运行”为准。

## 文件构成介绍

```shell

```

## Linux环境搭建

1. 首先安装虚拟机，Ubuntu20即可，建议分配至少20g，内存6g，处理器2个每个4核，可根据自己电脑性能调整
2. 共享文件夹，将3_NPU_Yolov8_PPOCR_Demo文件夹共享给虚拟机，共享后文件一般放在虚拟机目录：/mnt/hgfs/Yolo_LPR_RK3568_FPGA_Project/3_NPU_Yolov8_PPOCR_Demo，其后运行时每次从这里打开终端即可

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
建议开两个终端，一个主终端：在/mnt/hgfs/Yolo_LPR_RK3568_FPGA_Project/3_NPU_Yolov8_PPOCR_Demo交叉编译用，一个从终端：专门登入板端
```shell
cd /mnt/hgfs/Yolo_LPR_RK3568_FPGA_Project/3_NPU_Yolov8_PPOCR_Demo
export GCC_COMPILER=<GCC_COMPILER_PATH> #配置好后可省略

./build-linux.sh -t rk356x -a aarch64 -d yolov8_ppocr
```
/bin/bash^M 表示 build-linux.sh 被保存为 Windows 的 CRLF 换行。
在 Ubuntu 共享目录执行：
```shell
sed -i 's/\r$//' build-linux.sh
chmod +x build-linux.sh
./build-linux.sh -t rk3568 -a aarch64 -d yolov8_lpr
```

板端 PP-OCR 主链路：

```bash
cd /userdata/rknn_yolov8_ppocr_demo/yolov8_ppocr_pcie_demo
./yolov8_ppocr_pcie_demo \
  ./model/yolov8.rknn \
  ./model/ppocrv4_rec14_fold_affine_1x1_rk3568_hybrid_mmse_h2_add27_hsw4.rknn \
  ./model/cblprd_plate_dict.txt
```

LPRNet 基准对照：

```bash
cd /userdata/rknn_yolov8_ppocr_demo/yolov8_lpr_pcie_demo
./yolov8_lpr_pcie_demo \
  ./model/yolov8.rknn \
  ./model/lprnet7repair_i8.rknn \
  ./model/lprnet8repair_i8.rknn
```

`pango_pci_driver.ko` 的 vermagic 为 `6.1.99 SMP mod_unload aarch64`，加载前必须确认板端内核兼容；DRM 直显仍要求先停止占用 KMS master 的桌面服务。

## Push demo files to device

```shell
#主终端使用
#删除板端原有旧demo
adb shell rm -rf /userdata/rknn_yolov8_ppocr_demo

#推送完整安装目录，包含PP-OCR PCIe主程序、LPR基准程序、评测程序、模型、字典、驱动和依赖库
adb push install/rk356x_linux_aarch64/rknn_yolov8_ppocr_demo /userdata/

#核对主程序和部署文件
adb shell ls -lh \
  /userdata/rknn_yolov8_ppocr_demo/yolov8_ppocr_pcie_demo/yolov8_ppocr_pcie_demo \
  /userdata/rknn_yolov8_ppocr_demo/yolov8_ppocr_pcie_demo/model/yolov8.rknn \
  /userdata/rknn_yolov8_ppocr_demo/yolov8_ppocr_pcie_demo/model/ppocrv4_rec14_fold_affine_1x1_rk3568_hybrid_mmse_h2_add27_hsw4.rknn \
  /userdata/rknn_yolov8_ppocr_demo/yolov8_ppocr_pcie_demo/model/cblprd_plate_dict.txt \
  /userdata/rknn_yolov8_ppocr_demo/yolov8_ppocr_pcie_demo/pango_pci_driver.ko
```

## Run demo and pull result

```shell
#从终端使用
#登入板端
adb shell

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
```

```shell
#从终端使用
#确认可执行权限
chmod +x /userdata/rknn_yolov8_ppocr_demo/yolov8_ppocr_pcie_demo/yolov8_ppocr_pcie_demo
chmod +x /userdata/rknn_yolov8_ppocr_demo/yolov8_lpr_pcie_demo/yolov8_lpr_pcie_demo
chmod +x /userdata/rknn_yolov8_ppocr_demo/ppocr_rec_eval_demo/ppocr_rec_eval_demo
chmod +x /userdata/rknn_yolov8_ppocr_demo/ppocr_rec_eval_demo/eval_ppocr_rknn_subsets.sh
```

```shell
#pcie_demo
#1. 检查内核版本和PCIe枚举/链路状态
uname -a
uname -r

cd /userdata/rknn_yolov8_ppocr_demo/yolov8_ppocr_pcie_demo
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

#2. 卸载可能残留的旧模块，再加载随demo安装的模块
if lsmod | grep -q '^pango_pci_driver '; then
    rmmod pango_pci_driver
fi
insmod ./pango_pci_driver.ko

lsmod | grep pango_pci_driver
ls -l /dev/pango_pci_driver
dmesg | tail -n 80 | grep -Ei 'pango|pci|bar|dma|error|fail'
```

如果 `lspci` 和 `/sys/bus/pci/devices/` 都没有显示 FPGA Endpoint，应先检查 FPGA 上电、PCIe 参考时钟、复位、RK3568 Root Complex 配置和物理链路，不要继续运行 demo。

只有模块出现在 `lsmod`、`/dev/pango_pci_driver` 存在且 `dmesg` 无 probe/BAR/DMA 致命错误时，才继续运行。若 `insmod` 返回 `Invalid module format`，应检查 `uname -r`、模块 vermagic 和 `dmesg`，不能使用 `-f` 强制加载。

```shell
#3. 启动YOLOv8 + PP-OCR PCIe主demo
cd /userdata/rknn_yolov8_ppocr_demo/yolov8_ppocr_pcie_demo
export RKNN_LOG_LEVEL=0
./yolov8_ppocr_pcie_demo \
  ./model/yolov8.rknn \
  ./model/ppocrv4_rec14_fold_affine_1x1_rk3568_hybrid_mmse_h2_add27_hsw4.rknn \
  ./model/cblprd_plate_dict.txt \
  2>&1 | tee ppocr_pcie.log
```

正常启动应依次看到 PCIe vendor/device、Link Gen/Width、MPS、BGR565 1280×720 采集、YOLOv8 和 PP-OCR 模型初始化信息。识别日志会输出 `PP-OCR primary=... retry=... final=... score=... retry_status=... ga36_status=...`；`ga36_status=valid` 的结果保持原有效投票与连续命中显示逻辑，`ga36_status=invalid` 的相同文本连续命中 2 次后仅以 `RAW` 标记显示，不进入有效投票。正常运行不再逐帧打印 `rknn_run` 和 letterbox `scale/fill`；首次成功读取出现的 `read returned driver status 2` 表示 DMA 缓冲区中已有一帧完整的 1843200 字节图像。

使用 `Ctrl+C` 退出。终端应先打印 `PCIe: stop requested`，随后打印 PCIe Pipeline Statistics；同时确认 DRM 画面持续覆盖整个显示区域，退出时没有 DMA unmap 错误。若出现 `cannot acquire master`，必须先停止桌面显示服务，不能让 demo 与桌面同时控制 KMS。

```shell
#可选：运行原LPR模型作为同源PCIe基准对照
cd /userdata/rknn_yolov8_ppocr_demo/yolov8_lpr_pcie_demo
export RKNN_LOG_LEVEL=0
./yolov8_lpr_pcie_demo \
  ./model/yolov8.rknn \
  ./model/lprnet7repair_fp.rknn \
  ./model/lprnet8repair_fp.rknn \
  2>&1 | tee lpr_pcie.log
```

```shell
#退出板端终端命令为logout
logout

#主终端使用
#当前PCIe demo通过DRM显示结果，不再生成picture/video结果目录；拉取终端日志和内核诊断
mkdir -p ./result/log
adb pull /userdata/rknn_yolov8_ppocr_demo/yolov8_ppocr_pcie_demo/ppocr_pcie.log ./result/log/
adb shell dmesg > ./result/log/dmesg_pcie.log

#若运行了LPR基准对照，再拉取该日志
#adb pull /userdata/rknn_yolov8_ppocr_demo/yolov8_lpr_pcie_demo/lpr_pcie.log ./result/log/
```

```shell
#恢复3568桌面
adb shell
sudo systemctl set-default graphical.target
sudo reboot
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
- `src/pcie_frame_source.cc` opens the driver with `O_NONBLOCK`, so exit and
  pause paths do not depend on an indefinitely blocking driver read.

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
