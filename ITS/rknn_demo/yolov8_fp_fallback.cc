#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "image_utils.h"
#include "yolov8.h"

int inference_yolov8_model_fp_fallback(rknn_app_context_t* app_ctx,
                                       image_buffer_t* img,
                                       object_detect_result_list* od_results) {
    const float nms_threshold = NMS_THRESH;
    const float box_conf_threshold = BOX_THRESH;
    const int bg_color = 0;

    if (app_ctx == nullptr || img == nullptr || od_results == nullptr) {
        return -1;
    }

    if (app_ctx->is_quant) {
        return inference_yolov8_model(app_ctx, img, od_results);
    }

    memset(od_results, 0, sizeof(*od_results));

    image_buffer_t dst_img;
    letterbox_t letter_box;
    memset(&dst_img, 0, sizeof(dst_img));
    memset(&letter_box, 0, sizeof(letter_box));

    dst_img.width = app_ctx->model_width;
    dst_img.height = app_ctx->model_height;
    dst_img.format = IMAGE_FORMAT_RGB888;
    dst_img.size = get_image_size(&dst_img);
    dst_img.fd = app_ctx->input_mems[0]->fd;
    dst_img.virt_addr = static_cast<unsigned char*>(app_ctx->input_mems[0]->virt_addr);

    if (dst_img.virt_addr == nullptr && dst_img.fd == 0) {
        printf("fp fallback input buffer is invalid\n");
        return -1;
    }

    int ret = convert_image_with_letterbox(img, &dst_img, &letter_box, bg_color);
    if (ret < 0) {
        printf("fp fallback convert_image_with_letterbox fail! ret=%d\n", ret);
        return ret;
    }

    ret = rknn_run(app_ctx->rknn_ctx, nullptr);
    if (ret < 0) {
        printf("fp fallback rknn_run fail! ret=%d\n", ret);
        return ret;
    }

    rknn_output outputs[app_ctx->io_num.n_output];
    memset(outputs, 0, sizeof(outputs));
    for (uint32_t i = 0; i < app_ctx->io_num.n_output; ++i) {
        outputs[i].index = i;
        outputs[i].want_float = 1;
    }

    ret = rknn_outputs_get(app_ctx->rknn_ctx, app_ctx->io_num.n_output, outputs, nullptr);
    if (ret < 0) {
        printf("fp fallback rknn_outputs_get fail! ret=%d\n", ret);
        return ret;
    }

    ret = post_process(app_ctx, outputs, &letter_box, box_conf_threshold, nms_threshold, od_results);

    int release_ret = rknn_outputs_release(app_ctx->rknn_ctx, app_ctx->io_num.n_output, outputs);
    if (release_ret < 0) {
        printf("fp fallback rknn_outputs_release fail! ret=%d\n", release_ret);
        return release_ret;
    }

    return ret;
}
