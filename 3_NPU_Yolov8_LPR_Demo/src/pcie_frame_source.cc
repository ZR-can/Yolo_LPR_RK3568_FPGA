#include "pcie_frame_source.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "pango_pci.h"

namespace {

const uint32_t kStartCaptureCommand = 0xffffffe5U;
const uint32_t kStopCaptureCommand = 0xffffff00U;
const unsigned int kDmaLengthDwords = 3840U >> 2;

}  // namespace

struct PcieFrameSource::Impl {
    int fd;
    void* bar_mapping;
    size_t bar_mapping_length;
    volatile uint32_t* pio_address;
    bool dma_mapped;
    bool capture_started;
    bool reported_status_semantics;
    COMMAND_OPERATION device_command;
    COMMAND_OPERATION bar_command;
    DMA_OPERATION dma_operation;
    PcieDeviceInfo device_info;
    PcieReadStatistics statistics;
    long long last_driver_status;

    Impl()
        : fd(-1),
          bar_mapping(MAP_FAILED),
          bar_mapping_length(0),
          pio_address(nullptr),
          dma_mapped(false),
          capture_started(false),
          reported_status_semantics(false),
          last_driver_status(0) {
        memset(&device_command, 0, sizeof(device_command));
        memset(&bar_command, 0, sizeof(bar_command));
        memset(&dma_operation, 0, sizeof(dma_operation));
        memset(&device_info, 0, sizeof(device_info));
        memset(&statistics, 0, sizeof(statistics));
    }
};

constexpr int PcieFrameSource::kFrameWidth;
constexpr int PcieFrameSource::kFrameHeight;
constexpr int PcieFrameSource::kBytesPerPixel;
constexpr size_t PcieFrameSource::kFrameBytes;

PcieFrameSource::PcieFrameSource() : impl_(new Impl()) {}

PcieFrameSource::~PcieFrameSource() {
    Close();
    delete impl_;
}

int PcieFrameSource::Open() {
    if (impl_ == nullptr) {
        return -1;
    }
    if (impl_->fd >= 0) {
        return 0;
    }

    impl_->fd = open(PCIE_DRIVER_FILE_PATH, O_RDWR);
    if (impl_->fd < 0) {
        fprintf(stderr, "PCIe: open %s failed: %s\n", PCIE_DRIVER_FILE_PATH, strerror(errno));
        return -1;
    }

    if (ioctl(impl_->fd, PCI_READ_DATA_CMD, &impl_->device_command) < 0) {
        fprintf(stderr, "PCIe: reading device information failed: %s\n", strerror(errno));
        Close();
        return -1;
    }
    const PCI_DEVICE_INFO& pci = impl_->device_command.get_pci_dev_info;
    impl_->device_info.vendor_id = pci.vendor_id;
    impl_->device_info.device_id = pci.device_id;
    impl_->device_info.link_speed = pci.link_speed;
    impl_->device_info.link_width = pci.link_width;
    impl_->device_info.max_payload_size = pci.mps;

    if (ioctl(impl_->fd, PCI_SET_CONFIG, &impl_->dma_operation) < 0) {
        fprintf(stderr, "PCIe: DMA configuration failed: %s\n", strerror(errno));
        Close();
        return -1;
    }
    if (ioctl(impl_->fd, PCI_MAP_BAR0_CMD, &impl_->bar_command) < 0) {
        fprintf(stderr, "PCIe: BAR0 query failed: %s\n", strerror(errno));
        Close();
        return -1;
    }

    const unsigned long bar_address = impl_->bar_command.get_pci_dev_info.bar[0].bar_base;
    const long page_size = sysconf(_SC_PAGESIZE);
    if (bar_address == 0 || page_size <= 0) {
        fprintf(stderr, "PCIe: invalid BAR0 address or page size\n");
        Close();
        return -1;
    }
    const unsigned long page_mask = (unsigned long)page_size - 1UL;
    const unsigned long page_base = bar_address & ~page_mask;
    const size_t page_offset = (size_t)(bar_address - page_base);
    impl_->bar_mapping_length =
        (page_offset + BAR0_MAX + (size_t)page_size - 1U) & ~((size_t)page_size - 1U);

    const int mem_fd = open(MEM_FILE_PATH, O_RDWR | O_SYNC);
    if (mem_fd < 0) {
        fprintf(stderr, "PCIe: open %s failed: %s\n", MEM_FILE_PATH, strerror(errno));
        Close();
        return -1;
    }
    impl_->bar_mapping = mmap(nullptr, impl_->bar_mapping_length, PROT_READ | PROT_WRITE,
                              MAP_SHARED, mem_fd, (off_t)page_base);
    const int mmap_errno = errno;
    close(mem_fd);
    if (impl_->bar_mapping == MAP_FAILED) {
        errno = mmap_errno;
        fprintf(stderr, "PCIe: BAR0 mmap failed: %s\n", strerror(errno));
        Close();
        return -1;
    }
    impl_->pio_address = reinterpret_cast<volatile uint32_t*>(
        static_cast<unsigned char*>(impl_->bar_mapping) + page_offset);

    impl_->dma_operation.current_len = kDmaLengthDwords;
    impl_->dma_operation.offset_addr = 0;
    if (ioctl(impl_->fd, PCI_MAP_ADDR_CMD, &impl_->dma_operation) < 0) {
        fprintf(stderr, "PCIe: DMA address mapping failed: %s\n", strerror(errno));
        Close();
        return -1;
    }
    impl_->dma_mapped = true;

    *impl_->pio_address = kStartCaptureCommand;
    __sync_synchronize();
    impl_->capture_started = true;

    fprintf(stderr, "PCIe: vendor=0x%04x device=0x%04x link=gen%u x%u mps=%u\n",
            impl_->device_info.vendor_id, impl_->device_info.device_id,
            impl_->device_info.link_speed, impl_->device_info.link_width,
            impl_->device_info.max_payload_size);
    fprintf(stderr, "PCIe: capture started, BGR565 %dx%d (%zu bytes/frame)\n",
            kFrameWidth, kFrameHeight, kFrameBytes);
    return 0;
}

PcieFrameReadResult PcieFrameSource::ReadFrame(unsigned char* frame, size_t frame_size) {
    if (impl_ == nullptr || impl_->fd < 0 || frame == nullptr || frame_size != kFrameBytes) {
        return PCIE_FRAME_FATAL;
    }

    errno = 0;
    const ssize_t status = read(impl_->fd, frame, frame_size);
    impl_->last_driver_status = status;
    if (status > 0) {
        ++impl_->statistics.frames_ready;
        if (!impl_->reported_status_semantics) {
            fprintf(stderr,
                    "PCIe: read returned driver status %zd; accepting the DMA buffer as one complete frame\n",
                    status);
            impl_->reported_status_semantics = true;
        }
        return PCIE_FRAME_READY;
    }
    if (status == 0) {
        ++impl_->statistics.zero_status_retries;
        return PCIE_FRAME_RETRY;
    }

    if (errno == EINTR || errno == EAGAIN) {
        ++impl_->statistics.interrupted_retries;
        return PCIE_FRAME_RETRY;
    }
    if (errno == EPERM) {
        ++impl_->statistics.permission_retries;
        return PCIE_FRAME_RETRY;
    }

    ++impl_->statistics.other_errors;
    fprintf(stderr, "PCIe: frame read failed: %s\n", strerror(errno));
    return PCIE_FRAME_FATAL;
}

void PcieFrameSource::Close() {
    if (impl_ == nullptr) {
        return;
    }
    if (impl_->capture_started && impl_->pio_address != nullptr) {
        *impl_->pio_address = kStopCaptureCommand;
        __sync_synchronize();
        impl_->capture_started = false;
    }
    if (impl_->dma_mapped && impl_->fd >= 0) {
        if (ioctl(impl_->fd, PCI_UMAP_ADDR_CMD, &impl_->dma_operation) < 0) {
            fprintf(stderr, "PCIe: DMA address unmap failed: %s\n", strerror(errno));
        }
        impl_->dma_mapped = false;
    }
    if (impl_->bar_mapping != MAP_FAILED) {
        munmap(impl_->bar_mapping, impl_->bar_mapping_length);
        impl_->bar_mapping = MAP_FAILED;
        impl_->bar_mapping_length = 0;
        impl_->pio_address = nullptr;
    }
    if (impl_->fd >= 0) {
        close(impl_->fd);
        impl_->fd = -1;
    }
}

bool PcieFrameSource::IsOpen() const {
    return impl_ != nullptr && impl_->fd >= 0;
}

long long PcieFrameSource::LastDriverStatus() const {
    return impl_ == nullptr ? -1 : impl_->last_driver_status;
}

const PcieDeviceInfo& PcieFrameSource::DeviceInfo() const {
    return impl_->device_info;
}

const PcieReadStatistics& PcieFrameSource::Statistics() const {
    return impl_->statistics;
}
