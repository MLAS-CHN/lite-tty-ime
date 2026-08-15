/**
 * pinyin_virtual_cursor.h
 * 拼音缓冲虚拟光标操作接口。
 */
#ifndef PINYIN_VIRTUAL_CURSOR_H
#define PINYIN_VIRTUAL_CURSOR_H

#include <string>
#include <cstddef>

// 将虚拟光标重置到缓冲区右侧（buffer.length）
void reset_virtual_cursor_to_end(const std::string& buffer, size_t& virtual_cursor);

// 虚拟光标左移一位（最小到 0）
void move_virtual_cursor_left(size_t& virtual_cursor);

// 虚拟光标右移一位（最大到 buffer.length）
void move_virtual_cursor_right(size_t& virtual_cursor, const std::string& buffer);

// 在虚拟光标左侧（即当前位置）插入字符，并将光标右移
void insert_at_virtual_cursor(std::string& buffer, size_t& virtual_cursor, char ch);

// 检查是否允许在虚拟光标位置插入分词符单引号(')
bool can_insert_word_separator_at_virtual_cursor(const std::string& buffer, size_t virtual_cursor);

// 删除虚拟光标左侧一个字符（Backspace 语义），删除成功返回 true
bool backspace_at_virtual_cursor(std::string& buffer, size_t& virtual_cursor);

// 检查从偏移处开始是否匹配 Ctrl+Left 序列，匹配时返回 true 并输出消费字节数
bool match_ctrl_left_sequence(const char* buf, ssize_t total_len, ssize_t offset, ssize_t& consumed_len);

// 检查从偏移处开始是否匹配 Ctrl+Right 序列，匹配时返回 true 并输出消费字节数
bool match_ctrl_right_sequence(const char* buf, ssize_t total_len, ssize_t offset, ssize_t& consumed_len);

// 检查从偏移处开始是否匹配 Delete 序列，匹配时返回 true 并输出消费字节数
bool match_delete_sequence(const char* buf, ssize_t total_len, ssize_t offset, ssize_t& consumed_len);

#endif // PINYIN_VIRTUAL_CURSOR_H
