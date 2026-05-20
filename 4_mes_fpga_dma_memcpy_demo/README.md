# 4_mes_fpga_dma_memcpy_demo

## 功能说明

该目录用于 FPGA / PCIe DMA 链路的独立读写性能测试，不直接承担车牌检测与识别主流程。

当前代码的目标是：

- 访问 `/dev/pcie_dma_memcpy`
- 完成 DMA 读写测试
- 对比 DMA 与 CPU memcpy 的耗时与带宽
- 输出错误计数与吞吐统计

## 目录结构

- `src/mes_dma_memcpy_demo.c`
  主程序源码
- `src/Makefile`
  编译脚本
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

程序支持以下主要参数：

- `-a`：目标物理地址，十六进制
- `-s`：传输大小，单位字节
- `-c`：测试循环次数
- `-d`：输入事件设备路径

示例：

```bash
./dma_memcpy_demo -a 0x01000000 -s 2048 -c 1000 -d /dev/input/event2
```

## 输出内容

程序运行后会输出：

- DMA 读速度 / 写速度
- CPU 读速度 / 写速度
- 平均耗时
- 数据校验错误计数

## 注意事项

- 该 Demo 依赖目标系统已经提供 `/dev/pcie_dma_memcpy`
- 输入事件设备路径也必须有效，否则初始化会失败
- 当前 `Makefile` 默认输出文件名为 `dma_memcpy_demo`，而 `bin/` 目录中存在历史二进制 `mes_dma_memcpy_demo`，后续使用时以当前编译结果为准
