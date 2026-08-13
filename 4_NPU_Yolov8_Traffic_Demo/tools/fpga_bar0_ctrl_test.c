#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "pango_pci.h"

#define FPGA_REG_LEGACY_CONTROL 0x000U
#define FPGA_REG_MAGIC          0x100U
#define FPGA_REG_VERSION        0x110U
#define FPGA_REG_SCRATCH        0x120U
#define FPGA_REG_CAPTURE_CTRL   0x130U
#define FPGA_REG_PREPROC_MODE   0x150U
#define FPGA_REG_THRESHOLD      0x160U
#define FPGA_REG_ROI_XY         0x170U
#define FPGA_REG_ROI_WH         0x180U
#define FPGA_REG_DEBUG_TRIG     0x190U
#define FPGA_REG_FRAME_CFG      0x1a0U
#define FPGA_REG_CTRL_STATUS    0x1b0U
#define FPGA_REG_FRAME_STATUS   0x140U

#define FPGA_CMD_START_CAPTURE  0xffffffe5U
#define FPGA_CMD_STOP_CAPTURE   0xffffff00U

#define DEFAULT_DMA_LEN_DWORDS  (3840U >> 2)

typedef struct Bar0Mapping {
    void *mapping;
    size_t mapping_length;
    volatile uint32_t *regs;
} Bar0Mapping;

static void print_usage(const char *prog)
{
    printf("Usage: %s [options]\n", prog);
    printf("  --status              Read BAR0 frame status once\n");
    printf("  --poll N              Read frame status N times\n");
    printf("  --delay-ms N          Delay between polls, default 100 ms\n");
    printf("  --stop                Write legacy stop command to BAR0+0x000\n");
    printf("  --start               Configure DMA, then write legacy start command\n");
    printf("  --no-stop             Do not send stop after --start polling\n");
    printf("  --regs                Read RK-visible FPGA control register block\n");
    printf("  --scratch VALUE       Write BAR0+0x120 scratch, then read it back\n");
    printf("  --control VALUE       Write BAR0+0x130 capture control bit0\n");
    printf("  --read OFFSET         Read a 32-bit BAR0 register byte offset\n");
    printf("  --write OFFSET        Write a 32-bit BAR0 register byte offset, with --value\n");
    printf("  --value VALUE         Value used by --write\n");
    printf("  --burst-write N       Repeat --write/--value N times after one BAR0 mmap\n");
    printf("  --help                Show this help\n");
}

static int map_bar0(int fd, Bar0Mapping *bar)
{
    COMMAND_OPERATION bar_command;
    unsigned long bar_address;
    unsigned long page_mask;
    unsigned long page_base;
    size_t page_offset;
    long page_size;
    int mem_fd;
    int saved_errno;

    memset(&bar_command, 0, sizeof(bar_command));
    if (ioctl(fd, PCI_MAP_BAR0_CMD, &bar_command) < 0) {
        fprintf(stderr, "PCIe: BAR0 query failed: %s\n", strerror(errno));
        return -1;
    }

    bar_address = bar_command.get_pci_dev_info.bar[0].bar_base;
    page_size = sysconf(_SC_PAGESIZE);
    if (bar_address == 0 || page_size <= 0) {
        fprintf(stderr, "PCIe: invalid BAR0 address or page size\n");
        return -1;
    }

    page_mask = (unsigned long)page_size - 1UL;
    page_base = bar_address & ~page_mask;
    page_offset = (size_t)(bar_address - page_base);
    bar->mapping_length =
        (page_offset + BAR0_MAX + (size_t)page_size - 1U) & ~((size_t)page_size - 1U);

    mem_fd = open(MEM_FILE_PATH, O_RDWR | O_SYNC);
    if (mem_fd < 0) {
        fprintf(stderr, "PCIe: open %s failed: %s\n", MEM_FILE_PATH, strerror(errno));
        return -1;
    }

    bar->mapping = mmap(NULL, bar->mapping_length, PROT_READ | PROT_WRITE,
                        MAP_SHARED, mem_fd, (off_t)page_base);
    saved_errno = errno;
    close(mem_fd);
    if (bar->mapping == MAP_FAILED) {
        errno = saved_errno;
        fprintf(stderr, "PCIe: BAR0 mmap failed: %s\n", strerror(errno));
        return -1;
    }

    bar->regs = (volatile uint32_t *)((unsigned char *)bar->mapping + page_offset);
    printf("BAR0: phys=0x%lx mapped_len=%zu\n", bar_address, bar->mapping_length);
    return 0;
}

static void unmap_bar0(Bar0Mapping *bar)
{
    if (bar->mapping != MAP_FAILED && bar->mapping != NULL) {
        munmap(bar->mapping, bar->mapping_length);
    }
    bar->mapping = MAP_FAILED;
    bar->mapping_length = 0;
    bar->regs = NULL;
}

static int configure_dma_for_capture(int fd, DMA_OPERATION *dma_operation)
{
    memset(dma_operation, 0, sizeof(*dma_operation));
    if (ioctl(fd, PCI_SET_CONFIG, dma_operation) < 0) {
        fprintf(stderr, "PCIe: DMA configuration failed: %s\n", strerror(errno));
        return -1;
    }

    dma_operation->current_len = DEFAULT_DMA_LEN_DWORDS;
    dma_operation->offset_addr = 0;
    if (ioctl(fd, PCI_MAP_ADDR_CMD, dma_operation) < 0) {
        fprintf(stderr, "PCIe: DMA address mapping failed: %s\n", strerror(errno));
        return -1;
    }

    return 0;
}

static void print_device_info(int fd)
{
    COMMAND_OPERATION device_command;
    const PCI_DEVICE_INFO *pci;

    memset(&device_command, 0, sizeof(device_command));
    if (ioctl(fd, PCI_READ_DATA_CMD, &device_command) < 0) {
        fprintf(stderr, "PCIe: reading device information failed: %s\n", strerror(errno));
        return;
    }

    pci = &device_command.get_pci_dev_info;
    printf("PCIe: vendor=0x%04x device=0x%04x link=gen%u x%u mps=%u mrrs=%u\n",
           pci->vendor_id, pci->device_id, pci->link_speed, pci->link_width,
           pci->mps, pci->mrrs);
}

static uint32_t bar0_read32(const Bar0Mapping *bar, uint32_t offset)
{
    uint32_t value = bar->regs[offset >> 2];
    __sync_synchronize();
    return value;
}

static void bar0_write32(const Bar0Mapping *bar, uint32_t offset, uint32_t value)
{
    bar->regs[offset >> 2] = value;
    __sync_synchronize();
}

static void print_frame_status(const Bar0Mapping *bar, unsigned int sample)
{
    uint32_t status = bar0_read32(bar, FPGA_REG_FRAME_STATUS);
    unsigned int frame_done = status & 0x1U;
    unsigned int wr_index = (status >> 1) & 0x3U;

    printf("status[%u]: reg0x140=0x%08x frame_done=%u wr_index=%u\n",
           sample, status, frame_done, wr_index);
}

static void print_reg32(const Bar0Mapping *bar, const char *name, uint32_t offset)
{
    printf("%-14s BAR0+0x%03x = 0x%08x\n", name, offset, bar0_read32(bar, offset));
}

static void print_control_regs(const Bar0Mapping *bar)
{
    uint32_t ctrl_status;

    print_reg32(bar, "magic", FPGA_REG_MAGIC);
    print_reg32(bar, "version", FPGA_REG_VERSION);
    print_reg32(bar, "scratch", FPGA_REG_SCRATCH);
    print_reg32(bar, "capture_ctrl", FPGA_REG_CAPTURE_CTRL);
    print_reg32(bar, "preproc_mode", FPGA_REG_PREPROC_MODE);
    print_reg32(bar, "threshold", FPGA_REG_THRESHOLD);
    print_reg32(bar, "roi_xy", FPGA_REG_ROI_XY);
    print_reg32(bar, "roi_wh", FPGA_REG_ROI_WH);
    print_reg32(bar, "debug_trig", FPGA_REG_DEBUG_TRIG);
    print_reg32(bar, "frame_cfg", FPGA_REG_FRAME_CFG);
    ctrl_status = bar0_read32(bar, FPGA_REG_CTRL_STATUS);
    printf("%-14s BAR0+0x%03x = 0x%08x start=%u frame_done=%u wr_index=%u cfg_writes=%u\n",
           "ctrl_status", FPGA_REG_CTRL_STATUS, ctrl_status,
           ctrl_status & 0x1U, (ctrl_status >> 1) & 0x1U,
           (ctrl_status >> 2) & 0x3U, (ctrl_status >> 4) & 0xffffU);
    print_reg32(bar, "frame_status", FPGA_REG_FRAME_STATUS);
}

static void sleep_ms(unsigned int delay_ms)
{
    struct timespec req;

    req.tv_sec = delay_ms / 1000U;
    req.tv_nsec = (long)(delay_ms % 1000U) * 1000000L;
    while (nanosleep(&req, &req) < 0 && errno == EINTR) {
    }
}

int main(int argc, char **argv)
{
    int fd = -1;
    int opt;
    int option_index = 0;
    int do_status = 0;
    int do_start = 0;
    int do_stop = 0;
    int do_regs = 0;
    int do_scratch = 0;
    int do_control = 0;
    int do_read = 0;
    int do_write = 0;
    int have_write_value = 0;
    int no_stop = 0;
    unsigned int poll_count = 0;
    unsigned int delay_ms = 100;
    unsigned int burst_write_count = 0;
    unsigned int i;
    uint32_t scratch_value = 0;
    uint32_t control_value = 0;
    uint32_t read_offset = 0;
    uint32_t write_offset = 0;
    uint32_t write_value = 0;
    int dma_mapped = 0;
    DMA_OPERATION dma_operation;
    Bar0Mapping bar;

    static const struct option long_options[] = {
        {"status", no_argument, 0, 'r'},
        {"poll", required_argument, 0, 'p'},
        {"delay-ms", required_argument, 0, 'd'},
        {"stop", no_argument, 0, 's'},
        {"start", no_argument, 0, 't'},
        {"no-stop", no_argument, 0, 'n'},
        {"regs", no_argument, 0, 'g'},
        {"scratch", required_argument, 0, 'x'},
        {"control", required_argument, 0, 'c'},
        {"read", required_argument, 0, 'R'},
        {"write", required_argument, 0, 'W'},
        {"value", required_argument, 0, 'V'},
        {"burst-write", required_argument, 0, 'B'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    bar.mapping = MAP_FAILED;
    bar.mapping_length = 0;
    bar.regs = NULL;

    while ((opt = getopt_long(argc, argv, "rp:d:stngx:c:R:W:V:B:h", long_options, &option_index)) != -1) {
        switch (opt) {
        case 'r':
            do_status = 1;
            break;
        case 'p':
            poll_count = (unsigned int)strtoul(optarg, NULL, 0);
            break;
        case 'd':
            delay_ms = (unsigned int)strtoul(optarg, NULL, 0);
            break;
        case 's':
            do_stop = 1;
            break;
        case 't':
            do_start = 1;
            break;
        case 'n':
            no_stop = 1;
            break;
        case 'g':
            do_regs = 1;
            break;
        case 'x':
            do_scratch = 1;
            scratch_value = (uint32_t)strtoul(optarg, NULL, 0);
            break;
        case 'c':
            do_control = 1;
            control_value = (uint32_t)strtoul(optarg, NULL, 0);
            break;
        case 'R':
            do_read = 1;
            read_offset = (uint32_t)strtoul(optarg, NULL, 0);
            break;
        case 'W':
            do_write = 1;
            write_offset = (uint32_t)strtoul(optarg, NULL, 0);
            break;
        case 'V':
            have_write_value = 1;
            write_value = (uint32_t)strtoul(optarg, NULL, 0);
            break;
        case 'B':
            burst_write_count = (unsigned int)strtoul(optarg, NULL, 0);
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 2;
        }
    }

    if (do_write && !have_write_value) {
        fprintf(stderr, "BAR0: --write requires --value\n");
        return 2;
    }
    if (burst_write_count != 0U && (!do_write || !have_write_value)) {
        fprintf(stderr, "BAR0: --burst-write requires --write OFFSET --value VALUE\n");
        return 2;
    }

    if (!do_status && !do_start && !do_stop && !do_regs && !do_scratch &&
        !do_control && !do_read && !do_write && burst_write_count == 0U && poll_count == 0) {
        do_status = 1;
    }
    if (do_start && poll_count == 0) {
        poll_count = 30;
    }
    if (poll_count == 0 && do_status) {
        poll_count = 1;
    }

    fd = open(PCIE_DRIVER_FILE_PATH, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "PCIe: open %s failed: %s\n", PCIE_DRIVER_FILE_PATH, strerror(errno));
        return 1;
    }

    print_device_info(fd);
    if (map_bar0(fd, &bar) < 0) {
        close(fd);
        return 1;
    }

    if (do_stop) {
        printf("write BAR0+0x000 stop command 0x%08x\n", FPGA_CMD_STOP_CAPTURE);
        bar0_write32(&bar, FPGA_REG_LEGACY_CONTROL, FPGA_CMD_STOP_CAPTURE);
    }

    if (do_control) {
        printf("write BAR0+0x130 capture control 0x%08x\n", control_value);
        bar0_write32(&bar, FPGA_REG_CAPTURE_CTRL, control_value);
        print_reg32(&bar, "capture_ctrl", FPGA_REG_CAPTURE_CTRL);
        print_reg32(&bar, "ctrl_status", FPGA_REG_CTRL_STATUS);
    }

    if (do_scratch) {
        printf("write BAR0+0x120 scratch 0x%08x\n", scratch_value);
        bar0_write32(&bar, FPGA_REG_SCRATCH, scratch_value);
        print_reg32(&bar, "scratch", FPGA_REG_SCRATCH);
        print_reg32(&bar, "ctrl_status", FPGA_REG_CTRL_STATUS);
    }

    if (do_write) {
        if (burst_write_count != 0U) {
            printf("burst write BAR0+0x%03x 0x%08x count=%u\n",
                   write_offset, write_value, burst_write_count);
            for (i = 0; i < burst_write_count; ++i) {
                bar0_write32(&bar, write_offset, write_value + i);
            }
        } else {
            printf("write BAR0+0x%03x 0x%08x\n", write_offset, write_value);
            bar0_write32(&bar, write_offset, write_value);
        }
        print_reg32(&bar, "readback", write_offset);
    }

    if (do_read) {
        print_reg32(&bar, "read", read_offset);
    }

    if (do_start) {
        if (configure_dma_for_capture(fd, &dma_operation) < 0) {
            unmap_bar0(&bar);
            close(fd);
            return 1;
        }
        dma_mapped = 1;
        printf("write BAR0+0x000 start command 0x%08x\n", FPGA_CMD_START_CAPTURE);
        bar0_write32(&bar, FPGA_REG_LEGACY_CONTROL, FPGA_CMD_START_CAPTURE);
    }

    for (i = 0; i < poll_count; ++i) {
        print_frame_status(&bar, i);
        if (i + 1U < poll_count) {
            sleep_ms(delay_ms);
        }
    }

    if (do_regs) {
        print_control_regs(&bar);
    }

    if (do_start && !no_stop) {
        printf("write BAR0+0x000 stop command 0x%08x\n", FPGA_CMD_STOP_CAPTURE);
        bar0_write32(&bar, FPGA_REG_LEGACY_CONTROL, FPGA_CMD_STOP_CAPTURE);
    }

    if (dma_mapped && !no_stop) {
        if (ioctl(fd, PCI_UMAP_ADDR_CMD, &dma_operation) < 0) {
            fprintf(stderr, "PCIe: DMA address unmap failed: %s\n", strerror(errno));
        }
    }

    unmap_bar0(&bar);
    close(fd);
    return 0;
}
