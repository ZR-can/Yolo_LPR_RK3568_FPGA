# RK3568 PCIe LPR Qt UI 编译与运行记录

本文记录本项目 Qt UI 从虚拟机配置、交叉编译、ADB 推送到 RK3568 板端运行的完整流程。

适用目录：

```text
D:\a_fpga\codex\5_12_HDMI_IN_DDR3_HDMI_OUT\else\Yolo_LPR_RK3568_FPGA\3_NPU_Yolov8_PPOCR_Demo\qt_pcie_lpr_ui
```

## 1. 虚拟机 Qt 交叉编译环境配置

### 1.1 VMware 共享目录

Windows 工程目录：

```text
D:\a_fpga\codex\5_12_HDMI_IN_DDR3_HDMI_OUT\else\Yolo_LPR_RK3568_FPGA\3_NPU_Yolov8_PPOCR_Demo
```

在 VMware 中设置共享文件夹后，Ubuntu 内常见路径为：

```bash
/mnt/hgfs/3_NPU_Yolov8_PPOCR_Demo
```

检查：

```bash
ls /mnt/hgfs
cd /mnt/hgfs/3_NPU_Yolov8_PPOCR_Demo
ls
```

如果 `/mnt/hgfs` 为空，先在 VMware 设置里启用 Shared Folders，并在 Ubuntu 中执行：

```bash
sudo mkdir -p /mnt/hgfs
sudo vmhgfs-fuse .host:/ /mnt/hgfs -o allow_other
```

### 1.2 虚拟机联网

VMware 网络建议使用 NAT。

Ubuntu 中检查网卡：

```bash
ip link
```

本次网卡名为：

```text
ens33
```

如果状态是 `DOWN`，执行：

```bash
sudo ip link set ens33 up
sudo dhclient -v ens33
```

检查网络：

```bash
ping -c 4 8.8.8.8
ping -c 4 baidu.com
```

### 1.3 安装系统交叉编译器

本次 Qt 预编译包要求使用 Ubuntu 20.04 自带的 aarch64 交叉编译器，不要使用旧 Linaro GCC 6.3。

安装：

```bash
sudo apt update
sudo apt install -y build-essential cmake git perl python3 pkg-config \
  gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
```

检查：

```bash
/usr/bin/aarch64-linux-gnu-gcc --version
/usr/bin/aarch64-linux-gnu-g++ --version
cmake --version
```

本次踩坑记录：

如果 CMake 显示编译器为：

```text
GNU 6.3.1
```

并在链接时报：

```text
undefined reference to `renameat2@GLIBC_2.28'
undefined reference to `statx@GLIBC_2.28'
undefined reference to `log@GLIBC_2.29'
```

说明使用了旧 Linaro GCC 6.3。需要改用：

```text
/usr/bin/aarch64-linux-gnu-gcc
/usr/bin/aarch64-linux-gnu-g++
```

### 1.4 配置 Qt 5.12.9 ARM64

本次已经下载好的文件位于：

```text
qt_pcie_lpr_ui\qmake.tar
qt_pcie_lpr_ui\qt-everywhere-src-5.12.9.tar.xz
```

优先使用 `qmake.tar`，它已经包含编译好的：

```text
Qt-5.12.9-arm64
```

不需要重新编译 Qt 源码。

在 Ubuntu 中执行：

```bash
cd /mnt/hgfs/3_NPU_Yolov8_PPOCR_Demo/qt_pcie_lpr_ui
cp qmake.tar ~/
cd ~
tar -xf qmake.tar
```

检查：

```bash
ls ~/Qt-5.12.9-arm64/bin
~/Qt-5.12.9-arm64/bin/qmake -v
~/Qt-5.12.9-arm64/bin/uic -v
```

应能看到 `qmake`、`uic`、`moc`、`rcc` 等工具。

### 1.5 编译 Qt UI

进入 Qt UI 目录：

```bash
cd /mnt/hgfs/3_NPU_Yolov8_PPOCR_Demo/qt_pcie_lpr_ui
```

处理脚本权限和 Windows 换行：

```bash
sed -i 's/\r$//' build-qt-linux.sh
chmod +x build-qt-linux.sh
```

用系统 GCC 交叉编译：

```bash
GCC_COMPILER=/usr/bin/aarch64-linux-gnu \
QT_ARM64_PREFIX=/home/gyn/Qt-5.12.9-arm64 \
./build-qt-linux.sh
```

成功后会出现：

```text
Installed to: /mnt/hgfs/3_NPU_Yolov8_PPOCR_Demo/qt_pcie_lpr_ui/install/rk356x_linux_aarch64/yolov8_lpr_pcie_qt_ui
```

输出目录结构：

```text
qt_pcie_lpr_ui/install/rk356x_linux_aarch64/
├── lib/
│   ├── libmk_api.so
│   ├── librga.so
│   ├── librknnrt.so
│   └── librockchip_mpp.so
└── yolov8_lpr_pcie_qt_ui/
    ├── yolov8_lpr_pcie_qt_ui
    ├── pango_pci_driver.ko
    └── model/
        ├── yolov8.rknn
        ├── lprnet7repair_i8.rknn
        ├── lprnet8repair_i8.rknn
        └── ...
```

## 2. RK3568 板端文件位置与说明

板端最终部署目录：

```text
/userdata/yolov8_lpr_pcie_qt_ui
```

目录结构：

```text
/userdata/yolov8_lpr_pcie_qt_ui/
├── lib/
│   ├── libmk_api.so
│   ├── librga.so
│   ├── librknnrt.so
│   └── librockchip_mpp.so
└── yolov8_lpr_pcie_qt_ui/
    ├── yolov8_lpr_pcie_qt_ui
    ├── pango_pci_driver.ko
    └── model/
        ├── yolov8.rknn
        ├── lprnet7repair_i8.rknn
        ├── lprnet8repair_i8.rknn
        ├── lprnet7repair_fp.rknn
        ├── lprnet8repair_fp.rknn
        └── labels_list.txt
```

各文件作用：

- `yolov8_lpr_pcie_qt_ui`：Qt UI 主程序。
- `pango_pci_driver.ko`：Pango PCIe 驱动模块。
- `model/yolov8.rknn`：车牌检测模型。
- `model/lprnet7repair_i8.rknn`：普通车牌识别模型。
- `model/lprnet8repair_i8.rknn`：新能源车牌识别模型。
- `lib/librknnrt.so`：RKNN runtime。
- `lib/librga.so`：RGA 库。
- `lib/librockchip_mpp.so`、`libmk_api.so`：项目继承安装的依赖库。

本 Qt UI 的画框逻辑：

- Qt 不负责画检测框。
- 已复用现有 C/C++ 代码 `draw_pipeline_result_overlay()`。
- Qt 只把已经画好框和车牌文字的图像显示到界面上，并显示右侧状态文字。

## 3. Windows ADB 推送与板端运行流程

本机虚拟机内 ADB 可能无法直连 RK3568，因此本次使用 Windows PowerShell 的 ADB。

ADB 路径：

```text
D:\adb\bin\adb.exe
```

### 3.1 Windows 端检查 ADB

打开 PowerShell：

```powershell
cd D:\adb\bin
.\adb.exe devices
```

应能看到 RK3568 设备。

### 3.2 Windows 端推送文件

Windows 编译输出路径：

```text
D:\a_fpga\codex\5_12_HDMI_IN_DDR3_HDMI_OUT\else\Yolo_LPR_RK3568_FPGA\3_NPU_Yolov8_PPOCR_Demo\qt_pcie_lpr_ui\install\rk356x_linux_aarch64
```

删除旧文件并创建目录：

```powershell
.\adb.exe shell rm -rf /userdata/yolov8_lpr_pcie_qt_ui
.\adb.exe shell mkdir -p /userdata/yolov8_lpr_pcie_qt_ui
```

推送程序目录：

```powershell
.\adb.exe push "D:\a_fpga\codex\5_12_HDMI_IN_DDR3_HDMI_OUT\else\Yolo_LPR_RK3568_FPGA\3_NPU_Yolov8_PPOCR_Demo\qt_pcie_lpr_ui\install\rk356x_linux_aarch64\yolov8_lpr_pcie_qt_ui" /userdata/yolov8_lpr_pcie_qt_ui/
```

推送库目录：

```powershell
.\adb.exe push "D:\a_fpga\codex\5_12_HDMI_IN_DDR3_HDMI_OUT\else\Yolo_LPR_RK3568_FPGA\3_NPU_Yolov8_PPOCR_Demo\qt_pcie_lpr_ui\install\rk356x_linux_aarch64\lib" /userdata/yolov8_lpr_pcie_qt_ui/
```

### 3.3 进入板端 shell

```powershell
.\adb.exe shell
```

进入后应看到类似：

```text
root@linaro-alip:/#
```

### 3.4 启动图形桌面 X11

本板没有 `/dev/fb0`：

```sh
ls -l /dev/fb*
cat /proc/fb
```

均不存在，所以 `QT_QPA_PLATFORM=linuxfb` 不可用。

本次最终跑通方式是使用 X11/xcb。先启动图形目标：

```sh
systemctl isolate graphical.target
```

检查 X11：

```sh
ls -l /tmp/.X11-unix
ps -ef | grep -Ei 'Xorg|X11|lightdm|sddm|gdm|openbox|lxde' | grep -v grep
```

成功时可见：

```text
/tmp/.X11-unix/X0
/usr/lib/xorg/Xorg :0 ... -auth /var/run/lightdm/root/:0
```

注意本板 Xorg 授权文件是：

```text
/var/run/lightdm/root/:0
```

不是：

```text
/home/linaro/.Xauthority
```

### 3.5 加载 PCIe 驱动

```sh
cd /userdata/yolov8_lpr_pcie_qt_ui/yolov8_lpr_pcie_qt_ui

if lsmod | grep -q '^pango_pci_driver '; then
    rmmod pango_pci_driver
fi

insmod ./pango_pci_driver.ko
ls -l /dev/pango_pci_driver
```

如果 `/dev/pango_pci_driver` 存在，说明驱动节点已创建。

### 3.6 设置运行环境

```sh
cd /userdata/yolov8_lpr_pcie_qt_ui/yolov8_lpr_pcie_qt_ui

export DISPLAY=:0
export XAUTHORITY=/var/run/lightdm/root/:0
export QT_QPA_PLATFORM=xcb
export LD_LIBRARY_PATH=/userdata/yolov8_lpr_pcie_qt_ui/lib:$LD_LIBRARY_PATH
```

不要使用：

```sh
export QT_QPA_PLATFORM=linuxfb
```

因为板端没有 `/dev/fb0`。

也不优先使用：

```sh
export QT_QPA_PLATFORM=eglfs
```

本次 `eglfs` 曾报：

```text
EGL Error : Could not create the egl surface: error = 0x3009
```

并且画面没有显示。

### 3.7 运行 Qt UI

```sh
./yolov8_lpr_pcie_qt_ui \
  ./model/yolov8.rknn \
  ./model/lprnet7repair_i8.rknn \
  ./model/lprnet8repair_i8.rknn
```

如果界面出现，点击右侧 `Start` 按钮开始 PCIe 采集和识别。

### 3.8 常见错误与处理

#### A. `could not connect to display :0`

现象：

```text
qt.qpa.xcb: could not connect to display :0
```

处理：

1. 确认 X0 存在：

```sh
ls -l /tmp/.X11-unix
```

2. 如果没有 `X0`，启动图形桌面：

```sh
systemctl isolate graphical.target
```

3. 设置正确 XAUTHORITY：

```sh
export DISPLAY=:0
export XAUTHORITY=/var/run/lightdm/root/:0
export QT_QPA_PLATFORM=xcb
```

#### B. `linuxfb: Failed to initialize screen`

现象：

```text
Unable to figure out framebuffer device. Specify it manually.
linuxfb: Failed to initialize screen
```

原因：

板子没有 `/dev/fb0` 和 `/proc/fb`，不能走 `linuxfb`。

处理：

改用：

```sh
export QT_QPA_PLATFORM=xcb
```

#### C. `eglfs` 没画面

现象：

```text
EGL Error : Could not create the egl surface: error = 0x3009
```

或只打印：

```text
Failed to move cursor on screen HDMI1: -14
```

处理：

本项目当前不继续走 `eglfs_kms`，改用 X11/xcb。

#### D. `No such file or directory`

如果运行时报：

```text
bash: ./yolov8_lpr_pcie_qt_ui: No such file or directory
```

先确认当前目录：

```sh
pwd
ls -l
```

必须在：

```text
/userdata/yolov8_lpr_pcie_qt_ui/yolov8_lpr_pcie_qt_ui
```

再运行：

```sh
./yolov8_lpr_pcie_qt_ui ./model/yolov8.rknn ./model/lprnet7repair_i8.rknn ./model/lprnet8repair_i8.rknn
```

## 4. 当前推荐的一键式板端运行命令

每次板端启动后，可按以下顺序运行：

```sh
systemctl isolate graphical.target

cd /userdata/yolov8_lpr_pcie_qt_ui/yolov8_lpr_pcie_qt_ui

if lsmod | grep -q '^pango_pci_driver '; then
    rmmod pango_pci_driver
fi
insmod ./pango_pci_driver.ko
ls -l /dev/pango_pci_driver

export DISPLAY=:0
export XAUTHORITY=/var/run/lightdm/root/:0
export QT_QPA_PLATFORM=xcb
export LD_LIBRARY_PATH=/userdata/yolov8_lpr_pcie_qt_ui/lib:$LD_LIBRARY_PATH

./yolov8_lpr_pcie_qt_ui \
  ./model/yolov8.rknn \
  ./model/lprnet7repair_i8.rknn \
  ./model/lprnet8repair_i8.rknn
```

## 5. 当前状态结论

- 虚拟机 Qt UI 已成功交叉编译。
- 成功编译时使用系统 `/usr/bin/aarch64-linux-gnu` 工具链。
- 旧 Linaro GCC 6.3 会因 glibc 版本过低导致 Qt 链接失败。
- 板端无 framebuffer，`linuxfb` 不可用。
- `eglfs_kms` 能启动但无有效画面，本轮不作为主路径。
- 板端最终应使用 `graphical.target + xcb + /var/run/lightdm/root/:0` 运行 Qt UI。
