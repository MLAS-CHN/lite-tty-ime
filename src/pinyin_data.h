/**
 * pinyin_data.h
 * 拼音数据全局结构与变量声明。
 * 包含索引结构定义，以及字库/词库缓存与索引的 extern 声明。
 */
#ifndef PINYIN_DATA_H
#define PINYIN_DATA_H

#include <vector>
#include <string>
#include <unordered_map>

// 索引项结构：[起始字符(转为int), 起始行, 结束行]
struct PinyinIndexItem {
    int start_char; // 比如 'a'
    int start_line;
    int end_line;
};

// 全局索引，对应 pinyin_map.txt
extern std::vector<PinyinIndexItem> g_pinyin_map_index;

// 全局索引，对应 user_dict.txt
extern std::vector<PinyinIndexItem> g_user_dict_index;

// 全局索引，对应 char_freq.txt
extern std::vector<PinyinIndexItem> g_char_freq_index;

// 缓存文件内容，避免频繁 IO
extern std::vector<std::string> g_pinyin_map_lines;
extern std::vector<std::string> g_user_dict_lines;
extern std::vector<std::string> g_char_freq_lines;

// 单字库查找表：key = "拼音 文本"，O(1) 取行号/时间戳/计数。
struct CharFreqLookupEntry {
    int line_number;
    long long timestamp;
    int count;
};
extern std::unordered_map<std::string, CharFreqLookupEntry> g_char_freq_lookup;

#endif // PINYIN_DATA_H
