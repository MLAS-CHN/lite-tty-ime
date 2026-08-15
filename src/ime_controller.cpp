#include "ime_controller.h"
#include "candidate_item.h"
#include "pinyin_composition.h"
#include "pinyin_data.h"
#include "pinyin_file_io.h"
#include "pinyin_virtual_cursor.h"
#include "status_bar.h"
#include "util.h"
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

static const size_t kCandidatePageSize = 5;

struct ImeState {
    bool is_chinese_mode = false;
    bool is_delete_mode = false;
    std::string pinyin_buffer;
    size_t virtual_cursor = 0;
    std::vector<std::vector<CandidateItem>> current_candidates;
    size_t candidate_page = 0;
    std::vector<CandidateItem> continuous_input_container;
    bool is_continuous_input_mode = false;
    std::string pending_escape;
};

/**
 * 刷新预编辑显示与候选分页缓存，并通过状态栏模块更新底边栏内容。
 * - pinyin_buffer 为空时必须清空候选缓存，避免“隐形候选”仍可被数字键选中。
 * - reset_page 为 true 时会把候选页码归零。
 * - is_delete_mode 为 true 时会在状态栏的模式标签后展示 'x' 标记。
 */
static void update_composing_display(const std::string& pinyin_buffer,
                                     size_t virtual_cursor,
                                     std::vector<std::vector<CandidateItem>>& paged_candidates,
                                     size_t& candidate_page,
                                     bool reset_page,
                                     bool is_delete_mode) {
    if (pinyin_buffer.empty()) {
        paged_candidates.clear();
        candidate_page = 0;
        update_ime_status("中文", "");
        return;
    }

    std::string composing_text = buildComposingDisplayText(
        pinyin_buffer, virtual_cursor, candidate_page, kCandidatePageSize, &paged_candidates);

    if (reset_page) {
        candidate_page = 0;
    }

    if (paged_candidates.empty()) {
        candidate_page = 0;
    } else if (candidate_page >= paged_candidates.size()) {
        candidate_page = paged_candidates.size() - 1;
    }

    composing_text = buildComposingDisplayText(
        pinyin_buffer, virtual_cursor, candidate_page, kCandidatePageSize, &paged_candidates);

    if (is_delete_mode) {
        composing_text = "x" + composing_text;
    }

    update_ime_status("中文", composing_text);
}

/**
 * 根据“选中的候选拼音分段”，尽量消耗用户输入的 pinyin_buffer 前缀。
 * 该逻辑用于实现“连续输入”：选中一个候选后，剩余未消耗的拼音继续生成新候选。
 */
static void consume_pinyin_buffer_by_selected_candidate(std::vector<std::string>& selected_candidate_pinyin,
                                                       std::string& pinyin_buffer) {
    while (!selected_candidate_pinyin.empty() && !pinyin_buffer.empty()) {
        if (!pinyin_buffer.empty() && pinyin_buffer[0] == '\'') {
            pinyin_buffer.erase(0, 1);
            continue;
        }

        if (selected_candidate_pinyin[0].empty()) {
            selected_candidate_pinyin.erase(selected_candidate_pinyin.begin());
            continue;
        }

        if (selected_candidate_pinyin[0][0] == pinyin_buffer[0]) {
            selected_candidate_pinyin[0].erase(0, 1);
            pinyin_buffer.erase(0, 1);
            continue;
        }

        selected_candidate_pinyin.erase(selected_candidate_pinyin.begin());
    }
}

/**
 * 对选中的候选做“权重 +1”，若候选无法定位到源文件行号则落盘到对应词库文件。
 * - 单字候选写入/更新 char_freq
 * - 多拼音分段候选写入/更新 user_dict
 */
static void apply_candidate_weight_or_persist_if_missing(const CandidateItem& selected_candidate) {
    WeightTargetFile target_file = (selected_candidate.getPinyinParts().size() == 1)
                                       ? WeightTargetFile::CharFreq
                                       : WeightTargetFile::UserDict;
    int line_number = selected_candidate.findSourceLineNumber();
    if (line_number > 0) {
        update_timestamp_by_line(target_file, line_number);
        return;
    }

    if (target_file == WeightTargetFile::CharFreq) {
        write_and_update_index(get_char_freq_file_path(), selected_candidate, g_char_freq_lines, g_char_freq_index);
    } else {
        write_and_update_index(get_user_dict_file_path(), selected_candidate, g_user_dict_lines, g_user_dict_index);
    }
}

/**
 * 选中候选后的状态流转：
 * - 若 pinyin_buffer 被消耗为空：清空候选/退出删除模式，必要时把连续输入容器合并写入 user_dict。
 * - 若 pinyin_buffer 仍有剩余：刷新候选并进入/保持连续输入模式。
 */
static void finalize_or_continue_continuous_input(ImeState& state, const CandidateItem& selected_candidate) {
    if (state.pinyin_buffer.empty()) {
        state.is_delete_mode = false;
        state.virtual_cursor = 0;
        state.current_candidates.clear();
        state.candidate_page = 0;
        update_ime_status("中文", "");

        if (state.is_continuous_input_mode) {
            state.continuous_input_container.push_back(selected_candidate);
            CandidateItem merged_candidate = CandidateItem::mergeCandidateItems(state.continuous_input_container);
            if (!merged_candidate.getText().empty() && !merged_candidate.getPinyinParts().empty()) {
                write_and_update_index(get_user_dict_file_path(), merged_candidate, g_user_dict_lines, g_user_dict_index);
                write_log("Persist merged candidate to user_dict: " + merged_candidate.toString(), INFO);
            }
            state.continuous_input_container.clear();
            state.is_continuous_input_mode = false;
        }

        return;
    }

    state.virtual_cursor = state.pinyin_buffer.size();
    state.candidate_page = 0;
    update_composing_display(state.pinyin_buffer,
                            state.virtual_cursor,
                            state.current_candidates,
                            state.candidate_page,
                            true,
                            state.is_delete_mode);
    state.continuous_input_container.push_back(selected_candidate);
    state.is_continuous_input_mode = true;
}

/**
 * 处理 Delete 键切换删除模式。
 * 仅当当前存在候选时才允许进入/退出删除模式，避免无候选时误触发 UI 状态。
 *
 * @param consumed_len 识别到多字节序列时，告知调用方应跳过的字节数。
 */
static bool handle_delete_mode_toggle_key(const char* buf,
                                         ssize_t total_len,
                                         ssize_t offset,
                                         ssize_t& consumed_len,
                                         ImeState& state) {
    if (state.current_candidates.empty()) return false;
    if (!match_delete_sequence(buf, total_len, offset, consumed_len)) return false;

    state.is_delete_mode = !state.is_delete_mode;
    update_composing_display(state.pinyin_buffer,
                            state.virtual_cursor,
                            state.current_candidates,
                            state.candidate_page,
                            false,
                            state.is_delete_mode);
    return true;
}

/**
 * 处理候选选择键：
 * - 空格：选择当前页第 1 个候选
 * - 1-9：选择当前页第 1-9 个候选
 * - 0：选择当前页第 10 个候选
 *
 * 删除模式下：
 * - 若选中的是“词”候选：删除 user_dict 对应行并更新拼音缓冲与状态栏，退出删除模式。
 * - 若选中的是“字”候选：按正常路径上屏，但会退出删除模式。
 */
static bool handle_candidate_selection_key(unsigned char c, ImeState& state, int master_fd) {
    if (state.current_candidates.empty()) return false;
    if (!(c == ' ' || (c >= '0' && c <= '9'))) return false;

    size_t page_index = state.candidate_page;
    if (page_index >= state.current_candidates.size()) {
        page_index = state.current_candidates.size() - 1;
    }
    const std::vector<CandidateItem>& page_candidates = state.current_candidates[page_index];

    size_t selected_index = 0;
    if (c == ' ') {
        selected_index = 0;
    } else if (c == '0') {
        selected_index = 9;
    } else {
        selected_index = (size_t)(c - '1');
    }

    if (selected_index >= page_candidates.size()) {
        return true;
    }

    CandidateItem selected_candidate = page_candidates[selected_index];
    if (state.is_delete_mode) {
        bool is_word_candidate = selected_candidate.getPinyinParts().size() > 1;
        if (is_word_candidate) {
            int line_number = selected_candidate.findSourceLineNumber();
            if (line_number > 0) {
                delete_user_dict_line(line_number);
            }

            std::vector<std::string> selected_candidate_pinyin = selected_candidate.getPinyinParts();
            consume_pinyin_buffer_by_selected_candidate(selected_candidate_pinyin, state.pinyin_buffer);

            state.is_delete_mode = false;
            if (state.virtual_cursor > state.pinyin_buffer.size()) {
                state.virtual_cursor = state.pinyin_buffer.size();
            }
            update_composing_display(state.pinyin_buffer,
                                    state.virtual_cursor,
                                    state.current_candidates,
                                    state.candidate_page,
                                    true,
                                    state.is_delete_mode);
            return true;
        }

        state.is_delete_mode = false;
    }

    const std::string& selected_text = selected_candidate.getText();
    write_log("Select candidate: " + selected_text, INFO);
    if (!selected_text.empty()) {
        write(master_fd, selected_text.c_str(), selected_text.size());
    }

    apply_candidate_weight_or_persist_if_missing(selected_candidate);

    std::vector<std::string> selected_candidate_pinyin = selected_candidate.getPinyinParts();
    consume_pinyin_buffer_by_selected_candidate(selected_candidate_pinyin, state.pinyin_buffer);
    finalize_or_continue_continuous_input(state, selected_candidate);
    return true;
}

/**
 * 处理候选翻页键：
 * - '='：下一页
 * - '-'：上一页
 *
 * 仅在 pinyin_buffer 非空时生效。
 */
static bool handle_candidate_paging_key(unsigned char c, ImeState& state) {
    if (state.pinyin_buffer.empty()) return false;
    if (!(c == '=' || c == '-')) return false;

    if (c == '=') {
        if (state.candidate_page + 1 < state.current_candidates.size()) {
            ++state.candidate_page;
            update_composing_display(state.pinyin_buffer,
                                    state.virtual_cursor,
                                    state.current_candidates,
                                    state.candidate_page,
                                    false,
                                    state.is_delete_mode);
        }
    } else {
        if (state.candidate_page > 0) {
            --state.candidate_page;
            update_composing_display(state.pinyin_buffer,
                                    state.virtual_cursor,
                                    state.current_candidates,
                                    state.candidate_page,
                                    false,
                                    state.is_delete_mode);
        }
    }
    return true;
}

/**
 * 处理拼音输入键：
 * - a-z：作为拼音字符插入
 * - A-Z：大写字母直接进入缓冲（等价 Shift+字母，便于录入大写英文；
 *        拆分时作为 literal 段参与候选前缀拼接）
 * - '\\''：作为分词符插入（仅在当前位置允许插入时）
 */
static bool handle_pinyin_input_key(unsigned char c, ImeState& state) {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '\'')) return false;

    if (c == '\'' && !can_insert_word_separator_at_virtual_cursor(state.pinyin_buffer, state.virtual_cursor)) {
        return true;
    }
    insert_at_virtual_cursor(state.pinyin_buffer, state.virtual_cursor, (char)c);
    if (state.pinyin_buffer.size() == 1) {
        reset_virtual_cursor_to_end(state.pinyin_buffer, state.virtual_cursor);
    }
    update_composing_display(state.pinyin_buffer,
                            state.virtual_cursor,
                            state.current_candidates,
                            state.candidate_page,
                            true,
                            state.is_delete_mode);
    return true;
}

/**
 * 处理退格键（Backspace）：
 * 在虚拟光标处删除一个字符并刷新候选展示。
 */
static bool handle_backspace_key(unsigned char c, ImeState& state) {
    if (!(c == 127 || c == 8)) return false;
    if (!backspace_at_virtual_cursor(state.pinyin_buffer, state.virtual_cursor)) return false;
    update_composing_display(state.pinyin_buffer,
                            state.virtual_cursor,
                            state.current_candidates,
                            state.candidate_page,
                            true,
                            state.is_delete_mode);
    return true;
}

/**
 * 处理“兜底提交缓冲区”：
 * 当 pinyin_buffer 非空且按下空格/回车时，直接把缓冲区内容原样写入子 PTY，并清空输入法状态。
 */
static bool handle_flush_buffer_key(unsigned char c, ImeState& state, int master_fd) {
    if (state.pinyin_buffer.empty()) return false;
    if (!(c == ' ' || c == 13 || c == 10)) return false;

    write(master_fd, state.pinyin_buffer.c_str(), state.pinyin_buffer.length());
    state.pinyin_buffer.clear();
    state.is_delete_mode = false;
    state.virtual_cursor = 0;
    state.current_candidates.clear();
    state.candidate_page = 0;
    update_ime_status("中文", "");
    return true;
}

static bool handle_direct_fullwidth_punctuation_key(unsigned char c, const ImeState& state, int master_fd) {
    if (!state.is_chinese_mode) return false;
    if (!state.pinyin_buffer.empty()) return false;
    if (!state.current_candidates.empty()) return false;

    const char* text = nullptr;
    switch (c) {
        case '\\':
            text = u8"、";
            break;
        case ',':
            text = u8"，";
            break;
        case '!':
            text = u8"！";
            break;
        case '#':
            text = u8"＃";
            break;
        case '%':
            text = u8"％";
            break;
        case '(':
            text = u8"（";
            break;
        case ')':
            text = u8"）";
            break;
        case '_':
            text = u8"—";
            break;
        case '?':
            text = u8"？";
            break;
        case ';':
            text = u8"；";
            break;
        case ':':
            text = u8"：";
            break;
        case '^':
            text = u8"…";
            break;
        case '<':
            text = u8"《";
            break;
        case '>':
            text = u8"》";
            break;
        case '$':
            text = u8"￥";
            break;
        default:
            return false;
    }

    write(master_fd, text, (size_t)strlen(text));
    return true;
}

static int try_finish_pending_escape_sequence(ImeState& state, std::string& passthrough) {
    const std::string& s = state.pending_escape;
    if (s.empty()) return -1;
    if ((unsigned char)s[0] != 0x1B) {
        passthrough += s;
        state.pending_escape.clear();
        return 0;
    }

    if (s.size() == 1) return -1;
    unsigned char t = (unsigned char)s[1];

    if (t == '[') {
        if (s.size() < 3) return -1;
        unsigned char last = (unsigned char)s.back();
        if (last >= 0x40 && last <= 0x7E) {
            ssize_t consumed_len = 0;
            if (!state.pinyin_buffer.empty() && match_ctrl_left_sequence(s.data(), (ssize_t)s.size(), 0, consumed_len)) {
                move_virtual_cursor_left(state.virtual_cursor);
                update_composing_display(state.pinyin_buffer,
                                        state.virtual_cursor,
                                        state.current_candidates,
                                        state.candidate_page,
                                        false,
                                        state.is_delete_mode);
                state.pending_escape.clear();
                return 1;
            }
            if (!state.pinyin_buffer.empty() && match_ctrl_right_sequence(s.data(), (ssize_t)s.size(), 0, consumed_len)) {
                move_virtual_cursor_right(state.virtual_cursor, state.pinyin_buffer);
                update_composing_display(state.pinyin_buffer,
                                        state.virtual_cursor,
                                        state.current_candidates,
                                        state.candidate_page,
                                        false,
                                        state.is_delete_mode);
                state.pending_escape.clear();
                return 1;
            }
            if (!state.current_candidates.empty() && match_delete_sequence(s.data(), (ssize_t)s.size(), 0, consumed_len)) {
                state.is_delete_mode = !state.is_delete_mode;
                update_composing_display(state.pinyin_buffer,
                                        state.virtual_cursor,
                                        state.current_candidates,
                                        state.candidate_page,
                                        false,
                                        state.is_delete_mode);
                state.pending_escape.clear();
                return 1;
            }

            passthrough += s;
            state.pending_escape.clear();
            return 0;
        }
        return -1;
    }

    if (t == 'O') {
        if (s.size() < 3) return -1;
        passthrough += s;
        state.pending_escape.clear();
        return 0;
    }

    if (t == ']') {
        unsigned char last = (unsigned char)s.back();
        if (last == 0x07) {
            passthrough += s;
            state.pending_escape.clear();
            return 0;
        }
        if (s.size() >= 2 && s[s.size() - 2] == '\x1b' && s[s.size() - 1] == '\\') {
            passthrough += s;
            state.pending_escape.clear();
            return 0;
        }
        return -1;
    }

    if (s.size() >= 2) {
        passthrough += s;
        state.pending_escape.clear();
        return 0;
    }

    return -1;
}

/**
 * 初始化输入法控制模块（加载字库/词库与索引缓存）。
 * 由 tui_shell 在启动阶段调用一次即可。
 */
void ime_controller_init() {
    init_pinyin_data();
}

/**
 * 输入法控制入口：处理一段键盘输入字节流。
 * - 被中文输入逻辑消费的按键会更新输入法状态与状态栏，但不会透传给子 PTY。
 * - 未被消费的输入会原样写入 master_fd 透传给子 shell。
 *
 * @return true 表示这段输入中至少有一部分被输入法拦截处理；false 表示完全透传。
 */
bool ime_controller_handle_input_bytes(const char* buf, ssize_t n, int master_fd) {
    static ImeState ime_state;
    bool intercepted = false;
    std::string passthrough;
    passthrough.reserve((size_t)n);

    auto flush_passthrough = [&]() {
        if (passthrough.empty()) return;
        write(master_fd, passthrough.data(), passthrough.size());
        passthrough.clear();
    };

    for (ssize_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)buf[i];

        if (c == 0) {
            flush_passthrough();
            ime_state.is_chinese_mode = !ime_state.is_chinese_mode;
            ime_state.pinyin_buffer.clear();
            ime_state.is_delete_mode = false;
            ime_state.virtual_cursor = 0;
            ime_state.current_candidates.clear();
            ime_state.candidate_page = 0;
            ime_state.pending_escape.clear();
            update_ime_status(ime_state.is_chinese_mode ? "中文" : "EN", "");
            write_log("Switch to " + std::string(ime_state.is_chinese_mode ? "Chinese" : "English") + " mode", INFO);
            intercepted = true;
            continue;
        }

        if (!ime_state.is_chinese_mode) {
            passthrough.push_back((char)c);
            continue;
        }

        if (!ime_state.pending_escape.empty()) {
            ime_state.pending_escape.push_back((char)c);
            int r = try_finish_pending_escape_sequence(ime_state, passthrough);
            if (r == 1) {
                flush_passthrough();
                intercepted = true;
            }
            continue;
        }
        if (c == 0x1B) {
            ime_state.pending_escape.push_back((char)c);
            int r = try_finish_pending_escape_sequence(ime_state, passthrough);
            if (r == 1) {
                flush_passthrough();
                intercepted = true;
            }
            continue;
        }

        if (!ime_state.pinyin_buffer.empty()) {
            ssize_t consumed_len = 0;
            if (match_ctrl_left_sequence(buf, n, i, consumed_len)) {
                flush_passthrough();
                move_virtual_cursor_left(ime_state.virtual_cursor);
                update_composing_display(ime_state.pinyin_buffer,
                                        ime_state.virtual_cursor,
                                        ime_state.current_candidates,
                                        ime_state.candidate_page,
                                        false,
                                        ime_state.is_delete_mode);
                intercepted = true;
                i += consumed_len - 1;
                continue;
            }
            if (match_ctrl_right_sequence(buf, n, i, consumed_len)) {
                flush_passthrough();
                move_virtual_cursor_right(ime_state.virtual_cursor, ime_state.pinyin_buffer);
                update_composing_display(ime_state.pinyin_buffer,
                                        ime_state.virtual_cursor,
                                        ime_state.current_candidates,
                                        ime_state.candidate_page,
                                        false,
                                        ime_state.is_delete_mode);
                intercepted = true;
                i += consumed_len - 1;
                continue;
            }
        }

        ssize_t consumed_len = 0;
        if (handle_delete_mode_toggle_key(buf, n, i, consumed_len, ime_state)) {
            flush_passthrough();
            intercepted = true;
            i += consumed_len - 1;
            continue;
        }

        if (handle_candidate_paging_key(c, ime_state)) {
            flush_passthrough();
            intercepted = true;
            continue;
        }

        if (handle_candidate_selection_key(c, ime_state, master_fd)) {
            flush_passthrough();
            intercepted = true;
            continue;
        }

        if (handle_pinyin_input_key(c, ime_state)) {
            flush_passthrough();
            intercepted = true;
            continue;
        } else if (!ime_state.pinyin_buffer.empty() && c >= 32 && c <= 126) {
            flush_passthrough();
            intercepted = true;
            continue;
        } else if (handle_backspace_key(c, ime_state)) {
            flush_passthrough();
            intercepted = true;
            continue;
        } else if (handle_flush_buffer_key(c, ime_state, master_fd)) {
            flush_passthrough();
            intercepted = true;
            continue;
        } else if (handle_direct_fullwidth_punctuation_key(c, ime_state, master_fd)) {
            flush_passthrough();
            intercepted = true;
            continue;
        }

        passthrough.push_back((char)c);
    }

    flush_passthrough();

    return intercepted;
}
