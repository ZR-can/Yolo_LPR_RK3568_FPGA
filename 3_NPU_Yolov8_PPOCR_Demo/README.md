
# 3_NPU_Yolov8_PPOCR_Demo 操作手册

## 当前状态

工程主入口现为 `main_ppocr.cc`，面向 PCIe 输入的 1280×720 小端 BGR565 视频帧，执行 YOLOv8 车牌定位和 PP-OCRv4 字符识别。原 PCIe/LPRNet 入口仅改名为 `main_lpr.cc`，继续构建为 `yolov8_lpr_pcie_demo` 供同源对照。Qt UI 已迁移到 [`../5_QT_UI_Demo`](../5_QT_UI_Demo/README.md)：视频模式通过 `RunPpocrPcieDemo()` 使用微调 YOLO 和原轴对齐后处理；静态图片模式通过 `RunPpocrPcieImageDemo()` 使用 OBB 模型、旋转 NMS 和旋转矫正后的 PP-OCR 输入，同时检测图片 generation、隔离换图结果并连续确认；本目录不再构建 Qt 目标。

MPP 视频和文件型单图片入口已删除，构建不再生成 `yolov8_lpr_video_demo` 或 `yolov8_lpr_picture_demo`。新的 `yolov8_ppocr_pcie_demo` 继续复用原 PCIe 采集、6 槽帧池、每 2 帧推理一次、带高速首联尺寸门、真实观测速度、受限加速度和 8 帧短空窗预测的 DIoU 跟踪、长度截断、逐位合法性校验、有效结果投票、连续 2 次命中后显示、RGA 叠加和 DRM 输出逻辑。项目 5 的“图片识别”仍读取 PCIe 静态画面，不是文件选择器；它把 OBB 四角旋转矫正为右侧填充 128 的 `48×160` BGR 输入并使用 FP16 PP-OCR，再以静态图片 generation 隔离跨图片结果后复用 Tracker 稳定确认。当前改动已完成 Windows 工作区编码，尚待 Ubuntu aarch64 交叉编译和 RK3568 + FPGA 实链路验证。

`model/finetune_i8.rknn` 是项目 5 视频模式的车牌检测模型源文件；项目 5 构建时将其重命名
安装为 `yolov8_ppocr_pcie_qt_ui/model/yolov8.rknn`，阈值保持 `BOX_THRESH=0.55`。
图片模式另从项目 2 安装 `yolov8_obb_i8.rknn` 为 `model/yolov8_obb.rknn`。最终板端部署
按用户指定使用 `conf=0.55`、同类别旋转 NMS `0.55`。蓝/绿候选框 IoU 不低于 `0.65` 且
检测分差不超过 `0.10` 时，先统计矫正车牌有效区域的蓝/绿色饱和像素；颜色证据至少相差
25% 才选色。颜色证据不足时两个冲突候选均丢弃，不再送入 PP-OCR/Tracker，也不再按
GA 36、OCR 分数或绿色 8 位规则兜底。本目录原 `model/yolov8.rknn` 只继续服务项目 3 独立命令行
Demo，不再由项目 5 引用。

开发过程、模型验证和性能结论见 [DEVELOPMENT_RECORD.md](DEVELOPMENT_RECORD.md)。

## 历史资料说明

以下章节保留原 LPR/MPP 操作资料用于追溯；其中引用 `main_video.cc`、`main_picture.cc`、`main_pcie.cc`、`yolov8_lpr_video_demo` 或 `yolov8_lpr_picture_demo` 的命令已经失效。原 Qt FPS 与前后端交接记录已迁移到 [`../5_QT_UI_Demo/DEVELOPMENT_RECORD.md`](../5_QT_UI_Demo/DEVELOPMENT_RECORD.md)。当前命令行 PP-OCR 构建以本文前部为准，Qt 构建、部署和运行以 [`../5_QT_UI_Demo/README.md`](../5_QT_UI_Demo/README.md) 为准。

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
