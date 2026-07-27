#ifndef PLATE_RULE_H
#define PLATE_RULE_H

#include <string>

const char* plate_rule_version();

// Cleans a complete 7/8-character prediction and applies GA 36 positional
// I/O corrections without deployment truncation.
std::string normalize_plate_prediction(const std::string& text);

// Applies the same position-aware corrections to complete or overlength
// deployment results, then restores a green-plate 0->D marker only when
// exactly one small/large new-energy structure is valid.
std::string correct_plate_prediction_for_pipeline(const std::string& text,
                                                  bool is_green_plate);

// Keeps the existing pipeline's class-dependent length truncation as a
// separate stage after character correction.
std::string truncate_plate_prediction(const std::string& text,
                                      bool is_green_plate);

// Validates the supported GA 36 plate structures. The project intentionally
// excludes the 挂/试/超 plate types.
bool is_valid_ga36_plate(const std::string& plate,
                         const std::string& plate_type);

#endif  // PLATE_RULE_H
