#include <cstdio>
#include <string>

#include "ppocr_retry_policy.h"

namespace {

int CheckRoi(const char* name,
             const image_rect_t& actual,
             int left,
             int top,
             int right,
             int bottom) {
    if (actual.left == left && actual.top == top &&
        actual.right == right && actual.bottom == bottom) {
        return 0;
    }
    std::fprintf(stderr,
                 "%s failed: expected=(%d,%d,%d,%d), actual=(%d,%d,%d,%d)\n",
                 name,
                 left,
                 top,
                 right,
                 bottom,
                 actual.left,
                 actual.top,
                 actual.right,
                 actual.bottom);
    return 1;
}

int CheckBool(const char* name, bool actual, bool expected) {
    if (actual == expected) {
        return 0;
    }
    std::fprintf(stderr,
                 "%s failed: expected=%d, actual=%d\n",
                 name,
                 expected ? 1 : 0,
                 actual ? 1 : 0);
    return 1;
}

}  // namespace

int main() {
    int failures = 0;

    image_rect_t full_frame = {-2, -3, 1922, 1082};
    failures += CheckRoi("half-open frame clamp",
                         clamp_ppocr_roi(full_frame, 1920, 1080),
                         0,
                         0,
                         1920,
                         1080);

    image_rect_t center = {100, 200, 200, 240};
    failures += CheckRoi("five-percent expansion",
                         expand_ppocr_retry_roi(center, 1920, 1080),
                         95,
                         198,
                         205,
                         242);

    image_rect_t edge = {0, 0, 40, 20};
    failures += CheckRoi("expansion clamps to frame",
                         expand_ppocr_retry_roi(edge, 1920, 1080),
                         0,
                         0,
                         42,
                         21);

    image_rect_t outside = {2000, 1200, 2100, 1300};
    failures += CheckRoi("outside roi remains empty",
                         clamp_ppocr_roi(outside, 1920, 1080),
                         1920,
                         1080,
                         1920,
                         1080);

    failures += CheckBool("same roi",
                          ppocr_roi_equal(center, center),
                          true);
    failures += CheckBool("different roi",
                          ppocr_roi_equal(center, edge),
                          false);
    failures += CheckBool("valid blue does not retry",
                          should_retry_ppocr_plate("京A12345", false),
                          false);
    failures += CheckBool("invalid blue retries",
                          should_retry_ppocr_plate("京A1234", false),
                          true);
    failures += CheckBool("valid green does not retry",
                          should_retry_ppocr_plate("粤AD12345", true),
                          false);
    failures += CheckBool("green missing marker retries",
                          should_retry_ppocr_plate("粤A123456", true),
                          true);

    if (failures == 0) {
        std::printf("ppocr_retry_policy_test: all cases passed\n");
    }
    return failures == 0 ? 0 : 1;
}
