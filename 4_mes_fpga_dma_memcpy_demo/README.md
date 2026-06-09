# 4_mes_fpga_dma_memcpy_demo

## 功能说明

该目录用于 FPGA / PCIe DMA 链路的独立读写性能测试，以及 V2 RK3568-FPGA
PCIe 通信闭环验证；不直接承担车牌检测与识别主流程。

当前代码的目标是：

- 访问 `/dev/pcie_dma_memcpy`
- 完成 DMA 读写测试
- 对比 DMA 与 CPU memcpy 的耗时与带宽
- 输出错误计数与吞吐统计
- 通过 V2 BAR0 mailbox 验证 RK3568 与 FPGA 的 command/response
- 接收 FPGA 主动 MWr 到 RK DDR 的 `640x640 RGB888` 测试帧

## 目录结构

- `src/mes_dma_memcpy_demo.c`
  DMA benchmark 主程序源码
- `src/rk_fpga_pcie_comm_demo.c`
  V2 RK3568-FPGA PCIe 通信和 RGB888 帧接收测试工具
- `src/Makefile`
  编译脚本
- `fpga_pcie_dma_ep/`
  MES2L100H FPGA 端 PCIe DMA/PIO V2 源码工程，基于厂商 100H PCIe DMA 参考设计整理。
- `bin/`
  历史二进制文件

## 编译方法

```bash
cd 4_mes_fpga_dma_memcpy_demo/src
make
```

说明：

- `Makefile` 默认使用 `aarch64-buildroot-linux-gnu-gcc`
- 若交叉编译器前缀不同，可在命令行覆盖：

```bash
make CC=<your-gcc>
```

## 运行方法

DMA benchmark 支持以下主要参数：

- `-a`：目标物理地址，十六进制
- `-s`：传输大小，单位字节
- `-c`：测试循环次数
- `-d`：输入事件设备路径

示例：

```bash
./dma_memcpy_demo -a 0x01000000 -s 2048 -c 1000 -d /dev/input/event2
```

V2 通信闭环测试：

```bash
./rk_fpga_pcie_comm_demo --no-video
./rk_fpga_pcie_comm_demo --frames 1 --dump frame0.rgb
```

## 输出内容

程序运行后会输出：

- DMA 读速度 / 写速度
- CPU 读速度 / 写速度
- 平均耗时
- 数据校验错误计数

## 注意事项

- 该 Demo 依赖目标系统已经提供 `/dev/pcie_dma_memcpy`
- V2 RGB888 是 FPGA 内部测试源，用于 PCIe 通信闭环，不代表外部视频采集或主识别流程已经接入
- 输入事件设备路径也必须有效，否则初始化会失败
- 当前 `Makefile` 输出 `dma_memcpy_demo` 和 `rk_fpga_pcie_comm_demo`；`bin/` 目录中存在历史二进制，后续使用时以当前编译结果为准
