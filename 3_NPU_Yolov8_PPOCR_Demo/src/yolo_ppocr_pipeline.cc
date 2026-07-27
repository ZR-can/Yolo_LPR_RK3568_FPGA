#include "yolo_ppocr_pipeline.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "image_drawing.h"
#include "plate_rule.h"
#include "ppocr_retry_policy.h"

namespace {

struct PlateDisplayStyle {
    std::string type_name_cn;
    unsigned int box_color;
    unsigned int text_color;
};

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

    ctx->ppocr_primary_attempts = 0;
    ctx->ppocr_retry_attempts = 0;
    ctx->ppocr_retry_successes = 0;
    ctx->ppocr_retry_ms = 0.0;

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
