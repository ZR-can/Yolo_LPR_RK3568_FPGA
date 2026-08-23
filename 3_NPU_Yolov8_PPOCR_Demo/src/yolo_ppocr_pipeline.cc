#include "yolo_ppocr_pipeline.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "image_drawing.h"
#include "image_utils.h"
#include "obb_postprocess.h"
#include "plate_rule.h"
#include "ppocr_retry_policy.h"

namespace {

struct PlateDisplayStyle {
    std::string type_name_cn;
    unsigned int box_color;
    unsigned int text_color;
};

// Final image-mode deployment thresholds selected for the RK3568 pipeline.
const float kObbConfidenceThreshold = 0.55f;
const float kObbNmsThreshold = 0.55f;
const int kObbMaximumDetections = 128;
const int kObbLetterboxFill = 114;
const float kObbColorConflictIou = 0.65f;
const float kObbColorConflictScoreDelta = 0.10f;

struct PreparedObbCandidate {
    ObbDetection detection;
    ObbOcrInput ocr_input;
    image_rect_t roi;
    ObbPlateColorEvidence color;
    bool suppressed;
};

const char* ObbClassName(int class_id) {
    const char* names[] = {"blue", "green", "yellow_single", "other"};
    return class_id >= 0 && class_id < 4 ? names[class_id] : "unknown";
}

float RectIou(const image_rect_t& first, const image_rect_t& second) {
    const int left = std::max(first.left, second.left);
    const int top = std::max(first.top, second.top);
    const int right = std::min(first.right, second.right);
    const int bottom = std::min(first.bottom, second.bottom);
    if (right <= left || bottom <= top) {
        return 0.0f;
    }
    const float intersection =
        static_cast<float>((right - left) * (bottom - top));
    const float first_area = static_cast<float>(
        (first.right - first.left) * (first.bottom - first.top));
    const float second_area = static_cast<float>(
        (second.right - second.left) * (second.bottom - second.top));
    const float union_area = first_area + second_area - intersection;
    return union_area > 0.0f ? intersection / union_area : 0.0f;
}

void ResolveObbBlueGreenConflicts(
    std::vector<PreparedObbCandidate>* candidates) {
    for (size_t first_index = 0; first_index < candidates->size(); ++first_index) {
        PreparedObbCandidate& first = (*candidates)[first_index];
        if (first.suppressed ||
            (first.detection.class_id != 0 && first.detection.class_id != 1)) {
            continue;
        }
        for (size_t second_index = first_index + 1;
             second_index < candidates->size();
             ++second_index) {
            PreparedObbCandidate& second = (*candidates)[second_index];
            if (second.suppressed ||
                first.detection.class_id == second.detection.class_id ||
                (second.detection.class_id != 0 &&
                 second.detection.class_id != 1)) {
                continue;
            }
            const float overlap = RectIou(first.roi, second.roi);
            const float score_delta = std::fabs(
                first.detection.score - second.detection.score);
            if (overlap < kObbColorConflictIou ||
                score_delta > kObbColorConflictScoreDelta) {
                continue;
            }

            ObbPlateColorEvidence combined;
            combined.blue_score =
                first.color.blue_score + second.color.blue_score;
            combined.green_score =
                first.color.green_score + second.color.green_score;
            combined.colored_pixels =
                first.color.colored_pixels + second.color.colored_pixels;
            const int selected_class = DominantObbPlateColor(combined);
            if (selected_class < 0) {
                first.suppressed = true;
                second.suppressed = true;
                std::printf(
                    "OBB color conflict dropped: iou=%.3f delta=%.4f "
                    "blue_evidence=%llu green_evidence=%llu selected=none\n",
                    overlap,
                    score_delta,
                    static_cast<unsigned long long>(combined.blue_score),
                    static_cast<unsigned long long>(combined.green_score));
                break;
            }

            PreparedObbCandidate& rejected =
                first.detection.class_id == selected_class ? second : first;
            rejected.suppressed = true;
            std::printf(
                "OBB color conflict resolved: iou=%.3f delta=%.4f "
                "blue_evidence=%llu green_evidence=%llu selected=%s\n",
                overlap,
                score_delta,
                static_cast<unsigned long long>(combined.blue_score),
                static_cast<unsigned long long>(combined.green_score),
                ObbClassName(selected_class));
            if (first.suppressed) {
                break;
            }
        }
    }
}

void ResetPipelineStats(YOLOPPOCRPipelineContext* ctx) {
    ctx->ppocr_primary_attempts = 0;
    ctx->ppocr_retry_attempts = 0;
    ctx->ppocr_retry_successes = 0;
    ctx->ppocr_retry_stable_vote_suppressions = 0;
    ctx->ppocr_retry_ms = 0.0;
}

bool HasObbBranchShape(const rknn_tensor_attr& attr,
                       int height,
                       int width) {
    return attr.n_dims == 4U && attr.dims[0] == 1U &&
           attr.dims[1] == 68U && attr.dims[2] == static_cast<uint32_t>(height) &&
           attr.dims[3] == static_cast<uint32_t>(width) &&
           attr.type == RKNN_TENSOR_INT8;
}

bool ValidateObbModel(const rknn_app_context_t& app) {
    if (app.io_num.n_input != 1U || app.io_num.n_output != 4U ||
        app.input_attrs == NULL || app.output_attrs == NULL ||
        app.output_native_attrs == NULL || app.input_mems[0] == NULL ||
        app.model_width != 640 || app.model_height != 640 ||
        app.model_channel != 3 ||
        !HasObbBranchShape(app.output_attrs[0], 80, 80) ||
        !HasObbBranchShape(app.output_attrs[1], 40, 40) ||
        !HasObbBranchShape(app.output_attrs[2], 20, 20)) {
        return false;
    }
    const rknn_tensor_attr& angle = app.output_attrs[3];
    if (angle.n_dims != 3U || angle.dims[0] != 1U ||
        angle.dims[1] != 1U || angle.dims[2] != 8400U ||
        angle.type != RKNN_TENSOR_INT8) {
        return false;
    }
    for (uint32_t index = 0; index < app.io_num.n_output; ++index) {
        if (app.output_mems[index] == NULL ||
            app.output_native_attrs[index].type != RKNN_TENSOR_INT8 ||
            app.output_native_attrs[index].qnt_type !=
                RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC) {
            return false;
        }
    }
    return true;
}

QuantizedTensor MakeObbTensor(const std::vector<int8_t>& data,
                              const rknn_tensor_attr& logical,
                              const rknn_tensor_attr& native) {
    QuantizedTensor tensor;
    tensor.data = data.data();
    tensor.channels = static_cast<int>(logical.dims[1]);
    tensor.height = static_cast<int>(logical.dims[2]);
    tensor.width = logical.n_dims == 4U
                       ? static_cast<int>(logical.dims[3])
                       : 1;
    tensor.zero_point = native.zp;
    tensor.scale = native.scale;
    return tensor;
}

int RunObbDetector(rknn_app_context_t* app,
                   image_buffer_t* source_image,
                   letterbox_t* letterbox,
                   std::vector<ObbDetection>* detections) {
    if (app == NULL || source_image == NULL || letterbox == NULL ||
        detections == NULL || !ValidateObbModel(*app)) {
        return -1;
    }

    image_buffer_t model_input;
    std::memset(&model_input, 0, sizeof(model_input));
    model_input.width = app->model_width;
    model_input.height = app->model_height;
    model_input.width_stride = app->model_width;
    model_input.height_stride = app->model_height;
    model_input.format = IMAGE_FORMAT_RGB888;
    model_input.size = app->model_width * app->model_height * app->model_channel;
    model_input.fd = app->input_mems[0]->fd;
    model_input.virt_addr =
        static_cast<unsigned char*>(app->input_mems[0]->virt_addr);
    std::memset(letterbox, 0, sizeof(*letterbox));
    if (convert_image_with_letterbox(source_image,
                                     &model_input,
                                     letterbox,
                                     static_cast<char>(kObbLetterboxFill)) != 0) {
        std::printf("OBB letterbox preprocessing failed\n");
        return -1;
    }

    int ret = rknn_run(app->rknn_ctx, NULL);
    if (ret != RKNN_SUCC) {
        std::printf("OBB rknn_run failed: ret=%d\n", ret);
        return ret;
    }

    std::array<std::vector<int8_t>, 4> logical_data;
    for (size_t index = 0; index < logical_data.size(); ++index) {
        if (!UnpackObbNativeInt8(app->output_native_attrs[index],
                                 app->output_attrs[index],
                                 app->output_mems[index],
                                 &logical_data[index])) {
            std::printf("OBB native output unpack failed: index=%zu\n", index);
            return -1;
        }
    }
    const std::array<QuantizedTensor, 3> branches = {{
        MakeObbTensor(logical_data[0],
                      app->output_attrs[0],
                      app->output_native_attrs[0]),
        MakeObbTensor(logical_data[1],
                      app->output_attrs[1],
                      app->output_native_attrs[1]),
        MakeObbTensor(logical_data[2],
                      app->output_attrs[2],
                      app->output_native_attrs[2]),
    }};
    const QuantizedTensor angle =
        MakeObbTensor(logical_data[3],
                      app->output_attrs[3],
                      app->output_native_attrs[3]);
    return DecodeAndNmsObb(branches,
                           angle,
                           kObbConfidenceThreshold,
                           kObbNmsThreshold,
                           kObbMaximumDetections,
                           detections);
}

bool HasPlateTail(const std::string& plate, const std::string& tail) {
    return plate.size() >= tail.size() &&
           plate.compare(plate.size() - tail.size(), tail.size(), tail) == 0;
}

PlateDisplayStyle MakePlateDisplayStyle(const std::string& plate, int class_id) {
    PlateDisplayStyle style;
    style.type_name_cn = "黑";
    style.box_color = COLOR_MAGENTA;
    style.text_color = COLOR_WHITE;

    switch (class_id) {
        case 0:
            style.type_name_cn = "蓝";
            break;
        case 1:
            style.type_name_cn = "绿";
            break;
        case 2:
            style.type_name_cn = "黄";
            break;
        case 3:
            style.type_name_cn = "黑";
            break;
        default:
            break;
    }

    if (HasPlateTail(plate, "警")) {
        style.type_name_cn = "白";
    } else if (HasPlateTail(plate, "学")) {
        style.type_name_cn = "黄";
    } else if (HasPlateTail(plate, "港") ||
               HasPlateTail(plate, "澳") ||
               HasPlateTail(plate, "领") ||
               HasPlateTail(plate, "使")) {
        style.type_name_cn = "黑";
    }
    return style;
}

}  // namespace

int init_ppocr_pipeline(const char* yolov8_path,
                        const char* ppocr_path,
                        const char* dictionary_path,
                        YOLOPPOCRPipelineContext* ctx) {
    if (yolov8_path == NULL || ppocr_path == NULL || dictionary_path == NULL ||
        ctx == NULL) {
        return -1;
    }

    ResetPipelineStats(ctx);

    int ret = init_post_process();
    if (ret != 0) {
        return ret;
    }

    ret = init_yolov8_model(yolov8_path, &ctx->yolo_ctx);
    if (ret != 0) {
        deinit_post_process();
        return ret;
    }

    ret = init_ppocr_rec_model(ppocr_path, dictionary_path, &ctx->ppocr_ctx);
    if (ret != 0) {
        release_yolov8_model(&ctx->yolo_ctx);
        deinit_post_process();
        return ret;
    }

    std::printf("PP-OCR plate correction rule: %s\n", plate_rule_version());
    return 0;
}

int process_ppocr_pipeline(YOLOPPOCRPipelineContext* ctx,
                           image_buffer_t* source_image,
                           std::vector<PipelineResult>& results) {
    if (ctx == NULL || source_image == NULL || source_image->virt_addr == NULL ||
        source_image->width <= 0 || source_image->height <= 0) {
        return -1;
    }

    object_detect_result_list detections;
    const int yolo_ret =
        inference_yolov8_model(&ctx->yolo_ctx, source_image, &detections);
    if (yolo_ret != 0) {
        return yolo_ret;
    }

    results.clear();
    bool retry_consumed = false;
    for (int i = 0; i < detections.count; ++i) {
        const object_detect_result& detection = detections.results[i];
        image_rect_t detected_roi;
        detected_roi.left = detection.box.left;
        detected_roi.top = detection.box.top;
        detected_roi.right = detection.box.right;
        detected_roi.bottom = detection.box.bottom;
        const image_rect_t roi = clamp_ppocr_roi(
            detected_roi, source_image->width, source_image->height);
        if (roi.right <= roi.left || roi.bottom <= roi.top) {
            continue;
        }

        ppocr_rec_result_t recognition;
        ppocr_rec_perf_t recognition_perf;
        ++ctx->ppocr_primary_attempts;
        const int ppocr_ret = inference_ppocr_rec_model_roi(
            &ctx->ppocr_ctx, source_image, &roi, &recognition, &recognition_perf);
        if (ppocr_ret != 0) {
            continue;
        }

        const bool is_green_plate = detection.cls_id == 1;
        const std::string primary_raw = recognition.text;
        const std::string corrected =
            correct_plate_prediction_for_pipeline(recognition.text,
                                                  is_green_plate);
        std::string plate =
            truncate_plate_prediction(corrected, is_green_plate);
        PlateDisplayStyle style =
            MakePlateDisplayStyle(plate, detection.cls_id);
        bool retry_attempted = false;
        bool retry_accepted = false;
        std::string retry_raw;

        if (should_retry_ppocr_plate(plate, is_green_plate) &&
            !retry_consumed) {
            const image_rect_t retry_roi = expand_ppocr_retry_roi(
                roi, source_image->width, source_image->height);
            if (!ppocr_roi_equal(roi, retry_roi)) {
                retry_consumed = true;
                retry_attempted = true;
                ++ctx->ppocr_retry_attempts;

                ppocr_rec_result_t retry_recognition;
                ppocr_rec_perf_t retry_perf;
                const std::chrono::steady_clock::time_point retry_start =
                    std::chrono::steady_clock::now();
                const int retry_ret = inference_ppocr_rec_model_roi(
                    &ctx->ppocr_ctx,
                    source_image,
                    &retry_roi,
                    &retry_recognition,
                    &retry_perf);
                const std::chrono::steady_clock::time_point retry_end =
                    std::chrono::steady_clock::now();
                ctx->ppocr_retry_ms +=
                    std::chrono::duration<double, std::milli>(
                        retry_end - retry_start).count();
                if (retry_ret == 0) {
                    retry_raw = retry_recognition.text;
                    const std::string retry_corrected =
                        correct_plate_prediction_for_pipeline(
                            retry_recognition.text, is_green_plate);
                    const std::string retry_plate =
                        truncate_plate_prediction(retry_corrected,
                                                  is_green_plate);
                    if (!should_retry_ppocr_plate(retry_plate,
                                                  is_green_plate)) {
                        recognition = retry_recognition;
                        plate = retry_plate;
                        style = MakePlateDisplayStyle(plate, detection.cls_id);
                        retry_accepted = true;
                        ++ctx->ppocr_retry_successes;
                    }
                }
            }
        }

        const bool final_is_valid =
            !should_retry_ppocr_plate(plate, is_green_plate);
        std::printf(
            "Plate [%s] @ (%d %d %d %d) %.3f -> "
            "PP-OCR primary=%s retry=%s final=%s score=%.4f "
            "retry_status=%s ga36_status=%s\n",
            coco_cls_to_name(detection.cls_id),
            roi.left,
            roi.top,
            roi.right,
            roi.bottom,
            detection.prop,
            primary_raw.c_str(),
            retry_raw.empty() ? "-" : retry_raw.c_str(),
            plate.c_str(),
            recognition.score,
            retry_accepted ? "accepted"
                           : (retry_attempted ? "rejected" : "not-needed"),
            final_is_valid ? "valid" : "invalid");

        PipelineResult result;
        result.left = roi.left;
        result.top = roi.top;
        result.right = roi.right;
        result.bottom = roi.bottom;
        result.confidence = detection.prop;
        result.text_confidence = recognition.score;
        result.plate_name = plate;
        result.plate_type = style.type_name_cn;
        result.box_color = style.box_color;
        result.text_color = style.text_color;
        results.push_back(result);
    }
    return 0;
}

void release_ppocr_pipeline(YOLOPPOCRPipelineContext* ctx) {
    if (ctx == NULL) {
        return;
    }
    release_ppocr_rec_model(&ctx->ppocr_ctx);
    release_yolov8_model(&ctx->yolo_ctx);
    deinit_post_process();
}

int init_obb_ppocr_pipeline(const char* yolov8_obb_path,
                            const char* ppocr_path,
                            const char* dictionary_path,
                            YOLOPPOCRPipelineContext* ctx) {
    if (yolov8_obb_path == NULL || ppocr_path == NULL ||
        dictionary_path == NULL || ctx == NULL) {
        return -1;
    }
    ResetPipelineStats(ctx);

    int ret = init_yolov8_model(yolov8_obb_path, &ctx->yolo_ctx);
    if (ret != 0) {
        return ret;
    }
    if (!ValidateObbModel(ctx->yolo_ctx)) {
        std::printf("OBB model tensor contract mismatch\n");
        release_yolov8_model(&ctx->yolo_ctx);
        return -1;
    }

    ret = init_ppocr_rec_model(ppocr_path, dictionary_path, &ctx->ppocr_ctx);
    if (ret != 0) {
        release_yolov8_model(&ctx->yolo_ctx);
        return ret;
    }

    std::printf("Image detector: YOLOv8 OBB, conf=%.2f, rotated_nms=%.2f\n",
                kObbConfidenceThreshold,
                kObbNmsThreshold);
    std::printf("PP-OCR plate correction rule: %s\n", plate_rule_version());
    return 0;
}

int process_obb_ppocr_pipeline(YOLOPPOCRPipelineContext* ctx,
                               image_buffer_t* source_image,
                               std::vector<PipelineResult>& results,
                               const std::vector<PipelineResult>&
                                   stable_retry_results) {
    if (ctx == NULL || source_image == NULL || source_image->virt_addr == NULL ||
        source_image->width <= 0 || source_image->height <= 0) {
        return -1;
    }

    letterbox_t letterbox;
    std::vector<ObbDetection> detections;
    const int yolo_ret =
        RunObbDetector(&ctx->yolo_ctx, source_image, &letterbox, &detections);
    if (yolo_ret != 0) {
        return yolo_ret;
    }

    std::vector<PreparedObbCandidate> prepared_candidates;
    prepared_candidates.reserve(detections.size());
    for (const ObbDetection& detection : detections) {
        PreparedObbCandidate candidate;
        candidate.detection = detection;
        candidate.suppressed = false;
        if (!PrepareObbOcrInput(
                *source_image,
                letterbox,
                &candidate.detection,
                &candidate.ocr_input)) {
            continue;
        }
        candidate.roi = ObbSourceBoundingBox(
            candidate.detection,
            source_image->width,
            source_image->height);
        if (candidate.roi.right <= candidate.roi.left ||
            candidate.roi.bottom <= candidate.roi.top) {
            continue;
        }
        candidate.color = AnalyzeObbPlateColor(candidate.ocr_input);
        prepared_candidates.push_back(candidate);
    }
    ResolveObbBlueGreenConflicts(&prepared_candidates);

    results.clear();
    bool retry_consumed = false;
    for (PreparedObbCandidate& candidate : prepared_candidates) {
        if (candidate.suppressed) {
            continue;
        }
        ObbDetection& detection = candidate.detection;
        ObbOcrInput& ocr_input = candidate.ocr_input;
        const image_rect_t& roi = candidate.roi;

        ppocr_rec_result_t recognition;
        ppocr_rec_perf_t recognition_perf;
        ++ctx->ppocr_primary_attempts;
        const int ppocr_ret = inference_ppocr_rec_model_prepared_bgr(
            &ctx->ppocr_ctx,
            ocr_input.bgr.data(),
            ocr_input.bgr.size(),
            &recognition,
            &recognition_perf);
        if (ppocr_ret != 0) {
            continue;
        }

        const bool is_green_plate = detection.class_id == 1;
        const std::string primary_raw = recognition.text;
        const std::string corrected =
            correct_plate_prediction_for_pipeline(recognition.text,
                                                  is_green_plate);
        std::string plate =
            truncate_plate_prediction(corrected, is_green_plate);
        PlateDisplayStyle style =
            MakePlateDisplayStyle(plate, detection.class_id);
        bool retry_attempted = false;
        bool retry_accepted = false;
        bool retry_suppressed = false;
        std::string retry_raw;

        // Keep the existing one-retry budget. The primary input is rotation
        // corrected; only an invalid primary falls back to a slightly expanded
        // axis-aligned source ROI.
        if (should_retry_ppocr_plate(plate, is_green_plate) &&
            !retry_consumed) {
            const image_rect_t retry_roi = expand_ppocr_retry_roi(
                roi, source_image->width, source_image->height);
            if (!ppocr_roi_equal(roi, retry_roi)) {
                retry_suppressed = should_suppress_static_image_retry(
                    roi, stable_retry_results);
                if (retry_suppressed) {
                    ++ctx->ppocr_retry_stable_vote_suppressions;
                } else {
                    retry_consumed = true;
                    retry_attempted = true;
                    ++ctx->ppocr_retry_attempts;

                    ppocr_rec_result_t retry_recognition;
                    ppocr_rec_perf_t retry_perf;
                    const std::chrono::steady_clock::time_point retry_start =
                        std::chrono::steady_clock::now();
                    const int retry_ret = inference_ppocr_rec_model_roi(
                        &ctx->ppocr_ctx,
                        source_image,
                        &retry_roi,
                        &retry_recognition,
                        &retry_perf);
                    const std::chrono::steady_clock::time_point retry_end =
                        std::chrono::steady_clock::now();
                    ctx->ppocr_retry_ms +=
                        std::chrono::duration<double, std::milli>(
                            retry_end - retry_start)
                            .count();
                    if (retry_ret == 0) {
                        retry_raw = retry_recognition.text;
                        const std::string retry_corrected =
                            correct_plate_prediction_for_pipeline(
                                retry_recognition.text, is_green_plate);
                        const std::string retry_plate =
                            truncate_plate_prediction(retry_corrected,
                                                      is_green_plate);
                        if (!should_retry_ppocr_plate(retry_plate,
                                                      is_green_plate)) {
                            recognition = retry_recognition;
                            plate = retry_plate;
                            style = MakePlateDisplayStyle(
                                plate, detection.class_id);
                            retry_accepted = true;
                            ++ctx->ppocr_retry_successes;
                        }
                    }
                }
            }
        }

        const bool final_is_valid =
            !should_retry_ppocr_plate(plate, is_green_plate);
        std::printf(
            "OBB Plate [%s] @ (%d %d %d %d) %.3f angle=%.2f -> "
            "PP-OCR primary=%s retry=%s final=%s score=%.4f "
            "retry_status=%s ga36_status=%s\n",
            ObbClassName(detection.class_id),
            roi.left,
            roi.top,
            roi.right,
            roi.bottom,
            detection.score,
            detection.angle * 180.0f / 3.1415927410125732f,
            primary_raw.c_str(),
            retry_raw.empty() ? "-" : retry_raw.c_str(),
            plate.c_str(),
            recognition.score,
            retry_accepted
                ? "accepted"
                : (retry_attempted
                       ? "rejected"
                       : (retry_suppressed
                              ? "suppressed-stable-vote"
                              : "not-needed")),
            final_is_valid ? "valid" : "invalid");

        PipelineResult result;
        result.left = roi.left;
        result.top = roi.top;
        result.right = roi.right;
        result.bottom = roi.bottom;
        result.confidence = detection.score;
        result.text_confidence = recognition.score;
        result.plate_name = plate;
        result.plate_type = style.type_name_cn;
        result.box_color = style.box_color;
        result.text_color = style.text_color;
        results.push_back(result);
    }
    return 0;
}

void release_obb_ppocr_pipeline(YOLOPPOCRPipelineContext* ctx) {
    if (ctx == NULL) {
        return;
    }
    release_ppocr_rec_model(&ctx->ppocr_ctx);
    release_yolov8_model(&ctx->yolo_ctx);
}
