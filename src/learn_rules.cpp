#include "learn_rules.h"

#include <utility>

static std::vector<std::string> splitUtf8CharsLocal(const std::string &s) {
    std::vector<std::string> out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = 1;
        if ((c & 0x80u) == 0x00u) {
            len = 1;
        } else if ((c & 0xE0u) == 0xC0u) {
            len = 2;
        } else if ((c & 0xF0u) == 0xE0u) {
            len = 3;
        } else if ((c & 0xF8u) == 0xF0u) {
            len = 4;
        }
        if (i + len > s.size()) {
            len = 1;
        }
        out.push_back(s.substr(i, len));
        i += len;
    }
    return out;
}

static std::pair<size_t, size_t> countSelectedCharsAndWordsLocal(
    const std::vector<std::string> &selectedWords) {
    size_t charCount = 0;
    size_t wordCount = 0;
    for (const auto &w : selectedWords) {
        const size_t n = splitUtf8CharsLocal(w).size();
        if (n == 1) {
            charCount += 1;
        } else if (n >= 2) {
            wordCount += 1;
        }
    }
    return {charCount, wordCount};
}

bool shouldStoreContinuousAsPhrase(const std::vector<std::string> &selectedWords) {
    const auto cnt = countSelectedCharsAndWordsLocal(selectedWords);
    const size_t charCount = cnt.first;
    const size_t wordCount = cnt.second;
    if (wordCount >= 3) return true;
    if (wordCount == 2 && charCount > 0) return true;
    return false;
}

bool shouldStoreToLearnedByPhrase(
    const std::string &phrase,
    const std::unordered_set<std::string> &knownPhrasesForLearn) {
    const auto chars = splitUtf8CharsLocal(phrase);
    if (chars.size() < 2) {
        return false;
    }
    for (size_t i = 0; i < chars.size(); ++i) {
        std::string seg;
        for (size_t j = i; j < chars.size(); ++j) {
            seg += chars[j];
            if (j - i + 1 >= 2 && knownPhrasesForLearn.find(seg) != knownPhrasesForLearn.end()) {
                return true;
            }
        }
    }
    return false;
}
