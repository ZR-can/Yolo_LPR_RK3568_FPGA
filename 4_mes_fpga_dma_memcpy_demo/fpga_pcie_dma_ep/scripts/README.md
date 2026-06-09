# Scripts And Filelists

- `pds_sources.f`
  Source-only compile list for helper tools. PDS can still use the `.pds`
  project files directly.
- `check_project.ps1`
  Read-only Windows-side sanity check for the wrapper PDS, source list, and
  generated-artifact cleanup. It also fails if duplicate vendor RTL or stale
  non-wrapper PDS/FDC entrypoints are copied back into the tree.
- `run_v2_axis_mwr_sim.ps1`
  ModelSim self-test for the V2 mailbox/video MWr engine. It compiles
  `hdl/video/pcie_v2_axis_mwr.v` and `sim/pcie_v2_axis_mwr_tb.v` in a temporary
  work directory and requires the `V2_AXIS_MWR_TB_PASS` marker.
- `../impl.tcl`
  Optional PDS batch flow for `pcie_dma_test_100h_top`, using
  `pds_sources.f` and the wrapper FDC.

Keep scripts side-effect free by default. Do not commit generated PDS output
directories or bitstream files from this FPGA project.

Do not recursively glob every Verilog file as a compile list. The project build
uses the curated DMA/UART copies under `hdl/`, while PCIe IP and testbench
sources stay under `ipcore/`. Use `pds_sources.f` or the PDS project source
list to avoid duplicate module definitions.
