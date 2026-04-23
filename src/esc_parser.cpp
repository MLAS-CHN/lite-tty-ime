#include "esc_parser.h"

static void resetEscState(int &escState, std::string &escSeqBuf) {
    escState = 0;
    escSeqBuf.clear();
}

EscParseAction consumeEscSequenceChar(int &escState, std::string &escSeqBuf, char ch) {
    if (escState == 6) {
        escSeqBuf.push_back(ch);
        const unsigned char u = static_cast<unsigned char>(ch);
        if (u >= 0x40u && u <= 0x7Eu) {
            if (escSeqBuf == "\x1b[1;5D") {
                escState = 0;
                return EscParseAction::MoveCursorLeft;
            }
            if (escSeqBuf == "\x1b[1;5C") {
                escState = 0;
                return EscParseAction::MoveCursorRight;
            }
            escState = 0;
            return EscParseAction::ForwardToShell;
        }
        return EscParseAction::NeedMore;
    }

    if (escState == 7) {
        unsigned char u = static_cast<unsigned char>(ch);
        char c = ch;
        if (u >= 'a' && u <= 'd') {
            c = static_cast<char>(u - 32u);
        }
        if (c == 'A' || c == 'B' || c == 'C' || c == 'D') {
            escSeqBuf.push_back(c);
            escState = 0;
            return EscParseAction::ForwardToShell;
        }
        resetEscState(escState, escSeqBuf);
        return EscParseAction::None;
    }

    if (escState == 1) {
        if (ch == '[') {
            escSeqBuf.assign("\x1b[", 2);
            escState = 6;
            return EscParseAction::NeedMore;
        }
        if (ch == 'O' || ch == 'o') {
            escSeqBuf.assign("\x1bO", 2);
            escState = 7;
            return EscParseAction::NeedMore;
        }
        resetEscState(escState, escSeqBuf);
        return EscParseAction::ClearPreedit;
    }

    return EscParseAction::None;
}
