#pragma once

#include <cstdint>
#include <string>

namespace vterm {

struct Color {
    uint8_t kind = 0; // 0 default, 1 sgr, 2 256, 3 rgb
    uint16_t sgr = 0;
    uint8_t idx = 0;
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    bool operator==(const Color& o) const {
        return kind == o.kind && sgr == o.sgr && idx == o.idx && r == o.r && g == o.g && b == o.b;
    }
    bool operator!=(const Color& o) const { return !(*this == o); }
};

inline void append_color_sgr(std::string& out, bool is_fg, const Color& c) {
    if (c.kind == 0) return;
    if (c.kind == 1) {
        out += "\033[";
        out += std::to_string((int)c.sgr);
        out += "m";
        return;
    }
    if (c.kind == 2) {
        out += is_fg ? "\033[38;5;" : "\033[48;5;";
        out += std::to_string((int)c.idx);
        out += "m";
        return;
    }
    if (c.kind == 3) {
        out += is_fg ? "\033[38;2;" : "\033[48;2;";
        out += std::to_string((int)c.r);
        out += ";";
        out += std::to_string((int)c.g);
        out += ";";
        out += std::to_string((int)c.b);
        out += "m";
        return;
    }
}

}

