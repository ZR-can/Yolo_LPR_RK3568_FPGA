/*-------------------------------------------
                Includes
-------------------------------------------*/
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <iostream>
#include <vector>
#include <string>
#include <chrono>

#include "yolov8.h"
#include "lprnet.h"
#include "yolo_lpr_pipeline.h"
#include "image_utils.h"
#include "file_utils.h"
#include "image_drawing.h"
#include "drm_display.h" 
#include "opencv2/opencv.hpp"
#include "opencv2/core/utils/filesystem.hpp"

#if defined(RV1106_1103) 
    #include "dma_alloc.hpp"
#endif

// 性能测算工具类
class PerfTimer {
    std::chrono::high_resolution_clock::time_point start_t;
public:
    void start() { 
        start_t = std::chrono::high_resolution_clock::now(); 
    }
    double get_elapsed_ms() {
        auto end_t = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double, std::milli> elapsed = end_t - start_t;
        return elapsed.count();
    }
    void print(const char* name) {
        printf("[Performance] %s: %.3f ms\n", name, get_elapsed_ms());
    }
};

static int ensure_result_dir(const char* dir_path)
{
    if (cv::utils::fs::exists(dir_path)) return 0;
    if (!cv::utils::fs::createDirectories(dir_path)) {
        printf("create result directory failed: %s\n", dir_path);
        return -1;
    }
    return 0;
}

/*-------------------------------------------
                  Main Function
-------------------------------------------*/
int main(int argc, char **argv)
{
    if (argc != 5) {
        printf("Usage: %s <yolov8_model_path> <lprnet7_model_path> <lprnet8_model_path> <image_path>\n", argv[0]);
        return -1;
    }

    const char *yolov8_path = argv[1];
    const char *lprnet7_path = argv[2];
    const char *lprnet8_path = argv[3];
    const char *image_path = argv[4];
    
    std::vector<PipelineResult> results;
    int ret;
    // 性能测算变量声明
    PerfTimer total_timer;

    // 初始化YOLOLPRPipelineContext
    YOLOLPRPipelineContext pipeline_ctx;
    ret = init_pipeline(yolov8_path, lprnet7_path, lprnet8_path, &pipeline_ctx);
    if (ret != 0) goto out;

    image_buffer_t src_image;
    memset(&src_image, 0, sizeof(image_buffer_t));
    ret = read_image(image_path, &src_image);
    if (ret != 0) goto out;

#if defined(RV1106_1103) 
    ret = dma_buf_alloc(RV1106_CMA_HEAP_PATH, src_image.size, &pipeline_ctx.yolo_ctx.img_dma_buf.dma_buf_fd, 
                       (void **) & (pipeline_ctx.yolo_ctx.img_dma_buf.dma_buf_virt_addr));
    memcpy(pipeline_ctx.yolo_ctx.img_dma_buf.dma_buf_virt_addr, src_image.virt_addr, src_image.size);
    dma_sync_cpu_to_device(pipeline_ctx.yolo_ctx.img_dma_buf.dma_buf_fd);
    free(src_image.virt_addr);
    src_image.virt_addr = (unsigned char *)pipeline_ctx.yolo_ctx.img_dma_buf.dma_buf_virt_addr;
    src_image.fd = pipeline_ctx.yolo_ctx.img_dma_buf.dma_buf_fd;
    pipeline_ctx.yolo_ctx.img_dma_buf.size = src_image.size;
#endif
    {   
        
        // 开始测算处理一张图片的总耗时
        total_timer.start();

        // 处理图片
        ret = process_pipeline(&pipeline_ctx, &src_image, results, true);
        if (ret != 0) goto out;

        // 测算结束：处理一张图片的总耗时
        total_timer.print("Total Processing Time per Image");
        
        // ==========================================
        // DRM 显示部分：直接使用现有的接口
        // ==========================================
        
        DrmDisplay disp;
        memset(&disp, 0, sizeof(DrmDisplay));
        
        if (drm_display_init(&disp, 1920, 1080) == 0) {
            // 显示前刷新帧缓冲区（填充黑色清空）
            if (disp.map != nullptr && disp.size > 0) {
                memset(disp.map, 0, disp.size); // 填充0 = 黑色，清空缓冲区
                printf("[Display] Frame buffer cleared (refreshed) before presenting image.\n");
            }
            
            // 显示处理后的图片
            drm_display_present(&disp, &src_image);
            printf("[Display] Image presented on screen successfully.\n");
            printf("[Display] Press [ENTER] to save image and exit...\n");
            getchar(); // 阻塞以查看屏幕显示
            drm_display_deinit(&disp);
        } else {
            printf("[Display] Warning: Failed to initialize DRM display.\n");
        }
        // ==========================================    
        if (ensure_result_dir("result") != 0) goto out;

        {
            std::string input_path_str(image_path);
            size_t last_slash_idx = input_path_str.find_last_of("\\/");
            std::string filename = (last_slash_idx == std::string::npos) ? input_path_str : input_path_str.substr(last_slash_idx + 1);
            size_t last_dot_idx = filename.find_last_of(".");
            std::string basename = (last_dot_idx == std::string::npos) ? filename : filename.substr(0, last_dot_idx);

            char out_path[256];
            snprintf(out_path, sizeof(out_path), "result/%s_out.png", basename.c_str());
            write_image(out_path, &src_image);
        }
    }
out:
    deinit_post_process();
    release_yolov8_model(&pipeline_ctx.yolo_ctx);
    release_lprnet_model(&pipeline_ctx.lprnet7_ctx);
    release_lprnet_model(&pipeline_ctx.lprnet8_ctx);

    if (src_image.virt_addr != NULL) {
#if defined(RV1106_1103) 
        dma_buf_free(pipeline_ctx.yolo_ctx.img_dma_buf.size, &pipeline_ctx.yolo_ctx.img_dma_buf.dma_buf_fd, 
                     pipeline_ctx.yolo_ctx.img_dma_buf.dma_buf_virt_addr);
#else
        free(src_image.virt_addr);
#endif
    }
    return 0;
}