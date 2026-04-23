#include "query_forms.h"

#include <unordered_set>
#include <utility>

#include "pinyin.h"

static std::string replaceCharCopyLocal(std::string s, char from, char to) {
    for (char &ch : s) {
        if (ch == from) {
            ch = to;
        }
    }
    return s;
}

static size_t splitCountBySep(const std::string &s, char sep) {
    size_t cnt = 0;
    size_t i = 0;
    while (i <= s.size()) {
        const size_t p = s.find(sep, i);
        const size_t end = (p == std::string::npos) ? s.size() : p;
        if (end > i) {
            ++cnt;
        }
        if (p == std::string::npos) {
            break;
        }
        i = p + 1;
    }
    return cnt;
}

PinyinQueryForms buildPinyinQueryForms(
    const std::string &input,
    const std::unordered_set<std::string> &mapSyllables) {
    const std::string rawRad = aggressiveSplitPinyin(input, mapSyllables);
    const std::string rawCons = conservativeSplitPinyin(input, mapSyllables);
    std::string rawMerge = rawRad;
    const auto merged = mergeSplitPinyin(rawRad, mapSyllables);
    if (!merged.empty()) {
        rawMerge = merged.back();
    }

    const std::string mergeStr = replaceCharCopyLocal(rawMerge, ',', '\'');
    const std::string radStr = replaceCharCopyLocal(rawRad, ',', '\'');
    const std::string consStr = replaceCharCopyLocal(rawCons, ',', '\'');

    std::vector<std::string> orderedForms = {mergeStr, radStr, consStr};
    std::vector<std::string> orderedDisplayForms = {rawMerge, rawRad, rawCons};
    if (orderedForms.size() == 3) {
        const size_t m = splitCountBySep(orderedForms[0], '\'');
        const size_t r = splitCountBySep(orderedForms[1], '\'');
        const size_t c = splitCountBySep(orderedForms[2], '\'');
        if (c < m && c < r) {
            std::swap(orderedForms[0], orderedForms[2]);
            std::swap(orderedForms[1], orderedForms[2]);
            std::swap(orderedDisplayForms[0], orderedDisplayForms[2]);
            std::swap(orderedDisplayForms[1], orderedDisplayForms[2]);
        }
    }

    std::vector<std::string> logicForms;
    std::vector<std::string> displayForms;
    std::unordered_set<std::string> seen;
    for (size_t i = 0; i < orderedForms.size() && i < orderedDisplayForms.size(); ++i) {
        if (orderedForms[i].empty()) {
            continue;
        }
        if (seen.insert(orderedForms[i]).second) {
            logicForms.push_back(orderedForms[i]);
            displayForms.push_back(orderedDisplayForms[i]);
        }
    }

    PinyinQueryForms out;
    out.mergeLogic = mergeStr;
    out.mergeDisplay = rawMerge;
    out.logicForms = std::move(logicForms);
    out.displayForms = std::move(displayForms);
    return out;
}
