
// #ifndef SIMPLE_TRACKER_H
// #define SIMPLE_TRACKER_H

// #include <vector>
// #include <string>
// #include <map>
// #include "yolo_lpr_pipeline.h"

// struct TrackedPlate {
//     int id = -1;
//     int time_since_update = 0; 
    
//     float smooth_left = 0.0f;
//     float smooth_top = 0.0f;
//     float smooth_right = 0.0f;
//     float smooth_bottom = 0.0f;
    
//     float vel_left = 0.0f;
//     float vel_top = 0.0f;
//     float vel_right = 0.0f;
//     float vel_bottom = 0.0f;
    
//     float confidence = 0.0f; // YOLO 框置信度
//     float text_confidence = 0.0f; // LPRNet 置信度
//     std::string plate_type;
//     unsigned int box_color = 0;
//     unsigned int text_color = 0;

//     // 全周期得分池 (Key: 车牌号, Value: 累加置信度得分)
//     std::map<std::string, float> plate_votes;
// };

// class SimplePlateTracker {
// public:
//     SimplePlateTracker();
//     void reset();
//     void update(const std::vector<PipelineResult>& detections, int frame_id);
//     void predict(int frame_id, std::vector<PipelineResult>& out_results) const;

// private:
//     bool is_valid_plate(const std::string& plate, const std::string& plate_type) const;
//     std::string get_best_voted_plate(const std::map<std::string, float>& votes) const;
    
//     // 引入混合相似度计算，取代纯 IoU
//     float compute_similarity(const TrackedPlate& track, const PipelineResult& det) const;

//     std::vector<TrackedPlate> tracks_;
//     int next_id_ = 0;
//     int last_frame_id_ = -1;
    
//     // 提高平滑系数，降低历史惯性占比，提升快车跟随响应
//     float smooth_alpha_ = 0.80f; 
//     int max_age_frames_ = 3;    
//     float match_threshold_ = 0.3f; // 混合相似度阈值
// };

// #endif // SIMPLE_TRACKER_H

#ifndef SIMPLE_TRACKER_H
#define SIMPLE_TRACKER_H

#include <vector>
#include <string>
#include <map>
#include "plate_pipeline_result.h"

struct TrackedPlate {
    int id = -1;
    int time_since_update = 0; 
    int hit_streak = 0; // 关联命中次数，用于过滤突发噪点
    
    // 运动学状态 (中心点及宽高)
    float cx = 0.0f;
    float cy = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
    
    // 速度状态
    float vx = 0.0f;
    float vy = 0.0f;
    float vw = 0.0f;
    float vh = 0.0f;

    // 加速度状态，用于补偿车辆接近镜头时的透视加速。
    float ax = 0.0f;
    float ay = 0.0f;
    float aw = 0.0f;
    float ah = 0.0f;

    // 最近一次真实检测观测；空检测帧不得改变该时间基准。
    int last_observation_frame_id = -1;
    float observed_cx = 0.0f;
    float observed_cy = 0.0f;
    float observed_w = 0.0f;
    float observed_h = 0.0f;
    float observed_vx = 0.0f;
    float observed_vy = 0.0f;
    float observed_vw = 0.0f;
    float observed_vh = 0.0f;
    
    float confidence = 0.0f; 
    float text_confidence = 0.0f; 
    std::string plate_type;
    unsigned int box_color = 0;
    unsigned int text_color = 0;

    // 全周期得分池；同一合法文本连续命中 min_hits_ 次后才允许进入。
    std::map<std::string, float> plate_votes;

    // 未通过 GA 36 校验的结果仅用于连续命中后的诊断显示，不进入有效投票池。
    std::string latest_plate_text;
    int latest_plate_hits = 0;
};

class SimplePlateTracker {
public:
    SimplePlateTracker();
    void reset();
    void update(const std::vector<PipelineResult>& detections,
                int frame_id,
                bool static_image_mode = false);
    void predict(int frame_id,
                 std::vector<PipelineResult>& out_results,
                 bool static_image_mode = false) const;

private:
    bool is_valid_plate(const std::string& plate, const std::string& plate_type) const;
    std::string get_best_voted_plate(const std::map<std::string, float>& votes) const;
    
    // DIoU 常规关联，以及仅面向未确认且尺寸相近轨迹的高速首联保护。
    float compute_similarity(const TrackedPlate& track, const PipelineResult& det) const;
    bool is_fast_motion_initial_match(const TrackedPlate& track,
                                      const PipelineResult& det,
                                      float similarity) const;
    void advance_motion_state(TrackedPlate* track, int dt) const;
    void update_motion_from_observation(TrackedPlate* track,
                                        const PipelineResult& det,
                                        int frame_id) const;
    void clamp_acceleration(TrackedPlate* track) const;

    std::vector<TrackedPlate> tracks_;
    int next_id_ = 0;
    int last_frame_id_ = -1;
    
    // 真实观测速度 + 受限加速度滤波参数。
    float position_gain_ = 0.90f;
    float velocity_gain_ = 0.80f;
    float acceleration_gain_ = 0.35f;
    float acceleration_limit_ratio_ = 0.12f;
    
    int max_age_frames_ = 8;    // 允许短检测空窗继续预测，约 4 个推理周期
    int min_hits_ = 2;          // 确认为有效目标所需的最少连续命中次数
    float match_threshold_ = 0.30f; // 常规匹配阈值 (基于归一化 DIoU)
    float initial_match_threshold_ = 0.20f;
    float initial_match_min_size_ratio_ = 0.65f;
};

#endif // SIMPLE_TRACKER_H
