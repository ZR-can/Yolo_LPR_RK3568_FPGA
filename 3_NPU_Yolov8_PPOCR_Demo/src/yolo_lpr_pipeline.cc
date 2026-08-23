#include "yolo_lpr_pipeline.h"
#include "image_drawing.h"
#include "file_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <vector>
#include <string>

#define LPR_MODEL_WIDTH 94
#define LPR_MODEL_HEIGHT 24

struct PlateDisplayInfo {
    std::string plate_str;
    std::string type_name_cn;
    unsigned int box_color;
    unsigned int text_color;
};

static int clamp_int(int value, int low, int high) {
    return std::max(low, std::min(value, high));
}

static int measure_text_width_px(const std::string& text, int font_px) {
    int max_line_w = 0;
    int line_w = 0;

    for (size_t i = 0; i < text.size();) {
        unsigned char ch = (unsigned char)text[i];
        if (text[i] == '\n') {
            max_line_w = std::max(max_line_w, line_w);
            line_w = 0;
            ++i;
            continue;
        }

        if (ch >= 0xE0 && i + 2 < text.size()) {
            line_w += font_px;
            i += 3;
        } else {
            line_w += font_px / 2 + 2;
            ++i;
        }
    }

    return std::max(max_line_w, line_w);
}

static void draw_bold_text(image_buffer_t* image, const std::string& text, int x, int y,
                           unsigned int color, int font_px) {
    draw_text(image, text.c_str(), x, y, color, font_px);
    draw_text(image, text.c_str(), x + 1, y, color, font_px);
    draw_text(image, text.c_str(), x, y + 1, color, font_px);
}

static PlateDisplayInfo make_plate_display_info(const std::string& plate_str, int cls_id) {
    PlateDisplayInfo info;
    info.plate_str = plate_str;
    info.type_name_cn = "黑";
    info.box_color = COLOR_MAGENTA;
    info.text_color = COLOR_WHITE;

    switch (cls_id) {
        case 0: info.type_name_cn = "蓝"; break;
        case 1: info.type_name_cn = "绿"; break;
        case 2: info.type_name_cn = "黄"; break;
        case 3: info.type_name_cn = "黑"; break;
        default: break;
    }

    if (plate_str.find("警") != std::string::npos) {
        info.type_name_cn = "白";
    } else if (plate_str.find("学") != std::string::npos) {
        info.type_name_cn = "黄";
    } else if (plate_str.find("港") != std::string::npos || plate_str.find("澳") != std::string::npos ||
               plate_str.find("领") != std::string::npos || plate_str.find("使") != std::string::npos) {
        info.type_name_cn = "黑";
    }

    return info;
}

void draw_pipeline_result_overlay(image_buffer_t* image, const PipelineResult& result) {
    if (image == nullptr || image->virt_addr == nullptr || image->width <= 0 || image->height <= 0) {
        return;
    }

    const int img_w = image->width;
    const int img_h = image->height;
    int left = clamp_int(result.left, 0, img_w - 1);
    int top = clamp_int(result.top, 0, img_h - 1);
    int right = clamp_int(result.right, 0, img_w - 1);
    int bottom = clamp_int(result.bottom, 0, img_h - 1);

    if (right <= left || bottom <= top) {
        return;
    }

    const int box_w = right - left;
    const int box_h = bottom - top;

    draw_rectangle_alpha(image, left - 2, top - 2, box_w + 4, box_h + 4, COLOR_BLACK, 2, 120);
    draw_rectangle_alpha(image, left - 1, top - 1, box_w + 2, box_h + 2, COLOR_WHITE, 2, 255);
    draw_rectangle_alpha(image, left, top, box_w, box_h, COLOR_MAGENTA, 4, 255);

    const int plate_font_px = 32;
    const int meta_font_px = 26;
    const int pad_x = 8;
    const int pad_y = 4;
    const int text_gap = 10;
    const int box_gap = 4;
    const int panel_radius = 8;

    std::string plate_text = result.plate_name.empty() ? "-" : result.plate_name;
    std::string meta_text = result.plate_type;

    int plate_w = measure_text_width_px(plate_text, plate_font_px);
    int meta_w = measure_text_width_px(meta_text, meta_font_px);
    int row_h = std::max(plate_font_px * 2, meta_font_px * 2);
    int panel_w = pad_x * 2 + plate_w + text_gap + meta_w + 2;
    int panel_h = pad_y * 2 + row_h + 2;

    if (panel_w > img_w) {
        panel_w = img_w;
    }
    if (panel_h > img_h) {
        panel_h = img_h;
    }

    int panel_x = left;
    if (panel_x + panel_w > img_w) {
        panel_x = img_w - panel_w;
    }
    panel_x = std::max(0, panel_x);

    int panel_y = top - panel_h - box_gap;
    if (panel_y < 0) {
        panel_y = bottom + box_gap;
    }
    if (panel_y + panel_h > img_h) {
        panel_y = img_h - panel_h;
    }
    panel_y = std::max(0, panel_y);

    draw_filled_rounded_rectangle_alpha(image, panel_x, panel_y, panel_w, panel_h,
                                        panel_radius, COLOR_DARK_GRAY, 190);

    int plate_x = panel_x + pad_x;
    int plate_y = panel_y + pad_y;
    int meta_x = plate_x + plate_w + text_gap;
    int meta_y = plate_y + (plate_font_px * 2 - meta_font_px * 2) / 2;

    draw_bold_text(image, plate_text, plate_x, plate_y, COLOR_WHITE, plate_font_px);
    draw_bold_text(image, meta_text, meta_x, meta_y, COLOR_WHITE, meta_font_px);
}

int init_pipeline(const char* yolov8_path, const char* lprnet7_path, const char* lprnet8_path, YOLOLPRPipelineContext* ctx) {
    memset(ctx, 0, sizeof(YOLOLPRPipelineContext));
    init_post_process();

    int ret;
    ret = init_yolov8_model(yolov8_path, &ctx->yolo_ctx);
    if (ret != 0) {
        deinit_post_process();
        return ret;
    }

    ret = init_lprnet_model(lprnet7_path, &ctx->lprnet7_ctx);
    if (ret != 0) {
        release_yolov8_model(&ctx->yolo_ctx);
        deinit_post_process();
        return ret;
    }

    ret = init_lprnet_model(lprnet8_path, &ctx->lprnet8_ctx);
    if (ret != 0) {
        release_lprnet_model(&ctx->lprnet7_ctx);
        release_yolov8_model(&ctx->yolo_ctx);
        deinit_post_process();
        return ret;
    }

    return 0;
}

int process_pipeline(YOLOLPRPipelineContext* ctx, image_buffer_t* src_image, std::vector<PipelineResult>& out_results, bool draw_on_image) {
    object_detect_result_list od_results;
    int ret = inference_yolov8_model(&ctx->yolo_ctx, src_image, &od_results);
    if (ret != 0) return ret;

    out_results.clear();

    for (int i = 0; i < od_results.count; i++) {
        object_detect_result *det_result = &(od_results.results[i]);
            
        int x1 = std::max(0, det_result->box.left);
        int y1 = std::max(0, det_result->box.top);
        int x2 = std::min(src_image->width, det_result->box.right);
        int y2 = std::min(src_image->height, det_result->box.bottom);
        if (x2 <= x1 || y2 <= y1) {
            continue;
        }

        image_buffer_t crop_img;
        memset(&crop_img, 0, sizeof(image_buffer_t));
        crop_img.width = LPR_MODEL_WIDTH;
        crop_img.height = LPR_MODEL_HEIGHT;
        crop_img.format = IMAGE_FORMAT_RGB888;
        crop_img.size = crop_img.width * crop_img.height * 3;
        crop_img.virt_addr = (unsigned char *)malloc(crop_img.size);
        if (crop_img.virt_addr == nullptr) {
            continue;
        }

        if (image_preprocess(*src_image, crop_img, x1, y1, x2, y2) != 0) {
            free(crop_img.virt_addr);
            continue;
        }

        lprnet_app_context_t* current_lpr_ctx = (det_result->cls_id == 1) ? &ctx->lprnet8_ctx : &ctx->lprnet7_ctx;
            
        lprnet_result lpr_res;
        ret = inference_lprnet_model(current_lpr_ctx, &crop_img, &lpr_res);
            
        free(crop_img.virt_addr);

        if (ret != 0) {
            continue;
        }

        correct_plate_string(lpr_res.plate_name, det_result->cls_id);
        
        printf("Plate [%s] @ (%d %d %d %d) %.3f -> Text: %s\n", 
            coco_cls_to_name(det_result->cls_id), x1, y1, x2, y2, det_result->prop, lpr_res.plate_name.c_str());
        
        std::string plate_str = lpr_res.plate_name;
        PlateDisplayInfo display_info = make_plate_display_info(plate_str, det_result->cls_id);

        PipelineResult res;
        res.left = x1;
        res.top = y1;
        res.right = x2;
        res.bottom = y2;
        res.confidence = det_result->prop;
        res.text_confidence = lpr_res.text_confidence;
        res.plate_name = plate_str;
        res.plate_type = display_info.type_name_cn;
        res.box_color = display_info.box_color;
        res.text_color = display_info.text_color;
        
        out_results.push_back(res);

        if (draw_on_image) {
            draw_pipeline_result_overlay(src_image, res);
        }
    }

    return 0;
}

int process_pipeline_preprocessed(YOLOLPRPipelineContext* ctx, image_buffer_t* preprocessed_image,
                                  std::vector<PipelineResult>& out_results, bool draw_on_image) {
    if (ctx == nullptr || preprocessed_image == nullptr || preprocessed_image->virt_addr == nullptr) {
        return -1;
    }
    if (preprocessed_image->format != IMAGE_FORMAT_RGB888) {
        printf("process_pipeline_preprocessed expects RGB888 input, got format=%d\n", preprocessed_image->format);
        return -1;
    }

    object_detect_result_list od_results;
    int ret = inference_yolov8_model_preprocessed(&ctx->yolo_ctx, &od_results);
    if (ret != 0) return ret;

    image_buffer_t rgb_model_img = *preprocessed_image;
    if (rgb_model_img.width <= 0) {
        rgb_model_img.width = ctx->yolo_ctx.model_width;
    }
    if (rgb_model_img.height <= 0) {
        rgb_model_img.height = ctx->yolo_ctx.model_height;
    }
    if (rgb_model_img.size <= 0) {
        rgb_model_img.size = rgb_model_img.width * rgb_model_img.height * 3;
    }

    out_results.clear();

    for (int i = 0; i < od_results.count; i++) {
        object_detect_result *det_result = &(od_results.results[i]);
            
        int x1 = std::max(0, det_result->box.left);
        int y1 = std::max(0, det_result->box.top);
        int x2 = std::min(rgb_model_img.width, det_result->box.right);
        int y2 = std::min(rgb_model_img.height, det_result->box.bottom);
        if (x2 <= x1 || y2 <= y1) {
            continue;
        }

        image_buffer_t crop_img;
        memset(&crop_img, 0, sizeof(image_buffer_t));
        crop_img.width = LPR_MODEL_WIDTH;
        crop_img.height = LPR_MODEL_HEIGHT;
        crop_img.format = IMAGE_FORMAT_RGB888;
        crop_img.size = crop_img.width * crop_img.height * 3;
        crop_img.virt_addr = (unsigned char *)malloc(crop_img.size);
        if (crop_img.virt_addr == nullptr) {
            continue;
        }

        if (image_preprocess(rgb_model_img, crop_img, x1, y1, x2, y2) != 0) {
            free(crop_img.virt_addr);
            continue;
        }

        lprnet_app_context_t* current_lpr_ctx = (det_result->cls_id == 1) ? &ctx->lprnet8_ctx : &ctx->lprnet7_ctx;
            
        lprnet_result lpr_res;
        ret = inference_lprnet_model(current_lpr_ctx, &crop_img, &lpr_res);
            
        free(crop_img.virt_addr);

        if (ret != 0) {
            continue;
        }

        correct_plate_string(lpr_res.plate_name, det_result->cls_id);
        
        printf("Plate [%s] @ (%d %d %d %d) %.3f -> Text: %s\n", 
            coco_cls_to_name(det_result->cls_id), x1, y1, x2, y2, det_result->prop, lpr_res.plate_name.c_str());
        
        std::string plate_str = lpr_res.plate_name;
        PlateDisplayInfo display_info = make_plate_display_info(plate_str, det_result->cls_id);

        PipelineResult res;
        res.left = x1;
        res.top = y1;
        res.right = x2;
        res.bottom = y2;
        res.confidence = det_result->prop; // YOLO 的框置信度
        res.text_confidence = lpr_res.text_confidence; // LPRNet 的字符识别置信度
        res.plate_name = plate_str;
        res.plate_type = display_info.type_name_cn;
        res.box_color = display_info.box_color;
        res.text_color = display_info.text_color;
        
        out_results.push_back(res);

        if (draw_on_image) {
            draw_pipeline_result_overlay(&rgb_model_img, res);
        }

    }

    return 0;
}

void release_pipeline(YOLOLPRPipelineContext* ctx) {
    deinit_post_process();
    release_yolov8_model(&ctx->yolo_ctx);
    release_lprnet_model(&ctx->lprnet7_ctx);
    release_lprnet_model(&ctx->lprnet8_ctx);
}
