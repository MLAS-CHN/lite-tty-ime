#ifndef TUI_SHELL_H
#define TUI_SHELL_H

#include <string>

/**
 * 状态栏控制接口
 * 定义了用于操作底部状态栏的公开函数
 */

// 设置底部状态栏的背景颜色 (ANSI 颜色代码，如 47 是白色，44 是蓝色)
void set_status_bar_color(int ansi_color);

// 设置底部状态栏的前景颜色 (文字颜色，如 30 是黑色，31 是红色)
void set_status_bar_fg_color(int ansi_color);

// 向底部状态栏写入文字 (std::string 版本)
void write_status_bar(const std::string& text);

// 向底部状态栏写入文字 (C 风格字符串版本，方便调试时调用)
void write_status_bar(const char* text);

// 读取当前底部状态栏显示的文字内容 (Getter)
std::string get_status_bar_text();

#endif // TUI_SHELL_H
