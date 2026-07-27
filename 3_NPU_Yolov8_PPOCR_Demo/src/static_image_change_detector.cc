#include "static_image_change_detector.h"

#include <algorithm>
#include <cstdlib>

namespace {

const int kSampleStep = 16;
const int kSampleBlockSize = 2;
const int kSignificantColorDistance = 30;
const size_t kMinChangedSamples = 12U;
const size_t kChangedSampleDivisor = 500U;
const int kRequiredConsecutiveFrames = 2;

int Bgr565ColorDistance(uint16_t lhs, uint16_t rhs) {
    const int lhs_b = lhs & 0x1F;
    const int lhs_g = (lhs >> 5) & 0x3F;
    const int lhs_r = (lhs >> 11) & 0x1F;
    const int rhs_b = rhs & 0x1F;
    const int rhs_g = (rhs >> 5) & 0x3F;
    const int rhs_r = (rhs >> 11) & 0x1F;
    return std::abs(lhs_b - rhs_b) +
           std::abs(lhs_g - rhs_g) +
           std::abs(lhs_r - rhs_r);
}

uint16_t AverageBgr565Block(const unsigned char* pixels,
                            int width,
                            int height,
                            int stride_pixels,
                            int block_left,
                            int block_top) {
    const int block_right =
        std::min(width, block_left + kSampleBlockSize);
    const int block_bottom =
        std::min(height, block_top + kSampleBlockSize);
    unsigned int blue_sum = 0U;
    unsigned int green_sum = 0U;
    unsigned int red_sum = 0U;
    unsigned int pixel_count = 0U;
    for (int y = block_top; y < block_bottom; ++y) {
        for (int x = block_left; x < block_right; ++x) {
            const size_t byte_offset =
                (static_cast<size_t>(y) * static_cast<size_t>(stride_pixels) +
                 static_cast<size_t>(x)) *
                2U;
            const uint16_t pixel =
                static_cast<uint16_t>(pixels[byte_offset]) |
                static_cast<uint16_t>(
                    static_cast<uint16_t>(pixels[byte_offset + 1U]) << 8);
            blue_sum += pixel & 0x1FU;
            green_sum += (pixel >> 5) & 0x3FU;
            red_sum += (pixel >> 11) & 0x1FU;
            ++pixel_count;
        }
    }
    if (pixel_count == 0U) {
        return 0U;
    }
    const uint16_t blue =
        static_cast<uint16_t>(blue_sum / pixel_count);
    const uint16_t green =
        static_cast<uint16_t>(green_sum / pixel_count);
    const uint16_t red =
        static_cast<uint16_t>(red_sum / pixel_count);
    return static_cast<uint16_t>((red << 11) | (green << 5) | blue);
}

size_t RequiredChangedSamples(size_t sample_count) {
    return std::min(
        sample_count,
        std::max(kMinChangedSamples,
                 sample_count / kChangedSampleDivisor));
}

bool SamplesDiffer(const std::vector<uint16_t>& lhs,
                   const std::vector<uint16_t>& rhs) {
    if (lhs.size() != rhs.size() || lhs.empty()) {
        return true;
    }
    const size_t required_changes = RequiredChangedSamples(lhs.size());
    size_t changed_samples = 0U;
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (Bgr565ColorDistance(lhs[i], rhs[i]) >=
            kSignificantColorDistance) {
            ++changed_samples;
            if (changed_samples >= required_changes) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace

StaticImageChangeDetector::StaticImageChangeDetector()
    : pending_hits_(0),
      generation_(0) {}

uint64_t StaticImageChangeDetector::ObserveBgr565(
    const unsigned char* pixels,
    size_t size,
    int width,
    int height,
    int stride_pixels) {
    if (pixels == nullptr || width <= 0 || height <= 0 ||
        stride_pixels < width) {
        return generation_;
    }
    const size_t required_pixels =
        static_cast<size_t>(stride_pixels) * static_cast<size_t>(height);
    if (required_pixels > size / 2U) {
        return generation_;
    }

    current_samples_.clear();
    current_samples_.reserve(
        static_cast<size_t>((width + kSampleStep - 1) / kSampleStep) *
        static_cast<size_t>((height + kSampleStep - 1) / kSampleStep));
    for (int y = 0; y < height; y += kSampleStep) {
        for (int x = 0; x < width; x += kSampleStep) {
            current_samples_.push_back(AverageBgr565Block(
                pixels, width, height, stride_pixels, x, y));
        }
    }
    if (current_samples_.empty()) {
        return generation_;
    }

    if (reference_samples_.size() != current_samples_.size()) {
        reference_samples_.swap(current_samples_);
        pending_samples_.clear();
        pending_hits_ = 0;
        ++generation_;
        return generation_;
    }

    if (!SamplesDiffer(reference_samples_, current_samples_)) {
        pending_samples_.clear();
        pending_hits_ = 0;
        return generation_;
    }

    if (!pending_samples_.empty() &&
        !SamplesDiffer(pending_samples_, current_samples_)) {
        ++pending_hits_;
    } else {
        pending_samples_ = current_samples_;
        pending_hits_ = 1;
    }
    if (pending_hits_ >= kRequiredConsecutiveFrames) {
        reference_samples_.swap(pending_samples_);
        pending_samples_.clear();
        pending_hits_ = 0;
        ++generation_;
    }
    return generation_;
}
