/**
 * pinyin_file_io.h
 * 拼音数据文件读写接口声明。
 * 提供初始化加载、通用读取建索引、写入并刷新索引的能力。
 */
#ifndef PINYIN_FILE_IO_H
#define PINYIN_FILE_IO_H

#include <string>
#include <vector>
#include <cstdint>
#include "pinyin_data.h"

class CandidateItem;

bool set_pinyin_data_dir(const std::string& dir);
std::string get_pinyin_data_dir();
std::string get_pinyin_map_file_path();
std::string get_user_dict_file_path();
std::string get_char_freq_file_path();

/**
 * @brief 初始化并加载所有数据和索引
 */
void init_pinyin_data();

/**
 * @brief 从文件读取数据并更新缓存和索引
 * @param file_path 文件路径
 * @param lines 存储文件内容的 vector
 * @param index 存储索引的 vector
 */
void load_file_and_build_index(const std::string& file_path, 
                               std::vector<std::string>& lines, 
                               std::vector<PinyinIndexItem>& index);

/**
 * @brief 向文件写入一行数据并触发索引更新
 * @param file_path 文件路径
 * @param content 要写入的内容
 * @param lines 对应的缓存 vector
 * @param index 对应的索引 vector
 */
void write_and_update_index(const std::string& file_path, 
                            const std::string& content,
                            std::vector<std::string>& lines, 
                            std::vector<PinyinIndexItem>& index);

void write_and_update_index(const std::string& file_path,
                            const CandidateItem& candidate,
                            std::vector<std::string>& lines,
                            std::vector<PinyinIndexItem>& index);

bool delete_user_dict_line(int line_number);

enum class WeightTargetFile {
    UserDict,
    CharFreq
};

/**
 * @brief 更新指定行的时间戳与计数（count+1、timestamp=now）。
 * 目标文件可选：
 * - data/user_dict.txt
 * - data/char_freq.txt
 *
 * 对应行格式支持：
 * - pinyin_csv text
 * - pinyin_csv text count
 * - pinyin_csv text timestamp count
 * - pinyin text count
 *
 * 更新后统一写回 4 列格式：pinyin_csv text timestamp count。
 *
 * @param target_file 目标文件类型。
 * @param line_number 词库真实行号（1-based）。
 */
void update_timestamp_by_line(WeightTargetFile target_file, int line_number);

int get_weight_by_line(WeightTargetFile target_file, int line_number);

// ---- 延迟写盘支持 ----
// 词库变更只更新内存并 mark dirty（毫秒级），由事件循环在空闲点调用
// flush_dirty_dicts() 统一落盘（64MB 全量重写不阻塞输入）。
// wait_pending_persists() 在退出前调用，保证词库落盘。
void mark_dict_dirty(WeightTargetFile target_file);
void flush_dirty_dicts();
void wait_pending_persists();
bool has_pending_dict_writes();
// 事件循环空闲点调用：脏且超过去抖窗口（400ms）才落盘。
void flush_dirty_dicts_if_idle();

// ---- 词库 parts 硬盘缓存（mmap）----
// 二进制格式：词库每行的拼音段数组序列化到 cache 文件，mmap 映射按需加载，
// 避免匹配时对每行反复按需 split（百万行词库的主要开销）。
// 构建：init_pinyin_data 时生成（词库文件变化则重建）。
// 访问：match 时用 get_parts_cached_line() 读某行段数据。
void build_parts_cache();                 // 生成 cache 文件并 mmap
const char* get_parts_cached_line(int line_index);  // 返回第 line_index 行(0-based)的数据指针
uint8_t get_parts_cached_seg_count(const char* p);  // 读段数
const char* get_parts_cached_seg(const char* p, int seg_index, int& seg_len);  // 读第 seg 段
void invalidate_parts_cache();            // 插入/删除后调用：unmap，匹配回退按需 split

#endif // PINYIN_FILE_IO_H
