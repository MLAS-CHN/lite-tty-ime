#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace vterm {

// 将单个 Unicode 码点编码为 UTF-8 字节序列。
inline std::string utf8_encode(char32_t cp) {
    std::string out;
    if (cp <= 0x7F) {
        out.push_back((char)cp);
    } else if (cp <= 0x7FF) {
        out.push_back((char)(0xC0 | ((cp >> 6) & 0x1F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back((char)(0xE0 | ((cp >> 12) & 0x0F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        out.push_back((char)(0xF0 | ((cp >> 18) & 0x07)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
    return out;
}

// 从 UTF-8 字节流中解码一个码点；成功返回 true，并给出码点与消耗字节数。
inline bool utf8_decode_one(const unsigned char* s, size_t n, char32_t& out_cp, size_t& out_len) {
    if (n == 0) return false;
    unsigned char b0 = s[0];
    if (b0 < 0x80) {
        out_cp = b0;
        out_len = 1;
        return true;
    }
    if ((b0 & 0xE0) == 0xC0) {
        if (n < 2) return false;
        unsigned char b1 = s[1];
        if ((b1 & 0xC0) != 0x80) return false;
        out_cp = ((b0 & 0x1F) << 6) | (b1 & 0x3F);
        out_len = 2;
        return true;
    }
    if ((b0 & 0xF0) == 0xE0) {
        if (n < 3) return false;
        unsigned char b1 = s[1];
        unsigned char b2 = s[2];
        if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) return false;
        out_cp = ((b0 & 0x0F) << 12) | ((b1 & 0x3F) << 6) | (b2 & 0x3F);
        out_len = 3;
        return true;
    }
    if ((b0 & 0xF8) == 0xF0) {
        if (n < 4) return false;
        unsigned char b1 = s[1];
        unsigned char b2 = s[2];
        unsigned char b3 = s[3];
        if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80 || (b3 & 0xC0) != 0x80) return false;
        out_cp = ((b0 & 0x07) << 18) | ((b1 & 0x3F) << 12) | ((b2 & 0x3F) << 6) | (b3 & 0x3F);
        out_len = 4;
        return true;
    }
    return false;
}

}

