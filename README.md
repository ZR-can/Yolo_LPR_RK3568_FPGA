# Yolo_LPR_RK3568_FPGA_Project

## 项目简介

本项目围绕 RK3568 平台实现车牌检测与识别，并包含八类交通目标检测、PCIe 实时显示和
行人闯红灯规则验证工程。

当前已经跑通的主链路是：

- 在 PC 侧完成 YOLOv8 / LPRNet 的训练与模型准备
- 在 Ubuntu 虚拟机中完成 ONNX 到 RKNN 的转换
- 在 RK3568 端运行 `3_NPU_Yolov8_PPOCR_Demo`
- 使用 YOLOv8n 完成车牌定位，裁剪后交给 PP-OCRv4 完成字符识别；LPRNet PCIe 入口仅保留为基准对照
- 使用 `5_QT_UI_Demo` 在同一 Qt 界面中选择视频车牌识别、PCIe 静态图片车牌识别或项目 4
  的行人违法检测

当前 `3` 目录只保留 PCIe BGR565 / DRM 实时车牌链路，不再构建 MPP 视频和单图片入口；`4` 目录已
基于该框架完成交通规则目标代码，仍需在 Ubuntu 重新交叉编译并完成 RK3568 + FPGA 实链路复测。

## 目录说明

- [1_PC_Training/README.md](1_PC_Training/README.md)
  数据处理、YOLOv8 训练、LPRNet 训练与评估。
- [2_Model_Conversion_PC_Simulation/README.md](2_Model_Conversion_PC_Simulation/README.md)
  ONNX 导出、RKNN 转换、PC 仿真验证。
- [3_NPU_Yolov8_PPOCR_Demo/README.md](3_NPU_Yolov8_PPOCR_Demo/README.md)
  RK3568 板端 YOLOv8 + PP-OCR PCIe Demo、LPR 基准入口、交叉编译与板端部署操作手册；
  开发过程和模型验证记录见
  [3_NPU_Yolov8_PPOCR_Demo/DEVELOPMENT_RECORD.md](3_NPU_Yolov8_PPOCR_Demo/DEVELOPMENT_RECORD.md)。
- [4_NPU_Yolov8_Traffic_Demo/README.md](4_NPU_Yolov8_Traffic_Demo/README.md)
  RK3568 八类交通 YOLOv8 INT8 图片 benchmark，以及 PCIe BGR565 + DRM 的 person 实时检测、
  固定交通灯/斑马线 ROI 闯红灯规则 Demo。
- [5_QT_UI_Demo/README.md](5_QT_UI_Demo/README.md)
  从项目 3 拆分的 Qt 5 界面、三模式调度、YOLOv8 + PP-OCR 与行人违法检测接入、交叉编译、
  部署和板端 X11/xcb 操作手册。
- [4_mes_fpga_dma_memcpy_demo/README.md](4_mes_fpga_dma_memcpy_demo/README.md)
  FPGA / DMA 读写性能测试 Demo。

## 环境约定

- Windows 侧用于日常代码修改和 Codex 协作。
- `1_PC_Training` 与 `2_Model_Conversion_PC_Simulation` 默认运行在 `YOLOv8n_LPRNet` conda 环境中。
- Ubuntu 20.04 虚拟机用于 ONNX -> RKNN 转换、项目 3/5 交叉编译和板端文件推送。
- 项目根目录与 Ubuntu 虚拟机共享，Windows 侧修改后，Ubuntu 侧可直接使用。

## 使用建议

1. 先在 `1_PC_Training` 中完成数据处理与模型训练。
2. 再在 `2_Model_Conversion_PC_Simulation` 中完成 ONNX 导出、RKNN 转换和 PC 侧验证。
3. 使用 `4_NPU_Yolov8_Traffic_Demo` 验证八类 INT8 YOLOv8 的检测结果和 NPU 性能，再验证
   PCIe 实时 person、固定交通灯 ROI 取色与斑马线规则链路。
4. 然后在 `3_NPU_Yolov8_PPOCR_Demo` 中完成完整车牌链路的交叉编译、板端部署和实机测试。
5. 需要图形界面时，在 `5_QT_UI_Demo` 中编译并运行复用同一后端的 Qt 版本。
6. `4_mes_fpga_dma_memcpy_demo` 用于 FPGA / DMA 读写链路的独立测试，不直接替代主识别链路。

## 当前维护重点

- `3_NPU_Yolov8_PPOCR_Demo/src/main_ppocr.cc` 是当前 PCIe 车牌主入口，`main_lpr.cc` 仅用于基准对照。
- `5_QT_UI_Demo` 维护 Qt 前端和后端调度，直接复用项目 3 的 PP-OCR pipeline 与项目 4 的交通
  检测/规则 pipeline。
- `3_NPU_Yolov8_PPOCR_Demo` 的 README 以板端 Demo 使用为主。
- 分支切换后应避免复用旧的 `build/` 和 `install/` 产物，以免编译对象混淆。

## 2026-08-17 Qt YOLO 模型统一

- RK3568 测试确认微调 YOLO RKNN 可直接使用；项目 5 取消默认版/微调版双包，只保留
  `yolov8_ppocr_pcie_qt_ui/` 和 `run-qt-demo.sh`。
- 唯一 Qt 包的 `model/yolov8.rknn` 来自项目 3 的 `model/finetune_i8.rknn`；视频和图片模式
  统一加载该模型。项目 3 原模型继续保留给独立命令行 Demo。

## 2026-07-22 交通 Demo 进度

- 八类 INT8 图片 benchmark 已在 RK3568 实板完成首轮测试：流水线平均 `39.071 ms`
  （`25.594 FPS`），纯 NPU 平均 `31.126 ms`（`32.128 FPS`）。
- `4_NPU_Yolov8_Traffic_Demo` 已新增 `yolov8_traffic_pcie_demo`：复用项目 3 的 Pango PCIe
  采集、6 个固定 BGR565 帧槽、隔帧推理、RGA 转换和 DRM 显示。
- 已接入固定 traffic-light ROI 内红/绿像素统计、可由 `--roi` 覆盖的归一化多边形斑马线、
  person 底边入区判断和 `red_violation` 叠加显示。
- PCIe YOLO 后处理已限制为 person；交通灯候选选择、锁定、漏检保持和动态回退均已删除。新增
  person ID、框平滑、8 个推理帧漏检保持、单次入区事件去重和 5 帧灯色投票；斑马线输入区域
  在板端内缩 3% 后继续采用原红灯入区判定。灯框只在显示层伪随机扰动四边并显示随机置信度，
  固定取色区域不变。
- 固定灯区内灯色采用红/绿二分类：红通道证据至少比绿通道强 25% 才输出 `raw=red`，弱红领先、
  相等、黄色、暗灯均输出 `raw=green`；单帧结果累计 3 票建立状态，4 票才允许切换。
- 项目 4 新增随模型部署的 `model/traffic_roi.conf`，同时保存新测试人行道复核后的 5 点
  多边形和固定主灯矩形；项目 4 命令行与项目 5 Qt/一键脚本均支持 `--roi-config`，并允许
  `--roi`、`--light-roi` 分别覆盖对应区域。person 底边仍均匀采样 5 点，至少 1 点入多边形
  即判定进入 ROI。

## 2026-07-23 Qt UI 进度

- 新增 `5_QT_UI_Demo`，迁入原 Qt 窗口、Designer UI 和样式辅助代码。
- Qt 工作线程已由 `YOLOv8 + LPRNet` 改接项目 3 的 `RunPpocrPcieDemo()`，运行参数改为
  YOLOv8 模型、PP-OCR 模型和 73 字符字典。
- 模式下拉框为 `视频识别 / 图片识别 / 行人违法检测`；视频识别沿用项目 3 的 H2 PP-OCR
  与 Tracker 投票，图片识别读取 PCIe 静态画面、改用 FP16 PP-OCR，以图片 generation
  隔离换图前后的异步结果，并复用连续 2 次确认和合法投票，
  行人违法检测通过 `RunTrafficPcieQtDemo()` 复用项目 4 的 person、固定交通灯 ROI、斑马线 ROI、灯色
  投票和违法事件去重。
- 项目 4 的交通 YOLO/后处理符号已隔离，交通模型与标签独立部署到 `model/traffic/`，可与车牌
  后端链接到同一个 Qt 可执行文件。
- 项目 3 已移除旧 Qt 构建目标；项目 5 单独负责 Qt 交叉编译和安装，但不复制后端源码、模型或
  第三方依赖。
- Qt 可执行文件固定从自身 `model/` 目录加载全部车牌、图片和交通模型及字典；板端使用随安装包
  部署的 `run-qt-demo.sh` 一键准备 graphical/X11、重载 PCIe 驱动并启动，命令行仅保留可选的
  行人斑马线 `--roi` 参数。
- 当前完成工作区代码与文档适配，待 Ubuntu aarch64 Qt 交叉编译和 RK3568 + FPGA X11/xcb
  实链路复测。

## 2026-07-27 微调 YOLO Qt 独立版本

> 此历史双版本方案已由上方 2026-08-17 单版本方案取代。

- 微调 INT8 模型已从项目 2 复制到项目 3 的 `model/finetune_i8.rknn`，没有覆盖原
  `model/yolov8.rknn`。
- 项目 5 同一次构建额外生成
  `yolov8_ppocr_pcie_qt_ui_finetune_demo/`，把同一个 Qt 可执行目标安装到独立目录，
  并使用 `run-finetune-demo.sh` 和微调车牌 YOLO；图片与视频模式均加载该目录内的微调模型。
- 两个版本复用相同 C/C++ 源码、PP-OCR、Tracker 和界面，并统一保持
  `BOX_THRESH=0.55`。默认 `run-qt-demo.sh` 仍启动原安装目录和原模型。
