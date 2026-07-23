#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <errno.h>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "drm_display.h"
#include "image_utils.h"
#include "pcie_demo_bridge.h"
#include "pcie_frame_source.h"
#include "rga_overlay_renderer.h"
#include "simple_tracker.h"
#include "yolo_ppocr_pipeline.h"

namespace {

const size_t kFramePoolCapacity = 6;
const size_t kDisplayQueueCapacity = 2;
const size_t kInferenceQueueCapacity = 1;
const int kInferenceInterval = 2;
const int kMaxConsecutiveDisplayFailures = 3;

volatile sig_atomic_t g_should_stop = 0;

uint64_t NowMilliseconds() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

double ElapsedMilliseconds(const std::chrono::steady_clock::time_point& begin) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
}

void HandleSignal(int) {
    const int saved_errno = errno;
    g_should_stop = 1;
    static const char message[] =
        "\nPCIe: stop requested; stopping capture and worker threads\n";
    (void)write(STDERR_FILENO, message, sizeof(message) - 1);
    errno = saved_errno;
}

int InstallSignalHandlers() {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = HandleSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    if (sigaction(SIGINT, &action, nullptr) != 0 || sigaction(SIGTERM, &action, nullptr) != 0) {
        return -1;
    }
    return 0;
}

int SetTerminationSignalMask(int how) {
    sigset_t termination_signals;
    sigemptyset(&termination_signals);
    sigaddset(&termination_signals, SIGINT);
    sigaddset(&termination_signals, SIGTERM);
    return pthread_sigmask(how, &termination_signals, nullptr);
}

struct PcieFrame {
    std::vector<unsigned char> pixels;
    int frame_id;
    uint64_t enqueue_ms;

    PcieFrame()
        : pixels(PcieFrameSource::kFrameBytes), frame_id(-1), enqueue_ms(0) {}
};

class FramePool {
public:
    explicit FramePool(size_t capacity) {
        frames_.reserve(capacity);
        for (size_t i = 0; i < capacity; ++i) {
            frames_.emplace_back(new PcieFrame());
            free_frames_.push_back(frames_.back().get());
        }
    }

    std::shared_ptr<PcieFrame> Acquire() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (free_frames_.empty()) {
            return std::shared_ptr<PcieFrame>();
        }
        PcieFrame* frame = free_frames_.front();
        free_frames_.pop_front();
        return std::shared_ptr<PcieFrame>(frame, [this](PcieFrame* released) {
            std::lock_guard<std::mutex> release_lock(mutex_);
            free_frames_.push_back(released);
        });
    }

private:
    std::vector<std::unique_ptr<PcieFrame> > frames_;
    std::deque<PcieFrame*> free_frames_;
    std::mutex mutex_;
};

class LatestFrameQueue {
public:
    explicit LatestFrameQueue(size_t capacity) : capacity_(capacity), dropped_(0) {}

    void Push(const std::shared_ptr<PcieFrame>& frame) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (frames_.size() >= capacity_) {
                frames_.pop_front();
                ++dropped_;
            }
            frames_.push_back(frame);
        }
        condition_.notify_one();
    }

    bool WaitAndPop(std::shared_ptr<PcieFrame>* frame) {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this] { return g_should_stop || !frames_.empty(); });
        if (frames_.empty()) {
            return false;
        }
        *frame = frames_.front();
        frames_.pop_front();
        return true;
    }

    void NotifyStop() {
        condition_.notify_all();
    }

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        frames_.clear();
    }

    uint64_t Dropped() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return dropped_;
    }

private:
    const size_t capacity_;
    std::deque<std::shared_ptr<PcieFrame> > frames_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    uint64_t dropped_;
};

struct LatestPipelineResult {
    std::vector<PipelineResult> results;
    int frame_id;
    std::mutex mutex;

    LatestPipelineResult() : frame_id(-1) {}
};

struct PciePerformance {
    std::atomic<uint64_t> captured_frames;
    std::atomic<uint64_t> frame_pool_drops;
    std::atomic<uint64_t> inference_jobs;
    std::atomic<uint64_t> inference_failures;
    std::atomic<uint64_t> display_pipeline_frames;
    std::atomic<uint64_t> displayed_frames;
    std::atomic<uint64_t> display_failures;
    std::atomic<uint64_t> overlay_failures;
    std::atomic<uint64_t> plate_results;
    double inference_ms;
    double display_convert_ms;
    double overlay_ms;
    double present_ms;
    double end_to_end_ms;

    PciePerformance()
        : captured_frames(0),
          frame_pool_drops(0),
          inference_jobs(0),
          inference_failures(0),
          display_pipeline_frames(0),
          displayed_frames(0),
          display_failures(0),
          overlay_failures(0),
          plate_results(0),
          inference_ms(0.0),
          display_convert_ms(0.0),
          overlay_ms(0.0),
          present_ms(0.0),
          end_to_end_ms(0.0) {}
};

struct PcieAppContext {
    YOLOPPOCRPipelineContext pipeline;
    DrmDisplay drm_display;
    bool drm_initialized;
    RgaOverlayRenderer overlay_renderer;
    SimplePlateTracker tracker;
    int last_result_frame_id;
    LatestFrameQueue display_queue;
    LatestFrameQueue inference_queue;
    LatestPipelineResult latest_result;
    PciePerformance performance;

    PcieAppContext()
        : drm_initialized(false),
          last_result_frame_id(-1),
          display_queue(kDisplayQueueCapacity),
          inference_queue(kInferenceQueueCapacity) {}
};

image_buffer_t MakeBgr565Image(PcieFrame* frame) {
    image_buffer_t image;
    memset(&image, 0, sizeof(image));
    image.width = PcieFrameSource::kFrameWidth;
    image.height = PcieFrameSource::kFrameHeight;
    image.width_stride = PcieFrameSource::kFrameWidth;
    image.height_stride = PcieFrameSource::kFrameHeight;
    image.format = IMAGE_FORMAT_BGR565;
    image.virt_addr = frame->pixels.data();
    image.size = (int)frame->pixels.size();
    image.fd = -1;
    return image;
}

std::vector<PipelineResult> BuildDisplayResults(PcieAppContext* context,
                                                int display_frame_id,
                                                int display_width,
                                                int display_height) {
    std::vector<PipelineResult> current_results;
    int result_frame_id = -1;
    {
        std::lock_guard<std::mutex> lock(context->latest_result.mutex);
        current_results = context->latest_result.results;
        result_frame_id = context->latest_result.frame_id;
    }

    if (result_frame_id >= 0 && result_frame_id != context->last_result_frame_id) {
        context->tracker.update(current_results, result_frame_id);
        context->last_result_frame_id = result_frame_id;
        context->performance.plate_results.fetch_add(current_results.size());
    }

    std::vector<PipelineResult> tracked_results;
    context->tracker.predict(display_frame_id, tracked_results);

    const float scale_x = (float)display_width / PcieFrameSource::kFrameWidth;
    const float scale_y = (float)display_height / PcieFrameSource::kFrameHeight;
    std::vector<PipelineResult> display_results;
    display_results.reserve(tracked_results.size());
    for (const PipelineResult& tracked : tracked_results) {
        PipelineResult display_result = tracked;
        display_result.left = std::max(0, std::min((int)(tracked.left * scale_x), display_width - 1));
        display_result.top = std::max(0, std::min((int)(tracked.top * scale_y), display_height - 1));
        display_result.right = std::max(0, std::min((int)(tracked.right * scale_x), display_width - 1));
        display_result.bottom = std::max(0, std::min((int)(tracked.bottom * scale_y), display_height - 1));
        if (display_result.right > display_result.left && display_result.bottom > display_result.top) {
            display_results.push_back(display_result);
        }
    }
    return display_results;
}

PcieUiStatus BuildUiStatus(const PcieAppContext& context,
                           const PcieFrameSource& source,
                           uint64_t start_ms,
                           bool worker_alive,
                           bool capturing,
                           bool pcie_open,
                           const std::string& message,
                           const std::string& plate_text,
                           const std::string& plate_type = "",
                           float plate_confidence = 0.0f) {
    PcieUiStatus status;
    status.worker_alive = worker_alive;
    status.capturing = capturing;
    status.pcie_open = pcie_open;
    status.start_ms = start_ms;
    status.elapsed_ms = NowMilliseconds() - start_ms;
    status.captured_frames = context.performance.captured_frames.load();
    status.display_pipeline_frames = context.performance.display_pipeline_frames.load();
    status.displayed_frames = context.performance.displayed_frames.load();
    status.inference_jobs = context.performance.inference_jobs.load();
    status.inference_failures = context.performance.inference_failures.load();
    status.frame_pool_drops = context.performance.frame_pool_drops.load();
    status.display_queue_drops = context.display_queue.Dropped();
    status.inference_queue_drops = context.inference_queue.Dropped();
    const uint64_t displayed_frames = context.performance.displayed_frames.load();
    if (displayed_frames > 0) {
        status.avg_end_to_end_ms =
            context.performance.end_to_end_ms / displayed_frames;
    }
    status.vendor_id = source.DeviceInfo().vendor_id;
    status.device_id = source.DeviceInfo().device_id;
    status.link_speed = source.DeviceInfo().link_speed;
    status.link_width = source.DeviceInfo().link_width;
    status.max_payload_size = source.DeviceInfo().max_payload_size;
    status.message = message;
    status.plate_text = plate_text;
    status.plate_type = plate_type;
    status.plate_confidence = plate_confidence;
    return status;
}

void InferenceThread(PcieAppContext* context) {
    printf("[PCIe Inference] thread started\n");
    std::shared_ptr<PcieFrame> frame;
    while (context->inference_queue.WaitAndPop(&frame)) {
        image_buffer_t image = MakeBgr565Image(frame.get());
        std::vector<PipelineResult> results;
        const std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
        const int ret =
            process_ppocr_pipeline(&context->pipeline, &image, results);
        context->performance.inference_ms += ElapsedMilliseconds(begin);
        if (ret == 0) {
            std::lock_guard<std::mutex> lock(context->latest_result.mutex);
            context->latest_result.results.swap(results);
            context->latest_result.frame_id = frame->frame_id;
            ++context->performance.inference_jobs;
        } else {
            ++context->performance.inference_failures;
        }
        frame.reset();
    }
    printf("[PCIe Inference] thread exited\n");
}

void DisplayThread(PcieAppContext* context, const PcieUiCallbacks* callbacks,
                   const PcieFrameSource& source, uint64_t start_ms) {
    printf("[PCIe Display] thread started\n");
    const bool qt_mode = (callbacks != nullptr);
    if (!context->drm_initialized && !qt_mode) {
        return;
    }

    std::shared_ptr<PcieFrame> frame;
    int consecutive_present_failures = 0;
    while (context->display_queue.WaitAndPop(&frame)) {
        image_buffer_t raw_image = MakeBgr565Image(frame.get());
        if (qt_mode) {
            // Qt paints on the main thread. If it is already behind, drop here
            // before the BGR565->RGBA conversion and overlay work spend CPU.
            if (callbacks != nullptr && callbacks->can_accept_frame &&
                !callbacks->can_accept_frame()) {
                frame.reset();
                continue;
            }

            std::vector<unsigned char> rgba_pixels(PcieFrameSource::kFrameWidth *
                                                   PcieFrameSource::kFrameHeight * 4U);
            image_buffer_t ui_buffer;
            memset(&ui_buffer, 0, sizeof(ui_buffer));
            ui_buffer.width = PcieFrameSource::kFrameWidth;
            ui_buffer.height = PcieFrameSource::kFrameHeight;
            ui_buffer.width_stride = PcieFrameSource::kFrameWidth;
            ui_buffer.height_stride = PcieFrameSource::kFrameHeight;
            ui_buffer.format = IMAGE_FORMAT_RGBA8888;
            ui_buffer.virt_addr = rgba_pixels.data();
            ui_buffer.size = static_cast<int>(rgba_pixels.size());
            ui_buffer.fd = -1;

            std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
            if (convert_image(&raw_image, &ui_buffer, nullptr, nullptr, 0) != 0) {
                context->performance.display_convert_ms += ElapsedMilliseconds(begin);
                ++context->performance.display_failures;
                frame.reset();
                continue;
            }
            context->performance.display_convert_ms += ElapsedMilliseconds(begin);

            begin = std::chrono::steady_clock::now();
            const std::vector<PipelineResult> display_results =
                BuildDisplayResults(context, frame->frame_id, ui_buffer.width, ui_buffer.height);
            if (context->overlay_renderer.Render(&ui_buffer, display_results, false) != 0) {
                ++context->performance.overlay_failures;
            }
            context->performance.overlay_ms += ElapsedMilliseconds(begin);
            ++context->performance.display_pipeline_frames;
            context->performance.end_to_end_ms += (double)(NowMilliseconds() - frame->enqueue_ms);
            ++context->performance.displayed_frames;

            std::string plate_text;
            std::string plate_type;
            float plate_confidence = 0.0f;
            for (const PipelineResult& result : display_results) {
                if (result.has_valid_plate_text && !result.plate_name.empty()) {
                    plate_text = result.plate_name;
                    plate_type = result.plate_type;
                    plate_confidence = result.text_confidence > 0.0f
                                           ? result.text_confidence
                                           : result.confidence;
                    break;
                }
            }

            PcieUiStatus status = BuildUiStatus(*context, source, start_ms, true, true, true,
                                                "", plate_text, plate_type, plate_confidence);
            if (callbacks != nullptr && callbacks->on_status) {
                callbacks->on_status(status);
            }
            if (callbacks != nullptr && callbacks->on_frame) {
                PcieUiFrame ui_frame;
                ui_frame.pixels = std::make_shared<std::vector<unsigned char> >(std::move(rgba_pixels));
                ui_frame.width = ui_buffer.width;
                ui_frame.height = ui_buffer.height;
                ui_frame.stride = ui_buffer.width_stride > 0 ? ui_buffer.width_stride : ui_buffer.width;
                ui_frame.frame_id = frame->frame_id;
                callbacks->on_frame(ui_frame, status);
            }
            frame.reset();
            continue;
        }

        image_buffer_t ui_buffer;
        if (drm_display_get_ui_buffer(&context->drm_display, &ui_buffer) != 0) {
            ++context->performance.display_failures;
            frame.reset();
            continue;
        }

        std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
        if (convert_image(&raw_image, &ui_buffer, nullptr, nullptr, 0) != 0) {
            context->performance.display_convert_ms += ElapsedMilliseconds(begin);
            ++context->performance.display_failures;
            frame.reset();
            continue;
        }
        context->performance.display_convert_ms += ElapsedMilliseconds(begin);

        begin = std::chrono::steady_clock::now();
        const std::vector<PipelineResult> display_results =
            BuildDisplayResults(context, frame->frame_id, ui_buffer.width, ui_buffer.height);
        if (context->overlay_renderer.Render(&ui_buffer, display_results, false) != 0) {
            ++context->performance.overlay_failures;
        }
        context->performance.overlay_ms += ElapsedMilliseconds(begin);
        ++context->performance.display_pipeline_frames;

        begin = std::chrono::steady_clock::now();
        if (drm_display_commit_ui(&context->drm_display) == 0) {
            context->performance.present_ms += ElapsedMilliseconds(begin);
            context->performance.end_to_end_ms += (double)(NowMilliseconds() - frame->enqueue_ms);
            ++context->performance.displayed_frames;
            consecutive_present_failures = 0;
        } else {
            context->performance.present_ms += ElapsedMilliseconds(begin);
            ++context->performance.display_failures;
            ++consecutive_present_failures;
            if (consecutive_present_failures >= kMaxConsecutiveDisplayFailures) {
                fprintf(stderr,
                        "DRM: %d consecutive atomic commits failed; stopping instead of running without display ownership\n",
                        consecutive_present_failures);
                kill(getpid(), SIGTERM);
                frame.reset();
                break;
            }
        }
        frame.reset();
    }
    printf("[PCIe Display] thread exited\n");
}

void PrintPerformance(const PcieAppContext& context, const PcieFrameSource& source,
                      uint64_t start_ms) {
    const double elapsed_seconds = std::max(0.001, (NowMilliseconds() - start_ms) / 1000.0);
    const PcieReadStatistics& driver = source.Statistics();
    const uint64_t captured_frames = context.performance.captured_frames.load();
    const uint64_t display_pipeline_frames = context.performance.display_pipeline_frames.load();
    const uint64_t displayed_frames = context.performance.displayed_frames.load();
    const uint64_t inference_jobs = context.performance.inference_jobs.load();
    const uint64_t frame_pool_drops = context.performance.frame_pool_drops.load();
    const uint64_t display_failures = context.performance.display_failures.load();
    const uint64_t overlay_failures = context.performance.overlay_failures.load();
    const uint64_t inference_failures = context.performance.inference_failures.load();
    const uint64_t plate_results = context.performance.plate_results.load();
    printf("\n========== PCIe Pipeline Statistics ==========\n");
    printf("Captured: %llu (%.2f fps), pool drops: %llu\n",
           (unsigned long long)captured_frames,
           captured_frames / elapsed_seconds,
           (unsigned long long)frame_pool_drops);
    printf("Display pipeline: %llu (%.2f fps)\n",
           (unsigned long long)display_pipeline_frames,
           display_pipeline_frames / elapsed_seconds);
    printf("Display presented/handoff: %llu (%.2f fps), queue drops: %llu, display failures: %llu, overlay failures: %llu\n",
           (unsigned long long)displayed_frames,
           displayed_frames / elapsed_seconds,
           (unsigned long long)context.display_queue.Dropped(),
           (unsigned long long)display_failures,
           (unsigned long long)overlay_failures);
    printf("Inference: %llu (%.2f fps), queue drops: %llu, failures: %llu, plate results: %llu\n",
           (unsigned long long)inference_jobs,
           inference_jobs / elapsed_seconds,
           (unsigned long long)context.inference_queue.Dropped(),
           (unsigned long long)inference_failures,
           (unsigned long long)plate_results);
    if (inference_jobs > 0) {
        printf("Average inference pipeline: %.2f ms\n",
               context.performance.inference_ms / inference_jobs);
    }
    printf("PP-OCR primary/retry/retry accepted: %llu / %llu / %llu",
           (unsigned long long)context.pipeline.ppocr_primary_attempts,
           (unsigned long long)context.pipeline.ppocr_retry_attempts,
           (unsigned long long)context.pipeline.ppocr_retry_successes);
    if (context.pipeline.ppocr_retry_attempts > 0) {
        printf(", average retry: %.2f ms",
               context.pipeline.ppocr_retry_ms /
                   context.pipeline.ppocr_retry_attempts);
    }
    printf("\n");
    if (display_pipeline_frames > 0) {
        printf("Average display convert/overlay: %.2f / %.2f ms\n",
               context.performance.display_convert_ms / display_pipeline_frames,
               context.performance.overlay_ms / display_pipeline_frames);
    }
    if (displayed_frames > 0) {
        const uint64_t present_attempts = std::max<uint64_t>(1, display_pipeline_frames);
        printf("Average display present/end-to-end: %.2f / %.2f ms\n",
               context.performance.present_ms / present_attempts,
               context.performance.end_to_end_ms / displayed_frames);
    }
    printf("Driver retries: zero=%llu EPERM=%llu interrupted/EAGAIN=%llu, fatal errors=%llu\n",
           (unsigned long long)driver.zero_status_retries,
           (unsigned long long)driver.permission_retries,
           (unsigned long long)driver.interrupted_retries,
           (unsigned long long)driver.other_errors);
    printf("==============================================\n");
}

}  // namespace

int RunPpocrPcieDemo(const char* yolov8_model,
                     const char* ppocr_model,
                     const char* dictionary,
                     const PcieUiCallbacks* callbacks) {
    g_should_stop = 0;
    printf("========================================\n");
    printf("    YOLOv8 PP-OCR PCIe BGR565 Demo      \n");
    printf("========================================\n\n");

    if (yolov8_model == nullptr || ppocr_model == nullptr || dictionary == nullptr) {
        fprintf(stderr, "Model path is missing\n");
        return -1;
    }
    const bool use_signal_handlers = (callbacks == nullptr);
    if (use_signal_handlers && InstallSignalHandlers() != 0) {
        fprintf(stderr, "Failed to install signal handlers\n");
        return -1;
    }
    FramePool frame_pool(kFramePoolCapacity);
    PcieAppContext context;
    PcieFrameSource source;

    printf("========== Initializing Pipeline ==========\n");
    int ret = init_ppocr_pipeline(
        yolov8_model, ppocr_model, dictionary, &context.pipeline);
    if (ret != 0) {
        fprintf(stderr, "Pipeline initialization failed: %d\n", ret);
        return ret;
    }

    printf("========== Initializing Display ============\n");
    if (callbacks == nullptr) {
        if (drm_display_init(&context.drm_display, PcieFrameSource::kFrameWidth,
                             PcieFrameSource::kFrameHeight) != 0 ||
            context.overlay_renderer.Init(context.drm_display.ui_width,
                                          context.drm_display.ui_height) != 0) {
            fprintf(stderr, "PCIe display initialization failed\n");
            drm_display_deinit(&context.drm_display);
            release_ppocr_pipeline(&context.pipeline);
            return -1;
        }
        context.drm_initialized = true;
    } else {
        if (context.overlay_renderer.Init(PcieFrameSource::kFrameWidth,
                                          PcieFrameSource::kFrameHeight) != 0) {
            fprintf(stderr, "Qt display overlay initialization failed\n");
            release_ppocr_pipeline(&context.pipeline);
            return -1;
        }
    }

    printf("========== Initializing PCIe ==============\n");
    if (source.Open() != 0) {
        if (context.drm_initialized) {
            drm_display_deinit(&context.drm_display);
            context.drm_initialized = false;
        }
        release_ppocr_pipeline(&context.pipeline);
        return -1;
    }

    if (use_signal_handlers) {
        const int signal_mask_result = SetTerminationSignalMask(SIG_BLOCK);
        if (signal_mask_result != 0) {
            fprintf(stderr, "Failed to block worker termination signals: %s\n",
                    strerror(signal_mask_result));
            source.Close();
            if (context.drm_initialized) {
                drm_display_deinit(&context.drm_display);
                context.drm_initialized = false;
            }
            release_ppocr_pipeline(&context.pipeline);
            return -1;
        }
    }

    const uint64_t start_ms = NowMilliseconds();
    std::thread inference_thread(InferenceThread, &context);
    std::thread display_thread(DisplayThread, &context, callbacks, std::cref(source), start_ms);
    if (use_signal_handlers) {
        const int signal_unmask_result = SetTerminationSignalMask(SIG_UNBLOCK);
        if (signal_unmask_result != 0) {
            fprintf(stderr, "Failed to route termination signals to the capture thread: %s\n",
                    strerror(signal_unmask_result));
            g_should_stop = 1;
        }
    }

    if (callbacks != nullptr && callbacks->on_status) {
        callbacks->on_status(BuildUiStatus(context, source, start_ms, true, true, true,
                                           "等待首帧", ""));
    }

    std::vector<unsigned char> drain_buffer(PcieFrameSource::kFrameBytes);
    int next_frame_id = 0;
    int capture_result = 0;
    uint64_t last_status_ms = start_ms;
    uint64_t last_retry_log_ms = start_ms;

    printf("========== Capturing PCIe Frames ==========\n");
    while (!g_should_stop) {
        if (callbacks != nullptr && callbacks->should_stop && callbacks->should_stop()) {
            g_should_stop = 1;
            break;
        }
        const bool capture_enabled =
            !(callbacks != nullptr && callbacks->capture_enabled && !callbacks->capture_enabled());
        std::shared_ptr<PcieFrame> frame = capture_enabled ? frame_pool.Acquire()
                                                           : std::shared_ptr<PcieFrame>();
        unsigned char* destination = frame ? frame->pixels.data() : drain_buffer.data();
        const PcieFrameReadResult read_result =
            source.ReadFrame(destination, PcieFrameSource::kFrameBytes);
        if (read_result == PCIE_FRAME_RETRY) {
            const uint64_t now_ms = NowMilliseconds();
            if (now_ms - last_retry_log_ms >= 2000U) {
                const PcieReadStatistics& stats = source.Statistics();
                printf("PCIe: waiting for frame, last_status=%lld, ready=%llu, "
                       "retry zero=%llu EPERM=%llu EINTR/EAGAIN=%llu other=%llu\n",
                       source.LastDriverStatus(),
                       (unsigned long long)stats.frames_ready,
                       (unsigned long long)stats.zero_status_retries,
                       (unsigned long long)stats.permission_retries,
                       (unsigned long long)stats.interrupted_retries,
                       (unsigned long long)stats.other_errors);
                last_retry_log_ms = now_ms;
            }
            if (callbacks != nullptr && callbacks->on_status) {
                if (now_ms - last_status_ms >= 1000U) {
                    callbacks->on_status(BuildUiStatus(context, source, start_ms,
                                                       true, capture_enabled, true,
                                                       capture_enabled ? "等待PCIe帧数据"
                                                                       : "暂停中，保持PCIe读取",
                                                       ""));
                    last_status_ms = now_ms;
                }
            }
            frame.reset();
            if (!g_should_stop) {
                usleep(1000);
            }
            continue;
        }
        if (read_result == PCIE_FRAME_FATAL) {
            capture_result = -1;
            break;
        }

        if (!capture_enabled) {
            if (callbacks != nullptr && callbacks->on_status) {
                const uint64_t now_ms = NowMilliseconds();
                if (now_ms - last_status_ms >= 1000U) {
                    callbacks->on_status(BuildUiStatus(context, source, start_ms,
                                                       true, false, true,
                                                       "paused, draining PCIe", ""));
                    last_status_ms = now_ms;
                }
            }
            continue;
        }

        const int frame_id = next_frame_id++;
        ++context.performance.captured_frames;
        if (!frame) {
            ++context.performance.frame_pool_drops;
            continue;
        }

        frame->frame_id = frame_id;
        frame->enqueue_ms = NowMilliseconds();
        context.display_queue.Push(frame);
        if ((frame_id % kInferenceInterval) == 0) {
            context.inference_queue.Push(frame);
        }
        frame.reset();
    }

    g_should_stop = 1;
    source.Close();
    context.display_queue.NotifyStop();
    context.inference_queue.NotifyStop();
    if (display_thread.joinable()) {
        display_thread.join();
    }
    if (inference_thread.joinable()) {
        inference_thread.join();
    }
    context.display_queue.Clear();
    context.inference_queue.Clear();

    if (context.drm_initialized) {
        drm_display_deinit(&context.drm_display);
        context.drm_initialized = false;
    }

    PrintPerformance(context, source, start_ms);
    release_ppocr_pipeline(&context.pipeline);
    return capture_result;
}

#ifndef PCIE_QT_UI_BUILD
int main(int argc, char** argv) {
    if (argc != 4) {
        printf("Usage: %s <yolov8_model> <ppocr_model> <character_dictionary>\n",
               argv[0]);
        return -1;
    }
    return RunPpocrPcieDemo(argv[1], argv[2], argv[3], nullptr);
}
#endif
