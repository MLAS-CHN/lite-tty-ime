#ifndef INPUT_STATE_H
#define INPUT_STATE_H

#include <string>
#include <vector>

enum class KeyAction {
    SelectFirst,
    CommitRaw,
    SendLineEnd,
    None,
};

struct ImeRuntimeState {
    bool imeOn = false;
    bool imeModeHintActive = false;
    bool imeModeHintIsZh = false;
    std::string composed;
    std::string input;
    size_t inputCursor = 0;
    std::vector<std::string> cands;
    std::vector<std::string> candPyKeys;
    std::vector<size_t> candConsumeLens;
    std::vector<bool> candFromFallback;
    size_t candPageStart = 0;
    std::vector<std::string> pinyinSegments;
    std::string addWordPhrase;
    std::vector<std::string> addWordCommittedPhrases;
    std::vector<std::string> autoAddPinyinSegs;
    std::string autoAddPhraseAcc;
    bool autoAddUsedFallback = false;
    std::string simplePendingPhrase;
    std::string simplePendingPyKey;
    std::vector<std::string> simpleConSelectedWords;
    std::vector<std::string> simpleConSelectedPyKeys;
};

#endif // INPUT_STATE_H
