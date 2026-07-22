#include "traffic_temporal_tracker.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr int kLightVoteWindow = 5;
constexpr int kLightMajorityVotes = 3;
constexpr int kLightStateHoldVotes = 2;
constexpr int kLightBoxHoldFrames = 5;
constexpr int kPersonMaxMissedFrames = 8;
constexpr float kPersonBoxSmoothing = 0.65f;
constexpr float kMinimumMatchIou = 0.05f;
constexpr float kMaximumCenterDistance = 0.75f;

int Clamp(int value, int low, int high) {
    return std::max(low, std::min(value, high));
}

float BoxArea(const image_rect_t& box) {
    return static_cast<float>(std::max(0, box.right - box.left) *
                              std::max(0, box.bottom - box.top));
}

float BoxIou(const image_rect_t& first, const image_rect_t& second) {
    image_rect_t intersection;
    intersection.left = std::max(first.left, second.left);
    intersection.top = std::max(first.top, second.top);
    intersection.right = std::min(first.right, second.right);
    intersection.bottom = std::min(first.bottom, second.bottom);
    const float intersection_area = BoxArea(intersection);
    const float union_area = BoxArea(first) + BoxArea(second) - intersection_area;
    return union_area > 0.0f ? intersection_area / union_area : 0.0f;
}

float CenterDistance(const image_rect_t& first, const image_rect_t& second) {
    const float first_x = (first.left + first.right) * 0.5f;
    const float first_y = (first.top + first.bottom) * 0.5f;
    const float second_x = (second.left + second.right) * 0.5f;
    const float second_y = (second.top + second.bottom) * 0.5f;
    const float dx = first_x - second_x;
    const float dy = first_y - second_y;
    const float scale_x = static_cast<float>(
        std::max(std::max(1, first.right - first.left),
                 std::max(1, second.right - second.left)));
    const float scale_y = static_cast<float>(
        std::max(std::max(1, first.bottom - first.top),
                 std::max(1, second.bottom - second.top)));
    const float scale = std::sqrt(scale_x * scale_x + scale_y * scale_y);
    return scale > 0.0f ? std::sqrt(dx * dx + dy * dy) / scale : 1.0f;
}

image_rect_t SmoothBox(const image_rect_t& previous,
                       const image_rect_t& current,
                       int width,
                       int height) {
    const float old_weight = 1.0f - kPersonBoxSmoothing;
    image_rect_t box;
    box.left = Clamp(static_cast<int>(std::lround(
                         previous.left * old_weight + current.left * kPersonBoxSmoothing)),
                     0, width - 1);
    box.top = Clamp(static_cast<int>(std::lround(
                        previous.top * old_weight + current.top * kPersonBoxSmoothing)),
                    0, height - 1);
    box.right = Clamp(static_cast<int>(std::lround(
                          previous.right * old_weight + current.right * kPersonBoxSmoothing)),
                      box.left + 1, width);
    box.bottom = Clamp(static_cast<int>(std::lround(
                           previous.bottom * old_weight + current.bottom * kPersonBoxSmoothing)),
                       box.top + 1, height);
    return box;
}

struct MatchCandidate {
    int track_index;
    int detection_index;
    float score;
};

}  // namespace

int TrafficTemporalTracker::Init(const TrafficRoiConfig& roi) {
    if (roi.points.size() < 3) {
        return -1;
    }
    roi_ = roi;
    Reset();
    return 0;
}

void TrafficTemporalTracker::Reset() {
    person_tracks_.clear();
    light_votes_.clear();
    stable_light_ = TRAFFIC_LIGHT_UNKNOWN;
    cached_light_ = TrafficDetectionState();
    has_cached_light_ = false;
    cached_light_missed_frames_ = 0;
    next_track_id_ = 1;
    violation_event_total_ = 0;
}

TrafficLightState TrafficTemporalTracker::UpdateLightVote(TrafficLightState instant_state) {
    light_votes_.push_back(instant_state);
    while (light_votes_.size() > kLightVoteWindow) {
        light_votes_.pop_front();
    }

    int red_votes = 0;
    int green_votes = 0;
    for (TrafficLightState vote : light_votes_) {
        if (vote == TRAFFIC_LIGHT_RED) {
            ++red_votes;
        } else if (vote == TRAFFIC_LIGHT_GREEN) {
            ++green_votes;
        }
    }

    if (red_votes >= kLightMajorityVotes && red_votes > green_votes) {
        stable_light_ = TRAFFIC_LIGHT_RED;
    } else if (green_votes >= kLightMajorityVotes && green_votes > red_votes) {
        stable_light_ = TRAFFIC_LIGHT_GREEN;
    } else {
        const int stable_votes = stable_light_ == TRAFFIC_LIGHT_RED
                                     ? red_votes
                                     : (stable_light_ == TRAFFIC_LIGHT_GREEN ? green_votes : 0);
        if (stable_votes < kLightStateHoldVotes) {
            stable_light_ = TRAFFIC_LIGHT_UNKNOWN;
        }
    }
    return stable_light_;
}

int TrafficTemporalTracker::Update(const TrafficFrameAnalysis& instant,
                                   TrafficFrameAnalysis* tracked) {
    if (tracked == nullptr || instant.source_width <= 0 || instant.source_height <= 0 ||
        roi_.points.size() < 3) {
        return -1;
    }

    TrafficFrameAnalysis next = instant;
    next.light.instant_state = instant.light.instant_state;
    next.light.state = UpdateLightVote(instant.light.instant_state);
    next.detections.clear();
    next.person_count = 0;
    next.traffic_light_count = instant.traffic_light_count;
    next.persons_in_crosswalk = 0;
    next.violation_count = 0;
    next.violation_event_count = 0;
    next.violation_event_total = violation_event_total_;

    bool current_selected_light = false;
    for (const TrafficDetectionState& detection : instant.detections) {
        if (detection.detection.cls_id != kTrafficLightClassId) {
            continue;
        }
        TrafficDetectionState current = detection;
        current.predicted = false;
        next.detections.push_back(current);
        if (current.selected_light) {
            cached_light_ = current;
            has_cached_light_ = true;
            cached_light_missed_frames_ = 0;
            current_selected_light = true;
        }
    }
    if (!current_selected_light && has_cached_light_) {
        ++cached_light_missed_frames_;
        if (cached_light_missed_frames_ <= kLightBoxHoldFrames) {
            TrafficDetectionState held_light = cached_light_;
            held_light.predicted = true;
            held_light.selected_light = true;
            next.detections.push_back(held_light);
        } else {
            has_cached_light_ = false;
        }
    }

    std::vector<object_detect_result> persons;
    for (const TrafficDetectionState& detection : instant.detections) {
        if (detection.detection.cls_id == kTrafficPersonClassId) {
            persons.push_back(detection.detection);
        }
    }

    std::vector<MatchCandidate> candidates;
    for (size_t track_index = 0; track_index < person_tracks_.size(); ++track_index) {
        person_tracks_[track_index].matched = false;
        for (size_t detection_index = 0; detection_index < persons.size(); ++detection_index) {
            const float iou = BoxIou(person_tracks_[track_index].detection.box,
                                     persons[detection_index].box);
            const float distance = CenterDistance(person_tracks_[track_index].detection.box,
                                                  persons[detection_index].box);
            if (iou < kMinimumMatchIou && distance > kMaximumCenterDistance) {
                continue;
            }
            MatchCandidate candidate;
            candidate.track_index = static_cast<int>(track_index);
            candidate.detection_index = static_cast<int>(detection_index);
            candidate.score = iou * 0.8f + std::max(0.0f, 1.0f - distance) * 0.2f;
            candidates.push_back(candidate);
        }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const MatchCandidate& first, const MatchCandidate& second) {
                  return first.score > second.score;
              });

    std::vector<bool> matched_tracks(person_tracks_.size(), false);
    std::vector<bool> matched_detections(persons.size(), false);
    for (const MatchCandidate& candidate : candidates) {
        if (matched_tracks[candidate.track_index] ||
            matched_detections[candidate.detection_index]) {
            continue;
        }
        PersonTrack& track = person_tracks_[candidate.track_index];
        const object_detect_result& detection = persons[candidate.detection_index];
        track.detection.box = SmoothBox(track.detection.box, detection.box,
                                        instant.source_width, instant.source_height);
        track.detection.prop = track.detection.prop * (1.0f - kPersonBoxSmoothing) +
                               detection.prop * kPersonBoxSmoothing;
        track.detection.cls_id = kTrafficPersonClassId;
        track.missed_frames = 0;
        track.matched = true;
        matched_tracks[candidate.track_index] = true;
        matched_detections[candidate.detection_index] = true;
    }

    for (size_t i = 0; i < person_tracks_.size(); ++i) {
        if (!matched_tracks[i]) {
            ++person_tracks_[i].missed_frames;
            person_tracks_[i].matched = false;
        }
    }
    for (size_t i = 0; i < persons.size(); ++i) {
        if (matched_detections[i]) {
            continue;
        }
        PersonTrack track;
        track.id = next_track_id_++;
        track.detection = persons[i];
        track.detection.cls_id = kTrafficPersonClassId;
        track.missed_frames = 0;
        track.matched = true;
        person_tracks_.push_back(track);
    }
    person_tracks_.erase(
        std::remove_if(person_tracks_.begin(), person_tracks_.end(),
                       [](const PersonTrack& track) {
                           return track.missed_frames > kPersonMaxMissedFrames;
                       }),
        person_tracks_.end());

    for (PersonTrack& track : person_tracks_) {
        TrafficDetectionState state;
        state.detection = track.detection;
        state.track_id = track.id;
        state.predicted = !track.matched;
        state.bottom_in_crosswalk = traffic_box_bottom_in_roi(
            track.detection.box, roi_, instant.source_width, instant.source_height);
        state.violation = state.bottom_in_crosswalk &&
                          next.light.state == TRAFFIC_LIGHT_RED;
        if (state.violation && !track.violation_counted && track.matched) {
            state.violation_event = true;
            track.violation_counted = true;
            ++next.violation_event_count;
            ++violation_event_total_;
        }
        ++next.person_count;
        if (state.bottom_in_crosswalk) {
            ++next.persons_in_crosswalk;
        }
        if (state.violation) {
            ++next.violation_count;
        }
        next.detections.push_back(state);
    }
    next.violation_event_total = violation_event_total_;
    next.person_id_total = next_track_id_ - 1;

    *tracked = next;
    return 0;
}
