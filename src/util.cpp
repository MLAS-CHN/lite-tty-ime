#include "util.h"
#include <sys/ioctl.h>
#include <unistd.h>
#include <fstream>
#include <chrono>
#include <iomanip>
#include <ctime>
#include <cstdint>
#include <sstream>

static std::string g_log_file_path = "lite-tty-ime.log";
static LogLevel g_min_log_level = INFO;

/**
 * 辅助函数实现
 */

/**
 * 获取当前终端尺寸。
 *
 * @param rows 输出：终端行数。
 * @param cols 输出：终端列数。
 */
void get_terminal_size(int &rows, int &cols) {
    struct winsize ws;
    // ioctl 是一个底层的系统调用，用于获取设备信息
    // TIOCGWINSZ 表示 "Terminal IO Control Get Window Size"
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0) {
        rows = ws.ws_row;
        cols = ws.ws_col;
    } else {
        // 如果获取失败，设置一个默认值，防止程序崩溃
        rows = 24;
        cols = 80;
    }
}

void init_logger() {
    auto now = std::chrono::system_clock::now();
    auto now_time = std::chrono::system_clock::to_time_t(now);
    std::tm tm_now = *std::localtime(&now_time);

    std::ostringstream oss;
    oss << "lite-tty-ime_" << std::put_time(&tm_now, "%Y%m%d_%H%M%S") << ".log";
    g_log_file_path = oss.str();

    std::ofstream ofs(g_log_file_path, std::ios::trunc);
    (void)ofs;
}

/**
 * 统一日志写入入口。
 * 以追加模式写入 `lite-tty-ime.log`，并带时间戳与日志等级。
 *
 * @param message 日志正文。
 * @param level 日志等级。
 */
void write_log(const std::string& message, LogLevel level) {
    if (level < g_min_log_level) return;
    // 打开文件（追加模式）
    // 直接使用字符串字面量，避免 static string 在 atexit 时可能已析构的问题
    std::ofstream ofs(g_log_file_path.c_str(), std::ios::app);
    if (!ofs.is_open()) return;

    // 获取当前时间
    auto now = std::chrono::system_clock::now();
    auto now_time = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    // 格式化日志等级
    std::string level_str;
    switch (level) {
        case DEBUG: level_str = "DEBUG"; break;
        case INFO:  level_str = "INFO "; break;
        case WARN:  level_str = "WARN "; break;
        case ERROR: level_str = "ERROR"; break;
    }

    // 写入日志：[2023-04-25 10:00:00.123] [INFO ] Message
    ofs << "[" << std::put_time(std::localtime(&now_time), "%Y-%m-%d %H:%M:%S") 
        << "." << std::setfill('0') << std::setw(3) << ms.count() << "] "
        << "[" << level_str << "] " 
        << message << std::endl;
}

/**
 * 将原始按键字节流转为可读键名。
 * 支持普通键、控制键、方向键及常见转义序列。
 *
 * @param buf 原始输入缓冲区。
 * @param len 有效字节数。
 * @return 规范化键名字符串。
 */
std::string get_key_name(const char* buf, ssize_t len) {
    if (len <= 0) return "Unknown";

    unsigned char c = (unsigned char)buf[0];

    // 单字节普通按键
    if (len == 1) {
        if (c == 0) return "Ctrl+Space"; // 或者叫 NUL
        if (c == 32) return "Space";
        if (c == 9) return "Tab";
        if (c == 13 || c == 10) return "Enter";
        if (c == 27) return "Escape";
        if (c == 127 || c == 8) return "Backspace";
        
        // Ctrl + A-Z (1-26)
        if (c >= 1 && c <= 26) {
            std::string res = "Ctrl+";
            res += (char)('A' + c - 1);
            return res;
        }

        // Ctrl + 其他 (比如 Ctrl+])
        if (c == 29) return "Ctrl+]";

        // 可打印字符
        if (c >= 33 && c <= 126) {
            return std::string(1, (char)c);
        }
        
        char hex[16];
        sprintf(hex, "Key(0x%02X)", c);
        return hex;
    }

    // 多字节序列 (通常是功能键或转义序列)
    std::string seq(buf, len);
    if (seq == "\033[A") return "Up";
    if (seq == "\033[B") return "Down";
    if (seq == "\033[C") return "Right";
    if (seq == "\033[D") return "Left";
    if (seq == "\033[H") return "Home";
    if (seq == "\033[F") return "End";
    if (seq == "\033[3~") return "Delete";
    if (seq == "\033[5~") return "PageUp";
    if (seq == "\033[6~") return "PageDown";

    // 如果无法识别，返回十六进制
    std::string hex_str = "Seq(";
    for (ssize_t i = 0; i < len; ++i) {
        char h[8];
        sprintf(h, "%02X ", (unsigned char)buf[i]);
        hex_str += h;
    }
    hex_str.back() = ')';
    return hex_str;
}

static bool decode_utf8_codepoint(const std::string& s, size_t offset, uint32_t& codepoint, size_t& step) {
    if (offset >= s.size()) return false;
    unsigned char c0 = (unsigned char)s[offset];
    if ((c0 & 0x80) == 0) {
        codepoint = c0;
        step = 1;
        return true;
    }
    if ((c0 & 0xE0) == 0xC0) {
        if (offset + 2 > s.size()) return false;
        unsigned char c1 = (unsigned char)s[offset + 1];
        if ((c1 & 0xC0) != 0x80) return false;
        codepoint = ((uint32_t)(c0 & 0x1F) << 6) | (uint32_t)(c1 & 0x3F);
        step = 2;
        return true;
    }
    if ((c0 & 0xF0) == 0xE0) {
        if (offset + 3 > s.size()) return false;
        unsigned char c1 = (unsigned char)s[offset + 1];
        unsigned char c2 = (unsigned char)s[offset + 2];
        if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80) return false;
        codepoint = ((uint32_t)(c0 & 0x0F) << 12) | ((uint32_t)(c1 & 0x3F) << 6) | (uint32_t)(c2 & 0x3F);
        step = 3;
        return true;
    }
    if ((c0 & 0xF8) == 0xF0) {
        if (offset + 4 > s.size()) return false;
        unsigned char c1 = (unsigned char)s[offset + 1];
        unsigned char c2 = (unsigned char)s[offset + 2];
        unsigned char c3 = (unsigned char)s[offset + 3];
        if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80) return false;
        codepoint = ((uint32_t)(c0 & 0x07) << 18) | ((uint32_t)(c1 & 0x3F) << 12) |
                    ((uint32_t)(c2 & 0x3F) << 6) | (uint32_t)(c3 & 0x3F);
        step = 4;
        return true;
    }
    return false;
}

static bool is_candidate_superscript(uint32_t codepoint) {
    switch (codepoint) {
        case 0x00B9: // ¹
        case 0x00B2: // ²
        case 0x00B3: // ³
        case 0x2070: // ⁰
        case 0x2074: // ⁴
        case 0x2075: // ⁵
        case 0x2076: // ⁶
        case 0x2077: // ⁷
        case 0x2078: // ⁸
        case 0x2079: // ⁹
            return true;
        default:
            return false;
    }
}

/**
 * 计算字符串的显示列宽（粗略估算）。
 * 规则：
 * - 以第一个候选上标数字为分界点（¹/²/³/⁰-⁹）。
 * - 分界点之前：ASCII=1 列，非 ASCII=2 列。
 * - 分界点之后：上标数字与空格=1 列，其余全部按全角=2 列（包括 ASCII）。
 *
 * @param s UTF-8 字符串。
 * @return 估算后的显示列宽。
 */
int get_display_width(const std::string& s) {
    int width = 0;
    bool in_candidates = false;
    for (size_t i = 0; i < s.size();) {
        uint32_t cp = 0;
        size_t step = 1;
        if (!decode_utf8_codepoint(s, i, cp, step)) {
            cp = (unsigned char)s[i];
            step = 1;
        }

        if (!in_candidates && is_candidate_superscript(cp)) {
            in_candidates = true;
        }

        if (!in_candidates) {
            width += (cp <= 0x7F) ? 1 : 2;
        } else {
            if (cp == 0x20 || is_candidate_superscript(cp)) {
                width += 1;
            } else {
                width += 2;
            }
        }

        i += step;
    }
    return width;
}

/**
 * 按“显示列宽”裁剪 UTF-8 字符串，返回可安全截断的字节下标。
 * 规则同 get_display_width。
 *
 * @param s UTF-8 字符串。
 * @param max_width 允许的最大显示列宽。
 * @return 可用于 string::resize 的字节下标（不会截断到 UTF-8 字符中间）。
 */
size_t get_utf8_cut_index_by_width(const std::string& s, int max_width) {
    int width = 0;
    size_t i = 0;
    bool in_candidates = false;
    while (i < s.size()) {
        uint32_t cp = 0;
        size_t step = 1;
        if (!decode_utf8_codepoint(s, i, cp, step)) {
            cp = (unsigned char)s[i];
            step = 1;
        }

        if (!in_candidates && is_candidate_superscript(cp)) {
            in_candidates = true;
        }

        int char_width = 0;
        if (!in_candidates) {
            char_width = (cp <= 0x7F) ? 1 : 2;
        } else {
            if (cp == 0x20 || is_candidate_superscript(cp)) {
                char_width = 1;
            } else {
                char_width = 2;
            }
        }

        if (width + char_width > max_width) break;
        width += char_width;
        i += step;
    }
    return i;
}
