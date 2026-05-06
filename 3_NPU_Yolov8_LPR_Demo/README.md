
# 3_NPU_Yolov8_LPR_Demo用法

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
## PS：

1. 首先需要看懂main_video.cc的逻辑，别的需要关注的有mpp相关，其余不用管
2. CMakeLists：这部分需要改动的只有根目录下/，修改的方法为：  
   ./CMakeLists.txt：
   ```
    add_executable(${PROJECT_NAME}
    src/main_video2.cc #编译入口，根据不同的main用不同的文件名，例如src/main_video.cc ，src/main_video1.cc 
    ${DEMO_COMMON_SRCS}
    ······
    install(TARGETS ${PROJECT_NAME} DESTINATION .)
    file(GLOB RKNN_FILES "${CMAKE_CURRENT_SOURCE_DIR}/model/*.rknn")
    install(FILES ${RKNN_FILES} DESTINATION model)
    file(GLOB TXT_FILES "${CMAKE_CURRENT_SOURCE_DIR}/model/*.txt")
    install(FILES ${TXT_FILES} DESTINATION model)
    file(GLOB TEST_JPG_FILES "${CMAKE_CURRENT_SOURCE_DIR}/model/test*.jpg")
    install(FILES ${TEST_JPG_FILES} DESTINATION test)
    #安装文件用，如果写好了视频相关逻辑，需要将测试的视频文件写入install里装入demo中
    ```
    注意：整个涉及到CMakeLists.txt文件夹utils/、3rdparty/、src/,其构成不要随便加文件进去，否则需要重写CMakeLists，写入相关文件的链接，例如：  
    ```
    add_library(mpputils STATIC
    mpp_decoder.cpp
    mpp_encoder.cpp
    )

    target_include_directories(mpputils PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${LIBMPP_INCLUDES}
    )

    set(DEMO_COMMON_SRCS
    src/postprocess.cc
    src/yolov8_zero_copy.cc
    src/lprnet.cc
    )
    #自己加的.c,.cc,.h文件需要这样写入，让代码找到编译链接路径
    ```
3. main_video相关
   main_video1.cc和main_video.cc
   根据日志2的结果，目前RGA画框是4.6ms,但是使用cpu画文字只需要0.595ms,我没测算过cpu画框的性能，照画文字的表现，没准cpu反而更快，可能需要你们先测试一下main_video1.cc和main_video.cc里的画框谁更快一点，哪个快点用哪个，测试耗时的方式见main_video2.cc的逻辑
   确定谁更快后，你们修改主函数的时候在main_video1.cc的基础上copy出新文件再修改

4. 如果来的及的话可以试试修改lprnet.cc的代码，考虑将其也换成零拷贝的方式。然后测试一下其性能与耗时。
   零拷贝相关见开发资料02_Rockchip_RKNPU_User_Guide_RKNN_SDK_V2.3.0_CN.pdf
   零拷贝示例demo见yolov8_zero_copy.cc及rknn-toolkit2-2.3.0\rknpu2\examples\rknn_zero_copy
   如果不行的话，我后面再来试试
5. rknn-toolkit2-2.3.0和rknn-model_zoo-2.3.0均可在github上找到，用release里的2.3.0版的
6. 视频流参考代码可见rknn-toolkit2-2.3.0\rknpu2\examples\rknn_yolov5_demo，里面有mpp相关函数的使用
7. 对于目前视频流的实现，预想的造假方式是，用mpp解码读取视频文件（可以放letterbox resize到640*640后的视频），运行demo后，写一个读取显存写帧缓冲的代码，视频显示到接的显示屏上。
   至于解决目前推理速度慢的方法预想是，cpu双线程，一个采集 → 解码 → 显示：固定帧率，一个推理：放在另一个线程后台跑，不影响主链路。推理完后更新车牌位置和文字，推理没完成时则使用追踪的算法框选车牌。

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
*参数说明:*
- `<GCC_COMPILER_PATH>`: 指定交叉编译路径。不同的系统架构，所用的交叉编译工具并不相同。
    - `GCC_COMPILE_PATH` 示例:
        - aarch64: ~/tools/cross_compiler/arm/gcc-linaro-6.3.1-2017.05-x86_64_aarch64-linux-gnu/bin/aarch64-linux-gnu
        - armhf: ~/tools/cross_compiler/arm/gcc-arm-8.3-2019.03-x86_64-arm-linux-gnueabihf/bin/arm-linux-gnueabihf
        - armhf-uclibcgnueabihf(RV1103/RV1106): ~/tools/cross_compiler/arm/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf
- `<TARGET_PLATFORM>`: 指定目标平台。例如：`rk3588`。**注：每个模型当前支持的目标平台可能有所不同，请参考具体模型目录下的`README.md`文档。**
- `<ARCH>`: 指定系统架构。可以在目标设备执行如下命令查询系统架构: 
  ```shell
  # Query architecture. For Linux, ['aarch64' or 'armhf'] should shown in log.
  adb shell cat /proc/version
  ```
- `model_name`: 模型名，即examples目录下各个模型所在的文件夹名。

## Push demo files to device

```shell
#主终端使用
#删除原有旧demo
adb shell rm -rf /userdata/rknn_yolov8_lpr_demo
#推送
adb push install/rk356x_linux_aarch64/rknn_yolov8_lpr_demo /userdata/

adb push install/rk356x_linux_aarch64/rknn_yolov8_lpr_demo/yolov8_lpr_picture_demo /userdata/rknn_yolov8_lpr_demo
adb push install/rk356x_linux_aarch64/rknn_yolov8_lpr_demo/yolov8_lpr_video_demo /userdata/rknn_yolov8_lpr_demo

adb push model/testXX.jpg /userdata/rknn_yolov8_lpr_demo/yolov8_lpr_picture_demo/test
adb push model/testvideoXX.h264 /userdata/rknn_yolov8_lpr_demo/yolov8_lpr_video_demo/test
```

## Run demo and pull result

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
./yolov8_lpr_video_demo ./model/yolov8.rknn ./model/lprnet7repair_i8.rknn ./model/lprnet8repair_i8.rknn ./test/testvideo1.h264 200

#退出板端终端命令为logout

#主终端使用
#拉取结果
adb pull /userdata/rknn_yolov8_lpr_demo/result ./
adb pull /userdata/rknn_yolov8_lpr_demo/result ./ ; adb pull /userdata/rknn_yolov8_lpr_demo/testvideo1.h264 ./
```

```shell
#性能耗时评估

#从终端使用
export RKNN_LOG_LEVEL=4
export RKNN_LOG_LEVEL=0
#以下命令使用不同的main记得更改生成的.log日志文件名，不同的日志结果分开存放并注明测试的内容
#使用test1.jpg存在一张图多车牌，使用test2.jpg为单图绿牌，根据要测试的内容选择不同的test.jpg
cd /userdata/rknn_yolov8_lpr_demo/yolov8_lpr_picture_demo
./yolov8_lpr_picture_demo ./model/yolov8.rknn ./model/lprnet7repair_i8.rknn ./model/lprnet8repair_i8.rknn ./test/test2.jpg > rknn_perf2repair_i8.log 2>&1
#主终端使用
adb pull /userdata/rknn_yolov8_lpr_demo/yolov8_lpr_picture_demo/result ./ ; adb pull /userdata/rknn_yolov8_lpr_demo/yolov8_lpr_picture_demo/rknn_perf2repair_i8.log ./
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

