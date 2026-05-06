#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <mutex>

#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>

#include "yolo_lpr_pipeline.h" 
#include "image_utils.h"
#include "image_drawing.h"
#include "mpp_decoder.h"
#include "drm_display.h"
#include "simple_tracker.h"

#define DEFAULT_FPS 30
#define FILE_READ_CHUNK (512 * 1024)

// 帧处理上下文，管理 Pipeline 句柄、控制标记及性能统计
typedef struct FrameProcessContext {
    YOLOLPRPipelineContext pipeline_ctx;

    DrmDisplay drm_display;
    bool drm_initialized;
    image_buffer_t save_rgb_frame;
    SimplePlateTracker tracker;
    int target_fps;
    int last_result_frame_id;
    rknn_tensor_mem* yolo_input_mems[2];
    bool yolo_input_busy[2];
    int yolo_input_index;
    std::string input_path;
    int frame_index;
    int save_interval;

    // --- 细分性能统计指标 ---
    unsigned long long perf_total_frames;      // 总帧数
    unsigned long long perf_det_frames;        // 触发推理的帧数
    unsigned long long perf_total_lpr_count;   // 总识别车牌数
    double perf_total_decode_ms;       // 总解码耗时
    double perf_total_convert_ms;      // 总 RGA 预处理(NV12->RGB)耗时
    double perf_total_npu_infer_ms;    // 总 NPU pipeline 推理耗时
    double perf_total_ui_drawing_ms;   // 总 UI 绘制与追踪耗时
    double perf_total_present_ms;      // 总 DRM 送显耗时(NV12 转 RGBA8888并缩放至屏幕分辨率送显)
    double perf_total_end2end_ms;      // 总端到端延迟
    unsigned long long perf_start_ms;
} FrameProcessContext;

static volatile int g_should_stop = 0;

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
};

static unsigned long long now_ms() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (unsigned long long)tv.tv_sec * 1000ULL + (unsigned long long)tv.tv_usec / 1000ULL;
}

// ---------------------------------------------------------
// 共享推理输入缓冲区索引（主线程写入，推理线程读取）
// ---------------------------------------------------------
struct SharedInfer {
    int frame_id = -1;
    int buf_index = 0;
    bool ready = false;
    std::mutex mtx;
} g_infer;

// ---------------------------------------------------------
// 共享最新的 Pipeline 推理结果（推理线程写入，主线程读取）
// ---------------------------------------------------------
struct SharedResult {
    std::vector<PipelineResult> results;
    int frame_id = -1;
    std::mutex mtx;
} g_result;

static void handle_signal(int sig) {
    (void)sig;
    g_should_stop = 1;
}

static int detect_video_type(const std::string& input) {
    if (input.find("265") != std::string::npos || input.find("hevc") != std::string::npos ||
        input.compare(input.size() - 5, 5, ".h265") == 0) {
        return 265;
    }
    return 264;
}

static int ensure_result_dir(const char* dir_path) {
    struct stat st;
    if (stat(dir_path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 0 : -1;
    }
    if (mkdir(dir_path, 0755) != 0 && errno != EEXIST) {
        printf("ERROR: Failed to create result directory: %s (errno=%d)\n", dir_path, errno);
        return -1;
    }
    return 0;
}

static image_format_t map_mpp_format(int format) {
    int fmt = (format & MPP_FRAME_FMT_MASK);
    if (fmt == MPP_FMT_YUV420SP) return IMAGE_FORMAT_YUV420SP_NV12;
    if (fmt == MPP_FMT_YUV420SP_VU) return IMAGE_FORMAT_YUV420SP_NV21;
    return IMAGE_FORMAT_GRAY8;
}



// 视频写出初始化与处理
// ==================== 推理子线程 ====================
static void inference_thread_func(FrameProcessContext* ctx) {
    printf("[Infer Thread] Started.\n");
    int last_frame_id = -1;

    while (!g_should_stop) {
        int current_frame_id = -1;
        int buf_index = 0;
        {
            std::lock_guard<std::mutex> lock(g_infer.mtx);
            if (g_infer.ready) {
                current_frame_id = g_infer.frame_id;
                buf_index = g_infer.buf_index;
                g_infer.ready = false;
            }
        }

        if (current_frame_id < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        last_frame_id = current_frame_id;

        if ((current_frame_id % 2) != 0) {
            ctx->yolo_input_busy[buf_index] = false;
            continue;
        }

        rknn_tensor_mem* input_mem = ctx->yolo_input_mems[buf_index];
        if (input_mem == NULL) {
            ctx->yolo_input_busy[buf_index] = false;
            continue;
        }

        rknn_set_io_mem(ctx->pipeline_ctx.yolo_ctx.rknn_ctx, input_mem, &ctx->pipeline_ctx.yolo_ctx.input_native_attrs[0]);

        // 2. 调用 Pipeline 进行端到端推理分析
        std::vector<PipelineResult> local_results;
        PerfTimer npu_timer;
        npu_timer.start();
        // 传递 false 避免在推理线程执行绘制，以保证主线程绘制的实时性和线程安全
        if (process_pipeline_preprocessed(&ctx->pipeline_ctx, local_results, false) == 0) {
            // 3. 将约束提取的结果同步至共享区
            std::lock_guard<std::mutex> lock(g_result.mtx);
            g_result.results = local_results;
            g_result.frame_id = current_frame_id;
        }
        ctx->perf_total_npu_infer_ms += npu_timer.get_elapsed_ms();

        ctx->yolo_input_busy[buf_index] = false;
    }
    printf("[Infer Thread] Exited.\n");
}


// 单帧结果渲染与输出
static void process_one_frame(FrameProcessContext* ctx, image_buffer_t* target_img, int frame_id) {
    PerfTimer total_timer;
    total_timer.start();

    std::vector<PipelineResult> current_results;
    int result_frame_id = -1;
    {
        std::lock_guard<std::mutex> lock(g_result.mtx);
        current_results = g_result.results;
        result_frame_id = g_result.frame_id;
    }

    if (result_frame_id >= 0 && result_frame_id != ctx->last_result_frame_id) {
        ctx->tracker.update(current_results, result_frame_id);
        ctx->last_result_frame_id = result_frame_id;
        ctx->perf_det_frames++;
        ctx->perf_total_lpr_count += current_results.size();
    }

    PipelineResult tracked_res;
    std::vector<PipelineResult> tracked_results;
    ctx->tracker.predict(frame_id, tracked_results);

    for (const auto& tracked_res : tracked_results) {
        // --- 坐标缩放映射 ---
        float scale_x = (float)target_img->width / ctx->pipeline_ctx.yolo_ctx.model_width;
        float scale_y = (float)target_img->height / ctx->pipeline_ctx.yolo_ctx.model_height;

        int draw_left   = (int)(tracked_res.left * scale_x);
        int draw_top    = (int)(tracked_res.top * scale_y);
        int draw_right  = (int)(tracked_res.right * scale_x);
        int draw_bottom = (int)(tracked_res.bottom * scale_y);

        draw_left   = std::max(0, std::min(draw_left, target_img->width - 1));
        draw_top    = std::max(0, std::min(draw_top, target_img->height - 1));
        draw_right  = std::max(0, std::min(draw_right, target_img->width - 1));
        draw_bottom = std::max(0, std::min(draw_bottom, target_img->height - 1));

        int draw_w = draw_right - draw_left;
        int draw_h = draw_bottom - draw_top;

        draw_rectangle(target_img, draw_left, draw_top, draw_w, draw_h, tracked_res.box_color, 3);

        int dynamic_fontsize = std::max(18, std::min(24, draw_h / 2));
        int offset_line1 = dynamic_fontsize;
        int offset_line2 = dynamic_fontsize * 2 + 2;

        char text_buf[256];
        snprintf(text_buf, sizeof(text_buf), "%s%.1f%%", tracked_res.plate_type.c_str(), tracked_res.confidence * 100);
        draw_text(target_img, text_buf, draw_left, std::max(0, draw_top - offset_line1), tracked_res.text_color, dynamic_fontsize);
        snprintf(text_buf, sizeof(text_buf), "%s", tracked_res.plate_name.c_str());
        draw_text(target_img, text_buf, draw_left, std::max(0, draw_top - offset_line2), tracked_res.text_color, dynamic_fontsize);
    }
    // 抽帧保存单帧逻辑
    if (ctx->save_interval > 0 && (ctx->frame_index % ctx->save_interval) == 0) {
            char out_path[128];
            snprintf(out_path, sizeof(out_path), "result/%s_frame_%06d.jpg", 
                ctx->input_path.c_str(), // 必须调用 .c_str()
                ctx->frame_index);

            // 1. 检查并准备 RGB888 缓冲区
            if (ctx->save_rgb_frame.virt_addr == nullptr || 
                ctx->save_rgb_frame.width != target_img->width || 
                ctx->save_rgb_frame.height != target_img->height) {
                
                if (ctx->save_rgb_frame.virt_addr) free(ctx->save_rgb_frame.virt_addr);
                
                ctx->save_rgb_frame.width = target_img->width;
                ctx->save_rgb_frame.height = target_img->height;
                ctx->save_rgb_frame.format = IMAGE_FORMAT_RGB888;
                ctx->save_rgb_frame.size = target_img->width * target_img->height * 3;
                ctx->save_rgb_frame.virt_addr = (unsigned char*)malloc(ctx->save_rgb_frame.size);
            }

            if (ctx->save_rgb_frame.virt_addr) {
                // 2. 调用 RGA 将带框的显存 (RGBA8888) 转换为保存所需的 RGB888
                image_buffer_t dst_rgb = ctx->save_rgb_frame;
                dst_rgb.fd = 0; // 输出到普通内存
                
                int conv_ret = convert_image(target_img, &dst_rgb, NULL, NULL, 0);
                
                if (conv_ret == 0) {
                    // 3. 执行写文件 (此时格式已符合 RGB888 要求)
                    if (write_image(out_path, &dst_rgb) == 0) {
                        printf("DRM Frame Saved: %s (RGBA -> RGB via RGA)\n", out_path);
                    }
                } else {
                    printf("Error: RGA failed to convert DRM frame for saving.\n");
                }
            }
        }

    ctx->perf_total_ui_drawing_ms += total_timer.get_elapsed_ms();
}

// 硬件解码器回调
static void on_decoder_frame(void* userdata, int width_stride, int height_stride, int width, int height,
                             int format, int fd, void* data) {
    FrameProcessContext* ctx = (FrameProcessContext*)userdata;
    if (!ctx || !data || g_should_stop) return;

    unsigned long long frame_begin_ms = now_ms();
    image_format_t src_fmt = map_mpp_format(format);

    // 1. 直接包装 MPP 的硬件零拷贝 Buffer
    image_buffer_t mpp_img;
    memset(&mpp_img, 0, sizeof(mpp_img));
    mpp_img.width = width;
    mpp_img.height = height;
    mpp_img.width_stride = width_stride;
    mpp_img.height_stride = height_stride;
    mpp_img.format = src_fmt;
    mpp_img.fd = fd; 
    mpp_img.virt_addr = (unsigned char*)data;

    // --- RGA 送入 YOLO 推理 (零拷贝) ---
    int next_index = (ctx->yolo_input_index + 1) % 2;
    if (ctx->yolo_input_mems[1] == NULL) next_index = 0;
    
    if (!ctx->yolo_input_busy[next_index]) {
        ctx->yolo_input_busy[next_index] = true;
        ctx->yolo_input_index = next_index;

        image_buffer_t dst_fd_img;
        memset(&dst_fd_img, 0, sizeof(dst_fd_img));
        dst_fd_img.width = ctx->pipeline_ctx.yolo_ctx.model_width;
        dst_fd_img.height = ctx->pipeline_ctx.yolo_ctx.model_height;
        dst_fd_img.width_stride = ctx->pipeline_ctx.yolo_ctx.model_width;
        dst_fd_img.height_stride = ctx->pipeline_ctx.yolo_ctx.model_height;
        dst_fd_img.format = IMAGE_FORMAT_RGB888;
        dst_fd_img.fd = ctx->yolo_input_mems[next_index]->fd;

        unsigned long long convert_begin_ms = now_ms();
        // RGA 直接从 MPP fd 转换到 NPU fd
        int conv_ret = convert_image(&mpp_img, &dst_fd_img, NULL, NULL, 0);
        ctx->perf_total_convert_ms += (double)(now_ms() - convert_begin_ms);

        if (conv_ret == 0) {
            std::lock_guard<std::mutex> lock(g_infer.mtx);
            g_infer.frame_id = ctx->frame_index;
            g_infer.buf_index = next_index;
            g_infer.ready = true;
        } else {
            ctx->yolo_input_busy[next_index] = false;
        }
    }

    // --- DRM 显示与 CPU 显存直画 ---
    if (!ctx->drm_initialized) {
        if (drm_display_init(&ctx->drm_display, width, height) == 0) {
            ctx->drm_initialized = true;
        }
    }

    if (ctx->drm_initialized) {
        // 步骤 A: RGA 硬件级零拷贝 (MPP fd -> DRM dmabuf_fd)
        PerfTimer present_timer;
        present_timer.start();
        drm_display_present_NV12(&ctx->drm_display, &mpp_img);
        ctx->perf_total_present_ms += present_timer.get_elapsed_ms();

        // 步骤 B: 包装 DRM 的映射内存 (RGBA8888 格式)
        image_buffer_t drm_img;
        memset(&drm_img, 0, sizeof(drm_img));
        drm_img.width = ctx->drm_display.mode_width;
        drm_img.height = ctx->drm_display.mode_height;
        drm_img.width_stride = ctx->drm_display.pitch / 4; 
        drm_img.height_stride = ctx->drm_display.mode_height;
        drm_img.format = IMAGE_FORMAT_RGBA8888;
        drm_img.fd = ctx->drm_display.dmabuf_fd;
        drm_img.virt_addr = (unsigned char*)ctx->drm_display.map;

        // 步骤 C: CPU 直接操作显存绘制跟踪框和文字
        process_one_frame(ctx, &drm_img, ctx->frame_index);
    }

    ctx->perf_total_frames++;
    ctx->perf_total_end2end_ms += (double)(now_ms() - frame_begin_ms);
    ctx->frame_index++;
}

// 解析裸码流文件
static int decode_raw_h26x_file(const char* input_path, MppDecoder* decoder, FrameProcessContext* ctx) {
    printf("\n[RAW H.264/H.265] Opening file: %s\n", input_path);
    FILE* fp = fopen(input_path, "rb");
    if (!fp) return -1;

    unsigned char* pkt = (unsigned char*)malloc(FILE_READ_CHUNK);
    if (!pkt) { fclose(fp); return -1; }

    while (!g_should_stop) {
        size_t n = fread(pkt, 1, FILE_READ_CHUNK, fp);
        if (n == 0) break;

        int eos = feof(fp) ? 1 : 0;
        PerfTimer decode_timer;
        decode_timer.start();
        decoder->Decode(pkt, (int)n, eos);
        if (ctx) {
            ctx->perf_total_decode_ms += decode_timer.get_elapsed_ms();
        }
        
        if (eos) break;
    }

    free(pkt);
    fclose(fp);
    return 0;
}

int main(int argc, char** argv) {
    printf("========================================\n");
    printf("    Rockchip Pipeline Real-time System  \n");
    printf("    PC H264/H265 File Decoder Version   \n");
    printf("========================================\n\n");

    if (argc != 5 && argc != 6) {
        printf("Usage: %s <yolov8_model> <lprnet7_model> <lprnet8_model> <input.h264/.h265> [save_interval]\n", argv[0]);
        return -1;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    const char* yolov8_path = argv[1];
    const char* lprnet7_path = argv[2];
    const char* lprnet8_path = argv[3];
    const char* input_path = argv[4];
    int save_interval = (argc == 6) ? atoi(argv[5]) : 0;

    FrameProcessContext frame_ctx{};
    frame_ctx.frame_index = 0;
    frame_ctx.save_interval = save_interval;
    std::string s = input_path;
    // 假设 s 是输入路径 "data/video.mp4"
    std::string raw_path = s;
    size_t last_slash = raw_path.find_last_of("/\\");
    std::string filename = (last_slash == std::string::npos) ? raw_path : raw_path.substr(last_slash + 1);
    size_t last_dot = filename.find_last_of('.');
    if (last_dot != std::string::npos) {
        filename = filename.substr(0, last_dot);
    }
    // 确保 frame_ctx.input_path 是 std::string 类型
    frame_ctx.input_path = filename;
    frame_ctx.perf_total_frames = 0;
    frame_ctx.perf_det_frames = 0;
    frame_ctx.perf_total_lpr_count = 0;
    frame_ctx.perf_total_decode_ms = 0.0;
    frame_ctx.perf_total_convert_ms = 0.0;
    frame_ctx.perf_total_npu_infer_ms = 0.0;
    frame_ctx.perf_total_ui_drawing_ms = 0.0;
    frame_ctx.perf_total_present_ms = 0.0;
    frame_ctx.perf_total_end2end_ms = 0.0;
    frame_ctx.perf_start_ms = now_ms();
    frame_ctx.drm_initialized = false;
    frame_ctx.target_fps = DEFAULT_FPS;
    frame_ctx.last_result_frame_id = -1;
    frame_ctx.yolo_input_index = 0;
    frame_ctx.yolo_input_busy[0] = false;
    frame_ctx.yolo_input_busy[1] = false;
    frame_ctx.yolo_input_mems[0] = nullptr;
    frame_ctx.yolo_input_mems[1] = nullptr;
    memset(&frame_ctx.save_rgb_frame, 0, sizeof(frame_ctx.save_rgb_frame));

    if (save_interval > 0) {
        ensure_result_dir("result");
    }

    // 1. 初始化Pipeline
    printf("\n========== Initializing Pipeline ==========\n");
    int ret = init_pipeline(yolov8_path, lprnet7_path, lprnet8_path, &frame_ctx.pipeline_ctx);
    if (ret != 0) {
        printf("ERROR: Pipeline initialization failed! ret=%d\n", ret);
        return ret;
    }
    printf("Pipeline initialized successfully.\n");
    // 直接复用初始化时底层自动分配的内存，作为缓冲的 [0] 号位
    frame_ctx.yolo_input_mems[0] = frame_ctx.pipeline_ctx.yolo_ctx.input_mems[0];
    // 由于单块内存无法实现异步并发，再手动申请第二块备用缓冲区内存，作为 [1] 号位
    frame_ctx.yolo_input_mems[1] = rknn_create_mem(frame_ctx.pipeline_ctx.yolo_ctx.rknn_ctx,
                                                   frame_ctx.pipeline_ctx.yolo_ctx.input_native_attrs[0].size_with_stride);
    if (frame_ctx.yolo_input_mems[1] == NULL) {
        printf("Warning: second YOLO input buffer alloc failed, using single buffer\n");
    }

    // 2. 初始化硬件解码器
    std::string input_str(input_path);
    int video_type = detect_video_type(input_str);
    MppDecoder decoder;
    
    printf("\n========== Initializing MPP Decoder ==========\n");
    decoder.Init(video_type, frame_ctx.target_fps, &frame_ctx);
    decoder.SetCallback(on_decoder_frame);

    // 3. 启动异步推理管线
    std::thread infer_thread(inference_thread_func, &frame_ctx);

    // 4. 阻塞式执行码流解析
    printf("\n========== Starting Video Decoding ==========\n");
    decode_raw_h26x_file(input_path, &decoder, &frame_ctx);

    // 5. 终止逻辑与资源回收
    g_should_stop = 1;
    if (infer_thread.joinable()) {
        infer_thread.join();
    }

    if (frame_ctx.drm_initialized) {
        drm_display_deinit(&frame_ctx.drm_display);
        frame_ctx.drm_initialized = false;
    }

    release_pipeline(&frame_ctx.pipeline_ctx);

    if (frame_ctx.yolo_input_mems[1]) {
        rknn_destroy_mem(frame_ctx.pipeline_ctx.yolo_ctx.rknn_ctx, frame_ctx.yolo_input_mems[1]);
        frame_ctx.yolo_input_mems[1] = nullptr;
    }

    if (frame_ctx.save_rgb_frame.virt_addr) {
        free(frame_ctx.save_rgb_frame.virt_addr);
        frame_ctx.save_rgb_frame.virt_addr = nullptr;
    }

    // 6. 输出性能分析报告
    if (frame_ctx.perf_total_frames > 0) {
        double run_sec = (double)(now_ms() - frame_ctx.perf_start_ms) / 1000.0;
        printf("\n========== Performance Summary ==========\n");
        printf("Total Frames:        %llu\n", frame_ctx.perf_total_frames);
        printf("System Throughput:   %.2f FPS\n", (double)frame_ctx.perf_total_frames / run_sec);
        printf("NPU Inference FPS:   %.2f FPS\n", (double)frame_ctx.perf_det_frames / run_sec);
        printf("Total Plates Found:  %llu\n", frame_ctx.perf_total_lpr_count);
        printf("-----------------------------------------\n");
        printf("Avg E2E Latency:     %.2f ms/frame\n", frame_ctx.perf_total_end2end_ms / frame_ctx.perf_total_frames);
        printf("Avg MPP Decode:      %.2f ms/frame\n", frame_ctx.perf_total_decode_ms / frame_ctx.perf_total_frames);
        printf("Avg RGA Convert:     %.2f ms/frame\n", frame_ctx.perf_total_convert_ms / frame_ctx.perf_total_frames);
        printf("Avg NPU Inference:   %.2f ms/frame\n", frame_ctx.perf_det_frames > 0 ? frame_ctx.perf_total_npu_infer_ms / frame_ctx.perf_det_frames : 0.0);
        printf("Avg UI Drawing:      %.2f ms/frame\n", frame_ctx.perf_total_ui_drawing_ms / frame_ctx.perf_total_frames);
        printf("Avg DRM Present:     %.2f ms/frame\n", frame_ctx.perf_total_present_ms / frame_ctx.perf_total_frames);
        printf("=========================================\n");
    }

    return 0;
}