/**
 * pinyin_composition.cpp
 * 拼音组合与候选准备实现文件。
 * 负责将拆分结果转为展示文本，并预留候选元素聚合入口。
 */
#include "pinyin_composition.h"
#include "pinyin_split.h"
#include "word_matcher.h"
#include "util.h"
#include "pinyin_matcher.h"
#include "pinyin_data.h"
#include "pinyin_file_io.h"
#include <algorithm>
#include <ctime>
#include <sstream>
#include <unordered_map>

/**
 * 获取智能拼接短语候选（预留接口）。
 *
 * @param split_options 多套拼音拆分方案。
 * @return 智能拼接得到的候选元素数组（当前留空）。
 */
static std::vector<CandidateItem> getSmartPhraseCandidateElements(
    const std::vector<std::vector<std::string>>& split_options) {
    (void)split_options;
    return {};
}

// ---- literal（字面量）前缀支持 ----
// 拆分方案开头若含非纯小写字母段（大写/数字/符号/含撇号段），
// 匹配时把前缀原样拼到候选文本前，保证混合输入（如 Hao、2hao、xi'an）
// 上屏内容完整，字面量部分不会被丢弃。

static bool is_lowercase_token(const std::string& token) {
    if (token.empty()) return false;
    return std::all_of(token.begin(), token.end(), [](unsigned char ch) {
        return ch >= 'a' && ch <= 'z';
    });
}

static bool is_literal_token(const std::string& token) {
    return !token.empty() && !is_lowercase_token(token);
}

static std::vector<std::string> get_leading_literal_parts(const std::vector<std::string>& parts) {
    std::vector<std::string> prefix;
    for (const auto& part : parts) {
        if (!is_literal_token(part)) break;
        prefix.push_back(part);
    }
    return prefix;
}

static std::string join_literal_parts(const std::vector<std::string>& parts) {
    std::string prefix;
    for (const auto& part : parts) prefix += part;
    return prefix;
}

struct SplitContext {
    std::vector<std::string> literal_prefix_parts;
    std::vector<std::vector<std::string>> active_options;

    bool has_literal_prefix() const {
        return !literal_prefix_parts.empty();
    }
};

static std::vector<std::vector<std::string>> strip_leading_literal_parts(
    const std::vector<std::vector<std::string>>& split_options) {
    std::vector<std::vector<std::string>> stripped;
    stripped.reserve(split_options.size());
    for (const auto& option : split_options) {
        size_t start = 0;
        while (start < option.size() && is_literal_token(option[start])) ++start;
        if (start < option.size()) {
            stripped.emplace_back(option.begin() + static_cast<std::ptrdiff_t>(start), option.end());
        }
    }
    return stripped;
}

static SplitContext build_split_context(const std::vector<std::vector<std::string>>& split_options) {
    SplitContext context;
    if (split_options.empty()) return context;

    context.literal_prefix_parts = get_leading_literal_parts(split_options[0]);
    if (context.has_literal_prefix()) {
        context.active_options = strip_leading_literal_parts(split_options);
    } else {
        context.active_options = split_options;
    }
    return context;
}

static CandidateItem prefix_candidate(const CandidateItem& item, const std::vector<std::string>& literal_parts) {
    if (literal_parts.empty()) return item;
    std::vector<std::string> merged_parts = literal_parts;
    const auto& base_parts = item.getPinyinParts();
    merged_parts.insert(merged_parts.end(), base_parts.begin(), base_parts.end());
    return CandidateItem(merged_parts,
                         join_literal_parts(literal_parts) + item.getText(),
                         item.getTimestamp(),
                         item.getCount());
}

static std::vector<CandidateItem> prefix_candidates(const std::vector<CandidateItem>& items,
                                                    const std::vector<std::string>& literal_parts) {
    if (literal_parts.empty()) return items;
    std::vector<CandidateItem> prefixed;
    prefixed.reserve(items.size());
    for (const auto& item : items) {
        prefixed.push_back(prefix_candidate(item, literal_parts));
    }
    return prefixed;
}

/**
 * 获取所有合适的字候选（预留接口）。
 *
 * @param split_options 多套拼音拆分方案。
 * @return 合适的单字候选元素数组（当前留空）。
 */
static std::vector<CandidateItem> getAllSuitableCharElements(
    const std::vector<std::vector<std::string>>& split_options) {
    if (split_options.empty() || split_options[0].empty()) {
        return {};
    }

    std::vector<CandidateItem> candidates;
    std::string charPinyin = split_options[0][0];
    std::vector<int> match_lines;

    // 尝试完全匹配
    int exact_match = find_exact_match_char(charPinyin);
    if (exact_match > 0) {
        match_lines.push_back(exact_match);
    } else {
        // 完全匹配失败，降级为前缀匹配
        match_lines = find_prefix_match_char(charPinyin);
    }

    // 解析匹配到的行，构造候选元素
    for (int line_index : match_lines) {
        // 行号转换为数组下标（0-based）
        int array_index = line_index - 1;
        if (array_index >= 0 && array_index < (int)g_pinyin_map_lines.size()) {
            const std::string& line = g_pinyin_map_lines[array_index];
            size_t space_pos = line.find(' ');
            if (space_pos != std::string::npos) {
                std::string pinyin_part = line.substr(0, space_pos);
                std::string chars_part = line.substr(space_pos + 1);

                // 将后面的字符串按 UTF-8 字符拆分开
                for (size_t i = 0; i < chars_part.length();) {
                    unsigned char c = (unsigned char)chars_part[i];
                    size_t step = 1;
                    if ((c & 0xE0) == 0xC0) step = 2;
                    else if ((c & 0xF0) == 0xE0) step = 3;
                    else if ((c & 0xF8) == 0xF0) step = 4;

                    if (i + step > chars_part.length()) {
                        step = chars_part.length() - i; // 防止越界
                    }

                    std::string single_char = chars_part.substr(i, step);
                    
                    // 构造候选元素对象：从单字库查找表取真实 ts/count（O(1)）。
                    long long timestamp = 0;
                    int count = 1;
                    {
                        auto freq_it = g_char_freq_lookup.find(pinyin_part + " " + single_char);
                        if (freq_it != g_char_freq_lookup.end()) {
                            timestamp = freq_it->second.timestamp;
                            count = freq_it->second.count;
                        }
                    }
                    CandidateItem item({pinyin_part}, single_char, timestamp, count);
                    candidates.push_back(item);
                    
                    i += step;
                }
            }
        }
    }

    long long now = (long long)time(nullptr);
    std::stable_sort(candidates.begin(), candidates.end(),
                     [now](const CandidateItem& a, const CandidateItem& b) {
                         long long sa = a.computeScore(now);
                         long long sb = b.computeScore(now);
                         if (sa != sb) return sa > sb;
                         return a.getTimestamp() > b.getTimestamp();
                     });
    return candidates;
}

/**
 * 拼接任意数量的候选元素数组并返回一维结果。
 *
 * @param arrays 候选元素数组集合。
 * @return 拼接后的一维候选元素数组。
 */
static std::vector<CandidateItem> concatCandidateElementArrays(
    std::initializer_list<std::vector<CandidateItem>> arrays) {
    std::vector<CandidateItem> merged;
    size_t total_size = 0;
    for (const auto& arr : arrays) total_size += arr.size();
    merged.reserve(total_size);
    for (const auto& arr : arrays) {
        merged.insert(merged.end(), arr.begin(), arr.end());
    }
    return merged;
}

/**
 * 模拟 consume_buf：统计选中候选后输入缓冲被实际消耗的字母数。
 * 逐字母匹配：候选拼音段与缓冲从头部比对，相同则双方前进一位（消耗 +1）；
 * 段首字母不匹配则丢弃该段；缓冲中的 ' 分隔符跳过不计。
 * 与 ime_controller 的 consume_pinyin_buffer_by_selected_candidate 逻辑一致，
 * 作为排序的"消耗字母数"键。
 */
static size_t count_consumed_letters(const std::string& buf,
                                     const std::vector<std::string>& parts) {
    size_t consumed = 0;
    size_t b = 0;      // 缓冲游标
    size_t pi = 0;     // 段下标
    size_t pc = 0;     // 段内游标
    while (pi < parts.size() && b < buf.size()) {
        if (buf[b] == '\'') { ++b; continue; }
        if (pc >= parts[pi].size()) { ++pi; pc = 0; continue; }
        if (parts[pi][pc] == buf[b]) {
            ++consumed; ++pc; ++b;
        } else {
            ++pi; pc = 0;  // 段首/段中不匹配：丢弃整段
        }
    }
    return consumed;
}

/**
 * 候选去重：key = 拼音CSV + 文本。
 * 重复时保留 computeScore 更高者。
 */
static std::vector<CandidateItem> dedupe_candidates(const std::vector<CandidateItem>& candidates) {
    std::vector<CandidateItem> deduped;
    deduped.reserve(candidates.size());
    std::unordered_map<std::string, size_t> seen;
    long long now = (long long)time(nullptr);
    for (const auto& item : candidates) {
        std::string key = join_csv(item.getPinyinParts()) + "||" + item.getText();
        auto it = seen.find(key);
        if (it == seen.end()) {
            seen[key] = deduped.size();
            deduped.push_back(item);
        } else if (item.computeScore(now) > deduped[it->second].computeScore(now)) {
            deduped[it->second] = item;
        }
    }
    return deduped;
}

/**
 * 获取完整的候选项列表并按页拆分为二维数组。
 * 该函数整合了不同维度的候选结果，并根据 candidate_page_size 进行分页。
 *
 * 排序：所有候选（词 + 单字）统一进入一个池，按
 *   1. 消耗字母数（候选拼音与当前缓冲逐字母比对，选中后真正吃掉的输入
 *      字母数）降序 —— 不按完整拼音长度，"西安"(xi,an) 与 "先"(xian)
 *      消耗相同字母数即同等排序，拼音/字数更多不会天然占优；
 *   2. 权重分数降序；
 *   3. 时间戳降序。
 *
 * @param split_options 输入的多种拼音拆分方案。
 * @param candidate_page_size 每页显示的候选词数量。
 * @param pinyin_buffer 当前拼音输入缓冲（用于计算消耗字母数）。
 * @return 包含所有分页候选词的二维 CandidateItem 数组。
 */
std::vector<std::vector<CandidateItem>> getAllCandidateElements(
    const std::vector<std::vector<std::string>>& split_options,
    size_t candidate_page_size,
    const std::string& pinyin_buffer) {
    SplitContext context = build_split_context(split_options);

    std::vector<CandidateItem> smart_phrase_candidates =
        getSmartPhraseCandidateElements(split_options);
    std::vector<CandidateItem> suitable_word_candidates =
        getAllSuitableWords(context.active_options);
    std::vector<CandidateItem> suitable_char_candidates =
        getAllSuitableCharElements(context.active_options);

    std::vector<CandidateItem> supplemental_candidates;
    if (context.has_literal_prefix()) {
        std::vector<CandidateItem> prefixed_word_candidates =
            prefix_candidates(getAllSuitableWords(context.active_options), context.literal_prefix_parts);
        std::vector<CandidateItem> prefixed_char_candidates =
            prefix_candidates(suitable_char_candidates, context.literal_prefix_parts);
        supplemental_candidates = concatCandidateElementArrays(
            {prefixed_word_candidates, prefixed_char_candidates});
    }

    std::vector<CandidateItem> flat_candidates = concatCandidateElementArrays(
        {smart_phrase_candidates, suitable_word_candidates, suitable_char_candidates, supplemental_candidates});
    flat_candidates = dedupe_candidates(flat_candidates);

    long long now = (long long)time(nullptr);
    std::stable_sort(flat_candidates.begin(), flat_candidates.end(),
        [now, &pinyin_buffer](const CandidateItem& a, const CandidateItem& b) {
            size_t ca = count_consumed_letters(pinyin_buffer, a.getPinyinParts());
            size_t cb = count_consumed_letters(pinyin_buffer, b.getPinyinParts());
            if (ca != cb) return ca > cb;
            long long sa = a.computeScore(now);
            long long sb = b.computeScore(now);
            if (sa != sb) return sa > sb;
            return a.getTimestamp() > b.getTimestamp();
        });

    std::vector<std::vector<CandidateItem>> paged_candidates;
    if (flat_candidates.empty() || candidate_page_size == 0) {
        return paged_candidates;
    }

    size_t current_index = 0;
    while (current_index < flat_candidates.size()) {
        std::vector<CandidateItem> current_page;
        int count_to_take = static_cast<int>(std::min(candidate_page_size, flat_candidates.size() - current_index));
        for (int i = 0; i < count_to_take; ++i) {
            current_page.push_back(flat_candidates[current_index++]);
        }
        paged_candidates.push_back(current_page);
    }

    return paged_candidates;
}

/**
 * 构建用于状态栏显示的最终文本字符串。
 * 格式示例："pinyin|¹拼音 ²便宜"
 * 
 * 主要逻辑包括：
 * 1. 调用保守拆分算法获取拼音的显示形式（带逗号分隔）。
 * 2. 根据原始缓冲区的光标位置，在显示串中插入 "|" 符号表示虚拟光标。
 * 3. 根据当前页码从分页后的候选列表中提取对应的候选词数组进行展示。
 * 4. 为每个候选词配上上标数字序号（1-9 对应 ¹-⁹，10 对应 ⁰）。
 *
 * @param raw_buffer 用户输入的原始拼音字符串（不含分隔符）。
 * @param virtual_cursor 基于 raw_buffer 的光标下标位置。
 * @param paged_candidates 已经分好页的候选词二维数组。
 * @param candidate_page 当前显示的候选词页码（对应二维数组的第一层下标）。
 * @return 格式化后的状态栏显示字符串。
 */
static std::string buildDisplayFromSplitOptions(
    const std::string& raw_buffer,
    size_t virtual_cursor,
    const std::vector<std::vector<CandidateItem>>& paged_candidates,
    size_t candidate_page) {
    // 1. 调用拼音拆分核心算法，获取带逗号分隔符的拼音显示串
    // 例如：输入 "pinyin" -> 返回 "pin,yin"
    std::string display = conservativePinyinSplitMain(raw_buffer);

    // 安全检查：确保光标位置不超出原始输入缓冲区的长度
    if (virtual_cursor > raw_buffer.size()) virtual_cursor = raw_buffer.size();

    // 2. 将“基于原始缓冲区”的光标位置映射到“带逗号分隔符”的显示串中
    // 逻辑：遍历显示串，跳过自动生成的逗号，直到匹配到原始光标所在的字符数
    size_t display_pos = 0;
    size_t raw_count = 0;
    while (display_pos < display.size() && raw_count < virtual_cursor) {
        if (display[display_pos] != ',') {
            ++raw_count;
        }
        ++display_pos;
    }

    // 3. 在映射后的位置插入虚拟光标符号 "|"
    // 例如："pin,yin" 且光标在末尾 -> "pin,yin|"
    std::string pinyin_display = display.substr(0, display_pos) + "|" + display.substr(display_pos);

    // 如果没有候选项，则直接返回拼音部分
    if (paged_candidates.empty()) {
        return pinyin_display;
    }

    // 安全检查：确保页码不越界
    if (candidate_page >= paged_candidates.size()) {
        candidate_page = paged_candidates.size() - 1;
    }

    // 提取当前页需要显示的候选项数组
    const std::vector<CandidateItem>& current_page_candidates = paged_candidates[candidate_page];
    
    // 定义数字上标字符数组 (0-9 对应 ⁰-⁹)
    static const char* superscripts[] = {"⁰", "¹", "²", "³", "⁴", "⁵", "⁶", "⁷", "⁸", "⁹"};
    std::string candidate_display = "";

    // 6. 遍历当前页的候选项，构建带上标序号的展示文本
    for (size_t i = 0; i < current_page_candidates.size(); ++i) {
        size_t num = i + 1; // 当前页内的序号 (1, 2, 3...)
        
        // 将序号转换为上标数字显示
        if (num < 10) {
            candidate_display += superscripts[num]; // 1-9 使用对应上标
        } else if (num == 10) {
            candidate_display += superscripts[0];   // 10 使用上标 ⁰
        }
        
        // 拼接候选项文本
        candidate_display += current_page_candidates[i].getText();
        
        // 候选项之间添加空格分隔
        if (i + 1 < current_page_candidates.size()) candidate_display += " ";
    }

    // 7. 合并拼音部分和候选词部分并返回最终结果
    return pinyin_display + candidate_display;
}

/**
 * 输入法预编辑文本构建入口。
 * 流程：
 * 1) 对当前缓冲执行拆分；
 * 2) 拉取所有候选并按页拆分为二维数组；
 * 3) 根据当前页码返回用于状态栏展示的文本。
 *
 * @param pinyin_buffer 当前拼音输入缓冲。
 * @param virtual_cursor 虚拟光标位置（基于原始缓冲，范围 0..length）。
 * @param candidate_page 当前请求显示的页码。
 * @param candidate_page_size 每页候选数量。
 * @param out_paged_candidates 若非空，则通过此参数传回全部分页后的候选。
 * @return 状态栏展示文本。
 */
std::string buildComposingDisplayText(const std::string& pinyin_buffer,
                                      size_t virtual_cursor,
                                      size_t candidate_page,
                                      size_t candidate_page_size,
                                      std::vector<std::vector<CandidateItem>>* out_paged_candidates) {
    if (pinyin_buffer.empty()) return "";
    
    // 1. 获取拆分方案
    std::vector<std::vector<std::string>> split_options = splitConservativePinyin(pinyin_buffer);
    
    // 2. 获取所有候选项并按页拆分
    std::vector<std::vector<CandidateItem>> paged_candidates = getAllCandidateElements(split_options, candidate_page_size, pinyin_buffer);
    
    // 3. 如果需要，返回分页后的候选列表给调用方（用于翻页逻辑控制）
    if (out_paged_candidates) {
        *out_paged_candidates = paged_candidates;
    }
    
    // 4. 构建并返回展示文本
    return buildDisplayFromSplitOptions(pinyin_buffer, virtual_cursor, paged_candidates, candidate_page);
}
