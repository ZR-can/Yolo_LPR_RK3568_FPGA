#include "traffic_violation.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>

namespace {

constexpr int kBottomLineSamples = 5;
constexpr int kBottomLineMinInside = 1;
constexpr int kColorMinSaturation = 70;
constexpr int kColorMinValue = 90;
constexpr int kColorMinPixels = 2;
constexpr float kColorDominanceRatio = 1.20f;
constexpr float kLightBoxExpandRatio = 0.10f;

int Clamp(int value, int low, int high) {
    return std::max(low, std::min(value, high));
}

void SetError(std::string* error_message, const char* message) {
    if (error_message != nullptr) {
        *error_message = message;
    }
}

void SkipSpaces(const char** cursor) {
    while (**cursor == ' ' || **cursor == '\t') {
        ++(*cursor);
    }
}

bool PointInPolygon(float x, float y, const std::vector<TrafficPixelPoint>& polygon) {
    bool inside = false;
    const int count = static_cast<int>(polygon.size());
    for (int i = 0, j = count - 1; i < count; j = i++) {
        const float xi = static_cast<float>(polygon[i].x);
        const float yi = static_cast<float>(polygon[i].y);
        const float xj = static_cast<float>(polygon[j].x);
        const float yj = static_cast<float>(polygon[j].y);
        const bool intersects = ((yi > y) != (yj > y)) &&
                                (x < (xj - xi) * (y - yi) / ((yj - yi) + 1e-6f) + xi);
        if (intersects) {
            inside = !inside;
        }
    }
    return inside;
}

bool BottomLineInside(const image_rect_t& box,
                      const std::vector<TrafficPixelPoint>& polygon) {
    int inside_samples = 0;
    for (int i = 0; i < kBottomLineSamples; ++i) {
        const float t = kBottomLineSamples == 1
                            ? 0.5f
                            : static_cast<float>(i) / static_cast<float>(kBottomLineSamples - 1);
        const float x = static_cast<float>(box.left) * (1.0f - t) +
                        static_cast<float>(box.right) * t;
        if (PointInPolygon(x, static_cast<float>(box.bottom), polygon)) {
            ++inside_samples;
        }
    }
    return inside_samples >= kBottomLineMinInside;
}

image_rect_t ExpandBox(const image_rect_t& box, int width, int height) {
    const float box_width = static_cast<float>(std::max(1, box.right - box.left));
    const float box_height = static_cast<float>(std::max(1, box.bottom - box.top));
    const int pad_x = static_cast<int>(std::lround(box_width * kLightBoxExpandRatio));
    const int pad_y = static_cast<int>(std::lround(box_height * kLightBoxExpandRatio));
    image_rect_t expanded;
    expanded.left = Clamp(box.left - pad_x, 0, width - 1);
    expanded.top = Clamp(box.top - pad_y, 0, height - 1);
    expanded.right = Clamp(box.right + pad_x, expanded.left + 1, width);
    expanded.bottom = Clamp(box.bottom + pad_y, expanded.top + 1, height);
    return expanded;
}

bool ReadRgbPixel(const image_buffer_t* image, int x, int y, int* red, int* green, int* blue) {
    if (image == nullptr || image->virt_addr == nullptr || red == nullptr || green == nullptr || blue == nullptr) {
        return false;
    }
    const int stride = image->width_stride > 0 ? image->width_stride : image->width;
    if (image->format == IMAGE_FORMAT_RGB888) {
        const unsigned char* pixel = image->virt_addr + (y * stride + x) * 3;
        *red = pixel[0];
        *green = pixel[1];
        *blue = pixel[2];
        return true;
    }
    if (image->format == IMAGE_FORMAT_BGR565) {
        const unsigned char* bytes = image->virt_addr + (y * stride + x) * 2;
        const unsigned short pixel = static_cast<unsigned short>(bytes[0]) |
                                     (static_cast<unsigned short>(bytes[1]) << 8);
        const int red5 = pixel & 0x1f;
        const int green6 = (pixel >> 5) & 0x3f;
        const int blue5 = (pixel >> 11) & 0x1f;
        *red = (red5 << 3) | (red5 >> 2);
        *green = (green6 << 2) | (green6 >> 4);
        *blue = (blue5 << 3) | (blue5 >> 2);
        return true;
    }
    return false;
}

void RgbToHsv(int red, int green, int blue, int* hue, int* saturation, int* value) {
    const int maximum = std::max(red, std::max(green, blue));
    const int minimum = std::min(red, std::min(green, blue));
    const int delta = maximum - minimum;
    *value = maximum;
    *saturation = maximum == 0 ? 0 : (delta * 255) / maximum;
    if (delta == 0) {
        *hue = 0;
    } else if (maximum == red) {
        *hue = (60 * (green - blue) / delta + 360) % 360;
    } else if (maximum == green) {
        *hue = 60 * (blue - red) / delta + 120;
    } else {
        *hue = 60 * (red - green) / delta + 240;
    }
}

TrafficLightResult EstimateLight(const image_buffer_t* image,
                                 const image_rect_t& box,
                                 float score) {
    TrafficLightResult result;
    result.box = box;
    result.score = score;
    for (int y = box.top; y < box.bottom; ++y) {
        for (int x = box.left; x < box.right; ++x) {
            int red = 0;
            int green = 0;
            int blue = 0;
            if (!ReadRgbPixel(image, x, y, &red, &green, &blue)) {
                return result;
            }
            int hue = 0;
            int saturation = 0;
            int value = 0;
            RgbToHsv(red, green, blue, &hue, &saturation, &value);
            if (saturation < kColorMinSaturation || value < kColorMinValue) {
                continue;
            }
            ++result.active_count;
            if (hue <= 20 || hue >= 340) {
                ++result.red_count;
            } else if (hue >= 70 && hue <= 170) {
                ++result.green_count;
            }
        }
    }

    if (result.red_count >= kColorMinPixels &&
        result.red_count > static_cast<int>(result.green_count * kColorDominanceRatio)) {
        result.state = TRAFFIC_LIGHT_RED;
    } else if (result.green_count >= kColorMinPixels &&
               result.green_count > static_cast<int>(result.red_count * kColorDominanceRatio)) {
        result.state = TRAFFIC_LIGHT_GREEN;
    }
    result.instant_state = result.state;
    return result;
}

}  // namespace

TrafficRoiConfig default_traffic_roi() {
    TrafficRoiConfig config;
    config.points.push_back({1.000000f, 0.695622f});
    config.points.push_back({1.000000f, 0.765115f});
    config.points.push_back({0.000000f, 0.886727f});
    config.points.push_back({0.000000f, 0.645587f});
    config.points.push_back({0.661587f, 0.615705f});
    return config;
}

bool parse_normalized_traffic_roi(const char* text,
                                  TrafficRoiConfig* config,
                                  std::string* error_message) {
    if (text == nullptr || config == nullptr) {
        SetError(error_message, "ROI text or destination is missing");
        return false;
    }

    TrafficRoiConfig parsed;
    const char* cursor = text;
    while (true) {
        SkipSpaces(&cursor);
        errno = 0;
        char* end = nullptr;
        const float x = std::strtof(cursor, &end);
        if (errno != 0 || end == cursor || !std::isfinite(x)) {
            SetError(error_message, "invalid ROI x coordinate");
            return false;
        }
        cursor = end;
        SkipSpaces(&cursor);
        if (*cursor != ',') {
            SetError(error_message, "ROI point must use x,y format");
            return false;
        }
        ++cursor;
        SkipSpaces(&cursor);
        errno = 0;
        const float y = std::strtof(cursor, &end);
        if (errno != 0 || end == cursor || !std::isfinite(y)) {
            SetError(error_message, "invalid ROI y coordinate");
            return false;
        }
        if (x < 0.0f || x > 1.0f || y < 0.0f || y > 1.0f) {
            SetError(error_message, "ROI coordinates must be normalized to [0,1]");
            return false;
        }
        parsed.points.push_back({x, y});
        cursor = end;
        SkipSpaces(&cursor);
        if (*cursor == '\0') {
            break;
        }
        if (*cursor != ';') {
            SetError(error_message, "ROI points must be separated by semicolons");
            return false;
        }
        ++cursor;
    }

    if (parsed.points.size() < 3) {
        SetError(error_message, "ROI polygon requires at least three points");
        return false;
    }
    *config = parsed;
    if (error_message != nullptr) {
        error_message->clear();
    }
    return true;
}

std::vector<TrafficPixelPoint> resolve_traffic_roi(const TrafficRoiConfig& config,
                                                   int width,
                                                   int height) {
    std::vector<TrafficPixelPoint> polygon;
    if (width <= 0 || height <= 0) {
        return polygon;
    }
    polygon.reserve(config.points.size());
    for (const TrafficNormalizedPoint& point : config.points) {
        TrafficPixelPoint pixel;
        pixel.x = Clamp(static_cast<int>(std::lround(point.x * (width - 1))), 0, width - 1);
        pixel.y = Clamp(static_cast<int>(std::lround(point.y * (height - 1))), 0, height - 1);
        polygon.push_back(pixel);
    }
    return polygon;
}

const char* traffic_light_state_name(TrafficLightState state) {
    switch (state) {
        case TRAFFIC_LIGHT_RED: return "red";
        case TRAFFIC_LIGHT_GREEN: return "green";
        default: return "unknown";
    }
}

bool traffic_box_bottom_in_roi(const image_rect_t& box,
                               const TrafficRoiConfig& roi,
                               int width,
                               int height) {
    if (roi.points.size() < 3 || width <= 0 || height <= 0) {
        return false;
    }
    return BottomLineInside(box, resolve_traffic_roi(roi, width, height));
}

int analyze_traffic_frame(const image_buffer_t* image,
                          const object_detect_result_list* raw_detections,
                          const TrafficRoiConfig& roi,
                          int frame_id,
                          TrafficFrameAnalysis* analysis) {
    if (image == nullptr || image->virt_addr == nullptr || raw_detections == nullptr ||
        analysis == nullptr || image->width <= 0 || image->height <= 0 || roi.points.size() < 3) {
        return -1;
    }

    TrafficFrameAnalysis next;
    next.frame_id = frame_id;
    next.source_width = image->width;
    next.source_height = image->height;
    const std::vector<TrafficPixelPoint> polygon =
        resolve_traffic_roi(roi, image->width, image->height);

    int selected_light_index = -1;
    int best_color_evidence = -1;
    for (int i = 0; i < raw_detections->count; ++i) {
        const object_detect_result& detection = raw_detections->results[i];
        if (detection.cls_id != kTrafficLightClassId) {
            continue;
        }
        ++next.light.candidates;
        const image_rect_t expanded = ExpandBox(detection.box, image->width, image->height);
        TrafficLightResult candidate = EstimateLight(image, expanded, detection.prop);
        const int color_evidence = std::max(candidate.red_count, candidate.green_count);
        const bool better = color_evidence > best_color_evidence ||
                            (color_evidence == best_color_evidence &&
                             candidate.active_count > next.light.active_count) ||
                            (color_evidence == best_color_evidence &&
                             candidate.active_count == next.light.active_count &&
                             candidate.score > next.light.score);
        if (better) {
            const int candidates = next.light.candidates;
            next.light = candidate;
            next.light.candidates = candidates;
            selected_light_index = i;
            best_color_evidence = color_evidence;
        }
    }

    int light_candidates = 0;
    for (int i = 0; i < raw_detections->count; ++i) {
        const object_detect_result& detection = raw_detections->results[i];
        if (detection.cls_id != kTrafficPersonClassId && detection.cls_id != kTrafficLightClassId) {
            continue;
        }

        TrafficDetectionState state;
        state.detection = detection;
        if (detection.cls_id == kTrafficPersonClassId) {
            ++next.person_count;
            state.bottom_in_crosswalk = BottomLineInside(detection.box, polygon);
            if (state.bottom_in_crosswalk) {
                ++next.persons_in_crosswalk;
            }
            state.violation = state.bottom_in_crosswalk && next.light.state == TRAFFIC_LIGHT_RED;
            if (state.violation) {
                ++next.violation_count;
            }
        } else {
            ++next.traffic_light_count;
            state.selected_light = i == selected_light_index;
            ++light_candidates;
        }
        next.detections.push_back(state);
    }
    next.light.candidates = light_candidates;
    *analysis = next;
    return 0;
}
