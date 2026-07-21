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

#include "image_utils.h"
#include "traffic_scene.h"
#include "yolov8.h"

static bool has_image_ext(const std::string& path) {
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    return lower.size() >= 4 &&
           (lower.rfind(".jpg") == lower.size() - 4 ||
            lower.rfind(".png") == lower.size() - 4 ||
            lower.rfind(".bmp") == lower.size() - 4 ||
            (lower.size() >= 5 && lower.rfind(".jpeg") == lower.size() - 5));
}

static bool is_dir(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static std::vector<std::string> collect_images(const char* path) {
    std::vector<std::string> images;
    if (!is_dir(path)) {
        images.push_back(path);
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

static std::string basename_no_ext(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) {
        name = name.substr(0, dot);
    }
    if (name.empty()) {
        name = "image";
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

static void write_detection_csv_row(FILE* fp,
                                    size_t image_index,
                                    const std::string& image_path,
                                    const object_detect_result& det) {
    fprintf(fp, "%zu,%s,%d,%s,%.6f,%d,%d,%d,%d\n",
            image_index,
            image_path.c_str(),
            det.cls_id,
            traffic_class_name(det.cls_id),
            det.prop,
            det.box.left,
            det.box.top,
            det.box.right,
            det.box.bottom);
}

static double now_ms() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration<double, std::milli>(now).count();
}

static void print_usage(const char* prog) {
    printf("Usage: %s <coco_yolo_rknn> <image_or_folder> [repeat] [warmup] [output_dir]\n", prog);
    printf("Example: %s ./model/yolov8n_coco.rknn ./test_images 20 3 ./outputs\n", prog);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        print_usage(argv[0]);
        return -1;
    }

    const char* model_path = argv[1];
    const char* image_path = argv[2];
    int repeat = argc >= 4 ? atoi(argv[3]) : 20;
    int warmup = argc >= 5 ? atoi(argv[4]) : 3;
    const char* output_dir = argc >= 6 ? argv[5] : nullptr;
    if (repeat <= 0) repeat = 1;
    if (warmup < 0) warmup = 0;

    std::vector<std::string> images = collect_images(image_path);
    if (images.empty()) {
        printf("No input images found: %s\n", image_path);
        return -1;
    }

    printf("Traffic benchmark\n");
    printf("model: %s\n", model_path);
    printf("images: %zu\n", images.size());
    printf("repeat: %d, warmup: %d\n", repeat, warmup);
    if (output_dir != nullptr) {
        if (ensure_dir(output_dir) != 0) {
            printf("create output dir failed: %s\n", output_dir);
            return -1;
        }
        printf("output: %s\n", output_dir);
    }

    rknn_app_context_t yolo_ctx;
    memset(&yolo_ctx, 0, sizeof(yolo_ctx));

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

    double total_read_ms = 0.0;
    double total_infer_ms = 0.0;
    double total_rule_ms = 0.0;
    int measured_frames = 0;
    int total_detections = 0;
    int total_person_detections = 0;
    int total_light_detections = 0;
    double sum_detection_conf = 0.0;
    double sum_person_conf = 0.0;
    double sum_light_conf = 0.0;
    FILE* csv_fp = nullptr;
    std::string csv_path;
    if (output_dir != nullptr) {
        csv_path = join_path(output_dir, "detections.csv");
        csv_fp = fopen(csv_path.c_str(), "w");
        if (csv_fp == nullptr) {
            printf("open detection csv failed: %s\n", csv_path.c_str());
            release_yolov8_model(&yolo_ctx);
            deinit_post_process();
            return -1;
        }
        fprintf(csv_fp, "image_index,image_path,cls_id,class_name,score,left,top,right,bottom\n");
    }

    for (size_t img_idx = 0; img_idx < images.size(); ++img_idx) {
        image_buffer_t src_image;
        memset(&src_image, 0, sizeof(src_image));

        double read0 = now_ms();
        ret = read_image(images[img_idx].c_str(), &src_image);
        double read1 = now_ms();
        if (ret != 0) {
            printf("skip unreadable image: %s\n", images[img_idx].c_str());
            continue;
        }
        total_read_ms += (read1 - read0);

        object_detect_result_list raw_results;
        memset(&raw_results, 0, sizeof(raw_results));
        object_detect_result_list traffic_results;
        memset(&traffic_results, 0, sizeof(traffic_results));
        TrafficSceneResult traffic_result;
        reset_traffic_counts(&traffic_result.counts);
        traffic_result.warnings.clear();

        for (int i = 0; i < warmup; ++i) {
            if (yolo_ctx.is_quant) {
                inference_yolov8_model(&yolo_ctx, &src_image, &raw_results);
            } else {
                inference_yolov8_model_fp_fallback(&yolo_ctx, &src_image, &raw_results);
            }
        }

        for (int i = 0; i < repeat; ++i) {
            double infer0 = now_ms();
            if (yolo_ctx.is_quant) {
                ret = inference_yolov8_model(&yolo_ctx, &src_image, &raw_results);
            } else {
                ret = inference_yolov8_model_fp_fallback(&yolo_ctx, &src_image, &raw_results);
            }
            double infer1 = now_ms();
            if (ret != 0) {
                printf("inference failed on %s: %d\n", images[img_idx].c_str(), ret);
                continue;
            }

            filter_traffic_detections(&raw_results, &traffic_results);

            double rule0 = now_ms();
            analyze_traffic_scene(&traffic_results, src_image.width, src_image.height, &traffic_result);
            double rule1 = now_ms();

            total_infer_ms += (infer1 - infer0);
            total_rule_ms += (rule1 - rule0);
            measured_frames++;
        }

        printf("\n[%zu/%zu] %s\n", img_idx + 1, images.size(), images[img_idx].c_str());
        printf("raw detections: %d, traffic detections: %d\n", raw_results.count, traffic_results.count);
        printf("counts: person=%d bicycle=%d car=%d motorcycle=%d bus=%d truck=%d traffic_light=%d stop_sign=%d\n",
               traffic_result.counts.person,
               traffic_result.counts.bicycle,
               traffic_result.counts.car,
               traffic_result.counts.motorcycle,
               traffic_result.counts.bus,
               traffic_result.counts.truck,
               traffic_result.counts.traffic_light,
               traffic_result.counts.stop_sign);
        for (size_t i = 0; i < traffic_result.warnings.size(); ++i) {
            printf("warning: %s\n", traffic_result.warnings[i].message);
        }

        for (int i = 0; i < traffic_results.count; ++i) {
            const object_detect_result& det = traffic_results.results[i];
            total_detections++;
            sum_detection_conf += det.prop;
            if (det.cls_id == 0) {
                total_person_detections++;
                sum_person_conf += det.prop;
            } else if (det.cls_id == 9) {
                total_light_detections++;
                sum_light_conf += det.prop;
            }
        }

        if (csv_fp != nullptr) {
            for (int i = 0; i < traffic_results.count; ++i) {
                write_detection_csv_row(csv_fp, img_idx + 1, images[img_idx], traffic_results.results[i]);
            }
            fflush(csv_fp);
        }

        if (output_dir != nullptr) {
            draw_traffic_scene_overlay(&src_image, &traffic_results, &traffic_result);
            std::string out_name = std::string("result_") + basename_no_ext(images[img_idx]) + ".jpg";
            std::string out_path = join_path(output_dir, out_name);
            ret = write_image(out_path.c_str(), &src_image);
            if (ret == 0) {
                printf("saved result image: %s\n", out_path.c_str());
            } else {
                printf("save result image failed: %s ret=%d\n", out_path.c_str(), ret);
            }
        }

        if (src_image.virt_addr != nullptr) {
            free(src_image.virt_addr);
            src_image.virt_addr = nullptr;
        }
    }

    double avg_infer = measured_frames > 0 ? total_infer_ms / measured_frames : 0.0;
    double infer_fps = avg_infer > 0.0 ? 1000.0 / avg_infer : 0.0;
    double avg_rule = measured_frames > 0 ? total_rule_ms / measured_frames : 0.0;
    double avg_total = measured_frames > 0 ? (total_infer_ms + total_rule_ms) / measured_frames : 0.0;
    double total_fps = avg_total > 0.0 ? 1000.0 / avg_total : 0.0;
    double avg_conf_all = total_detections > 0 ? sum_detection_conf / total_detections : 0.0;
    double avg_conf_person = total_person_detections > 0 ? sum_person_conf / total_person_detections : 0.0;
    double avg_conf_light = total_light_detections > 0 ? sum_light_conf / total_light_detections : 0.0;

    if (measured_frames > 0) {
        printf("\nBenchmark summary\n");
        printf("measured frames: %d\n", measured_frames);
        printf("avg inference: %.3f ms, %.2f FPS\n", avg_infer, infer_fps);
        printf("avg risk/rules: %.3f ms\n", avg_rule);
        printf("avg infer+rules: %.3f ms, %.2f FPS\n", avg_total, total_fps);
        printf("image read total: %.3f ms\n", total_read_ms);
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

    if (output_dir != nullptr) {
        std::string summary_path = join_path(output_dir, "summary.txt");
        FILE* summary_fp = fopen(summary_path.c_str(), "w");
        if (summary_fp != nullptr) {
            fprintf(summary_fp, "Traffic benchmark summary\n");
            fprintf(summary_fp, "model=%s\n", model_path);
            fprintf(summary_fp, "input=%s\n", image_path);
            fprintf(summary_fp, "output=%s\n", output_dir);
            fprintf(summary_fp, "repeat=%d\n", repeat);
            fprintf(summary_fp, "warmup=%d\n", warmup);
            fprintf(summary_fp, "measured_frames=%d\n", measured_frames);
            fprintf(summary_fp, "avg_inference_ms=%.3f\n", avg_infer);
            fprintf(summary_fp, "inference_fps=%.2f\n", infer_fps);
            fprintf(summary_fp, "avg_rule_ms=%.3f\n", avg_rule);
            fprintf(summary_fp, "avg_infer_rules_ms=%.3f\n", avg_total);
            fprintf(summary_fp, "infer_rules_fps=%.2f\n", total_fps);
            fprintf(summary_fp, "image_read_total_ms=%.3f\n", total_read_ms);
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
    }

    if (csv_fp != nullptr) {
        fclose(csv_fp);
        csv_fp = nullptr;
    }

    release_yolov8_model(&yolo_ctx);
    deinit_post_process();
    return 0;
}
