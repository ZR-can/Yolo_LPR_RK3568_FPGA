#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "image_drawing.h"
#include "image_utils.h"
#include "yolov8.h"

namespace {

struct BenchmarkStats {
    int image_count = 0;
    int measured_calls = 0;
    int npu_samples = 0;
    int detection_count = 0;
    double pipeline_ms = 0.0;
    double npu_ms = 0.0;
};

std::string join_path(const std::string &left, const std::string &right)
{
    if (left.empty() || left == ".") {
        return left.empty() ? right : left + "/" + right;
    }
    if (left.back() == '/') {
        return left + right;
    }
    return left + "/" + right;
}

std::string parent_path(const std::string &path)
{
    const std::string::size_type pos = path.find_last_of("/\\");
    if (pos == std::string::npos) {
        return ".";
    }
    return pos == 0 ? path.substr(0, 1) : path.substr(0, pos);
}

std::string file_stem(const std::string &path)
{
    const std::string::size_type slash = path.find_last_of("/\\");
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const std::string::size_type dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

bool is_directory(const std::string &path)
{
    struct stat info;
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool is_regular_file(const std::string &path)
{
    struct stat info;
    return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

bool is_supported_image(const std::string &path)
{
    const std::string::size_type dot = path.find_last_of('.');
    if (dot == std::string::npos) {
        return false;
    }
    std::string extension = path.substr(dot + 1);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == "jpg" || extension == "jpeg" || extension == "png" || extension == "bmp";
}

bool collect_images(const std::string &input_path, std::vector<std::string> *images)
{
    if (is_regular_file(input_path)) {
        if (is_supported_image(input_path)) {
            images->push_back(input_path);
            return true;
        }
        std::fprintf(stderr, "Unsupported image type: %s\n", input_path.c_str());
        return false;
    }

    if (!is_directory(input_path)) {
        std::fprintf(stderr, "Input path does not exist: %s\n", input_path.c_str());
        return false;
    }

    DIR *directory = opendir(input_path.c_str());
    if (directory == nullptr) {
        std::fprintf(stderr, "Cannot open input directory: %s\n", input_path.c_str());
        return false;
    }

    for (dirent *entry = readdir(directory); entry != nullptr; entry = readdir(directory)) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        const std::string path = join_path(input_path, entry->d_name);
        if (is_regular_file(path) && is_supported_image(path)) {
            images->push_back(path);
        }
    }
    closedir(directory);
    std::sort(images->begin(), images->end());

    if (images->empty()) {
        std::fprintf(stderr, "No supported images found in: %s\n", input_path.c_str());
        return false;
    }
    return true;
}

bool make_directory(const std::string &path)
{
    if (path.empty() || path == "." || is_directory(path)) {
        return true;
    }

    for (std::string::size_type i = 1; i < path.size(); ++i) {
        if (path[i] != '/') {
            continue;
        }
        const std::string part = path.substr(0, i);
        if (!part.empty() && !is_directory(part)) {
            if (mkdir(part.c_str(), 0755) != 0 && (errno != EEXIST || !is_directory(part))) {
                return false;
            }
        }
    }
    if (mkdir(path.c_str(), 0755) == 0) {
        return true;
    }
    return errno == EEXIST && is_directory(path);
}

bool parse_count(const char *text, int minimum, int *value)
{
    char *end = nullptr;
    errno = 0;
    const long parsed = std::strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed < minimum || parsed > 1000000) {
        return false;
    }
    *value = static_cast<int>(parsed);
    return true;
}

unsigned int class_color(int class_id)
{
    static const unsigned int colors[OBJ_CLASS_NUM] = {
        COLOR_GREEN, COLOR_YELLOW, COLOR_BLUE, COLOR_MAGENTA,
        COLOR_ORANGE, COLOR_RED, COLOR_WHITE, COLOR_DARK_GRAY
    };
    return colors[class_id >= 0 && class_id < OBJ_CLASS_NUM ? class_id : 0];
}

void print_and_draw_detections(image_buffer_t *image, const object_detect_result_list &results)
{
    for (int i = 0; i < results.count; ++i) {
        const object_detect_result &detection = results.results[i];
        const char *name = traffic_cls_to_name(detection.cls_id);
        std::printf("  [%d] class=%s(%d), confidence=%.3f, box=(%d,%d,%d,%d)\n",
                    i, name, detection.cls_id, detection.prop,
                    detection.box.left, detection.box.top,
                    detection.box.right, detection.box.bottom);

        const int width = detection.box.right - detection.box.left;
        const int height = detection.box.bottom - detection.box.top;
        if (width <= 0 || height <= 0) {
            continue;
        }

        const unsigned int color = class_color(detection.cls_id);
        draw_rectangle_alpha(image, detection.box.left, detection.box.top, width, height, color, 3, 255);

        char label[OBJ_NAME_MAX_SIZE + 16];
        std::snprintf(label, sizeof(label), "%s %.2f", name, detection.prop);
        const int text_y = detection.box.top >= 24 ? detection.box.top - 24 : detection.box.top;
        draw_text(image, label, detection.box.left, text_y, color, 20);
    }
}

double fps_from_ms(double average_ms)
{
    return average_ms > 0.0 ? 1000.0 / average_ms : 0.0;
}

bool benchmark_image(const std::string &image_path,
                     const std::string &output_directory,
                     int warmup_count,
                     int repeat_count,
                     rknn_app_context_t *context,
                     BenchmarkStats *stats)
{
    image_buffer_t image;
    std::memset(&image, 0, sizeof(image));
    if (read_image(image_path.c_str(), &image) != 0) {
        std::fprintf(stderr, "Failed to read image: %s\n", image_path.c_str());
        return false;
    }

    object_detect_result_list detections;
    std::memset(&detections, 0, sizeof(detections));

    for (int i = 0; i < warmup_count; ++i) {
        if (inference_yolov8_model(context, &image, &detections) != 0) {
            std::fprintf(stderr, "Warmup failed for: %s\n", image_path.c_str());
            std::free(image.virt_addr);
            return false;
        }
    }

    double image_pipeline_ms = 0.0;
    double image_npu_ms = 0.0;
    int image_npu_samples = 0;
    for (int i = 0; i < repeat_count; ++i) {
        const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        const int ret = inference_yolov8_model(context, &image, &detections);
        const std::chrono::steady_clock::time_point finish = std::chrono::steady_clock::now();
        if (ret != 0) {
            std::fprintf(stderr, "Inference failed for: %s\n", image_path.c_str());
            std::free(image.virt_addr);
            return false;
        }

        image_pipeline_ms += std::chrono::duration<double, std::milli>(finish - start).count();

        rknn_perf_run perf;
        std::memset(&perf, 0, sizeof(perf));
        if (rknn_query(context->rknn_ctx, RKNN_QUERY_PERF_RUN, &perf, sizeof(perf)) == RKNN_SUCC &&
            perf.run_duration > 0) {
            image_npu_ms += static_cast<double>(perf.run_duration) / 1000.0;
            ++image_npu_samples;
        }
    }

    const double average_pipeline_ms = image_pipeline_ms / repeat_count;
    std::printf("\n[image] %s\n", image_path.c_str());
    std::printf("  detections=%d\n", detections.count);
    std::printf("  pipeline: %.3f ms, %.2f FPS (repeat=%d, warmup=%d)\n",
                average_pipeline_ms, fps_from_ms(average_pipeline_ms), repeat_count, warmup_count);
    if (image_npu_samples > 0) {
        const double average_npu_ms = image_npu_ms / image_npu_samples;
        std::printf("  NPU:      %.3f ms, %.2f FPS\n", average_npu_ms, fps_from_ms(average_npu_ms));
    } else {
        std::printf("  NPU:      unavailable (RKNN_QUERY_PERF_RUN failed)\n");
    }

    print_and_draw_detections(&image, detections);
    const std::string output_path = join_path(output_directory, file_stem(image_path) + "_detected.png");
    if (write_image(output_path.c_str(), &image) <= 0) {
        std::fprintf(stderr, "Failed to write result image: %s\n", output_path.c_str());
        std::free(image.virt_addr);
        return false;
    }
    std::printf("  output=%s\n", output_path.c_str());

    ++stats->image_count;
    stats->measured_calls += repeat_count;
    stats->detection_count += detections.count;
    stats->pipeline_ms += image_pipeline_ms;
    stats->npu_samples += image_npu_samples;
    stats->npu_ms += image_npu_ms;
    std::free(image.virt_addr);
    return true;
}

void write_summary(const std::string &output_directory,
                   int warmup_count,
                   int repeat_count,
                   const BenchmarkStats &stats)
{
    const double average_pipeline_ms = stats.measured_calls > 0
                                           ? stats.pipeline_ms / stats.measured_calls
                                           : 0.0;
    const double average_npu_ms = stats.npu_samples > 0 ? stats.npu_ms / stats.npu_samples : 0.0;

    std::ostringstream summary;
    summary << std::fixed << std::setprecision(3)
            << "images=" << stats.image_count << '\n'
            << "detections=" << stats.detection_count << '\n'
            << "warmup_per_image=" << warmup_count << '\n'
            << "repeat_per_image=" << repeat_count << '\n'
            << "measured_calls=" << stats.measured_calls << '\n'
            << "pipeline_average_ms=" << average_pipeline_ms << '\n'
            << "pipeline_fps=" << fps_from_ms(average_pipeline_ms) << '\n'
            << "npu_samples=" << stats.npu_samples << '\n'
            << "npu_average_ms=" << average_npu_ms << '\n'
            << "npu_fps=" << fps_from_ms(average_npu_ms) << '\n';

    std::printf("\n[summary]\n%s", summary.str().c_str());
    const std::string summary_path = join_path(output_directory, "summary.txt");
    std::ofstream output(summary_path.c_str());
    output << summary.str();
    std::printf("summary_file=%s\n", summary_path.c_str());
}

void print_usage(const char *program)
{
    std::printf("Usage: %s <model.rknn> <image_or_directory> [repeat=20] [warmup=3] [output_dir=./outputs]\n",
                program);
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 3 || argc > 6) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    int repeat_count = 20;
    int warmup_count = 3;
    if ((argc >= 4 && !parse_count(argv[3], 1, &repeat_count)) ||
        (argc >= 5 && !parse_count(argv[4], 0, &warmup_count))) {
        std::fprintf(stderr, "repeat must be >= 1 and warmup must be >= 0\n");
        return EXIT_FAILURE;
    }

    const std::string model_path = argv[1];
    const std::string input_path = argv[2];
    const std::string output_directory = argc >= 6 ? argv[5] : "./outputs";
    const std::string label_path = join_path(parent_path(model_path), "labels_list.txt");

    std::vector<std::string> images;
    if (!is_regular_file(model_path) || !collect_images(input_path, &images)) {
        if (!is_regular_file(model_path)) {
            std::fprintf(stderr, "Model file does not exist: %s\n", model_path.c_str());
        }
        return EXIT_FAILURE;
    }
    if (!make_directory(output_directory)) {
        std::fprintf(stderr, "Cannot create output directory: %s\n", output_directory.c_str());
        return EXIT_FAILURE;
    }

    if (init_post_process(label_path.c_str()) != 0) {
        return EXIT_FAILURE;
    }

    rknn_app_context_t context;
    std::memset(&context, 0, sizeof(context));
    context.person_light_only = false;
    if (init_yolov8_model(model_path.c_str(), &context) != 0) {
        deinit_post_process();
        return EXIT_FAILURE;
    }

    std::printf("Benchmark images=%zu, repeat=%d, warmup=%d\n",
                images.size(), repeat_count, warmup_count);
    BenchmarkStats stats;
    bool succeeded = true;
    for (const std::string &image_path : images) {
        if (!benchmark_image(image_path, output_directory, warmup_count, repeat_count, &context, &stats)) {
            succeeded = false;
            break;
        }
    }

    if (stats.image_count > 0) {
        write_summary(output_directory, warmup_count, repeat_count, stats);
    }

    const int release_ret = release_yolov8_model(&context);
    deinit_post_process();
    return succeeded && release_ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
