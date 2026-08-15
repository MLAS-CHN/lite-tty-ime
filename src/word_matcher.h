/**
 * word_matcher.h
 * 词语匹配模块接口声明。
 */
#ifndef WORD_MATCHER_H
#define WORD_MATCHER_H

#include <vector>
#include <string>
#include "candidate_item.h"

// 获取所有合适的词语（暂留空实现）
std::vector<CandidateItem> getAllSuitableWords(
    const std::vector<std::vector<std::string>>& split_options);

#endif // WORD_MATCHER_H
