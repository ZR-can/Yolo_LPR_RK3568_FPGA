#ifndef YOLO_LPR_PIPELINE_H
#define YOLO_LPR_PIPELINE_H

#include "yolov8.h"
#include "lprnet.h"
#include "image_utils.h"
#include "file_utils.h"
#include "image_drawing.h"
#include "opencv2/opencv.hpp"
#include "opencv2/core/utils/filesystem.hpp"
#include "plate_pipeline_result.h"

struct YOLOLPRPipelineContext {
    rknn_app_context_t yolo_ctx;
    lprnet_app_context_t lprnet7_ctx;
    lprnet_app_context_t lprnet8_ctx;
};

int init_pipeline(const char* yolov8_path, const char* lprnet7_path, const char* lprnet8_path, YOLOLPRPipelineContext* ctx);
int process_pipeline(YOLOLPRPipelineContext* ctx, image_buffer_t* src_image, std::vector<PipelineResult>& results, bool draw_on_image = true);
int process_pipeline_preprocessed(YOLOLPRPipelineContext* ctx, image_buffer_t* preprocessed_image, std::vector<PipelineResult>& results, bool draw_on_image = true);
void draw_pipeline_result_overlay(image_buffer_t* image, const PipelineResult& result);
void release_pipeline(YOLOLPRPipelineContext* ctx);

#endif // YOLO_LPR_PIPELINE_H
