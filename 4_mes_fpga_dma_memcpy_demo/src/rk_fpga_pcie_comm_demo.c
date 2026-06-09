/*
 * RK3568 <-> FPGA PCIe V2 communication smoke test.
 *
 * This userspace tool reuses the existing /dev/pcie_dma_memcpy driver. It
 * writes a V2 mailbox command to FPGA BAR0 through the legacy DMA path, then
 * polls a shared RK DDR buffer for FPGA-initiated MWr response and RGB888 frame
 * descriptors.
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define DMA_DEVICE_PATH         "/dev/pcie_dma_memcpy"

#define DMA_CMD_SETUP           0x01000000u
#define DMA_CMD_START           0x02000000u
#define DMA_CMD_GET_TIMING      0x03000000u
#define DMA_CMD_SHUTDOWN        0x04000000u

#define V2_CMD_MAGIC            0x56325043u
#define V2_RESP_MAGIC           0x52325043u
#define V2_FRAME_MAGIC          0x46325043u

#define V2_OP_PING              0x00000001u
#define V2_OP_VIDEO_START       0x00000002u
#define V2_OP_VIDEO_STOP        0x00000003u

#define VIDEO_WIDTH             640u
#define VIDEO_HEIGHT            640u
#define VIDEO_STRIDE            1920u
#define VIDEO_BYTES             (VIDEO_WIDTH * VIDEO_HEIGHT * 3u)
#define FRAME_DESC_BYTES        64u
#define DEFAULT_SLOT_SIZE       (FRAME_DESC_BYTES + VIDEO_BYTES)
#define DEFAULT_RESPONSE_OFFSET 4096u
#define DEFAULT_RING_OFFSET     8192u

typedef struct {
    uint32_t src_phys_addr;
    uint32_t dst_phys_addr;
    uint32_t direction;
    uint32_t chunk_size;
    uint32_t total_size;
} DMAConfig;

typedef struct __attribute__((packed, aligned(16))) {
    uint32_t magic;
    uint32_t op;
    uint32_t seq;
    uint32_t payload_len;

    uint64_t response_addr;
    uint32_t slot_size;
    uint32_t slot_count;

    uint64_t ring_base;
    uint32_t control;
    uint32_t reserved0;

    uint32_t checksum;
    uint32_t reserved1[3];
} V2Command;

typedef struct __attribute__((packed, aligned(16))) {
    uint32_t magic;
    uint32_t seq;
    uint32_t heartbeat;
    uint32_t checksum;
    uint32_t status;
    uint32_t frame_id;
    uint32_t frame_bytes;
    uint32_t error_flags;
} V2Response;

typedef struct __attribute__((packed, aligned(16))) {
    uint32_t magic;
    uint32_t frame_bytes;
    uint32_t frame_id;
    uint32_t checksum;
    uint32_t slot_index;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
} V2FrameDesc;

typedef struct {
    const char *device_path;
    const char *dump_path;
    uint32_t fpga_addr;
    uint32_t slots;
    uint32_t frames;
    uint32_t timeout_ms;
    bool video;
    bool send_stop;
} AppConfig;

static uint64_t monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "RK3568 FPGA PCIe V2 communication demo\n\n"
        "Usage: %s [options]\n\n"
        "Options:\n"
        "  -d, --device PATH       DMA device path (default: %s)\n"
        "  -a, --fpga-addr HEX     FPGA BAR0 mailbox address (default: 0x0)\n"
        "  -s, --slots N           RGB888 ring slots (default: 2)\n"
        "  -f, --frames N          Frames to wait for in video mode (default: 1)\n"
        "  -o, --dump PATH         Dump the first completed RGB888 frame\n"
        "  -t, --timeout-ms N      Poll timeout in milliseconds (default: 3000)\n"
        "      --no-video          Only run command/response ping\n"
        "      --stop              Send a video-stop command before exit\n"
        "  -h, --help              Show this help\n\n"
        "Runtime assumption: DMA_CMD_SETUP must return an FPGA-visible RK DDR\n"
        "bus address for the mapped buffer, because the FPGA writes responses\n"
        "and video frames to that address with PCIe MWr.\n",
        prog, DMA_DEVICE_PATH);
}

static bool parse_u32(const char *text, uint32_t *out)
{
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 0);
    if (!text[0] || (end && *end) || value > 0xfffffffful) {
        return false;
    }
    *out = (uint32_t)value;
    return true;
}

static bool parse_args(int argc, char **argv, AppConfig *cfg)
{
    static const struct option long_options[] = {
        {"device", required_argument, NULL, 'd'},
        {"fpga-addr", required_argument, NULL, 'a'},
        {"slots", required_argument, NULL, 's'},
        {"frames", required_argument, NULL, 'f'},
        {"dump", required_argument, NULL, 'o'},
        {"timeout-ms", required_argument, NULL, 't'},
        {"no-video", no_argument, NULL, 1000},
        {"stop", no_argument, NULL, 1001},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int opt;
    memset(cfg, 0, sizeof(*cfg));
    cfg->device_path = DMA_DEVICE_PATH;
    cfg->fpga_addr = 0;
    cfg->slots = 2;
    cfg->frames = 1;
    cfg->timeout_ms = 3000;
    cfg->video = true;

    while ((opt = getopt_long(argc, argv, "d:a:s:f:o:t:h", long_options, NULL)) != -1) {
        switch (opt) {
        case 'd':
            cfg->device_path = optarg;
            break;
        case 'a':
            if (!parse_u32(optarg, &cfg->fpga_addr)) {
                fprintf(stderr, "Invalid --fpga-addr: %s\n", optarg);
                return false;
            }
            break;
        case 's':
            if (!parse_u32(optarg, &cfg->slots) || cfg->slots == 0) {
                fprintf(stderr, "Invalid --slots: %s\n", optarg);
                return false;
            }
            break;
        case 'f':
            if (!parse_u32(optarg, &cfg->frames) || cfg->frames == 0) {
                fprintf(stderr, "Invalid --frames: %s\n", optarg);
                return false;
            }
            break;
        case 'o':
            cfg->dump_path = optarg;
            break;
        case 't':
            if (!parse_u32(optarg, &cfg->timeout_ms) || cfg->timeout_ms == 0) {
                fprintf(stderr, "Invalid --timeout-ms: %s\n", optarg);
                return false;
            }
            break;
        case 1000:
            cfg->video = false;
            break;
        case 1001:
            cfg->send_stop = true;
            break;
        case 'h':
            usage(argv[0]);
            exit(EXIT_SUCCESS);
        default:
            return false;
        }
    }

    return true;
}

static uint32_t command_checksum(const V2Command *cmd)
{
    return cmd->magic ^ cmd->op ^ cmd->seq ^ cmd->payload_len ^
           (uint32_t)cmd->response_addr ^ (uint32_t)(cmd->response_addr >> 32) ^
           cmd->slot_size ^ cmd->slot_count ^
           (uint32_t)cmd->ring_base ^ (uint32_t)(cmd->ring_base >> 32) ^
           cmd->control;
}

static uint32_t response_checksum(uint32_t seq, uint32_t heartbeat,
                                  uint32_t status, uint32_t frame_id,
                                  uint32_t frame_bytes, uint32_t error_flags)
{
    return V2_RESP_MAGIC ^ seq ^ heartbeat ^ status ^ frame_id ^
           frame_bytes ^ error_flags;
}

static uint32_t frame_checksum(uint32_t frame_id)
{
    return V2_FRAME_MAGIC ^ frame_id ^ VIDEO_BYTES ^ VIDEO_WIDTH ^
           VIDEO_HEIGHT ^ VIDEO_STRIDE;
}

static int write_command(int fd, void *map, uint32_t seq, uint32_t op,
                         uint64_t bus_base, uint32_t response_offset,
                         uint32_t ring_offset, uint32_t slot_size,
                         uint32_t slots)
{
    V2Command *cmd = (V2Command *)map;
    memset(cmd, 0, sizeof(*cmd));
    cmd->magic = V2_CMD_MAGIC;
    cmd->op = op;
    cmd->seq = seq;
    cmd->payload_len = sizeof(*cmd);
    cmd->response_addr = bus_base + response_offset;
    cmd->slot_size = slot_size;
    cmd->slot_count = slots;
    cmd->ring_base = bus_base + ring_offset;
    cmd->control = 0;
    cmd->checksum = command_checksum(cmd);

    if (msync(map, sizeof(*cmd), MS_SYNC) < 0) {
        perror("msync command");
    }

    if (ioctl(fd, DMA_CMD_START, 0) < 0) {
        perror("DMA_CMD_START");
        return -1;
    }

    return 0;
}

static int wait_response(volatile V2Response *resp, uint32_t seq,
                         uint32_t timeout_ms)
{
    uint64_t deadline = monotonic_ms() + timeout_ms;
    while (monotonic_ms() < deadline) {
        __sync_synchronize();
        if (resp->magic == V2_RESP_MAGIC && resp->seq == seq) {
            uint32_t expected = response_checksum(resp->seq, resp->heartbeat,
                                                  resp->status, resp->frame_id,
                                                  resp->frame_bytes,
                                                  resp->error_flags);
            if (resp->checksum != expected) {
                fprintf(stderr,
                        "Invalid response checksum: got 0x%08x expected 0x%08x\n",
                        resp->checksum, expected);
                return -1;
            }
            if ((resp->status & 0x1u) == 0 || resp->frame_bytes != VIDEO_BYTES ||
                resp->error_flags != 0) {
                fprintf(stderr,
                        "Invalid response status=0x%08x frame_bytes=%u errors=0x%08x\n",
                        resp->status, resp->frame_bytes, resp->error_flags);
                return -1;
            }
            return 0;
        }
        usleep(1000);
    }

    return -1;
}

static int wait_frame(uint8_t *base, uint32_t ring_offset, uint32_t slot_size,
                      uint32_t slots, uint32_t last_frame, uint32_t timeout_ms,
                      V2FrameDesc **out_desc, uint8_t **out_rgb)
{
    uint64_t deadline = monotonic_ms() + timeout_ms;
    while (monotonic_ms() < deadline) {
        uint32_t i;
        for (i = 0; i < slots; ++i) {
            uint8_t *slot = base + ring_offset + (uint64_t)i * slot_size;
            volatile V2FrameDesc *desc = (volatile V2FrameDesc *)slot;
            __sync_synchronize();
            if (desc->magic == V2_FRAME_MAGIC &&
                desc->frame_bytes == VIDEO_BYTES &&
                desc->frame_id != last_frame) {
                uint32_t expected = frame_checksum(desc->frame_id);
                if (desc->checksum != expected ||
                    desc->width != VIDEO_WIDTH ||
                    desc->height != VIDEO_HEIGHT ||
                    desc->stride != VIDEO_STRIDE) {
                    fprintf(stderr,
                            "Invalid frame descriptor: id=%u checksum=0x%08x expected=0x%08x %ux%u stride=%u\n",
                            desc->frame_id, desc->checksum, expected,
                            desc->width, desc->height, desc->stride);
                    return -1;
                }
                *out_desc = (V2FrameDesc *)slot;
                *out_rgb = slot + FRAME_DESC_BYTES;
                return 0;
            }
        }
        usleep(1000);
    }

    return -1;
}

static int dump_rgb(const char *path, const uint8_t *rgb)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        perror("fopen dump");
        return -1;
    }

    if (fwrite(rgb, 1, VIDEO_BYTES, fp) != VIDEO_BYTES) {
        perror("fwrite dump");
        fclose(fp);
        return -1;
    }

    fclose(fp);
    return 0;
}

int main(int argc, char **argv)
{
    AppConfig app;
    int fd = -1;
    void *mapped = MAP_FAILED;
    uint32_t slot_size = DEFAULT_SLOT_SIZE;
    uint32_t ring_offset = DEFAULT_RING_OFFSET;
    uint32_t response_offset = DEFAULT_RESPONSE_OFFSET;
    uint32_t shared_size;
    uint32_t bus_base;
    long setup_ret;
    uint32_t seq = 1;
    uint32_t frame;
    uint32_t last_frame = 0xffffffffu;

    if (!parse_args(argc, argv, &app)) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }

    if (app.slots > (UINT32_MAX - ring_offset) / slot_size) {
        fprintf(stderr, "Ring size overflows 32-bit DMA setup: slots=%u\n", app.slots);
        return EXIT_FAILURE;
    }

    shared_size = ring_offset + slot_size * app.slots;
    if (!app.video) {
        shared_size = ring_offset;
    }

    fd = open(app.device_path, O_RDWR);
    if (fd < 0) {
        perror("open DMA device");
        return EXIT_FAILURE;
    }

    DMAConfig dma_cfg;
    memset(&dma_cfg, 0, sizeof(dma_cfg));
    dma_cfg.src_phys_addr = app.fpga_addr;
    dma_cfg.dst_phys_addr = 0;
    dma_cfg.direction = 0;
    dma_cfg.chunk_size = sizeof(V2Command);
    dma_cfg.total_size = shared_size;

    setup_ret = ioctl(fd, DMA_CMD_SETUP, &dma_cfg);
    if (setup_ret <= 0) {
        if (setup_ret < 0) {
            perror("DMA_CMD_SETUP");
        } else {
            fprintf(stderr, "DMA_CMD_SETUP returned bus address 0\n");
        }
        close(fd);
        return EXIT_FAILURE;
    }
    bus_base = (uint32_t)setup_ret;

    mapped = mmap(NULL, shared_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED) {
        perror("mmap DMA buffer");
        ioctl(fd, DMA_CMD_SHUTDOWN, 0);
        close(fd);
        return EXIT_FAILURE;
    }
    memset(mapped, 0, shared_size);

    printf("mapped %u bytes, FPGA-visible bus base 0x%08x\n", shared_size, bus_base);
    printf("sending ping command to FPGA BAR0 offset 0x%08x\n", app.fpga_addr);

    if (write_command(fd, mapped, seq, V2_OP_PING, bus_base, response_offset,
                      ring_offset, slot_size, app.slots) < 0) {
        goto fail;
    }

    volatile V2Response *resp = (volatile V2Response *)((uint8_t *)mapped + response_offset);
    if (wait_response(resp, seq, app.timeout_ms) < 0) {
        fprintf(stderr, "Timed out waiting for FPGA response seq=%u\n", seq);
        goto fail;
    }

    printf("response: seq=%u heartbeat=%u status=0x%08x frame_id=%u errors=0x%08x\n",
           resp->seq, resp->heartbeat, resp->status, resp->frame_id, resp->error_flags);

    if (app.video) {
        seq++;
        printf("starting RGB888 video ring: %u slot(s), slot_size=%u\n",
               app.slots, slot_size);
        if (write_command(fd, mapped, seq, V2_OP_VIDEO_START, bus_base,
                          response_offset, ring_offset, slot_size, app.slots) < 0) {
            goto fail;
        }
        if (wait_response(resp, seq, app.timeout_ms) < 0) {
            fprintf(stderr, "Timed out waiting for video-start response seq=%u\n", seq);
            goto fail;
        }
        if ((resp->status & 0x2u) == 0) {
            fprintf(stderr, "Video-start response did not set video-enabled status\n");
            goto fail;
        }

        for (frame = 0; frame < app.frames; ++frame) {
            V2FrameDesc *desc = NULL;
            uint8_t *rgb = NULL;
            if (wait_frame((uint8_t *)mapped, ring_offset, slot_size, app.slots,
                           last_frame, app.timeout_ms, &desc, &rgb) < 0) {
                fprintf(stderr, "Timed out waiting for RGB888 frame %u\n", frame);
                goto fail;
            }

            printf("frame: id=%u bytes=%u %ux%u stride=%u slot=%u\n",
                   desc->frame_id, desc->frame_bytes, desc->width, desc->height,
                   desc->stride, desc->slot_index);

            if (frame == 0 && app.dump_path) {
                if (dump_rgb(app.dump_path, rgb) < 0) {
                    goto fail;
                }
                printf("dumped first RGB888 frame to %s\n", app.dump_path);
            }
            last_frame = desc->frame_id;
        }
    }

    if (app.send_stop || app.video) {
        seq++;
        (void)write_command(fd, mapped, seq, V2_OP_VIDEO_STOP, bus_base,
                            response_offset, ring_offset, slot_size, app.slots);
    }

    munmap(mapped, shared_size);
    ioctl(fd, DMA_CMD_SHUTDOWN, 0);
    close(fd);
    return EXIT_SUCCESS;

fail:
    if (mapped != MAP_FAILED) {
        munmap(mapped, shared_size);
    }
    if (fd >= 0) {
        ioctl(fd, DMA_CMD_SHUTDOWN, 0);
        close(fd);
    }
    return EXIT_FAILURE;
}
