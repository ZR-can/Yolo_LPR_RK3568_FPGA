#ifndef ITS_TRAFFIC_SCENE_H_
#define ITS_TRAFFIC_SCENE_H_

#include <vector>

#include "image_utils.h"
#include "yolov8.h"

struct TrafficCounts {
    int person;
    int bicycle;
    int car;
    int motorcycle;
    int bus;
    int truck;
    int traffic_light;
    int stop_sign;
};

struct TrafficWarning {
    const char* code;
    char message[128];
};

struct TrafficSceneResult {
    TrafficCounts counts;
    std::vector<TrafficWarning> warnings;
};

void reset_traffic_counts(TrafficCounts* counts);
bool is_traffic_class(int cls_id);
const char* traffic_class_name(int cls_id);
int load_traffic_scene_config(const char* path);
int filter_traffic_detections(const object_detect_result_list* src,
                              object_detect_result_list* dst);

int analyze_traffic_scene(const object_detect_result_list* detections,
                          int image_width,
                          int image_height,
                          TrafficSceneResult* result);

void draw_traffic_scene_overlay(image_buffer_t* image,
                                const object_detect_result_list* detections,
                                const TrafficSceneResult* result);

#endif
