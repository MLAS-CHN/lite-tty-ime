#ifndef INPUT_HANDLERS_H
#define INPUT_HANDLERS_H

#include <cstddef>
#include <functional>

#include "input_state.h"

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
    const std::function<void()> &redrawCurrentLine);

bool handleCandidatePaging(char ch, size_t candidatePageSize, ImeRuntimeState &st, const std::function<void()> &redrawCurrentLine);

bool handleDigitKey(
    char ch,
    size_t candidatePageSize,
    ImeRuntimeState &st,
    bool shellMode,
    const std::function<void(char)> &writeByteToShell,
    const std::function<void(int)> &commitCandidate,
    const std::function<void()> &tryAutoAddPhraseSilent,
    const std::function<void()> &resetAutoAddState,
    const std::function<void()> &redrawCurrentLine);

bool handleFullWidthPunctuation(char ch, ImeRuntimeState &st, const std::function<void(const char *, size_t)> &appendBytes,
                                const std::function<void()> &redrawCurrentLine);

bool handleManualSeparator(char ch, ImeRuntimeState &st, const std::function<void()> &refreshCandidates,
                           const std::function<void()> &maybeCommitSimplePhrase, const std::function<void()> &redrawCurrentLine);

bool handleAlphabetInput(char ch, ImeRuntimeState &st, const std::function<void()> &refreshCandidates,
                         const std::function<void()> &maybeCommitSimplePhrase, const std::function<void()> &redrawCurrentLine);

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
    const std::function<void()> &sendBackspaceToShell);

bool maybeForwardUnhandledCtrlToShell(char ch, bool shellMode, const std::function<void(char)> &writeByteToShell);

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
    const std::function<void()> &sendBackspaceToShell);

bool handleAddWordEnter(
    char ch,
    bool enableAddWordWrite,
    bool addWordMode,
    ImeRuntimeState &st,
    const std::function<bool(const std::string &, const std::string &)> &storeAddWord,
    const std::function<void()> &onEmptyKey,
    const std::function<void()> &onStoreFailed,
    const std::function<void()> &redrawCurrentLine);

} // namespace input_handlers

#endif // INPUT_HANDLERS_H
