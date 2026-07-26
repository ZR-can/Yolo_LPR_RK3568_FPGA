#ifndef _RKNN_YOLOV8_DEMO_POSTPROCESS_H_
#define _RKNN_YOLOV8_DEMO_POSTPROCESS_H_

#include <stdint.h>
#include <vector>
#include "rknn_api.h"
#include "common.h"
#include "image_utils.h"

#define OBJ_NAME_MAX_SIZE 64
#define OBJ_NUMB_MAX_SIZE 128
#define OBJ_CLASS_NUM 8
#define TRAFFIC_PERSON_CLASS_ID 0
#define TRAFFIC_LIGHT_CLASS_ID 6
#define NMS_THRESH 0.5f
#define BOX_THRESH 0.25f
#define TRAFFIC_PERSON_BOX_THRESH 0.50f

// traffic_rknn_app_context_t is declared by yolov8.h before this header.

typedef struct {
    image_rect_t box;
    float prop;
    int cls_id;
} object_detect_result;

typedef struct {
    int id;
    int count;
    object_detect_result results[OBJ_NUMB_MAX_SIZE];
} object_detect_result_list;

int init_traffic_post_process(const char *label_path);
void deinit_traffic_post_process();
const char *traffic_cls_to_name(int cls_id);
int traffic_post_process(traffic_rknn_app_context_t *app_ctx,
                         void *outputs,
                         letterbox_t *letter_box,
                         float conf_threshold,
                         float nms_threshold,
                         object_detect_result_list *od_results);
#endif //_RKNN_YOLOV8_DEMO_POSTPROCESS_H_
