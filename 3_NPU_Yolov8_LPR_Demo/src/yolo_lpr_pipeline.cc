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
    unsigned int text_color;
};

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

    char text[256];
    out_results.clear();

    for (int i = 0; i < od_results.count; i++) {
        object_detect_result *det_result = &(od_results.results[i]);
            
        int x1 = std::max(0, det_result->box.left);
        int y1 = std::max(0, det_result->box.top);
        int x2 = std::min(src_image->width - 1, det_result->box.right);
        int y2 = std::min(src_image->height - 1, det_result->box.bottom);

        image_buffer_t crop_img;
        memset(&crop_img, 0, sizeof(image_buffer_t));
        crop_img.width = LPR_MODEL_WIDTH;
        crop_img.height = LPR_MODEL_HEIGHT;
        crop_img.format = IMAGE_FORMAT_RGB888;
        crop_img.size = crop_img.width * crop_img.height * 3;
        crop_img.virt_addr = (unsigned char *)malloc(crop_img.size);

        image_preprocess(*src_image, crop_img, x1, y1, x2, y2);

        lprnet_app_context_t* current_lpr_ctx = (det_result->cls_id == 1) ? &ctx->lprnet8_ctx : &ctx->lprnet7_ctx;
            
        lprnet_result lpr_res;
        inference_lprnet_model(current_lpr_ctx, &crop_img, &lpr_res);
            
        free(crop_img.virt_addr);

        correct_plate_string(lpr_res.plate_name, det_result->cls_id);
        
        printf("Plate [%s] @ (%d %d %d %d) %.3f -> Text: %s\n", 
            coco_cls_to_name(det_result->cls_id), x1, y1, x2, y2, det_result->prop, lpr_res.plate_name.c_str());
        
        std::string plate_str = lpr_res.plate_name;    
        const char* type_name_cn = "黑";
        unsigned int cpu_box_color = COLOR_BLACK; 
        unsigned int cpu_text_color = COLOR_RED; 
        
        switch (det_result->cls_id) {
            case 0: type_name_cn = "蓝"; cpu_box_color = COLOR_BLUE; break;
            case 1: type_name_cn = "绿"; cpu_box_color = COLOR_GREEN; break;
            case 2: type_name_cn = "黄"; cpu_box_color = COLOR_YELLOW; break;
            case 3: type_name_cn = "黑"; cpu_box_color = COLOR_BLACK; break;
        }
        
        if (plate_str.find("警") != std::string::npos) {
            type_name_cn = "白"; cpu_box_color = COLOR_WHITE; cpu_text_color = COLOR_RED;
        } else if (plate_str.find("学") != std::string::npos) {
            type_name_cn = "黄"; cpu_box_color = COLOR_YELLOW; cpu_text_color = COLOR_YELLOW;
        } else if (plate_str.find("港") != std::string::npos || plate_str.find("澳") != std::string::npos || 
                    plate_str.find("领") != std::string::npos || plate_str.find("使") != std::string::npos) {
            type_name_cn = "黑"; cpu_box_color = COLOR_BLACK; cpu_text_color = COLOR_WHITE;
        }

        PipelineResult res;
        res.left = x1;
        res.top = y1;
        res.right = x2;
        res.bottom = y2;
        res.confidence = det_result->prop;
        res.text_confidence = lpr_res.text_confidence;
        res.plate_name = plate_str;
        res.plate_type = type_name_cn;
        res.box_color = cpu_box_color;
        res.text_color = cpu_text_color;
        
        out_results.push_back(res);

        if (draw_on_image) {
            int box_h = y2 - y1;
            
            // 动态字号计算：基础比例跟随框高，限制在 [18, 24] 像素之间
            int dynamic_fontsize = std::max(18, std::min(24, box_h / 2));

            // 动态行高偏移：第一行紧贴框顶，第二行在其上方
            int offset_line1 = dynamic_fontsize;
            int offset_line2 = dynamic_fontsize * 2 + 2; 

            // 画检测框
            draw_rectangle(src_image, x1, y1, x2 - x1, box_h, cpu_box_color, 3);
            
            // 画第一行文字 (类型与置信度)，加入 std::max 保护防止 Y 坐标越界
            sprintf(text, "%s%.1f%%", type_name_cn, det_result->prop * 100);
            draw_text(src_image, text, x1, std::max(0, y1 - offset_line1), cpu_text_color, dynamic_fontsize);
            
            // 画第二行文字 (车牌号)
            sprintf(text, "%s", plate_str.c_str());
            draw_text(src_image, text, x1, std::max(0, y1 - offset_line2), cpu_text_color, dynamic_fontsize); 
        }
    }

    return 0;
}

int process_pipeline_preprocessed(YOLOLPRPipelineContext* ctx, std::vector<PipelineResult>& out_results, bool draw_on_image) {
    object_detect_result_list od_results;
    int ret = inference_yolov8_model_preprocessed(&ctx->yolo_ctx, &od_results);
    if (ret != 0) return ret;

    image_buffer_t rgb_model_img;
    memset(&rgb_model_img, 0, sizeof(image_buffer_t));
    rgb_model_img.width = ctx->yolo_ctx.model_width;
    rgb_model_img.height = ctx->yolo_ctx.model_height;
    rgb_model_img.format = IMAGE_FORMAT_RGB888;
    rgb_model_img.size = rgb_model_img.width * rgb_model_img.height * 3;
    rgb_model_img.virt_addr = (unsigned char*)ctx->yolo_ctx.input_mems[0]->virt_addr;

    char text[256];
    out_results.clear();

    for (int i = 0; i < od_results.count; i++) {
        object_detect_result *det_result = &(od_results.results[i]);
            
        int x1 = std::max(0, det_result->box.left);
        int y1 = std::max(0, det_result->box.top);
        int x2 = std::min(rgb_model_img.width - 1, det_result->box.right);
        int y2 = std::min(rgb_model_img.height - 1, det_result->box.bottom);

        image_buffer_t crop_img;
        memset(&crop_img, 0, sizeof(image_buffer_t));
        crop_img.width = LPR_MODEL_WIDTH;
        crop_img.height = LPR_MODEL_HEIGHT;
        crop_img.format = IMAGE_FORMAT_RGB888;
        crop_img.size = crop_img.width * crop_img.height * 3;
        crop_img.virt_addr = (unsigned char *)malloc(crop_img.size);

        image_preprocess(rgb_model_img, crop_img, x1, y1, x2, y2);

        lprnet_app_context_t* current_lpr_ctx = (det_result->cls_id == 1) ? &ctx->lprnet8_ctx : &ctx->lprnet7_ctx;
            
        lprnet_result lpr_res;
        inference_lprnet_model(current_lpr_ctx, &crop_img, &lpr_res);
            
        free(crop_img.virt_addr);

        correct_plate_string(lpr_res.plate_name, det_result->cls_id);
        
        printf("Plate [%s] @ (%d %d %d %d) %.3f -> Text: %s\n", 
            coco_cls_to_name(det_result->cls_id), x1, y1, x2, y2, det_result->prop, lpr_res.plate_name.c_str());
        
        std::string plate_str = lpr_res.plate_name;    
        const char* type_name_cn = "黑";
        unsigned int cpu_box_color = COLOR_BLACK; 
        unsigned int cpu_text_color = COLOR_RED; 
        
        switch (det_result->cls_id) {
            case 0: type_name_cn = "蓝"; cpu_box_color = COLOR_BLUE; break;
            case 1: type_name_cn = "绿"; cpu_box_color = COLOR_GREEN; break;
            case 2: type_name_cn = "黄"; cpu_box_color = COLOR_YELLOW; break;
            case 3: type_name_cn = "黑"; cpu_box_color = COLOR_BLACK; break;
        }
        
        if (plate_str.find("警") != std::string::npos) {
            type_name_cn = "白"; cpu_box_color = COLOR_WHITE; cpu_text_color = COLOR_RED;
        } else if (plate_str.find("学") != std::string::npos) {
            type_name_cn = "黄"; cpu_box_color = COLOR_YELLOW; cpu_text_color = COLOR_YELLOW;
        } else if (plate_str.find("港") != std::string::npos || plate_str.find("澳") != std::string::npos || 
                    plate_str.find("领") != std::string::npos || plate_str.find("使") != std::string::npos) {
            type_name_cn = "黑"; cpu_box_color = COLOR_BLACK; cpu_text_color = COLOR_WHITE;
        }

        PipelineResult res;
        res.left = x1;
        res.top = y1;
        res.right = x2;
        res.bottom = y2;
        res.confidence = det_result->prop; // YOLO 的框置信度
        res.text_confidence = lpr_res.text_confidence; // LPRNet 的字符识别置信度
        res.plate_name = plate_str;
        res.plate_type = type_name_cn;
        res.box_color = cpu_box_color;
        res.text_color = cpu_text_color;
        
        out_results.push_back(res);

        if (draw_on_image) {
            int box_h = y2 - y1;
            
            // 动态字号计算：基础比例跟随框高，限制在 [18, 24] 像素之间
            int dynamic_fontsize = std::max(18, std::min(24, box_h / 2));

            // 动态行高偏移：第一行紧贴框顶，第二行在其上方
            int offset_line1 = dynamic_fontsize;
            int offset_line2 = dynamic_fontsize * 2 + 2; 

            // 画检测框
            draw_rectangle(&rgb_model_img, x1, y1, x2 - x1, box_h, cpu_box_color, 3);
            
            // 画第一行文字 (类型与置信度)，加入 std::max 保护防止 Y 坐标越界
            sprintf(text, "%s%.1f%%", type_name_cn, det_result->prop * 100);
            draw_text(&rgb_model_img, text, x1, std::max(0, y1 - offset_line1), cpu_text_color, dynamic_fontsize);
            
            // 画第二行文字 (车牌号)
            sprintf(text, "%s", plate_str.c_str());
            draw_text(&rgb_model_img, text, x1, std::max(0, y1 - offset_line2), cpu_text_color, dynamic_fontsize); 
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