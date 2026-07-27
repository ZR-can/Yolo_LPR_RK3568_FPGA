#ifndef TRAFFIC_TEMPORAL_TRACKER_H
#define TRAFFIC_TEMPORAL_TRACKER_H

#include <deque>
#include <vector>

#include "traffic_violation.h"

class TrafficTemporalTracker {
public:
    int Init(const TrafficRoiConfig& roi);
    void Reset();
    int Update(const TrafficFrameAnalysis& instant, TrafficFrameAnalysis* tracked);

private:
    struct PersonTrack {
        int id = -1;
        object_detect_result detection;
        int missed_frames = 0;
        bool matched = false;
        bool violation_counted = false;
    };

    TrafficLightState UpdateLightVote(TrafficLightState instant_state);

    TrafficRoiConfig roi_;
    std::vector<PersonTrack> person_tracks_;
    std::deque<TrafficLightState> light_votes_;
    TrafficLightState stable_light_ = TRAFFIC_LIGHT_UNKNOWN;
    int next_track_id_ = 1;
    int violation_event_total_ = 0;
};

#endif  // TRAFFIC_TEMPORAL_TRACKER_H
