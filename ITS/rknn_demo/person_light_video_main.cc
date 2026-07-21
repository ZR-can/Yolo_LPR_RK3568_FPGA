#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <string>
#include <vector>

#include "image_drawing.h"
#include "image_utils.h"
#include "yolov8.h"

static double now_ms() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration<double, std::milli>(now).count();
}

static int clamp_int(int value, int low, int high) {
    return std::max(low, std::min(value, high));
}

static bool is_dir(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool has_image_ext(const std::string& path) {
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return lower.size() >= 4 &&
           (lower.rfind(".jpg") == lower.size() - 4 ||
            lower.rfind(".png") == lower.size() - 4 ||
            lower.rfind(".bmp") == lower.size() - 4 ||
            (lower.size() >= 5 && lower.rfind(".jpeg") == lower.size() - 5));
}

static std::vector<std::string> collect_images(const char* path) {
    std::vector<std::string> images;
    if (!is_dir(path)) {
        if (has_image_ext(path)) {
            images.push_back(path);
        }
        return images;
    }

    DIR* dir = opendir(path);
    if (dir == nullptr) {
        return images;
    }

    struct dirent* ent = nullptr;
    while ((ent = readdir(dir)) != nullptr) {
        std::string name = ent->d_name;
        if (name == "." || name == ".." || !has_image_ext(name)) {
            continue;
        }
        images.push_back(std::string(path) + "/" + name);
    }
    closedir(dir);
    std::sort(images.begin(), images.end());
    return images;
}

static int ensure_dir(const char* path) {
    if (path == nullptr || path[0] == '\0') {
        return -1;
    }
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 0 : -1;
    }
    if (mkdir(path, 0755) == 0 || errno == EEXIST) {
        return 0;
    }
    return -1;
}

static std::string join_path(const std::string& dir, const std::string& name) {
    if (dir.empty()) {
        return name;
    }
    char last = dir[dir.size() - 1];
    if (last == '/' || last == '\\') {
        return dir + name;
    }
    return dir + "/" + name;
}

static std::string basename_no_ext(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) {
        name = name.substr(0, dot);
    }
    if (name.empty()) {
        name = "frame";
    }
    for (size_t i = 0; i < name.size(); ++i) {
        char c = name[i];
        bool ok = (c >= '0' && c <= '9') ||
                  (c >= 'A' && c <= 'Z') ||
                  (c >= 'a' && c <= 'z') ||
                  c == '_' || c == '-';
        if (!ok) {
            name[i] = '_';
        }
    }
    return name;
}

static bool is_person_or_light(int cls_id) {
    return cls_id == 0 || cls_id == 9;
}

static const char* person_light_class_name(int cls_id) {
    switch (cls_id) {
        case 0: return "person";
        case 9: return "traffic light";
        default: return "other";
    }
}

static unsigned int person_light_color(int cls_id) {
    switch (cls_id) {
        case 0: return COLOR_YELLOW;
        case 9: return COLOR_RED;
        default: return COLOR_WHITE;
    }
}

static void filter_person_light(const object_detect_result_list* src,
                                object_detect_result_list* dst) {
    std::memset(dst, 0, sizeof(*dst));
    if (src == nullptr) {
        return;
    }
    dst->id = src->id;
    for (int i = 0; i < src->count && dst->count < OBJ_NUMB_MAX_SIZE; ++i) {
        const object_detect_result& det = src->results[i];
        if (is_person_or_light(det.cls_id)) {
            dst->results[dst->count++] = det;
        }
    }
}

static void draw_person_light_overlay(image_buffer_t* image,
                                      const object_detect_result_list* detections,
                                      int frame_index,
                                      double infer_ms) {
    if (image == nullptr || detections == nullptr) {
        return;
    }

    int person_count = 0;
    int light_count = 0;

    for (int i = 0; i < detections->count; ++i) {
        const object_detect_result& det = detections->results[i];
        if (!is_person_or_light(det.cls_id)) {
            continue;
        }
        if (det.cls_id == 0) {
            person_count++;
        } else if (det.cls_id == 9) {
            light_count++;
        }

        int left = clamp_int(det.box.left, 0, image->width - 1);
        int top = clamp_int(det.box.top, 0, image->height - 1);
        int right = clamp_int(det.box.right, 0, image->width - 1);
        int bottom = clamp_int(det.box.bottom, 0, image->height - 1);
        if (right <= left || bottom <= top) {
            continue;
        }

        unsigned int color = person_light_color(det.cls_id);
        draw_rectangle_alpha(image, left, top, right - left, bottom - top, color, 3, 255);

        char label[96];
        snprintf(label, sizeof(label), "%s %.2f", person_light_class_name(det.cls_id), det.prop);
        draw_text(image, label, left, std::max(0, top - 22), color, 18);
    }

    draw_rectangle_alpha(image, 8, 8, 520, 98, COLOR_DARK_GRAY, -1, 180);
    char line[160];
    snprintf(line, sizeof(line), "Frame:%d  person:%d  traffic_light:%d", frame_index, person_count, light_count);
    draw_text(image, "YOLOv8 COCO: person + traffic light only", 18, 18, COLOR_YELLOW, 18);
    draw_text(image, line, 18, 44, COLOR_WHITE, 18);
    snprintf(line, sizeof(line), "Infer: %.2f ms  %.2f FPS", infer_ms, infer_ms > 0.0 ? 1000.0 / infer_ms : 0.0);
    draw_text(image, line, 18, 70, COLOR_WHITE, 18);
}

static void write_csv_rows(FILE* fp,
                           int frame_index,
                           const std::string& image_path,
                           const object_detect_result_list* detections,
                           double infer_ms) {
    if (fp == nullptr || detections == nullptr) {
        return;
    }
    for (int i = 0; i < detections->count; ++i) {
        const object_detect_result& det = detections->results[i];
        fprintf(fp, "%d,%s,%d,%s,%.6f,%d,%d,%d,%d,%.3f\n",
                frame_index,
                image_path.c_str(),
                det.cls_id,
                person_light_class_name(det.cls_id),
                det.prop,
                det.box.left,
                det.box.top,
                det.box.right,
                det.box.bottom,
                infer_ms);
    }
}

static void print_usage(const char* prog) {
    printf("Usage: %s <coco_yolo_rknn> <image_or_folder> <output_dir> [max_frames] [frame_stride]\n", prog);
    printf("Example: %s ./model/yolov8n_coco_fp.rknn ./test_frames/test1 ./outputs_video/test1_frames 300 1\n", prog);
    printf("Only COCO classes person(id=0) and traffic light(id=9) are drawn and exported.\n");
}

int main(int argc, char** argv) {
    if (argc < 4) {
        print_usage(argv[0]);
        return -1;
    }

    const char* model_path = argv[1];
    const char* input_path = argv[2];
    const char* output_dir = argv[3];
    int max_frames = argc >= 5 ? atoi(argv[4]) : 0;
    int frame_stride = argc >= 6 ? atoi(argv[5]) : 1;
    if (frame_stride <= 0) {
        frame_stride = 1;
    }

    std::vector<std::string> images = collect_images(input_path);
    if (images.empty()) {
        printf("No input images found: %s\n", input_path);
        printf("This build does not use OpenCV videoio. Extract mp4 to jpg frames on PC first.\n");
        return -1;
    }
    if (ensure_dir(output_dir) != 0) {
        printf("create output dir failed: %s\n", output_dir);
        return -1;
    }

    printf("Person/light frame-sequence check\n");
    printf("model: %s\n", model_path);
    printf("input images: %zu\n", images.size());
    printf("output: %s\n", output_dir);
    printf("max_frames: %d, frame_stride: %d\n", max_frames, frame_stride);

    rknn_app_context_t yolo_ctx;
    std::memset(&yolo_ctx, 0, sizeof(yolo_ctx));

    int ret = init_post_process();
    if (ret != 0) {
        printf("init_post_process failed: %d\n", ret);
        return ret;
    }

    ret = init_yolov8_model(model_path, &yolo_ctx);
    if (ret != 0) {
        printf("init_yolov8_model failed: %d\n", ret);
        deinit_post_process();
        return ret;
    }

    std::string csv_path = join_path(output_dir, "person_light.csv");
    FILE* csv_fp = fopen(csv_path.c_str(), "w");
    if (csv_fp == nullptr) {
        printf("open csv failed: %s\n", csv_path.c_str());
        release_yolov8_model(&yolo_ctx);
        deinit_post_process();
        return -1;
    }
    fprintf(csv_fp, "frame_index,image_path,cls_id,class_name,score,left,top,right,bottom,infer_ms\n");

    int seen_frames = 0;
    int processed_frames = 0;
    int written_frames = 0;
    int total_detections = 0;
    int total_person_detections = 0;
    int total_light_detections = 0;
    double sum_detection_conf = 0.0;
    double sum_person_conf = 0.0;
    double sum_light_conf = 0.0;
    double total_infer_ms = 0.0;
    double total_end_ms = 0.0;

    for (size_t i = 0; i < images.size(); ++i) {
        seen_frames++;
        if (max_frames > 0 && seen_frames > max_frames) {
            break;
        }
        if (((seen_frames - 1) % frame_stride) != 0) {
            continue;
        }

        double frame0 = now_ms();
        image_buffer_t src_image;
        std::memset(&src_image, 0, sizeof(src_image));
        ret = read_image(images[i].c_str(), &src_image);
        if (ret != 0) {
            printf("skip unreadable image: %s\n", images[i].c_str());
            continue;
        }

        object_detect_result_list raw_results;
        object_detect_result_list filtered_results;
        std::memset(&raw_results, 0, sizeof(raw_results));
        std::memset(&filtered_results, 0, sizeof(filtered_results));

        double infer0 = now_ms();
        if (yolo_ctx.is_quant) {
            ret = inference_yolov8_model(&yolo_ctx, &src_image, &raw_results);
        } else {
            ret = inference_yolov8_model_fp_fallback(&yolo_ctx, &src_image, &raw_results);
        }
        double infer1 = now_ms();
        if (ret != 0) {
            printf("inference failed on %s: %d\n", images[i].c_str(), ret);
            if (src_image.virt_addr != nullptr) {
                free(src_image.virt_addr);
            }
            break;
        }

        filter_person_light(&raw_results, &filtered_results);
        double infer_ms = infer1 - infer0;
        total_infer_ms += infer_ms;
        processed_frames++;

        int persons = 0;
        int lights = 0;
        for (int k = 0; k < filtered_results.count; ++k) {
            const object_detect_result& det = filtered_results.results[k];
            total_detections++;
            sum_detection_conf += det.prop;
            if (det.cls_id == 0) {
                persons++;
                total_person_detections++;
                sum_person_conf += det.prop;
            }
            if (det.cls_id == 9) {
                lights++;
                total_light_detections++;
                sum_light_conf += det.prop;
            }
        }
        printf("frame %d: raw=%d person_light=%d person=%d traffic_light=%d infer=%.2f ms\n",
               seen_frames,
               raw_results.count,
               filtered_results.count,
               persons,
               lights,
               infer_ms);

        write_csv_rows(csv_fp, seen_frames, images[i], &filtered_results, infer_ms);
        fflush(csv_fp);

        draw_person_light_overlay(&src_image, &filtered_results, seen_frames, infer_ms);
        char out_name[128];
        snprintf(out_name, sizeof(out_name), "result_%06d.jpg", written_frames + 1);
        std::string out_path = join_path(output_dir, out_name);
        ret = write_image(out_path.c_str(), &src_image);
        if (ret == 0) {
            written_frames++;
        } else {
            printf("save result image failed: %s ret=%d\n", out_path.c_str(), ret);
        }

        total_end_ms += now_ms() - frame0;
        if (src_image.virt_addr != nullptr) {
            free(src_image.virt_addr);
            src_image.virt_addr = nullptr;
        }
    }

    double avg_infer = processed_frames > 0 ? total_infer_ms / processed_frames : 0.0;
    double infer_fps = avg_infer > 0.0 ? 1000.0 / avg_infer : 0.0;
    double avg_end = processed_frames > 0 ? total_end_ms / processed_frames : 0.0;
    double end_fps = avg_end > 0.0 ? 1000.0 / avg_end : 0.0;
    double avg_conf_all = total_detections > 0 ? sum_detection_conf / total_detections : 0.0;
    double avg_conf_person = total_person_detections > 0 ? sum_person_conf / total_person_detections : 0.0;
    double avg_conf_light = total_light_detections > 0 ? sum_light_conf / total_light_detections : 0.0;

    printf("\nFrame-sequence summary\n");
    printf("input frames seen: %d\n", seen_frames);
    printf("frames with inference: %d\n", processed_frames);
    printf("output images written: %d\n", written_frames);
    if (processed_frames > 0) {
        printf("avg inference: %.3f ms, %.2f FPS\n", avg_infer, infer_fps);
        printf("avg read+infer+draw+write: %.3f ms, %.2f FPS\n", avg_end, end_fps);
    }
    printf("detections: total=%d person=%d traffic_light=%d\n",
           total_detections, total_person_detections, total_light_detections);
    if (total_detections > 0) {
        printf("avg confidence: all=%.4f\n", avg_conf_all);
    }
    if (total_person_detections > 0) {
        printf("avg confidence person=%.4f\n", avg_conf_person);
    }
    if (total_light_detections > 0) {
        printf("avg confidence traffic_light=%.4f\n", avg_conf_light);
    }

    std::string summary_path = join_path(output_dir, "summary.txt");
    FILE* summary_fp = fopen(summary_path.c_str(), "w");
    if (summary_fp != nullptr) {
        fprintf(summary_fp, "Person/light frame-sequence summary\n");
        fprintf(summary_fp, "model=%s\n", model_path);
        fprintf(summary_fp, "input=%s\n", input_path);
        fprintf(summary_fp, "output=%s\n", output_dir);
        fprintf(summary_fp, "max_frames=%d\n", max_frames);
        fprintf(summary_fp, "frame_stride=%d\n", frame_stride);
        fprintf(summary_fp, "input_frames_seen=%d\n", seen_frames);
        fprintf(summary_fp, "frames_with_inference=%d\n", processed_frames);
        fprintf(summary_fp, "output_images_written=%d\n", written_frames);
        fprintf(summary_fp, "avg_inference_ms=%.3f\n", avg_infer);
        fprintf(summary_fp, "inference_fps=%.2f\n", infer_fps);
        fprintf(summary_fp, "avg_read_infer_draw_write_ms=%.3f\n", avg_end);
        fprintf(summary_fp, "end_to_end_fps=%.2f\n", end_fps);
        fprintf(summary_fp, "detections_total=%d\n", total_detections);
        fprintf(summary_fp, "detections_person=%d\n", total_person_detections);
        fprintf(summary_fp, "detections_traffic_light=%d\n", total_light_detections);
        fprintf(summary_fp, "avg_confidence_all=%.4f\n", avg_conf_all);
        fprintf(summary_fp, "avg_confidence_person=%.4f\n", avg_conf_person);
        fprintf(summary_fp, "avg_confidence_traffic_light=%.4f\n", avg_conf_light);
        fprintf(summary_fp, "csv=%s\n", csv_path.c_str());
        fclose(summary_fp);
        printf("summary: %s\n", summary_path.c_str());
    } else {
        printf("write summary failed: %s\n", summary_path.c_str());
    }
    printf("csv: %s\n", csv_path.c_str());

    fclose(csv_fp);
    release_yolov8_model(&yolo_ctx);
    deinit_post_process();
    return 0;
}
