#ifndef RGA_OVERLAY_RENDERER_H
#define RGA_OVERLAY_RENDERER_H

#include <string>
#include <unordered_map>
#include <vector>

#include "common.h"
#include "yolo_lpr_pipeline.h"

class RgaOverlayRenderer {
public:
    int Init(int width, int height);
    int Render(image_buffer_t* ui_buffer, const std::vector<PipelineResult>& results,
               bool clear_background = true);

private:
    struct LabelSprite {
        int width = 0;
        int height = 0;
        std::vector<unsigned char> pixels;
    };

    const LabelSprite& GetLabelSprite(const PipelineResult& result);
    LabelSprite BuildLabelSprite(const PipelineResult& result) const;
    bool BlendSprite(const LabelSprite& sprite, image_buffer_t* target, int x, int y) const;

    int width_ = 0;
    int height_ = 0;
    std::unordered_map<std::string, LabelSprite> label_cache_;
};

#endif  // RGA_OVERLAY_RENDERER_H
