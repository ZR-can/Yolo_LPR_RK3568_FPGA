#ifndef YOLO_PPOCR_PIPELINE_H
#define YOLO_PPOCR_PIPELINE_H

#include <vector>

#include "plate_pipeline_result.h"
#include "ppocr_rec.h"
#include "yolov8.h"

struct YOLOPPOCRPipelineContext {
    rknn_app_context_t yolo_ctx;
    ppocr_rec_context_t ppocr_ctx;
    uint64_t ppocr_primary_attempts;
    uint64_t ppocr_retry_attempts;
    uint64_t ppocr_retry_successes;
    uint64_t ppocr_retry_stable_vote_suppressions;
    double ppocr_retry_ms;

    YOLOPPOCRPipelineContext()
        : yolo_ctx(),
          ppocr_ctx(),
          ppocr_primary_attempts(0),
          ppocr_retry_attempts(0),
          ppocr_retry_successes(0),
          ppocr_retry_stable_vote_suppressions(0),
          ppocr_retry_ms(0.0) {}
};

int init_ppocr_pipeline(const char* yolov8_path,
                        const char* ppocr_path,
                        const char* dictionary_path,
                        YOLOPPOCRPipelineContext* ctx);

int process_ppocr_pipeline(YOLOPPOCRPipelineContext* ctx,
                           image_buffer_t* source_image,
                           std::vector<PipelineResult>& results);

void release_ppocr_pipeline(YOLOPPOCRPipelineContext* ctx);

// Image-mode detector path. It shares only the result/context contract with
// the video pipeline; the OBB model and postprocess are initialized separately.
int init_obb_ppocr_pipeline(const char* yolov8_obb_path,
                            const char* ppocr_path,
                            const char* dictionary_path,
                            YOLOPPOCRPipelineContext* ctx);

int process_obb_ppocr_pipeline(YOLOPPOCRPipelineContext* ctx,
                               image_buffer_t* source_image,
                               std::vector<PipelineResult>& results,
                               const std::vector<PipelineResult>&
                                   stable_retry_results);

void release_obb_ppocr_pipeline(YOLOPPOCRPipelineContext* ctx);

#endif  // YOLO_PPOCR_PIPELINE_H
