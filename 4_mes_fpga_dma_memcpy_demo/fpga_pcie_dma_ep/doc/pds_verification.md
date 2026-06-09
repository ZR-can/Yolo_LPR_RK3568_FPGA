# PDS Verification Record

## Host-Side Result

This project has been verified with PDS/Fabric Compiler `2022.2-SP6.4` on
Windows. The latest clean-source `impl.tcl` rerun was completed on
`2026-06-06 23:20` from a temporary copy of the project, after the V2 AXIS MWr
state machine, command decode timing, and 4 KB video-packet boundary handling
were fixed.

Command used:

```powershell
D:/PDS_2022.2-SP6.4/bin/pds_shell.exe -project <temp>/pcie_dma_test_100h_top.pds -run gen_bit_stream
```

The equivalent batch entry also passed:

```powershell
D:/PDS_2022.2-SP6.4/bin/pds_shell.exe -file impl.tcl -work_dir .
```

The full flow completed through bitstream generation:

| Stage | Result |
| --- | --- |
| Compile | Pass |
| Synthesize | Pass |
| Device map | Pass |
| Place and route | Pass |
| Report timing | Pass |
| Generate bitstream | Pass |

Generated bitstream in the temporary verification workspace:

```text
generate_bitstream/pcie_dma_test_100h_top.sbit
```

Bitstream size: `2532364` bytes.

Generated PDS outputs are intentionally not copied into this repository.

Before the PDS rerun, `scripts/check_project.ps1` passed with these key checks:
wrapper top selected, all 87 source-list entries present, 87 modules parsed, no
duplicate module definitions, no redundant vendor example RTL copies, no stale
non-wrapper PDS/FDC entrypoints, and no generated artifacts in the source tree.

## Timing Summary

Post-PnR timing report:

```text
Design Summary : All Constraints Met.
```

Clock performance summary:

| Clock | Requested | Estimated | Worst slack |
| --- | ---: | ---: | ---: |
| `pclk` | 250.0000 MHz | 279.4077 MHz | 0.421 ns |
| `pclk_div2` | 125.0000 MHz | 130.6336 MHz | 0.345 ns |
| `ref_clk` | 100.0000 MHz | 174.0947 MHz | 4.256 ns |

The timing run also reports unconstrained top-level control/status ports:
`pclk_led`, `ref_led`, `txp/txn`, `button_rst_n`, and `perst_n`. These are
consistent with the current reference-style constraint set and did not produce
timing violations.

## Resource Snapshot

Synthesis resource summary:

| Resource | Used | Available | Utilization |
| --- | ---: | ---: | ---: |
| CLMA LUTs | 3766 | 46700 | 9% |
| CLMA FFs | 4442 | 93400 | 5% |
| CLMS LUTs | 1554 | 19900 | 8% |
| CLMS FFs | 1658 | 39800 | 5% |
| DRM | 40.5 | 155 | 27% |
| PCIE | 1 | 1 | 100% |

## Known PDS Warnings

- `Flow-0177`: if PDS GUI cannot load `ipcore/pcie_test/pcie_test.idf`, install
  the matching Logos2 PCIe IP package before customizing the IP. For this
  generated IP use `ips2l_pcie_gen2_v1_2c.iar` and keep the external RTL
  interface and PCIe settings unchanged if you regenerate it for
  `PG2L100H/FBG484/-6`.
- `Bitstream-2001`: `SCBV has not been set`. Confirm the board-specific SCBV
  bitstream option before treating the generated image as a production image.

## ModelSim Simulation Snapshot

### V2 AXIS MWr Video Self-Test

The V2 mailbox/video MWr engine is covered by an independent ModelSim
self-checking testbench:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run_v2_axis_mwr_sim.ps1
```

Latest result on `2026-06-06 22:51`: pass.

```text
V2_AXIS_MWR_TB_PASS: packets=1201 video_bytes=1228800 frame_id=1
```

The testbench checks:

- BAR0 video-start mailbox command and checksum handling.
- FPGA-initiated response MWr header, payload, status, and checksum.
- Complete `640x640 RGB888` payload transfer of `1,228,800` bytes.
- Video MWr chunking without crossing 4 KB boundaries.
- Final frame descriptor MWr after the video payload.

The simulation caught and fixed two V2 issues: continuous-`tready` AXIS payload
advancement after the header, and a 10-bit `1024 DW` overflow at 4 KB-aligned
packet boundaries.

### Vendor PCIe Example

The vendor PCIe example simulation entry has been checked with ModelSim Intel
FPGA Edition `10.5b` and PDS simulation libraries from `D:/PDS_2022.2-SP6.4`.
The checked-in filelist now points DMA/UART entries at the canonical `hdl/`
RTL instead of removed duplicate example-design copies.
Use:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run_modelsim_pcie_example.ps1 -Mode compile
```

Result: compile completed with `Errors: 0`. The project RTL compile stage
reported `Warnings: 0`; remaining warnings are from the encrypted PDS PCIe
simulation library on ModelSim Starter/Intel FPGA Edition.

Short smoke run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/run_modelsim_pcie_example.ps1 -Mode run -RunTime 10us
```

Result: simulation loaded and advanced for `10us` with `Errors: 0`.

The full vendor `run -all` simulation did not finish within a 20-minute local
tool timeout on ModelSim Starter Edition. No runtime error was seen before the
timeout, but the testbench did not reach the printed `PCIe link up!` or
DMA/PIO pass messages in that window. Treat complete DMA/PIO simulation
coverage as pending until it is run to `$finish` on a faster or fully licensed
simulator.

## Board-Side Status

Host-side PDS verification is complete. Board-side PCIe enumeration and DMA
correctness are still pending manual validation on RK3568.
