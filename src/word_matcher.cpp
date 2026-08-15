/**
 * word_matcher.cpp
 * 词语匹配模块实现文件。
 */
#include "word_matcher.h"
#include "pinyin_matcher.h"
#include "util.h"
#include <algorithm>
#include <ctime>
#include <unordered_map>

// 轮内范围池：key = 段组合 CSV（如 "a,b"），value = 该段的宽松匹配行号范围。
// 快照复用：一次候选重建中，后段的检索范围复用它所有前缀段组合已算出的范围，
// 避免每套拆分方案都从首字母索引重新全区间扫描。
using RangePool = std::unordered_map<std::string, std::pair<int, int>>;

/**
 * 在池中查找当前段组合的最长前缀范围。
 * key 是 current_csv 的字符串前缀（前面完全一样），取最长的 key。
 * 找不到返回 {-1, -1}（调用方用首字母索引兜底）。
 */
static std::pair<int, int> lookup_pool(const RangePool& pool, const std::string& current_csv) {
    int best_len = -1;
    std::pair<int, int> best = {-1, -1};
    for (const auto& kv : pool) {
        if ((int)kv.first.size() > best_len &&
            current_csv.compare(0, kv.first.size(), kv.first) == 0) {
            best_len = (int)kv.first.size();
            best = kv.second;
        }
    }
    return best;
}

/**
 * 获取一套拼音分段对应的候选词数组。
 * 从最短分段（2 段）开始逐步加长：
 * - 第一层用首字母索引兜底锁定初始范围；后续层复用范围池中最长前缀的范围；
 * - 在检索范围内做宽松+严格一次扫描（scan_in_range）；
 * - 宽松范围为空 ⇒ 更长段组合必然也不存在，停止；
 * - 前缀存在但当前 len 无严格匹配，继续尝试更长段
 *   （可能有"无短词但有长词"的情况，如只有 西安八桥 没有 西安）。
 */
static std::vector<CandidateItem> getSuitableWordLineIndexes(
    const std::vector<std::string>& pinyin_parts, RangePool& range_pool) {
    std::vector<CandidateItem> result;

    for (size_t len = 2; len <= pinyin_parts.size(); ++len) {
        std::vector<std::string> current_parts(
            pinyin_parts.begin(), pinyin_parts.begin() + static_cast<std::ptrdiff_t>(len));
        std::string current_csv = join_csv(current_parts);

        // 检索范围：池中最长前缀；池无命中则首字母索引兜底。
        auto range = lookup_pool(range_pool, current_csv);
        if (range.first == -1) {
            range = initial_range(current_parts[0]);
        }

        ScanResult sr = scan_in_range(current_parts, range.first, range.second);

        // 宽松范围为空 ⇒ 更长段组合必然也不存在，停止。
        if (sr.loose_lo == -1) break;

        // 存池：当前段组合的精确宽松范围。
        range_pool[current_csv] = {sr.loose_lo, sr.loose_hi};

        for (int line_number : sr.strict_lines) {
            result.push_back(CandidateItem::fromWordDictLineNumber(line_number));
        }
    }

    return result;
}

/**
 * 构建候选去重键：拼音CSV + 文本。
 */
static std::string candidate_key(const CandidateItem& item) {
    std::string key = join_csv(item.getPinyinParts());
    key += "||";
    key += item.getText();
    return key;
}

/**
 * 收集并去重一组拆分方案产生的候选词。
 * 范围池在此创建（一次 rebuild 内跨方案共享公共前缀），函数结束自动析构清空。
 * 重复项保留 computeScore 更高者。
 */
static std::vector<CandidateItem> collectAndDedupeWords(
    const std::vector<std::vector<std::string>>& split_options) {

    RangePool range_pool;

    std::vector<CandidateItem> all;
    for (const auto& pinyin_parts : split_options) {
        auto matched = getSuitableWordLineIndexes(pinyin_parts, range_pool);
        all.insert(all.end(), matched.begin(), matched.end());
    }

    std::vector<CandidateItem> deduped;
    deduped.reserve(all.size());
    std::unordered_map<std::string, size_t> seen;
    long long now = (long long)time(nullptr);
    for (const auto& item : all) {
        std::string key = candidate_key(item);
        auto it = seen.find(key);
        if (it == seen.end()) {
            seen[key] = deduped.size();
            deduped.push_back(item);
        } else {
            if (item.computeScore(now) > deduped[it->second].computeScore(now)) {
                deduped[it->second] = item;
            }
        }
    }
    return deduped;
}

/**
 * 获取所有合适的词语。
 * 流程：收集 → 去重 → 返回。
 * 排序在 pinyin_composition 统一完成（消耗字母数 → 分数 → 时间戳），
 * 此处不再排序，避免与单字候选的混排顺序被破坏。
 */
std::vector<CandidateItem> getAllSuitableWords(
    const std::vector<std::vector<std::string>>& split_options) {

    return collectAndDedupeWords(split_options);
}
