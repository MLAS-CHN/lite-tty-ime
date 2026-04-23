#ifndef ESC_PARSER_H
#define ESC_PARSER_H

#include <string>

enum class EscParseAction {
    None,
    NeedMore,
    ForwardToShell,
    MoveCursorLeft,
    MoveCursorRight,
    ClearPreedit,
};

EscParseAction consumeEscSequenceChar(int &escState, std::string &escSeqBuf, char ch);

#endif // ESC_PARSER_H
