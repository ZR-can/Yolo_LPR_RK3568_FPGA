
# 3_NPU_Yolov8_LPR_Demo用法

## 当前状态

当前主线实现的是 RK3568 端的视频解码、RGA 预处理、YOLOv8n 车牌定位、LPRNet 字符识别和双线程显示。
FPGA 侧的 PCIe 预处理链路仍在后续开发阶段，当前 README 不把这部分视为已完成能力。

## 开发记录

### 2026-07-12 车牌文字显示校验

- 视频路径的车牌投票校验已补全为逐位规则：普通牌为“省份 + 字母 + 字母/数字”，末位保留既有的“警、学、港、澳、领、使”特殊尾标；绿牌为“省份 + 字母 + 6 位字母/数字”，且第 3 位或末位必须为 `D/F`。因此 `津E黑S使云2` 等含非法中文字符的结果不会进入投票池。
- 跟踪目标未得到有效车牌投票时，RGA 仍绘制检测框，但跳过标签精灵，DRM UI plane 不再显示 `:`、`-` 或原始错误车牌文字。该逻辑尚待 Ubuntu 交叉编译和 RK3568 板端复测确认。

### 2026-07-10 绘制性能回退排查

- 用户反馈三层定位框、半透明圆角文字底板和加粗文字启用后帧率显著下降。
- 静态排查确认：该 overlay 在 DRM 映射帧缓冲上执行大量 CPU 读-改-写。半透明圆角底板逐像素做 alpha 混合，三层框增加描边写入；两段文字各执行三次栅格化，且每次均重新缩放字模并申请释放临时内存。
- 优先恢复单层不透明边框和单次文字栅格化，以现有 `Avg UI Drawing` 与 `System Throughput` 为基线验证；视觉样式需要保留时，再将叠加层改为预生成小型 RGBA overlay 后由 RGA 合成，避免 CPU 直接对 DRM 帧缓冲做 alpha 混合。
- GPU 可参与 UI 渲染，但首选架构是 MPP NV12 DMA-BUF 直接进入 DRM 视频 plane，Mali GPU 通过 GBM + EGL + GLES 渲染 ARGB UI plane，DRM atomic KMS 以 zpos 和 alpha 合成两个 plane。这样 CPU 仅更新目标几何和文本状态，避免 GPU 合成整帧视频。
- 该路线需先在板端确认 atomic plane 同时支持 NV12 和 ARGB8888，以及 `zpos`、`alpha`、fence 属性；MPP 的输出 buffer 必须在对应 DRM page-flip 完成前保持引用，不能在 decoder callback 返回并执行 `mpp_frame_deinit()` 后继续无保护地 scanout。
- 对当前少量车牌框与标签的 UI，RGA 是优先方案：复用已有 DMA-BUF fd 导入能力，以缓存的 ARGB 字形、圆角面板和边框位图作为源，由 RGA 完成填充与 alpha 合成。GPU 仅在后续需要动画、大量动态矢量对象或复杂特效时引入；无论采用 RGA 还是 GPU，最终都应由 DRM 双 plane 合成，避免每帧生成完整的 CPU 可见视频帧。
- 若后续扩展为左侧识别结果、上方状态栏、下方模式选择和右侧视频的交互式界面，渲染职责按 plane 划分：MPP/RGA 只提供视频 plane，Mali GPU 统一渲染整张透明 UI plane（包括视频上的检测框和标签），DRM 完成两层合成。不要让 RGA 与 GPU 分别写相邻或重叠的 UI 区域，以免增加额外 buffer、合成和 fence 同步复杂度。
- 已完成 RGA-only 双 plane 代码路径：视频 target 的 `main()` 位于 `src/main_video.cc`，MPP NV12 DMA-BUF 由 DRM atomic KMS 导入底层 video plane；两个不映射到 CPU 的 ABGR UI buffer 由 RGA 轮换写入上层 plane。
- 新增 `RgaOverlayRenderer`：每帧 RGA 清空透明 UI buffer 并绘制三层框；车牌、类型和置信度构成缓存键，缓存失效时才在普通内存生成小型直 alpha 圆角标签，随后通过位置化 RGA alpha blend 写入 UI DMA-BUF。CPU 不再访问 DRM 帧缓冲的像素。
- MPP decoder callback 现为显示持有额外 `MppBuffer` 引用，当前 scanout 帧在下一次 atomic commit 成功替换后才释放。`save_interval` 在 dual-plane 模式暂时禁用，避免导出不完整的单 plane 图像。
- RK3568 板端已完成一次视频复测：MPP 输出与显示均为 1600 帧、无显示丢帧和 overlay 失败，显示吞吐为 30.07 FPS，NPU 推理为 15.03 FPS。`Avg Decode Call` 为 33.16 ms，其中 32.21 ms 为按 30 FPS 的主动节流；MPP put/get 调用分别为 0.02/0.03 ms，未观察到解码背压。
- 后续连续运行复测出现 4.08 FPS：`MPP Input Retries` 与 `MPP Buffer Full` 同为 294215，平均重试等待 195.85 ms、连续重试峰值 894；MPP 输入/输出错误和 1 秒无进展 stall 均为零。同期 NPU 推理升至 145.72 ms，而 RGA/UI/DRM 仍约为 0.76/4.56/9.96 ms。该现象表明 VPU 在持续取得少量输出但长期受输入背压限制，优先排查残留进程、DDR/VPU/NPU DVFS、温度和内核日志，不应归因于 overlay 绘制。
- 已发现并修复 `MppDecoder::Init()` 的 context 所有权错误：局部变量曾遮蔽类成员 `mpp_ctx`，导致析构函数无法调用 `mpp_destroy()`，`Reset()` 也会向空 context 发命令。该缺口会使多次运行后的 MPP/VPU 资源回收不可靠，与“重启后恢复、连续运行退化”的现象直接相关。现在 context 由类成员持有，初始化失败路径和主函数初始化失败路径均会释放已分配资源。
- 修复后一次重启复测曾在输出 3 帧后遇到 `MPP_ERR_BUFFER_FULL` 持续约 1 秒，旧 watchdog 主动返回 `MPP_ERR_TIMEOUT` 并退出；当时 MPP 输入/输出错误均为零，说明是 watchdog 阈值而非 MPP 致命返回。现已改为 1 秒记录 `MPP Input Stalls` 告警、连续 5 秒无输出才记录 `MPP Input Aborts` 并退出，避免把短暂 VPU 启动背压误判为故障。
- 上述启动背压期间的板端状态为：CPU 51.25°C、CPU 1416 MHz、NPU 负载 3%、可用内存约 1399 MB；DMC 工具报告值为 1560（标签显示 GHz，实际单位需在板端确认）。未见 CPU 温度、NPU 负载或内存压力异常，后续应优先采集 `dmesg` 中 rkvdec/VPU/IOMMU/DMC 相关日志，并检查 raw H.264 parser 状态。
- `dmesg` 已确认启动背压的根因在板端电源管理路径：`mpp_rkvdec2 fdf80200.rkvdec: Cannot set voltage 875000 uV` 与 `rk3x-i2c fdd40000.i2c: timeout` 同时出现；随后 CPU 的 `_set_opp_voltage` 和 `cpufreq` 也以 `-110` 失败。VPU 与 CPU 均无法通过 PMIC I2C 切换 OPP 电压，导致 rkvdec 无法正常提升/切换性能状态，从而持续出现 MPP `BUFFER_FULL`。该问题不属于 demo 用户态代码，需要检查板级供电、PMIC/I2C 总线、内核 DTS regulator/OPP 配置及对应内核驱动。
- 未修改内核的临时运行适配：新增 `run_video_retry.sh`。它默认在首次启动前等待 15 秒，并在 demo 因 MPP 中止而退出后等待 3 秒再创建一次全新进程，最多执行 2 次。可通过 `MPP_STARTUP_DELAY_S`、`MPP_RETRY_DELAY_S` 和 `MPP_MAX_ATTEMPTS` 覆盖；它只用于规避冷启动的瞬时 PMIC/OPP 故障，不能修复 I2C 调压超时本身。
- 修复后重启板端首次复测为 30.03 FPS：MPP 输出/显示均为 1600 帧、无显示丢帧和 MPP 输入/输出错误。仅出现 6 次短暂 `MPP_ERR_BUFFER_FULL`，无实际退避等待、无 stall；NPU 36.50 ms、RGA/UI/DRM 为 0.60/4.63/8.69 ms，恢复到正常基线。仍需在不重启的前提下连续多次运行验证 context 回收是否彻底消除退化。
- 未重启条件下第二次连续复测为 30.09 FPS：10 次短暂 `MPP_ERR_BUFFER_FULL`、连续重试峰值 3、无退避等待/stall/MPP 错误；NPU 36.60 ms、RGA/UI/DRM 为 0.61/4.71/8.70 ms。首轮与第二轮无性能退化，表明 MPP context 与显示 buffer 回收已在连续运行间生效。
- 未重启条件下第三次连续复测为 30.08 FPS：13 次短暂 `MPP_ERR_BUFFER_FULL`、连续重试峰值 4、无退避等待/stall/MPP 错误；NPU 36.67 ms、RGA/UI/DRM 为 0.60/4.71/8.26 ms。输入重试计数在每个进程启动时清零，6/10/13 是各次独立运行的瞬时非阻塞队列满次数，并非跨运行累积；三次均保持 30 FPS，完成连续运行稳定性验证。
- 修复 Ubuntu GCC 6.3 构建兼容：图片 demo 不再访问已移除的 CPU 映射 framebuffer；`InferJob` 使用显式构造函数入队，避免旧编译器拒绝花括号初始化。等待 Ubuntu 重新编译。
- 修复 RGA overlay 链接冲突：`plate_font.h` 的字体位图改为仅由 `image_drawing.c` 定义，RGA 渲染模块仅引用该唯一实例，避免 `plate_font_data` 在两个 target 中重复定义。
- 根据板端 `2.43 FPS` 数据修复显示反压：原 `Avg MPP Decode` 计时覆盖整个同步 `decoder->Decode()` 调用，并非纯硬件解码；RGA UI 与 DRM atomic commit 曾直接在该回调中执行。
- 视频显示改为独立线程和容量为 2 的最新帧队列。解码回调只完成 NPU 预处理并移交持有引用的 MPP frame；队列满时丢弃旧帧并立即归还其 `MppBuffer`，避免显示阻塞 MPP 输出。
- DRM 现在按 MPP DMA-BUF fd 与帧布局缓存 GEM handle / framebuffer，消除每帧 PRIME import、`ADDFB2`、`RMFB` 与 `GEM_CLOSE`；分辨率或格式变化后的旧缓存仅在新 atomic commit 成功后清理。
- 默认关闭 MPP 每帧日志，性能统计改为分别报告 MPP 输出、实际送显、显示丢帧和 NPU 任务；尚待 Ubuntu 交叉编译与 RK3568 板端复测。
- 修复 RK3568 RGA overlay 的 `RGA_COLORFILL fail: Invalid argument`：旧 `imrectangle` 会把细边转换为如 `2x6` 的独立 color-fill 目标。现在以完整 UI DMA-BUF 为目标，通过四个裁剪 `imfill` 区域绘制框线，并显式使用 `wrapbuffer_fd_t` 传入完整宽高和 stride。
- 标签面板现按车牌框水平中心对齐，优先完整放置在框上方；上方空间不足时自动切换到框下方。面板坐标始终夹紧至 UI 边界，缓存生成时会按显示分辨率缩小字号，RGA blend 也保留最终裁剪保护，避免文字框越界或被裁切。
- 视频编译入口已迁回 `src/main_video.cc`；`src/main.cc` 保留为空文件且不参与 video target 编译，避免双 main 或额外跳转接口。
- 当前工作区中的 `build_and_push.sh` 已被删除；恢复该脚本前不能使用 README 中的一键构建推送流程。
- 视频性能摘要包含 MPP callback、put/get-frame、节流睡眠和 buffer-group 峰值统计；单个 RGA 框或标签操作失败只跳过该对象，避免逐帧错误刷屏拖慢解码。
- 当前本地版本使用 512 KiB 裸码流读取块，并显式将 MPP 输入和输出配置为非阻塞模式。`decode_put_packet()` 仅对 `MPP_ERR_BUFFER_FULL` 和输入超时重试；无输出进展时以 1 ms 退避，单个输入包持续 1 秒无进展仅记录背压告警，连续 5 秒无进展才停止解码并返回错误。EOS 包会等待最终输出帧，等待超过 1 秒同样作为错误返回。其他输入错误立即终止当前解码，主函数以非零状态退出，不对背压状态盲目 reset 解码器。
- 裸码流读取结束后始终单独提交零长度 EOS 包，不再依赖最后一次 `fread()` 是否触发 `feof()`；这保证文件大小恰好为读取块整数倍时也能完成 VPU drain 和 MPP 资源收尾。
- 性能摘要现记录输入重试次数、buffer-full/输入超时分类、实际退避等待、连续重试峰值、输入背压告警、输入中止和输入/输出侧错误数。这样可区分正常短暂背压、输出 buffer 未归还、VPU/DDR 降速及不可恢复的 MPP 错误。
- MPP 初始化现在在 `mpp_init()` 前显式启用 raw-stream split parser 和 parser fast mode，符合本地 MPP API 的时序要求；若板端仍存在高频输入重试，需要结合 DDR/VPU 频率、温度和 `dmesg` 排查硬件资源状态。

### 2026-07-07 双缓冲与绘制修复

- 本地工作区已从 GitHub `main` 回退后重新开始修复，并新建 `codex/fix-buffer-overlay` 分支；本地 `codex/pcie` 已删除，`git ls-remote --heads origin codex/pcie` 未发现远端同名 head。
- 修复视频异步推理调度：将 `g_infer` 从单槽 ready 状态改为任务队列，避免新帧覆盖尚未被推理线程消费的输入任务。
- 修复 `yolo_input_busy[]` 线程数据竞争：缓冲预约、失败释放、推理完成释放均集中到同一把互斥锁保护的 helper 中。
- 修复 `process_pipeline_preprocessed()` 与双缓冲潜在不一致：预处理 pipeline 现在显式接收当前 RKNN 输入 buffer 对应的 `image_buffer_t`，LPR 裁剪不再固定读取 `input_mems[0]`。
- 修复第二块 YOLO 输入缓冲释放顺序：备用 `rknn_tensor_mem` 在 `release_pipeline()` 销毁 RKNN context 之前释放，释放前先把 RKNN 输入重新绑定回主 buffer。
- 更新车牌 overlay：统一使用洋红色三层定位框、深黑灰半透明圆角文字底板、白色粗体固定字号单行文字，并限制到画面边界内。

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
│   ├── main.cc             #保留为空，不参与编译
│   ├── main_video.cc       #视频 demo 主函数
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

板端 PMIC/I2C 电源管理异常，默认开机后等待60s，规避部分冷启动 PMIC/OPP 瞬时异常，再运行demo

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
./yolov8_lpr_video_demo ./model/yolov8.rknn ./model/lprnet7repair_i8.rknn ./model/lprnet8repair_i8.rknn ./test/testvideo1.h264 0

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
