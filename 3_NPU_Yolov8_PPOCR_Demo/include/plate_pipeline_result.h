#ifndef PLATE_PIPELINE_RESULT_H
#define PLATE_PIPELINE_RESULT_H

#include <string>

struct PipelineResult {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    float confidence = 0.0f;
    float text_confidence = 0.0f;
    bool has_valid_plate_text = false;
    std::string plate_name;
    std::string plate_type;
    unsigned int box_color = 0;
    unsigned int text_color = 0;
};

#endif  // PLATE_PIPELINE_RESULT_H
