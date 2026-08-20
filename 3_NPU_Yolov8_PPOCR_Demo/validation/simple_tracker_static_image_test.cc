#include <algorithm>
#include <climits>
#include <cstdio>
#include <vector>

#include "simple_tracker.h"

namespace {

PipelineResult MakeDetection(int left, int top, int right, int bottom) {
    PipelineResult detection;
    detection.left = left;
    detection.top = top;
    detection.right = right;
    detection.bottom = bottom;
    detection.confidence = 0.9f;
    detection.text_confidence = 0.9f;
    detection.plate_name = "京A12345";
    detection.plate_type = "蓝";
    return detection;
}

}  // namespace

int main() {
    SimplePlateTracker tracker;
    const int jitter[] = {0, 6, -5, 5, -6, 4, -4, 3, -3, 2, -2, 0};
    int minimum_left = INT_MAX;
    int maximum_left = INT_MIN;
    for (size_t index = 0; index < sizeof(jitter) / sizeof(jitter[0]); ++index) {
        const int offset = jitter[index];
        std::vector<PipelineResult> detections;
        detections.push_back(
            MakeDetection(100 + offset, 100, 220 + offset, 140));
        detections.push_back(
            MakeDetection(101 + offset, 101, 221 + offset, 141));
        tracker.update(detections, static_cast<int>(index) * 12, true);

        if (index == 0U) {
            continue;
        }
        std::vector<PipelineResult> tracked;
        tracker.predict(static_cast<int>(index) * 12, tracked, true);
        if (tracked.size() != 1U) {
            std::fprintf(stderr,
                         "expected one static track, got %zu\n",
                         tracked.size());
            return 1;
        }
        minimum_left = std::min(minimum_left, tracked[0].left);
        maximum_left = std::max(maximum_left, tracked[0].left);
    }

    std::vector<PipelineResult> tracked;
    tracker.predict(400, tracked, true);
    if (tracked.size() != 1U || maximum_left - minimum_left > 2) {
        std::fprintf(stderr,
                     "static stability failed: tracks=%zu left_range=%d\n",
                     tracked.size(),
                     maximum_left - minimum_left);
        return 1;
    }

    std::printf("PASS static_tracks=1 left_range=%d\n",
                maximum_left - minimum_left);
    return 0;
}
