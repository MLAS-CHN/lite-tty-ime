#ifndef PINYIN_H
#define PINYIN_H

#include <string>
#include <unordered_set>
#include <vector>

/**
 * 激进拆分拼音：尽可能短的分段，优先使用合法音节
 * @param normalizedInput 归一化后的拼音输入（小写）
 * @param mapSyllables 合法音节集合
 * @return 拆分后的拼音字符串
 */
std::string aggressiveSplitPinyin(const std::string &normalizedInput,
                                  const std::unordered_set<std::string> &mapSyllables);

/**
 * 保守拆分拼音：尽可能长的分段，不轻易分割
 * @param normalizedInput 归一化后的拼音输入（小写）
 * @param mapSyllables 合法音节集合
 * @return 拆分后的拼音字符串
 */
std::string conservativeSplitPinyin(const std::string &normalizedInput,
                                    const std::unordered_set<std::string> &mapSyllables);

/**
 * 合并拆分：尝试合并相邻的音节
 * @param radStr 输入字符串（可能包含逗号或撇号分隔符）
 * @param mapSyllables 合法音节集合
 * @return 合并后的拼音字符串列表
 */
std::vector<std::string> mergeSplitPinyin(const std::string &radStr,
                                          const std::unordered_set<std::string> &mapSyllables);

/**
 * 计算分段拼音输入：按"激进 -> 保守 -> 合并"流程处理
 * @param rawInput 原始拼音输入
 * @param mapSyllables 合法音节集合
 * @return 去重后的拼音字符串列表
 */
std::vector<std::string> computeSegmentedPinyinInputs(
    const std::string &rawInput,
    const std::unordered_set<std::string> &mapSyllables);

#endif // PINYIN_H
