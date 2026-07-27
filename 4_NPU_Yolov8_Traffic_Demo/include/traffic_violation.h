#ifndef TRAFFIC_VIOLATION_H
#define TRAFFIC_VIOLATION_H

#include <string>
#include <vector>

#include "image_utils.h"
#include "yolov8.h"

constexpr int kTrafficPersonClassId = TRAFFIC_PERSON_CLASS_ID;

enum TrafficLightState {
    TRAFFIC_LIGHT_UNKNOWN = 0,
    TRAFFIC_LIGHT_RED,
    TRAFFIC_LIGHT_GREEN,
};

struct TrafficNormalizedPoint {
    float x;
    float y;
};

struct TrafficPixelPoint {
    int x;
    int y;
};

struct TrafficRoiConfig {
    std::vector<TrafficNormalizedPoint> points;
};

struct TrafficNormalizedRect {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
};

struct TrafficLightRoiConfig {
    bool enabled = false;
    TrafficNormalizedRect rect;
};

struct TrafficLightResult {
    TrafficLightState state = TRAFFIC_LIGHT_UNKNOWN;
    TrafficLightState instant_state = TRAFFIC_LIGHT_UNKNOWN;
    image_rect_t box = {-1, -1, -1, -1};
    int red_evidence = 0;
    int green_evidence = 0;
    int active_count = 0;
};

struct TrafficDetectionState {
    object_detect_result detection;
    int track_id = -1;
    bool predicted = false;
    bool bottom_in_crosswalk = false;
    bool violation = false;
    bool violation_event = false;
};

struct TrafficFrameAnalysis {
    int frame_id = -1;
    int source_width = 0;
    int source_height = 0;
    double inference_ms = 0.0;
    TrafficLightResult light;
    std::vector<TrafficDetectionState> detections;
    int person_count = 0;
    int persons_in_crosswalk = 0;
    int violation_count = 0;
    int violation_event_count = 0;
    int violation_event_total = 0;
    int person_id_total = 0;
};

TrafficRoiConfig default_traffic_roi();
TrafficLightRoiConfig default_traffic_light_roi();
bool load_traffic_roi_config_file(const char* path,
                                  TrafficRoiConfig* traffic_roi,
                                  TrafficLightRoiConfig* light_roi,
                                  std::string* error_message);
bool parse_normalized_traffic_roi(const char* text,
                                  TrafficRoiConfig* config,
                                  std::string* error_message);
bool parse_normalized_traffic_light_roi(const char* text,
                                        TrafficLightRoiConfig* config,
                                        std::string* error_message);
std::vector<TrafficPixelPoint> resolve_traffic_roi(const TrafficRoiConfig& config,
                                                   int width,
                                                   int height);
image_rect_t resolve_traffic_light_roi(const TrafficLightRoiConfig& config,
                                       int width,
                                       int height);
const char* traffic_light_state_name(TrafficLightState state);
bool traffic_box_bottom_in_roi(const image_rect_t& box,
                               const TrafficRoiConfig& roi,
                               int width,
                               int height);

int analyze_traffic_frame(const image_buffer_t* image,
                          const object_detect_result_list* raw_detections,
                          const TrafficRoiConfig& roi,
                          const image_rect_t& light_box,
                          int frame_id,
                          TrafficFrameAnalysis* analysis);

#endif  // TRAFFIC_VIOLATION_H
