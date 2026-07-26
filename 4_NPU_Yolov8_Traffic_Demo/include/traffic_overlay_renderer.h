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
    struct BlendSpan {
        int y;
        int left;
        int right;
    };

    int BuildOverlay(const TrafficFrameAnalysis& analysis, bool include_roi_fill);
    void BuildRoiSpans();
    void FillRoiOverlay();
    void RebuildBlendSpans();
    bool BlendRoi(image_buffer_t* target) const;
    bool BlendOverlay(image_buffer_t* target) const;

    int width_ = 0;
    int height_ = 0;
    int cached_frame_id_ = -2;
    bool cached_with_roi_fill_ = false;
    std::vector<TrafficPixelPoint> roi_polygon_;
    std::vector<BlendSpan> roi_spans_;
    std::vector<unsigned char> overlay_;
    std::vector<BlendSpan> blend_spans_;
};

#endif  // TRAFFIC_OVERLAY_RENDERER_H
