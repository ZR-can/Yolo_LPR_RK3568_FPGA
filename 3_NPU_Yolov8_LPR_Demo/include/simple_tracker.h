
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
#include "yolo_lpr_pipeline.h"

struct TrackedPlate {
    int id = -1;
    int time_since_update = 0; 
    int hit_streak = 0; // 连续命中次数，用于过滤突发噪点
    
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
    
    float confidence = 0.0f; 
    float text_confidence = 0.0f; 
    std::string plate_type;
    unsigned int box_color = 0;
    unsigned int text_color = 0;

    // 全周期得分池 (多数投票机制)
    std::map<std::string, float> plate_votes;
};

class SimplePlateTracker {
public:
    SimplePlateTracker();
    void reset();
    void update(const std::vector<PipelineResult>& detections, int frame_id);
    void predict(int frame_id, std::vector<PipelineResult>& out_results) const;

private:
    bool is_valid_plate(const std::string& plate, const std::string& plate_type) const;
    std::string get_best_voted_plate(const std::map<std::string, float>& votes) const;
    
    // 替换为 DIoU 相似度，天然解决快速移动物体 IoU 为 0 的匹配问题
    float compute_similarity(const TrackedPlate& track, const PipelineResult& det) const;

    std::vector<TrackedPlate> tracks_;
    int next_id_ = 0;
    int last_frame_id_ = -1;
    
    // Alpha-Beta 滤波器参数 (稳态卡尔曼)
    float alpha_ = 0.70f;  // 位置更新增益
    float beta_  = 0.40f;  // 速度更新增益
    
    int max_age_frames_ = 5;    // 允许的最大丢失帧数
    int min_hits_ = 2;          // 确认为有效目标所需的最少连续命中次数
    float match_threshold_ = 0.35f; // 匹配阈值 (基于归一化 DIoU)
};

#endif // SIMPLE_TRACKER_H