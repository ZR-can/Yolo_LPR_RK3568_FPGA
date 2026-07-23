#include <cstdio>
#include <string>
#include <vector>

#include "plate_rule.h"
#include "simple_tracker.h"

namespace {

int Check(const char* name,
          const std::string& actual,
          const std::string& expected) {
    if (actual == expected) {
        return 0;
    }
    std::fprintf(stderr,
                 "%s failed: expected [%s], actual [%s]\n",
                 name,
                 expected.c_str(),
                 actual.c_str());
    return 1;
}

int CheckTrackerVote(const char* name,
                     const std::string& plate,
                     const std::string& plate_type,
                     bool expected_valid) {
    PipelineResult detection;
    detection.left = 100;
    detection.top = 100;
    detection.right = 200;
    detection.bottom = 140;
    detection.confidence = 0.9f;
    detection.text_confidence = 0.9f;
    detection.plate_name = plate;
    detection.plate_type = plate_type;

    const std::vector<PipelineResult> detections(1, detection);
    SimplePlateTracker tracker;
    tracker.update(detections, 0);
    tracker.update(detections, 1);

    std::vector<PipelineResult> tracked;
    tracker.predict(1, tracked);
    const bool actual_valid = !tracked.empty() &&
                              tracked[0].has_valid_plate_text &&
                              tracked[0].plate_name == plate;
    if (actual_valid == expected_valid) {
        return 0;
    }
    std::fprintf(stderr,
                 "%s failed: expected valid=%d, actual valid=%d\n",
                 name,
                 expected_valid ? 1 : 0,
                 actual_valid ? 1 : 0);
    return 1;
}

int CheckTrackerRawFallback(const char* name,
                            const std::string& plate,
                            const std::string& plate_type) {
    PipelineResult detection;
    detection.left = 100;
    detection.top = 100;
    detection.right = 200;
    detection.bottom = 140;
    detection.confidence = 0.9f;
    detection.text_confidence = 0.9f;
    detection.plate_name = plate;
    detection.plate_type = plate_type;

    const std::vector<PipelineResult> detections(1, detection);
    SimplePlateTracker tracker;
    tracker.update(detections, 0);

    std::vector<PipelineResult> tracked;
    tracker.predict(0, tracked);
    if (!tracked.empty()) {
        std::fprintf(stderr, "%s failed: raw text displayed before two hits\n", name);
        return 1;
    }

    tracker.update(detections, 1);
    tracker.predict(1, tracked);
    const bool has_raw_fallback = tracked.size() == 1U &&
                                  !tracked[0].has_valid_plate_text &&
                                  tracked[0].plate_name == plate;
    if (has_raw_fallback) {
        return 0;
    }
    std::fprintf(stderr,
                 "%s failed: expected repeated invalid text as RAW fallback\n",
                 name);
    return 1;
}

}  // namespace

int main() {
    int failures = 0;
    failures += Check("rule version",
                      plate_rule_version(),
                      "ga36_plate_type_v3");
    failures += Check("ordinary plate",
                      normalize_plate_prediction("京0A12O3"),
                      "京OA1203");
    failures += Check("authority I correction",
                      normalize_plate_prediction("京1A12I3"),
                      "京IA1213");
    failures += Check("consular plate",
                      normalize_plate_prediction("粤OI234领"),
                      "粤01234领");
    failures += Check("embassy plate",
                      normalize_plate_prediction("224O7I使"),
                      "224071使");
    failures += Check("invalid embassy length only cleans",
                      normalize_plate_prediction("224O7I8使"),
                      "224O7I8使");
    failures += Check("police authority correction",
                      normalize_plate_prediction("京0A12O警"),
                      "京OA120警");
    failures += Check("separator cleanup",
                      normalize_plate_prediction(" ·粤 0A12O3 "),
                      "粤OA1203");
    failures += Check("invalid length only cleans",
                      normalize_plate_prediction("京0A12O"),
                      "京0A12O");
    const std::string corrected_overlength =
        correct_plate_prediction_for_pipeline("京0A12OI34", false);
    failures += Check("overlength correction",
                      corrected_overlength,
                      "京OA120134");
    failures += Check("overlength correction then truncation",
                      truncate_plate_prediction(corrected_overlength, false),
                      "京OA1201");
    const std::string corrected_embassy_overlength =
        correct_plate_prediction_for_pipeline("224O7I9使", false);
    failures += Check("embassy overlength correction",
                      corrected_embassy_overlength,
                      "2240719使");
    failures += Check("embassy overlength correction then truncation",
                      truncate_plate_prediction(corrected_embassy_overlength,
                                                false),
                      "224071使");
    failures += Check("green third-position recovery",
                      correct_plate_prediction_for_pipeline("粤A012345", true),
                      "粤AD12345");
    failures += Check("green ambiguous zero remains unchanged",
                      correct_plate_prediction_for_pipeline("粤A012340", true),
                      "粤A012340");
    failures += Check("green last-position recovery",
                      correct_plate_prediction_for_pipeline("粤A123450", true),
                      "粤A12345D");
    failures += Check("green valid small ending zero unchanged",
                      correct_plate_prediction_for_pipeline("贵JDM1570", true),
                      "贵JDM1570");
    failures += Check("green valid large leading zero unchanged",
                      correct_plate_prediction_for_pipeline("豫X07199F", true),
                      "豫X07199F");
    failures += Check(
        "green overlength recovery before truncation",
        truncate_plate_prediction(
            correct_plate_prediction_for_pipeline("粤A123450X", true), true),
        "粤A12345D");
    failures += Check("ordinary truncation",
                      truncate_plate_prediction("京A1234567", false),
                      "京A12345");
    failures += Check("special-tail truncation",
                      truncate_plate_prediction("京A123456领", false),
                      "京A1234领");
    failures += Check("green truncation",
                      truncate_plate_prediction("粤AD123456", true),
                      "粤AD12345");
    failures += CheckTrackerVote("authority O is valid",
                                 "京OA1203",
                                 "蓝",
                                 true);
    failures += CheckTrackerVote("authority I is valid",
                                 "京IA1213",
                                 "蓝",
                                 true);
    failures += CheckTrackerVote("police authority letter",
                                 "京OA120警",
                                 "白",
                                 true);
    failures += CheckTrackerVote("police numeric authority rejected",
                                 "京0A120警",
                                 "白",
                                 false);
    failures += CheckTrackerVote("consular numeric institution code",
                                 "粤01234领",
                                 "黑",
                                 true);
    failures += CheckTrackerVote("consular optional sequence letter",
                                 "沪2247A领",
                                 "黑",
                                 true);
    failures += CheckTrackerVote("consular invalid institution letter",
                                 "沪22A78领",
                                 "黑",
                                 false);
    failures += CheckTrackerVote("embassy numeric structure",
                                 "224578使",
                                 "黑",
                                 true);
    failures += CheckTrackerVote("embassy rejects letters",
                                 "22457A使",
                                 "黑",
                                 false);
    failures += CheckTrackerVote("ordinary two sequence letters",
                                 "京AAB123",
                                 "蓝",
                                 true);
    failures += CheckTrackerVote("ordinary three sequence letters",
                                 "京AABC12",
                                 "蓝",
                                 false);
    failures += CheckTrackerRawFallback("repeated invalid raw display",
                                       "湘VWUJ3N",
                                       "蓝");
    failures += CheckTrackerVote("ordinary sequence O rejected",
                                 "京AA12O3",
                                 "蓝",
                                 false);
    failures += CheckTrackerVote("special tail two sequence letters",
                                 "京AAB12警",
                                 "白",
                                 true);
    failures += CheckTrackerVote("special tail three sequence letters",
                                 "京AABC1警",
                                 "白",
                                 false);
    failures += CheckTrackerVote("small new energy A with second letter",
                                 "粤AAB1234",
                                 "绿",
                                 true);
    const std::string energy_letters = "ABCDEFGHJK";
    for (size_t i = 0; i < energy_letters.size(); ++i) {
        const std::string marker(1U, energy_letters[i]);
        const std::string small_name = "small new energy marker " + marker;
        const std::string large_name = "large new energy marker " + marker;
        failures += CheckTrackerVote(small_name.c_str(),
                                     "粤A" + marker + "12345",
                                     "绿",
                                     true);
        failures += CheckTrackerVote(large_name.c_str(),
                                     "粤A12345" + marker,
                                     "绿",
                                     true);
    }
    failures += CheckTrackerVote("new energy missing marker",
                                 "粤A123456",
                                 "绿",
                                 false);
    failures += CheckTrackerVote("small new energy misplaced letter",
                                 "粤AD12A45",
                                 "绿",
                                 false);
    failures += CheckTrackerVote("small new energy sequence I rejected",
                                 "粤ADI2345",
                                 "绿",
                                 false);
    failures += CheckTrackerVote("large new energy misplaced letter",
                                 "粤A12A45D",
                                 "绿",
                                 false);
    failures += CheckTrackerVote("unsupported trailer tail",
                                 "京A1234挂",
                                 "黄",
                                 false);

    if (failures == 0) {
        std::printf("plate_rule_test: all cases passed (%s)\n",
                    plate_rule_version());
    }
    return failures == 0 ? 0 : 1;
}
