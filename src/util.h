#ifndef UTIL_H
#define UTIL_H

#include <cstddef>
#include <string>
#include <vector>

/**
 * 从词典行中提取拼音键（空格前部分）。
 */
inline std::string get_pinyin_from_line(const std::string& line) {
    size_t space_pos = line.find(' ');
    if (space_pos == std::string::npos) return line;
    return line.substr(0, space_pos);
}

/**
 * 日志等级
 */
enum LogLevel {
    DEBUG,
    INFO,
    WARN,
    ERROR
};

/**
 * 通用工具函数接口
 * 包含与终端交互相关的辅助函数
 */

// 获取当前终端的尺寸（行数和列数）
// 通过引用参数 rows 和 cols 返回结果
void get_terminal_size(int &rows, int &cols);

// 写入日志
// 参数：message - 日志内容，level - 日志等级
void write_log(const std::string& message, LogLevel level = INFO);

void init_logger();

/**
 * 将按键序列转换为可读名称
 */
std::string get_key_name(const char* buf, ssize_t len);

/**
 * 计算字符串的显示列宽（粗略估算）。
 * 规则：ASCII 字符按 1 列；非 ASCII 字符按 2 列。
 *
 * @param s UTF-8 字符串。
 * @return 估算后的显示列宽。
 */
int get_display_width(const std::string& s);

/**
 * 按“显示列宽”裁剪 UTF-8 字符串，返回可安全截断的字节下标。
 * 规则同 get_display_width：ASCII=1 列，非 ASCII=2 列。
 *
 * @param s UTF-8 字符串。
 * @param max_width 允许的最大显示列宽。
 * @return 可用于 string::resize 的字节下标（不会截断到 UTF-8 字符中间）。
 */
size_t get_utf8_cut_index_by_width(const std::string& s, int max_width);

/**
 * 以逗号分割字符串。
 */
inline std::vector<std::string> split_csv(const std::string& csv) {
    std::vector<std::string> parts;
    std::string current;
    for (char c : csv) {
        if (c == ',') {
            if (!current.empty()) parts.push_back(current);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) parts.push_back(current);
    return parts;
}

/**
 * 将字符串数组以逗号拼接。
 */
inline std::string join_csv(const std::vector<std::string>& parts) {
    std::string res;
    for (size_t i = 0; i < parts.size(); ++i) {
        res += parts[i];
        if (i + 1 < parts.size()) res += ",";
    }
    return res;
}

#endif // UTIL_H
