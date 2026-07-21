#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>

#include "image_utils.h"
#include "ppocr_rec.h"

namespace {

int parse_count(const char* text, int minimum, const char* name, int* value)
{
    if (text == NULL || value == NULL) {
        return -1;
    }
    errno = 0;
    char* end = NULL;
    const long parsed = std::strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed < minimum ||
        parsed > INT_MAX) {
        std::printf("invalid %s: %s\n", name, text);
        return -1;
    }
    *value = static_cast<int>(parsed);
    return 0;
}

void add_perf(ppocr_rec_perf_t* total, const ppocr_rec_perf_t& value)
{
    total->preprocess_ms += value.preprocess_ms;
    total->input_set_ms += value.input_set_ms;
    total->rknn_run_wall_ms += value.rknn_run_wall_ms;
    total->rknn_official_ms += value.rknn_official_ms;
    total->output_get_ms += value.output_get_ms;
    total->postprocess_ms += value.postprocess_ms;
    total->total_ms += value.total_ms;
}

void print_average_perf(const ppocr_rec_perf_t& total, int repeat_count)
{
    const double scale = 1.0 / repeat_count;
    const double average_rknn_wall = total.rknn_run_wall_ms * scale;
    const double average_rknn_official = total.rknn_official_ms * scale;
    const double average_total = total.total_ms * scale;
    std::printf("[Performance] repeats: %d\n", repeat_count);
    std::printf("[Performance] average preprocess: %.3f ms\n",
                total.preprocess_ms * scale);
    std::printf("[Performance] average input set: %.3f ms\n",
                total.input_set_ms * scale);
    std::printf("[Performance] average RKNN official inference: %.3f ms\n",
                average_rknn_official);
    std::printf("[Performance] average rknn_run wall: %.3f ms\n",
                average_rknn_wall);
    std::printf("[Performance] average output get: %.3f ms\n",
                total.output_get_ms * scale);
    std::printf("[Performance] average CTC decode: %.3f ms\n",
                total.postprocess_ms * scale);
    std::printf("[Performance] average end-to-end: %.3f ms\n", average_total);
    std::printf("[Performance] RKNN official throughput: %.2f FPS\n",
                average_rknn_official > 0.0
                    ? 1000.0 / average_rknn_official
                    : 0.0);
    std::printf("[Performance] rknn_run wall throughput: %.2f FPS\n",
                average_rknn_wall > 0.0 ? 1000.0 / average_rknn_wall : 0.0);
    std::printf("[Performance] end-to-end throughput: %.2f FPS\n",
                average_total > 0.0 ? 1000.0 / average_total : 0.0);
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 4 || argc > 6) {
        std::printf("Usage: %s <model_path> <dictionary_path> <image_path> "
                    "[repeat_count=1] [warmup_count=0]\n",
                    argv[0]);
        return 1;
    }

    int repeat_count = 1;
    int warmup_count = 0;
    if (argc >= 5 && parse_count(argv[4], 1, "repeat_count", &repeat_count) != 0) {
        return 1;
    }
    if (argc >= 6 && parse_count(argv[5], 0, "warmup_count", &warmup_count) != 0) {
        return 1;
    }

    ppocr_rec_context_t app_ctx = {};
    int ret = init_ppocr_rec_model(argv[1], argv[2], &app_ctx);
    if (ret != 0) {
        std::printf("init_ppocr_rec_model failed: ret=%d\n", ret);
        return 1;
    }

    image_buffer_t source_image = {};
    ret = read_image(argv[3], &source_image);
    if (ret != 0) {
        std::printf("read image failed: ret=%d, path=%s\n", ret, argv[3]);
        release_ppocr_rec_model(&app_ctx);
        return 1;
    }
    std::printf("Test image: %s, width=%d, height=%d, format=%d\n",
                argv[3],
                source_image.width,
                source_image.height,
                source_image.format);

    ppocr_rec_result_t result;
    ppocr_rec_perf_t perf = {};
    for (int i = 0; i < warmup_count; ++i) {
        ret = inference_ppocr_rec_model(&app_ctx, &source_image, &result, &perf);
        if (ret != 0) {
            std::printf("warmup inference failed at %d/%d: ret=%d\n",
                        i + 1,
                        warmup_count,
                        ret);
            break;
        }
    }

    ppocr_rec_perf_t total_perf = {};
    if (ret == 0) {
        for (int i = 0; i < repeat_count; ++i) {
            ret = inference_ppocr_rec_model(&app_ctx, &source_image, &result, &perf);
            if (ret != 0) {
                std::printf("timed inference failed at %d/%d: ret=%d\n",
                            i + 1,
                            repeat_count,
                            ret);
                break;
            }
            add_perf(&total_perf, perf);
        }
    }

    if (ret == 0) {
        std::printf("PP-OCR result: %s, score=%.6f\n",
                    result.text.c_str(),
                    result.score);
        std::printf("Warmup runs: %d\n", warmup_count);
        print_average_perf(total_perf, repeat_count);
    }

    if (source_image.virt_addr != NULL) {
        std::free(source_image.virt_addr);
        source_image.virt_addr = NULL;
    }
    release_ppocr_rec_model(&app_ctx);
    return ret == 0 ? 0 : 1;
}
