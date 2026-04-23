#ifndef LEARN_RULES_H
#define LEARN_RULES_H

#include <string>
#include <unordered_set>
#include <vector>

// 连续输入结果是否应写入短语库（learned_phrases）。
bool shouldStoreContinuousAsPhrase(const std::vector<std::string> &selectedWords);

// 普通自动学习时，是否应按“包含已有词典词”规则写入短语库。
bool shouldStoreToLearnedByPhrase(
    const std::string &phrase,
    const std::unordered_set<std::string> &knownPhrasesForLearn);

#endif // LEARN_RULES_H
