#ifndef PPOCR_REC_H
#define PPOCR_REC_H

#include <cstddef>
#include <stdint.h>

#include <string>
#include <vector>

#include "common.h"
#include "rknn_api.h"

struct ppocr_rec_result_t {
    std::string text;
    float score;
};

struct ppocr_rec_perf_t {
    double preprocess_ms;
    double input_set_ms;
    double rknn_run_wall_ms;
    double rknn_official_ms;
    double output_get_ms;
    double postprocess_ms;
    double total_ms;
};

struct ppocr_rec_context_t {
    rknn_context rknn_ctx;
    rknn_tensor_attr input_attr;
    rknn_tensor_attr output_attr;
    int model_width;
    int model_height;
    int model_channel;
    int output_sequence_length;
    int output_class_count;
    std::vector<std::string> dictionary;
    std::vector<uint8_t> input_buffer;
};

int init_ppocr_rec_model(const char* model_path,
                         const char* dictionary_path,
                         ppocr_rec_context_t* app_ctx);

int release_ppocr_rec_model(ppocr_rec_context_t* app_ctx);

int inference_ppocr_rec_model(ppocr_rec_context_t* app_ctx,
                              const image_buffer_t* src_image,
                              ppocr_rec_result_t* result,
                              ppocr_rec_perf_t* perf);

// The ROI uses source-image coordinates with an exclusive right/bottom edge.
// A null ROI processes the complete source image.
int inference_ppocr_rec_model_roi(ppocr_rec_context_t* app_ctx,
                                  const image_buffer_t* src_image,
                                  const image_rect_t* roi,
                                  ppocr_rec_result_t* result,
                                  ppocr_rec_perf_t* perf);

// Runs PP-OCR from an already rectified, resized and right-padded interleaved
// UINT8 BGR [48,160,3] buffer. This is the image-mode OBB handoff contract.
int inference_ppocr_rec_model_prepared_bgr(ppocr_rec_context_t* app_ctx,
                                           const uint8_t* prepared_bgr,
                                           size_t prepared_size,
                                           ppocr_rec_result_t* result,
                                           ppocr_rec_perf_t* perf);

#endif  // PPOCR_REC_H
