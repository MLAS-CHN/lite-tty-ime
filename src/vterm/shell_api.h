#pragma once

#include <string>

namespace vterm {

inline int status_bar_bg_ansi = 40;
inline int status_bar_fg_ansi = 37;
inline std::string status_bar_text{};

inline void set_status_bar_bg_ansi(int ansi_color) {
    status_bar_bg_ansi = ansi_color;
}

inline void set_status_bar_fg_ansi(int ansi_color) {
    status_bar_fg_ansi = ansi_color;
}

inline void write_status_bar(const std::string& text) {
    status_bar_text = text;
}

inline std::string get_status_bar_text() {
    return status_bar_text;
}

}

