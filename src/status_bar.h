#ifndef STATUS_BAR_H
#define STATUS_BAR_H

#include <string>

/**
 * 状态栏管理模块
 * 负责状态栏的内容逻辑管理
 */

// 初始化状态栏（设置默认常态文字等）
void init_status_bar();

void set_status_bar_colors(int fg_ansi, int bg_ansi);

// 刷新状态栏内容（根据当前状态重新拼接字符串并写入底端）
void refresh_status_bar();

/**
 * 更新输入法状态
 * @param mode_name 模式名称（如 "EN", "中文"）
 * @param buffer 当前拼音缓冲区内容
 */
void update_ime_status(const std::string& mode_name, const std::string& buffer);

#endif // STATUS_BAR_H
