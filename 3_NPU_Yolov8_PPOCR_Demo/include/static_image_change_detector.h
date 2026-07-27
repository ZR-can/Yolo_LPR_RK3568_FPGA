#ifndef STATIC_IMAGE_CHANGE_DETECTOR_H
#define STATIC_IMAGE_CHANGE_DETECTOR_H

#include <cstddef>
#include <cstdint>
#include <vector>

// Assigns a monotonically increasing generation to a static BGR565 source.
// Sparse color-distance sampling keeps the capture path inexpensive while
// allowing a newly selected still image to invalidate results from the prior
// image before the next inference finishes.
class StaticImageChangeDetector {
public:
    StaticImageChangeDetector();

    uint64_t ObserveBgr565(const unsigned char* pixels,
                           size_t size,
                           int width,
                           int height,
                           int stride_pixels);

private:
    std::vector<uint16_t> reference_samples_;
    std::vector<uint16_t> current_samples_;
    std::vector<uint16_t> pending_samples_;
    int pending_hits_;
    uint64_t generation_;
};

#endif  // STATIC_IMAGE_CHANGE_DETECTOR_H
