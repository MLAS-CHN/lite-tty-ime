#pragma once

#include <wchar.h>

namespace vterm {

// 计算一个 Unicode 码点在终端中的“列宽”（0/1/2），用于全角字符占两列。
inline int codepoint_display_width(char32_t cp) {
    if (cp == 0) return 0;
    if (cp < 0x20) return 0;
    if (cp >= 0x7F && cp < 0xA0) return 0;
    int w = wcwidth((wchar_t)cp);
    if (w < 0) return 1;
    if (w > 2) return 2;
    return w;
}

}

