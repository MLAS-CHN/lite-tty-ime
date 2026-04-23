#include "input_handlers.h"

#include <cctype>
#include <cstring>

namespace input_handlers {

bool applyImeAction(
    KeyAction action,
    char lineEndCh,
    ImeRuntimeState &st,
    const std::function<void(int)> &commitCandidate,
    const std::function<void()> &tryAutoAddPhraseSilent,
    const std::function<void()> &resetAutoAddState,
    const std::function<void()> &clearPreeditState,
    const std::function<void(char)> &sendLineEnd,
    const std::function<void(const char *, size_t)> &appendBytes,
    const std::function<void()> &redrawCurrentLine) {
    switch (action) {
    case KeyAction::SelectFirst:
        if (!st.cands.empty()) {
            commitCandidate(0);
            if (st.input.empty() && st.cands.empty()) {
                tryAutoAddPhraseSilent();
                resetAutoAddState();
            }
            redrawCurrentLine();
            return true;
        }
        return false;
    case KeyAction::CommitRaw:
        if (!st.input.empty()) {
            appendBytes(st.input.data(), st.input.size());
            clearPreeditState();
            redrawCurrentLine();
            return true;
        }
        return false;
    case KeyAction::SendLineEnd:
        if (st.input.empty() && st.cands.empty()) {
            sendLineEnd(lineEndCh);
            return true;
        }
        return false;
    case KeyAction::None:
    default:
        return false;
    }
}

bool handleCandidatePaging(char ch, size_t candidatePageSize, ImeRuntimeState &st, const std::function<void()> &redrawCurrentLine) {
    if ((ch == '=' || ch == '+') && !st.cands.empty()) {
        if (candidatePageSize == 0) return true;
        const size_t pageCnt = (st.cands.size() + candidatePageSize - 1) / candidatePageSize;
        const size_t curPage = st.candPageStart / candidatePageSize;
        const size_t nextPage = (curPage + 1 < pageCnt) ? (curPage + 1) : curPage;
        st.candPageStart = nextPage * candidatePageSize;
        redrawCurrentLine();
        return true;
    }
    if (ch == '-' && !st.cands.empty()) {
        if (candidatePageSize == 0) return true;
        const size_t curPage = st.candPageStart / candidatePageSize;
        const size_t prevPage = (curPage == 0) ? 0 : (curPage - 1);
        st.candPageStart = prevPage * candidatePageSize;
        redrawCurrentLine();
        return true;
    }
    return false;
}

bool handleDigitKey(
    char ch,
    size_t candidatePageSize,
    ImeRuntimeState &st,
    bool shellMode,
    const std::function<void(char)> &writeByteToShell,
    const std::function<void(int)> &commitCandidate,
    const std::function<void()> &tryAutoAddPhraseSilent,
    const std::function<void()> &resetAutoAddState,
    const std::function<void()> &redrawCurrentLine) {
    if (ch < '1' || ch > '9') {
        return false;
    }
    if (!st.cands.empty()) {
        int idx = ch - '1';
        if (idx < static_cast<int>(candidatePageSize)) {
            commitCandidate(idx);
            if (st.input.empty() && st.cands.empty()) {
                tryAutoAddPhraseSilent();
                resetAutoAddState();
            }
        }
        redrawCurrentLine();
        return true;
    }
    if (shellMode) {
        writeByteToShell(ch);
    }
    return true;
}

bool handleFullWidthPunctuation(char ch, ImeRuntimeState &st, const std::function<void(const char *, size_t)> &appendBytes,
                                const std::function<void()> &redrawCurrentLine) {
    if (!st.input.empty() || !st.cands.empty()) {
        return false;
    }
    const char *fw = nullptr;
    switch (ch) {
    case '`': fw = "·"; break;
    case '~': fw = "~"; break;
    case '!': fw = "！"; break;
    case '@': fw = "@"; break;
    case '#': fw = "#"; break;
    case '$': fw = "￥"; break;
    case '%': fw = "%"; break;
    case '^': fw = "……"; break;
    case '&': fw = "&"; break;
    case '*': fw = "*"; break;
    case '(': fw = "（"; break;
    case ')': fw = "）"; break;
    case '_': fw = "——"; break;
    case '-': fw = "-"; break;
    case '+': fw = "+"; break;
    case '=': fw = "="; break;
    case ',': fw = "，"; break;
    case '.': fw = "。"; break;
    case '/': fw = "、"; break;
    case ';': fw = "；"; break;
    case '\'': fw = "‘"; break;
    case '[': fw = "【"; break;
    case ']': fw = "】"; break;
    case '\\': fw = "、"; break;
    case '<': fw = "《"; break;
    case '>': fw = "》"; break;
    case '?': fw = "？"; break;
    case ':': fw = "："; break;
    case '"': fw = "“"; break;
    case '{': fw = "{"; break;
    case '}': fw = "}"; break;
    case '|': fw = "|"; break;
    default: break;
    }
    if (!fw) {
        return false;
    }
    appendBytes(fw, std::strlen(fw));
    redrawCurrentLine();
    return true;
}

bool handleManualSeparator(char ch, ImeRuntimeState &st, const std::function<void()> &refreshCandidates,
                           const std::function<void()> &maybeCommitSimplePhrase, const std::function<void()> &redrawCurrentLine) {
    if (ch != '\'') {
        return false;
    }
    st.input.insert(st.input.begin() + static_cast<std::string::difference_type>(st.inputCursor), ch);
    ++st.inputCursor;
    refreshCandidates();
    maybeCommitSimplePhrase();
    redrawCurrentLine();
    return true;
}

bool handleAlphabetInput(char ch, ImeRuntimeState &st, const std::function<void()> &refreshCandidates,
                         const std::function<void()> &maybeCommitSimplePhrase, const std::function<void()> &redrawCurrentLine) {
    if (!std::isalpha(static_cast<unsigned char>(ch))) {
        return false;
    }
    st.input.insert(st.input.begin() + static_cast<std::string::difference_type>(st.inputCursor),
                    static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    ++st.inputCursor;
    refreshCandidates();
    maybeCommitSimplePhrase();
    redrawCurrentLine();
    return true;
}

bool handleEnglishModeKey(
    char ch,
    ImeRuntimeState &st,
    bool shellMode,
    int &imeEscState,
    std::string &escSeqBuf,
    const std::function<void(char)> &sendLineEnd,
    const std::function<void()> &redrawCurrentLine,
    const std::function<void(const char *, size_t)> &appendBytes,
    const std::function<void(std::string &)> &popLastUtf8CharInPlace,
    const std::function<void()> &sendBackspaceToShell) {
    if (st.imeOn) {
        return false;
    }
    if (st.imeModeHintActive) {
        st.imeModeHintActive = false;
    }
    if (ch == 0x7F || ch == 0x08) {
        if (!st.composed.empty()) {
            popLastUtf8CharInPlace(st.composed);
        }
        if (shellMode) {
            sendBackspaceToShell();
        }
        if (!shellMode) {
            redrawCurrentLine();
        }
    } else if (ch == '\r' || ch == '\n') {
        sendLineEnd(ch);
    } else {
        appendBytes(&ch, 1);
        if (!shellMode) {
            redrawCurrentLine();
        }
    }
    imeEscState = 0;
    escSeqBuf.clear();
    return true;
}

bool maybeForwardUnhandledCtrlToShell(char ch, bool shellMode, const std::function<void(char)> &writeByteToShell) {
    const unsigned char u = static_cast<unsigned char>(ch);
    if (shellMode && (u < 0x20u || u == 0x7fu)) {
        writeByteToShell(ch);
        return true;
    }
    return false;
}

bool handleBackspaceKey(
    char ch,
    ImeRuntimeState &st,
    bool addWordMode,
    bool shellMode,
    const std::function<void()> &resetAutoAddState,
    const std::function<void()> &refreshCandidates,
    const std::function<void()> &maybeCommitSimplePhrase,
    const std::function<void()> &redrawCurrentLine,
    const std::function<void(std::string &)> &popLastUtf8CharInPlace,
    const std::function<void()> &sendBackspaceToShell) {
    if (!(ch == 0x7F || ch == 0x08)) {
        return false;
    }
    if (!st.input.empty()) {
        if (st.inputCursor > 0) {
            st.input.erase(st.input.begin() + static_cast<std::string::difference_type>(st.inputCursor - 1));
            --st.inputCursor;
        }
    } else {
        if (!st.composed.empty()) {
            popLastUtf8CharInPlace(st.composed);
        }
        if (shellMode) {
            sendBackspaceToShell();
        }
        if (!addWordMode) {
            resetAutoAddState();
        }
        if (addWordMode && !st.addWordCommittedPhrases.empty()) {
            const std::string lastPiece = st.addWordCommittedPhrases.back();
            st.addWordCommittedPhrases.pop_back();
            if (st.addWordPhrase.size() >= lastPiece.size() &&
                st.addWordPhrase.compare(st.addWordPhrase.size() - lastPiece.size(),
                                         lastPiece.size(), lastPiece) == 0) {
                st.addWordPhrase.erase(st.addWordPhrase.size() - lastPiece.size());
            } else if (!st.addWordPhrase.empty()) {
                popLastUtf8CharInPlace(st.addWordPhrase);
            }
            if (!st.pinyinSegments.empty()) {
                st.pinyinSegments.pop_back();
            }
        }
    }
    refreshCandidates();
    maybeCommitSimplePhrase();
    redrawCurrentLine();
    return true;
}

bool handleAddWordEnter(
    char ch,
    bool enableAddWordWrite,
    bool addWordMode,
    ImeRuntimeState &st,
    const std::function<bool(const std::string &, const std::string &)> &storeAddWord,
    const std::function<void()> &onEmptyKey,
    const std::function<void()> &onStoreFailed,
    const std::function<void()> &redrawCurrentLine) {
    if (!(enableAddWordWrite && addWordMode && (ch == '\r' || ch == '\n'))) {
        return false;
    }
    if (!(st.input.empty() && st.cands.empty() && !st.addWordPhrase.empty())) {
        return false;
    }
    std::string pyKey;
    for (size_t si = 0; si < st.pinyinSegments.size(); ++si) {
        if (si) {
            pyKey += ',';
        }
        pyKey += st.pinyinSegments[si];
    }
    if (pyKey.empty()) {
        onEmptyKey();
    } else {
        const std::string phrase = st.addWordPhrase;
        if (!storeAddWord(pyKey, phrase)) {
            onStoreFailed();
        }
    }
    st.addWordPhrase.clear();
    st.pinyinSegments.clear();
    st.addWordCommittedPhrases.clear();
    redrawCurrentLine();
    return true;
}

} // namespace input_handlers
