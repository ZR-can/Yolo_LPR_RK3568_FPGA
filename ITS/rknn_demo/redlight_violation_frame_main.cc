#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <string>
#include <vector>

#include "image_drawing.h"
#include "image_utils.h"
#include "yolov8.h"

struct Point {
    int x;
    int y;
};

struct LightJudge {
    std::string state;
    image_rect_t box;
    int red_count;
    int green_count;
    int active_count;
    float red_ratio;
    float green_ratio;
    float score;
    int candidates;
};

static const float kCrosswalkPolyNorm[][2] = {
    {0.003918f, 0.606209f},
    {0.881440f, 0.606209f},
    {0.881440f, 0.856197f},
    {0.003918f, 0.856197f},
};
static const int kCrosswalkPolyCount = 4;
static const int kBottomLineSamples = 5;
static const int kBottomLineMinInside = 1;
static const int kColorMinSaturation = 70;
static const int kColorMinValue = 90;
static const int kColorMinPixels = 8;
static const float kColorDominanceRatio = 1.20f;
static const float kLightBoxExpandRatio = 0.10f;

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

static bool is_person_or_light(int cls_id) {
    return cls_id == 0 || cls_id == 9;
}

static const char* class_name(int cls_id) {
    switch (cls_id) {
        case 0: return "person";
        case 9: return "traffic light";
        default: return "other";
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

static std::vector<Point> resolve_crosswalk_polygon(int width, int height) {
    std::vector<Point> poly;
    for (int i = 0; i < kCrosswalkPolyCount; ++i) {
        int x = static_cast<int>(std::round(kCrosswalkPolyNorm[i][0] * (width - 1)));
        int y = static_cast<int>(std::round(kCrosswalkPolyNorm[i][1] * (height - 1)));
        poly.push_back({clamp_int(x, 0, width - 1), clamp_int(y, 0, height - 1)});
    }
    return poly;
}

static bool point_in_polygon(float x, float y, const std::vector<Point>& poly) {
    bool inside = false;
    int n = static_cast<int>(poly.size());
    for (int i = 0, j = n - 1; i < n; j = i++) {
        float xi = static_cast<float>(poly[i].x);
        float yi = static_cast<float>(poly[i].y);
        float xj = static_cast<float>(poly[j].x);
        float yj = static_cast<float>(poly[j].y);
        bool intersect = ((yi > y) != (yj > y)) &&
                         (x < (xj - xi) * (y - yi) / ((yj - yi) + 1e-6f) + xi);
        if (intersect) {
            inside = !inside;
        }
    }
    return inside;
}

static bool bottom_line_inside_crosswalk(const image_rect_t& box,
                                         const std::vector<Point>& poly,
                                         int* inside_samples,
                                         int* sample_count) {
    int inside = 0;
    int count = std::max(1, kBottomLineSamples);
    float y = static_cast<float>(box.bottom);
    for (int i = 0; i < count; ++i) {
        float t = count == 1 ? 0.5f : static_cast<float>(i) / static_cast<float>(count - 1);
        float x = static_cast<float>(box.left) * (1.0f - t) + static_cast<float>(box.right) * t;
        if (point_in_polygon(x, y, poly)) {
            inside++;
        }
    }
    if (inside_samples) *inside_samples = inside;
    if (sample_count) *sample_count = count;
    return inside >= kBottomLineMinInside;
}

static image_rect_t clamp_rect(image_rect_t r, int width, int height) {
    r.left = clamp_int(r.left, 0, width - 1);
    r.top = clamp_int(r.top, 0, height - 1);
    r.right = clamp_int(r.right, r.left + 1, width);
    r.bottom = clamp_int(r.bottom, r.top + 1, height);
    return r;
}

static image_rect_t expand_box(const image_rect_t& box, float ratio, int width, int height) {
    float bw = static_cast<float>(std::max(1, box.right - box.left));
    float bh = static_cast<float>(std::max(1, box.bottom - box.top));
    int pad_x = static_cast<int>(std::round(bw * ratio));
    int pad_y = static_cast<int>(std::round(bh * ratio));
    image_rect_t out;
    out.left = box.left - pad_x;
    out.top = box.top - pad_y;
    out.right = box.right + pad_x;
    out.bottom = box.bottom + pad_y;
    return clamp_rect(out, width, height);
}

static void rgb_to_hsv_int(int r, int g, int b, int* hue, int* sat, int* val) {
    int maxc = std::max(r, std::max(g, b));
    int minc = std::min(r, std::min(g, b));
    int delta = maxc - minc;
    *val = maxc;
    *sat = maxc == 0 ? 0 : (delta * 255) / maxc;
    if (delta == 0) {
        *hue = 0;
    } else if (maxc == r) {
        *hue = (60 * (g - b) / delta + 360) % 360;
    } else if (maxc == g) {
        *hue = 60 * (b - r) / delta + 120;
    } else {
        *hue = 60 * (r - g) / delta + 240;
    }
}

static LightJudge estimate_light_state(const image_buffer_t* image, const image_rect_t& box) {
    LightJudge judge;
    judge.state = "unknown";
    judge.box = box;
    judge.red_count = 0;
    judge.green_count = 0;
    judge.active_count = 0;
    judge.red_ratio = 0.0f;
    judge.green_ratio = 0.0f;
    judge.score = 0.0f;
    judge.candidates = 0;

    if (image == nullptr || image->virt_addr == nullptr || image->format != IMAGE_FORMAT_RGB888) {
        return judge;
    }

    int stride = image->width_stride > 0 ? image->width_stride : image->width;
    for (int y = box.top; y < box.bottom; ++y) {
        const unsigned char* row = image->virt_addr + y * stride * 3;
        for (int x = box.left; x < box.right; ++x) {
            const unsigned char* px = row + x * 3;
            int r = px[0];
            int g = px[1];
            int b = px[2];
            int hue = 0, sat = 0, val = 0;
            rgb_to_hsv_int(r, g, b, &hue, &sat, &val);
            if (sat < kColorMinSaturation || val < kColorMinValue) {
                continue;
            }
            judge.active_count++;
            if (hue <= 20 || hue >= 340) {
                judge.red_count++;
            } else if (hue >= 70 && hue <= 170) {
                judge.green_count++;
            }
        }
    }

    int denom = std::max(1, judge.red_count + judge.green_count);
    judge.red_ratio = static_cast<float>(judge.red_count) / static_cast<float>(denom);
    judge.green_ratio = static_cast<float>(judge.green_count) / static_cast<float>(denom);

    if (judge.red_count >= kColorMinPixels &&
        judge.red_count > static_cast<int>(judge.green_count * kColorDominanceRatio)) {
        judge.state = "red";
    } else if (judge.green_count >= kColorMinPixels &&
               judge.green_count > static_cast<int>(judge.red_count * kColorDominanceRatio)) {
        judge.state = "green";
    }
    return judge;
}

static LightJudge choose_light_by_color(const image_buffer_t* image,
                                        const object_detect_result_list* detections) {
    LightJudge best;
    best.state = "unknown";
    best.box = {-1, -1, -1, -1};
    best.red_count = 0;
    best.green_count = 0;
    best.active_count = 0;
    best.red_ratio = 0.0f;
    best.green_ratio = 0.0f;
    best.score = 0.0f;
    best.candidates = 0;
    if (image == nullptr || detections == nullptr) {
        return best;
    }

    int best_color = -1;
    for (int i = 0; i < detections->count; ++i) {
        const object_detect_result& det = detections->results[i];
        if (det.cls_id != 9) {
            continue;
        }
        best.candidates++;
        image_rect_t expanded = expand_box(det.box, kLightBoxExpandRatio, image->width, image->height);
        LightJudge cur = estimate_light_state(image, expanded);
        cur.score = det.prop;
        cur.candidates = best.candidates;
        int color_evidence = std::max(cur.red_count, cur.green_count);
        bool better = color_evidence > best_color ||
                      (color_evidence == best_color && cur.active_count > best.active_count) ||
                      (color_evidence == best_color && cur.active_count == best.active_count && cur.score > best.score);
        if (better) {
            best = cur;
            best_color = color_evidence;
        }
    }
    best.candidates = 0;
    for (int i = 0; i < detections->count; ++i) {
        if (detections->results[i].cls_id == 9) {
            best.candidates++;
        }
    }
    return best;
}

static void draw_crosswalk(image_buffer_t* image, const std::vector<Point>& poly) {
    if (image == nullptr || poly.size() < 2) {
        return;
    }
    for (size_t i = 0; i < poly.size(); ++i) {
        const Point& a = poly[i];
        const Point& b = poly[(i + 1) % poly.size()];
        draw_line(image, a.x, a.y, b.x, b.y, COLOR_BLUE, 3);
    }
    draw_text(image, "crosswalk ROI", poly[0].x + 6, std::max(20, poly[0].y - 22), COLOR_BLUE, 18);
}

static void draw_overlay(image_buffer_t* image,
                         const object_detect_result_list* detections,
                         const std::vector<int>& violator_indices,
                         const std::vector<Point>& crosswalk,
                         const LightJudge& light,
                         int frame_index,
                         int persons_in_crosswalk,
                         double infer_ms) {
    if (image == nullptr || detections == nullptr) {
        return;
    }
    draw_crosswalk(image, crosswalk);

    if (light.box.left >= 0) {
        unsigned int lcolor = light.state == "red" ? COLOR_RED :
                              light.state == "green" ? COLOR_GREEN : COLOR_YELLOW;
        draw_rectangle_alpha(image, light.box.left, light.box.top,
                             light.box.right - light.box.left,
                             light.box.bottom - light.box.top, lcolor, 2, 255);
        draw_text(image, "YOLO traffic light", light.box.left, std::max(20, light.box.top - 22), lcolor, 18);
    }

    int person_count = 0;
    int light_count = 0;
    for (int i = 0; i < detections->count; ++i) {
        const object_detect_result& det = detections->results[i];
        if (!is_person_or_light(det.cls_id)) {
            continue;
        }
        if (det.cls_id == 0) person_count++;
        if (det.cls_id == 9) light_count++;

        bool violation = std::find(violator_indices.begin(), violator_indices.end(), i) != violator_indices.end();
        unsigned int color = det.cls_id == 9 ? COLOR_GREEN : (violation ? COLOR_RED : COLOR_YELLOW);
        int left = clamp_int(det.box.left, 0, image->width - 1);
        int top = clamp_int(det.box.top, 0, image->height - 1);
        int right = clamp_int(det.box.right, 0, image->width - 1);
        int bottom = clamp_int(det.box.bottom, 0, image->height - 1);
        if (right <= left || bottom <= top) {
            continue;
        }
        draw_rectangle_alpha(image, left, top, right - left, bottom - top, color, 3, 255);
        draw_line(image, left, bottom, right, bottom, color, 3);
        char label[128];
        snprintf(label, sizeof(label), "%s%s %.2f", violation ? "VIOLATION " : "", class_name(det.cls_id), det.prop);
        draw_text(image, label, left, std::max(0, top - 22), color, 18);
    }

    bool warning = light.state == "red" && !violator_indices.empty();
    unsigned int banner = warning ? COLOR_RED : (light.state == "green" ? COLOR_GREEN : COLOR_DARK_GRAY);
    draw_rectangle_alpha(image, 8, 8, 700, 108, banner, -1, 190);
    const char* title = warning ? "RED LIGHT: pedestrian violation" :
                        (light.state == "red" ? "RED LIGHT: no pedestrian in crosswalk" :
                         (light.state == "green" ? "GREEN LIGHT: no pedestrian violation" :
                          "LIGHT UNKNOWN: violation not confirmed"));
    draw_text(image, title, 18, 18, COLOR_WHITE, 20);
    char line[192];
    snprintf(line, sizeof(line), "Frame:%d person:%d light:%d in_crosswalk:%d violators:%zu",
             frame_index, person_count, light_count, persons_in_crosswalk, violator_indices.size());
    draw_text(image, line, 18, 46, COLOR_WHITE, 18);
    snprintf(line, sizeof(line), "light=%s red=%d green=%d active=%d infer=%.1fms",
             light.state.c_str(), light.red_count, light.green_count, light.active_count, infer_ms);
    draw_text(image, line, 18, 74, COLOR_WHITE, 18);
}

static void print_usage(const char* prog) {
    printf("Usage: %s <coco_yolo_rknn> <image_folder> <output_dir> [max_frames] [frame_stride]\n", prog);
    printf("Example: %s ./model/yolov8n_coco_fp.rknn ./test_frames/test1 ./outputs_redlight/test1 0 1\n", prog);
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

    printf("Red-light violation frame-sequence check\n");
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

    std::string csv_path = join_path(output_dir, "redlight_violation.csv");
    FILE* csv_fp = fopen(csv_path.c_str(), "w");
    if (csv_fp == nullptr) {
        printf("open csv failed: %s\n", csv_path.c_str());
        release_yolov8_model(&yolo_ctx);
        deinit_post_process();
        return -1;
    }
    fprintf(csv_fp,
            "frame_index,light_state,rule_status,red_count,green_count,active_color_count,"
            "warning,yolo_light_score,yolo_light_candidates,yolo_light_left,yolo_light_top,"
            "yolo_light_right,yolo_light_bottom,det_index,class_name,score,left,top,right,bottom,"
            "bottom_inside,inside_samples,sample_count,infer_ms\n");

    int seen_frames = 0;
    int processed_frames = 0;
    int written_frames = 0;
    int warning_frames = 0;
    int red_frames = 0;
    int green_frames = 0;
    int unknown_light_frames = 0;
    int red_violation_frames = 0;
    int red_no_violation_frames = 0;
    int green_no_violation_frames = 0;
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
        object_detect_result_list detections;
        std::memset(&raw_results, 0, sizeof(raw_results));
        std::memset(&detections, 0, sizeof(detections));

        double infer0 = now_ms();
        if (yolo_ctx.is_quant) {
            ret = inference_yolov8_model(&yolo_ctx, &src_image, &raw_results);
        } else {
            ret = inference_yolov8_model_fp_fallback(&yolo_ctx, &src_image, &raw_results);
        }
        double infer1 = now_ms();
        if (ret != 0) {
            printf("inference failed on %s: %d\n", images[i].c_str(), ret);
            if (src_image.virt_addr != nullptr) free(src_image.virt_addr);
            break;
        }

        filter_person_light(&raw_results, &detections);
        double infer_ms = infer1 - infer0;
        total_infer_ms += infer_ms;
        processed_frames++;

        for (int k = 0; k < detections.count; ++k) {
            const object_detect_result& det = detections.results[k];
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

        std::vector<Point> crosswalk = resolve_crosswalk_polygon(src_image.width, src_image.height);
        LightJudge light = choose_light_by_color(&src_image, &detections);
        std::vector<int> violator_indices;
        int persons_in_crosswalk = 0;

        struct RowInfo {
            int det_index;
            int cls_id;
            image_rect_t box;
            float score;
            bool inside;
            int inside_samples;
            int sample_count;
        };
        std::vector<RowInfo> rows;
        for (int k = 0; k < detections.count; ++k) {
            const object_detect_result& det = detections.results[k];
            RowInfo row;
            row.det_index = k;
            row.cls_id = det.cls_id;
            row.box = det.box;
            row.score = det.prop;
            row.inside = false;
            row.inside_samples = 0;
            row.sample_count = 0;
            if (det.cls_id == 0) {
                row.inside = bottom_line_inside_crosswalk(det.box, crosswalk, &row.inside_samples, &row.sample_count);
                if (row.inside) {
                    persons_in_crosswalk++;
                }
                if (light.state == "red" && row.inside) {
                    violator_indices.push_back(k);
                }
            }
            rows.push_back(row);
        }

        bool warning = light.state == "red" && !violator_indices.empty();
        if (warning) {
            warning_frames++;
        }
        const char* rule_status = warning ? "red_violation" :
                                  (light.state == "red" ? "red_no_violation" :
                                   (light.state == "green" ? "green_no_violation" : "unknown"));
        if (light.state == "red") {
            red_frames++;
            if (warning) {
                red_violation_frames++;
            } else {
                red_no_violation_frames++;
            }
        } else if (light.state == "green") {
            green_frames++;
            green_no_violation_frames++;
        } else {
            unknown_light_frames++;
        }

        printf("frame %d: light=%s status=%s in_crosswalk=%d violators=%zu detections=%d infer=%.2f ms\n",
               seen_frames, light.state.c_str(), rule_status, persons_in_crosswalk,
               violator_indices.size(), detections.count, infer_ms);

        for (size_t r = 0; r < rows.size(); ++r) {
            const RowInfo& row = rows[r];
            fprintf(csv_fp,
                    "%d,%s,%s,%d,%d,%d,%d,%.6f,%d,%d,%d,%d,%d,%d,%s,%.6f,%d,%d,%d,%d,%d,%d,%d,%.3f\n",
                    seen_frames, light.state.c_str(), rule_status,
                    light.red_count, light.green_count, light.active_count, warning ? 1 : 0,
                    light.score, light.candidates,
                    light.box.left, light.box.top, light.box.right, light.box.bottom,
                    row.det_index, class_name(row.cls_id), row.score,
                    row.box.left, row.box.top, row.box.right, row.box.bottom,
                    row.inside ? 1 : 0, row.inside_samples, row.sample_count, infer_ms);
        }
        fflush(csv_fp);

        draw_overlay(&src_image, &detections, violator_indices, crosswalk, light,
                     seen_frames, persons_in_crosswalk, infer_ms);
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
    double warning_ratio = processed_frames > 0 ? 100.0 * warning_frames / processed_frames : 0.0;
    double avg_conf_all = total_detections > 0 ? sum_detection_conf / total_detections : 0.0;
    double avg_conf_person = total_person_detections > 0 ? sum_person_conf / total_person_detections : 0.0;
    double avg_conf_light = total_light_detections > 0 ? sum_light_conf / total_light_detections : 0.0;

    printf("\nRed-light violation summary\n");
    printf("input frames seen: %d\n", seen_frames);
    printf("frames with inference: %d\n", processed_frames);
    printf("warning frames: %d\n", warning_frames);
    printf("output images written: %d\n", written_frames);
    if (processed_frames > 0) {
        printf("avg inference: %.3f ms, %.2f FPS\n", avg_infer, infer_fps);
        printf("avg read+infer+draw+write: %.3f ms, %.2f FPS\n", avg_end, end_fps);
        printf("light state frames: red=%d green=%d unknown=%d\n",
               red_frames, green_frames, unknown_light_frames);
        printf("rule status frames: red_violation=%d red_no_violation=%d green_no_violation=%d unknown=%d\n",
               red_violation_frames, red_no_violation_frames,
               green_no_violation_frames, unknown_light_frames);
        printf("warning ratio: %.2f%%\n", warning_ratio);
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
        fprintf(summary_fp, "Red-light violation summary\n");
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
        fprintf(summary_fp, "red_frames=%d\n", red_frames);
        fprintf(summary_fp, "green_frames=%d\n", green_frames);
        fprintf(summary_fp, "unknown_light_frames=%d\n", unknown_light_frames);
        fprintf(summary_fp, "red_violation_frames=%d\n", red_violation_frames);
        fprintf(summary_fp, "red_no_violation_frames=%d\n", red_no_violation_frames);
        fprintf(summary_fp, "green_no_violation_frames=%d\n", green_no_violation_frames);
        fprintf(summary_fp, "warning_frames=%d\n", warning_frames);
        fprintf(summary_fp, "warning_ratio_percent=%.2f\n", warning_ratio);
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
