#include "traffic_scene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "image_drawing.h"

struct PointF {
    float x;
    float y;
};

struct BoxF {
    float left;
    float top;
    float right;
    float bottom;
};

struct PairRisk {
    float distance;
    bool overlap;
};

static const float kMinPersonHeightRatio = 0.045f;
static const float kMinVehicleHeightRatio = 0.045f;
static const float kBoxExpandRatio = 0.12f;
static const int kMaxDrawnRiskLinks = 12;

static int clamp_int(int value, int low, int high) {
    return std::max(low, std::min(value, high));
}

static bool is_vehicle_class(int cls_id) {
    return cls_id == 1 || cls_id == 2 || cls_id == 3 || cls_id == 5 || cls_id == 7;
}

static float box_width(const object_detect_result& det) {
    return static_cast<float>(std::max(0, det.box.right - det.box.left));
}

static float box_height(const object_detect_result& det) {
    return static_cast<float>(std::max(0, det.box.bottom - det.box.top));
}

static bool is_active_person_target(const object_detect_result& det, int image_height) {
    return det.cls_id == 0 && box_height(det) >= image_height * kMinPersonHeightRatio;
}

static bool is_active_vehicle_target(const object_detect_result& det, int image_height) {
    return is_vehicle_class(det.cls_id) && box_height(det) >= image_height * kMinVehicleHeightRatio;
}

static PointF bottom_center(const object_detect_result& det) {
    PointF p;
    p.x = 0.5f * (det.box.left + det.box.right);
    p.y = static_cast<float>(det.box.bottom);
    return p;
}

static float distance_between(PointF a, PointF b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

static BoxF expanded_box(const object_detect_result& det, float ratio, int width, int height) {
    const float w = box_width(det);
    const float h = box_height(det);
    BoxF out;
    out.left = std::max(0.0f, static_cast<float>(det.box.left) - w * ratio);
    out.top = std::max(0.0f, static_cast<float>(det.box.top) - h * ratio);
    out.right = std::min(static_cast<float>(width - 1), static_cast<float>(det.box.right) + w * ratio);
    out.bottom = std::min(static_cast<float>(height - 1), static_cast<float>(det.box.bottom) + h * ratio);
    return out;
}

static bool boxes_overlap(BoxF a, BoxF b) {
    return a.left <= b.right && a.right >= b.left && a.top <= b.bottom && a.bottom >= b.top;
}

static float conflict_distance_threshold(const object_detect_result& person,
                                         const object_detect_result& vehicle,
                                         int image_width,
                                         int image_height) {
    const float image_floor = 0.055f * std::min(image_width, image_height);
    const float scale_floor = 0.35f * (box_height(person) + box_height(vehicle));
    const float max_reasonable = 0.18f * std::min(image_width, image_height);
    return std::min(std::max(image_floor, scale_floor), max_reasonable);
}

static bool evaluate_pair_risk(const object_detect_result& person,
                               const object_detect_result& vehicle,
                               int image_width,
                               int image_height,
                               PairRisk* risk) {
    const PointF person_anchor = bottom_center(person);
    const PointF vehicle_anchor = bottom_center(vehicle);
    const float distance = distance_between(person_anchor, vehicle_anchor);
    const float threshold = conflict_distance_threshold(person, vehicle, image_width, image_height);
    const bool overlap = boxes_overlap(expanded_box(person, kBoxExpandRatio, image_width, image_height),
                                       expanded_box(vehicle, kBoxExpandRatio, image_width, image_height));

    if (risk != nullptr) {
        risk->distance = distance;
        risk->overlap = overlap;
    }

    return overlap || distance <= threshold;
}

static void add_warning2(TrafficSceneResult* result,
                         const char* code,
                         const char* fmt,
                         int a,
                         int b) {
    if (result == nullptr) {
        return;
    }
    TrafficWarning w;
    w.code = code;
    snprintf(w.message, sizeof(w.message), fmt, a, b);
    result->warnings.push_back(w);
}

static unsigned int class_color(int cls_id) {
    switch (cls_id) {
        case 0: return COLOR_YELLOW;
        case 1: return 0xFF00FFFF;
        case 2: return COLOR_BLUE;
        case 3: return COLOR_MAGENTA;
        case 5: return COLOR_GREEN;
        case 7: return COLOR_ORANGE;
        case 9: return COLOR_RED;
        case 11: return COLOR_RED;
        default: return COLOR_WHITE;
    }
}

void reset_traffic_counts(TrafficCounts* counts) {
    if (counts != nullptr) {
        std::memset(counts, 0, sizeof(TrafficCounts));
    }
}

bool is_traffic_class(int cls_id) {
    return cls_id == 0 ||
           cls_id == 1 ||
           cls_id == 2 ||
           cls_id == 3 ||
           cls_id == 5 ||
           cls_id == 7 ||
           cls_id == 9 ||
           cls_id == 11;
}

const char* traffic_class_name(int cls_id) {
    switch (cls_id) {
        case 0: return "person";
        case 1: return "bicycle";
        case 2: return "car";
        case 3: return "motorcycle";
        case 5: return "bus";
        case 7: return "truck";
        case 9: return "traffic light";
        case 11: return "stop sign";
        default: return "other";
    }
}

int load_traffic_scene_config(const char* path) {
    (void)path;
    return 0;
}

int filter_traffic_detections(const object_detect_result_list* src,
                              object_detect_result_list* dst) {
    if (src == nullptr || dst == nullptr) {
        return -1;
    }

    std::memset(dst, 0, sizeof(*dst));
    dst->id = src->id;
    for (int i = 0; i < src->count && dst->count < OBJ_NUMB_MAX_SIZE; ++i) {
        const object_detect_result& det = src->results[i];
        if (!is_traffic_class(det.cls_id)) {
            continue;
        }
        dst->results[dst->count++] = det;
    }
    return 0;
}

int analyze_traffic_scene(const object_detect_result_list* detections,
                          int image_width,
                          int image_height,
                          TrafficSceneResult* result) {
    if (detections == nullptr || result == nullptr || image_width <= 0 || image_height <= 0) {
        return -1;
    }

    reset_traffic_counts(&result->counts);
    result->warnings.clear();

    int high_risk_pairs = 0;
    int medium_risk_pairs = 0;
    float closest_high = 1000000.0f;
    float closest_medium = 1000000.0f;

    for (int i = 0; i < detections->count; ++i) {
        const object_detect_result& det = detections->results[i];
        if (!is_traffic_class(det.cls_id)) {
            continue;
        }

        switch (det.cls_id) {
            case 0: result->counts.person++; break;
            case 1: result->counts.bicycle++; break;
            case 2: result->counts.car++; break;
            case 3: result->counts.motorcycle++; break;
            case 5: result->counts.bus++; break;
            case 7: result->counts.truck++; break;
            case 9: result->counts.traffic_light++; break;
            case 11: result->counts.stop_sign++; break;
            default: break;
        }
    }

    for (int pi = 0; pi < detections->count; ++pi) {
        const object_detect_result& person = detections->results[pi];
        if (!is_active_person_target(person, image_height)) {
            continue;
        }

        for (int vi = 0; vi < detections->count; ++vi) {
            const object_detect_result& vehicle = detections->results[vi];
            if (!is_active_vehicle_target(vehicle, image_height)) {
                continue;
            }

            PairRisk risk;
            if (!evaluate_pair_risk(person, vehicle, image_width, image_height, &risk)) {
                continue;
            }

            if (risk.overlap) {
                high_risk_pairs++;
                closest_high = std::min(closest_high, risk.distance);
            } else {
                medium_risk_pairs++;
                closest_medium = std::min(closest_medium, risk.distance);
            }
        }
    }

    if (high_risk_pairs > 0) {
        add_warning2(result,
                     "person_vehicle_conflict",
                     "person-vehicle conflict HIGH: pairs=%d closest=%dpx",
                     high_risk_pairs,
                     static_cast<int>(closest_high + 0.5f));
    }
    if (medium_risk_pairs > 0) {
        add_warning2(result,
                     "pedestrian_near_vehicle",
                     "pedestrian near vehicle MED: pairs=%d closest=%dpx",
                     medium_risk_pairs,
                     static_cast<int>(closest_medium + 0.5f));
    }
    return 0;
}

void draw_traffic_scene_overlay(image_buffer_t* image,
                                const object_detect_result_list* detections,
                                const TrafficSceneResult* result) {
    if (image == nullptr || detections == nullptr || result == nullptr) {
        return;
    }

    for (int i = 0; i < detections->count; ++i) {
        const object_detect_result& det = detections->results[i];
        if (!is_traffic_class(det.cls_id)) {
            continue;
        }

        int left = clamp_int(det.box.left, 0, image->width - 1);
        int top = clamp_int(det.box.top, 0, image->height - 1);
        int right = clamp_int(det.box.right, 0, image->width - 1);
        int bottom = clamp_int(det.box.bottom, 0, image->height - 1);
        if (right <= left || bottom <= top) {
            continue;
        }

        unsigned int color = class_color(det.cls_id);
        if ((det.cls_id == 0 && !is_active_person_target(det, image->height)) ||
            (is_vehicle_class(det.cls_id) && !is_active_vehicle_target(det, image->height))) {
            color = COLOR_WHITE;
        }

        draw_rectangle_alpha(image, left, top, right - left, bottom - top, color, 3, 255);

        char label[96];
        snprintf(label, sizeof(label), "%s %.2f", traffic_class_name(det.cls_id), det.prop);
        draw_text(image, label, left, std::max(0, top - 22), color, 18);
    }

    int drawn_links = 0;
    for (int pi = 0; pi < detections->count && drawn_links < kMaxDrawnRiskLinks; ++pi) {
        const object_detect_result& person = detections->results[pi];
        if (!is_active_person_target(person, image->height)) {
            continue;
        }

        for (int vi = 0; vi < detections->count && drawn_links < kMaxDrawnRiskLinks; ++vi) {
            const object_detect_result& vehicle = detections->results[vi];
            if (!is_active_vehicle_target(vehicle, image->height)) {
                continue;
            }

            PairRisk risk;
            if (!evaluate_pair_risk(person, vehicle, image->width, image->height, &risk)) {
                continue;
            }

            const PointF pa = bottom_center(person);
            const PointF va = bottom_center(vehicle);
            const unsigned int color = risk.overlap ? COLOR_RED : COLOR_ORANGE;
            const int px = clamp_int(static_cast<int>(pa.x + 0.5f), 0, image->width - 1);
            const int py = clamp_int(static_cast<int>(pa.y + 0.5f), 0, image->height - 1);
            const int vx = clamp_int(static_cast<int>(va.x + 0.5f), 0, image->width - 1);
            const int vy = clamp_int(static_cast<int>(va.y + 0.5f), 0, image->height - 1);

            draw_line(image, px, py, vx, vy, color, risk.overlap ? 4 : 3);
            draw_circle(image, px, py, 5, color, -1);
            draw_circle(image, vx, vy, 5, color, -1);
            drawn_links++;
        }
    }

    int panel_h = 158 + static_cast<int>(result->warnings.size()) * 22;
    panel_h = std::min(panel_h, std::max(120, image->height - 16));
    draw_rectangle_alpha(image, 8, 8, 620, panel_h, COLOR_DARK_GRAY, -1, 180);

    char line[128];
    snprintf(line, sizeof(line), "P:%d Bic:%d C:%d M:%d Bus:%d Trk:%d TL:%d Stop:%d",
             result->counts.person,
             result->counts.bicycle,
             result->counts.car,
             result->counts.motorcycle,
             result->counts.bus,
             result->counts.truck,
             result->counts.traffic_light,
             result->counts.stop_sign);
    draw_text(image, "Traffic targets", 18, 18, COLOR_YELLOW, 20);
    draw_text(image, line, 18, 46, COLOR_WHITE, 18);

    draw_text(image, "Risk: person-vehicle spatial conflict", 18, 76, COLOR_WHITE, 18);
    if (result->warnings.empty()) {
        draw_text(image, "Warnings: none", 18, 104, COLOR_GREEN, 18);
    } else {
        draw_text(image, "Warnings", 18, 104, COLOR_ORANGE, 18);
        const int max_lines = std::max(0, (panel_h - 132) / 22);
        for (size_t i = 0; i < result->warnings.size() && static_cast<int>(i) < max_lines; ++i) {
            draw_text(image,
                      result->warnings[i].message,
                      18,
                      132 + static_cast<int>(i) * 22,
                      COLOR_RED,
                      18);
        }
    }
}
