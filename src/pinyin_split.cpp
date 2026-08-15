/**
 * pinyin_split.cpp
 * 拼音拆分算法实现文件。
 * 包含激进/保守拆分策略，以及拆分结果合并与最终输出流程。
 */
#include "pinyin_split.h"
#include "pinyin_data.h"
#include "pinyin_matcher.h"
#include "util.h"
#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>
#include <unordered_set>

static bool is_lowercase_pinyin_segment(const std::string& input) {
    if (input.empty()) return false;
    return std::all_of(input.begin(), input.end(), [](unsigned char ch) {
        return ch >= 'a' && ch <= 'z';
    });
}

std::string aggressivePinyinSplit(const std::string& input);
std::string conservativePinyinSplit(const std::string& input);

static bool is_uppercase_literal_char(unsigned char ch) {
    return ch >= 'A' && ch <= 'Z';
}

static std::vector<std::string> split_mixed_segment(const std::string& segment, bool aggressive) {
    std::vector<std::string> tokens;
    if (segment.empty()) return tokens;

    size_t i = 0;
    while (i < segment.size()) {
        size_t j = i;
        if (is_lowercase_pinyin_segment(std::string(1, segment[i]))) {
            while (j < segment.size() && segment[j] >= 'a' && segment[j] <= 'z') ++j;
            std::string lowercase_run = segment.substr(i, j - i);
            std::string split = aggressive ? aggressivePinyinSplit(lowercase_run)
                                           : conservativePinyinSplit(lowercase_run);
            std::vector<std::string> parts = split_csv(split);
            tokens.insert(tokens.end(), parts.begin(), parts.end());
        } else if (is_uppercase_literal_char((unsigned char)segment[i])) {
            while (j < segment.size() && is_uppercase_literal_char((unsigned char)segment[j])) ++j;
            tokens.push_back(segment.substr(i, j - i));
        } else {
            while (j < segment.size() &&
                   !(segment[j] >= 'a' && segment[j] <= 'z') &&
                   !is_uppercase_literal_char((unsigned char)segment[j])) {
                ++j;
            }
            tokens.push_back(segment.substr(i, j - i));
        }
        i = j;
    }

    return tokens;
}

// =============================================================================
// 2. 数据结构：SimpleStack (栈)
// =============================================================================

/**
 * 专为拼音拆分逻辑设计的轻量级栈结构
 */
class SimpleStack {
public:
    std::vector<int> items;

    SimpleStack() {}

    /** 入栈 */
    void push(int element) {
        items.push_back(element);
    }

    /** 出栈并返回顶层元素 */
    int pop() {
        if (isEmpty()) return -1;
        int val = items.back();
        items.pop_back();
        return val;
    }

    /** 查看顶层元素 */
    int peek() const {
        return isEmpty() ? -1 : items.back();
    }

    /** 查看第二层元素 */
    int peekSecond() const {
        return items.size() < 2 ? -1 : items[items.size() - 2];
    }

    /** 检查是否为空 */
    bool isEmpty() const {
        return items.empty();
    }

    /** 创建当前栈的深度副本 */
    SimpleStack clone() const {
        SimpleStack newStack;
        newStack.items = items;
        return newStack;
    }

    /** 从副本恢复栈状态 */
    void restore(const SimpleStack& otherStack) {
        items = otherStack.items;
    }
};

// =============================================================================
// 4. 算法辅助工具函数
// =============================================================================

/** 字符串插值助手 */
std::string insertAfter(const std::string& str, int idx, char c) {
    if (idx < 0 || idx >= (int)str.length()) return str;
    return str.substr(0, idx + 1) + c + str.substr(idx + 1);
}

/** 后移/前移栈顶逗号位置 */
void moveComma(SimpleStack& stack) {
    stack.push(stack.pop() + 1);
}
void moveCommaBack(SimpleStack& stack) {
    stack.push(stack.pop() - 1);
}

/**
 * 回溯逻辑：记录当前快照并尝试调整上一个断点
 * 用于解决贪婪匹配导致的死胡同
 */
SimpleStack backtrackComma(SimpleStack& commaStack) {
    if (commaStack.peekSecond() == -1) {
        commaStack.push(commaStack.peek() + 1);
        return commaStack;
    }
    SimpleStack snapshot = commaStack.clone();
    commaStack.pop(); // 移除当前失败断点
    int last = commaStack.pop();
    commaStack.push(last + 1); // 将上一个断点后移一位
    return snapshot;
}

/**
 * 根据逗号栈生成最终的拆分字符串
 */
std::string buildSplitResult(const SimpleStack& commaStack, const std::string& input) {
    std::string res = input;
    // 从后往前插入，确保下标在插入过程中保持有效
    for (int i = (int)commaStack.items.size() - 1; i > 0; i--) {
        int pos = commaStack.items[i];
        if (pos >= 0 && pos < (int)res.length() - 1) {
            res = insertAfter(res, pos, ',');
        }
    }
    // 清理首尾多余逗号 (C++ 实现中通常不会出现首尾逗号，除非逻辑有误)
    return res;
}

// =============================================================================
// 5. 核心算法：激进拆分策略 (Aggressive)
// =============================================================================

/** 激进拆分核心 */
std::string aggressivePinyinSplit(const std::string& input) {
    if (input.empty()) return "";
    SimpleStack stack;
    stack.push(-1); stack.push(0);
    
    bool isBacktracked = false;
    SimpleStack snapshot;

    size_t max_iter = input.length() * input.length() + 100;
    size_t iter = 0;
    while (true) {
        if (++iter > max_iter) break;
        int start = stack.peekSecond() + 1;
        int len = stack.peek() - stack.peekSecond();
        if (start >= (int)input.length()) break;
        std::string segment = input.substr(start, len);
        if (segment.empty()) break;

        int status = find_exact_match_char(segment);

        if (status == -2) { // 彻底无匹配
            if (isBacktracked) { // 已回溯过则强制跳过当前位置
                stack.restore(snapshot);
                stack.push(stack.peek() + 1);
                isBacktracked = false;
                continue;
            }
            if (len == 1) { // 单字符无匹配（如 v/q 等非声母音节）：直接跳过该字符
                stack.push(stack.peek() + 1);
                continue;
            }
            if (stack.peekSecond() == -1) {
                moveCommaBack(stack); stack.push(stack.peek() + 1);
                continue;
            }
            snapshot = backtrackComma(stack);
            moveCommaBack(snapshot);
            isBacktracked = true;
        } else if (status == -1) { // 仅前缀匹配，继续延长
            if (stack.peek() == (int)input.length() - 1) { // 到底了还没凑成完整拼音
                if (isBacktracked) {
                    stack.restore(snapshot); stack.push(stack.peek() + 1);
                    isBacktracked = false; continue;
                }
                snapshot = backtrackComma(stack);
                isBacktracked = true;
            } else {
                moveComma(stack);
            }
        } else { // 匹配成功，标记断点并开始下一段
            isBacktracked = false;
            stack.push(stack.peek() + 1);
        }
    }
    return buildSplitResult(stack, input);
}

/** 激进拆分主入口 (处理带 ' 的混合输入) */
std::string aggressivePinyinSplitMain(const std::string& raw) {
    if (raw.empty()) return "";
    std::string result;
    std::stringstream ss(raw);
    std::string segment;
    bool first = true;
    while (std::getline(ss, segment, '\'')) {
        if (!first) result += "'";
        result += join_csv(split_mixed_segment(segment, true));
        first = false;
    }
    return result;
}

// =============================================================================
// 6. 核心算法：保守拆分策略 (Conservative)
// =============================================================================

/** 保守拆分核心 */
std::string conservativePinyinSplit(const std::string& input) {
    if (input.empty()) return "";
    SimpleStack stack;
    stack.push(-1); stack.push(0);

    size_t max_iter = input.length() * input.length() + 100;
    size_t iter = 0;
    while (true) {
        if (++iter > max_iter) break;
        int start = stack.peekSecond() + 1;
        int len = stack.peek() - stack.peekSecond();
        if (start >= (int)input.length() || stack.peek() >= (int)input.length()) break;
        std::string segment = input.substr(start, len);
        if (segment.empty()) break;

        int status = find_exact_match_char(segment, true);
        if (status == -2) { // 彻底无匹配
            // 单字母且连前缀都匹配不到：尝试与前一块合并后再判定
            if ((int)segment.length() == 1) {
                SimpleStack snapshot = stack.clone();

                // 前移上一个逗号（左移一位）后重试前缀匹配
                if (stack.items.size() >= 3) {
                    int last_comma = stack.pop();
                    int prev_comma = stack.peek();
                    int prev_prev_comma = stack.peekSecond();
                    if (prev_comma > prev_prev_comma) {
                        stack.pop();
                        stack.push(prev_comma - 1);
                    }
                    stack.push(last_comma);

                    int merged_start = stack.peekSecond() + 1;
                    int merged_len = stack.peek() - stack.peekSecond();
                    if (merged_start >= 0 &&
                        merged_len > 0 &&
                        merged_start + merged_len <= (int)input.length()) {
                        std::string merged_segment = input.substr(merged_start, merged_len);
                        int merged_status = find_exact_match_char(merged_segment, true);
                        if (merged_status != -2) {
                            // 合并后可匹配（完整或前缀），继续向后尝试
                            moveComma(stack);
                            continue;
                        }
                    }
                }

                // 合并仍失败：恢复快照，并直接在当前字母后继续前进
                stack.restore(snapshot);
                stack.push(stack.peek() + 1);
                continue;
            }

            moveCommaBack(stack);
            stack.push(stack.peek() + 1);
        } else { // 无论完全匹配还是前缀匹配，都继续向后尝试
            moveComma(stack);
        }
    }
    return buildSplitResult(stack, input);
}

/** 保守拆分主入口 */
std::string conservativePinyinSplitMain(const std::string& raw) {
    if (raw.empty()) return "";
    std::string result;
    std::stringstream ss(raw);
    std::string segment;
    bool first = true;
    while (std::getline(ss, segment, '\'')) {
        if (!first) result += "'";
        result += join_csv(split_mixed_segment(segment, false));
        first = false;
    }
    return result;
}

// =============================================================================
// 7. 合并逻辑与最终输出
// =============================================================================

/**
 * 将初步拆分的拼音进行两轮合并
 */
std::vector<std::string> mergeConservativePinyin(const std::string& input) {
    if (input.empty()) return {};

    auto performMerge = [](const std::vector<std::string>& arr) {
        std::vector<std::string> merged;
        for (size_t i = 0; i < arr.size(); i++) {
            if (i < arr.size() - 1) {
                std::string combined = arr[i] + arr[i + 1];
                if (find_exact_match_char(combined) != -2) {
                    merged.push_back(combined); i++; // 成功合并，跳过下一位
                } else {
                    merged.push_back(arr[i]);
                }
            } else {
                merged.push_back(arr[i]);
            }
        }
        return merged;
    };

    // 先按手动拆分符(')切段：每段内部允许合并，段与段之间禁止跨界合并。
    std::vector<std::string> all_parts;
    std::stringstream segment_ss(input);
    std::string apostrophe_segment;
    while (std::getline(segment_ss, apostrophe_segment, '\'')) {
        // 每段内部按系统分隔符(,)切分
        std::vector<std::string> parts;
        std::stringstream part_ss(apostrophe_segment);
        std::string part;
        while (std::getline(part_ss, part, ',')) {
            if (!part.empty()) parts.push_back(part);
        }
        if (parts.empty()) continue;

        // 每个手动段内部执行两轮合并（如 t-i-a -> tia）
        parts = performMerge(parts);
        parts = performMerge(parts);

        all_parts.insert(all_parts.end(), parts.begin(), parts.end());
    }
    return all_parts;
}

/**
 * 总拆分导出函数
 */
SplitResult splitConservativePinyinEx(const std::string& input) {
    SplitResult empty;
    if (input.empty()) return empty;
    
    // 1. 获取保守拆分并合并后的结果
    std::string cons = conservativePinyinSplitMain(input);
    std::vector<std::string> consArray;
    std::stringstream ss1(cons);
    std::string p;
    while (std::getline(ss1, p, ',')) {
        if (!p.empty()) consArray.push_back(p);
    }
    
    // 2. 获取激进拆分并合并后的结果
    std::string aggr = aggressivePinyinSplitMain(input);
    std::vector<std::string> aggrMerged = mergeConservativePinyin(aggr);
    
    // 3. 日志记录
    std::string aggr_str = aggr;
    std::string cons_str = cons;
    std::string merged_str = "";
    for (size_t i = 0; i < aggrMerged.size(); ++i) {
        merged_str += aggrMerged[i];
        if (i < aggrMerged.size() - 1) merged_str += ",";
    }

    write_log("Pinyin Split Process for [" + input + "]:", DEBUG);
    write_log("  - Aggressive Raw: [" + aggr_str + "]", DEBUG);
    write_log("  - Conservative:   [" + cons_str + "]", DEBUG);
    write_log("  - Merged (Aggr):  [" + merged_str + "]", DEBUG);
    
    // 4. 结果去重比较
    auto normalize_token = [](std::string s) {
        s.erase(std::remove(s.begin(), s.end(), '\''), s.end());
        return s;
    };

    // 激进原始拆分（未做相邻合并），供"相邻段可拼成合法音节但仍按
    // 未合并方式匹配"的场景使用（如 xi,an 不打撇号也能候选到 西安）。
    std::vector<std::string> aggrRaw;
    {
        std::stringstream ss(aggr);
        std::string p;
        while (std::getline(ss, p, ',')) {
            if (!p.empty()) aggrRaw.push_back(p);
        }
    }

    bool equal = (aggrMerged.size() == consArray.size());
    if (equal) {
        for (size_t i = 0; i < aggrMerged.size(); i++) {
            if (normalize_token(aggrMerged[i]) != normalize_token(consArray[i])) {
                equal = false;
                break;
            }
        }
    }

    // 三套方案：激进合并 / 激进原始 / 保守。三者互相去重，完全相同者归一。
    // （激进原始不再强制保留——若与其它方案完全相同，保留它没有意义。）
    std::vector<std::vector<std::string>> finalResults;
    finalResults.push_back(aggrMerged);
    finalResults.push_back(aggrRaw);
    if (!equal) {
        finalResults.push_back(consArray);
    }
    {
        std::vector<std::vector<std::string>> deduped;
        for (const auto& opt : finalResults) {
            bool dup = false;
            for (const auto& existing : deduped) {
                if (existing.size() == opt.size()) {
                    bool same = true;
                    for (size_t i = 0; i < opt.size(); i++) {
                        if (normalize_token(existing[i]) != normalize_token(opt[i])) {
                            same = false;
                            break;
                        }
                    }
                    if (same) { dup = true; break; }
                }
            }
            if (!dup) deduped.push_back(opt);
        }
        finalResults.swap(deduped);
    }

    // 先归一化最终方案（逗号统一为单引号并按单引号拆分重组）
    std::vector<std::string> normalized_option_strings;
    normalized_option_strings.reserve(finalResults.size());
    for (size_t i = 0; i < finalResults.size(); ++i) {
        std::vector<std::string> normalized_parts;
        for (const auto& token : finalResults[i]) {
            std::string normalized = token;
            std::replace(normalized.begin(), normalized.end(), ',', '\'');
            std::stringstream ss(normalized);
            std::string part;
            while (std::getline(ss, part, '\'')) {
                if (!part.empty()) normalized_parts.push_back(part);
            }
        }

        std::string opt_str = "";
        for (size_t j = 0; j < normalized_parts.size(); ++j) {
            opt_str += normalized_parts[j];
            if (j < normalized_parts.size() - 1) opt_str += "'";
        }
        normalized_option_strings.push_back(opt_str);
    }

    // 归一化后去重，再统一打印，避免日志中重复项
    std::vector<std::string> unique_option_strings;
    std::unordered_set<std::string> seen_options;
    for (const auto& option_str : normalized_option_strings) {
        if (seen_options.insert(option_str).second) {
            unique_option_strings.push_back(option_str);
        }
    }
    for (size_t i = 0; i < unique_option_strings.size(); ++i) {
        write_log("  - Final Option [" + std::to_string(i) + "]: [" + unique_option_strings[i] + "]", DEBUG);
    }
    
    SplitResult result;
    result.options = std::move(finalResults);
    return result;
}

std::vector<std::vector<std::string>> splitConservativePinyin(const std::string& input) {
    return splitConservativePinyinEx(input).options;
}
