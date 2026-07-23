#ifndef PCIE_FRAME_SOURCE_H
#define PCIE_FRAME_SOURCE_H

#include <stddef.h>
#include <stdint.h>

struct PcieDeviceInfo {
    unsigned int vendor_id;
    unsigned int device_id;
    unsigned int link_speed;
    unsigned int link_width;
    unsigned int max_payload_size;
};

struct PcieReadStatistics {
    uint64_t frames_ready;
    uint64_t zero_status_retries;
    uint64_t permission_retries;
    uint64_t interrupted_retries;
    uint64_t other_errors;
};

enum PcieFrameReadResult {
    PCIE_FRAME_FATAL = -1,
    PCIE_FRAME_RETRY = 0,
    PCIE_FRAME_READY = 1,
};

class PcieFrameSource {
public:
    static constexpr int kFrameWidth = 1280;
    static constexpr int kFrameHeight = 720;
    static constexpr int kBytesPerPixel = 2;
    static constexpr size_t kFrameBytes =
        (size_t)kFrameWidth * kFrameHeight * kBytesPerPixel;

    PcieFrameSource();
    ~PcieFrameSource();

    int Open();
    PcieFrameReadResult ReadFrame(unsigned char* frame, size_t frame_size);
    void Close();

    bool IsOpen() const;
    long long LastDriverStatus() const;
    const PcieDeviceInfo& DeviceInfo() const;
    const PcieReadStatistics& Statistics() const;

private:
    struct Impl;
    Impl* impl_;

    PcieFrameSource(const PcieFrameSource&) = delete;
    PcieFrameSource& operator=(const PcieFrameSource&) = delete;
};

#endif  // PCIE_FRAME_SOURCE_H
