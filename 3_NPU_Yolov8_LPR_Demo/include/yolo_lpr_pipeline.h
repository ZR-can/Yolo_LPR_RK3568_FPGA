#ifndef YOLO_LPR_PIPELINE_H
#define YOLO_LPR_PIPELINE_H

#include "yolov8.h"
#include "lprnet.h"
#include "image_utils.h"
#include "file_utils.h"
#include "image_drawing.h"
#include "opencv2/opencv.hpp"
#include "opencv2/core/utils/filesystem.hpp"

struct YOLOLPRPipelineContext {
    rknn_app_context_t yolo_ctx;
    lprnet_app_context_t lprnet7_ctx;
    lprnet_app_context_t lprnet8_ctx;
};

struct PipelineResult {
    int left, top, right, bottom;
    float confidence;          // YOLO 框置信度
    float text_confidence;     // LPRNet 字符识别置信度
    std::string plate_name;
    std::string plate_type;
    unsigned int box_color;
    unsigned int text_color;
};

int init_pipeline(const char* yolov8_path, const char* lprnet7_path, const char* lprnet8_path, YOLOLPRPipelineContext* ctx);
int process_pipeline(YOLOLPRPipelineContext* ctx, image_buffer_t* src_image, std::vector<PipelineResult>& results, bool draw_on_image = true);
int process_pipeline_preprocessed(YOLOLPRPipelineContext* ctx, std::vector<PipelineResult>& results, bool draw_on_image = true);
void release_pipeline(YOLOLPRPipelineContext* ctx);

#endif // YOLO_LPR_PIPELINE_H