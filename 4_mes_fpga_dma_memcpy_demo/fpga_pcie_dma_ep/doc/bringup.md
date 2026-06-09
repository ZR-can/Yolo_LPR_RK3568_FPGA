# RK3568 Bring-Up Checklist

## Power-Up Order

1. Program or solidify the FPGA PCIe bitstream first.
2. Power-cycle or boot RK3568 after the FPGA endpoint is ready.
3. Load the RK-side PCIe DMA driver.

RK3568 only enumerates the endpoint during PCIe scan. If the FPGA image is not
loaded in time, reboot RK after programming FPGA.

## Link Check

```bash
insmod smdt_pcie_dma_memcpy.ko
lspci -vv
```

Expected:

- Device ID shows `0755:0755`.
- Link status shows `Speed 5GT/s`.
- Link width shows `Width x2`.
- FPGA link LEDs blink as defined by the reference top.

If the RK side does not enumerate the endpoint, first confirm that the FPGA
bitstream was loaded before RK PCIe scan, then reboot RK after programming the
FPGA.

## Bitstream Notes

The Windows/PDS verification flow reached bitstream generation and timing
closure. Before using the generated image as a production image, confirm the
board-specific SCBV bitstream option in PDS. The tool currently reports:

```text
Bitstream-2001: SCBV has not been set.
```

The PCIe IP metadata and project target are both set to
`PG2L100H/FBG484/-6`. If the local PDS GUI still blocks on `Flow-0177`, install
the matching Logos2 PCIe IP package before customizing `pcie_test.idf`. For
this generated IP use `ips2l_pcie_gen2_v1_2c.iar`, and keep the external RTL
interface and PCIe settings unchanged if you regenerate it.

## DMA Check

Use the existing board-side demo. Replace `eventX` with the interrupt/input
device path reported by the board image.

```bash
./smdt_dma_memcpy_demo -a 0xf0200000 -s 20480 -c 1 -d /dev/input/eventX
```

Then repeat with:

```bash
./smdt_dma_memcpy_demo -a 0xf0200000 -s 2048 -c 10 -d /dev/input/eventX
./smdt_dma_memcpy_demo -a 0xf0200000 -s 65536 -c 10 -d /dev/input/eventX
```

Pass criteria: the executable starts, DMA read/write complete, and total error
count remains 0. Build success or `adb push` success alone is not runtime
validation.

## V2 RK-FPGA Communication Check

Build the userspace tools on the Ubuntu/RK3568 build side:

```bash
cd 4_mes_fpga_dma_memcpy_demo/src
make
```

After the FPGA V2 bitstream is loaded before RK PCIe enumeration, run:

```bash
insmod smdt_pcie_dma_memcpy.ko
lspci -vv
./rk_fpga_pcie_comm_demo --no-video
```

Pass criteria for the command/response path:

- The tool maps `/dev/pcie_dma_memcpy` successfully.
- `DMA_CMD_SETUP` returns a nonzero FPGA-visible RK DDR bus address.
- The FPGA writes back a response descriptor with magic `0x52325043` and a
  matching sequence number.
- Response checksum matches and `error_flags` is `0`.

Then check the RGB888 video ring:

```bash
./rk_fpga_pcie_comm_demo --frames 1 --dump frame0.rgb
```

Pass criteria for the video path:

- The tool prints a frame descriptor with magic `0x46325043`.
- Frame bytes are `1228800`, width is `640`, height is `640`, and stride is
  `1920`.
- Frame checksum matches the descriptor fields.
- `frame0.rgb` can be inspected as raw RGB888 `640x640`.

If `DMA_CMD_SETUP` does not return an address usable by FPGA MWr, the existing
driver must be extended with an ioctl that reports the DMA buffer bus address,
or V2 must move to the dedicated RK PCIe driver route.
