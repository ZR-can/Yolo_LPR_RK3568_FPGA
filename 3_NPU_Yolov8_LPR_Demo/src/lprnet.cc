#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "lprnet.h"
#include "common.h"
#include "file_utils.h"
#include "image_utils.h"
#include "opencv2/opencv.hpp"

static void dump_tensor_attr(rknn_tensor_attr *attr)
{
    std::string shape_str = attr->n_dims < 1 ? "" : std::to_string(attr->dims[0]);
    for (int i = 1; i < attr->n_dims; ++i)
    {
        shape_str += ", " + std::to_string(attr->dims[i]);
    }

    printf("  index=%d, name=%s, n_dims=%d, dims=[%s], n_elems=%d, size=%d, fmt=%s, type=%s, qnt_type=%s, "
           "zp=%d, scale=%f\n",
           attr->index, attr->name, attr->n_dims, shape_str.c_str(), attr->n_elems, attr->size, get_format_string(attr->fmt),
           get_type_string(attr->type), get_qnt_type_string(attr->qnt_type), attr->zp, attr->scale);
}

// 使用引用传递，并传入裁剪坐标
void image_preprocess(const image_buffer_t& src_img, image_buffer_t& dst_img, int x1, int y1, int x2, int y2)
{
    // 1. 将原图映射为 Mat (假设原图是 RGB)
    cv::Mat full_img(src_img.height, src_img.width, CV_8UC3, src_img.virt_addr);

    // 2. 裁剪 ROI
    cv::Rect roi(x1, y1, x2 - x1, y2 - y1);
    cv::Mat crop_mat = full_img(roi);

    // 3. 缩放
    cv::Mat resized_mat;
    cv::resize(crop_mat, resized_mat, cv::Size(dst_img.width, dst_img.height));

    // 4. 颜色转换 (RGB -> BGR)
    cv::Mat bgr_mat;
    cv::cvtColor(resized_mat, bgr_mat, cv::COLOR_RGB2BGR);

    // 5. 深拷贝数据到 main 函数分配的内存中
    // 这样即便函数结束，dst_img.virt_addr 里的数据依然有效
    memcpy(dst_img.virt_addr, bgr_mat.data, dst_img.size);
}

int init_lprnet_model(const char *model_path, lprnet_app_context_t *app_ctx)
{
    int ret;
    int model_len = 0;
    char *model;
    rknn_context ctx = 0;

    // Load RKNN Model
    model_len = read_data_from_file(model_path, &model);
    if (model == NULL)
    {
        printf("load_model fail!\n");
        return -1;
    }

    ret = rknn_init(&ctx, model, model_len, 0, NULL);
    free(model);
    if (ret < 0)
    {
        printf("rknn_init fail! ret=%d\n", ret);
        return -1;
    }

    // Get Model Input Output Number
    rknn_input_output_num io_num;
    ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret != RKNN_SUCC)
    {
        printf("rknn_query fail! ret=%d\n", ret);
        return -1;
    }
    printf("model input num: %d, output num: %d\n", io_num.n_input, io_num.n_output);

    // Get Model Input Info
    printf("input tensors:\n");
    rknn_tensor_attr input_attrs[io_num.n_input];
    memset(input_attrs, 0, sizeof(input_attrs));
    for (int i = 0; i < io_num.n_input; i++)
    {
        input_attrs[i].index = i;
        ret = rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &(input_attrs[i]), sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC)
        {
            printf("rknn_query fail! ret=%d\n", ret);
            return -1;
        }
        dump_tensor_attr(&(input_attrs[i]));
    }

    // Get Model Output Info
    printf("output tensors:\n");
    rknn_tensor_attr output_attrs[io_num.n_output];
    memset(output_attrs, 0, sizeof(output_attrs));
    for (int i = 0; i < io_num.n_output; i++)
    {
        output_attrs[i].index = i;
        ret = rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &(output_attrs[i]), sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC)
        {
            printf("rknn_query fail! ret=%d\n", ret);
            return -1;
        }
        dump_tensor_attr(&(output_attrs[i]));
    }

    // Set to context
    app_ctx->rknn_ctx = ctx;
    app_ctx->io_num = io_num;
    app_ctx->input_attrs = (rknn_tensor_attr *)malloc(io_num.n_input * sizeof(rknn_tensor_attr));
    memcpy(app_ctx->input_attrs, input_attrs, io_num.n_input * sizeof(rknn_tensor_attr));
    app_ctx->output_attrs = (rknn_tensor_attr *)malloc(io_num.n_output * sizeof(rknn_tensor_attr));
    memcpy(app_ctx->output_attrs, output_attrs, io_num.n_output * sizeof(rknn_tensor_attr));

    if (input_attrs[0].fmt == RKNN_TENSOR_NCHW)
    {
        printf("model is NCHW input fmt\n");
        app_ctx->model_channel = input_attrs[0].dims[1];
        app_ctx->model_height = input_attrs[0].dims[2];
        app_ctx->model_width = input_attrs[0].dims[3];
    }
    else
    {
        printf("model is NHWC input fmt\n");
        app_ctx->model_height = input_attrs[0].dims[1];
        app_ctx->model_width = input_attrs[0].dims[2];
        app_ctx->model_channel = input_attrs[0].dims[3];
    }
    printf("model input height=%d, width=%d, channel=%d\n",
           app_ctx->model_height, app_ctx->model_width, app_ctx->model_channel);

    return 0;
}

int release_lprnet_model(lprnet_app_context_t *app_ctx)
{
    if (app_ctx->input_attrs != NULL)
    {
        free(app_ctx->input_attrs);
        app_ctx->input_attrs = NULL;
    }
    if (app_ctx->output_attrs != NULL)
    {
        free(app_ctx->output_attrs);
        app_ctx->output_attrs = NULL;
    }
    if (app_ctx->rknn_ctx != 0)
    {
        rknn_destroy(app_ctx->rknn_ctx);
        app_ctx->rknn_ctx = 0;
    }
    return 0;
}

int inference_lprnet_model(lprnet_app_context_t *app_ctx, image_buffer_t *src_img, lprnet_result *out_result)
{
    int ret;
    rknn_input inputs[1];
    rknn_output outputs[1];

    memset(inputs, 0, sizeof(inputs));
    memset(outputs, 0, sizeof(outputs));

    // Set Input Data
    inputs[0].index = 0;
    inputs[0].type = RKNN_TENSOR_UINT8;
    inputs[0].fmt = RKNN_TENSOR_NHWC;
    inputs[0].size = app_ctx->model_width * app_ctx->model_height * app_ctx->model_channel;
    inputs[0].buf = (uint8_t *)src_img->virt_addr;

    ret = rknn_inputs_set(app_ctx->rknn_ctx, 1, inputs);
    if (ret < 0)
    {
        printf("rknn_input_set fail! ret=%d\n", ret);
        return -1;
    }

    // Run
    printf("rknn_run\n");
    ret = rknn_run(app_ctx->rknn_ctx, nullptr);
    if (ret < 0)
    {
        printf("rknn_run fail! ret=%d\n", ret);
        return -1;
    }

    // Get Output
    outputs[0].want_float = 1;
    ret = rknn_outputs_get(app_ctx->rknn_ctx, 1, outputs, NULL);
    if (ret < 0)
    {
        printf("rknn_outputs_get fail! ret=%d\n", ret);
        return ret;
    }

    // Post Process
    std::vector<int> no_repeat_blank_label{};
    std::vector<float> no_repeat_blank_prob{}; // 用于存每个字符的真实概率
    float prebs[OUT_COLS];
    float max_probs[OUT_COLS]; // 用于存每个列的最大概率
    int pre_c;
    for (int x = 0; x < OUT_COLS; x++) // Traverse OUT_COLS license plate positions
    {
        float *ptr = (float *)outputs[0].buf;
        float preb[OUT_ROWS];
        for (int y = 0; y < OUT_ROWS; y++) // Traverse OUT_ROWS string positions
        {
            preb[y] = ptr[x];
            ptr += OUT_COLS;
        }
        int max_num_index = std::max_element(preb, preb + OUT_ROWS) - preb;
        prebs[x] = max_num_index;
        // 计算稳定的 Softmax 概率
        float max_val = preb[max_num_index];
        float sum_exp = 0.0f;
        for (int y = 0; y < OUT_ROWS; y++) {
            sum_exp += exp(preb[y] - max_val); // 减去 max_val 防止 exp 溢出
        }
        max_probs[x] = 1.0f / sum_exp;
    }

    // Remove duplicates and blanks
    pre_c = prebs[0];
    if (pre_c != OUT_ROWS - 1)
    {
        no_repeat_blank_label.push_back(pre_c);
        no_repeat_blank_prob.push_back(max_probs[0]);
    }
    
    for (int i = 0; i < OUT_COLS; ++i)
    {
        int value = prebs[i];
        if (value == OUT_ROWS - 1 || value == pre_c)
        {
            if (value == OUT_ROWS - 1)
            {
                // 遇到空白符时，更新pre_c阻断连续状态
                pre_c = value; 
            }
            continue;
        }
        no_repeat_blank_label.push_back(value);
        no_repeat_blank_prob.push_back(max_probs[i]);
        pre_c = value;
    }

    // The license plate is converted into a string according to the dictionary
    out_result->plate_name.clear();
    float total_prob = 0.0f;
    for (int hh : no_repeat_blank_label)
    {
        out_result->plate_name += plate_code[hh];
        total_prob += no_repeat_blank_prob[hh];
    }

    // 计算该车牌的平均字符置信度
    if (!no_repeat_blank_label.empty()) {
        out_result->text_confidence = total_prob / no_repeat_blank_label.size();
    } else {
        out_result->text_confidence = 0.0f;
    }

    // Remeber to release rknn output
    rknn_outputs_release(app_ctx->rknn_ctx, 1, outputs);

    return ret;
}

// 拆分 UTF-8 字符串，确保中文字符和 ASCII 字符被正确分离
static std::vector<std::string> parse_utf8(const std::string& str) {
    std::vector<std::string> chars;
    for (size_t i = 0; i < str.length();) {
        int cplen = 1;
        if ((str[i] & 0xF8) == 0xF0) cplen = 4;
        else if ((str[i] & 0xF0) == 0xE0) cplen = 3;
        else if ((str[i] & 0xE0) == 0xC0) cplen = 2;
        chars.push_back(str.substr(i, cplen));
        i += cplen;
    }
    return chars;
}

// 基于 GA 36-2018 及自定义规则的车牌后处理修正函数
void correct_plate_string(std::string& plate, int cls_id) {
    std::vector<std::string> chars = parse_utf8(plate);
    if (chars.size() < 2) return;

    std::string province = chars[0];

    // 规则 1：处理 O 和 I 的混淆
    // 除了 京、津、渝、沪 的第二位允许出现 O/I，其余所有位置的 O 替换为 0，I 替换为 1
    for (size_t i = 1; i < chars.size(); ++i) {
        bool is_allowed_oi = (i == 1 && (province == "京" || province == "津" || province == "渝" || province == "沪"));
        if (!is_allowed_oi) {
            if (chars[i] == "O") chars[i] = "0";
            if (chars[i] == "I") chars[i] = "1";
        }
    }

    // 规则 2：新能源车牌（绿牌 cls_id == 1）的特殊校验
    if (cls_id == 1 && chars.size() >= 3) {
        // 小型新能源车牌的第 3 位通常是 D（纯电）或 F（混动）等字母。若被误识别为数字 0，则强转为 D
        if (chars[2] == "0") {
            chars[2] = "D";
        }
        // 大型新能源车牌的字母位于最后一位，若末位误识别为 0，通常也修正为 D
        if (chars.back() == "0") {
            chars.back() = "D";
        }
    }

    // 规则 3：长度截断与特殊尾标处理
    std::string special_tail = "";
    std::string last_c = chars.back();
    // 检查是否存在特殊尾缀
    if (last_c == "警" || last_c == "学" || last_c == "港" || last_c == "澳" || last_c == "领" || last_c == "使") {
        special_tail = last_c;
        chars.pop_back();
    }

    // 根据车辆类型设定标准长度（绿牌通常为 8 位，其他为 7 位）
    size_t target_len = (cls_id == 1) ? 8 : 7;
    target_len -= (special_tail.empty() ? 0 : 1);

    // 如果模型输出了多余的冗余字符，从尾部强制截断
    while (chars.size() > target_len) {
        chars.pop_back();
    }

    // 重组字符串
    plate = "";
    for (const auto& c : chars) {
        plate += c;
    }
    plate += special_tail;
}