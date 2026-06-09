# Simulation Notes

The vendor PCIe simulation entry remains under:

- `ipcore/pcie_test/example_design/bench/pango_pcie_top_tb.v`
- `ipcore/pcie_test/sim/modelsim/pango_pcie_top_filelist.f`
- `ipcore/pcie_test/sim/modelsim/pango_pcie_top_sim.do`

Use those files when validating the reference TLP flows described in the
manual: PIO, DMA Mrd, DMA Mwr, CplD, `tkeep/tlast/tvalid`, and the 4 KB boundary
flag.

`scripts/pds_sources.f` is a project-level source list for lint/elaboration
helpers. It intentionally uses the RTL under `hdl/` for DMA and UART modules,
instead of the duplicate copies under `ipcore/pcie_test/example_design/rtl/`.
The checked-in ModelSim filelist follows the same rule.

## V2 AXIS MWr Self-Test

`pcie_v2_axis_mwr_tb.v` is a focused self-checking testbench for the V2
mailbox/video MWr engine. It does not instantiate the PCIe hard IP model.
Instead, it drives BAR0 mailbox writes directly and checks the generated
AXI4-Stream MWr packets.

Run it from the project root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run_v2_axis_mwr_sim.ps1
```

The test verifies response MWr, a complete generated `640x640 RGB888` frame
payload, 4 KB packet-boundary handling, and the final frame descriptor.

## ModelSim Helper

Use `scripts/run_modelsim_pcie_example.ps1` to run the vendor PCIe example
simulation from a temporary directory. The helper rewrites the filelist to
absolute source paths and replaces the stale PDS simulation-library path in the
original `.do` file.

Compile-only check:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run_modelsim_pcie_example.ps1 -Mode compile
```

Full run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run_modelsim_pcie_example.ps1 -Mode run
```

Short smoke run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run_modelsim_pcie_example.ps1 -Mode run -RunTime 10us
```

The default PDS root is `D:/PDS_2022.2-SP6.4`. Override it with `-PdsRoot` if
PDS is installed elsewhere. All ModelSim `work/` libraries and logs are emitted
under a temporary directory, not inside this repository. The helper skips the
wave setup by default because ModelSim batch mode does not support those GUI
wave commands; pass `-KeepWave` if running interactively.
