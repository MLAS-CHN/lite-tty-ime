#pragma once

#include <cstddef>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

#include "vterm/screen.h"

extern bool g_app_cursor_mode;
extern bool g_app_keypad_mode;

class AnsiParser {
public:
    explicit AnsiParser(VirtualScreen& screen, int master_fd) : screen_(screen), master_fd_(master_fd) {}

    void feed(const char* data, size_t len) {
        const unsigned char* p = (const unsigned char*)data;
        size_t i = 0;
        while (i < len) {
            unsigned char b = p[i];
            if (state_ == State::Normal) {
                if (b == 0x1B) {
                    state_ = State::Esc;
                    i++;
                    continue;
                }
                if (b == 0x7F) {
                    screen_.put_char(U'\b');
                    i++;
                    continue;
                }
                if (b == 0x07 || b == 0x00 || b == 0x0E || b == 0x0F) {
                    i++;
                    continue;
                }
                if (b == 0x0B || b == 0x0C) {
                    screen_.put_char(U'\n');
                    i++;
                    continue;
                }
                if (b < 0x20) {
                    screen_.put_char((char32_t)b);
                    i++;
                    continue;
                }
                char32_t cp{};
                size_t used{};
                if (vterm::utf8_decode_one(p + i, len - i, cp, used)) {
                    screen_.put_char(cp);
                    i += used;
                    continue;
                }
                screen_.put_char((char32_t)b);
                i++;
                continue;
            }

            if (state_ == State::Esc) {
                if (b == '[') {
                    state_ = State::Csi;
                    csi_.clear();
                    i++;
                    continue;
                }
                if (b == '=') {
                    g_app_keypad_mode = true;
                    state_ = State::Normal;
                    i++;
                    continue;
                }
                if (b == '>') {
                    g_app_keypad_mode = false;
                    state_ = State::Normal;
                    i++;
                    continue;
                }
                if (b == ']') {
                    state_ = State::Osc;
                    i++;
                    continue;
                }
                if (b == 'D') {
                    screen_.index();
                    state_ = State::Normal;
                    i++;
                    continue;
                }
                if (b == 'M') {
                    screen_.reverse_index();
                    state_ = State::Normal;
                    i++;
                    continue;
                }
                if (b == 'E') {
                    screen_.next_line();
                    state_ = State::Normal;
                    i++;
                    continue;
                }
                if (b == '7') {
                    screen_.save_cursor();
                    state_ = State::Normal;
                    i++;
                    continue;
                }
                if (b == '8') {
                    screen_.restore_cursor();
                    state_ = State::Normal;
                    i++;
                    continue;
                }
                state_ = State::Normal;
                i++;
                continue;
            }

            if (state_ == State::Csi) {
                csi_.push_back((char)b);
                i++;
                if (b >= 0x40 && b <= 0x7E) {
                    execute_csi(csi_);
                    state_ = State::Normal;
                }
                continue;
            }

            if (state_ == State::Osc) {
                if (b == 0x07) {
                    state_ = State::Normal;
                    i++;
                    continue;
                }
                if (b == 0x1B) {
                    state_ = State::OscEsc;
                    i++;
                    continue;
                }
                i++;
                continue;
            }

            if (state_ == State::OscEsc) {
                if (b == '\\') {
                    state_ = State::Normal;
                    i++;
                    continue;
                }
                if (b == 0x1B) {
                    i++;
                    continue;
                }
                state_ = State::Osc;
                continue;
            }
        }
    }

private:
    enum class State { Normal, Esc, Csi, Osc, OscEsc };

    static std::vector<int> parse_params(const std::string& s, bool& is_private) {
        is_private = false;
        size_t start = 0;
        if (!s.empty() && s[0] == '?') {
            is_private = true;
            start = 1;
        }
        std::vector<int> params;
        int cur = -1;
        for (size_t i = start; i + 1 < s.size(); ++i) {
            char ch = s[i];
            if (ch >= '0' && ch <= '9') {
                if (cur < 0) cur = 0;
                cur = cur * 10 + (ch - '0');
                continue;
            }
            if (ch == ';') {
                params.push_back(cur);
                cur = -1;
            }
        }
        params.push_back(cur);
        return params;
    }

    static int param_or_default(const std::vector<int>& p, size_t idx, int def) {
        if (idx >= p.size()) return def;
        return p[idx] < 0 ? def : p[idx];
    }

    void execute_csi(const std::string& seq) {
        if (seq.empty()) return;
        char final = seq.back();
        bool is_private = false;
        std::vector<int> params = parse_params(seq, is_private);

        if (final == 'A') {
            screen_.cursor_up(param_or_default(params, 0, 1));
            return;
        }
        if (final == 'B') {
            screen_.cursor_down(param_or_default(params, 0, 1));
            return;
        }
        if (final == 'C') {
            screen_.cursor_right(param_or_default(params, 0, 1));
            return;
        }
        if (final == 'D') {
            screen_.cursor_left(param_or_default(params, 0, 1));
            return;
        }
        if (final == 'H' || final == 'f') {
            int r = param_or_default(params, 0, 1);
            int c = param_or_default(params, 1, 1);
            screen_.cursor_pos(r, c);
            return;
        }
        if (final == 'G') {
            int c = param_or_default(params, 0, 1);
            screen_.cursor_column(c);
            return;
        }
        if (final == 'd') {
            int r = param_or_default(params, 0, 1);
            screen_.cursor_row_only(r);
            return;
        }
        if (final == 'E') {
            int n = param_or_default(params, 0, 1);
            screen_.cursor_down(n);
            screen_.cursor_column(1);
            return;
        }
        if (final == 'F') {
            int n = param_or_default(params, 0, 1);
            screen_.cursor_up(n);
            screen_.cursor_column(1);
            return;
        }
        if (final == 'J') {
            screen_.erase_in_display(param_or_default(params, 0, 0));
            return;
        }
        if (final == 'K') {
            screen_.erase_in_line(param_or_default(params, 0, 0));
            return;
        }
        if (final == 'X') {
            int n = param_or_default(params, 0, 1);
            screen_.erase_chars(n);
            return;
        }
        if (final == '@') {
            int n = param_or_default(params, 0, 1);
            screen_.insert_chars(n);
            return;
        }
        if (final == 'P') {
            int n = param_or_default(params, 0, 1);
            screen_.delete_chars(n);
            return;
        }
        if (final == 'L') {
            int n = param_or_default(params, 0, 1);
            screen_.insert_lines(n);
            return;
        }
        if (final == 'M') {
            int n = param_or_default(params, 0, 1);
            screen_.delete_lines(n);
            return;
        }
        if (final == 'S') {
            int n = param_or_default(params, 0, 1);
            screen_.scroll_up(n);
            return;
        }
        if (final == 'T') {
            int n = param_or_default(params, 0, 1);
            screen_.scroll_down(n);
            return;
        }
        if (final == 'r') {
            int top = param_or_default(params, 0, 1);
            int bottom = param_or_default(params, 1, screen_.rows());
            screen_.set_scroll_region(top, bottom);
            return;
        }
        if (final == 'm') {
            screen_.set_sgr(params);
            return;
        }
        if (final == 's') {
            screen_.save_cursor();
            return;
        }
        if (final == 'u') {
            screen_.restore_cursor();
            return;
        }
        if (final == 'n' && !is_private) {
            int p0 = param_or_default(params, 0, 0);
            if (p0 == 5) {
                const char* ok = "\033[0n";
                if (master_fd_ >= 0) write(master_fd_, ok, (size_t)strlen(ok));
                return;
            }
            if (p0 == 6) {
                int r = screen_.cursor_row() + 1;
                int c = screen_.cursor_col() + 1;
                std::string resp = "\033[" + std::to_string(r) + ";" + std::to_string(c) + "R";
                if (master_fd_ >= 0) write(master_fd_, resp.c_str(), resp.size());
                return;
            }
        }
        if ((final == 'h' || final == 'l') && is_private) {
            for (int p : params) {
                if (p == 1) {
                    g_app_cursor_mode = (final == 'h');
                }
                if (p == 25) {
                    screen_.set_cursor_visible(final == 'h');
                }
                if (p == 7) {
                    screen_.set_autowrap(final == 'h');
                }
                if (p == 6) {
                    screen_.set_origin_mode(final == 'h');
                }
                if (p == 1049 || p == 47 || p == 1047) {
                    if (final == 'h') {
                        screen_.enter_alternate_screen();
                    } else {
                        screen_.exit_alternate_screen();
                    }
                }
            }
            return;
        }
    }

    VirtualScreen& screen_;
    int master_fd_ = -1;
    State state_ = State::Normal;
    std::string csi_{};
};
