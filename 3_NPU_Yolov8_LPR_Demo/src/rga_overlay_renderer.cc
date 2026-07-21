#include "rga_overlay_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "font.h"
#include "im2d.h"
#include "plate_font.h"

namespace {

constexpr unsigned int kBlackAlpha = 0x78000000;
constexpr unsigned int kWhite = 0xFFFFFFFF;
constexpr unsigned int kMagenta = 0xFFFF00FF;
constexpr unsigned char kPanelAlpha = 190;
constexpr unsigned char kPanelColor = 0x20;
constexpr int kPlateFontPx = 28;
constexpr int kMetaFontPx = 22;
constexpr int kPanelRadius = 8;
constexpr int kPadX = 8;
constexpr int kPadY = 4;
constexpr int kTextGap = 10;
constexpr int kBoxGap = 4;

int Clamp(int value, int low, int high) {
    return std::max(low, std::min(value, high));
}

int ToRgaFormat(image_format_t format) {
    switch (format) {
    case IMAGE_FORMAT_RGBA8888:
        return RK_FORMAT_RGBA_8888;
    default:
        return -1;
    }
}

const unsigned char* FindGlyph(const char* text, int* step) {
    const unsigned char ch = static_cast<unsigned char>(text[0]);
    if (ch >= 0xE0) {
        *step = 3;
        for (int i = 0; i < 81; ++i) {
            if (plate_char_map[i].bytes == 3 && strncmp(text, plate_char_map[i].name, 3) == 0) {
                return plate_font_data[plate_char_map[i].index];
            }
        }
        return nullptr;
    }

    *step = 1;
    for (int i = 0; i < 81; ++i) {
        if (plate_char_map[i].bytes == 1 && text[0] == plate_char_map[i].name[0]) {
            return plate_font_data[plate_char_map[i].index];
        }
    }
    if (ch >= ' ' && ch <= '~') {
        return mono_font_data[ch - ' '];
    }
    return nullptr;
}

void ResizeGlyph(const unsigned char* src, unsigned char* dst, int dst_w, int dst_h) {
    for (int y = 0; y < dst_h; ++y) {
        const int src_y = y * 40 / dst_h;
        for (int x = 0; x < dst_w; ++x) {
            const int src_x = x * 20 / dst_w;
            dst[y * dst_w + x] = src[src_y * 20 + src_x];
        }
    }
}

int MeasureTextWidth(const std::string& text, int font_px) {
    int width = 0;
    for (size_t i = 0; i < text.size();) {
        const unsigned char ch = static_cast<unsigned char>(text[i]);
        if (ch >= 0xE0 && i + 2 < text.size()) {
            width += font_px;
            i += 3;
        } else {
            width += font_px / 2 + 2;
            ++i;
        }
    }
    return width;
}

void BlendStraightPixel(unsigned char* pixel, unsigned char red, unsigned char green,
                        unsigned char blue, unsigned char alpha) {
    const unsigned int dst_alpha = pixel[3];
    const unsigned int out_alpha = alpha + (dst_alpha * (255 - alpha) + 127) / 255;
    if (out_alpha == 0) {
        return;
    }

    const unsigned int src_factor = alpha * 255;
    const unsigned int dst_factor = dst_alpha * (255 - alpha);
    pixel[0] = static_cast<unsigned char>((red * src_factor + pixel[0] * dst_factor) / (out_alpha * 255));
    pixel[1] = static_cast<unsigned char>((green * src_factor + pixel[1] * dst_factor) / (out_alpha * 255));
    pixel[2] = static_cast<unsigned char>((blue * src_factor + pixel[2] * dst_factor) / (out_alpha * 255));
    pixel[3] = static_cast<unsigned char>(out_alpha);
}

void FillRoundedRect(std::vector<unsigned char>* pixels, int image_w, int image_h,
                     int x, int y, int width, int height, int radius) {
    const int radius_sq = radius * radius;
    for (int py = 0; py < height; ++py) {
        for (int px = 0; px < width; ++px) {
            int corner_x = px;
            int corner_y = py;
            if (px < radius) {
                corner_x = radius;
            } else if (px >= width - radius) {
                corner_x = width - radius - 1;
            }
            if (py < radius) {
                corner_y = radius;
            } else if (py >= height - radius) {
                corner_y = height - radius - 1;
            }
            const int dx = px - corner_x;
            const int dy = py - corner_y;
            if (dx * dx + dy * dy > radius_sq) {
                continue;
            }

            const int dst_x = x + px;
            const int dst_y = y + py;
            if (dst_x < 0 || dst_x >= image_w || dst_y < 0 || dst_y >= image_h) {
                continue;
            }
            unsigned char* pixel = pixels->data() + (dst_y * image_w + dst_x) * 4;
            BlendStraightPixel(pixel, kPanelColor, kPanelColor, kPanelColor, kPanelAlpha);
        }
    }
}

void DrawText(std::vector<unsigned char>* pixels, int image_w, int image_h, const std::string& text,
              int start_x, int start_y, int font_px) {
    int cursor_x = start_x;
    for (size_t i = 0; i < text.size();) {
        int step = 1;
        const unsigned char* glyph = FindGlyph(text.c_str() + i, &step);
        if (glyph != nullptr) {
            const bool wide = step == 3;
            const int glyph_w = wide ? font_px : font_px / 2;
            const int glyph_h = wide ? font_px * 2 : font_px;
            const int y_offset = wide ? 0 : (font_px * 2 - glyph_h) / 2;
            std::vector<unsigned char> resized(glyph_w * glyph_h);
            ResizeGlyph(glyph, resized.data(), glyph_w, glyph_h);

            for (int y = 0; y < glyph_h; ++y) {
                const int dst_y = start_y + y_offset + y;
                if (dst_y < 0 || dst_y >= image_h) {
                    continue;
                }
                for (int x = 0; x < glyph_w; ++x) {
                    const int dst_x = cursor_x + x;
                    if (dst_x < 0 || dst_x >= image_w) {
                        continue;
                    }
                    const unsigned char alpha = resized[y * glyph_w + x];
                    if (alpha == 0) {
                        continue;
                    }
                    unsigned char* pixel = pixels->data() + (dst_y * image_w + dst_x) * 4;
                    BlendStraightPixel(pixel, 255, 255, 255, alpha);
                }
            }
            cursor_x += wide ? font_px : glyph_w + 2;
        }
        i += step;
    }
}

void DrawBoldText(std::vector<unsigned char>* pixels, int image_w, int image_h, const std::string& text,
                  int x, int y, int font_px) {
    DrawText(pixels, image_w, image_h, text, x, y, font_px);
    DrawText(pixels, image_w, image_h, text, x + 1, y, font_px);
    DrawText(pixels, image_w, image_h, text, x, y + 1, font_px);
}

bool FillUi(image_buffer_t* image, unsigned int color) {
    if (image == nullptr) {
        return false;
    }
    if (image->fd < 0 && image->virt_addr != nullptr && image->format == IMAGE_FORMAT_RGBA8888) {
        const unsigned char alpha = static_cast<unsigned char>((color >> 24) & 0xff);
        const unsigned char red = static_cast<unsigned char>((color >> 16) & 0xff);
        const unsigned char green = static_cast<unsigned char>((color >> 8) & 0xff);
        const unsigned char blue = static_cast<unsigned char>(color & 0xff);
        const int stride = image->width_stride > 0 ? image->width_stride : image->width;
        for (int y = 0; y < image->height; ++y) {
            unsigned char* row = image->virt_addr + y * stride * 4;
            for (int x = 0; x < image->width; ++x) {
                row[x * 4 + 0] = red;
                row[x * 4 + 1] = green;
                row[x * 4 + 2] = blue;
                row[x * 4 + 3] = alpha;
            }
        }
        return true;
    }
    if (image->fd < 0) {
        return false;
    }
    const int format = ToRgaFormat(image->format);
    if (format < 0) {
        return false;
    }
    const int stride = image->width_stride > 0 ? image->width_stride : image->width;
    rga_buffer_t target = wrapbuffer_fd_t(image->fd, image->width, image->height, stride,
                                           image->height_stride > 0 ? image->height_stride : image->height,
                                           format);
    const im_rect rect = {0, 0, image->width, image->height};
    return imfill(target, rect, color, 1) == IM_STATUS_SUCCESS;
}

bool FillRectangle(const rga_buffer_t& target, int x, int y, int width, int height,
                   unsigned int color) {
    if (width <= 0 || height <= 0) {
        return true;
    }
    const im_rect rect = {x, y, width, height};
    return imfill(target, rect, color, 1) == IM_STATUS_SUCCESS;
}

bool DrawRectangle(image_buffer_t* image, int x, int y, int width, int height,
                   unsigned int color, int thickness) {
    if (image == nullptr || width <= 0 || height <= 0) {
        return false;
    }
    const int x0 = Clamp(x, 0, image->width);
    const int y0 = Clamp(y, 0, image->height);
    const int x1 = Clamp(x + width, 0, image->width);
    const int y1 = Clamp(y + height, 0, image->height);
    if (x1 <= x0 || y1 <= y0) {
        return true;
    }

    const int line_width = std::min(std::max(thickness, 1), std::min(x1 - x0, y1 - y0));
    if (image->fd < 0 && image->virt_addr != nullptr && image->format == IMAGE_FORMAT_RGBA8888) {
        const unsigned char alpha = static_cast<unsigned char>((color >> 24) & 0xff);
        const unsigned char red = static_cast<unsigned char>((color >> 16) & 0xff);
        const unsigned char green = static_cast<unsigned char>((color >> 8) & 0xff);
        const unsigned char blue = static_cast<unsigned char>(color & 0xff);
        const int stride = image->width_stride > 0 ? image->width_stride : image->width;
        auto fill_rect_cpu = [&](int rx, int ry, int rw, int rh) {
            if (rw <= 0 || rh <= 0) {
                return;
            }
            for (int py = ry; py < ry + rh; ++py) {
                unsigned char* row = image->virt_addr + py * stride * 4;
                for (int px = rx; px < rx + rw; ++px) {
                    BlendStraightPixel(row + px * 4, red, green, blue, alpha);
                }
            }
        };
        fill_rect_cpu(x0, y0, x1 - x0, line_width);
        fill_rect_cpu(x0, y1 - line_width, x1 - x0, line_width);
        fill_rect_cpu(x0, y0 + line_width, line_width, y1 - y0 - line_width * 2);
        fill_rect_cpu(x1 - line_width, y0 + line_width, line_width, y1 - y0 - line_width * 2);
        return true;
    }
    if (image->fd < 0) {
        return false;
    }
    const int format = ToRgaFormat(image->format);
    if (format < 0) {
        return false;
    }
    const int stride = image->width_stride > 0 ? image->width_stride : image->width;
    rga_buffer_t target = wrapbuffer_fd_t(image->fd, image->width, image->height, stride,
                                           image->height_stride > 0 ? image->height_stride : image->height,
                                           format);

    return FillRectangle(target, x0, y0, x1 - x0, line_width, color) &&
           FillRectangle(target, x0, y1 - line_width, x1 - x0, line_width, color) &&
           FillRectangle(target, x0, y0 + line_width, line_width, y1 - y0 - line_width * 2, color) &&
           FillRectangle(target, x1 - line_width, y0 + line_width, line_width,
                         y1 - y0 - line_width * 2, color);
}

}  // namespace

int RgaOverlayRenderer::Init(int width, int height) {
    if (width <= 0 || height <= 0) {
        return -1;
    }
    width_ = width;
    height_ = height;
    label_cache_.clear();
    return 0;
}

const RgaOverlayRenderer::LabelSprite& RgaOverlayRenderer::GetLabelSprite(const PipelineResult& result) {
    const int confidence_per_mille = static_cast<int>(std::lround(result.confidence * 1000.0f));
    const std::string key = result.plate_name + "\x1f" + result.plate_type + "\x1f" + std::to_string(confidence_per_mille);
    const auto found = label_cache_.find(key);
    if (found != label_cache_.end()) {
        return found->second;
    }

    if (label_cache_.size() >= 64) {
        label_cache_.clear();
    }
    return label_cache_.emplace(key, BuildLabelSprite(result)).first->second;
}

RgaOverlayRenderer::LabelSprite RgaOverlayRenderer::BuildLabelSprite(const PipelineResult& result) const {
    char meta_buffer[96];
    snprintf(meta_buffer, sizeof(meta_buffer), "%s %.1f%%", result.plate_type.c_str(), result.confidence * 100.0f);
    const std::string plate_text = result.plate_name.empty() ? "-" : result.plate_name;
    const std::string meta_text = meta_buffer;

    int plate_font_px = kPlateFontPx;
    int meta_font_px = kMetaFontPx;
    int plate_width = 0;
    int meta_width = 0;
    int row_height = 0;
    int sprite_width = 0;
    int sprite_height = 0;
    do {
        plate_width = MeasureTextWidth(plate_text, plate_font_px);
        meta_width = MeasureTextWidth(meta_text, meta_font_px);
        row_height = std::max(plate_font_px * 2, meta_font_px * 2);
        sprite_width = kPadX * 2 + plate_width + kTextGap + meta_width + 2;
        sprite_height = kPadY * 2 + row_height + 2;

        if ((sprite_width <= width_ && sprite_height <= height_) ||
            (plate_font_px <= 8 && meta_font_px <= 8)) {
            break;
        }
        plate_font_px = std::max(8, plate_font_px - 2);
        meta_font_px = std::max(8, meta_font_px - 2);
    } while (true);

    LabelSprite sprite;
    sprite.width = sprite_width;
    sprite.height = sprite_height;
    sprite.pixels.assign(sprite.width * sprite.height * 4, 0);

    FillRoundedRect(&sprite.pixels, sprite.width, sprite.height, 0, 0,
                    sprite.width, sprite.height, kPanelRadius);
    DrawBoldText(&sprite.pixels, sprite.width, sprite.height, plate_text, kPadX, kPadY, plate_font_px);
    DrawBoldText(&sprite.pixels, sprite.width, sprite.height, meta_text,
                 kPadX + plate_width + kTextGap,
                 kPadY + (plate_font_px * 2 - meta_font_px * 2) / 2, meta_font_px);
    return sprite;
}

bool RgaOverlayRenderer::BlendSprite(const LabelSprite& sprite, image_buffer_t* target, int x, int y) const {
    if (sprite.width <= 0 || sprite.height <= 0 || sprite.pixels.empty() || target == nullptr) {
        return false;
    }
    if (target->fd < 0 && target->virt_addr != nullptr && target->format == IMAGE_FORMAT_RGBA8888) {
        if (x < 0 || y < 0 || x >= target->width || y >= target->height) {
            return false;
        }
        const int blend_width = std::min(sprite.width, target->width - x);
        const int blend_height = std::min(sprite.height, target->height - y);
        if (blend_width <= 0 || blend_height <= 0) {
            return false;
        }
        const int stride = target->width_stride > 0 ? target->width_stride : target->width;
        for (int py = 0; py < blend_height; ++py) {
            const unsigned char* src_row = sprite.pixels.data() + py * sprite.width * 4;
            unsigned char* dst_row = target->virt_addr + (y + py) * stride * 4 + x * 4;
            for (int px = 0; px < blend_width; ++px) {
                const unsigned char* src = src_row + px * 4;
                if (src[3] == 0) {
                    continue;
                }
                BlendStraightPixel(dst_row + px * 4, src[0], src[1], src[2], src[3]);
            }
        }
        return true;
    }
    if (target->fd < 0) {
        return false;
    }
    const int format = ToRgaFormat(target->format);
    if (format < 0) {
        return false;
    }

    if (x < 0 || y < 0 || x >= target->width || y >= target->height) {
        return false;
    }
    const int blend_width = std::min(sprite.width, target->width - x);
    const int blend_height = std::min(sprite.height, target->height - y);
    if (blend_width <= 0 || blend_height <= 0) {
        return false;
    }

    rga_buffer_t source = wrapbuffer_virtualaddr(const_cast<unsigned char*>(sprite.pixels.data()),
                                                   sprite.width, sprite.height, RK_FORMAT_RGBA_8888);
    const int stride = target->width_stride > 0 ? target->width_stride : target->width;
    rga_buffer_t destination = wrapbuffer_fd_t(target->fd, target->width, target->height, stride,
                                                 target->height_stride > 0 ? target->height_stride : target->height,
                                                 format);
    rga_buffer_t pattern;
    memset(&pattern, 0, sizeof(pattern));
    const im_rect src_rect = {0, 0, blend_width, blend_height};
    const im_rect dst_rect = {x, y, blend_width, blend_height};
    const im_rect pat_rect = {0, 0, 0, 0};
    return improcess(source, destination, pattern, src_rect, dst_rect, pat_rect,
                     IM_ALPHA_BLEND_SRC_OVER) == IM_STATUS_SUCCESS;
}

int RgaOverlayRenderer::Render(image_buffer_t* ui_buffer, const std::vector<PipelineResult>& results,
                               bool clear_background) {
    if (ui_buffer == nullptr || ui_buffer->width != width_ || ui_buffer->height != height_) {
        return -1;
    }
    if (clear_background && !FillUi(ui_buffer, 0x00000000)) {
        return -1;
    }

    for (const PipelineResult& result : results) {
        const int left = Clamp(result.left, 0, width_ - 1);
        const int top = Clamp(result.top, 0, height_ - 1);
        const int right = Clamp(result.right, 0, width_ - 1);
        const int bottom = Clamp(result.bottom, 0, height_ - 1);
        if (right <= left || bottom <= top) {
            continue;
        }

        const int box_width = right - left;
        const int box_height = bottom - top;
        if (!DrawRectangle(ui_buffer, left - 2, top - 2, box_width + 4, box_height + 4, kBlackAlpha, 2) ||
            !DrawRectangle(ui_buffer, left - 1, top - 1, box_width + 2, box_height + 2, kWhite, 2) ||
            !DrawRectangle(ui_buffer, left, top, box_width, box_height, kMagenta, 4)) {
            continue;
        }

        if (!result.has_valid_plate_text) {
            continue;
        }

        const LabelSprite& sprite = GetLabelSprite(result);
        const int max_label_x = std::max(0, width_ - sprite.width);
        const int max_label_y = std::max(0, height_ - sprite.height);
        const int box_center_x = left + box_width / 2;
        int label_x = Clamp(box_center_x - sprite.width / 2, 0, max_label_x);
        const int above_y = top - sprite.height - kBoxGap;
        const int below_y = bottom + kBoxGap;
        int label_y = above_y;
        if (above_y < 0) {
            label_y = below_y <= max_label_y ? below_y : max_label_y;
        }
        label_y = Clamp(label_y, 0, max_label_y);
        if (!BlendSprite(sprite, ui_buffer, label_x, label_y)) {
            continue;
        }
    }
    return 0;
}
