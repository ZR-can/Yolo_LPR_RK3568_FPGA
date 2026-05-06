
// #include "simple_tracker.h"
// #include <math.h>
// #include <algorithm>

// static std::vector<std::string> tracker_parse_utf8(const std::string& str) {
//     std::vector<std::string> chars;
//     for (size_t i = 0; i < str.length();) {
//         int cplen = 1;
//         if ((str[i] & 0xF8) == 0xF0) cplen = 4;
//         else if ((str[i] & 0xF0) == 0xE0) cplen = 3;
//         else if ((str[i] & 0xE0) == 0xC0) cplen = 2;
//         chars.push_back(str.substr(i, cplen));
//         i += cplen;
//     }
//     return chars;
// }

// SimplePlateTracker::SimplePlateTracker() = default;

// void SimplePlateTracker::reset() {
//     tracks_.clear();
//     next_id_ = 0;
//     last_frame_id_ = -1;
// }

// bool SimplePlateTracker::is_valid_plate(const std::string& plate, const std::string& plate_type) const {
//     std::vector<std::string> chars = tracker_parse_utf8(plate);
//     if (chars.empty()) return false;

//     size_t expected_len = (plate_type == "绿") ? 8 : 7;
//     if (chars.size() != expected_len) {
//         return false;
//     }

//     const std::string valid_provinces = "京津沪渝冀豫云辽黑湘皖鲁新苏浙赣鄂桂甘晋蒙陕吉闽贵粤青藏川宁琼使领";
//     if (valid_provinces.find(chars[0]) == std::string::npos) {
//         return false;
//     }

//     return true;
// }

// std::string SimplePlateTracker::get_best_voted_plate(const std::map<std::string, float>& votes) const {
//     if (votes.empty()) return "";
//     float max_score = 0.0f;
//     std::string best_plate = "";
//     for (const auto& pair : votes) {
//         if (pair.second > max_score) {
//             max_score = pair.second;
//             best_plate = pair.first;
//         }
//     }
//     return best_plate;
// }

// // 距离与尺寸感知的混合匹配器
// float SimplePlateTracker::compute_similarity(const TrackedPlate& track, const PipelineResult& det) const {
//     // 1. 常规 IoU 计算 (应对慢速与重叠情况)
//     float x_left = std::max(track.smooth_left, static_cast<float>(det.left));
//     float y_top = std::max(track.smooth_top, static_cast<float>(det.top));
//     float x_right = std::min(track.smooth_right, static_cast<float>(det.right));
//     float y_bottom = std::min(track.smooth_bottom, static_cast<float>(det.bottom));

//     float iou = 0.0f;
//     if (x_right > x_left && y_bottom > y_top) {
//         float intersection_area = (x_right - x_left) * (y_bottom - y_top);
//         float track_area = (track.smooth_right - track.smooth_left) * (track.smooth_bottom - track.smooth_top);
//         float det_area = (det.right - det.left) * (det.bottom - det.top);
//         iou = intersection_area / (track_area + det_area - intersection_area);
//     }

//     // 2. 质心距离计算 (破解快车 0 IoU 的断连)
//     float cx_t = (track.smooth_left + track.smooth_right) / 2.0f;
//     float cy_t = (track.smooth_top + track.smooth_bottom) / 2.0f;
//     float cx_d = (det.left + det.right) / 2.0f;
//     float cy_d = (det.top + det.bottom) / 2.0f;

//     float w_t = track.smooth_right - track.smooth_left;
//     float h_t = track.smooth_bottom - track.smooth_top;
//     float w_d = det.right - det.left;
//     float h_d = det.bottom - det.top;

//     float dx = cx_t - cx_d;
//     float dy = cy_t - cy_d;
//     float dist_sq = dx * dx + dy * dy;

//     // 允许的最大搜索半径：车牌最大尺寸的 5 倍
//     float max_dist = 5.0f * std::max(w_t, h_t);
//     float max_dist_sq = max_dist * max_dist;

//     // 尺寸相似度约束：宽高不能突变超过 1 倍，防误匹
//     float size_ratio_w = std::min(w_t, w_d) / std::max(w_t, w_d);
//     float size_ratio_h = std::min(h_t, h_d) / std::max(h_t, h_d);

//     float sim_score = 0.0f;
//     if (size_ratio_w > 0.5f && size_ratio_h > 0.5f && dist_sq < max_dist_sq) {
//         // 归一化距离得分 (0.0 ~ 1.0)
//         sim_score = 1.0f - (dist_sq / max_dist_sq); 
//     }

//     // 如果物理上有重叠，IoU 具有绝对优先权
//     if (iou > 0.1f) {
//         return 1.0f + iou; // 返回 1.1 ~ 2.0
//     }

//     return sim_score; // 返回 0.0 ~ 1.0
// }

// void SimplePlateTracker::update(const std::vector<PipelineResult>& detections, int frame_id) {
//     int dt = (last_frame_id_ < 0) ? 1 : (frame_id - last_frame_id_);
//     if (dt < 1) dt = 1;
//     last_frame_id_ = frame_id;

//     for (auto& track : tracks_) {
//         track.smooth_left += track.vel_left * dt;
//         track.smooth_top += track.vel_top * dt;
//         track.smooth_right += track.vel_right * dt;
//         track.smooth_bottom += track.vel_bottom * dt;
//         track.time_since_update += dt;
//     }

//     std::vector<bool> det_matched(detections.size(), false);
//     std::vector<bool> track_matched(tracks_.size(), false);

//     for (size_t d = 0; d < detections.size(); ++d) {
//         float best_score = match_threshold_;
//         int best_track_idx = -1;

//         for (size_t t = 0; t < tracks_.size(); ++t) {
//             if (track_matched[t]) continue;
//             float score = compute_similarity(tracks_[t], detections[d]);
//             if (score > best_score) {
//                 best_score = score;
//                 best_track_idx = static_cast<int>(t);
//             }
//         }

//         if (best_track_idx >= 0) {
//             det_matched[d] = true;
//             track_matched[best_track_idx] = true;
//             TrackedPlate& tk = tracks_[best_track_idx];

//             // 1. 计算预测中心点与当前 YOLO 检测中心点的物理偏差
//             float pred_cx = tk.smooth_left + (tk.smooth_right - tk.smooth_left) / 2.0f;
//             float pred_cy = tk.smooth_top + (tk.smooth_bottom - tk.smooth_top) / 2.0f;
//             float det_cx = detections[d].left + (detections[d].right - detections[d].left) / 2.0f;
//             float det_cy = detections[d].top + (detections[d].bottom - detections[d].top) / 2.0f;
            
//             float dx = det_cx - pred_cx;
//             float dy = det_cy - pred_cy;
//             float dist = sqrtf(dx * dx + dy * dy);

//             // 2. 将绝对距离转化为相对车牌尺寸的归一化位移
//             float box_w = detections[d].right - detections[d].left;
//             float box_h = detections[d].bottom - detections[d].top;
//             float diag = sqrtf(box_w * box_w + box_h * box_h);
//             float norm_dist = dist / (diag + 1e-5f);

//             // 3. 计算自适应动态系数
//             // 慢车静止时 norm_dist 趋近于0，alpha 保持在 0.35（重度抗抖动）
//             // 快车经过时 norm_dist 变大，alpha 迅速攀升至 1.0（极速跟随）
//             float dynamic_alpha = 0.35f + (norm_dist * 2.0f);
//             if (dynamic_alpha > 1.0f) dynamic_alpha = 1.0f;
//             if (dynamic_alpha < 0.35f) dynamic_alpha = 0.35f;

//             float dynamic_vel_alpha = 0.20f + (norm_dist * 1.5f);
//             if (dynamic_vel_alpha > 1.0f) dynamic_vel_alpha = 1.0f;
//             if (dynamic_vel_alpha < 0.20f) dynamic_vel_alpha = 0.20f;
//             if (tk.plate_votes.size() < 3) {
//                 dynamic_alpha = 1.0f;
//                 dynamic_vel_alpha = 1.0f;
//             }

//             // 4. 使用动态系数进行位置平滑
//             float new_left = tk.smooth_left + (detections[d].left - tk.smooth_left) * dynamic_alpha;
//             float new_top = tk.smooth_top + (detections[d].top - tk.smooth_top) * dynamic_alpha;
//             float new_right = tk.smooth_right + (detections[d].right - tk.smooth_right) * dynamic_alpha;
//             float new_bottom = tk.smooth_bottom + (detections[d].bottom - tk.smooth_bottom) * dynamic_alpha;

//             // 5. 计算瞬时速度并使用动态系数进行速度平滑
//             // 位移小于 4 像素则强制瞬时速度为 0
//             float inst_vel_left, inst_vel_top, inst_vel_right, inst_vel_bottom;
//             if (dist < 4.0f) {
//                 inst_vel_left = 0.0f;
//                 inst_vel_top = 0.0f;
//                 inst_vel_right = 0.0f;
//                 inst_vel_bottom = 0.0f;
//             } else {
//                 inst_vel_left = (new_left - tk.smooth_left) / dt;
//                 inst_vel_top = (new_top - tk.smooth_top) / dt;
//                 inst_vel_right = (new_right - tk.smooth_right) / dt;
//                 inst_vel_bottom = (new_bottom - tk.smooth_bottom) / dt;
//             }

//             tk.vel_left = tk.vel_left * (1.0f - dynamic_vel_alpha) + inst_vel_left * dynamic_vel_alpha;
//             tk.vel_top = tk.vel_top * (1.0f - dynamic_vel_alpha) + inst_vel_top * dynamic_vel_alpha;
//             tk.vel_right = tk.vel_right * (1.0f - dynamic_vel_alpha) + inst_vel_right * dynamic_vel_alpha;
//             tk.vel_bottom = tk.vel_bottom * (1.0f - dynamic_vel_alpha) + inst_vel_bottom * dynamic_vel_alpha;

//             // 6. 更新最终状态
//             tk.smooth_left = new_left;
//             tk.smooth_top = new_top;
//             tk.smooth_right = new_right;
//             tk.smooth_bottom = new_bottom;

//             tk.time_since_update = 0; 
//             tk.confidence = detections[d].confidence; // YOLO 置信度留给框
//             tk.text_confidence = detections[d].text_confidence; // LPRNet 的字符识别置信度
//             tk.plate_type = detections[d].plate_type;
//             tk.box_color = detections[d].box_color;
//             tk.text_color = detections[d].text_color;

//             if (is_valid_plate(detections[d].plate_name, tk.plate_type)) {
//                 tk.plate_votes[detections[d].plate_name] += tk.confidence; // 换回 YOLO 权重
//             }
//         }
//     }

//     for (size_t d = 0; d < detections.size(); ++d) {
//         if (!det_matched[d]) {
//             TrackedPlate new_tk;
//             new_tk.id = next_id_++;
//             new_tk.time_since_update = 0;
//             new_tk.smooth_left = static_cast<float>(detections[d].left);
//             new_tk.smooth_top = static_cast<float>(detections[d].top);
//             new_tk.smooth_right = static_cast<float>(detections[d].right);
//             new_tk.smooth_bottom = static_cast<float>(detections[d].bottom);
//             new_tk.confidence = detections[d].confidence;
//             new_tk.text_confidence = detections[d].text_confidence;
//             new_tk.plate_type = detections[d].plate_type;
//             new_tk.box_color = detections[d].box_color;
//             new_tk.text_color = detections[d].text_color;

//             if (is_valid_plate(detections[d].plate_name, new_tk.plate_type)) {
//                 new_tk.plate_votes[detections[d].plate_name] += new_tk.confidence; // 换回 YOLO 权重
//             }
//             tracks_.push_back(new_tk);
//         }
//     }

//     for (auto it = tracks_.begin(); it != tracks_.end(); ) {
//         if (it->time_since_update > max_age_frames_) {
//             it = tracks_.erase(it);
//         } else {
//             ++it;
//         }
//     }
// }

// void SimplePlateTracker::predict(int frame_id, std::vector<PipelineResult>& out_results) const {
//     out_results.clear();
    
//     // 计算当前显示帧与上一次 NPU 刷新帧之间的时间差
//     int dt = (last_frame_id_ < 0) ? 0 : (frame_id - last_frame_id_);

//     for (const auto& track : tracks_) {
//         // 严格阻断：仅渲染当前有效目标，杜绝残留
//         if (track.time_since_update == 0) {
//             PipelineResult res;
            
//             // 补齐惯性预测逻辑：利用速度向量在 NPU 间隙补帧，实现 30FPS 丝滑移动
//             res.left = static_cast<int>(track.smooth_left + track.vel_left * dt + 0.5f);
//             res.top = static_cast<int>(track.smooth_top + track.vel_top * dt + 0.5f);
//             res.right = static_cast<int>(track.smooth_right + track.vel_right * dt + 0.5f);
//             res.bottom = static_cast<int>(track.smooth_bottom + track.vel_bottom * dt + 0.5f);
            
//             res.confidence = track.confidence;
//             res.plate_type = track.plate_type;
//             res.box_color = track.box_color;
//             res.text_color = track.text_color;

//             std::string final_plate = get_best_voted_plate(track.plate_votes);
//             res.plate_name = final_plate.empty() ? ":" : final_plate;
            
//             out_results.push_back(res);
//         }
//     }
// }

#include "simple_tracker.h"
#include <cmath>
#include <algorithm>

static std::vector<std::string> tracker_parse_utf8(const std::string& str) {
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

SimplePlateTracker::SimplePlateTracker() = default;

void SimplePlateTracker::reset() {
    tracks_.clear();
    next_id_ = 0;
    last_frame_id_ = -1;
}

bool SimplePlateTracker::is_valid_plate(const std::string& plate, const std::string& plate_type) const {
    std::vector<std::string> chars = tracker_parse_utf8(plate);
    if (chars.empty()) return false;

    size_t expected_len = (plate_type == "绿") ? 8 : 7;
    if (chars.size() != expected_len) {
        return false;
    }

    const std::string valid_provinces = "京津沪渝冀豫云辽黑湘皖鲁新苏浙赣鄂桂甘晋蒙陕吉闽贵粤青藏川宁琼";
    if (valid_provinces.find(chars[0]) == std::string::npos) {
        return false;
    }

    const std::string valid_letters = "ABCDEFGHJKLMNPQRSTUVWXYZ"; //暂时省略 I 和 O，避免与数字混淆
    if (valid_letters.find(chars[1]) == std::string::npos) {
        return false;
    }

    return true;
}

std::string SimplePlateTracker::get_best_voted_plate(const std::map<std::string, float>& votes) const {
    if (votes.empty()) return "";
    float max_score = 0.0f;
    std::string best_plate = "";
    for (const auto& pair : votes) {
        if (pair.second > max_score) {
            max_score = pair.second;
            best_plate = pair.first;
        }
    }
    return best_plate;
}

// 基于 DIoU (Distance-IoU) 的相似度计算
float SimplePlateTracker::compute_similarity(const TrackedPlate& track, const PipelineResult& det) const {
    float t_left = track.cx - track.w / 2.0f;
    float t_top = track.cy - track.h / 2.0f;
    float t_right = track.cx + track.w / 2.0f;
    float t_bottom = track.cy + track.h / 2.0f;

    float inter_left = std::max(t_left, static_cast<float>(det.left));
    float inter_top = std::max(t_top, static_cast<float>(det.top));
    float inter_right = std::min(t_right, static_cast<float>(det.right));
    float inter_bottom = std::min(t_bottom, static_cast<float>(det.bottom));

    float iou = 0.0f;
    if (inter_right > inter_left && inter_bottom > inter_top) {
        float inter_area = (inter_right - inter_left) * (inter_bottom - inter_top);
        float track_area = track.w * track.h;
        float det_area = (det.right - det.left) * (det.bottom - det.top);
        iou = inter_area / (track_area + det_area - inter_area + 1e-6f);
    }

    float d_cx = det.left + (det.right - det.left) / 2.0f;
    float d_cy = det.top + (det.bottom - det.top) / 2.0f;
    
    // 计算中心点欧氏距离平方
    float dist_sq = (track.cx - d_cx) * (track.cx - d_cx) + (track.cy - d_cy) * (track.cy - d_cy);

    // 计算最小闭包区域的对角线距离平方
    float enc_left = std::min(t_left, static_cast<float>(det.left));
    float enc_top = std::min(t_top, static_cast<float>(det.top));
    float enc_right = std::max(t_right, static_cast<float>(det.right));
    float enc_bottom = std::max(t_bottom, static_cast<float>(det.bottom));
    float diag_sq = (enc_right - enc_left) * (enc_right - enc_left) + (enc_bottom - enc_top) * (enc_bottom - enc_top) + 1e-6f;

    // DIoU 取值范围 [-1, 1]
    float diou = iou - (dist_sq / diag_sq);
    
    // 映射到 [0, 1] 区间作为相似度得分
    return (diou + 1.0f) / 2.0f;
}

void SimplePlateTracker::update(const std::vector<PipelineResult>& detections, int frame_id) {
    int dt = (last_frame_id_ < 0) ? 1 : (frame_id - last_frame_id_);
    if (dt < 1) dt = 1;
    last_frame_id_ = frame_id;

    // 1. 状态预测 (使用等速运动模型)
    for (auto& track : tracks_) {
        track.cx += track.vx * dt;
        track.cy += track.vy * dt;
        track.w  += track.vw * dt;
        track.h  += track.vh * dt;
        track.time_since_update += dt;
    }

    // 2. 构建相似度矩阵并进行贪心二分图匹配
    struct Match { int trk_idx; int det_idx; float score; };
    std::vector<Match> matches;
    matches.reserve(tracks_.size() * detections.size());

    for (size_t t = 0; t < tracks_.size(); ++t) {
        for (size_t d = 0; d < detections.size(); ++d) {
            float score = compute_similarity(tracks_[t], detections[d]);
            if (score > match_threshold_) {
                matches.push_back({static_cast<int>(t), static_cast<int>(d), score});
            }
        }
    }

    // 按相似度降序排列
    std::sort(matches.begin(), matches.end(), [](const Match& a, const Match& b) {
        return a.score > b.score;
    });

    std::vector<bool> det_matched(detections.size(), false);
    std::vector<bool> track_matched(tracks_.size(), false);

    // 3. 执行匹配与状态更新 (Alpha-Beta 滤波)
    for (const auto& m : matches) {
        if (!det_matched[m.det_idx] && !track_matched[m.trk_idx]) {
            det_matched[m.det_idx] = true;
            track_matched[m.trk_idx] = true;

            TrackedPlate& tk = tracks_[m.trk_idx];
            const auto& det = detections[m.det_idx];

            float det_w = det.right - det.left;
            float det_h = det.bottom - det.top;
            float det_cx = det.left + det_w / 2.0f;
            float det_cy = det.top + det_h / 2.0f;

            // 计算测量残差
            float res_cx = det_cx - tk.cx;
            float res_cy = det_cy - tk.cy;
            float res_w  = det_w - tk.w;
            float res_h  = det_h - tk.h;

            // 应用 Alpha-Beta 方程更新状态和速度
            tk.cx += alpha_ * res_cx;
            tk.cy += alpha_ * res_cy;
            tk.w  += alpha_ * res_w;
            tk.h  += alpha_ * res_h;

            tk.vx += (beta_ / dt) * res_cx;
            tk.vy += (beta_ / dt) * res_cy;
            tk.vw += (beta_ / dt) * res_w;
            tk.vh += (beta_ / dt) * res_h;

            // 更新属性与投票
            tk.time_since_update = 0;
            tk.hit_streak++;
            tk.confidence = det.confidence;
            tk.text_confidence = det.text_confidence;
            tk.plate_type = det.plate_type;
            tk.box_color = det.box_color;
            tk.text_color = det.text_color;

            if (is_valid_plate(det.plate_name, tk.plate_type)) {
                tk.plate_votes[det.plate_name] += tk.confidence;
            }
        }
    }

    // 4. 为未匹配的检测创建新轨迹
    for (size_t d = 0; d < detections.size(); ++d) {
        if (!det_matched[d]) {
            const auto& det = detections[d];
            TrackedPlate new_tk;
            new_tk.id = next_id_++;
            new_tk.time_since_update = 0;
            new_tk.hit_streak = 1;

            new_tk.w = det.right - det.left;
            new_tk.h = det.bottom - det.top;
            new_tk.cx = det.left + new_tk.w / 2.0f;
            new_tk.cy = det.top + new_tk.h / 2.0f;
            
            new_tk.confidence = det.confidence;
            new_tk.text_confidence = det.text_confidence;
            new_tk.plate_type = det.plate_type;
            new_tk.box_color = det.box_color;
            new_tk.text_color = det.text_color;

            if (is_valid_plate(det.plate_name, new_tk.plate_type)) {
                new_tk.plate_votes[det.plate_name] += new_tk.confidence;
            }
            tracks_.push_back(new_tk);
        }
    }

    // 5. 淘汰过期轨迹
    tracks_.erase(std::remove_if(tracks_.begin(), tracks_.end(),
        [this](const TrackedPlate& tk) { return tk.time_since_update > max_age_frames_; }),
        tracks_.end());
}

void SimplePlateTracker::predict(int frame_id, std::vector<PipelineResult>& out_results) const {
    out_results.clear();
    
    int dt = (last_frame_id_ < 0) ? 0 : (frame_id - last_frame_id_);

    for (const auto& track : tracks_) {
        // 仅输出当前帧存活且生命周期达到阈值的稳定目标（防止噪点闪烁）
        if (track.time_since_update == 0 && track.hit_streak >= min_hits_) {
            PipelineResult res;
            
            // 结合速度向量进行渲染预测补偿
            float pred_cx = track.cx + track.vx * dt;
            float pred_cy = track.cy + track.vy * dt;
            float pred_w  = track.w + track.vw * dt;
            float pred_h  = track.h + track.vh * dt;

            res.left   = static_cast<int>(pred_cx - pred_w / 2.0f + 0.5f);
            res.top    = static_cast<int>(pred_cy - pred_h / 2.0f + 0.5f);
            res.right  = static_cast<int>(pred_cx + pred_w / 2.0f + 0.5f);
            res.bottom = static_cast<int>(pred_cy + pred_h / 2.0f + 0.5f);
            
            res.confidence = track.confidence;
            res.text_confidence = track.text_confidence;
            res.plate_type = track.plate_type;
            res.box_color = track.box_color;
            res.text_color = track.text_color;

            std::string final_plate = get_best_voted_plate(track.plate_votes);
            res.plate_name = final_plate.empty() ? ":" : final_plate;
            
            out_results.push_back(res);
        }
    }
}