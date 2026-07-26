#include "traffic_violation.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace {

constexpr int kBottomLineSamples = 5;
constexpr int kBottomLineMinInside = 1;
constexpr float kTrafficRoiInsetRatio = 0.03f;
constexpr int kColorMinSaturation = 70;
constexpr int kColorMinValue = 90;
constexpr int kRedEvidenceAdvantagePercent = 25;

int Clamp(int value, int low, int high) {
    return std::max(low, std::min(value, high));
}

void SetError(std::string* error_message, const std::string& message) {
    if (error_message != nullptr) {
        *error_message = message;
    }
}

std::string Trim(const std::string& text) {
    const std::string::size_type begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return std::string();
    }
    const std::string::size_type end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
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
        const int blue5 = pixel & 0x1f;
        const int green6 = (pixel >> 5) & 0x3f;
        const int red5 = (pixel >> 11) & 0x1f;
        *red = (red5 << 3) | (red5 >> 2);
        *green = (green6 << 2) | (green6 >> 4);
        *blue = (blue5 << 3) | (blue5 >> 2);
        return true;
    }
    return false;
}

bool IsActiveColor(int red, int green, int blue) {
    const int maximum = std::max(red, std::max(green, blue));
    const int minimum = std::min(red, std::min(green, blue));
    const int delta = maximum - minimum;
    const int saturation = maximum == 0 ? 0 : (delta * 255) / maximum;
    return saturation >= kColorMinSaturation && maximum >= kColorMinValue;
}

TrafficLightResult EstimateLight(const image_buffer_t* image,
                                 const image_rect_t& box) {
    TrafficLightResult result;
    result.box = box;
    result.state = TRAFFIC_LIGHT_GREEN;
    result.instant_state = TRAFFIC_LIGHT_GREEN;
    for (int y = box.top; y < box.bottom; ++y) {
        for (int x = box.left; x < box.right; ++x) {
            int red = 0;
            int green = 0;
            int blue = 0;
            if (!ReadRgbPixel(image, x, y, &red, &green, &blue)) {
                return result;
            }
            if (!IsActiveColor(red, green, blue)) {
                continue;
            }
            ++result.active_count;
            result.red_evidence += red;
            result.green_evidence += green;
        }
    }

    // Yellow, darkness and a weak red lead remain passable. This keeps the
    // single-frame rule binary after a light has been located and prevents a
    // dimming green lamp from casting a red vote.
    if (result.active_count > 0 &&
        result.red_evidence > result.green_evidence) {
        const int64_t scaled_red =
            static_cast<int64_t>(result.red_evidence) * 100;
        const int64_t required_red =
            static_cast<int64_t>(result.green_evidence) *
            (100 + kRedEvidenceAdvantagePercent);
        if (scaled_red >= required_red) {
            result.state = TRAFFIC_LIGHT_RED;
        }
    }
    result.instant_state = result.state;
    return result;
}

}  // namespace

TrafficRoiConfig default_traffic_roi() {
    TrafficRoiConfig config;
    config.points.push_back({1.000000f, 0.690454f});
    config.points.push_back({1.000000f, 0.762743f});
    config.points.push_back({0.000000f, 0.886932f});
    config.points.push_back({0.000000f, 0.645042f});
    config.points.push_back({0.675873f, 0.617238f});
    return config;
}

TrafficLightRoiConfig default_traffic_light_roi() {
    TrafficLightRoiConfig config;
    config.enabled = true;
    config.rect.left = 0.547917f;
    config.rect.top = 0.235185f;
    config.rect.right = 0.581250f;
    config.rect.bottom = 0.339815f;
    return config;
}

bool load_traffic_roi_config_file(const char* path,
                                  TrafficRoiConfig* traffic_roi,
                                  TrafficLightRoiConfig* light_roi,
                                  std::string* error_message) {
    if (path == nullptr || *path == '\0' ||
        traffic_roi == nullptr || light_roi == nullptr) {
        SetError(error_message, "ROI config path or destination is missing");
        return false;
    }

    std::ifstream input(path);
    if (!input.is_open()) {
        SetError(error_message, std::string("cannot open ROI config: ") + path);
        return false;
    }

    TrafficRoiConfig parsed_traffic_roi;
    TrafficLightRoiConfig parsed_light_roi;
    bool traffic_roi_seen = false;
    bool light_roi_seen = false;
    std::string line;
    int line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        if (line_number == 1 && line.size() >= 3U &&
            static_cast<unsigned char>(line[0]) == 0xef &&
            static_cast<unsigned char>(line[1]) == 0xbb &&
            static_cast<unsigned char>(line[2]) == 0xbf) {
            line.erase(0, 3);
        }
        line = Trim(line);
        if (line.empty() || line[0] == '#') {
            continue;
        }

        const std::string::size_type separator = line.find('=');
        if (separator == std::string::npos) {
            std::ostringstream message;
            message << "ROI config line " << line_number
                    << " must use key=value format";
            SetError(error_message, message.str());
            return false;
        }
        const std::string key = Trim(line.substr(0, separator));
        std::string value = Trim(line.substr(separator + 1));
        if (value.empty()) {
            std::ostringstream message;
            message << "ROI config line " << line_number
                    << " has an empty value";
            SetError(error_message, message.str());
            return false;
        }
        if (value[0] == '"' || value[value.size() - 1] == '"') {
            if (value.size() < 2U ||
                value[0] != '"' || value[value.size() - 1] != '"') {
                std::ostringstream message;
                message << "ROI config line " << line_number
                        << " has unmatched double quotes";
                SetError(error_message, message.str());
                return false;
            }
            value = value.substr(1, value.size() - 2);
            if (value.empty()) {
                std::ostringstream message;
                message << "ROI config line " << line_number
                        << " has an empty quoted value";
                SetError(error_message, message.str());
                return false;
            }
        }

        std::string parse_error;
        if (key == "roi") {
            if (traffic_roi_seen) {
                SetError(error_message, "ROI config contains duplicate roi entries");
                return false;
            }
            if (!parse_normalized_traffic_roi(
                    value.c_str(), &parsed_traffic_roi, &parse_error)) {
                SetError(error_message, std::string("invalid roi: ") + parse_error);
                return false;
            }
            traffic_roi_seen = true;
        } else if (key == "light_roi") {
            if (light_roi_seen) {
                SetError(error_message,
                         "ROI config contains duplicate light_roi entries");
                return false;
            }
            if (!parse_normalized_traffic_light_roi(
                    value.c_str(), &parsed_light_roi, &parse_error)) {
                SetError(error_message,
                         std::string("invalid light_roi: ") + parse_error);
                return false;
            }
            light_roi_seen = true;
        } else {
            std::ostringstream message;
            message << "ROI config line " << line_number
                    << " has unknown key: " << key;
            SetError(error_message, message.str());
            return false;
        }
    }
    if (input.bad()) {
        SetError(error_message, std::string("failed while reading ROI config: ") + path);
        return false;
    }
    if (!traffic_roi_seen || !light_roi_seen) {
        SetError(error_message,
                 "ROI config must contain both roi and light_roi entries");
        return false;
    }

    *traffic_roi = parsed_traffic_roi;
    *light_roi = parsed_light_roi;
    if (error_message != nullptr) {
        error_message->clear();
    }
    return true;
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

bool parse_normalized_traffic_light_roi(const char* text,
                                        TrafficLightRoiConfig* config,
                                        std::string* error_message) {
    if (text == nullptr || config == nullptr) {
        SetError(error_message, "traffic-light ROI text or destination is missing");
        return false;
    }

    float values[4] = {};
    const char* cursor = text;
    for (int index = 0; index < 4; ++index) {
        SkipSpaces(&cursor);
        errno = 0;
        char* end = nullptr;
        values[index] = std::strtof(cursor, &end);
        if (errno != 0 || end == cursor || !std::isfinite(values[index])) {
            SetError(error_message, "traffic-light ROI requires four numeric coordinates");
            return false;
        }
        if (values[index] < 0.0f || values[index] > 1.0f) {
            SetError(error_message, "traffic-light ROI coordinates must be in [0,1]");
            return false;
        }
        cursor = end;
        SkipSpaces(&cursor);
        if (index < 3) {
            if (*cursor != ',') {
                SetError(error_message, "traffic-light ROI must use left,top,right,bottom");
                return false;
            }
            ++cursor;
        }
    }
    SkipSpaces(&cursor);
    if (*cursor != '\0') {
        SetError(error_message, "unexpected text after traffic-light ROI");
        return false;
    }
    if (values[0] >= values[2] || values[1] >= values[3]) {
        SetError(error_message, "traffic-light ROI must have left < right and top < bottom");
        return false;
    }

    TrafficLightRoiConfig parsed;
    parsed.enabled = true;
    parsed.rect.left = values[0];
    parsed.rect.top = values[1];
    parsed.rect.right = values[2];
    parsed.rect.bottom = values[3];
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
    if (config.points.size() < 3U) {
        return polygon;
    }
    float center_x = 0.0f;
    float center_y = 0.0f;
    for (const TrafficNormalizedPoint& point : config.points) {
        center_x += point.x;
        center_y += point.y;
    }
    center_x /= static_cast<float>(config.points.size());
    center_y /= static_cast<float>(config.points.size());
    const float inset_scale = 1.0f - kTrafficRoiInsetRatio;

    polygon.reserve(config.points.size());
    for (const TrafficNormalizedPoint& point : config.points) {
        const float inset_x =
            center_x + (point.x - center_x) * inset_scale;
        const float inset_y =
            center_y + (point.y - center_y) * inset_scale;
        TrafficPixelPoint pixel;
        pixel.x = Clamp(
            static_cast<int>(std::lround(inset_x * (width - 1))),
            0,
            width - 1);
        pixel.y = Clamp(
            static_cast<int>(std::lround(inset_y * (height - 1))),
            0,
            height - 1);
        polygon.push_back(pixel);
    }
    return polygon;
}

image_rect_t resolve_traffic_light_roi(const TrafficLightRoiConfig& config,
                                       int width,
                                       int height) {
    image_rect_t box = {-1, -1, -1, -1};
    if (!config.enabled || width <= 0 || height <= 0) {
        return box;
    }
    box.left = Clamp(
        static_cast<int>(std::floor(config.rect.left * width)), 0, width - 1);
    box.top = Clamp(
        static_cast<int>(std::floor(config.rect.top * height)), 0, height - 1);
    box.right = Clamp(
        static_cast<int>(std::ceil(config.rect.right * width)), box.left + 1, width);
    box.bottom = Clamp(
        static_cast<int>(std::ceil(config.rect.bottom * height)), box.top + 1, height);
    return box;
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
                          const image_rect_t& light_box,
                          int frame_id,
                          TrafficFrameAnalysis* analysis) {
    if (image == nullptr || image->virt_addr == nullptr || raw_detections == nullptr ||
        analysis == nullptr || image->width <= 0 || image->height <= 0 ||
        roi.points.size() < 3 || light_box.left < 0 || light_box.top < 0 ||
        light_box.right > image->width || light_box.bottom > image->height ||
        light_box.left >= light_box.right || light_box.top >= light_box.bottom) {
        return -1;
    }

    TrafficFrameAnalysis next;
    next.frame_id = frame_id;
    next.source_width = image->width;
    next.source_height = image->height;
    const std::vector<TrafficPixelPoint> polygon =
        resolve_traffic_roi(roi, image->width, image->height);
    next.light = EstimateLight(image, light_box);

    for (int i = 0; i < raw_detections->count; ++i) {
        const object_detect_result& detection = raw_detections->results[i];
        if (detection.cls_id != kTrafficPersonClassId) {
            continue;
        }

        TrafficDetectionState state;
        state.detection = detection;
        ++next.person_count;
        state.bottom_in_crosswalk = BottomLineInside(detection.box, polygon);
        if (state.bottom_in_crosswalk) {
            ++next.persons_in_crosswalk;
        }
        state.violation =
            state.bottom_in_crosswalk && next.light.state == TRAFFIC_LIGHT_RED;
        if (state.violation) {
            ++next.violation_count;
        }
        next.detections.push_back(state);
    }

    *analysis = next;
    return 0;
}
