#ifndef PCIE_DEMO_BRIDGE_H_
#define PCIE_DEMO_BRIDGE_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct PcieUiFrame {
    std::shared_ptr<std::vector<unsigned char> > pixels;
    int width = 0;
    int height = 0;
    int stride = 0;
    int frame_id = -1;
};

struct PcieUiStatus {
    bool worker_alive = false;
    bool capturing = false;
    bool pcie_open = false;
    uint64_t start_ms = 0;
    uint64_t elapsed_ms = 0;
    uint64_t captured_frames = 0;
    uint64_t display_pipeline_frames = 0;
    uint64_t displayed_frames = 0;
    uint64_t inference_jobs = 0;
    uint64_t inference_failures = 0;
    uint64_t frame_pool_drops = 0;
    uint64_t display_queue_drops = 0;
    uint64_t inference_queue_drops = 0;
    double avg_end_to_end_ms = 0.0;
    unsigned int vendor_id = 0;
    unsigned int device_id = 0;
    unsigned int link_speed = 0;
    unsigned int link_width = 0;
    unsigned int max_payload_size = 0;
    std::string plate_text;
    std::string plate_type;
    float plate_confidence = 0.0f;
    std::string traffic_light_state;
    int traffic_person_count = 0;
    int traffic_person_id_total = 0;
    int traffic_persons_in_crosswalk = 0;
    int traffic_violation_count = 0;
    int traffic_violation_event_total = 0;
    std::string message;
};

struct PcieUiCallbacks {
    std::function<bool()> should_stop;
    std::function<bool()> capture_enabled;
    // Back-pressure hook used by the Qt frontend to reject frames before the
    // backend spends CPU on conversion and overlay for a frame it cannot paint.
    std::function<bool()> can_accept_frame;
    std::function<void(const PcieUiFrame&, const PcieUiStatus&)> on_frame;
    std::function<void(const PcieUiStatus&)> on_status;
};

int RunPcieDemo(const char* yolov8_model,
                const char* lprnet7_model,
                const char* lprnet8_model,
                const PcieUiCallbacks* callbacks);

int RunPpocrPcieDemo(const char* yolov8_model,
                     const char* ppocr_model,
                     const char* dictionary,
                     const PcieUiCallbacks* callbacks);

int RunPpocrPcieImageDemo(const char* yolov8_model,
                          const char* ppocr_model,
                          const char* dictionary,
                          const PcieUiCallbacks* callbacks);

#endif  // PCIE_DEMO_BRIDGE_H_
