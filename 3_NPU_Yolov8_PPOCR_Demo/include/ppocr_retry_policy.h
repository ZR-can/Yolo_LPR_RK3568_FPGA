#ifndef PPOCR_RETRY_POLICY_H
#define PPOCR_RETRY_POLICY_H

#include <string>

#include "common.h"

// ROI coordinates use half-open intervals: [left, right) and [top, bottom).
image_rect_t clamp_ppocr_roi(const image_rect_t& roi,
                             int image_width,
                             int image_height);

// Expand every edge by 5% of the original ROI size, with a minimum of one
// pixel, then clamp the result to the source image.
image_rect_t expand_ppocr_retry_roi(const image_rect_t& roi,
                                    int image_width,
                                    int image_height);

bool ppocr_roi_equal(const image_rect_t& lhs, const image_rect_t& rhs);

// The primary result is retried only when the current GA 36 v3 validator would
// reject it. A valid primary result must never be replaced by the retry path.
bool should_retry_ppocr_plate(const std::string& plate, bool is_green_plate);

#endif  // PPOCR_RETRY_POLICY_H
