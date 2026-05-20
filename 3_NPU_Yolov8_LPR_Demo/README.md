
# 3_NPU_Yolov8_LPR_Demo用法

## 当前状态

当前主线实现的是 RK3568 端的视频解码、RGA 预处理、YOLOv8n 车牌定位、LPRNet 字符识别和双线程显示。
FPGA 侧的 PCIe 预处理链路仍在后续开发阶段，当前 README 不把这部分视为已完成能力。

## 文件构成介绍

```
├── 3rdparty\        #第三方库，只需要看看mpp，别的已经改好
│   ├── allocator
│   ├── CMakeLists.txt      #第三方库CMakeLists.txt这个目前已经修改好不用动
│   ├── fftw
│   ├── jpeg_turbo
│   ├── kaldi_native_fbank
│   ├── librga
│   ├── libsndfile
│   ├── mpp #mpp视频硬件支持，视频流可能需要用到
│   ├── opencl
│   ├── opencv
│   ├── rknpu1
│   ├── stb_image
│   ├── rknpu2
│   ├── timer
│   └── zlmediakit
├── adb\    #adb Windows支持工具,不同于Linux上使用adb,Windows上使用adb需要在adb前加上.\,例如.\adb shell,.\adb devices
│   ├── adb.exe
│   ├── AdbWinApi.dll
│   └── AdbWinUsbApi.dll
├── build\      #交叉编译build文件
│   └── build_rknn_yolov8_lpr_demo_rk356x_linux_aarch64_Release
├── include\        #工程头文件
│   ├── drm_func.h
│   ├── lprnet.h
│   ├── postprocess.h
│   └── yolov8.h
├── install\    #交叉编译后push到板端使用的demo
│   └── rk356x_linux_aarch64
├── model\      #model相关
│   ├── labels_list.txt
│   ├── lprnet7.rknn
│   ├── lprnet8.rknn
│   ├── test1.jpg
│   ├── test2.jpg
│   ├── test3.jpg
│   ├── test4.jpg
│   └── yolov8.rknn
├── result\     #从板端pull回PC的运行result
│   ├── test1_out.png
│   ├── test2_out.png
│   ├── test3_out.png
│   └── test4_out.png
├── src\        #工程源代码
│   ├── lprnet.cc
│   ├── main.cc
│   ├── main_video.cc       #初版图片推理
│   ├── main_video1.cc      #初版基础上将CPU画矩形框改为用RGA画矩形框
│   ├── main_video2.cc      #1版基础上加上了耗时与性能分析
│   ├── postprocess.cc
│   └── yolov8_zero_copy.cc
├── utils\      #封装的常用函数包,只需关注mpp相关
│   ├── audio_utils.c
│   ├── audio_utils.h
│   ├── CMakeLists.txt      #CMakeLists.txt,目前的处理图片的demo所需的相关代码已经写入其中
│   ├── common.h
│   ├── file_utils.c
│   ├── file_utils.h
│   ├── font.h
│   ├── image_drawing.c     #图片后处理，有画矩形框框选车牌函数draw_rectangle与画文字函数draw_text
│   ├── image_drawing.h
│   ├── image_utils.c
│   ├── image_utils.h
│   ├── mpp_decoder.cpp
│   ├── mpp_decoder.h
│   ├── mpp_encoder.cpp
│   ├── mpp_encoder.h
│   ├── plate_font.h
│   └── rga_fill_rectangle_task_array_demo.cpp
├── build-linux.sh      #交叉编译命令脚本
├── CMakeLists.txt      #根文件CMakeLists.txt,在其中引入前两份CMakeLists.txt的内容,整个工程链接用,重要！！
├── generate_c_font_array.py        #生成字符点阵字符用
├── README.md
├── rknn_perf.log
├── rknn_perf2.log      #两份demo运行日志,分析运行结果用,从板端pull回,重要！！
└── 上板结果.txt
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

如果希望一键完成“清理旧 build、重新编译、推送到板端并恢复可执行权限”，可直接使用：

```shell
cd /mnt/hgfs/Yolo_LPR_RK3568_FPGA_Project/3_NPU_Yolov8_LPR_Demo
./build_and_push.sh
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

adb push model/testX.jpg /userdata/rknn_yolov8_lpr_demo/yolov8_lpr_picture_demo/test
adb push model/testvideoX.h264 /userdata/rknn_yolov8_lpr_demo/yolov8_lpr_video_demo/test
```

## Run demo and pull result

```shell
# 切到命令行模式，立刻关闭3568桌面
sudo systemctl isolate multi-user.target
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
# export LD_LIBRARY_PATH=./lib

#推理单图片
cd /userdata/rknn_yolov8_lpr_demo/yolov8_lpr_picture_demo
./yolov8_lpr_picture_demo ./model/yolov8.rknn ./model/lprnet7repair_fp.rknn ./model/lprnet8repair_fp.rknn ./test/test1.jpg
for img in ./test/test{1..4}.jpg; do ./yolov8_lpr_picture_demo ./model/yolov8.rknn ./model/lprnet7repair_fp.rknn ./model/lprnet8repair_fp.rknn $img; done

#推理视频
cd /userdata/rknn_yolov8_lpr_demo/yolov8_lpr_video_demo
./yolov8_lpr_video_demo ./model/yolov8.rknn ./model/lprnet7repair_i8.rknn ./model/lprnet8repair_i8.rknn ./test/testvideo.h264 0

#退出板端终端命令为logout

#主终端使用
#拉取结果
adb pull /userdata/rknn_yolov8_lpr_demo/yolov8_lpr_picture_demo/result ./result/picture
adb pull /userdata/rknn_yolov8_lpr_demo/yolov8_lpr_video_demo/result ./result/video
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
