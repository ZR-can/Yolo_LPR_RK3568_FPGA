#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <errno.h>
#include <functional>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <signal.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "drm_display.h"
#include "image_utils.h"
#include "pcie_frame_source.h"
#include "traffic_pcie_bridge.h"
#include "traffic_overlay_renderer.h"
#include "traffic_temporal_tracker.h"
#include "traffic_violation.h"
#include "yolov8.h"

namespace {

constexpr size_t kFramePoolCapacity = 6;
constexpr size_t kDisplayQueueCapacity = 2;
constexpr size_t kInferenceQueueCapacity = 1;
constexpr int kDefaultInferenceInterval = 2;
constexpr int kMaxConsecutiveDisplayFailures = 3;

volatile sig_atomic_t g_should_stop = 0;

uint64_t NowMilliseconds() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

double ElapsedMilliseconds(const std::chrono::steady_clock::time_point& begin) {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - begin).count();
}

void HandleSignal(int) {
    const int saved_errno = errno;
    g_should_stop = 1;
    static const char message[] =
        "\nTraffic PCIe: stop requested; stopping capture and worker threads\n";
    const ssize_t written =
        write(STDERR_FILENO, message, sizeof(message) - 1);
    (void)written;
    errno = saved_errno;
}

int InstallSignalHandlers() {
    struct sigaction action;
    std::memset(&action, 0, sizeof(action));
    action.sa_handler = HandleSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    return sigaction(SIGINT, &action, nullptr) == 0 &&
                   sigaction(SIGTERM, &action, nullptr) == 0
               ? 0
               : -1;
}

int SetTerminationSignalMask(int how) {
    sigset_t termination_signals;
    sigemptyset(&termination_signals);
    sigaddset(&termination_signals, SIGINT);
    sigaddset(&termination_signals, SIGTERM);
    return pthread_sigmask(how, &termination_signals, nullptr);
}

std::string ParentPath(const std::string& path) {
    const std::string::size_type position = path.find_last_of("/\\");
    if (position == std::string::npos) {
        return ".";
    }
    return position == 0 ? path.substr(0, 1) : path.substr(0, position);
}

std::string JoinPath(const std::string& left, const std::string& right) {
    if (left.empty() || left == ".") {
        return left.empty() ? right : left + "/" + right;
    }
    return left[left.size() - 1] == '/' ? left + right : left + "/" + right;
}

bool ParsePositiveInt(const char* text, int* value) {
    if (text == nullptr || value == nullptr) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const long parsed = std::strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed < 1 || parsed > 1000000) {
        return false;
    }
    *value = static_cast<int>(parsed);
    return true;
}

void PrintUsage(const char* program) {
    std::printf(
        "Usage: %s <model.rknn> [--interval N] [--roi-config PATH] "
        "[--roi \"x1,y1;...\"] "
        "[--light-roi \"left,top,right,bottom\"]\n"
        "  Default config: <model-directory>/traffic_roi.conf\n"
        "  Explicit --roi and --light-roi values override the config individually.\n"
        "  All ROI coordinates are normalized to [0,1].\n",
        program);
}

struct CommandLineOptions {
    std::string model_path;
    int inference_interval = kDefaultInferenceInterval;
    TrafficRoiConfig roi = default_traffic_roi();
    TrafficLightRoiConfig light_roi = default_traffic_light_roi();
    std::string roi_config_path;
    bool roi_config_loaded = false;
};

bool ParseCommandLine(int argc, char** argv, CommandLineOptions* options) {
    if (options == nullptr || argc < 2) {
        return false;
    }
    if (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0) {
        return false;
    }
    *options = CommandLineOptions();
    options->model_path = argv[1];
    options->roi_config_path =
        JoinPath(ParentPath(options->model_path), "traffic_roi.conf");
    bool roi_config_explicit = false;
    bool roi_seen = false;
    bool light_roi_seen = false;
    std::string roi_text;
    std::string light_roi_text;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--interval") == 0) {
            if (i + 1 >= argc || !ParsePositiveInt(argv[++i], &options->inference_interval)) {
                std::fprintf(stderr, "--interval requires an integer >= 1\n");
                return false;
            }
        } else if (std::strcmp(argv[i], "--roi-config") == 0) {
            if (roi_config_explicit) {
                std::fprintf(stderr, "--roi-config may only be specified once\n");
                return false;
            }
            if (i + 1 >= argc || argv[i + 1][0] == '\0') {
                std::fprintf(stderr, "--roi-config requires a file path\n");
                return false;
            }
            options->roi_config_path = argv[++i];
            roi_config_explicit = true;
        } else if (std::strcmp(argv[i], "--roi") == 0) {
            if (roi_seen) {
                std::fprintf(stderr, "--roi may only be specified once\n");
                return false;
            }
            if (i + 1 >= argc) {
                std::fprintf(stderr, "--roi requires a polygon string\n");
                return false;
            }
            roi_text = argv[++i];
            roi_seen = true;
        } else if (std::strcmp(argv[i], "--light-roi") == 0) {
            if (light_roi_seen) {
                std::fprintf(stderr, "--light-roi may only be specified once\n");
                return false;
            }
            if (i + 1 >= argc) {
                std::fprintf(stderr, "--light-roi requires left,top,right,bottom\n");
                return false;
            }
            light_roi_text = argv[++i];
            light_roi_seen = true;
        } else {
            std::fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return false;
        }
    }

    std::string error;
    if (access(options->roi_config_path.c_str(), R_OK) == 0) {
        if (!load_traffic_roi_config_file(
                options->roi_config_path.c_str(),
                &options->roi,
                &options->light_roi,
                &error)) {
            std::fprintf(stderr, "Invalid ROI config: %s\n", error.c_str());
            return false;
        }
        options->roi_config_loaded = true;
    } else if (roi_config_explicit) {
        std::fprintf(stderr,
                     "Explicit ROI config is not readable: %s\n",
                     options->roi_config_path.c_str());
        return false;
    } else {
        std::fprintf(
            stderr,
            "Warning: default ROI config is not readable; using built-in values: %s\n",
            options->roi_config_path.c_str());
    }

    if (roi_seen &&
        !parse_normalized_traffic_roi(roi_text.c_str(), &options->roi, &error)) {
        std::fprintf(stderr, "Invalid --roi: %s\n", error.c_str());
        return false;
    }
    if (light_roi_seen &&
        !parse_normalized_traffic_light_roi(
            light_roi_text.c_str(), &options->light_roi, &error)) {
        std::fprintf(stderr, "Invalid --light-roi: %s\n", error.c_str());
        return false;
    }
    return true;
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

struct LatestTrafficResult {
    TrafficFrameAnalysis analysis;
    bool valid;
    std::mutex mutex;

    LatestTrafficResult() : valid(false) {}
};

struct PipelinePerformance {
    std::atomic<uint64_t> captured_frames;
    std::atomic<uint64_t> frame_pool_drops;
    std::atomic<uint64_t> inference_jobs;
    std::atomic<uint64_t> inference_failures;
    std::atomic<uint64_t> displayed_frames;
    std::atomic<uint64_t> display_failures;
    std::atomic<uint64_t> overlay_failures;
    std::atomic<uint64_t> person_detections;
    std::atomic<uint64_t> violation_events;
    double inference_ms;
    double color_rule_ms;
    double display_convert_ms;
    double overlay_ms;
    double present_ms;
    double end_to_end_ms;

    PipelinePerformance()
        : captured_frames(0),
          frame_pool_drops(0),
          inference_jobs(0),
          inference_failures(0),
          displayed_frames(0),
          display_failures(0),
          overlay_failures(0),
          person_detections(0),
          violation_events(0),
          inference_ms(0.0),
          color_rule_ms(0.0),
          display_convert_ms(0.0),
          overlay_ms(0.0),
          present_ms(0.0),
          end_to_end_ms(0.0) {}
};

struct TrafficAppContext {
    traffic_rknn_app_context_t yolo;
    DrmDisplay drm;
    bool drm_initialized;
    TrafficRoiConfig roi;
    int inference_interval;
    TrafficOverlayRenderer overlay_renderer;
    TrafficTemporalTracker temporal_tracker;
    image_rect_t light_box = {-1, -1, -1, -1};
    LatestFrameQueue display_queue;
    LatestFrameQueue inference_queue;
    LatestTrafficResult latest_result;
    PipelinePerformance performance;

    TrafficAppContext()
        : drm_initialized(false),
          inference_interval(kDefaultInferenceInterval),
          display_queue(kDisplayQueueCapacity),
          inference_queue(kInferenceQueueCapacity) {
        std::memset(&yolo, 0, sizeof(yolo));
    }
};

image_buffer_t MakeBgr565Image(PcieFrame* frame) {
    image_buffer_t image;
    std::memset(&image, 0, sizeof(image));
    image.width = PcieFrameSource::kFrameWidth;
    image.height = PcieFrameSource::kFrameHeight;
    image.width_stride = PcieFrameSource::kFrameWidth;
    image.height_stride = PcieFrameSource::kFrameHeight;
    image.format = IMAGE_FORMAT_BGR565;
    image.virt_addr = frame->pixels.data();
    image.size = static_cast<int>(frame->pixels.size());
    image.fd = -1;
    return image;
}

void InferenceThread(TrafficAppContext* context) {
    std::printf("[Traffic Inference] thread started\n");
    std::shared_ptr<PcieFrame> frame;
    while (context->inference_queue.WaitAndPop(&frame)) {
        image_buffer_t image = MakeBgr565Image(frame.get());
        object_detect_result_list detections;
        std::memset(&detections, 0, sizeof(detections));

        const std::chrono::steady_clock::time_point inference_begin =
            std::chrono::steady_clock::now();
        const int inference_result =
            inference_traffic_yolov8_model(&context->yolo, &image, &detections);
        const double inference_ms = ElapsedMilliseconds(inference_begin);
        context->performance.inference_ms += inference_ms;

        if (inference_result == 0) {
            TrafficFrameAnalysis instant_analysis;
            const std::chrono::steady_clock::time_point rule_begin =
                std::chrono::steady_clock::now();
            const int rule_result = analyze_traffic_frame(
                &image,
                &detections,
                context->roi,
                context->light_box,
                frame->frame_id,
                &instant_analysis);
            instant_analysis.inference_ms = inference_ms;
            TrafficFrameAnalysis analysis;
            const int tracking_result = rule_result == 0
                                            ? context->temporal_tracker.Update(
                                                  instant_analysis, &analysis)
                                            : -1;
            context->performance.color_rule_ms += ElapsedMilliseconds(rule_begin);
            if (rule_result == 0 && tracking_result == 0) {
                {
                    std::lock_guard<std::mutex> lock(context->latest_result.mutex);
                    context->latest_result.analysis = analysis;
                    context->latest_result.valid = true;
                }
                ++context->performance.inference_jobs;
                context->performance.person_detections.fetch_add(analysis.person_count);
                context->performance.violation_events.fetch_add(
                    analysis.violation_event_count);
                std::printf(
                    "[Traffic] frame=%d light=%s(raw=%s) red_score=%d green_score=%d color_active=%d tracked=%d roi=%d active=%d new_events=%d total_events=%d\n",
                    analysis.frame_id,
                    traffic_light_state_name(analysis.light.state),
                    traffic_light_state_name(analysis.light.instant_state),
                    analysis.light.red_evidence,
                    analysis.light.green_evidence,
                    analysis.light.active_count,
                    analysis.person_count,
                    analysis.persons_in_crosswalk,
                    analysis.violation_count,
                    analysis.violation_event_count,
                    analysis.violation_event_total);
            } else {
                ++context->performance.inference_failures;
            }
        } else {
            ++context->performance.inference_failures;
        }
        frame.reset();
    }
    std::printf("[Traffic Inference] thread exited\n");
}

TrafficFrameAnalysis GetLatestAnalysis(TrafficAppContext* context) {
    std::lock_guard<std::mutex> lock(context->latest_result.mutex);
    if (context->latest_result.valid) {
        return context->latest_result.analysis;
    }
    TrafficFrameAnalysis waiting;
    waiting.source_width = PcieFrameSource::kFrameWidth;
    waiting.source_height = PcieFrameSource::kFrameHeight;
    return waiting;
}

PcieUiStatus BuildUiStatus(const TrafficAppContext& context,
                           const PcieFrameSource& source,
                           uint64_t start_ms,
                           bool worker_alive,
                           bool capturing,
                           bool pcie_open,
                           const std::string& message,
                           const TrafficFrameAnalysis& analysis) {
    PcieUiStatus status;
    status.worker_alive = worker_alive;
    status.capturing = capturing;
    status.pcie_open = pcie_open;
    status.start_ms = start_ms;
    status.elapsed_ms = NowMilliseconds() - start_ms;
    status.captured_frames = context.performance.captured_frames.load();
    status.display_pipeline_frames = context.performance.displayed_frames.load();
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
    status.traffic_light_state = traffic_light_state_name(analysis.light.state);
    status.traffic_person_count = analysis.person_count;
    status.traffic_person_id_total = analysis.person_id_total;
    status.traffic_persons_in_crosswalk = analysis.persons_in_crosswalk;
    status.traffic_violation_count = analysis.violation_count;
    status.traffic_violation_event_total = analysis.violation_event_total;
    status.message = message;
    return status;
}

void DisplayThread(TrafficAppContext* context,
                   const PcieUiCallbacks* callbacks,
                   const PcieFrameSource& source,
                   uint64_t start_ms) {
    std::printf("[Traffic Display] thread started\n");
    const bool qt_mode = callbacks != nullptr;
    if (!context->drm_initialized && !qt_mode) {
        return;
    }
    std::shared_ptr<PcieFrame> frame;
    int consecutive_present_failures = 0;
    while (context->display_queue.WaitAndPop(&frame)) {
        if (qt_mode && callbacks->can_accept_frame &&
            !callbacks->can_accept_frame()) {
            frame.reset();
            continue;
        }

        image_buffer_t raw_image = MakeBgr565Image(frame.get());
        image_buffer_t ui_buffer;
        std::memset(&ui_buffer, 0, sizeof(ui_buffer));
        std::shared_ptr<std::vector<unsigned char> > rgba_pixels;
        if (qt_mode) {
            rgba_pixels = std::make_shared<std::vector<unsigned char> >(
                PcieFrameSource::kFrameWidth * PcieFrameSource::kFrameHeight * 4U);
            ui_buffer.width = PcieFrameSource::kFrameWidth;
            ui_buffer.height = PcieFrameSource::kFrameHeight;
            ui_buffer.width_stride = PcieFrameSource::kFrameWidth;
            ui_buffer.height_stride = PcieFrameSource::kFrameHeight;
            ui_buffer.format = IMAGE_FORMAT_RGBA8888;
            ui_buffer.virt_addr = rgba_pixels->data();
            ui_buffer.size = static_cast<int>(rgba_pixels->size());
            ui_buffer.fd = -1;
        } else if (drm_display_get_ui_buffer(&context->drm, &ui_buffer) != 0) {
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
        const TrafficFrameAnalysis analysis = GetLatestAnalysis(context);
        if (context->overlay_renderer.Render(&ui_buffer, analysis) != 0) {
            ++context->performance.overlay_failures;
        }
        context->performance.overlay_ms += ElapsedMilliseconds(begin);

        if (qt_mode) {
            context->performance.end_to_end_ms +=
                static_cast<double>(NowMilliseconds() - frame->enqueue_ms);
            ++context->performance.displayed_frames;
            const PcieUiStatus status =
                BuildUiStatus(*context, source, start_ms, true, true, true, "", analysis);
            if (callbacks->on_status) {
                callbacks->on_status(status);
            }
            if (callbacks->on_frame) {
                PcieUiFrame ui_frame;
                ui_frame.pixels = rgba_pixels;
                ui_frame.width = ui_buffer.width;
                ui_frame.height = ui_buffer.height;
                ui_frame.stride = ui_buffer.width_stride > 0
                                      ? ui_buffer.width_stride
                                      : ui_buffer.width;
                ui_frame.frame_id = frame->frame_id;
                callbacks->on_frame(ui_frame, status);
            }
            frame.reset();
            continue;
        }

        begin = std::chrono::steady_clock::now();
        if (drm_display_commit_ui(&context->drm) == 0) {
            context->performance.present_ms += ElapsedMilliseconds(begin);
            context->performance.end_to_end_ms +=
                static_cast<double>(NowMilliseconds() - frame->enqueue_ms);
            ++context->performance.displayed_frames;
            consecutive_present_failures = 0;
        } else {
            context->performance.present_ms += ElapsedMilliseconds(begin);
            ++context->performance.display_failures;
            ++consecutive_present_failures;
            if (consecutive_present_failures >= kMaxConsecutiveDisplayFailures) {
                std::fprintf(stderr,
                             "DRM: %d consecutive commits failed; stopping traffic demo\n",
                             consecutive_present_failures);
                kill(getpid(), SIGTERM);
                frame.reset();
                break;
            }
        }
        frame.reset();
    }
    std::printf("[Traffic Display] thread exited\n");
}

void PrintRoi(const TrafficRoiConfig& roi) {
    std::printf("Crosswalk ROI (normalized): ");
    for (size_t i = 0; i < roi.points.size(); ++i) {
        std::printf("%s%.6f,%.6f", i == 0 ? "" : ";",
                    roi.points[i].x, roi.points[i].y);
    }
    std::printf("\n");
}

void PrintLightRoi(const TrafficLightRoiConfig& roi) {
    std::printf(
        "Traffic light ROI (normalized): %.6f,%.6f,%.6f,%.6f\n",
        roi.rect.left,
        roi.rect.top,
        roi.rect.right,
        roi.rect.bottom);
}

void PrintPerformance(const TrafficAppContext& context,
                      const PcieFrameSource& source,
                      uint64_t start_ms) {
    const double elapsed_seconds =
        std::max(0.001, (NowMilliseconds() - start_ms) / 1000.0);
    const uint64_t captured = context.performance.captured_frames.load();
    const uint64_t displayed = context.performance.displayed_frames.load();
    const uint64_t inference_jobs = context.performance.inference_jobs.load();
    const uint64_t inference_attempts =
        inference_jobs + context.performance.inference_failures.load();
    const PcieReadStatistics& driver = source.Statistics();

    std::printf("\n========== Traffic PCIe Statistics ==========\n");
    std::printf("Captured: %llu (%.2f fps), pool drops: %llu\n",
                static_cast<unsigned long long>(captured),
                captured / elapsed_seconds,
                static_cast<unsigned long long>(
                    context.performance.frame_pool_drops.load()));
    std::printf("Displayed: %llu (%.2f fps), queue drops: %llu, failures: %llu, overlay failures: %llu\n",
                static_cast<unsigned long long>(displayed),
                displayed / elapsed_seconds,
                static_cast<unsigned long long>(context.display_queue.Dropped()),
                static_cast<unsigned long long>(context.performance.display_failures.load()),
                static_cast<unsigned long long>(context.performance.overlay_failures.load()));
    std::printf("Inference: %llu (%.2f fps), interval=%d, queue drops: %llu, failures: %llu\n",
                static_cast<unsigned long long>(inference_jobs),
                inference_jobs / elapsed_seconds,
                context.inference_interval,
                static_cast<unsigned long long>(context.inference_queue.Dropped()),
                static_cast<unsigned long long>(
                    context.performance.inference_failures.load()));
    std::printf("Inference samples: tracked_person=%llu red_violation_events=%llu\n",
                static_cast<unsigned long long>(
                    context.performance.person_detections.load()),
                static_cast<unsigned long long>(
                    context.performance.violation_events.load()));
    if (inference_attempts > 0) {
        std::printf("Average inference/color-rule: %.2f / %.2f ms\n",
                    context.performance.inference_ms / inference_attempts,
                    context.performance.color_rule_ms / inference_attempts);
    }
    if (displayed > 0) {
        std::printf("Average display convert/overlay/present/end-to-end: %.2f / %.2f / %.2f / %.2f ms\n",
                    context.performance.display_convert_ms / displayed,
                    context.performance.overlay_ms / displayed,
                    context.performance.present_ms / displayed,
                    context.performance.end_to_end_ms / displayed);
    }
    std::printf("Driver retries: zero=%llu EPERM=%llu interrupted/EAGAIN=%llu fatal=%llu\n",
                static_cast<unsigned long long>(driver.zero_status_retries),
                static_cast<unsigned long long>(driver.permission_retries),
                static_cast<unsigned long long>(driver.interrupted_retries),
                static_cast<unsigned long long>(driver.other_errors));
    std::printf("=============================================\n");
}

int RunTrafficPcieDemo(const CommandLineOptions& options,
                       const PcieUiCallbacks* callbacks) {
    g_should_stop = 0;
    const bool qt_mode = callbacks != nullptr;
    const bool use_signal_handlers = !qt_mode;
    if (use_signal_handlers && InstallSignalHandlers() != 0) {
        std::fprintf(stderr, "Failed to install signal handlers\n");
        return -1;
    }

    FramePool frame_pool(kFramePoolCapacity);
    TrafficAppContext context;
    context.roi = options.roi;
    context.inference_interval = options.inference_interval;
    if (!options.light_roi.enabled) {
        std::fprintf(stderr, "Traffic light ROI is required\n");
        return -1;
    }
    context.light_box = resolve_traffic_light_roi(
        options.light_roi,
        PcieFrameSource::kFrameWidth,
        PcieFrameSource::kFrameHeight);
    PcieFrameSource source;
    const std::string labels_path = JoinPath(ParentPath(options.model_path), "labels_list.txt");

    std::printf("========================================\n");
    std::printf(" YOLOv8 Traffic PCIe BGR565 Demo\n");
    std::printf("========================================\n");
    std::printf("Model: %s\nLabels: %s\nInference interval: %d\n",
                options.model_path.c_str(), labels_path.c_str(),
                context.inference_interval);
    if (!options.roi_config_path.empty()) {
        std::printf("ROI config: %s (%s)\n",
                    options.roi_config_path.c_str(),
                    options.roi_config_loaded ? "loaded" : "built-in fallback");
    }
    PrintRoi(context.roi);
    PrintLightRoi(options.light_roi);

    if (init_traffic_post_process(labels_path.c_str()) != 0) {
        return -1;
    }
    if (init_traffic_yolov8_model(options.model_path.c_str(), &context.yolo) != 0) {
        deinit_traffic_post_process();
        return -1;
    }
    context.yolo.person_only = true;
    std::printf(
        "Postprocess: person only, confidence > %.2f; "
        "traffic-light color uses fixed ROI pixels\n",
        TRAFFIC_PERSON_BOX_THRESH);
    std::printf(
        "Temporal rules: person hold=2 inference frames, "
        "light acquire=3-of-5, switch=4-of-5\n");
    std::printf(
        "Violation rule: stable red + person inside 3%% inset crosswalk ROI\n");
    if (context.temporal_tracker.Init(context.roi) != 0) {
        std::fprintf(stderr, "Traffic temporal tracker initialization failed\n");
        release_traffic_yolov8_model(&context.yolo);
        deinit_traffic_post_process();
        return -1;
    }

    int overlay_width = PcieFrameSource::kFrameWidth;
    int overlay_height = PcieFrameSource::kFrameHeight;
    if (!qt_mode &&
        drm_display_init(&context.drm,
                         PcieFrameSource::kFrameWidth,
                         PcieFrameSource::kFrameHeight) != 0) {
        std::fprintf(stderr, "Traffic DRM display initialization failed\n");
        release_traffic_yolov8_model(&context.yolo);
        deinit_traffic_post_process();
        return -1;
    }
    if (!qt_mode) {
        context.drm_initialized = true;
        overlay_width = context.drm.ui_width;
        overlay_height = context.drm.ui_height;
    }
    if (context.overlay_renderer.Init(
            overlay_width, overlay_height, context.roi) != 0) {
        std::fprintf(stderr, "Traffic overlay initialization failed\n");
        if (context.drm_initialized) {
            drm_display_deinit(&context.drm);
            context.drm_initialized = false;
        }
        release_traffic_yolov8_model(&context.yolo);
        deinit_traffic_post_process();
        return -1;
    }

    if (source.Open() != 0) {
        if (context.drm_initialized) {
            drm_display_deinit(&context.drm);
            context.drm_initialized = false;
        }
        release_traffic_yolov8_model(&context.yolo);
        deinit_traffic_post_process();
        return -1;
    }

    const int signal_mask_result =
        use_signal_handlers ? SetTerminationSignalMask(SIG_BLOCK) : 0;
    if (use_signal_handlers && signal_mask_result != 0) {
        std::fprintf(stderr, "Failed to block worker termination signals: %s\n",
                     std::strerror(signal_mask_result));
        source.Close();
        if (context.drm_initialized) {
            drm_display_deinit(&context.drm);
            context.drm_initialized = false;
        }
        release_traffic_yolov8_model(&context.yolo);
        deinit_traffic_post_process();
        return -1;
    }

    const uint64_t start_ms = NowMilliseconds();
    std::thread inference_thread(InferenceThread, &context);
    std::thread display_thread(
        DisplayThread, &context, callbacks, std::cref(source), start_ms);
    const int signal_unmask_result =
        use_signal_handlers ? SetTerminationSignalMask(SIG_UNBLOCK) : 0;
    if (use_signal_handlers && signal_unmask_result != 0) {
        std::fprintf(stderr, "Failed to route termination signals to capture thread: %s\n",
                     std::strerror(signal_unmask_result));
        g_should_stop = 1;
    }

    if (callbacks != nullptr && callbacks->on_status) {
        callbacks->on_status(BuildUiStatus(
            context, source, start_ms, true, true, true,
            "等待首帧", GetLatestAnalysis(&context)));
    }

    std::vector<unsigned char> drain_buffer(PcieFrameSource::kFrameBytes);
    int next_frame_id = 0;
    int capture_result = 0;
    uint64_t last_status_ms = start_ms;
    uint64_t last_retry_log_ms = start_ms;
    std::printf("Waiting for 1280x720 BGR565 PCIe frames...\n");
    while (!g_should_stop) {
        if (callbacks != nullptr && callbacks->should_stop &&
            callbacks->should_stop()) {
            g_should_stop = 1;
            break;
        }
        const bool capture_enabled =
            !(callbacks != nullptr && callbacks->capture_enabled &&
              !callbacks->capture_enabled());
        std::shared_ptr<PcieFrame> frame =
            capture_enabled ? frame_pool.Acquire() : std::shared_ptr<PcieFrame>();
        unsigned char* destination = frame ? frame->pixels.data() : drain_buffer.data();
        const PcieFrameReadResult read_result =
            source.ReadFrame(destination, PcieFrameSource::kFrameBytes);
        if (read_result == PCIE_FRAME_RETRY) {
            const uint64_t now_ms = NowMilliseconds();
            if (now_ms - last_retry_log_ms >= 2000U) {
                const PcieReadStatistics& statistics = source.Statistics();
                std::fprintf(stderr,
                             "PCIe: waiting, last_status=%lld ready=%llu retries(zero=%llu EPERM=%llu EINTR/EAGAIN=%llu other=%llu)\n",
                             source.LastDriverStatus(),
                             static_cast<unsigned long long>(statistics.frames_ready),
                             static_cast<unsigned long long>(statistics.zero_status_retries),
                             static_cast<unsigned long long>(statistics.permission_retries),
                             static_cast<unsigned long long>(statistics.interrupted_retries),
                             static_cast<unsigned long long>(statistics.other_errors));
                last_retry_log_ms = now_ms;
            }
            if (callbacks != nullptr && callbacks->on_status &&
                now_ms - last_status_ms >= 1000U) {
                callbacks->on_status(BuildUiStatus(
                    context, source, start_ms, true, capture_enabled, true,
                    capture_enabled ? "等待PCIe帧数据" : "暂停中，保持PCIe读取",
                    GetLatestAnalysis(&context)));
                last_status_ms = now_ms;
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
            const uint64_t now_ms = NowMilliseconds();
            if (callbacks != nullptr && callbacks->on_status &&
                now_ms - last_status_ms >= 1000U) {
                callbacks->on_status(BuildUiStatus(
                    context, source, start_ms, true, false, true,
                    "暂停中，保持PCIe读取", GetLatestAnalysis(&context)));
                last_status_ms = now_ms;
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
        if ((frame_id % context.inference_interval) == 0) {
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
        drm_display_deinit(&context.drm);
        context.drm_initialized = false;
    }
    PrintPerformance(context, source, start_ms);

    const int release_result = release_traffic_yolov8_model(&context.yolo);
    deinit_traffic_post_process();
    return capture_result == 0 && release_result == 0 ? 0 : -1;
}

}  // namespace

int RunTrafficPcieQtDemo(const char* model_path,
                         const TrafficRoiConfig& roi,
                         const TrafficLightRoiConfig& light_roi,
                         const PcieUiCallbacks* callbacks) {
    if (model_path == nullptr || roi.points.size() < 3U ||
        !light_roi.enabled || callbacks == nullptr) {
        return -1;
    }
    CommandLineOptions options;
    options.model_path = model_path;
    options.roi = roi;
    options.light_roi = light_roi;
    return RunTrafficPcieDemo(options, callbacks);
}

#if !defined(TRAFFIC_QT_UI_BUILD)
int main(int argc, char** argv) {
    CommandLineOptions options;
    if (!ParseCommandLine(argc, argv, &options)) {
        PrintUsage(argv[0]);
        return EXIT_FAILURE;
    }
    return RunTrafficPcieDemo(options, nullptr) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
#endif
