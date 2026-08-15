/**
 * pinyin_matcher.cpp
 * 拼音匹配模块实现文件。
 * 负责基于拼音键在单字库/词库缓存中执行完整匹配与前缀匹配。
 */
#include "pinyin_matcher.h"
#include "pinyin_data.h"
#include "pinyin_file_io.h"
#include "util.h"
#include <algorithm>
#include <cstring>
#include <utility>

/**
 * 拼音匹配模块实现
 */

/**
 * 以逗号分割拼音串。
 *
 * @param pinyin_csv 逗号分隔的拼音字符串（如 "jiu,nei,rong"）。
 * @return 分割后的拼音段数组。
 */
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

/**
 * 最小化智能匹配：
 * - exact_flags[i] == 1: 原拼音必须与目标拼音完全相等；
 * - exact_flags[i] == 0: 原拼音可与目标拼音相等，或为目标拼音前缀。
 *
 * @param source_parts 原拼音分段数组。
 * @param exact_flags 逐位精确匹配标记（1/0）。
 * @param target_parts 目标词条拼音分段数组。
 * @return 匹配是否通过。
 */
static bool minimal_smart_match(const std::vector<std::string>& source_parts,
                                const std::vector<int>& exact_flags,
                                const std::vector<std::string>& target_parts) {
    if (source_parts.size() != exact_flags.size() || source_parts.size() != target_parts.size()) {
        write_log("minimal_smart_match parameter length mismatch", ERROR);
        return false;
    }

    for (size_t i = 0; i < source_parts.size(); ++i) {
        const std::string& source = source_parts[i];
        const std::string& target = target_parts[i];
        if (exact_flags[i] == 1) {
            if (source != target) return false;
        } else {
            bool equal = (source == target);
            bool is_prefix = target.compare(0, source.size(), source) == 0;
            if (!equal && !is_prefix) return false;
        }
    }
    return true;
}

int find_exact_match_char(const std::string& pinyin, bool reject_iuv_as_initial) {
    /**
     * 在单字库中进行“完整匹配 + 前缀判定”：
     * - 返回 >0：完整命中行号（1-based）
     * - 返回 -1：无完整命中，但存在前缀
     * - 返回 -2：连前缀都不存在
     */
    if (pinyin.empty()) return -2;

    // 1. 查找索引，定位起始字符
    char first_char = pinyin[0];
    if (reject_iuv_as_initial && (first_char == 'i' || first_char == 'u' || first_char == 'v')) {
        return -2;
    }
    int start_line = -1, end_line = -1;

    for (const auto& item : g_pinyin_map_index) {
        if (item.start_char == (int)first_char) {
            start_line = item.start_line;
            end_line = item.end_line;
            break;
        }
    }

    if (start_line == -1) return -2; // 连首字母索引都找不到，肯定返回 -2

    bool has_prefix = false;
    // 2. 在指定行范围内进行搜索
    for (int i = start_line - 1; i < end_line && i < (int)g_pinyin_map_lines.size(); ++i) {
        std::string current_pinyin = get_pinyin_from_line(g_pinyin_map_lines[i]);
        
        // 检查是否完全匹配
        if (current_pinyin == pinyin) {
            return i + 1; // 找到完整匹配
        }
        
        // 如果还没找到完整匹配，检查是否是前缀
        if (!has_prefix && current_pinyin.compare(0, pinyin.length(), pinyin) == 0) {
            has_prefix = true;
        }
    }

    return has_prefix ? -1 : -2;
}

std::vector<int> find_prefix_match_char(const std::string& pinyin) {
    /**
     * 在单字库中执行前缀搜索，返回所有命中的真实行号（1-based）。
     */
    std::vector<int> results;
    if (pinyin.empty()) return results;

    // 1. 查找索引，定位起始字符
    char first_char = pinyin[0];
    int start_line = -1, end_line = -1;

    for (const auto& item : g_pinyin_map_index) {
        if (item.start_char == (int)first_char) {
            start_line = item.start_line;
            end_line = item.end_line;
            break;
        }
    }

    if (start_line == -1) return results;

    // 2. 在指定行范围内进行前缀搜索
    for (int i = start_line - 1; i < end_line && i < (int)g_pinyin_map_lines.size(); ++i) {
        std::string current_pinyin = get_pinyin_from_line(g_pinyin_map_lines[i]);
        // 检查 current_pinyin 是否以输入的 pinyin 开头
        if (current_pinyin.compare(0, pinyin.length(), pinyin) == 0) {
            results.push_back(i + 1); // 返回 1-based 真实行号
        }
    }

    return results;
}

ScanResult scan_in_range(const std::vector<std::string>& pinyin_parts,
                         int range_lo, int range_hi) {
    ScanResult r;
    if (pinyin_parts.empty()) return r;

    size_t len = pinyin_parts.size();

    // 严格匹配 flag：单字母段不要求精确（如 "a" 可匹配 ai/an/... 前缀）；
    // 多字母段若能完整命中拼音则要求精确，否则允许前缀。
    std::vector<int> exact_match_flags;
    exact_match_flags.reserve(len);
    for (const auto& part : pinyin_parts) {
        if (part.size() == 1) {
            exact_match_flags.push_back(0);
            continue;
        }
        int match_result = find_exact_match_char(part, false);
        exact_match_flags.push_back(match_result > 0 ? 1 : 0);
    }

    if (range_hi > (int)g_user_dict_lines.size()) range_hi = (int)g_user_dict_lines.size();
    for (int line_num = range_lo; line_num <= range_hi; ++line_num) {
        int idx = line_num - 1;
        if (idx < 0) continue;

        // 行拼音段访问统一入口：优先 mmap parts cache（快），cache 失效时按需 split。
        const char* p = get_parts_cached_line(idx);
        std::vector<std::string> split_target;
        auto get_seg = [&](int i, size_t& tlen) -> const char* {
            if (p) {
                int l = 0;
                const char* s = get_parts_cached_seg(p, i, l);
                tlen = (l > 0) ? (size_t)l : 0;
                return s;
            }
            if (i >= (int)split_target.size()) { tlen = 0; return nullptr; }
            tlen = split_target[i].size();
            return split_target[i].data();
        };
        uint8_t seg_count = 0;
        if (p) {
            seg_count = get_parts_cached_seg_count(p);
        } else {
            split_target = split_pinyin_csv(get_pinyin_from_line(g_user_dict_lines[idx]));
            seg_count = (uint8_t)split_target.size();
        }

        // 宽松：逐段前缀（不限段数），记录本段精确范围。
        if (seg_count >= len) {
            bool loose = true;
            for (size_t i = 0; i < len; ++i) {
                size_t tlen = 0;
                const char* tseg = get_seg((int)i, tlen);
                const std::string& src = pinyin_parts[i];
                if (!tseg || tlen < src.size() ||
                    memcmp(tseg, src.data(), src.size()) != 0) {
                    loose = false;
                    break;
                }
            }
            if (loose) {
                if (r.loose_lo == -1) r.loose_lo = line_num;
                r.loose_hi = line_num;
            }
        }
        // 严格：段数恰好相等 + 精确规则。
        if (seg_count == len) {
            std::vector<std::string> target;
            target.reserve(len);
            for (size_t i = 0; i < len; ++i) {
                size_t tlen = 0;
                const char* tseg = get_seg((int)i, tlen);
                target.emplace_back(tseg ? tseg : "", tseg ? tlen : 0);
            }
            if (minimal_smart_match(pinyin_parts, exact_match_flags, target)) {
                r.strict_lines.push_back(line_num);
            }
        }
    }
    return r;
}

std::pair<int, int> initial_range(const std::string& first_segment) {
    if (first_segment.empty()) return {1, (int)g_user_dict_lines.size()};
    char first_char = first_segment[0];
    for (const auto& item : g_user_dict_index) {
        if (item.start_char == (int)first_char) {
            return {item.start_line, item.end_line};
        }
    }
    return {1, (int)g_user_dict_lines.size()};
}
