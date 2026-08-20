#include "obb_postprocess.h"

#include <algorithm>
#include <cmath>

namespace {

const int kClassCount = 4;
const int kRegMax = 16;
const int kDflChannels = 4 * kRegMax;
const float kPi = 3.1415927410125732f;

float Dequantize(int8_t value, int32_t zero_point, float scale) {
    return (static_cast<float>(value) - zero_point) * scale;
}

float Sigmoid(float value) {
    return 1.0f / (1.0f + std::exp(-value));
}

void Softmax16(float* values) {
    float maximum = values[0];
    for (int index = 1; index < kRegMax; ++index) {
        maximum = std::max(maximum, values[index]);
    }
    float sum = 0.0f;
    for (int index = 0; index < kRegMax; ++index) {
        values[index] = std::exp(values[index] - maximum);
        sum += values[index];
    }
    for (int index = 0; index < kRegMax; ++index) {
        values[index] /= sum;
    }
}

std::array<PointF, 4> DetectionCorners(float center_x,
                                       float center_y,
                                       float width,
                                       float height,
                                       float angle) {
    const float half_width = width * 0.5f;
    const float half_height = height * 0.5f;
    const float cosine = std::cos(angle);
    const float sine = std::sin(angle);
    const PointF local[4] = {
        {-half_width, -half_height},
        {half_width, -half_height},
        {half_width, half_height},
        {-half_width, half_height},
    };
    std::array<PointF, 4> corners;
    for (int index = 0; index < 4; ++index) {
        corners[index].x =
            cosine * local[index].x - sine * local[index].y + center_x;
        corners[index].y =
            sine * local[index].x + cosine * local[index].y + center_y;
    }
    return corners;
}

float Cross(const PointF& a, const PointF& b, const PointF& point) {
    return (b.x - a.x) * (point.y - a.y) -
           (b.y - a.y) * (point.x - a.x);
}

float SignedArea(const std::vector<PointF>& polygon) {
    if (polygon.size() < 3U) {
        return 0.0f;
    }
    float sum = 0.0f;
    for (size_t index = 0; index < polygon.size(); ++index) {
        const PointF& first = polygon[index];
        const PointF& second = polygon[(index + 1U) % polygon.size()];
        sum += first.x * second.y - second.x * first.y;
    }
    return sum * 0.5f;
}

PointF LineIntersection(const PointF& first,
                        const PointF& second,
                        const PointF& clip_first,
                        const PointF& clip_second) {
    const float first_dx = second.x - first.x;
    const float first_dy = second.y - first.y;
    const float clip_dx = clip_second.x - clip_first.x;
    const float clip_dy = clip_second.y - clip_first.y;
    const float denominator = first_dx * clip_dy - first_dy * clip_dx;
    if (std::fabs(denominator) < 1e-8f) {
        return second;
    }
    const float t = ((clip_first.x - first.x) * clip_dy -
                     (clip_first.y - first.y) * clip_dx) /
                    denominator;
    return {first.x + t * first_dx, first.y + t * first_dy};
}

std::vector<PointF> ConvexIntersection(
    const std::array<PointF, 4>& subject_array,
    const std::array<PointF, 4>& clip_array) {
    std::vector<PointF> output(subject_array.begin(), subject_array.end());
    const std::vector<PointF> clip(clip_array.begin(), clip_array.end());
    const float orientation = SignedArea(clip) >= 0.0f ? 1.0f : -1.0f;
    for (size_t edge = 0; edge < clip.size() && !output.empty(); ++edge) {
        const PointF clip_first = clip[edge];
        const PointF clip_second = clip[(edge + 1U) % clip.size()];
        const std::vector<PointF> input = output;
        output.clear();
        PointF previous = input.back();
        bool previous_inside =
            orientation * Cross(clip_first, clip_second, previous) >= -1e-5f;
        for (const PointF& current : input) {
            const bool current_inside =
                orientation * Cross(clip_first, clip_second, current) >= -1e-5f;
            if (current_inside != previous_inside) {
                output.push_back(LineIntersection(
                    previous, current, clip_first, clip_second));
            }
            if (current_inside) {
                output.push_back(current);
            }
            previous = current;
            previous_inside = current_inside;
        }
    }
    return output;
}

float RotatedIou(const ObbDetection& first, const ObbDetection& second) {
    const std::array<PointF, 4> first_corners = DetectionCorners(
        first.center_x, first.center_y, first.width, first.height, first.angle);
    const std::array<PointF, 4> second_corners = DetectionCorners(
        second.center_x, second.center_y, second.width, second.height, second.angle);
    const std::vector<PointF> intersection =
        ConvexIntersection(first_corners, second_corners);
    const float intersection_area = std::fabs(SignedArea(intersection));
    const float union_area = first.width * first.height +
                             second.width * second.height - intersection_area;
    return union_area > 0.0f ? intersection_area / union_area : 0.0f;
}

float Clamp(float value, float minimum, float maximum) {
    return std::max(minimum, std::min(value, maximum));
}

uint8_t Expand5To8(unsigned int value) {
    return static_cast<uint8_t>((value << 3U) | (value >> 2U));
}

uint8_t Expand6To8(unsigned int value) {
    return static_cast<uint8_t>((value << 2U) | (value >> 4U));
}

uint8_t ReadRgbChannel(const image_buffer_t& image,
                       int x,
                       int y,
                       int channel) {
    const int stride = image.width_stride > 0 ? image.width_stride : image.width;
    if (image.format == IMAGE_FORMAT_RGB888) {
        return image.virt_addr[
            (static_cast<size_t>(y) * stride + x) * 3U + channel];
    }

    const uint8_t* bytes = image.virt_addr +
                           (static_cast<size_t>(y) * stride + x) * 2U;
    const unsigned int pixel = static_cast<unsigned int>(bytes[0]) |
                               (static_cast<unsigned int>(bytes[1]) << 8U);
    // Match the existing PP-OCR cv::COLOR_BGR5652BGR path: R is the low
    // 5-bit component and B is the high 5-bit component.
    if (channel == 0) {
        return Expand5To8(pixel & 0x1fU);
    }
    if (channel == 1) {
        return Expand6To8((pixel >> 5U) & 0x3fU);
    }
    return Expand5To8((pixel >> 11U) & 0x1fU);
}

uint8_t BilinearRgb(const image_buffer_t& image,
                    float x,
                    float y,
                    int channel) {
    x = Clamp(x, 0.0f, static_cast<float>(image.width - 1));
    y = Clamp(y, 0.0f, static_cast<float>(image.height - 1));
    const int x0 = static_cast<int>(std::floor(x));
    const int y0 = static_cast<int>(std::floor(y));
    const int x1 = std::min(x0 + 1, image.width - 1);
    const int y1 = std::min(y0 + 1, image.height - 1);
    const float x_weight = x - x0;
    const float y_weight = y - y0;
    const float top = ReadRgbChannel(image, x0, y0, channel) *
                          (1.0f - x_weight) +
                      ReadRgbChannel(image, x1, y0, channel) * x_weight;
    const float bottom = ReadRgbChannel(image, x0, y1, channel) *
                             (1.0f - x_weight) +
                         ReadRgbChannel(image, x1, y1, channel) * x_weight;
    return static_cast<uint8_t>(
        std::round(top * (1.0f - y_weight) + bottom * y_weight));
}

}  // namespace

bool UnpackObbNativeInt8(const rknn_tensor_attr& native_attr,
                         const rknn_tensor_attr& logical_attr,
                         const rknn_tensor_mem* memory,
                         std::vector<int8_t>* output) {
    if (memory == nullptr || memory->virt_addr == nullptr || output == nullptr ||
        native_attr.type != RKNN_TENSOR_INT8 ||
        logical_attr.type != RKNN_TENSOR_INT8 || logical_attr.n_elems == 0U) {
        return false;
    }
    output->assign(logical_attr.n_elems, 0);
    const int8_t* source = static_cast<const int8_t*>(memory->virt_addr);
    if (native_attr.fmt != RKNN_TENSOR_NC1HWC2) {
        std::copy(source, source + logical_attr.n_elems, output->begin());
        return true;
    }
    if (native_attr.n_dims != 5 || native_attr.dims[4] == 0U ||
        logical_attr.n_dims < 3 || logical_attr.dims[0] != 1U ||
        logical_attr.dims[1] == 0U) {
        return false;
    }
    const int channels = static_cast<int>(logical_attr.dims[1]);
    const int logical_spatial = static_cast<int>(
        logical_attr.n_elems / logical_attr.dims[0] / logical_attr.dims[1]);
    const int c1 = static_cast<int>(native_attr.dims[1]);
    const int native_spatial = static_cast<int>(
        native_attr.dims[2] * native_attr.dims[3]);
    const int c2 = static_cast<int>(native_attr.dims[4]);
    if ((channels + c2 - 1) / c2 > c1 || logical_spatial > native_spatial) {
        return false;
    }
    for (int channel = 0; channel < channels; ++channel) {
        const int plane = channel / c2;
        const int offset = channel % c2;
        for (int position = 0; position < logical_spatial; ++position) {
            (*output)[channel * logical_spatial + position] =
                source[(plane * native_spatial + position) * c2 + offset];
        }
    }
    return true;
}

int DecodeAndNmsObb(const std::array<QuantizedTensor, 3>& branches,
                    const QuantizedTensor& angle,
                    float confidence_threshold,
                    float nms_threshold,
                    int maximum_detections,
                    std::vector<ObbDetection>* detections) {
    if (angle.data == nullptr || detections == nullptr ||
        maximum_detections <= 0) {
        return -1;
    }
    std::vector<ObbDetection> candidates;
    int angle_offset = 0;
    const int strides[3] = {8, 16, 32};
    for (int branch_index = 0; branch_index < 3; ++branch_index) {
        const QuantizedTensor& branch = branches[branch_index];
        if (branch.data == nullptr ||
            branch.channels != kDflChannels + kClassCount ||
            branch.height <= 0 || branch.width <= 0) {
            return -1;
        }
        const int spatial = branch.height * branch.width;
        for (int y = 0; y < branch.height; ++y) {
            for (int x = 0; x < branch.width; ++x) {
                const int position = y * branch.width + x;
                for (int class_id = 0; class_id < kClassCount; ++class_id) {
                    const int class_index =
                        (kDflChannels + class_id) * spatial + position;
                    const float score = Sigmoid(Dequantize(
                        branch.data[class_index], branch.zero_point, branch.scale));
                    if (score < confidence_threshold) {
                        continue;
                    }
                    float distances[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                    for (int side = 0; side < 4; ++side) {
                        float logits[kRegMax];
                        for (int bin = 0; bin < kRegMax; ++bin) {
                            const int index =
                                (side * kRegMax + bin) * spatial + position;
                            logits[bin] = Dequantize(
                                branch.data[index],
                                branch.zero_point,
                                branch.scale);
                        }
                        Softmax16(logits);
                        for (int bin = 0; bin < kRegMax; ++bin) {
                            distances[side] += logits[bin] * bin;
                        }
                    }
                    const int angle_index = angle_offset + position;
                    const float decoded_angle =
                        (Dequantize(angle.data[angle_index],
                                    angle.zero_point,
                                    angle.scale) -
                         0.25f) *
                        kPi;
                    const float cosine = std::cos(decoded_angle);
                    const float sine = std::sin(decoded_angle);
                    const float delta_x =
                        (distances[2] - distances[0]) * 0.5f;
                    const float delta_y =
                        (distances[3] - distances[1]) * 0.5f;
                    ObbDetection detection;
                    detection.class_id = class_id;
                    detection.score = score;
                    detection.center_x =
                        (delta_x * cosine - delta_y * sine + x + 0.5f) *
                        strides[branch_index];
                    detection.center_y =
                        (delta_x * sine + delta_y * cosine + y + 0.5f) *
                        strides[branch_index];
                    detection.width =
                        (distances[0] + distances[2]) * strides[branch_index];
                    detection.height =
                        (distances[1] + distances[3]) * strides[branch_index];
                    detection.angle = decoded_angle;
                    detection.source_corners = {};
                    candidates.push_back(detection);
                }
            }
        }
        angle_offset += spatial;
    }
    if (angle.channels * angle.height * angle.width != angle_offset) {
        return -1;
    }

    std::sort(candidates.begin(),
              candidates.end(),
              [](const ObbDetection& first, const ObbDetection& second) {
                  return first.score > second.score;
              });
    detections->clear();
    for (const ObbDetection& candidate : candidates) {
        bool suppressed = false;
        for (const ObbDetection& kept : *detections) {
            // Preserve cross-color candidates until after OCR. INT8 can map
            // blue/green logits to the same score, so suppressing here could
            // discard the color whose 7/8-character structure is correct.
            if (candidate.class_id == kept.class_id &&
                RotatedIou(candidate, kept) > nms_threshold) {
                suppressed = true;
                break;
            }
        }
        if (!suppressed) {
            detections->push_back(candidate);
            if (static_cast<int>(detections->size()) >= maximum_detections) {
                break;
            }
        }
    }
    return 0;
}

bool PrepareObbOcrInput(const image_buffer_t& source_image,
                        const letterbox_t& letterbox,
                        ObbDetection* detection,
                        ObbOcrInput* output) {
    if (detection == nullptr || output == nullptr ||
        source_image.virt_addr == nullptr ||
        (source_image.format != IMAGE_FORMAT_RGB888 &&
         source_image.format != IMAGE_FORMAT_BGR565) ||
        source_image.width <= 0 || source_image.height <= 0 ||
        letterbox.scale <= 0.0f) {
        return false;
    }
    const int stride = source_image.width_stride > 0
                           ? source_image.width_stride
                           : source_image.width;
    const int bytes_per_pixel =
        source_image.format == IMAGE_FORMAT_RGB888 ? 3 : 2;
    const size_t minimum_size = static_cast<size_t>(stride) *
                                source_image.height * bytes_per_pixel;
    if (source_image.size < static_cast<int>(minimum_size)) {
        return false;
    }

    float width = detection->width / letterbox.scale;
    float height = detection->height / letterbox.scale;
    float angle = detection->angle;
    if (height > width) {
        std::swap(width, height);
        angle += kPi * 0.5f;
    }
    while (angle >= kPi * 0.5f) {
        angle -= kPi;
    }
    while (angle < -kPi * 0.5f) {
        angle += kPi;
    }
    const float center_x =
        (detection->center_x - letterbox.x_pad) / letterbox.scale;
    const float center_y =
        (detection->center_y - letterbox.y_pad) / letterbox.scale;
    detection->source_corners =
        DetectionCorners(center_x, center_y, width, height, angle);

    const float ratio = height > 0.0f ? width / height : 1.0f;
    output->resized_width = std::max(
        1,
        std::min(static_cast<int>(ObbOcrInput::kWidth),
                 static_cast<int>(std::ceil(ObbOcrInput::kHeight * ratio))));
    output->bgr.assign(ObbOcrInput::kWidth * ObbOcrInput::kHeight *
                           ObbOcrInput::kChannels,
                       128);
    const PointF& top_left = detection->source_corners[0];
    const PointF& top_right = detection->source_corners[1];
    const PointF& bottom_left = detection->source_corners[3];
    for (int y = 0; y < ObbOcrInput::kHeight; ++y) {
        const float v = (y + 0.5f) / ObbOcrInput::kHeight;
        for (int x = 0; x < output->resized_width; ++x) {
            const float u = (x + 0.5f) / output->resized_width;
            const float source_x =
                top_left.x + u * (top_right.x - top_left.x) +
                v * (bottom_left.x - top_left.x);
            const float source_y =
                top_left.y + u * (top_right.y - top_left.y) +
                v * (bottom_left.y - top_left.y);
            const size_t target =
                (static_cast<size_t>(y) * ObbOcrInput::kWidth + x) * 3U;
            output->bgr[target + 0U] =
                BilinearRgb(source_image, source_x, source_y, 2);
            output->bgr[target + 1U] =
                BilinearRgb(source_image, source_x, source_y, 1);
            output->bgr[target + 2U] =
                BilinearRgb(source_image, source_x, source_y, 0);
        }
    }
    return true;
}

image_rect_t ObbSourceBoundingBox(const ObbDetection& detection,
                                  int image_width,
                                  int image_height) {
    image_rect_t box = {0, 0, 0, 0};
    if (image_width <= 0 || image_height <= 0) {
        return box;
    }
    float minimum_x = detection.source_corners[0].x;
    float maximum_x = detection.source_corners[0].x;
    float minimum_y = detection.source_corners[0].y;
    float maximum_y = detection.source_corners[0].y;
    for (size_t index = 1; index < detection.source_corners.size(); ++index) {
        minimum_x = std::min(minimum_x, detection.source_corners[index].x);
        maximum_x = std::max(maximum_x, detection.source_corners[index].x);
        minimum_y = std::min(minimum_y, detection.source_corners[index].y);
        maximum_y = std::max(maximum_y, detection.source_corners[index].y);
    }
    box.left = std::max(
        0, std::min(static_cast<int>(std::floor(minimum_x)), image_width));
    box.top = std::max(
        0, std::min(static_cast<int>(std::floor(minimum_y)), image_height));
    box.right = std::max(
        0, std::min(static_cast<int>(std::ceil(maximum_x)), image_width));
    box.bottom = std::max(
        0, std::min(static_cast<int>(std::ceil(maximum_y)), image_height));
    return box;
}

ObbPlateColorEvidence AnalyzeObbPlateColor(const ObbOcrInput& input) {
    ObbPlateColorEvidence evidence = {0U, 0U, 0U};
    if (input.resized_width <= 0 ||
        input.resized_width > ObbOcrInput::kWidth ||
        input.bgr.size() != static_cast<size_t>(
                                ObbOcrInput::kWidth *
                                ObbOcrInput::kHeight *
                                ObbOcrInput::kChannels)) {
        return evidence;
    }

    const int left = input.resized_width / 20;
    const int right = input.resized_width - left;
    const int top = ObbOcrInput::kHeight / 8;
    const int bottom = ObbOcrInput::kHeight - top;
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            const size_t offset =
                (static_cast<size_t>(y) * ObbOcrInput::kWidth + x) * 3U;
            const int blue = input.bgr[offset + 0U];
            const int green = input.bgr[offset + 1U];
            const int red = input.bgr[offset + 2U];
            const int maximum = std::max(red, std::max(green, blue));
            const int minimum = std::min(red, std::min(green, blue));
            if (maximum < 50 || maximum - minimum < 24) {
                continue;
            }
            ++evidence.colored_pixels;
            const int blue_margin = blue - std::max(green, red);
            const int green_margin = green - std::max(blue, red);
            if (blue_margin > 0) {
                evidence.blue_score += static_cast<uint64_t>(blue_margin);
            }
            if (green_margin > 0) {
                evidence.green_score += static_cast<uint64_t>(green_margin);
            }
        }
    }
    return evidence;
}

int DominantObbPlateColor(const ObbPlateColorEvidence& evidence) {
    if (evidence.colored_pixels == 0U) {
        return -1;
    }
    const uint64_t minimum_evidence =
        std::max<uint64_t>(100U, evidence.colored_pixels * 4ULL);
    if (evidence.blue_score < minimum_evidence &&
        evidence.green_score < minimum_evidence) {
        return -1;
    }
    if (evidence.blue_score * 4ULL >= evidence.green_score * 5ULL) {
        return 0;
    }
    if (evidence.green_score * 4ULL >= evidence.blue_score * 5ULL) {
        return 1;
    }
    return -1;
}
