# Yolo_LPR_RK3568_FPGA_Project

## 项目简介

本项目围绕 RK3568 平台实现车牌检测与识别，并包含八类交通目标检测、PCIe 实时显示和
行人闯红灯规则验证工程。

当前已经跑通的主链路是：

- 在 PC 侧完成 YOLOv8 / LPRNet 的训练与模型准备
- 在 Ubuntu 虚拟机中完成 ONNX 到 RKNN 的转换
- 在 RK3568 端运行 `3_NPU_Yolov8_LPR_Demo`
- 使用 YOLOv8n 完成车牌定位，裁剪后交给 LPRNet 完成字符识别

当前 `3` 目录保留本地视频车牌链路，并提供可复用的 PCIe BGR565 / DRM 实时框架；`4` 目录已
基于该框架完成交通规则目标代码，仍需在 Ubuntu 重新交叉编译并完成 RK3568 + FPGA 实链路复测。

## 目录说明

- [1_PC_Training/README.md](1_PC_Training/README.md)
  数据处理、YOLOv8 训练、LPRNet 训练与评估。
- [2_Model_Conversion_PC_Simulation/README.md](2_Model_Conversion_PC_Simulation/README.md)
  ONNX 导出、RKNN 转换、PC 仿真验证。
- [3_NPU_Yolov8_LPR_Demo/README.md](3_NPU_Yolov8_LPR_Demo/README.md)
  RK3568 板端 C/C++ Demo、交叉编译、模型打包与板端部署说明。
- [4_NPU_Yolov8_Traffic_Demo/README.md](4_NPU_Yolov8_Traffic_Demo/README.md)
  RK3568 八类交通 YOLOv8 INT8 图片 benchmark，以及 PCIe BGR565 + DRM 的 person / traffic light
  实时检测和人工斑马线 ROI 闯红灯规则 Demo。
- [4_mes_fpga_dma_memcpy_demo/README.md](4_mes_fpga_dma_memcpy_demo/README.md)
  FPGA / DMA 读写性能测试 Demo。

## 环境约定

- Windows 侧用于日常代码修改和 Codex 协作。
- `1_PC_Training` 与 `2_Model_Conversion_PC_Simulation` 默认运行在 `YOLOv8n_LPRNet` conda 环境中。
- Ubuntu 20.04 虚拟机用于 ONNX -> RKNN 转换、`3_NPU_Yolov8_LPR_Demo` 交叉编译和板端文件推送。
- 项目根目录与 Ubuntu 虚拟机共享，Windows 侧修改后，Ubuntu 侧可直接使用。

## 使用建议

1. 先在 `1_PC_Training` 中完成数据处理与模型训练。
2. 再在 `2_Model_Conversion_PC_Simulation` 中完成 ONNX 导出、RKNN 转换和 PC 侧验证。
3. 使用 `4_NPU_Yolov8_Traffic_Demo` 验证八类 INT8 YOLOv8 的检测结果和 NPU 性能，再验证
   PCIe 实时 person / traffic light 与斑马线规则链路。
4. 然后在 `3_NPU_Yolov8_LPR_Demo` 中完成完整车牌链路的交叉编译、板端部署和实机测试。
5. `4_mes_fpga_dma_memcpy_demo` 用于 FPGA / DMA 读写链路的独立测试，不直接替代主识别链路。

## 当前维护重点

- `main_video.cc` 与 `main_picture.cc` 都属于后续持续修改和测试的重点入口文件。
- `3_NPU_Yolov8_LPR_Demo` 的 README 以板端 Demo 使用为主。
- 分支切换后应避免复用旧的 `build/` 和 `install/` 产物，以免编译对象混淆。

## 2026-07-22 交通 Demo 进度

- 八类 INT8 图片 benchmark 已在 RK3568 实板完成首轮测试：流水线平均 `39.071 ms`
  （`25.594 FPS`），纯 NPU 平均 `31.126 ms`（`32.128 FPS`）。
- `4_NPU_Yolov8_Traffic_Demo` 已新增 `yolov8_traffic_pcie_demo`：复用项目 3 的 Pango PCIe
  采集、6 个固定 BGR565 帧槽、隔帧推理、RGA 转换和 DRM 显示。
- 已接入 traffic light 框内红/绿像素统计、可由 `--roi` 覆盖的归一化多边形斑马线、
  person 底边入区判断和 `red_violation` 叠加显示。
- PCIe 后处理已限制为 person/light 并优先保留交通灯；新增 person ID、框平滑、8 个推理帧漏检
  保持、单次入区事件去重和 5 帧灯色投票。该实时目标尚待 Ubuntu 交叉编译和实板联调。
- 根据板端 `red=2～3 green=0` 实测，将小交通灯的最小红/绿有效像素数从 8 调为 2，保留 1.2 倍
  颜色优势约束；终端日志新增 `color_active`。静态红灯应先得到 `raw=red`，累计 3 票后稳定为红灯。
- 项目 4 的内置斑马线 ROI 已更新为 PC 端稳定掩码生成的 5 点归一化多边形；不传 `--roi` 时直接
  使用该区域。person 底边仍均匀采样 5 点，至少 1 点入多边形即判定进入 ROI。
