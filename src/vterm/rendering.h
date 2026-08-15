#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <unistd.h>

#include "util.h"
#include "vterm/shell_api.h"

inline void render_to_real_terminal(const VirtualScreen& screen, int term_rows, int term_cols) {
    static bool blink_on = true;
    static auto last_blink_toggle = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (now - last_blink_toggle >= std::chrono::milliseconds(150)) {
        blink_on = !blink_on;
        last_blink_toggle = now;
    }

    std::string out;
    out.reserve((size_t)term_rows * (size_t)(term_cols + 8));
    out += "\033[?25l\033[?7l";
    bool cursor_on = screen.cursor_visible() && blink_on;
    int cursor_r = screen.cursor_row();
    int cursor_c = screen.cursor_col();
    for (int r = 0; r < screen.rows(); ++r) {
        out += "\033[";
        out += std::to_string(r + 1);
        out += ";1H";
        out += screen.render_row(r, cursor_r, cursor_c, cursor_on);
    }
    out += "\033[";
    out += std::to_string(term_rows);
    out += ";1H";
    out += "\033[";
    out += std::to_string(vterm::status_bar_fg_ansi);
    out += ";";
    out += std::to_string(vterm::status_bar_bg_ansi);
    out += "m";
    out += vterm::status_bar_text;
    int status_w = get_display_width(vterm::status_bar_text);
    if (status_w < 0) status_w = 0;
    int pad = term_cols - status_w;
    if (pad > 0) out.append((size_t)pad, ' ');
    out += "\033[0m";

    int r = screen.cursor_row() + 1;
    int c = screen.cursor_col() + 1;
    if (r < 1) r = 1;
    if (c < 1) c = 1;
    if (r > term_rows - 1) r = term_rows > 1 ? (term_rows - 1) : 1;
    if (c > term_cols) c = term_cols > 0 ? term_cols : 1;
    out += "\033[?7h";
    out += "\033[?25l";
    out += "\033[";
    out += std::to_string(r);
    out += ";";
    out += std::to_string(c);
    out += "H";
    write(STDOUT_FILENO, out.c_str(), out.size());
}
