# Verilog Source Partition

## Effective Synthesis Sources

The V2 synthesis design is driven by:

```text
scripts/pds_sources.f
pcie_dma_test_100h_top.pds
```

The intended top-level chain is:

```text
pcie_dma_test_100h_top
  -> pcie_dma_test
    -> pcie_test
    -> ips2l_pcie_dma
    -> pgr_uart2apb_top_32bit
    -> pcie_v2_axis_mwr
```

Use `scripts/check_project.ps1` for the current source count and duplicate
module check.

Keep the following RTL groups as synthesis-effective sources:

| Path | Role |
| --- | --- |
| `hdl/top/pcie_dma_test_100h_top.v` | Project wrapper top |
| `hdl/pcie_dma_test.v` | Vendor integration top |
| `hdl/pcie_dma_ctrl/` | DMA/PIO RX, TX, controller, RAM/FIFO |
| `hdl/video/` | V2 BAR0 mailbox observer and FPGA-initiated response/RGB888 MWr engine |
| `hdl/uart2apb_32bit/` | Debug APB bridge retained from reference design |
| `ipcore/pcie_test/pcie_test.v` | Generated PCIe IP wrapper |
| `ipcore/pcie_test/rtl/` | Generated PCIe IP RTL and HSST support |
| `ipcore/pcie_test/example_design/rtl/pcie_cfg_ctrl/` | RC-side config helper, retained for simulation/reference conditional code |

## Removed Redundant Sources

The vendor package duplicated DMA and UART/APB RTL under
`ipcore/pcie_test/example_design/rtl/`. Those copies are not used by the PDS
wrapper project and can create duplicate-module confusion if added to the same
compile set.

The following duplicate paths are intentionally absent:

```text
ipcore/pcie_test/example_design/rtl/pcie_dma_ctrl/
ipcore/pcie_test/example_design/rtl/uart2apb_32bit/
ipcore/pcie_test/pcie_test_tmpl.v
```

`scripts/check_project.ps1` fails if these redundant RTL copies reappear.

The old non-wrapper implementation entrypoints are also intentionally absent:

```text
pcie_dma_test.pds
fdc/pcie_dma_test.fdc
ipcore/pcie_test/pnr/example_design/
ipcore/pcie_test/pnr/core_only/
```

Keep `pcie_dma_test_100h_top.pds` as the only PDS project entry for V2.

## Simulation Sources

The vendor testbench files remain under:

```text
ipcore/pcie_test/example_design/bench/
ipcore/pcie_test/sim/modelsim/
```

The ModelSim filelist keeps PCIe IP and testbench sources from `ipcore/`, but
uses the canonical `hdl/` copies for DMA/UART RTL.

The `.v` files outside `scripts/pds_sources.f` are intentionally limited to:

```text
ipcore/pcie_test/example_design/bench/
ipcore/pcie_test/sim/modelsim/*_init_param.v
ipcore/pcie_test/sim/modelsim/*_Reset_Value.v
```
