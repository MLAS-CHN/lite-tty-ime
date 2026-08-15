#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "vterm/color.h"
#include "vterm/term_width.h"
#include "vterm/utf8.h"

struct Cell {
    char32_t ch = U' ';
    vterm::Color fg{};
    vterm::Color bg{};
    uint8_t attrs = 0;
    uint8_t wide = 0;
};

class VirtualScreen {
public:
    VirtualScreen() = default;

    void resize(int rows, int cols) {
        int old_rows = rows_;
        bool was_empty = cells_.empty() || old_rows <= 0 || cols_ <= 0;
        if (rows < 1) rows = 1;
        if (cols < 1) cols = 1;
        std::vector<Cell> new_cells((size_t)rows * (size_t)cols);
        int copy_rows = rows < rows_ ? rows : rows_;
        int copy_cols = cols < cols_ ? cols : cols_;
        for (int r = 0; r < copy_rows; ++r) {
            for (int c = 0; c < copy_cols; ++c) {
                new_cells[(size_t)r * (size_t)cols + (size_t)c] = cells_[(size_t)r * (size_t)cols_ + (size_t)c];
            }
        }
        rows_ = rows;
        cols_ = cols;
        cells_.swap(new_cells);
        if (was_empty) {
            scroll_top_ = 0;
            scroll_bottom_ = rows_ - 1;
            cur_r_ = 0;
            cur_c_ = 0;
            saved_r_ = 0;
            saved_c_ = 0;
        }
        if (cur_r_ >= rows_) cur_r_ = rows_ - 1;
        if (cur_c_ >= cols_) cur_c_ = cols_ - 1;
        if (scroll_top_ >= rows_) scroll_top_ = 0;
        if (scroll_bottom_ >= rows_) scroll_bottom_ = rows_ - 1;
        if (scroll_top_ > scroll_bottom_) {
            scroll_top_ = 0;
            scroll_bottom_ = rows_ - 1;
        }
        wrap_pending_ = false;
    }

    int rows() const { return rows_; }
    int cols() const { return cols_; }
    int cursor_row() const { return cur_r_; }
    int cursor_col() const { return cur_c_; }
    bool cursor_visible() const { return cursor_visible_; }
    void set_cursor_visible(bool v) { cursor_visible_ = v; }
    void set_autowrap(bool v) { autowrap_ = v; }
    bool autowrap() const { return autowrap_; }
    void set_origin_mode(bool v) { origin_mode_ = v; }
    bool origin_mode() const { return origin_mode_; }

    void enter_alternate_screen() {
        if (in_alt_screen_) return;
        saved_main_cells_ = cells_;
        saved_main_rows_ = rows_;
        saved_main_cols_ = cols_;
        saved_main_cur_r_ = cur_r_;
        saved_main_cur_c_ = cur_c_;
        saved_main_scroll_top_ = scroll_top_;
        saved_main_scroll_bottom_ = scroll_bottom_;
        saved_main_saved_r_ = saved_r_;
        saved_main_saved_c_ = saved_c_;
        saved_main_fg_ = cur_fg_;
        saved_main_bg_ = cur_bg_;
        saved_main_attrs_ = cur_attrs_;
        saved_main_cursor_visible_ = cursor_visible_;
        saved_main_wrap_pending_ = wrap_pending_;

        in_alt_screen_ = true;
        clear_all();
    }

    void exit_alternate_screen() {
        if (!in_alt_screen_) return;
        in_alt_screen_ = false;
        if (!saved_main_cells_.empty()) {
            cells_.swap(saved_main_cells_);
            rows_ = saved_main_rows_;
            cols_ = saved_main_cols_;
            cur_r_ = saved_main_cur_r_;
            cur_c_ = saved_main_cur_c_;
            scroll_top_ = saved_main_scroll_top_;
            scroll_bottom_ = saved_main_scroll_bottom_;
            saved_r_ = saved_main_saved_r_;
            saved_c_ = saved_main_saved_c_;
            cur_fg_ = saved_main_fg_;
            cur_bg_ = saved_main_bg_;
            cur_attrs_ = saved_main_attrs_;
            cursor_visible_ = saved_main_cursor_visible_;
            wrap_pending_ = saved_main_wrap_pending_;
        }
    }

    void set_sgr(const std::vector<int>& params) {
        if (params.empty() || (params.size() == 1 && params[0] < 0)) {
            reset_attrs();
            return;
        }

        for (size_t i = 0; i < params.size(); ++i) {
            int p = params[i] < 0 ? 0 : params[i];
            if (p == 0) {
                reset_attrs();
                continue;
            }
            if (p == 1) {
                cur_attrs_ |= AttrBold;
                continue;
            }
            if (p == 22) {
                cur_attrs_ &= (uint8_t)~AttrBold;
                continue;
            }
            if (p == 4) {
                cur_attrs_ |= AttrUnderline;
                continue;
            }
            if (p == 24) {
                cur_attrs_ &= (uint8_t)~AttrUnderline;
                continue;
            }
            if (p == 7) {
                cur_attrs_ |= AttrInverse;
                continue;
            }
            if (p == 27) {
                cur_attrs_ &= (uint8_t)~AttrInverse;
                continue;
            }
            if ((p >= 30 && p <= 37) || (p >= 90 && p <= 97)) {
                cur_fg_.kind = 1;
                cur_fg_.sgr = (uint16_t)p;
                continue;
            }
            if (p == 39) {
                cur_fg_ = vterm::Color{};
                continue;
            }
            if ((p >= 40 && p <= 47) || (p >= 100 && p <= 107)) {
                cur_bg_.kind = 1;
                cur_bg_.sgr = (uint16_t)p;
                continue;
            }
            if (p == 49) {
                cur_bg_ = vterm::Color{};
                continue;
            }
            if (p == 38 || p == 48) {
                bool is_fg = (p == 38);
                int mode = (i + 1 < params.size()) ? (params[i + 1] < 0 ? 0 : params[i + 1]) : 0;
                if (mode == 5) {
                    if (i + 2 < params.size()) {
                        int idx = params[i + 2] < 0 ? 0 : params[i + 2];
                        if (idx < 0) idx = 0;
                        if (idx > 255) idx = 255;
                        vterm::Color c{};
                        c.kind = 2;
                        c.idx = (uint8_t)idx;
                        if (is_fg) cur_fg_ = c;
                        else cur_bg_ = c;
                        i += 2;
                        continue;
                    }
                } else if (mode == 2) {
                    if (i + 4 < params.size()) {
                        int r = params[i + 2] < 0 ? 0 : params[i + 2];
                        int g = params[i + 3] < 0 ? 0 : params[i + 3];
                        int b = params[i + 4] < 0 ? 0 : params[i + 4];
                        if (r < 0) r = 0;
                        if (g < 0) g = 0;
                        if (b < 0) b = 0;
                        if (r > 255) r = 255;
                        if (g > 255) g = 255;
                        if (b > 255) b = 255;
                        vterm::Color c{};
                        c.kind = 3;
                        c.r = (uint8_t)r;
                        c.g = (uint8_t)g;
                        c.b = (uint8_t)b;
                        if (is_fg) cur_fg_ = c;
                        else cur_bg_ = c;
                        i += 4;
                        continue;
                    }
                }
            }
        }
    }

    void put_char(char32_t ch) {
        if (rows_ <= 0 || cols_ <= 0) return;
        if (ch == U'\n') {
            cur_c_ = 0;
            index();
            wrap_pending_ = false;
            return;
        }
        if (ch == U'\r') {
            cur_c_ = 0;
            wrap_pending_ = false;
            return;
        }
        if (ch == U'\b') {
            if (cur_c_ > 0) cur_c_--;
            wrap_pending_ = false;
            return;
        }
        if (ch == U'\t') {
            int next = ((cur_c_ / 8) + 1) * 8;
            cur_c_ = next < cols_ ? next : (cols_ - 1);
            wrap_pending_ = false;
            return;
        }

        if (wrap_pending_) {
            next_line();
            wrap_pending_ = false;
        }

        if (cur_r_ < 0) cur_r_ = 0;
        if (cur_r_ >= rows_) cur_r_ = rows_ - 1;
        if (cur_c_ < 0) cur_c_ = 0;
        if (cur_c_ >= cols_) {
            next_line();
        }
        if (cur_c_ < 0) cur_c_ = 0;
        if (cur_c_ >= cols_) cur_c_ = cols_ - 1;

        int w = vterm::codepoint_display_width(ch);
        if (w <= 0) return;
        if (w > 2) w = 2;

        if (w == 2 && cur_c_ == cols_ - 1) {
            if (autowrap_) {
                next_line();
            }
        }

        clear_wide_at(cur_r_, cur_c_);
        if (w == 2 && cur_c_ + 1 < cols_) {
            clear_wide_at(cur_r_, cur_c_ + 1);
        }

        Cell& cell = cells_[(size_t)cur_r_ * (size_t)cols_ + (size_t)cur_c_];
        cell.ch = ch;
        cell.fg = cur_fg_;
        cell.bg = cur_bg_;
        cell.attrs = cur_attrs_;
        cell.wide = (w == 2) ? 1 : 0;

        if (w == 2 && cur_c_ + 1 < cols_) {
            Cell& cont = cells_[(size_t)cur_r_ * (size_t)cols_ + (size_t)(cur_c_ + 1)];
            cont.ch = 0;
            cont.fg = cur_fg_;
            cont.bg = cur_bg_;
            cont.attrs = cur_attrs_;
            cont.wide = 2;
        }

        if (w == 2) {
            if (cur_c_ + 2 >= cols_) {
                if (autowrap_) wrap_pending_ = true;
                cur_c_ = cols_ - 1;
            } else {
                cur_c_ += 2;
            }
        } else {
            if (cur_c_ == cols_ - 1) {
                if (autowrap_) wrap_pending_ = true;
            } else {
                cur_c_++;
            }
        }
    }

    void cursor_up(int n) {
        if (n < 1) n = 1;
        cur_r_ -= n;
        if (cur_r_ < 0) cur_r_ = 0;
        wrap_pending_ = false;
    }

    void cursor_down(int n) {
        if (n < 1) n = 1;
        cur_r_ += n;
        if (cur_r_ >= rows_) cur_r_ = rows_ - 1;
        wrap_pending_ = false;
    }

    void cursor_right(int n) {
        if (n < 1) n = 1;
        cur_c_ += n;
        if (cur_c_ >= cols_) cur_c_ = cols_ - 1;
        wrap_pending_ = false;
    }

    void cursor_left(int n) {
        if (n < 1) n = 1;
        cur_c_ -= n;
        if (cur_c_ < 0) cur_c_ = 0;
        wrap_pending_ = false;
    }

    void cursor_pos(int r1, int c1) {
        int r = r1 <= 0 ? 0 : (r1 - 1);
        int c = c1 <= 0 ? 0 : (c1 - 1);
        if (r < 0) r = 0;
        if (c < 0) c = 0;
        if (origin_mode_) r = scroll_top_ + r;
        if (r >= rows_) r = rows_ - 1;
        if (c >= cols_) c = cols_ - 1;
        if (origin_mode_) {
            if (r < scroll_top_) r = scroll_top_;
            if (r > scroll_bottom_) r = scroll_bottom_;
        }
        cur_r_ = r;
        cur_c_ = c;
        wrap_pending_ = false;
    }

    void cursor_column(int c1) {
        int c = c1 <= 0 ? 0 : (c1 - 1);
        if (c < 0) c = 0;
        if (c >= cols_) c = cols_ - 1;
        cur_c_ = c;
        wrap_pending_ = false;
    }

    void cursor_row_only(int r1) {
        int r = r1 <= 0 ? 0 : (r1 - 1);
        if (r < 0) r = 0;
        if (origin_mode_) r = scroll_top_ + r;
        if (r >= rows_) r = rows_ - 1;
        if (origin_mode_) {
            if (r < scroll_top_) r = scroll_top_;
            if (r > scroll_bottom_) r = scroll_bottom_;
        }
        cur_r_ = r;
        wrap_pending_ = false;
    }

    void erase_in_display(int mode) {
        if (mode == 2) {
            clear_all();
            return;
        }
        if (mode == 0) {
            for (int r = cur_r_; r < rows_; ++r) {
                int start_c = (r == cur_r_) ? cur_c_ : 0;
                for (int c = start_c; c < cols_; ++c) {
                    set_cell_blank(cells_[(size_t)r * (size_t)cols_ + (size_t)c]);
                }
            }
        } else if (mode == 1) {
            for (int r = 0; r <= cur_r_; ++r) {
                int end_c = (r == cur_r_) ? cur_c_ : (cols_ - 1);
                for (int c = 0; c <= end_c; ++c) {
                    set_cell_blank(cells_[(size_t)r * (size_t)cols_ + (size_t)c]);
                }
            }
        }
    }

    void erase_in_line(int mode) {
        if (cur_r_ < 0 || cur_r_ >= rows_) return;
        if (mode == 2) {
            for (int c = 0; c < cols_; ++c) {
                set_cell_blank(cells_[(size_t)cur_r_ * (size_t)cols_ + (size_t)c]);
            }
            return;
        }
        if (mode == 0) {
            for (int c = cur_c_; c < cols_; ++c) {
                set_cell_blank(cells_[(size_t)cur_r_ * (size_t)cols_ + (size_t)c]);
            }
        } else if (mode == 1) {
            for (int c = 0; c <= cur_c_ && c < cols_; ++c) {
                set_cell_blank(cells_[(size_t)cur_r_ * (size_t)cols_ + (size_t)c]);
            }
        }
    }

    void set_scroll_region(int top1, int bottom1) {
        int top = top1 <= 0 ? 0 : (top1 - 1);
        int bottom = bottom1 <= 0 ? (rows_ - 1) : (bottom1 - 1);
        if (top < 0) top = 0;
        if (bottom < 0) bottom = 0;
        if (top >= rows_) top = rows_ - 1;
        if (bottom >= rows_) bottom = rows_ - 1;
        if (top > bottom) {
            top = 0;
            bottom = rows_ - 1;
        }
        scroll_top_ = top;
        scroll_bottom_ = bottom;
        cursor_pos(1, 1);
    }

    void scroll_up(int n) {
        if (n < 1) n = 1;
        if (scroll_top_ < 0) scroll_top_ = 0;
        if (scroll_bottom_ >= rows_) scroll_bottom_ = rows_ - 1;
        int height = scroll_bottom_ - scroll_top_ + 1;
        if (height <= 0) return;
        if (n > height) n = height;
        for (int r = scroll_top_; r <= scroll_bottom_ - n; ++r) {
            for (int c = 0; c < cols_; ++c) {
                cells_[(size_t)r * (size_t)cols_ + (size_t)c] = cells_[(size_t)(r + n) * (size_t)cols_ + (size_t)c];
            }
        }
        for (int r = scroll_bottom_ - n + 1; r <= scroll_bottom_; ++r) {
            for (int c = 0; c < cols_; ++c) {
                set_cell_blank_to_current(cells_[(size_t)r * (size_t)cols_ + (size_t)c]);
            }
        }
    }

    void scroll_down(int n) {
        if (n < 1) n = 1;
        if (scroll_top_ < 0) scroll_top_ = 0;
        if (scroll_bottom_ >= rows_) scroll_bottom_ = rows_ - 1;
        int height = scroll_bottom_ - scroll_top_ + 1;
        if (height <= 0) return;
        if (n > height) n = height;
        for (int r = scroll_bottom_; r >= scroll_top_ + n; --r) {
            for (int c = 0; c < cols_; ++c) {
                cells_[(size_t)r * (size_t)cols_ + (size_t)c] = cells_[(size_t)(r - n) * (size_t)cols_ + (size_t)c];
            }
        }
        for (int r = scroll_top_; r < scroll_top_ + n; ++r) {
            for (int c = 0; c < cols_; ++c) {
                set_cell_blank_to_current(cells_[(size_t)r * (size_t)cols_ + (size_t)c]);
            }
        }
    }

    void insert_lines(int n) {
        if (n < 1) n = 1;
        if (cur_r_ < scroll_top_ || cur_r_ > scroll_bottom_) return;
        int height = scroll_bottom_ - cur_r_ + 1;
        if (height <= 0) return;
        if (n > height) n = height;
        for (int r = scroll_bottom_; r >= cur_r_ + n; --r) {
            for (int c = 0; c < cols_; ++c) {
                cells_[(size_t)r * (size_t)cols_ + (size_t)c] = cells_[(size_t)(r - n) * (size_t)cols_ + (size_t)c];
            }
        }
        for (int r = cur_r_; r < cur_r_ + n; ++r) {
            for (int c = 0; c < cols_; ++c) {
                set_cell_blank_to_current(cells_[(size_t)r * (size_t)cols_ + (size_t)c]);
            }
        }
    }

    void delete_lines(int n) {
        if (n < 1) n = 1;
        if (cur_r_ < scroll_top_ || cur_r_ > scroll_bottom_) return;
        int height = scroll_bottom_ - cur_r_ + 1;
        if (height <= 0) return;
        if (n > height) n = height;
        for (int r = cur_r_; r <= scroll_bottom_ - n; ++r) {
            for (int c = 0; c < cols_; ++c) {
                cells_[(size_t)r * (size_t)cols_ + (size_t)c] = cells_[(size_t)(r + n) * (size_t)cols_ + (size_t)c];
            }
        }
        for (int r = scroll_bottom_ - n + 1; r <= scroll_bottom_; ++r) {
            for (int c = 0; c < cols_; ++c) {
                set_cell_blank_to_current(cells_[(size_t)r * (size_t)cols_ + (size_t)c]);
            }
        }
    }

    void insert_chars(int n) {
        if (n < 1) n = 1;
        if (cur_r_ < 0 || cur_r_ >= rows_) return;
        if (cur_c_ < 0) cur_c_ = 0;
        if (cur_c_ >= cols_) cur_c_ = cols_ - 1;
        if (n > cols_ - cur_c_) n = cols_ - cur_c_;
        for (int c = cols_ - 1; c >= cur_c_ + n; --c) {
            cells_[(size_t)cur_r_ * (size_t)cols_ + (size_t)c] = cells_[(size_t)cur_r_ * (size_t)cols_ + (size_t)(c - n)];
        }
        for (int c = cur_c_; c < cur_c_ + n; ++c) {
            set_cell_blank_to_current(cells_[(size_t)cur_r_ * (size_t)cols_ + (size_t)c]);
        }
    }

    void delete_chars(int n) {
        if (n < 1) n = 1;
        if (cur_r_ < 0 || cur_r_ >= rows_) return;
        if (cur_c_ < 0) cur_c_ = 0;
        if (cur_c_ >= cols_) cur_c_ = cols_ - 1;
        if (n > cols_ - cur_c_) n = cols_ - cur_c_;
        for (int c = cur_c_; c < cols_ - n; ++c) {
            cells_[(size_t)cur_r_ * (size_t)cols_ + (size_t)c] = cells_[(size_t)cur_r_ * (size_t)cols_ + (size_t)(c + n)];
        }
        for (int c = cols_ - n; c < cols_; ++c) {
            set_cell_blank_to_current(cells_[(size_t)cur_r_ * (size_t)cols_ + (size_t)c]);
        }
    }

    void erase_chars(int n) {
        if (n < 1) n = 1;
        if (cur_r_ < 0 || cur_r_ >= rows_) return;
        if (cur_c_ < 0) cur_c_ = 0;
        if (cur_c_ >= cols_) cur_c_ = cols_ - 1;
        if (n > cols_ - cur_c_) n = cols_ - cur_c_;
        for (int c = cur_c_; c < cur_c_ + n; ++c) {
            set_cell_blank_to_current(cells_[(size_t)cur_r_ * (size_t)cols_ + (size_t)c]);
        }
    }

    void index() {
        wrap_pending_ = false;
        if (cur_r_ < scroll_bottom_) {
            cur_r_++;
            return;
        }
        scroll_up(1);
    }

    void reverse_index() {
        wrap_pending_ = false;
        if (cur_r_ > scroll_top_) {
            cur_r_--;
            return;
        }
        scroll_down(1);
    }

    void next_line() {
        cur_c_ = 0;
        index();
    }

    void save_cursor() {
        saved_r_ = cur_r_;
        saved_c_ = cur_c_;
    }

    void restore_cursor() {
        cur_r_ = saved_r_;
        cur_c_ = saved_c_;
        if (cur_r_ < 0) cur_r_ = 0;
        if (cur_c_ < 0) cur_c_ = 0;
        if (cur_r_ >= rows_) cur_r_ = rows_ - 1;
        if (cur_c_ >= cols_) cur_c_ = cols_ - 1;
    }

    std::string render_text() const {
        std::string out;
        out.reserve((size_t)rows_ * (size_t)(cols_ + 16) * 2);
        vterm::Color last_fg{};
        vterm::Color last_bg{};
        uint8_t last_attrs = 0;
        bool have_last = false;
        for (int r = 0; r < rows_; ++r) {
            for (int c = 0; c < cols_; ++c) {
                const Cell& cell = cells_[(size_t)r * (size_t)cols_ + (size_t)c];
                if (cell.wide == 2) continue;
                char32_t ch = cell.ch;
                if (ch == 0) ch = U' ';

                if (!have_last || cell.fg != last_fg || cell.bg != last_bg || cell.attrs != last_attrs) {
                    out += "\033[0m";
                    if (cell.attrs & AttrBold) out += "\033[1m";
                    if (cell.attrs & AttrUnderline) out += "\033[4m";
                    if (cell.attrs & AttrInverse) out += "\033[7m";
                    vterm::append_color_sgr(out, true, cell.fg);
                    vterm::append_color_sgr(out, false, cell.bg);
                    last_fg = cell.fg;
                    last_bg = cell.bg;
                    last_attrs = cell.attrs;
                    have_last = true;
                }
                out += vterm::utf8_encode(ch);
            }
            if (r != rows_ - 1) out += "\r\n";
        }
        return out;
    }

    std::string render_row(int r) const {
        if (r < 0 || r >= rows_ || cols_ <= 0) return "";
        std::string out;
        out.reserve((size_t)cols_ * 2 + 32);
        out += "\033[0m";

        vterm::Color last_fg{};
        vterm::Color last_bg{};
        uint8_t last_attrs = 0;
        bool have_last = false;

        for (int c = 0; c < cols_; ++c) {
            const Cell& cell = cells_[(size_t)r * (size_t)cols_ + (size_t)c];
            if (cell.wide == 2) continue;
            char32_t ch = cell.ch;
            if (ch == 0) ch = U' ';

            if (!have_last || cell.fg != last_fg || cell.bg != last_bg || cell.attrs != last_attrs) {
                out += "\033[0m";
                if (cell.attrs & AttrBold) out += "\033[1m";
                if (cell.attrs & AttrUnderline) out += "\033[4m";
                if (cell.attrs & AttrInverse) out += "\033[7m";
                vterm::append_color_sgr(out, true, cell.fg);
                vterm::append_color_sgr(out, false, cell.bg);
                last_fg = cell.fg;
                last_bg = cell.bg;
                last_attrs = cell.attrs;
                have_last = true;
            }

            out += vterm::utf8_encode(ch);
        }

        out += "\033[0m";
        return out;
    }

    std::string render_row(int r, int cursor_r, int cursor_c, bool cursor_on) const {
        if (r < 0 || r >= rows_ || cols_ <= 0) return "";
        std::string out;
        out.reserve((size_t)cols_ * 2 + 48);
        out += "\033[0m";

        bool cursor_row = cursor_on && (cursor_r == r);
        int cursor_col = cursor_c;
        if (cursor_row && cursor_col >= 0 && cursor_col < cols_) {
            const Cell& cursor_cell = cells_[(size_t)r * (size_t)cols_ + (size_t)cursor_col];
            if (cursor_cell.wide == 2) cursor_col -= 1;
        }

        vterm::Color last_fg{};
        vterm::Color last_bg{};
        uint8_t last_attrs = 0;
        bool have_last = false;

        for (int c = 0; c < cols_; ++c) {
            const Cell& cell = cells_[(size_t)r * (size_t)cols_ + (size_t)c];
            if (cell.wide == 2) continue;
            char32_t ch = cell.ch;
            if (ch == 0) ch = U' ';

            uint8_t attrs = cell.attrs;
            if (cursor_row && c == cursor_col) {
                attrs |= AttrUnderline;
                if (ch == U' ') ch = U'\u2581';
            }

            if (!have_last || cell.fg != last_fg || cell.bg != last_bg || attrs != last_attrs) {
                out += "\033[0m";
                if (attrs & AttrBold) out += "\033[1m";
                if (attrs & AttrUnderline) out += "\033[4m";
                if (attrs & AttrInverse) out += "\033[7m";
                vterm::append_color_sgr(out, true, cell.fg);
                vterm::append_color_sgr(out, false, cell.bg);
                last_fg = cell.fg;
                last_bg = cell.bg;
                last_attrs = attrs;
                have_last = true;
            }

            out += vterm::utf8_encode(ch);
        }

        out += "\033[0m";
        return out;
    }

private:
    enum : uint8_t {
        AttrBold = 1 << 0,
        AttrUnderline = 1 << 1,
        AttrInverse = 1 << 2,
    };

    static void set_cell_blank(Cell& cell) {
        cell.ch = U' ';
        cell.fg = vterm::Color{};
        cell.bg = vterm::Color{};
        cell.attrs = 0;
        cell.wide = 0;
    }

    void set_cell_blank_to_current(Cell& cell) {
        cell.ch = U' ';
        cell.fg = cur_fg_;
        cell.bg = cur_bg_;
        cell.attrs = cur_attrs_;
        cell.wide = 0;
    }

    void reset_attrs() {
        cur_fg_ = vterm::Color{};
        cur_bg_ = vterm::Color{};
        cur_attrs_ = 0;
    }

    void clear_all() {
        for (auto& cell : cells_) set_cell_blank(cell);
        cur_r_ = 0;
        cur_c_ = 0;
        scroll_top_ = 0;
        scroll_bottom_ = rows_ > 0 ? (rows_ - 1) : 0;
        saved_r_ = 0;
        saved_c_ = 0;
        wrap_pending_ = false;
    }

    void clear_wide_at(int r, int c) {
        if (r < 0 || r >= rows_ || c < 0 || c >= cols_) return;
        Cell& cell = cells_[(size_t)r * (size_t)cols_ + (size_t)c];
        if (cell.wide == 1) {
            set_cell_blank(cell);
            if (c + 1 < cols_) {
                Cell& cont = cells_[(size_t)r * (size_t)cols_ + (size_t)(c + 1)];
                if (cont.wide == 2) set_cell_blank(cont);
            }
            return;
        }
        if (cell.wide == 2) {
            set_cell_blank(cell);
            if (c - 1 >= 0) {
                Cell& start = cells_[(size_t)r * (size_t)cols_ + (size_t)(c - 1)];
                if (start.wide == 1) set_cell_blank(start);
            }
            return;
        }
    }

    void newline() {
        cur_c_ = 0;
        if (cur_r_ < scroll_bottom_) {
            cur_r_++;
            return;
        }
        if (scroll_top_ < 0) scroll_top_ = 0;
        if (scroll_bottom_ >= rows_) scroll_bottom_ = rows_ - 1;
        if (scroll_top_ > scroll_bottom_) {
            scroll_top_ = 0;
            scroll_bottom_ = rows_ - 1;
        }
        for (int r = scroll_top_; r < scroll_bottom_; ++r) {
            for (int c = 0; c < cols_; ++c) {
                cells_[(size_t)r * (size_t)cols_ + (size_t)c] = cells_[(size_t)(r + 1) * (size_t)cols_ + (size_t)c];
            }
        }
        for (int c = 0; c < cols_; ++c) {
            set_cell_blank(cells_[(size_t)scroll_bottom_ * (size_t)cols_ + (size_t)c]);
        }
    }

    int rows_ = 0;
    int cols_ = 0;
    int cur_r_ = 0;
    int cur_c_ = 0;
    int saved_r_ = 0;
    int saved_c_ = 0;
    int scroll_top_ = 0;
    int scroll_bottom_ = 0;
    std::vector<Cell> cells_{};

    vterm::Color cur_fg_{};
    vterm::Color cur_bg_{};
    uint8_t cur_attrs_ = 0;
    bool cursor_visible_ = true;
    bool wrap_pending_ = false;
    bool in_alt_screen_ = false;
    bool autowrap_ = true;
    bool origin_mode_ = false;

    std::vector<Cell> saved_main_cells_{};
    int saved_main_rows_ = 0;
    int saved_main_cols_ = 0;
    int saved_main_cur_r_ = 0;
    int saved_main_cur_c_ = 0;
    int saved_main_scroll_top_ = 0;
    int saved_main_scroll_bottom_ = 0;
    int saved_main_saved_r_ = 0;
    int saved_main_saved_c_ = 0;
    vterm::Color saved_main_fg_{};
    vterm::Color saved_main_bg_{};
    uint8_t saved_main_attrs_ = 0;
    bool saved_main_cursor_visible_ = true;
    bool saved_main_wrap_pending_ = false;
};
