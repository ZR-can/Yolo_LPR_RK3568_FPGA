# MES2L100H FPGA PCIe DMA/PIO Endpoint V2

This directory contains a source-only FPGA PCIe DMA/PIO V2 project for the
MES2L100H board. It is based on the vendor `fpga_demo_100h/pcie_dma_test_100h`
reference project and is intended to stay compatible with the RK-side
`smdt_pcie_dma_memcpy.ko` driver while adding a first RK3568-FPGA PCIe
communication and RGB888 video test path.

## Scope

- FPGA role: PCIe Endpoint.
- Link target: Gen2 x2, 128-bit AXI4-Stream.
- Expected PCI ID during board bring-up: `0755:0755`.
- V2 data path: legacy PIO/DMA memory access plus BAR0 mailbox command capture,
  FPGA-initiated response MWr, and internal `640x640 RGB888 @30fps` test-video
  MWr into RK DDR.
- Not included: external camera/video ingress or integration into
  `3_NPU_Yolov8_LPR_Demo`.

## Directory Layout

- `hdl/`
  Effective synthesis RTL: vendor integration top, DMA controller RTL,
  UART-to-APB debug RTL, V2 video/communication RTL, and the project wrapper
  `hdl/top/pcie_dma_test_100h_top.v`.
- `ipcore/`
  Purple/Pango PCIe IP wrapper sources and simulation support files. Duplicate
  vendor example DMA/UART RTL is intentionally not kept here.
- `fdc/`
  MES2L100H constraints for the project wrapper top.
- `sim/`
  Local filelists and notes for simulation entrypoints.
- `doc/`
  Architecture, register map, PDS verification, and RK-side bring-up notes.
- `scripts/`
  Helper filelists or future PDS automation snippets.

## Recommended PDS Entry

Use `pcie_dma_test_100h_top.pds` for V1. It selects the wrapper top
`pcie_dma_test_100h_top` and the wrapper constraint file
`fdc/pcie_dma_test_100h_top.fdc`. Do not switch the active top back to the
vendor `pcie_dma_test` module when building this project.

Legacy non-wrapper PDS/FDC entrypoints from the vendor example are intentionally
not kept in this source tree. The only supported implementation entry for V1 is
the wrapper project above or the equivalent `impl.tcl` batch flow.

The wrapper PDS and PCIe IP metadata target `PG2L100H/FBG484/-6`. The IP RTL is
configured for Endpoint, Gen2 x2, PCI ID `0755:0755`, BAR0 enabled, and BAR1
enabled for the experiment-manual DMA control window. If PDS reports
`Flow-0177` while opening or customizing `pcie_test.idf`, install the matching
Logos2 PCIe IP package first. For this generated IP use
`ips2l_pcie_gen2_v1_2c.iar`; copy it to an ASCII-only local path before adding
it through `Tools -> IP Compiler -> File -> Update -> Add Packages -> Install`.

Generated folders such as `compile/`, `synthesize/`, `place_route/`, and
`generate_bitstream/` are intentionally ignored and should not be committed.

For a command-line PDS flow, run `impl.tcl` from this directory with
`pds_shell.exe -file impl.tcl -work_dir .`. It uses `scripts/pds_sources.f`
and `fdc/pcie_dma_test_100h_top.fdc` so it does not depend on stale absolute
paths from the vendor package.

The effective Verilog source partition is documented in
`doc/source_partition.md`.

## Verified PDS Status

The wrapper project has completed compile, synthesize, place and route, timing
report, and bitstream generation in PDS `2022.2-SP6.4`. The timing summary is
`Design Summary : All Constraints Met.` See `doc/pds_verification.md` for the
captured flow result, timing/resource snapshot, and known PDS warnings.

The V2 mailbox/video MWr engine also has a local self-checking ModelSim test:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run_v2_axis_mwr_sim.ps1
```

The latest run transferred one complete generated `640x640 RGB888` frame
payload (`1,228,800` bytes), verified 4 KB-safe MWr chunking, and observed the
final frame descriptor.

## Board Bring-Up Boundary

The FPGA bitstream must be loaded before RK3568 enumerates PCIe. After RK boots,
load the existing driver and confirm the link manually:

```bash
insmod smdt_pcie_dma_memcpy.ko
lspci -vv
```

Success criteria for the link are device `0755:0755`, `Speed 5GT/s`, and
`Width x2`. DMA and V2 communication/video functional validation are still
board-side manual work.
