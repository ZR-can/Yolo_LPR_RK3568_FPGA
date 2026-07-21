#include "ppocr_rec.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>

#include "file_utils.h"
#include "opencv2/imgproc.hpp"

namespace {

const int kExpectedInputHeight = 48;
const int kExpectedInputWidth = 160;
const int kExpectedInputChannel = 3;
const int kExpectedOutputSequence = 20;
const int kExpectedDictionarySize = 73;
const int kExpectedOutputClasses = kExpectedDictionarySize + 1;
const uint8_t kPaddingValue = 128;

typedef std::chrono::steady_clock Clock;

double elapsed_ms(const Clock::time_point& start, const Clock::time_point& end)
{
    return std::chrono::duration<double, std::milli>(end - start).count();
}

void dump_tensor_attr(const char* label, const rknn_tensor_attr& attr)
{
    std::printf("%s: index=%u, name=%s, dims=[", label, attr.index, attr.name);
    for (uint32_t i = 0; i < attr.n_dims; ++i) {
        std::printf("%s%u", i == 0 ? "" : ",", attr.dims[i]);
    }
    std::printf(
        "], n_elems=%u, size=%u, fmt=%s, type=%s, qnt_type=%s, zp=%d, scale=%f\n",
        attr.n_elems,
        attr.size,
        get_format_string(attr.fmt),
        get_type_string(attr.type),
        get_qnt_type_string(attr.qnt_type),
        attr.zp,
        attr.scale);
}

int load_dictionary(const char* path, std::vector<std::string>* dictionary)
{
    if (path == NULL || dictionary == NULL) {
        return -1;
    }

    std::ifstream input(path);
    if (!input.is_open()) {
        std::printf("open dictionary failed: %s\n", path);
        return -1;
    }

    dictionary->clear();
    std::set<std::string> unique_entries;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line[line.size() - 1] == '\r') {
            line.erase(line.size() - 1);
        }
        if (line.empty()) {
            std::printf("dictionary contains an empty entry: %s\n", path);
            return -1;
        }
        if (!unique_entries.insert(line).second) {
            std::printf("dictionary contains duplicate entry: %s\n", line.c_str());
            return -1;
        }
        dictionary->push_back(line);
    }

    if (dictionary->size() != static_cast<size_t>(kExpectedDictionarySize)) {
        std::printf("dictionary entry count mismatch: expected=%d, actual=%zu\n",
                    kExpectedDictionarySize,
                    dictionary->size());
        dictionary->clear();
        return -1;
    }
    return 0;
}

int validate_input_attr(ppocr_rec_context_t* app_ctx)
{
    const rknn_tensor_attr& attr = app_ctx->input_attr;
    if (attr.n_dims != 4 || attr.dims[0] != 1) {
        std::printf("unsupported PP-OCR input rank or batch\n");
        return -1;
    }

    if (attr.fmt == RKNN_TENSOR_NCHW) {
        app_ctx->model_channel = attr.dims[1];
        app_ctx->model_height = attr.dims[2];
        app_ctx->model_width = attr.dims[3];
    } else if (attr.fmt == RKNN_TENSOR_NHWC) {
        app_ctx->model_height = attr.dims[1];
        app_ctx->model_width = attr.dims[2];
        app_ctx->model_channel = attr.dims[3];
    } else {
        std::printf("unsupported PP-OCR input format: %s\n", get_format_string(attr.fmt));
        return -1;
    }

    if (app_ctx->model_height != kExpectedInputHeight ||
        app_ctx->model_width != kExpectedInputWidth ||
        app_ctx->model_channel != kExpectedInputChannel) {
        std::printf("PP-OCR input shape mismatch: expected=1x3x48x160, actual=%dx%dx%d\n",
                    app_ctx->model_channel,
                    app_ctx->model_height,
                    app_ctx->model_width);
        return -1;
    }
    return 0;
}

int validate_output_attr(ppocr_rec_context_t* app_ctx)
{
    const rknn_tensor_attr& attr = app_ctx->output_attr;
    if (attr.n_dims != 3 || attr.dims[0] != 1 ||
        attr.dims[1] != kExpectedOutputSequence ||
        attr.dims[2] != kExpectedOutputClasses ||
        attr.n_elems != kExpectedOutputSequence * kExpectedOutputClasses) {
        std::printf("PP-OCR output shape mismatch: expected=[1,20,74]\n");
        return -1;
    }

    app_ctx->output_sequence_length = attr.dims[1];
    app_ctx->output_class_count = attr.dims[2];
    if (app_ctx->output_class_count !=
        static_cast<int>(app_ctx->dictionary.size()) + 1) {
        std::printf("PP-OCR output classes do not match dictionary + CTC blank\n");
        return -1;
    }
    return 0;
}

int preprocess_image(ppocr_rec_context_t* app_ctx, const image_buffer_t* src_image)
{
    if (src_image == NULL || src_image->virt_addr == NULL ||
        src_image->format != IMAGE_FORMAT_RGB888 || src_image->width <= 0 ||
        src_image->height <= 0 ||
        src_image->size < src_image->width * src_image->height * 3) {
        std::printf("invalid source image for PP-OCR preprocessing\n");
        return -1;
    }

    cv::Mat rgb(src_image->height,
                src_image->width,
                CV_8UC3,
                const_cast<unsigned char*>(src_image->virt_addr));
    cv::Mat bgr;
    cv::cvtColor(rgb, bgr, cv::COLOR_RGB2BGR);

    const double ratio = static_cast<double>(src_image->width) / src_image->height;
    const int resized_width = std::min(
        app_ctx->model_width,
        static_cast<int>(std::ceil(app_ctx->model_height * ratio)));
    cv::Mat resized;
    cv::resize(bgr,
               resized,
               cv::Size(resized_width, app_ctx->model_height),
               0.0,
               0.0,
               cv::INTER_LINEAR);

    const size_t input_size = static_cast<size_t>(app_ctx->model_width) *
                              app_ctx->model_height * app_ctx->model_channel;
    app_ctx->input_buffer.assign(input_size, kPaddingValue);
    cv::Mat prepared(app_ctx->model_height,
                     app_ctx->model_width,
                     CV_8UC3,
                     app_ctx->input_buffer.data());
    resized.copyTo(prepared(cv::Rect(0, 0, resized_width, app_ctx->model_height)));
    return 0;
}

int ctc_decode(const float* output,
               const ppocr_rec_context_t* app_ctx,
               ppocr_rec_result_t* result)
{
    if (output == NULL || app_ctx == NULL || result == NULL) {
        return -1;
    }

    result->text.clear();
    result->score = 0.0f;
    float score_sum = 0.0f;
    int character_count = 0;
    int previous_index = -1;

    for (int sequence = 0; sequence < app_ctx->output_sequence_length; ++sequence) {
        const float* row = output + sequence * app_ctx->output_class_count;
        const float* maximum =
            std::max_element(row, row + app_ctx->output_class_count);
        const int index = static_cast<int>(maximum - row);

        if (index != 0 && index != previous_index) {
            const int dictionary_index = index - 1;
            if (dictionary_index < 0 ||
                dictionary_index >= static_cast<int>(app_ctx->dictionary.size())) {
                std::printf("CTC class index is outside the dictionary: %d\n", index);
                return -1;
            }
            result->text += app_ctx->dictionary[dictionary_index];
            score_sum += *maximum;
            ++character_count;
        }
        previous_index = index;
    }

    if (character_count > 0) {
        result->score = score_sum / character_count;
    }
    return 0;
}

}  // namespace

int init_ppocr_rec_model(const char* model_path,
                         const char* dictionary_path,
                         ppocr_rec_context_t* app_ctx)
{
    if (model_path == NULL || dictionary_path == NULL || app_ctx == NULL) {
        return -1;
    }
    if (app_ctx->rknn_ctx != 0) {
        std::printf("PP-OCR context is already initialized\n");
        return -1;
    }
    if (load_dictionary(dictionary_path, &app_ctx->dictionary) != 0) {
        return -1;
    }

    char* model_data = NULL;
    const int model_size = read_data_from_file(model_path, &model_data);
    if (model_size <= 0 || model_data == NULL) {
        std::printf("load PP-OCR model failed: %s\n", model_path);
        app_ctx->dictionary.clear();
        return -1;
    }

    rknn_context rknn_ctx = 0;
    int ret = rknn_init(&rknn_ctx, model_data, model_size, 0, NULL);
    std::free(model_data);
    if (ret != RKNN_SUCC) {
        std::printf("rknn_init failed: ret=%d, model=%s\n", ret, model_path);
        app_ctx->dictionary.clear();
        return ret;
    }
    app_ctx->rknn_ctx = rknn_ctx;

    rknn_sdk_version sdk_version;
    std::memset(&sdk_version, 0, sizeof(sdk_version));
    ret = rknn_query(app_ctx->rknn_ctx,
                     RKNN_QUERY_SDK_VERSION,
                     &sdk_version,
                     sizeof(sdk_version));
    if (ret == RKNN_SUCC) {
        std::printf("RKNN API version: %s\n", sdk_version.api_version);
        std::printf("RKNN driver version: %s\n", sdk_version.drv_version);
    }

    rknn_input_output_num io_num;
    std::memset(&io_num, 0, sizeof(io_num));
    ret = rknn_query(app_ctx->rknn_ctx,
                     RKNN_QUERY_IN_OUT_NUM,
                     &io_num,
                     sizeof(io_num));
    if (ret != RKNN_SUCC || io_num.n_input != 1 || io_num.n_output != 1) {
        std::printf("PP-OCR requires exactly one input and one output: ret=%d, in=%u, out=%u\n",
                    ret,
                    io_num.n_input,
                    io_num.n_output);
        release_ppocr_rec_model(app_ctx);
        return -1;
    }

    std::memset(&app_ctx->input_attr, 0, sizeof(app_ctx->input_attr));
    app_ctx->input_attr.index = 0;
    ret = rknn_query(app_ctx->rknn_ctx,
                     RKNN_QUERY_INPUT_ATTR,
                     &app_ctx->input_attr,
                     sizeof(app_ctx->input_attr));
    if (ret != RKNN_SUCC) {
        std::printf("query PP-OCR input attribute failed: ret=%d\n", ret);
        release_ppocr_rec_model(app_ctx);
        return ret;
    }

    std::memset(&app_ctx->output_attr, 0, sizeof(app_ctx->output_attr));
    app_ctx->output_attr.index = 0;
    ret = rknn_query(app_ctx->rknn_ctx,
                     RKNN_QUERY_OUTPUT_ATTR,
                     &app_ctx->output_attr,
                     sizeof(app_ctx->output_attr));
    if (ret != RKNN_SUCC) {
        std::printf("query PP-OCR output attribute failed: ret=%d\n", ret);
        release_ppocr_rec_model(app_ctx);
        return ret;
    }

    dump_tensor_attr("PP-OCR input", app_ctx->input_attr);
    dump_tensor_attr("PP-OCR output", app_ctx->output_attr);
    if (validate_input_attr(app_ctx) != 0 || validate_output_attr(app_ctx) != 0) {
        release_ppocr_rec_model(app_ctx);
        return -1;
    }

    std::printf("PP-OCR dictionary entries: %zu\n", app_ctx->dictionary.size());
    std::printf("PP-OCR input mode: UINT8 BGR, right padding=%u, "
                "RKNN embedded normalization=(x-127.5)/127.5\n",
                kPaddingValue);
    return 0;
}

int release_ppocr_rec_model(ppocr_rec_context_t* app_ctx)
{
    if (app_ctx == NULL) {
        return -1;
    }
    if (app_ctx->rknn_ctx != 0) {
        rknn_destroy(app_ctx->rknn_ctx);
        app_ctx->rknn_ctx = 0;
    }
    app_ctx->dictionary.clear();
    app_ctx->input_buffer.clear();
    app_ctx->model_width = 0;
    app_ctx->model_height = 0;
    app_ctx->model_channel = 0;
    app_ctx->output_sequence_length = 0;
    app_ctx->output_class_count = 0;
    return 0;
}

int inference_ppocr_rec_model(ppocr_rec_context_t* app_ctx,
                              const image_buffer_t* src_image,
                              ppocr_rec_result_t* result,
                              ppocr_rec_perf_t* perf)
{
    if (app_ctx == NULL || app_ctx->rknn_ctx == 0 || result == NULL || perf == NULL) {
        return -1;
    }
    std::memset(perf, 0, sizeof(*perf));
    const Clock::time_point total_start = Clock::now();

    Clock::time_point stage_start = Clock::now();
    int ret = preprocess_image(app_ctx, src_image);
    Clock::time_point stage_end = Clock::now();
    perf->preprocess_ms = elapsed_ms(stage_start, stage_end);
    if (ret != 0) {
        return ret;
    }

    rknn_input input;
    std::memset(&input, 0, sizeof(input));
    input.index = 0;
    input.type = RKNN_TENSOR_UINT8;
    input.fmt = RKNN_TENSOR_NHWC;
    input.size = static_cast<uint32_t>(app_ctx->input_buffer.size());
    input.buf = app_ctx->input_buffer.data();

    stage_start = Clock::now();
    ret = rknn_inputs_set(app_ctx->rknn_ctx, 1, &input);
    stage_end = Clock::now();
    perf->input_set_ms = elapsed_ms(stage_start, stage_end);
    if (ret != RKNN_SUCC) {
        std::printf("rknn_inputs_set failed: ret=%d\n", ret);
        return ret;
    }

    stage_start = Clock::now();
    ret = rknn_run(app_ctx->rknn_ctx, NULL);
    stage_end = Clock::now();
    perf->rknn_run_wall_ms = elapsed_ms(stage_start, stage_end);
    if (ret != RKNN_SUCC) {
        std::printf("rknn_run failed: ret=%d\n", ret);
        return ret;
    }

    rknn_output output;
    std::memset(&output, 0, sizeof(output));
    output.want_float = 1;
    stage_start = Clock::now();
    ret = rknn_outputs_get(app_ctx->rknn_ctx, 1, &output, NULL);
    stage_end = Clock::now();
    perf->output_get_ms = elapsed_ms(stage_start, stage_end);
    if (ret != RKNN_SUCC || output.buf == NULL) {
        std::printf("rknn_outputs_get failed: ret=%d\n", ret);
        return ret == RKNN_SUCC ? -1 : ret;
    }

    rknn_perf_run official_perf;
    std::memset(&official_perf, 0, sizeof(official_perf));
    const Clock::time_point perf_query_start = Clock::now();
    ret = rknn_query(app_ctx->rknn_ctx,
                     RKNN_QUERY_PERF_RUN,
                     &official_perf,
                     sizeof(official_perf));
    const double perf_query_ms = elapsed_ms(perf_query_start, Clock::now());
    if (ret != RKNN_SUCC || official_perf.run_duration <= 0) {
        std::printf("RKNN_QUERY_PERF_RUN failed: ret=%d, duration=%lld us\n",
                    ret,
                    static_cast<long long>(official_perf.run_duration));
        rknn_outputs_release(app_ctx->rknn_ctx, 1, &output);
        return ret == RKNN_SUCC ? -1 : ret;
    }
    perf->rknn_official_ms = official_perf.run_duration / 1000.0;

    stage_start = Clock::now();
    ret = ctc_decode(static_cast<const float*>(output.buf), app_ctx, result);
    stage_end = Clock::now();
    perf->postprocess_ms = elapsed_ms(stage_start, stage_end);

    const int release_ret =
        rknn_outputs_release(app_ctx->rknn_ctx, 1, &output);
    if (release_ret != RKNN_SUCC) {
        std::printf("rknn_outputs_release failed: ret=%d\n", release_ret);
        if (ret == 0) {
            ret = release_ret;
        }
    }
    perf->total_ms = elapsed_ms(total_start, Clock::now()) - perf_query_ms;
    return ret;
}
