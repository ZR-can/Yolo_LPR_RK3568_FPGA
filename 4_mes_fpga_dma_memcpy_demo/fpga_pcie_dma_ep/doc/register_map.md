# PCIe DMA/V2 Register Map

V2 keeps the vendor reference DMA register behavior and adds a BAR0 mailbox for
RK3568-to-FPGA commands. The existing RK-side driver can still use the original
DMA/PIO path, while the FPGA can now send response and RGB888 frame data back to
RK DDR with PCIe MWr.

## PCIe BAR Layout

| BAR | Use | Notes |
| --- | --- | --- |
| `BAR0` | PIO/legacy BRAM and V2 mailbox | Offset `0x000` is observed by V2 command logic while still writing the legacy BAR0 BRAM. |
| `BAR1` | DMA control register window | Must be enabled in the PCIe IP; contains the experiment manual registers below. |
| `BAR2` | Disabled in V2 | Kept disabled to avoid a new RK-side BAR dependency. |

## DMA Command Window

| Offset | Name | Direction | Description |
| --- | --- | --- | --- |
| `0x100` | `dma_cmd_reg` | RW | DMA command register |
| `0x110` | `dma_cmd_l_addr` | RW | Low 32 bits of target memory address |
| `0x120` | `dma_cmd_h_addr` | RW | High 32 bits of target memory address |
| `0x190` | `dma_check_result_l` | RO | Low 32 bits of DMA check result |
| `0x1A0` | `dma_check_result_h` | RO | High 32 bits of DMA check result |
| `0x1B0` | `dma_check_success` | RO | DMA check success flag |

## `dma_cmd_reg`

| Bits | Meaning |
| --- | --- |
| `[9:0]` | DMA transfer length in DWORDs; `0` means 1024 DWORDs / 4096 bytes in the reference logic |
| `[16]` | Address width, `0 = 32-bit`, `1 = 64-bit` |
| `[24]` | DMA packet type, `0 = Mrd`, `1 = Mwr` |

## Address Registers

- `dma_cmd_l_addr[31:2]` carries `dma_addr[31:2]`.
- `dma_cmd_l_addr[1:0]` is reserved.
- `dma_cmd_h_addr[31:0]` carries `dma_addr[63:32]` when 64-bit addressing is enabled.

V2 does not change these BAR1 offsets so the existing RK-side driver behavior is
kept intact.

## V2 BAR0 Mailbox

RK writes the following 64-byte descriptor to `BAR0 + 0x000`. The FPGA observes
the writes and responds with an MWr to `response_addr`.

| BAR0 Offset | DWORDs | Description |
| --- | --- | --- |
| `0x000` | `magic`, `op`, `seq`, `payload_len` | `magic = 0x56325043`; `op`: `1=ping`, `2=start RGB888 video`, `3=stop video`. |
| `0x010` | `response_addr_lo`, `response_addr_hi`, `slot_size`, `slot_count` | RK DDR address for the response descriptor and video ring slot parameters. |
| `0x020` | `ring_base_lo`, `ring_base_hi`, `control`, `reserved` | RK DDR ring base for RGB888 frame slots. |
| `0x030` | `checksum`, reserved | Command checksum for software/debug visibility. |

## V2 MWr Response Descriptor

The FPGA writes this 32-byte descriptor to `response_addr`.

| Field | Description |
| --- | --- |
| `magic` | `0x52325043` |
| `seq` | Mirrors the command sequence number. |
| `heartbeat` | FPGA free-running counter in the `pclk_div2` domain. |
| `checksum` | `magic ^ seq ^ heartbeat ^ status ^ frame_id ^ frame_bytes ^ error_flags`. |
| `status` | Bit 0 indicates V2 logic alive; bit 1 indicates video enabled. |
| `frame_id` | Last completed RGB888 frame counter. |
| `frame_bytes` | Fixed at `1,228,800`. |
| `error_flags` | Bit 0 indicates an unsupported command opcode; bit 1 indicates a command checksum mismatch. |

## V2 RGB888 Frame Slot

Each ring slot is `64 + 1,228,800` bytes by default. The FPGA writes the
`640x640` RGB888 frame payload first, then writes the descriptor so RK can treat
descriptor arrival as "frame complete".

| Slot Offset | Description |
| --- | --- |
| `0x000` | 32-byte frame descriptor, `magic = 0x46325043`. |
| `0x040` | RGB888 payload, width `640`, height `640`, stride `1920`, frame size `1,228,800`. |
