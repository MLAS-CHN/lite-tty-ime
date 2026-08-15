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
        // 拼接上次未完成的 UTF-8 序列（跨 read 块边界截断的字符）。
        // 仅当新块首字节是续接字节（0x80-0xBF）时才拼接；否则残段作废
        //（可能被 ANSI 序列打断），避免把不完整前缀解码成乱码。
        std::string merged;
        if (!pending_utf8_.empty()) {
            if (len > 0 && ((unsigned char)data[0] & 0xC0) == 0x80) {
                merged.reserve(pending_utf8_.size() + len);
                merged = pending_utf8_;
                merged.append(data, len);
                data = merged.data();
                len = merged.size();
            }
            pending_utf8_.clear();
        }
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
                if (b >= 0x80) {
                    // 多字节 UTF-8 开头：若剩余字节不足以完整解码，说明
                    // 序列被 read 块截断，缓存待下次 feed 拼接（避免把
                    // 首字节/续接字节当单字符写入屏幕造成乱码）。
                    int need = (b & 0xE0) == 0xC0 ? 2 :
                               (b & 0xF0) == 0xE0 ? 3 :
                               (b & 0xF8) == 0xF0 ? 4 : 0;
                    if (need > 0 && (int)(len - i) < need) {
                        pending_utf8_.assign((const char*)p + i, len - i);
                        i = len;
                        break;
                    }
                    if (need == 0) {
                        // 孤立的续接字节（0x80-0xBF）：不完整序列的残留，忽略。
                        i++;
                        continue;
                    }
                }
                char32_t cp{};
                size_t used{};
                if (vterm::utf8_decode_one(p + i, len - i, cp, used)) {
                    screen_.put_char(cp);
                    i += used;
                    continue;
                }
                // 非法字节：按单字节写入（与原始终端行为一致）。
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
                if (b == 'O') {
                    // SS3 序列（ESC O x）：进入等待状态，下一字节为序列主体，
                    // 直接跳过（不写入屏幕）。用状态而非布尔标记，跨 read 块安全。
                    state_ = State::EscO;
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
                if (b == 'P') {
                    // DCS（Device Control String，如 tmux synchronized output
                    // "ESC P tmux;...ESC \"）：内容整体丢弃，防止文本进入屏幕。
                    state_ = State::Dcs;
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

            if (state_ == State::EscO) {
                // 跳过序列主体字符（任意字节），回到普通状态。
                state_ = State::Normal;
                i++;
                continue;
            }

            if (state_ == State::Csi) {
                if (csi_.size() >= kMaxCsiLen) {
                    // 异常超长 CSI（无终止符）：丢弃序列主体，防内存膨胀。
                    state_ = State::Normal;
                    csi_.clear();
                    i++;
                    continue;
                }
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

            if (state_ == State::Dcs) {
                // 丢弃 DCS 内容直到 ESC \（或 BEL）。
                if (b == 0x1B) {
                    state_ = State::DcsEsc;
                    i++;
                    continue;
                }
                i++;
                continue;
            }

            if (state_ == State::DcsEsc) {
                if (b == '\\') {
                    state_ = State::Normal;
                    i++;
                    continue;
                }
                if (b == 0x1B) {
                    i++;
                    continue;
                }
                state_ = State::Dcs;
                continue;
            }
        }
    }

private:
    enum class State { Normal, Esc, Csi, Osc, OscEsc, EscO, Dcs, DcsEsc };

    static std::vector<int> parse_params(const std::string& s, bool& is_private) {
        is_private = false;
        size_t start = 0;
        if (!s.empty() && (s[0] == '?' || s[0] == '>')) {
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
            // DSR（设备状态报告 / 光标位置查询）：不响应。
            // 响应会写回子程序输入流，干扰 tmux 等终端模拟器的状态机
            //（与 DA 同理）。客户端无响应时按默认状态继续。
            return;
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
        if (final == 'c') {
            // DA（Device Attributes）查询：不响应。
            // 注意：响应会写回子程序输入流，干扰 tmux 等终端模拟器的
            // 状态机（detach 时把响应当输入显示/吞键）。opencode 等客户
            // 端无响应时按默认能力渲染，行为正常。
            return;
        }
        if (final == 'q' && is_private && !params.empty() && params[0] == 0) {
            // XTVERSION 查询：不响应（理由同上）。
            return;
        }
    }

    VirtualScreen& screen_;
    int master_fd_ = -1;
    State state_ = State::Normal;
    std::string csi_{};
    std::string pending_utf8_{};
    static constexpr size_t kMaxCsiLen = 256;
};
