/**
 * pinyin_matcher.h
 * 拼音匹配模块接口声明。
 * 对外提供单字/词组的完整匹配与前缀匹配函数。
 */
#ifndef PINYIN_MATCHER_H
#define PINYIN_MATCHER_H

#include <string>
#include <vector>

/**
 * 拼音匹配模块
 * 负责在字库和词库中搜索拼音匹配项。
 */

// --- 单字匹配 (Character Matching) ---

/**
 * 匹配全拼音字（一字不差）
 * 返回值：
 *   > 0 : 字库中的真实行号（1-based）
 *   -1  : 未找到完整匹配，但存在以此为前缀的拼音
 *   -2  : 连前缀都匹配不到
 */
int find_exact_match_char(const std::string& pinyin, bool reject_iuv_as_initial = false);

/**
 * 匹配前缀拼音字
 */
std::vector<int> find_prefix_match_char(const std::string& pinyin);


// --- 词语匹配 (Word Matching) ---

/**
 * 区间扫描结果：宽松匹配范围 + 严格命中行。
 */
struct ScanResult {
    int loose_lo = -1, loose_hi = -1;  // 本段在检索范围内的精确宽松范围（1-based）
    std::vector<int> strict_lines;     // 段数恰好相等 + 精确规则命中的行号
};

/**
 * 在 [range_lo, range_hi] 区间内对一段拼音做单次扫描：
 * - 宽松：逐段前缀（如 c 匹配 ci/cha/...，不限段数），记录本段精确范围；
 * - 严格：段数恰好相等 + 逐段精确/前缀，收集候选行号。
 * 宽松范围为空（loose_lo == -1）⇒ 更长段组合必然也不存在（前缀剪枝）。
 */
ScanResult scan_in_range(const std::vector<std::string>& pinyin_parts,
                         int range_lo, int range_hi);

/**
 * 首字母索引兜底：根据首段的首字符返回其在词库中的行号块 [lo, hi]（1-based）。
 * 找不到对应字母块时返回全库范围 {1, 总行数}。
 */
std::pair<int, int> initial_range(const std::string& first_segment);

#endif // PINYIN_MATCHER_H
