#include "traffic_overlay_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "image_drawing.h"
#include "im2d.h"

namespace {

constexpr int kPanelRadius = 8;
constexpr int kBoxThickness = 4;
constexpr int kLabelFontSize = 18;
constexpr int kStatusFontSize = 28;

int Clamp(int value, int low, int high) {
    return std::max(low, std::min(value, high));
}

void BlendPixel(unsigned char* pixel,
                unsigned char red,
                unsigned char green,
                unsigned char blue,
                unsigned char alpha) {
    const unsigned int dst_alpha = pixel[3];
    const unsigned int out_alpha = alpha + (dst_alpha * (255 - alpha) + 127) / 255;
    if (out_alpha == 0) {
        return;
    }
    const unsigned int src_factor = alpha * 255;
    const unsigned int dst_factor = dst_alpha * (255 - alpha);
    pixel[0] = static_cast<unsigned char>(
        (red * src_factor + pixel[0] * dst_factor) / (out_alpha * 255));
    pixel[1] = static_cast<unsigned char>(
        (green * src_factor + pixel[1] * dst_factor) / (out_alpha * 255));
    pixel[2] = static_cast<unsigned char>(
        (blue * src_factor + pixel[2] * dst_factor) / (out_alpha * 255));
    pixel[3] = static_cast<unsigned char>(out_alpha);
}

void PutPixel(std::vector<unsigned char>* pixels,
              int width,
              int height,
              int x,
              int y,
              unsigned char red,
              unsigned char green,
              unsigned char blue,
              unsigned char alpha) {
    if (x < 0 || x >= width || y < 0 || y >= height) {
        return;
    }
    BlendPixel(pixels->data() + (y * width + x) * 4, red, green, blue, alpha);
}

void FillRoundedRect(std::vector<unsigned char>* pixels,
                     int image_width,
                     int image_height,
                     int x,
                     int y,
                     int width,
                     int height,
                     int radius,
                     unsigned char red,
                     unsigned char green,
                     unsigned char blue,
                     unsigned char alpha) {
    if (width <= 0 || height <= 0) {
        return;
    }
    radius = std::max(0, std::min(radius, std::min(width, height) / 2));
    const int radius_squared = radius * radius;
    for (int py = 0; py < height; ++py) {
        for (int px = 0; px < width; ++px) {
            int nearest_x = px;
            int nearest_y = py;
            if (px < radius) {
                nearest_x = radius;
            } else if (px >= width - radius) {
                nearest_x = width - radius - 1;
            }
            if (py < radius) {
                nearest_y = radius;
            } else if (py >= height - radius) {
                nearest_y = height - radius - 1;
            }
            const int dx = px - nearest_x;
            const int dy = py - nearest_y;
            if (dx * dx + dy * dy <= radius_squared) {
                PutPixel(pixels, image_width, image_height, x + px, y + py,
                         red, green, blue, alpha);
            }
        }
    }
}

void FillPolygon(std::vector<unsigned char>* pixels,
                 int width,
                 int height,
                 const std::vector<TrafficPixelPoint>& polygon,
                 unsigned char red,
                 unsigned char green,
                 unsigned char blue,
                 unsigned char alpha) {
    if (polygon.size() < 3) {
        return;
    }
    int min_y = height - 1;
    int max_y = 0;
    for (const TrafficPixelPoint& point : polygon) {
        min_y = std::min(min_y, point.y);
        max_y = std::max(max_y, point.y);
    }
    min_y = Clamp(min_y, 0, height - 1);
    max_y = Clamp(max_y, 0, height - 1);
    std::vector<float> intersections;
    intersections.reserve(polygon.size());
    for (int y = min_y; y <= max_y; ++y) {
        intersections.clear();
        for (size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++) {
            const TrafficPixelPoint& first = polygon[j];
            const TrafficPixelPoint& second = polygon[i];
            if ((first.y > y) == (second.y > y)) {
                continue;
            }
            const float x = first.x + (static_cast<float>(y - first.y) *
                                       static_cast<float>(second.x - first.x)) /
                                          static_cast<float>(second.y - first.y);
            intersections.push_back(x);
        }
        std::sort(intersections.begin(), intersections.end());
        for (size_t i = 0; i + 1 < intersections.size(); i += 2) {
            const int left = Clamp(static_cast<int>(std::ceil(intersections[i])), 0, width - 1);
            const int right = Clamp(static_cast<int>(std::floor(intersections[i + 1])), 0, width - 1);
            for (int x = left; x <= right; ++x) {
                PutPixel(pixels, width, height, x, y, red, green, blue, alpha);
            }
        }
    }
}

void DrawDisk(std::vector<unsigned char>* pixels,
              int width,
              int height,
              int center_x,
              int center_y,
              int radius,
              unsigned char red,
              unsigned char green,
              unsigned char blue,
              unsigned char alpha) {
    for (int y = -radius; y <= radius; ++y) {
        for (int x = -radius; x <= radius; ++x) {
            if (x * x + y * y <= radius * radius) {
                PutPixel(pixels, width, height, center_x + x, center_y + y,
                         red, green, blue, alpha);
            }
        }
    }
}

void DrawLine(std::vector<unsigned char>* pixels,
              int width,
              int height,
              int x0,
              int y0,
              int x1,
              int y1,
              int thickness,
              unsigned char red,
              unsigned char green,
              unsigned char blue,
              unsigned char alpha) {
    const int dx = std::abs(x1 - x0);
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0);
    const int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    while (true) {
        DrawDisk(pixels, width, height, x0, y0, std::max(1, thickness / 2),
                 red, green, blue, alpha);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int doubled = 2 * error;
        if (doubled >= dy) {
            error += dy;
            x0 += sx;
        }
        if (doubled <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

void DrawBox(std::vector<unsigned char>* pixels,
             int width,
             int height,
             int left,
             int top,
             int right,
             int bottom,
             unsigned char red,
             unsigned char green,
             unsigned char blue) {
    DrawLine(pixels, width, height, left - 2, top - 2, right + 2, top - 2,
             2, 0, 0, 0, 220);
    DrawLine(pixels, width, height, right + 2, top - 2, right + 2, bottom + 2,
             2, 0, 0, 0, 220);
    DrawLine(pixels, width, height, right + 2, bottom + 2, left - 2, bottom + 2,
             2, 0, 0, 0, 220);
    DrawLine(pixels, width, height, left - 2, bottom + 2, left - 2, top - 2,
             2, 0, 0, 0, 220);
    DrawLine(pixels, width, height, left, top, right, top,
             kBoxThickness, red, green, blue, 255);
    DrawLine(pixels, width, height, right, top, right, bottom,
             kBoxThickness, red, green, blue, 255);
    DrawLine(pixels, width, height, right, bottom, left, bottom,
             kBoxThickness, red, green, blue, 255);
    DrawLine(pixels, width, height, left, bottom, left, top,
             kBoxThickness, red, green, blue, 255);
}

int TextWidth(const char* text, int font_size) {
    return static_cast<int>(std::strlen(text)) * (font_size / 2 + 2);
}

void DrawPanelText(std::vector<unsigned char>* pixels,
                   int width,
                   int height,
                   int x,
                   int y,
                   const char* text,
                   int font_size,
                   unsigned char red,
                   unsigned char green,
                   unsigned char blue,
                   unsigned char panel_red = 32,
                   unsigned char panel_green = 32,
                   unsigned char panel_blue = 32,
                   unsigned char panel_alpha = 210) {
    const int panel_width = std::min(width, TextWidth(text, font_size) + 18);
    const int panel_height = font_size * 2 + 8;
    x = Clamp(x, 0, std::max(0, width - panel_width));
    y = Clamp(y, 0, std::max(0, height - panel_height));
    FillRoundedRect(pixels, width, height, x, y, panel_width, panel_height,
                    kPanelRadius, panel_red, panel_green, panel_blue, panel_alpha);

    image_buffer_t overlay_image;
    std::memset(&overlay_image, 0, sizeof(overlay_image));
    overlay_image.width = width;
    overlay_image.height = height;
    overlay_image.width_stride = width;
    overlay_image.height_stride = height;
    overlay_image.format = IMAGE_FORMAT_RGBA8888;
    overlay_image.virt_addr = pixels->data();
    overlay_image.size = static_cast<int>(pixels->size());
    overlay_image.fd = -1;
    const unsigned int color = 0xff000000U |
                               (static_cast<unsigned int>(red) << 16) |
                               (static_cast<unsigned int>(green) << 8) |
                               static_cast<unsigned int>(blue);
    draw_text(&overlay_image, text, x + 9, y + 3, color, font_size);
}

}  // namespace

int TrafficOverlayRenderer::Init(int width, int height, const TrafficRoiConfig& roi) {
    if (width <= 0 || height <= 0 || roi.points.size() < 3) {
        return -1;
    }
    width_ = width;
    height_ = height;
    roi_ = roi;
    cached_frame_id_ = -2;
    overlay_.assign(static_cast<size_t>(width_) * height_ * 4, 0);
    return 0;
}

int TrafficOverlayRenderer::BuildOverlay(const TrafficFrameAnalysis& analysis) {
    std::fill(overlay_.begin(), overlay_.end(), 0);

    const std::vector<TrafficPixelPoint> polygon = resolve_traffic_roi(roi_, width_, height_);
    FillPolygon(&overlay_, width_, height_, polygon, 20, 110, 255, 72);
    for (size_t i = 0; i < polygon.size(); ++i) {
        const TrafficPixelPoint& first = polygon[i];
        const TrafficPixelPoint& second = polygon[(i + 1) % polygon.size()];
        DrawLine(&overlay_, width_, height_, first.x, first.y, second.x, second.y,
                 5, 30, 150, 255, 255);
    }
    if (!polygon.empty()) {
        DrawPanelText(&overlay_, width_, height_, polygon[0].x, polygon[0].y - 48,
                      "CROSSWALK ROI", kLabelFontSize, 255, 255, 255);
    }

    char status_line[128];
    std::snprintf(status_line, sizeof(status_line),
                  "LIGHT=%s  violation=%d  person=%d  infer=%.1fms",
                  traffic_light_state_name(analysis.light.state),
                  analysis.violation_event_total,
                  analysis.person_id_total,
                  analysis.inference_ms);
    const bool has_violation = analysis.violation_count > 0;
    const unsigned char panel_red = has_violation ? 200 : 20;
    const unsigned char panel_green = has_violation ? 35 : 150;
    const unsigned char panel_blue = has_violation ? 35 : 65;
    DrawPanelText(&overlay_, width_, height_, 12, 12, status_line,
                  kStatusFontSize, 255, 255, 255,
                  panel_red, panel_green, panel_blue, 230);

    const int source_width = analysis.source_width > 0 ? analysis.source_width : width_;
    const int source_height = analysis.source_height > 0 ? analysis.source_height : height_;
    const float scale_x = static_cast<float>(width_) / source_width;
    const float scale_y = static_cast<float>(height_) / source_height;
    for (const TrafficDetectionState& state : analysis.detections) {
        const image_rect_t& source_box = state.detection.box;
        const int left = Clamp(static_cast<int>(std::lround(source_box.left * scale_x)), 0, width_ - 1);
        const int top = Clamp(static_cast<int>(std::lround(source_box.top * scale_y)), 0, height_ - 1);
        const int right = Clamp(static_cast<int>(std::lround(source_box.right * scale_x)), 0, width_ - 1);
        const int bottom = Clamp(static_cast<int>(std::lround(source_box.bottom * scale_y)), 0, height_ - 1);
        if (right <= left || bottom <= top) {
            continue;
        }

        unsigned char red = 255;
        unsigned char green = 210;
        unsigned char blue = 0;
        char label[96];
        if (state.detection.cls_id == kTrafficPersonClassId) {
            const char* event_name = state.predicted ? "hold" : "normal";
            if (state.violation) {
                red = 255;
                green = 30;
                blue = 30;
                event_name = "person/red_violation";
            } else if (state.bottom_in_crosswalk &&
                       analysis.light.state == TRAFFIC_LIGHT_GREEN) {
                red = 20;
                green = 255;
                blue = 60;
                event_name = "person/green_pass";
            }
            std::snprintf(label, sizeof(label), "person#%d/%s %.0f%%",
                          state.track_id, event_name,
                          state.detection.prop * 100.0f);
        } else {
            TrafficLightState light_state = state.selected_light
                                                ? analysis.light.state
                                                : TRAFFIC_LIGHT_UNKNOWN;
            if (light_state == TRAFFIC_LIGHT_RED) {
                red = 255;
                green = 30;
                blue = 30;
            } else if (light_state == TRAFFIC_LIGHT_GREEN) {
                red = 20;
                green = 255;
                blue = 60;
            }
            std::snprintf(label, sizeof(label), "traffic light/%s%s %.0f%%",
                          traffic_light_state_name(light_state),
                          state.predicted ? "/hold" : "",
                          state.detection.prop * 100.0f);
        }
        DrawBox(&overlay_, width_, height_, left, top, right, bottom, red, green, blue);
        DrawPanelText(&overlay_, width_, height_, left, top - 44, label,
                      kLabelFontSize, red, green, blue);
    }

    cached_frame_id_ = analysis.frame_id;
    return 0;
}

bool TrafficOverlayRenderer::BlendOverlay(image_buffer_t* target) const {
    if (target == nullptr || target->format != IMAGE_FORMAT_RGBA8888 ||
        target->width != width_ || target->height != height_ || overlay_.empty()) {
        return false;
    }
    if (target->fd < 0 && target->virt_addr != nullptr) {
        const int target_stride = target->width_stride > 0 ? target->width_stride : target->width;
        for (int y = 0; y < height_; ++y) {
            const unsigned char* source_row = overlay_.data() + y * width_ * 4;
            unsigned char* target_row = target->virt_addr + y * target_stride * 4;
            for (int x = 0; x < width_; ++x) {
                const unsigned char* source = source_row + x * 4;
                if (source[3] != 0) {
                    BlendPixel(target_row + x * 4, source[0], source[1], source[2], source[3]);
                }
            }
        }
        return true;
    }
    if (target->fd < 0) {
        return false;
    }

    rga_buffer_t source = wrapbuffer_virtualaddr_t(
        const_cast<unsigned char*>(overlay_.data()), width_, height_,
        width_, height_, RK_FORMAT_RGBA_8888);
    const int target_stride = target->width_stride > 0 ? target->width_stride : target->width;
    rga_buffer_t destination = wrapbuffer_fd_t(
        target->fd, target->width, target->height, target_stride,
        target->height_stride > 0 ? target->height_stride : target->height,
        RK_FORMAT_RGBA_8888);
    rga_buffer_t pattern;
    std::memset(&pattern, 0, sizeof(pattern));
    const im_rect source_rect = {0, 0, width_, height_};
    const im_rect destination_rect = {0, 0, width_, height_};
    const im_rect pattern_rect = {0, 0, 0, 0};
    return improcess(source, destination, pattern,
                     source_rect, destination_rect, pattern_rect,
                     IM_ALPHA_BLEND_SRC_OVER) == IM_STATUS_SUCCESS;
}

int TrafficOverlayRenderer::Render(image_buffer_t* ui_buffer,
                                   const TrafficFrameAnalysis& analysis) {
    if (ui_buffer == nullptr || ui_buffer->width != width_ || ui_buffer->height != height_) {
        return -1;
    }
    if (analysis.frame_id != cached_frame_id_ && BuildOverlay(analysis) != 0) {
        return -1;
    }
    return BlendOverlay(ui_buffer) ? 0 : -1;
}
