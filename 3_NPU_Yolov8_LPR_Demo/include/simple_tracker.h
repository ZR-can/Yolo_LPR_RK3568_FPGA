
#ifndef SIMPLE_TRACKER_H
#define SIMPLE_TRACKER_H

#include <vector>
#include <string>
#include <map>
#include "yolo_lpr_pipeline.h"

struct TrackedPlate {
    int id = -1;
    int time_since_update = 0; 
    
    float smooth_left = 0.0f;
    float smooth_top = 0.0f;
    float smooth_right = 0.0f;
    float smooth_bottom = 0.0f;
    
    float vel_left = 0.0f;
    float vel_top = 0.0f;
    float vel_right = 0.0f;
    float vel_bottom = 0.0f;
    
    float confidence = 0.0f; // YOLO 框置信度
    float text_confidence = 0.0f; // LPRNet 置信度
    std::string plate_type;
    unsigned int box_color = 0;
    unsigned int text_color = 0;

    // 全周期得分池 (Key: 车牌号, Value: 累加置信度得分)
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
    
    // 引入混合相似度计算，取代纯 IoU
    float compute_similarity(const TrackedPlate& track, const PipelineResult& det) const;

    std::vector<TrackedPlate> tracks_;
    int next_id_ = 0;
    int last_frame_id_ = -1;
    
    // 提高平滑系数，降低历史惯性占比，提升快车跟随响应
    float smooth_alpha_ = 0.80f; 
    int max_age_frames_ = 3;    
    float match_threshold_ = 0.3f; // 混合相似度阈值
};

#endif // SIMPLE_TRACKER_H