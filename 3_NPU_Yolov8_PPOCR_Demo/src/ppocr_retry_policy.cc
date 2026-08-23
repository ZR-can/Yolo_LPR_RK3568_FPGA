#include "ppocr_retry_policy.h"

#include <algorithm>
#include <cmath>

#include "plate_rule.h"

namespace {

const double kRetryExpansionRatio = 0.05;
const float kStableVoteOverlapThreshold = 0.50f;

float RectIou(const image_rect_t& first, const PipelineResult& second) {
    const int left = std::max(first.left, second.left);
    const int top = std::max(first.top, second.top);
    const int right = std::min(first.right, second.right);
    const int bottom = std::min(first.bottom, second.bottom);
    if (right <= left || bottom <= top) {
        return 0.0f;
    }

    const float intersection = static_cast<float>(
        (right - left) * (bottom - top));
    const float first_area = static_cast<float>(
        std::max(0, first.right - first.left) *
        std::max(0, first.bottom - first.top));
    const float second_area = static_cast<float>(
        std::max(0, second.right - second.left) *
        std::max(0, second.bottom - second.top));
    const float union_area = first_area + second_area - intersection;
    return union_area > 0.0f ? intersection / union_area : 0.0f;
}

}  // namespace

image_rect_t clamp_ppocr_roi(const image_rect_t& roi,
                             int image_width,
                             int image_height) {
    image_rect_t clamped = {0, 0, 0, 0};
    if (image_width <= 0 || image_height <= 0) {
        return clamped;
    }

    clamped.left = std::max(0, std::min(roi.left, image_width));
    clamped.top = std::max(0, std::min(roi.top, image_height));
    clamped.right = std::max(0, std::min(roi.right, image_width));
    clamped.bottom = std::max(0, std::min(roi.bottom, image_height));
    return clamped;
}

image_rect_t expand_ppocr_retry_roi(const image_rect_t& roi,
                                    int image_width,
                                    int image_height) {
    const image_rect_t clamped =
        clamp_ppocr_roi(roi, image_width, image_height);
    if (clamped.right <= clamped.left || clamped.bottom <= clamped.top) {
        return clamped;
    }

    const int width = clamped.right - clamped.left;
    const int height = clamped.bottom - clamped.top;
    const int margin_x = std::max(
        1, static_cast<int>(std::ceil(width * kRetryExpansionRatio)));
    const int margin_y = std::max(
        1, static_cast<int>(std::ceil(height * kRetryExpansionRatio)));

    image_rect_t expanded;
    expanded.left = clamped.left - margin_x;
    expanded.top = clamped.top - margin_y;
    expanded.right = clamped.right + margin_x;
    expanded.bottom = clamped.bottom + margin_y;
    return clamp_ppocr_roi(expanded, image_width, image_height);
}

bool ppocr_roi_equal(const image_rect_t& lhs, const image_rect_t& rhs) {
    return lhs.left == rhs.left && lhs.top == rhs.top &&
           lhs.right == rhs.right && lhs.bottom == rhs.bottom;
}

bool should_retry_ppocr_plate(const std::string& plate,
                              bool is_green_plate) {
    return !is_valid_ga36_plate(plate, is_green_plate ? "绿" : "蓝");
}

bool should_suppress_static_image_retry(
    const image_rect_t& roi,
    const std::vector<PipelineResult>& stable_vote_results) {
    for (const PipelineResult& stable : stable_vote_results) {
        if (stable.has_valid_plate_text &&
            RectIou(roi, stable) >= kStableVoteOverlapThreshold) {
            return true;
        }
    }
    return false;
}
