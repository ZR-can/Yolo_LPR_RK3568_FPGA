#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <string>
#include <vector>

#include "image_utils.h"
#include "ppocr_rec.h"

/*
cd /userdata/rknn_yolov8_ppocr_demo/ppocr_rec_eval_demo
export LD_LIBRARY_PATH=../lib:$LD_LIBRARY_PATH

./eval_ppocr_rknn_subsets.sh \
  ./model/ppocrv4_rec14_rk3568_i8.rknn \
  ./model/cblprd_plate_dict.txt \
  /userdata/cblprd_eval/CBLPRD \
  /userdata/cblprd_eval/CBLPRD \
  ./result/ppocr_i8 \
  20 \
  500

mkdir -p result/ppocr_i8_board

adb pull \
  /userdata/rknn_yolov8_ppocr_demo/ppocr_rec_eval_demo/result/ppocr_i8/. \
  result/ppocr_i8_board/
*/


namespace {

typedef std::chrono::steady_clock Clock;

struct manifest_spec_t {
    std::string subset;
    std::string path;
};

struct sample_t {
    std::string relative_path;
    std::string image_path;
    std::string label;
};

struct subset_data_t {
    manifest_spec_t spec;
    std::vector<sample_t> samples;
};

struct eval_stats_t {
    long long samples;
    long long correct;
    double score_sum;
    ppocr_rec_perf_t perf;
};

double elapsed_ms(const Clock::time_point& start, const Clock::time_point& end)
{
    return std::chrono::duration<double, std::milli>(end - start).count();
}

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
        std::fprintf(stderr, "invalid %s: %s\n", name, text);
        return -1;
    }
    *value = static_cast<int>(parsed);
    return 0;
}

void strip_carriage_return(std::string* line)
{
    if (line != NULL && !line->empty() && (*line)[line->size() - 1] == '\r') {
        line->erase(line->size() - 1);
    }
}

std::string join_path(const std::string& root, const std::string& path)
{
    if (!path.empty() && path[0] == '/') {
        return path;
    }
    if (root.empty() || root[root.size() - 1] == '/') {
        return root + path;
    }
    return root + "/" + path;
}

int load_manifest_specs(const char* path, std::vector<manifest_spec_t>* specs)
{
    std::ifstream input(path);
    if (!input.is_open()) {
        std::fprintf(stderr, "open manifest list failed: %s\n", path);
        return -1;
    }

    specs->clear();
    std::string line;
    int line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        strip_carriage_return(&line);
        if (line.empty()) {
            continue;
        }
        const size_t separator = line.find('\t');
        if (separator == std::string::npos || separator == 0 ||
            separator + 1 >= line.size()) {
            std::fprintf(stderr,
                         "invalid manifest list row: %s:%d\n",
                         path,
                         line_number);
            return -1;
        }
        manifest_spec_t spec;
        spec.subset = line.substr(0, separator);
        spec.path = line.substr(separator + 1);
        specs->push_back(spec);
    }
    if (specs->empty()) {
        std::fprintf(stderr, "manifest list is empty: %s\n", path);
        return -1;
    }
    return 0;
}

int load_samples(const manifest_spec_t& spec,
                 const std::string& data_root,
                 std::vector<sample_t>* samples)
{
    std::ifstream input(spec.path.c_str());
    if (!input.is_open()) {
        std::fprintf(stderr, "open subset manifest failed: %s\n", spec.path.c_str());
        return -1;
    }

    samples->clear();
    std::string line;
    int line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        strip_carriage_return(&line);
        if (line.empty()) {
            continue;
        }
        const size_t separator = line.find('\t');
        if (separator == std::string::npos || separator == 0 ||
            separator + 1 >= line.size()) {
            std::fprintf(stderr,
                         "invalid manifest row: %s:%d\n",
                         spec.path.c_str(),
                         line_number);
            return -1;
        }
        sample_t sample;
        sample.relative_path = line.substr(0, separator);
        sample.image_path = join_path(data_root, sample.relative_path);
        sample.label = line.substr(separator + 1);
        samples->push_back(sample);
    }
    if (samples->empty()) {
        std::fprintf(stderr, "subset manifest is empty: %s\n", spec.path.c_str());
        return -1;
    }
    return 0;
}

void release_image(image_buffer_t* image)
{
    if (image != NULL && image->virt_addr != NULL) {
        std::free(image->virt_addr);
        image->virt_addr = NULL;
    }
}

int load_rgb_image(const std::string& path, image_buffer_t* image)
{
    if (image == NULL) {
        return -1;
    }
    std::memset(image, 0, sizeof(*image));
    const int ret = read_image(path.c_str(), image);
    if (ret != 0 || image->virt_addr == NULL) {
        std::fprintf(stderr, "decode image failed: %s\n", path.c_str());
        release_image(image);
        return -1;
    }
    if (image->format != IMAGE_FORMAT_RGB888) {
        std::fprintf(stderr,
                     "unsupported decoded image format: path=%s, format=%d\n",
                     path.c_str(),
                     image->format);
        release_image(image);
        return -1;
    }
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

void merge_stats(eval_stats_t* total, const eval_stats_t& value)
{
    total->samples += value.samples;
    total->correct += value.correct;
    total->score_sum += value.score_sum;
    add_perf(&total->perf, value.perf);
}

void print_stats(const char* prefix,
                 const std::string& subset,
                 const eval_stats_t& stats)
{
    const double scale = stats.samples > 0 ? 1.0 / stats.samples : 0.0;
    const double accuracy = stats.correct * scale;
    const double average_score = stats.score_sum * scale;
    const double average_official = stats.perf.rknn_official_ms * scale;
    const double average_wall = stats.perf.rknn_run_wall_ms * scale;
    const double average_end_to_end = stats.perf.total_ms * scale;

    std::printf("%-12s %10lld %10lld %12.4f%% %12.6f %14.3f %14.3f\n",
                subset.c_str(),
                stats.samples,
                stats.correct,
                accuracy * 100.0,
                average_score,
                average_official,
                average_end_to_end);
    std::printf(
        "%s subset=%s samples=%lld correct=%lld accuracy=%.8f "
        "average_score=%.8f average_rknn_official_ms=%.6f "
        "average_rknn_wall_ms=%.6f average_inference_e2e_ms=%.6f\n",
        prefix,
        subset.c_str(),
        stats.samples,
        stats.correct,
        accuracy,
        average_score,
        average_official,
        average_wall,
        average_end_to_end);
}

int warmup_model(ppocr_rec_context_t* app_ctx,
                 const sample_t& sample,
                 int warmup_count)
{
    if (warmup_count == 0) {
        return 0;
    }
    image_buffer_t image = {};
    if (load_rgb_image(sample.image_path, &image) != 0) {
        return -1;
    }

    ppocr_rec_result_t result;
    ppocr_rec_perf_t perf = {};
    for (int i = 0; i < warmup_count; ++i) {
        const int ret = inference_ppocr_rec_model(
            app_ctx, &image, &result, &perf);
        if (ret != 0) {
            std::fprintf(stderr,
                         "warmup failed at %d/%d: ret=%d\n",
                         i + 1,
                         warmup_count,
                         ret);
            release_image(&image);
            return ret;
        }
    }
    release_image(&image);
    return 0;
}

int evaluate(ppocr_rec_context_t* app_ctx,
             const std::vector<subset_data_t>& subsets,
             const char* details_path,
             int warmup_count,
             int progress_step)
{
    std::ofstream details(details_path);
    if (!details.is_open()) {
        std::fprintf(stderr, "open details output failed: %s\n", details_path);
        return -1;
    }
    details << "subset\timage\tlabel\tprediction\tconfidence\tcorrect\t"
               "rknn_official_ms\trknn_wall_ms\tinference_e2e_ms\n";
    details << std::fixed << std::setprecision(8);

    if (warmup_model(app_ctx, subsets[0].samples[0], warmup_count) != 0) {
        return -1;
    }

    std::printf("Warmup runs: %d\n", warmup_count);
    std::printf("%-12s %10s %10s %13s %12s %14s %14s\n",
                "subset",
                "samples",
                "correct",
                "accuracy",
                "avg_score",
                "official_ms",
                "infer_e2e_ms");

    eval_stats_t total = {};
    const Clock::time_point dataset_start = Clock::now();
    for (size_t subset_index = 0; subset_index < subsets.size(); ++subset_index) {
        const subset_data_t& subset = subsets[subset_index];
        eval_stats_t stats = {};
        for (size_t sample_index = 0; sample_index < subset.samples.size();
            ++sample_index) {
            const sample_t& sample = subset.samples[sample_index];
            image_buffer_t image = {};
            if (load_rgb_image(sample.image_path, &image) != 0) {
                return -1;
            }

            ppocr_rec_result_t result;
            ppocr_rec_perf_t perf = {};
            const int ret = inference_ppocr_rec_model(
                app_ctx, &image, &result, &perf);
            release_image(&image);
            if (ret != 0) {
                std::fprintf(stderr,
                             "inference failed: subset=%s, image=%s, ret=%d\n",
                             subset.spec.subset.c_str(),
                             sample.image_path.c_str(),
                             ret);
                return ret;
            }

            const bool correct = result.text == sample.label;
            ++stats.samples;
            stats.correct += correct ? 1 : 0;
            stats.score_sum += result.score;
            add_perf(&stats.perf, perf);

            details << subset.spec.subset << '\t' << sample.relative_path << '\t'
                    << sample.label << '\t' << result.text << '\t'
                    << result.score << '\t' << (correct ? 1 : 0) << '\t'
                    << perf.rknn_official_ms << '\t' << perf.rknn_run_wall_ms
                    << '\t' << perf.total_ms << '\n';

            if (progress_step > 0 && stats.samples % progress_step == 0) {
                std::fprintf(stderr,
                             "[%s] processed %lld/%zu, accuracy=%.4f%%\n",
                             subset.spec.subset.c_str(),
                             stats.samples,
                             subset.samples.size(),
                             100.0 * stats.correct / stats.samples);
            }
        }
        if (!details.good()) {
            std::fprintf(stderr, "write details output failed: %s\n", details_path);
            return -1;
        }
        print_stats("EVAL_SUBSET", subset.spec.subset, stats);
        merge_stats(&total, stats);
    }

    const double dataset_elapsed = elapsed_ms(dataset_start, Clock::now());
    print_stats("EVAL_TOTAL", "TOTAL", total);
    std::printf("EVAL_DATASET samples=%lld elapsed_ms=%.3f throughput=%.3f "
                "details=%s\n",
                total.samples,
                dataset_elapsed,
                dataset_elapsed > 0.0 ? total.samples * 1000.0 / dataset_elapsed
                                     : 0.0,
                details_path);
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 6 || argc > 8) {
        std::printf(
            "Usage: %s <model_path> <dictionary_path> <manifest_list.tsv> "
            "<data_root> <details.tsv> [warmup_count=20] "
            "[progress_step=500]\n",
            argv[0]);
        return 1;
    }

    int warmup_count = 20;
    int progress_step = 500;
    if (argc >= 7 &&
        parse_count(argv[6], 0, "warmup_count", &warmup_count) != 0) {
        return 1;
    }
    if (argc >= 8 &&
        parse_count(argv[7], 0, "progress_step", &progress_step) != 0) {
        return 1;
    }

    std::vector<manifest_spec_t> specs;
    if (load_manifest_specs(argv[3], &specs) != 0) {
        return 1;
    }
    std::vector<subset_data_t> subsets;
    for (size_t i = 0; i < specs.size(); ++i) {
        subset_data_t subset;
        subset.spec = specs[i];
        if (load_samples(specs[i], argv[4], &subset.samples) != 0) {
            return 1;
        }
        subsets.push_back(subset);
    }

    ppocr_rec_context_t app_ctx = {};
    int ret = init_ppocr_rec_model(argv[1], argv[2], &app_ctx);
    if (ret != 0) {
        std::fprintf(stderr, "init_ppocr_rec_model failed: ret=%d\n", ret);
        return 1;
    }

    ret = evaluate(&app_ctx,
                   subsets,
                   argv[5],
                   warmup_count,
                   progress_step);
    release_ppocr_rec_model(&app_ctx);
    return ret == 0 ? 0 : 1;
}
