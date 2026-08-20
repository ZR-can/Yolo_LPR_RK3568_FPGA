#ifndef OBB_POSTPROCESS_H
#define OBB_POSTPROCESS_H

#include <array>
#include <cstdint>
#include <vector>

#include "common.h"
#include "image_utils.h"
#include "rknn_api.h"

struct PointF {
    float x;
    float y;
};

struct QuantizedTensor {
    const int8_t* data;
    int channels;
    int height;
    int width;
    int32_t zero_point;
    float scale;
};

struct ObbDetection {
    int class_id;
    float score;
    float center_x;
    float center_y;
    float width;
    float height;
    float angle;
    std::array<PointF, 4> source_corners;
};

struct ObbOcrInput {
    enum {
        kWidth = 160,
        kHeight = 48,
        kChannels = 3,
    };

    int resized_width;
    std::vector<uint8_t> bgr;
};

struct ObbPlateColorEvidence {
    uint64_t blue_score;
    uint64_t green_score;
    uint32_t colored_pixels;
};

bool UnpackObbNativeInt8(const rknn_tensor_attr& native_attr,
                         const rknn_tensor_attr& logical_attr,
                         const rknn_tensor_mem* memory,
                         std::vector<int8_t>* output);

int DecodeAndNmsObb(const std::array<QuantizedTensor, 3>& branches,
                    const QuantizedTensor& angle,
                    float confidence_threshold,
                    float nms_threshold,
                    int maximum_detections,
                    std::vector<ObbDetection>* detections);

// Maps an OBB from the 640x640 letterboxed model input back to the PCIe source
// and produces the exact interleaved UINT8 BGR buffer consumed by PP-OCR.
bool PrepareObbOcrInput(const image_buffer_t& source_image,
                        const letterbox_t& letterbox,
                        ObbDetection* detection,
                        ObbOcrInput* output);

// Returns a half-open axis-aligned display/Tracker box enclosing all four OBB
// corners. The current renderer deliberately remains rectangle-only.
image_rect_t ObbSourceBoundingBox(const ObbDetection& detection,
                                  int image_width,
                                  int image_height);

// Uses saturated pixels from the rectified, unpadded plate area. The returned
// class is 0 for blue, 1 for green, and -1 when color evidence is inconclusive.
ObbPlateColorEvidence AnalyzeObbPlateColor(const ObbOcrInput& input);
int DominantObbPlateColor(const ObbPlateColorEvidence& evidence);

#endif  // OBB_POSTPROCESS_H
