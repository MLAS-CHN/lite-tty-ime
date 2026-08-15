/**
 * pinyin_data.cpp
 * 拼音数据全局缓存定义文件。
 * 定义字库/词库的行缓存与首字母索引全局变量。
 */
#include "pinyin_data.h"

// 定义全局变量
std::vector<PinyinIndexItem> g_pinyin_map_index;
std::vector<PinyinIndexItem> g_user_dict_index;
std::vector<PinyinIndexItem> g_char_freq_index;

std::vector<std::string> g_pinyin_map_lines;
std::vector<std::string> g_user_dict_lines;
std::vector<std::string> g_char_freq_lines;

std::unordered_map<std::string, CharFreqLookupEntry> g_char_freq_lookup;
