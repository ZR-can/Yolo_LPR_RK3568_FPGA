#ifndef TRAFFIC_OVERLAY_RENDERER_H
#define TRAFFIC_OVERLAY_RENDERER_H

#include <vector>

#include "common.h"
#include "traffic_violation.h"

class TrafficOverlayRenderer {
public:
    int Init(int width, int height, const TrafficRoiConfig& roi);
    int Render(image_buffer_t* ui_buffer, const TrafficFrameAnalysis& analysis);

private:
    int BuildOverlay(const TrafficFrameAnalysis& analysis);
    bool BlendOverlay(image_buffer_t* target) const;

    int width_ = 0;
    int height_ = 0;
    int cached_frame_id_ = -2;
    TrafficRoiConfig roi_;
    std::vector<unsigned char> overlay_;
};

#endif  // TRAFFIC_OVERLAY_RENDERER_H
