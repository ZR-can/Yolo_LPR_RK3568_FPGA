#include "plate_rule.h"

#include <cctype>
#include <vector>

namespace {

const char kRuleVersion[] = "ga36_plate_type_v3";
const char kProvinceAbbreviations[] =
    "京津沪渝冀豫云辽黑湘皖鲁新苏浙赣鄂桂甘晋蒙陕吉闽贵粤青藏川宁琼";
const char kAuthorityLetters[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
const char kSequenceLetters[] = "ABCDEFGHJKLMNPQRSTUVWXYZ";
const char kNewEnergyLetters[] = "ABCDEFGHJK";

std::vector<std::string> SplitUtf8(const std::string& text) {
    std::vector<std::string> characters;
    for (size_t offset = 0; offset < text.size();) {
        const unsigned char lead = static_cast<unsigned char>(text[offset]);
        size_t length = 1;
        if ((lead & 0xF8U) == 0xF0U) {
            length = 4;
        } else if ((lead & 0xF0U) == 0xE0U) {
            length = 3;
        } else if ((lead & 0xE0U) == 0xC0U) {
            length = 2;
        }
        if (offset + length > text.size()) {
            length = 1;
        }
        characters.push_back(text.substr(offset, length));
        offset += length;
    }
    return characters;
}

std::string JoinUtf8(const std::vector<std::string>& characters) {
    std::string joined;
    for (size_t i = 0; i < characters.size(); ++i) {
        joined += characters[i];
    }
    return joined;
}

std::string RemovePlateSeparators(const std::string& text) {
    const std::vector<std::string> characters = SplitUtf8(text);
    std::string normalized;
    for (size_t i = 0; i < characters.size(); ++i) {
        if (characters[i] == "·") {
            continue;
        }
        if (characters[i].size() == 1 &&
            std::isspace(static_cast<unsigned char>(characters[i][0])) != 0) {
            continue;
        }
        normalized += characters[i];
    }
    return normalized;
}

bool IsProvince(const std::string& character) {
    return character.size() == 3 &&
           std::string(kProvinceAbbreviations).find(character) != std::string::npos;
}

bool IsAsciiDigit(const std::string& character) {
    return character.size() == 1U && character[0] >= '0' && character[0] <= '9';
}

bool IsAuthorityLetter(const std::string& character) {
    return character.size() == 1U &&
           std::string(kAuthorityLetters).find(character) != std::string::npos;
}

bool IsSequenceLetter(const std::string& character) {
    return character.size() == 1U &&
           std::string(kSequenceLetters).find(character) != std::string::npos;
}

bool IsNewEnergyLetter(const std::string& character) {
    return character.size() == 1U &&
           std::string(kNewEnergyLetters).find(character) != std::string::npos;
}

void ConvertLetterToDigit(std::string* character) {
    if (*character == "O") {
        *character = "0";
    } else if (*character == "I") {
        *character = "1";
    }
}

void ConvertDigitToAuthorityLetter(std::string* character) {
    if (*character == "0") {
        *character = "O";
    } else if (*character == "1") {
        *character = "I";
    }
}

bool HasSupportedSpecialTail(const std::vector<std::string>& characters) {
    if (characters.empty()) {
        return false;
    }
    const std::string& tail = characters.back();
    return tail == "警" || tail == "学" || tail == "港" || tail == "澳" ||
           tail == "领" || tail == "使";
}

void ApplyPlateTypeCorrections(std::vector<std::string>* characters) {
    if (characters->empty()) {
        return;
    }

    if (characters->back() == "使") {
        for (size_t i = 0; i + 1 < characters->size(); ++i) {
            ConvertLetterToDigit(&(*characters)[i]);
        }
        return;
    }

    if (characters->back() == "领") {
        for (size_t i = 1; i + 1 < characters->size(); ++i) {
            ConvertLetterToDigit(&(*characters)[i]);
        }
        return;
    }

    if (characters->size() < 2U) {
        return;
    }
    ConvertDigitToAuthorityLetter(&(*characters)[1]);
    for (size_t i = 2; i < characters->size(); ++i) {
        ConvertLetterToDigit(&(*characters)[i]);
    }
}

bool IsValidSequence(const std::vector<std::string>& characters,
                     size_t begin,
                     size_t end,
                     size_t max_letters) {
    size_t letter_count = 0U;
    for (size_t i = begin; i < end; ++i) {
        if (IsSequenceLetter(characters[i])) {
            ++letter_count;
        } else if (!IsAsciiDigit(characters[i])) {
            return false;
        }
    }
    return letter_count <= max_letters;
}

bool IsValidEmbassyPlate(const std::vector<std::string>& characters) {
    if (characters.size() != 7U || characters.back() != "使") {
        return false;
    }
    for (size_t i = 0; i < 6U; ++i) {
        if (!IsAsciiDigit(characters[i])) {
            return false;
        }
    }
    return true;
}

bool IsValidConsularPlate(const std::vector<std::string>& characters) {
    if (characters.size() != 7U || characters.back() != "领" ||
        !IsProvince(characters[0])) {
        return false;
    }
    for (size_t i = 1; i <= 4U; ++i) {
        if (!IsAsciiDigit(characters[i])) {
            return false;
        }
    }
    return IsAsciiDigit(characters[5]) || IsSequenceLetter(characters[5]);
}

bool IsValidNewEnergyPlate(const std::vector<std::string>& characters) {
    if (characters.size() != 8U || !IsProvince(characters[0]) ||
        !IsAuthorityLetter(characters[1])) {
        return false;
    }

    // Small new-energy plate: the first sequence character is the energy
    // marker; the second may be one sequence letter; all remaining are digits.
    if (IsNewEnergyLetter(characters[2])) {
        if (!IsAsciiDigit(characters[3]) && !IsSequenceLetter(characters[3])) {
            return false;
        }
        for (size_t i = 4; i < 8U; ++i) {
            if (!IsAsciiDigit(characters[i])) {
                return false;
            }
        }
        return true;
    }

    // Large new-energy plate: the sixth sequence character is the energy
    // marker and the preceding five sequence characters are digits.
    if (!IsNewEnergyLetter(characters[7])) {
        return false;
    }
    for (size_t i = 2; i < 7U; ++i) {
        if (!IsAsciiDigit(characters[i])) {
            return false;
        }
    }
    return true;
}

void RestoreUnambiguousNewEnergyD(
    std::vector<std::string>* characters) {
    if (characters->size() < 8U) {
        return;
    }

    const std::vector<std::string> plate(characters->begin(),
                                         characters->begin() + 8U);
    if (IsValidNewEnergyPlate(plate)) {
        return;
    }

    bool is_small_plate_candidate = false;
    if (plate[2] == "0") {
        std::vector<std::string> candidate = plate;
        candidate[2] = "D";
        is_small_plate_candidate = IsValidNewEnergyPlate(candidate);
    }

    bool is_large_plate_candidate = false;
    if (plate[7] == "0") {
        std::vector<std::string> candidate = plate;
        candidate[7] = "D";
        is_large_plate_candidate = IsValidNewEnergyPlate(candidate);
    }

    if (is_small_plate_candidate == is_large_plate_candidate) {
        return;
    }
    (*characters)[is_small_plate_candidate ? 2U : 7U] = "D";
}

}  // namespace

const char* plate_rule_version() {
    return kRuleVersion;
}

std::string normalize_plate_prediction(const std::string& text) {
    const std::string normalized = RemovePlateSeparators(text);
    std::vector<std::string> characters = SplitUtf8(normalized);
    if (characters.size() != 7U && characters.size() != 8U) {
        return normalized;
    }
    const bool is_embassy_plate =
        characters.size() == 7U && characters.back() == "使";
    if (!is_embassy_plate &&
        (characters.empty() || !IsProvince(characters[0]))) {
        return normalized;
    }

    ApplyPlateTypeCorrections(&characters);
    return JoinUtf8(characters);
}

std::string correct_plate_prediction_for_pipeline(const std::string& text,
                                                  bool is_green_plate) {
    const std::string normalized = RemovePlateSeparators(text);
    std::vector<std::string> characters = SplitUtf8(normalized);
    const size_t expected_length = is_green_plate ? 8U : 7U;
    const bool is_embassy_plate = !characters.empty() && characters.back() == "使";
    const bool has_supported_prefix =
        is_embassy_plate || (!characters.empty() && IsProvince(characters[0]));
    if (characters.size() >= expected_length && has_supported_prefix) {
        ApplyPlateTypeCorrections(&characters);
    }

    if (is_green_plate && characters.size() >= expected_length &&
        !characters.empty() && IsProvince(characters[0])) {
        RestoreUnambiguousNewEnergyD(&characters);
    }
    return JoinUtf8(characters);
}

std::string truncate_plate_prediction(const std::string& text,
                                      bool is_green_plate) {
    std::vector<std::string> characters = SplitUtf8(text);
    const size_t target_length = is_green_plate ? 8U : 7U;
    if (characters.size() <= target_length) {
        return text;
    }

    std::string special_tail;
    const std::string& last = characters.back();
    if (HasSupportedSpecialTail(characters)) {
        special_tail = last;
        characters.pop_back();
    }

    const size_t body_length = target_length - (special_tail.empty() ? 0U : 1U);
    characters.resize(body_length);
    return JoinUtf8(characters) + special_tail;
}

bool is_valid_ga36_plate(const std::string& plate,
                         const std::string& plate_type) {
    const std::vector<std::string> characters = SplitUtf8(plate);
    const bool is_green_plate = plate_type == "绿";

    if (is_green_plate) {
        return IsValidNewEnergyPlate(characters);
    }
    if (characters.size() != 7U) {
        return false;
    }
    if (characters.back() == "使") {
        return IsValidEmbassyPlate(characters);
    }
    if (characters.back() == "领") {
        return IsValidConsularPlate(characters);
    }
    if (!IsProvince(characters[0]) || !IsAuthorityLetter(characters[1])) {
        return false;
    }

    const bool has_special_tail = characters.back() == "警" ||
                                  characters.back() == "学" ||
                                  characters.back() == "港" ||
                                  characters.back() == "澳";
    const size_t sequence_end = has_special_tail ? 6U : 7U;
    return IsValidSequence(characters, 2U, sequence_end, 2U);
}
