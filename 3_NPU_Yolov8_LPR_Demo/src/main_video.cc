#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>

#include <signal.h>
#include <sys/time.h>
#include <unistd.h>

#include "yolo_lpr_pipeline.h" 
#include "image_utils.h"
#include "mpp_decoder.h"
#include "drm_display.h"
#include "rga_overlay_renderer.h"
#include "simple_tracker.h"

#define DEFAULT_FPS 30
#define FILE_READ_CHUNK (512 * 1024)
#define DISPLAY_QUEUE_CAPACITY 2

// 帧处理上下文，管理 Pipeline 句柄、控制标记及性能统计
typedef struct FrameProcessContext {
    YOLOLPRPipelineContext pipeline_ctx;

    DrmDisplay drm_display;
    bool drm_initialized;
    RgaOverlayRenderer overlay_renderer;
    MppBuffer displayed_mpp_buffer;
    SimplePlateTracker tracker;
    int target_fps;
    int last_result_frame_id;
    rknn_tensor_mem* yolo_input_mems[2];
    bool yolo_input_busy[2];
    int yolo_input_index;
    int frame_index;

    // --- 细分性能统计指标 ---
    unsigned long long perf_total_frames;      // 成功送显帧数
    unsigned long long perf_total_input_frames; // MPP 输出帧数
    unsigned long long perf_total_npu_jobs;
    unsigned long long perf_display_dropped_frames;
    unsigned long long perf_overlay_failures;
    unsigned long long perf_total_lpr_count;   // 总识别车牌数
    double perf_total_decode_ms;       // 总解码耗时
    double perf_total_convert_ms;      // 总 RGA 预处理(NV12->RGB)耗时
    double perf_total_npu_infer_ms;    // 总 NPU pipeline 推理耗时
    double perf_total_ui_drawing_ms;   // 总 UI 绘制与追踪耗时
    double perf_total_present_ms;      // 总 DRM 原子双 plane 提交耗时
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
struct InferJob {
    int frame_id;
    int buf_index;

    InferJob(int frame_id_value = -1, int buf_index_value = 0)
        : frame_id(frame_id_value), buf_index(buf_index_value) {}
};

struct SharedInfer {
    std::deque<InferJob> jobs;
    std::mutex mtx;
    std::condition_variable cv;
} g_infer;

struct DisplayJob {
    image_buffer_t video;
    MppBuffer buffer_ref;
    int frame_id;
    unsigned long long enqueue_ms;

    DisplayJob()
        : buffer_ref(NULL), frame_id(-1), enqueue_ms(0) {
        memset(&video, 0, sizeof(video));
    }
};

struct SharedDisplay {
    std::deque<DisplayJob> jobs;
    std::mutex mtx;
    std::condition_variable cv;
} g_display;

// ---------------------------------------------------------
// 共享最新的 Pipeline 推理结果（推理线程写入，主线程读取）
// ---------------------------------------------------------
struct SharedResult {
    std::vector<PipelineResult> results;
    int frame_id = -1;
    std::mutex mtx;
} g_result;

static int reserve_yolo_input_buffer(FrameProcessContext* ctx) {
    std::lock_guard<std::mutex> lock(g_infer.mtx);
    int buffer_count = (ctx->yolo_input_mems[1] != nullptr) ? 2 : 1;

    for (int i = 0; i < buffer_count; ++i) {
        int candidate = (ctx->yolo_input_index + 1 + i) % buffer_count;
        if (!ctx->yolo_input_busy[candidate]) {
            ctx->yolo_input_busy[candidate] = true;
            ctx->yolo_input_index = candidate;
            return candidate;
        }
    }

    return -1;
}

static void release_yolo_input_buffer(FrameProcessContext* ctx, int buf_index) {
    if (buf_index < 0 || buf_index >= 2) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_infer.mtx);
    ctx->yolo_input_busy[buf_index] = false;
}

static void enqueue_infer_job(FrameProcessContext* ctx, int frame_id, int buf_index) {
    if (buf_index < 0 || buf_index >= 2) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_infer.mtx);
        if (g_should_stop) {
            ctx->yolo_input_busy[buf_index] = false;
            return;
        }
        g_infer.jobs.push_back(InferJob(frame_id, buf_index));
    }
    g_infer.cv.notify_one();
}

static void release_display_job(DisplayJob* job) {
    if (job != nullptr && job->buffer_ref != NULL) {
        mpp_buffer_put(job->buffer_ref);
        job->buffer_ref = NULL;
    }
}

static void enqueue_display_job(FrameProcessContext* ctx, const image_buffer_t& video,
                                int frame_id, MppBuffer buffer_ref, unsigned long long enqueue_ms) {
    if (ctx == nullptr || buffer_ref == NULL) {
        if (buffer_ref != NULL) {
            mpp_buffer_put(buffer_ref);
        }
        return;
    }

    std::vector<MppBuffer> dropped_buffers;
    bool should_release = false;
    {
        std::lock_guard<std::mutex> lock(g_display.mtx);
        if (g_should_stop) {
            should_release = true;
        } else {
            while (g_display.jobs.size() >= DISPLAY_QUEUE_CAPACITY) {
                DisplayJob& dropped = g_display.jobs.front();
                dropped_buffers.push_back(dropped.buffer_ref);
                dropped.buffer_ref = NULL;
                g_display.jobs.pop_front();
                ctx->perf_display_dropped_frames++;
            }

            DisplayJob job;
            job.video = video;
            job.video.virt_addr = NULL;
            job.buffer_ref = buffer_ref;
            job.frame_id = frame_id;
            job.enqueue_ms = enqueue_ms;
            g_display.jobs.push_back(job);
        }
    }

    for (MppBuffer dropped_buffer : dropped_buffers) {
        if (dropped_buffer != NULL) {
            mpp_buffer_put(dropped_buffer);
        }
    }
    if (should_release) {
        mpp_buffer_put(buffer_ref);
        return;
    }
    g_display.cv.notify_one();
}

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

    while (true) {
        InferJob job;
        {
            std::unique_lock<std::mutex> lock(g_infer.mtx);
            g_infer.cv.wait(lock, [] {
                return g_should_stop || !g_infer.jobs.empty();
            });

            if (g_infer.jobs.empty()) {
                if (g_should_stop) {
                    break;
                }
                continue;
            }

            job = g_infer.jobs.front();
            g_infer.jobs.pop_front();
        }

        if (job.buf_index < 0 || job.buf_index >= 2) {
            continue;
        }

        rknn_tensor_mem* input_mem = ctx->yolo_input_mems[job.buf_index];
        if (input_mem == NULL) {
            release_yolo_input_buffer(ctx, job.buf_index);
            continue;
        }

        int ret = rknn_set_io_mem(ctx->pipeline_ctx.yolo_ctx.rknn_ctx, input_mem, &ctx->pipeline_ctx.yolo_ctx.input_native_attrs[0]);
        if (ret < 0) {
            printf("rknn_set_io_mem failed for YOLO input buffer %d, ret=%d\n", job.buf_index, ret);
            release_yolo_input_buffer(ctx, job.buf_index);
            continue;
        }

        image_buffer_t yolo_input_img;
        memset(&yolo_input_img, 0, sizeof(image_buffer_t));
        yolo_input_img.width = ctx->pipeline_ctx.yolo_ctx.model_width;
        yolo_input_img.height = ctx->pipeline_ctx.yolo_ctx.model_height;
        yolo_input_img.width_stride = ctx->pipeline_ctx.yolo_ctx.model_width;
        yolo_input_img.height_stride = ctx->pipeline_ctx.yolo_ctx.model_height;
        yolo_input_img.format = IMAGE_FORMAT_RGB888;
        yolo_input_img.size = yolo_input_img.width * yolo_input_img.height * 3;
        yolo_input_img.fd = input_mem->fd;
        yolo_input_img.virt_addr = (unsigned char*)input_mem->virt_addr;
        if (yolo_input_img.virt_addr == nullptr) {
            printf("YOLO input buffer %d has null virtual address\n", job.buf_index);
            release_yolo_input_buffer(ctx, job.buf_index);
            continue;
        }

        // 2. 调用 Pipeline 进行端到端推理分析
        std::vector<PipelineResult> local_results;
        PerfTimer npu_timer;
        npu_timer.start();
        // 传递 false 避免在推理线程执行绘制，以保证主线程绘制的实时性和线程安全
        if (process_pipeline_preprocessed(&ctx->pipeline_ctx, &yolo_input_img, local_results, false) == 0) {
            // 3. 将约束提取的结果同步至共享区
            std::lock_guard<std::mutex> lock(g_result.mtx);
            g_result.results = local_results;
            g_result.frame_id = job.frame_id;
            ctx->perf_total_npu_jobs++;
        }
        ctx->perf_total_npu_infer_ms += npu_timer.get_elapsed_ms();

        release_yolo_input_buffer(ctx, job.buf_index);
    }
    printf("[Infer Thread] Exited.\n");
}


// 单帧结果渲染与输出
static void process_one_frame(FrameProcessContext* ctx, image_buffer_t* ui_buffer, int frame_id) {
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
        ctx->perf_total_lpr_count += current_results.size();
    }

    std::vector<PipelineResult> tracked_results;
    ctx->tracker.predict(frame_id, tracked_results);

    std::vector<PipelineResult> draw_results;
    draw_results.reserve(tracked_results.size());

    for (const auto& tracked_res : tracked_results) {
        // --- 坐标缩放映射 ---
        float scale_x = (float)ui_buffer->width / ctx->pipeline_ctx.yolo_ctx.model_width;
        float scale_y = (float)ui_buffer->height / ctx->pipeline_ctx.yolo_ctx.model_height;

        int draw_left   = (int)(tracked_res.left * scale_x);
        int draw_top    = (int)(tracked_res.top * scale_y);
        int draw_right  = (int)(tracked_res.right * scale_x);
        int draw_bottom = (int)(tracked_res.bottom * scale_y);

        draw_left   = std::max(0, std::min(draw_left, ui_buffer->width - 1));
        draw_top    = std::max(0, std::min(draw_top, ui_buffer->height - 1));
        draw_right  = std::max(0, std::min(draw_right, ui_buffer->width - 1));
        draw_bottom = std::max(0, std::min(draw_bottom, ui_buffer->height - 1));

        if (draw_right <= draw_left || draw_bottom <= draw_top) {
            continue;
        }

        PipelineResult draw_res = tracked_res;
        draw_res.left = draw_left;
        draw_res.top = draw_top;
        draw_res.right = draw_right;
        draw_res.bottom = draw_bottom;
        draw_results.push_back(draw_res);
    }

    if (ctx->overlay_renderer.Render(ui_buffer, draw_results) != 0) {
        ctx->perf_overlay_failures++;
        if (ctx->perf_overlay_failures == 1 || (ctx->perf_overlay_failures % 120) == 0) {
            fprintf(stderr, "RGA overlay frame render failed (%llu failures)\n",
                    ctx->perf_overlay_failures);
        }
    }

    ctx->perf_total_ui_drawing_ms += total_timer.get_elapsed_ms();
}

static void display_thread_func(FrameProcessContext* ctx) {
    printf("[Display Thread] Started.\n");

    while (true) {
        DisplayJob job;
        {
            std::unique_lock<std::mutex> lock(g_display.mtx);
            g_display.cv.wait(lock, [] {
                return g_should_stop || !g_display.jobs.empty();
            });

            if (g_display.jobs.empty()) {
                if (g_should_stop) {
                    break;
                }
                continue;
            }

            job = g_display.jobs.front();
            g_display.jobs.pop_front();
        }

        if (!ctx->drm_initialized) {
            if (drm_display_init(&ctx->drm_display, job.video.width, job.video.height) == 0) {
                ctx->drm_initialized = ctx->overlay_renderer.Init(ctx->drm_display.mode_width,
                                                                    ctx->drm_display.mode_height) == 0;
                if (!ctx->drm_initialized) {
                    drm_display_deinit(&ctx->drm_display);
                }
            }
        }

        if (ctx->drm_initialized) {
            image_buffer_t ui_buffer;
            if (drm_display_get_ui_buffer(&ctx->drm_display, &ui_buffer) == 0) {
                process_one_frame(ctx, &ui_buffer, job.frame_id);

                PerfTimer present_timer;
                present_timer.start();
                if (drm_display_present_nv12(&ctx->drm_display, &job.video) == 0) {
                    ctx->perf_total_present_ms += present_timer.get_elapsed_ms();
                    if (ctx->displayed_mpp_buffer != NULL) {
                        mpp_buffer_put(ctx->displayed_mpp_buffer);
                    }
                    ctx->displayed_mpp_buffer = job.buffer_ref;
                    job.buffer_ref = NULL;
                    ctx->perf_total_frames++;
                    ctx->perf_total_end2end_ms += (double)(now_ms() - job.enqueue_ms);
                }
            }
        }

        release_display_job(&job);
    }

    printf("[Display Thread] Exited.\n");
}

// MPP decoder callback: preprocess for inference and enqueue the display frame.
static void on_decoder_frame(void* userdata, int width_stride, int height_stride, int width, int height,
                             int format, int fd, void* data, MppBuffer buffer_ref) {
    FrameProcessContext* ctx = (FrameProcessContext*)userdata;
    if (!ctx || g_should_stop) {
        if (buffer_ref) {
            mpp_buffer_put(buffer_ref);
        }
        return;
    }

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
    if ((ctx->frame_index % 2) == 0) {
        int next_index = reserve_yolo_input_buffer(ctx);
        rknn_tensor_mem* input_mem = (next_index >= 0) ? ctx->yolo_input_mems[next_index] : nullptr;
        if (input_mem != nullptr) {
            image_buffer_t dst_fd_img;
            memset(&dst_fd_img, 0, sizeof(dst_fd_img));
            dst_fd_img.width = ctx->pipeline_ctx.yolo_ctx.model_width;
            dst_fd_img.height = ctx->pipeline_ctx.yolo_ctx.model_height;
            dst_fd_img.width_stride = ctx->pipeline_ctx.yolo_ctx.model_width;
            dst_fd_img.height_stride = ctx->pipeline_ctx.yolo_ctx.model_height;
            dst_fd_img.format = IMAGE_FORMAT_RGB888;
            dst_fd_img.size = dst_fd_img.width * dst_fd_img.height * 3;
            dst_fd_img.fd = input_mem->fd;
            dst_fd_img.virt_addr = (unsigned char*)input_mem->virt_addr;

            unsigned long long convert_begin_ms = now_ms();
            // RGA 直接从 MPP fd 转换到 NPU fd
            int conv_ret = convert_image(&mpp_img, &dst_fd_img, NULL, NULL, 0);
            ctx->perf_total_convert_ms += (double)(now_ms() - convert_begin_ms);

            if (conv_ret == 0) {
                enqueue_infer_job(ctx, ctx->frame_index, next_index);
            } else {
                release_yolo_input_buffer(ctx, next_index);
            }
        } else if (next_index >= 0) {
            release_yolo_input_buffer(ctx, next_index);
        }
    }

    enqueue_display_job(ctx, mpp_img, ctx->frame_index, buffer_ref, frame_begin_ms);
    buffer_ref = NULL;

    ctx->perf_total_input_frames++;
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
        if (n == 0) {
            if (ferror(fp)) {
                fprintf(stderr, "Failed to read video stream: %s\n", input_path);
                free(pkt);
                fclose(fp);
                return -1;
            }
            break;
        }

        PerfTimer decode_timer;
        decode_timer.start();
        int decode_ret = decoder->Decode(pkt, (int)n, 0);
        if (ctx) {
            ctx->perf_total_decode_ms += decode_timer.get_elapsed_ms();
        }
        if (decode_ret != MPP_OK) {
            fprintf(stderr, "MPP Decode failed: %d\n", decode_ret);
            free(pkt);
            fclose(fp);
            return -1;
        }
    }

    if (!g_should_stop) {
        PerfTimer eos_timer;
        eos_timer.start();
        int eos_ret = decoder->Decode(NULL, 0, 1);
        if (ctx) {
            ctx->perf_total_decode_ms += eos_timer.get_elapsed_ms();
        }
        if (eos_ret != MPP_OK) {
            fprintf(stderr, "MPP EOS drain failed: %d\n", eos_ret);
            free(pkt);
            fclose(fp);
            return -1;
        }
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

    g_should_stop = 0;
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    const char* yolov8_path = argv[1];
    const char* lprnet7_path = argv[2];
    const char* lprnet8_path = argv[3];
    const char* input_path = argv[4];
    int save_interval = (argc == 6) ? atoi(argv[5]) : 0;
    if (save_interval > 0) {
        printf("Warning: frame saving is disabled while the dual-plane display path is active.\n");
    }

    FrameProcessContext frame_ctx{};
    frame_ctx.frame_index = 0;
    frame_ctx.perf_total_frames = 0;
    frame_ctx.perf_total_input_frames = 0;
    frame_ctx.perf_total_npu_jobs = 0;
    frame_ctx.perf_display_dropped_frames = 0;
    frame_ctx.perf_overlay_failures = 0;
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
    frame_ctx.displayed_mpp_buffer = nullptr;

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
    ret = decoder.Init(video_type, frame_ctx.target_fps, &frame_ctx);
    if (ret != 1) {
        fprintf(stderr, "MPP decoder initialization failed: %d\n", ret);
        if (frame_ctx.yolo_input_mems[1] != NULL) {
            rknn_destroy_mem(frame_ctx.pipeline_ctx.yolo_ctx.rknn_ctx, frame_ctx.yolo_input_mems[1]);
            frame_ctx.yolo_input_mems[1] = NULL;
        }
        release_pipeline(&frame_ctx.pipeline_ctx);
        return -1;
    }
    decoder.SetCallback(on_decoder_frame);

    // 3. 启动异步推理管线
    {
        std::lock_guard<std::mutex> lock(g_infer.mtx);
        g_infer.jobs.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_display.mtx);
        g_display.jobs.clear();
    }
    std::thread display_thread(display_thread_func, &frame_ctx);
    std::thread infer_thread(inference_thread_func, &frame_ctx);

    // 4. 阻塞式执行码流解析
    printf("\n========== Starting Video Decoding ==========\n");
    int decode_result = decode_raw_h26x_file(input_path, &decoder, &frame_ctx);
    if (decode_result != 0) {
        fprintf(stderr, "Video decode stopped because of an MPP error.\n");
    }

    // 5. 终止逻辑与资源回收
    g_should_stop = 1;
    g_infer.cv.notify_all();
    g_display.cv.notify_all();
    if (display_thread.joinable()) {
        display_thread.join();
    }
    if (infer_thread.joinable()) {
        infer_thread.join();
    }

    if (frame_ctx.drm_initialized) {
        drm_display_deinit(&frame_ctx.drm_display);
        frame_ctx.drm_initialized = false;
    }

    if (frame_ctx.displayed_mpp_buffer) {
        mpp_buffer_put(frame_ctx.displayed_mpp_buffer);
        frame_ctx.displayed_mpp_buffer = nullptr;
    }

    if (frame_ctx.yolo_input_mems[1]) {
        if (frame_ctx.yolo_input_mems[0] && frame_ctx.pipeline_ctx.yolo_ctx.input_native_attrs) {
            rknn_set_io_mem(frame_ctx.pipeline_ctx.yolo_ctx.rknn_ctx,
                            frame_ctx.yolo_input_mems[0],
                            &frame_ctx.pipeline_ctx.yolo_ctx.input_native_attrs[0]);
        }
        rknn_destroy_mem(frame_ctx.pipeline_ctx.yolo_ctx.rknn_ctx, frame_ctx.yolo_input_mems[1]);
        frame_ctx.yolo_input_mems[1] = nullptr;
    }

    release_pipeline(&frame_ctx.pipeline_ctx);

    // 6. 输出性能分析报告
    MppDecoderStats decoder_stats = decoder.GetStats();
    if (frame_ctx.perf_total_frames > 0) {
        double run_sec = (double)(now_ms() - frame_ctx.perf_start_ms) / 1000.0;
        printf("\n========== Performance Summary ==========\n");
        printf("MPP Output Frames:   %llu\n", frame_ctx.perf_total_input_frames);
        printf("Displayed Frames:    %llu\n", frame_ctx.perf_total_frames);
        printf("Display Drops:       %llu\n", frame_ctx.perf_display_dropped_frames);
        printf("Overlay Failures:    %llu\n", frame_ctx.perf_overlay_failures);
        printf("Display Throughput:  %.2f FPS\n", (double)frame_ctx.perf_total_frames / run_sec);
        printf("NPU Inference FPS:   %.2f FPS\n", (double)frame_ctx.perf_total_npu_jobs / run_sec);
        printf("Total Plates Found:  %llu\n", frame_ctx.perf_total_lpr_count);
        printf("-----------------------------------------\n");
        printf("Avg E2E Latency:     %.2f ms/frame\n", frame_ctx.perf_total_end2end_ms / frame_ctx.perf_total_frames);
        printf("Avg Decode Call:     %.2f ms/frame\n", frame_ctx.perf_total_input_frames > 0 ? frame_ctx.perf_total_decode_ms / frame_ctx.perf_total_input_frames : 0.0);
        printf("Avg MPP Callback:    %.2f ms/frame\n", decoder_stats.callback_count > 0 ? (double)decoder_stats.callback_time_ms / decoder_stats.callback_count : 0.0);
        printf("Avg MPP Put Packet:  %.2f ms/frame\n", frame_ctx.perf_total_input_frames > 0 ? (double)decoder_stats.put_packet_time_ms / frame_ctx.perf_total_input_frames : 0.0);
        printf("Avg MPP Get Frame:   %.2f ms/frame\n", frame_ctx.perf_total_input_frames > 0 ? (double)decoder_stats.get_frame_time_ms / frame_ctx.perf_total_input_frames : 0.0);
        printf("Avg MPP Timeout Wait:%.2f ms/frame\n", frame_ctx.perf_total_input_frames > 0 ? (double)decoder_stats.timeout_sleep_ms / frame_ctx.perf_total_input_frames : 0.0);
        printf("MPP Input Retries:   %llu\n", decoder_stats.input_retry_count);
        printf("MPP Buffer Full:     %llu\n", decoder_stats.input_buffer_full_count);
        printf("MPP Input Timeouts:  %llu\n", decoder_stats.input_timeout_count);
        printf("Avg MPP Retry Wait:  %.2f ms/frame\n", frame_ctx.perf_total_input_frames > 0 ? (double)decoder_stats.input_retry_sleep_ms / frame_ctx.perf_total_input_frames : 0.0);
        printf("MPP Retry Peak:      %llu\n", decoder_stats.max_input_retry_streak);
        printf("MPP Input Stalls:    %llu\n", decoder_stats.input_stall_count);
        printf("MPP Input Aborts:    %llu\n", decoder_stats.input_abort_count);
        printf("MPP Input Errors:    %llu\n", decoder_stats.input_fatal_error_count);
        printf("MPP Output Errors:   %llu\n", decoder_stats.output_error_count);
        printf("Avg MPP Pace Sleep:  %.2f ms/frame\n", frame_ctx.perf_total_input_frames > 0 ? (double)decoder_stats.pacing_sleep_ms / frame_ctx.perf_total_input_frames : 0.0);
        printf("MPP Buffer Peak:     %zu KiB\n", decoder_stats.max_buffer_group_usage / 1024);
        printf("Avg RGA Convert:     %.2f ms/frame\n", frame_ctx.perf_total_input_frames > 0 ? frame_ctx.perf_total_convert_ms / frame_ctx.perf_total_input_frames : 0.0);
        printf("Avg NPU Inference:   %.2f ms/frame\n", frame_ctx.perf_total_npu_jobs > 0 ? frame_ctx.perf_total_npu_infer_ms / frame_ctx.perf_total_npu_jobs : 0.0);
        printf("Avg UI Drawing:      %.2f ms/frame\n", frame_ctx.perf_total_ui_drawing_ms / frame_ctx.perf_total_frames);
        printf("Avg DRM Present:     %.2f ms/frame\n", frame_ctx.perf_total_present_ms / frame_ctx.perf_total_frames);
        printf("=========================================\n");
    }

    return decode_result == 0 ? 0 : -1;
}
