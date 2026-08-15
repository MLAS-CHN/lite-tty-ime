/**
 * candidate_item.cpp
 * 候选元素构建逻辑实现文件。
 */
#include "candidate_item.h"
#include "pinyin_data.h"
#include "pinyin_file_io.h"
#include "util.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <utility>

static std::vector<std::string> split_pinyin_csv(const std::string& pinyin_csv) {
    std::vector<std::string> parts;
    std::string current;
    for (char c : pinyin_csv) {
        if (c == ',') {
            if (!current.empty()) parts.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) parts.push_back(current);
    return parts;
}

static std::string build_pinyin_csv_from_parts(const std::vector<std::string>& parts) {
    std::string res;
    for (size_t i = 0; i < parts.size(); ++i) {
        res += parts[i];
        if (i + 1 < parts.size()) res += ",";
    }
    return res;
}

std::string CandidateItem::toString() const {
    std::string pinyin_csv = build_pinyin_csv_from_parts(pinyin_parts_);
    return pinyin_csv + " " + text_ + " " + std::to_string(timestamp_) + " " + std::to_string(count_);
}

std::string CandidateItem::getSourceFileName() const {
    if (pinyin_parts_.size() == 1) {
        return get_char_freq_file_path();
    }
    return get_user_dict_file_path();
}

int CandidateItem::findSourceLineNumber() const {
    std::string pinyin_csv = build_pinyin_csv_from_parts(pinyin_parts_);
    std::string key = pinyin_csv + " " + text_;

    if (pinyin_parts_.size() == 1) {
        // 单字：O(1) 查找表。
        auto it = g_char_freq_lookup.find(key);
        if (it != g_char_freq_lookup.end()) return it->second.line_number;
        return -1;
    }

    // 词库按整行排序（拼音 文本 ts count），用二分查找"拼音 文本"前缀。
    std::string file_path = get_user_dict_file_path();
    std::string target = key;
    auto it = std::lower_bound(g_user_dict_lines.begin(), g_user_dict_lines.end(), target);
    if (it != g_user_dict_lines.end() &&
        it->compare(0, target.size(), target) == 0 &&
        (it->size() == target.size() || (*it)[target.size()] == ' ')) {
        return (int)std::distance(g_user_dict_lines.begin(), it) + 1;
    }
    return -1;
}

CandidateItem CandidateItem::fromCharDictLineNumber(int line_number) {
    // 预留：后续按字库格式解析并构建。
    (void)line_number;
    return CandidateItem();
}

CandidateItem CandidateItem::fromWordDictLineNumber(int line_number) {
    if (line_number <= 0 || line_number > (int)g_user_dict_lines.size()) {
        return CandidateItem();
    }

    const std::string& line = g_user_dict_lines[(size_t)line_number - 1];
    std::istringstream iss(line);

    std::string pinyin_csv;
    std::string text;
    std::string ts_str;
    std::string count_str;
    long long timestamp = 0;
    int count = 1;

    if (!(iss >> pinyin_csv >> text)) {
        return CandidateItem();
    }

    // 兼容三种行格式：
    // 4 列：pinyin_csv text timestamp count
    // 3 列：pinyin_csv text count
    // 2 列：pinyin_csv text
    if (iss >> ts_str) {
        if (iss >> count_str) {
            try {
                timestamp = std::stoll(ts_str);
                count = std::stoi(count_str);
            } catch (...) {
                timestamp = 0;
                count = 1;
            }
        } else {
            try {
                count = std::stoi(ts_str);
            } catch (...) {
                count = 1;
            }
        }
    }

    std::vector<std::string> pinyin_parts = split_pinyin_csv(pinyin_csv);
    return CandidateItem(pinyin_parts, text, timestamp, count);
}

CandidateItem CandidateItem::mergeCandidateItems(const std::vector<CandidateItem>& items) {
    if (items.empty()) return CandidateItem();

    std::vector<std::string> merged_pinyin_parts;
    std::string merged_text;
    for (const auto& item : items) {
        const auto& parts = item.getPinyinParts();
        merged_pinyin_parts.insert(merged_pinyin_parts.end(), parts.begin(), parts.end());
        merged_text += item.getText();
    }

    if (merged_pinyin_parts.empty() || merged_text.empty()) return CandidateItem();
    long long now = (long long)time(nullptr);
    return CandidateItem(merged_pinyin_parts, merged_text, now, 1);
}

void CandidateItem::quickSortByTimestampDesc(std::vector<CandidateItem>& candidates) {
    std::sort(candidates.begin(), candidates.end(),
              [](const CandidateItem& a, const CandidateItem& b) {
                  return a.getTimestamp() > b.getTimestamp();
              });
}
