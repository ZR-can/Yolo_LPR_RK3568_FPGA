#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "obb_postprocess.h"

namespace {

void SetCandidate(std::vector<int8_t>* branch,
                  int height,
                  int width,
                  int y,
                  int x,
                  int class_id) {
    const int spatial = height * width;
    const int position = y * width + x;
    for (int side = 0; side < 4; ++side) {
        const int target_bin = side % 2 == 0 ? 2 : 1;
        for (int bin = 0; bin < 16; ++bin) {
            (*branch)[(side * 16 + bin) * spatial + position] =
                static_cast<int8_t>(bin == target_bin ? 100 : -100);
        }
    }
    (*branch)[(64 + class_id) * spatial + position] = 30;
}

}  // namespace

int main() {
    // The RK3568 runtime exposes the logical [1,1,8400] angle tensor through
    // an internally aligned 67,200-byte NC1HWC2 buffer.
    rknn_tensor_attr native_angle = {};
    native_angle.type = RKNN_TENSOR_INT8;
    native_angle.fmt = RKNN_TENSOR_NC1HWC2;
    native_angle.n_dims = 5;
    native_angle.dims[0] = 1;
    native_angle.dims[1] = 1;
    native_angle.dims[2] = 8400;
    native_angle.dims[3] = 1;
    native_angle.dims[4] = 8;
    rknn_tensor_attr logical_angle = {};
    logical_angle.type = RKNN_TENSOR_INT8;
    logical_angle.n_dims = 3;
    logical_angle.dims[0] = 1;
    logical_angle.dims[1] = 1;
    logical_angle.dims[2] = 8400;
    logical_angle.n_elems = 8400;
    std::vector<int8_t> native_angle_data(67200, -1);
    for (int position = 0; position < 8400; ++position) {
        native_angle_data[position * 8] =
            static_cast<int8_t>(position % 127);
    }
    rknn_tensor_mem angle_memory = {};
    angle_memory.virt_addr = native_angle_data.data();
    angle_memory.size = static_cast<uint32_t>(native_angle_data.size());
    std::vector<int8_t> unpacked_angle;
    if (!UnpackObbNativeInt8(native_angle,
                             logical_angle,
                             &angle_memory,
                             &unpacked_angle) ||
        unpacked_angle.size() != 8400U || unpacked_angle[8399] != 17) {
        std::fprintf(stderr, "Aligned OBB angle output unpack test failed\n");
        return 1;
    }

    std::vector<int8_t> stride8(68 * 80 * 80, -100);
    std::vector<int8_t> stride16(68 * 40 * 40, -100);
    std::vector<int8_t> stride32(68 * 20 * 20, -100);
    std::vector<int8_t> angle_data(8400, 25);
    SetCandidate(&stride8, 80, 80, 10, 10, 0);
    // Cross-color candidates must survive same-class NMS so OCR/GA 36 can
    // resolve an INT8 score tie without discarding the correct plate type.
    stride8[(64 + 3) * 80 * 80 + 10 * 80 + 10] = 25;

    const std::array<QuantizedTensor, 3> branches = {{
        {stride8.data(), 68, 80, 80, 0, 0.1f},
        {stride16.data(), 68, 40, 40, 0, 0.1f},
        {stride32.data(), 68, 20, 20, 0, 0.1f},
    }};
    const QuantizedTensor angle =
        {angle_data.data(), 1, 8400, 1, 0, 0.01f};
    std::vector<ObbDetection> detections;
    if (DecodeAndNmsObb(branches, angle, 0.55f, 0.55f, 128, &detections) != 0 ||
        detections.size() != 2U || detections[0].class_id != 0 ||
        detections[0].score < 0.9f ||
        std::fabs(detections[0].angle) > 1e-4f) {
        std::fprintf(stderr, "Decode/rotated-NMS test failed\n");
        return 1;
    }

    // BGR565 uses little-endian B5:G6:R5, matching the existing PP-OCR
    // cv::COLOR_BGR5652BGR input path. 0x001f is full red.
    std::vector<uint8_t> source_pixels(640 * 640 * 2);
    for (size_t offset = 0; offset < source_pixels.size(); offset += 2U) {
        source_pixels[offset] = 0x1f;
        source_pixels[offset + 1U] = 0x00;
    }
    image_buffer_t source = {};
    source.width = 640;
    source.height = 640;
    source.width_stride = 640;
    source.format = IMAGE_FORMAT_BGR565;
    source.virt_addr = source_pixels.data();
    source.size = static_cast<int>(source_pixels.size());
    letterbox_t letterbox = {};
    letterbox.scale = 1.0f;
    ObbOcrInput ocr_input;
    if (!PrepareObbOcrInput(source, letterbox, &detections[0], &ocr_input) ||
        ocr_input.bgr.size() != 160U * 48U * 3U ||
        ocr_input.resized_width != 96 || ocr_input.bgr[0] != 0U ||
        ocr_input.bgr[1] != 0U || ocr_input.bgr[2] != 255U) {
        std::fprintf(stderr, "BGR565 OBB-to-PP-OCR input test failed\n");
        return 1;
    }

    ObbOcrInput color_input;
    color_input.resized_width = ObbOcrInput::kWidth;
    color_input.bgr.resize(
        ObbOcrInput::kWidth * ObbOcrInput::kHeight * 3U);
    const auto fill_color = [&color_input](uint8_t blue,
                                          uint8_t green,
                                          uint8_t red) {
        for (size_t offset = 0; offset < color_input.bgr.size(); offset += 3U) {
            color_input.bgr[offset + 0U] = blue;
            color_input.bgr[offset + 1U] = green;
            color_input.bgr[offset + 2U] = red;
        }
    };
    fill_color(70U, 180U, 60U);
    if (DominantObbPlateColor(AnalyzeObbPlateColor(color_input)) != 1) {
        std::fprintf(stderr, "Green plate color evidence test failed\n");
        return 1;
    }
    fill_color(180U, 80U, 60U);
    if (DominantObbPlateColor(AnalyzeObbPlateColor(color_input)) != 0) {
        std::fprintf(stderr, "Blue plate color evidence test failed\n");
        return 1;
    }
    fill_color(128U, 128U, 128U);
    if (DominantObbPlateColor(AnalyzeObbPlateColor(color_input)) != -1) {
        std::fprintf(stderr, "Neutral plate color evidence test failed\n");
        return 1;
    }

    const image_rect_t box =
        ObbSourceBoundingBox(detections[0], source.width, source.height);
    if (box.left != 68 || box.top != 76 || box.right != 100 ||
        box.bottom != 92) {
        std::fprintf(stderr,
                     "OBB display bounding box failed: %d %d %d %d\n",
                     box.left,
                     box.top,
                     box.right,
                     box.bottom);
        return 1;
    }

    std::printf(
        "PASS detections=%zu score=%.6f ocr_bgr=48x160x3 "
        "resized_width=%d box=(%d,%d,%d,%d)\n",
        detections.size(),
        detections[0].score,
        ocr_input.resized_width,
        box.left,
        box.top,
        box.right,
        box.bottom);
    return 0;
}
