/**
 * pinyin_file_io.cpp
 * 拼音数据文件读写实现文件。
 * 负责加载字库/词库文本、构建首字母索引，并在写入后刷新缓存。
 */
#include "pinyin_file_io.h"
#include "candidate_item.h"
#include "util.h"
#include <fstream>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <cctype>
#include <cstdint>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static std::string g_data_dir = "";

static std::string join_path(const std::string& dir, const std::string& file) {
    if (dir.empty()) return file;
    if (dir.back() == '/') return dir + file;
    return dir + "/" + file;
}

static bool file_exists(const std::string& file_path) {
    std::ifstream f(file_path);
    return f.is_open();
}

static bool data_dir_valid(const std::string& dir) {
    return file_exists(join_path(dir, "pinyin_map.txt")) &&
           file_exists(join_path(dir, "user_dict.txt")) &&
           file_exists(join_path(dir, "char_freq.txt"));
}

static std::string resolve_default_data_dir() {
    std::vector<std::string> candidates;
    candidates.push_back("/usr/share/lite-tty-ime/data");
    const char* home = getenv("HOME");
    if (home && *home) candidates.push_back(std::string(home) + "/.lite-tty-ime/data");
    candidates.push_back("data");

    for (const auto& dir : candidates) {
        if (data_dir_valid(dir)) return dir;
    }
    return "data";
}

static void ensure_data_dir_resolved() {
    if (!g_data_dir.empty()) return;
    g_data_dir = resolve_default_data_dir();
}

bool set_pinyin_data_dir(const std::string& dir) {
    if (dir.empty()) return false;
    if (!data_dir_valid(dir)) return false;
    g_data_dir = dir;
    return true;
}

std::string get_pinyin_data_dir() {
    ensure_data_dir_resolved();
    return g_data_dir;
}

std::string get_pinyin_map_file_path() {
    ensure_data_dir_resolved();
    return join_path(g_data_dir, "pinyin_map.txt");
}

std::string get_user_dict_file_path() {
    ensure_data_dir_resolved();
    return join_path(g_data_dir, "user_dict.txt");
}

std::string get_char_freq_file_path() {
    ensure_data_dir_resolved();
    return join_path(g_data_dir, "char_freq.txt");
}

static bool is_all_digits(const std::string& s) {
    if (s.empty()) return false;
    for (unsigned char c : s) {
        if (!std::isdigit(c)) return false;
    }
    return true;
}

static void build_index_from_lines(const std::vector<std::string>& lines,
                                   std::vector<PinyinIndexItem>& index) {
    index.clear();
    if (lines.empty()) return;

    int current_line_num = 1;
    char current_char = '\0';
    int start_line = -1;

    for (const auto& line : lines) {
        if (line.empty()) {
            ++current_line_num;
            continue;
        }

        char first_char = line[0];
        if (first_char != current_char) {
            if (current_char != '\0') {
                index.push_back({(int)current_char, start_line, current_line_num - 1});
            }
            current_char = first_char;
            start_line = current_line_num;
        }
        ++current_line_num;
    }

    if (current_char != '\0') {
        index.push_back({(int)current_char, start_line, current_line_num - 1});
    }
}

// ---- 延迟写盘支持 ----
// 词库变更只改内存 + mark dirty（毫秒级），事件循环空闲时 flush 统一落盘。
static void persist_lines_to_file(const std::string& file_path, const std::vector<std::string>& lines);

static bool g_user_dict_dirty = false;
static bool g_char_freq_dirty = false;
static std::chrono::steady_clock::time_point g_last_dirty_time{};

void mark_dict_dirty(WeightTargetFile target_file) {
    if (target_file == WeightTargetFile::UserDict) g_user_dict_dirty = true;
    else g_char_freq_dirty = true;
    g_last_dirty_time = std::chrono::steady_clock::now();
}

bool has_pending_dict_writes() {
    return g_user_dict_dirty || g_char_freq_dirty;
}

// 去抖落盘：距最后一次变更超过 kDebounceMs 才写，连续选词不会反复全量写盘。
static constexpr long long kDebounceMs = 400;

void flush_dirty_dicts() {
    bool need_user = g_user_dict_dirty;
    bool need_char = g_char_freq_dirty;
    if (!need_user && !need_char) return;
    g_user_dict_dirty = false;
    g_char_freq_dirty = false;
    if (need_user) persist_lines_to_file(get_user_dict_file_path(), g_user_dict_lines);
    if (need_char) persist_lines_to_file(get_char_freq_file_path(), g_char_freq_lines);
}

// 事件循环空闲点调用：脏且超过去抖窗口才落盘。
void flush_dirty_dicts_if_idle() {
    if (!has_pending_dict_writes()) return;
    auto now = std::chrono::steady_clock::now();
    if (now - g_last_dirty_time < std::chrono::milliseconds(kDebounceMs)) return;
    flush_dirty_dicts();
}

void wait_pending_persists() {
    flush_dirty_dicts();
}

static void persist_lines_to_file(const std::string& file_path, const std::vector<std::string>& lines) {
    // 原子写：先写临时文件，再 rename 覆盖，避免崩溃时词库损坏。
    std::string tmp_path = file_path + ".tmp";
    std::ofstream file(tmp_path, std::ios::trunc);
    if (!file.is_open()) {
        std::cerr << "Failed to open file for writing: " << tmp_path << std::endl;
        return;
    }
    for (const auto& line : lines) {
        file << line << "\n";
    }
    file.close();
    std::error_code ec;
    std::filesystem::rename(tmp_path, file_path, ec);
}

// 解析一行 char_freq 文本（"拼音 文本 [ts] [count]"）并写入查找表。
static void update_char_freq_lookup_for_line(const std::string& line, int line_number);

static void insert_line_keep_ascii_sorted(const std::string& file_path,
                                          const std::string& new_line,
                                          std::vector<std::string>& lines,
                                          std::vector<PinyinIndexItem>& index) {
    if (new_line.empty()) return;

    if (lines.empty()) {
        lines.push_back(new_line);
        write_log("Insert line into " + file_path + " at line 1: " + new_line, INFO);
        if (&lines == &g_user_dict_lines) invalidate_parts_cache();
        mark_dict_dirty(&lines == &g_user_dict_lines ? WeightTargetFile::UserDict
                                                     : WeightTargetFile::CharFreq);
        build_index_from_lines(lines, index);
        if (&lines == &g_char_freq_lines) update_char_freq_lookup_for_line(new_line, 1);
        return;
    }

    char first_char = new_line[0];
    auto index_it = std::lower_bound(
        index.begin(), index.end(), (int)first_char,
        [](const PinyinIndexItem& item, int c) { return item.start_char < c; });

    size_t insert_pos = 0;
    if (index_it != index.end() && index_it->start_char == (int)first_char) {
        size_t start_idx = (size_t)std::max(0, index_it->start_line - 1);
        size_t end_exclusive = (size_t)std::max(0, index_it->end_line);
        if (start_idx > lines.size()) start_idx = lines.size();
        if (end_exclusive > lines.size()) end_exclusive = lines.size();

        auto it = std::lower_bound(lines.begin() + (std::ptrdiff_t)start_idx,
                                   lines.begin() + (std::ptrdiff_t)end_exclusive,
                                   new_line);
        insert_pos = (size_t)std::distance(lines.begin(), it);
    } else {
        if (index_it == index.end()) {
            insert_pos = lines.size();
        } else {
            insert_pos = (size_t)std::max(0, index_it->start_line - 1);
            if (insert_pos > lines.size()) insert_pos = lines.size();
        }
    }

    lines.insert(lines.begin() + (std::ptrdiff_t)insert_pos, new_line);
    write_log("Insert line into " + file_path + " at line " + std::to_string(insert_pos + 1) + ": " + new_line,
              INFO);
    if (&lines == &g_user_dict_lines) invalidate_parts_cache();
    mark_dict_dirty(&lines == &g_user_dict_lines ? WeightTargetFile::UserDict
                                                 : WeightTargetFile::CharFreq);
    build_index_from_lines(lines, index);
    if (&lines == &g_char_freq_lines) update_char_freq_lookup_for_line(new_line, insert_pos + 1);
}

// 解析一行 char_freq 文本（"拼音 文本 [ts] [count]"）并写入查找表。
static void update_char_freq_lookup_for_line(const std::string& line, int line_number) {
    std::istringstream iss(line);
    std::string py, text, ts_str, count_str;
    if (!(iss >> py >> text)) return;
    long long timestamp = 0;
    int count = 1;
    if (iss >> ts_str && is_all_digits(ts_str)) {
        timestamp = std::stoll(ts_str);
    }
    if (iss >> count_str && is_all_digits(count_str)) {
        count = std::stoi(count_str);
    }
    g_char_freq_lookup[py + " " + text] = {line_number, timestamp, count};
}

/**
 * 初始化拼音相关数据缓存。
 * 启动时一次性加载：
 * - 单字库及其首字母索引；
 * - 用户词库及其首字母索引。
 */
// 构建单字库 O(1) 查找表：key = "拼音 文本"。
static void build_char_freq_lookup() {
    g_char_freq_lookup.clear();
    g_char_freq_lookup.reserve(g_char_freq_lines.size());
    for (int i = 0; i < (int)g_char_freq_lines.size(); ++i) {
        const std::string& line = g_char_freq_lines[i];
        std::istringstream iss(line);
        std::string py, text, ts_str, count_str;
        if (!(iss >> py >> text)) continue;
        long long timestamp = 0;
        int count = 1;
        if (iss >> ts_str && is_all_digits(ts_str)) {
            timestamp = std::stoll(ts_str);
        }
        if (iss >> count_str && is_all_digits(count_str)) {
            count = std::stoi(count_str);
        }
        g_char_freq_lookup[py + " " + text] = {i + 1, timestamp, count};
    }
}

void init_pinyin_data() {
    load_file_and_build_index(get_pinyin_map_file_path(), g_pinyin_map_lines, g_pinyin_map_index);
    load_file_and_build_index(get_user_dict_file_path(), g_user_dict_lines, g_user_dict_index);
    load_file_and_build_index(get_char_freq_file_path(), g_char_freq_lines, g_char_freq_index);
    build_char_freq_lookup();
    build_parts_cache();
}

// ---- 词库 parts 硬盘缓存（mmap）----
// 二进制格式：
//   [u32 magic=0x50415944][u32 line_count]
//   [u64 user_dict mtime][u64 user_dict size]
//   [offset 表: line_count × u32]      每行数据区起始偏移
//   [数据区: 逐行]  u8 seg_count, 每段: u8 seg_len + seg_len 字节
//
// 头里带词库文件的 mtime+size 指纹：行数相同不代表内容相同（外部修改、
// 恢复备份等），仅校验行数会复用陈旧 cache，导致拼音段与文本按行错位
// （表现为输入 dyx 匹配出超长词）。指纹一致才允许复用。
static const uint32_t kPartsMagic = 0x50415944;  // "PYAD"

static int g_parts_fd = -1;
static const uint8_t* g_parts_base = nullptr;
static size_t g_parts_map_size = 0;
static uint32_t g_parts_line_count = 0;

static std::string get_parts_cache_path() {
    return join_path(join_path(get_pinyin_data_dir(), "cache"), "user_dict_parts.bin");
}

// 词库文件指纹：修改时间 + 文件大小（64 位）。失败返回 false。
static bool user_dict_file_fingerprint(uint64_t& mtime, uint64_t& size) {
    std::error_code ec;
    auto ft = std::filesystem::last_write_time(get_user_dict_file_path(), ec);
    if (ec) return false;
    uint64_t sz = (uint64_t)std::filesystem::file_size(get_user_dict_file_path(), ec);
    if (ec) return false;
    mtime = (uint64_t)ft.time_since_epoch().count();
    size = sz;
    return true;
}

// 逐段遍历拼音 CSV（如 "a,bc,d" → 依次回调 "a","bc","d"）。
template <typename F>
static void for_each_segment(const std::string& pinyin_csv, F&& fn) {
    size_t pos = 0;
    while (pos <= pinyin_csv.size()) {
        size_t next = pinyin_csv.find(',', pos);
        size_t end = (next == std::string::npos) ? pinyin_csv.size() : next;
        size_t len = end - pos;
        if (len > 0) fn(pinyin_csv.data() + pos, len);
        if (next == std::string::npos) break;
        pos = next + 1;
    }
}

// 生成 parts cache 文件（词库变化时重建），临时文件 + rename 原子替换。
static void generate_parts_cache_file(const std::vector<std::string>& lines) {
    std::string path = get_parts_cache_path();
    std::string cache_dir = join_path(get_pinyin_data_dir(), "cache");
    std::error_code ec;
    std::filesystem::create_directories(cache_dir, ec);

    // 每行数据字节数 = 段数(1) + Σ(长度(1) + 内容)。
    auto row_bytes = [](const std::string& line) {
        std::string py = get_pinyin_from_line(line);
        uint64_t size = 1;  // 段数
        for_each_segment(py, [&size](const char*, size_t n) { size += 1 + n; });
        return size;
    };
    uint64_t exact_size = 0;
    for (const auto& line : lines) exact_size += row_bytes(line);

    uint64_t header_size = 8 + 8 + 8 + (uint64_t)lines.size() * 4;

    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) return;

        uint32_t magic = kPartsMagic;
        uint32_t count = (uint32_t)lines.size();
        uint64_t src_mtime = 0, src_size = 0;
        if (!user_dict_file_fingerprint(src_mtime, src_size)) {
            src_mtime = 0;
            src_size = 0;
        }
        f.write((const char*)&magic, 4);
        f.write((const char*)&count, 4);
        f.write((const char*)&src_mtime, 8);
        f.write((const char*)&src_size, 8);

        // 先写完整 offset 表（占位），再写数据区。
        uint32_t cur_offset = (uint32_t)header_size;
        for (uint32_t i = 0; i < count; ++i) {
            f.write((const char*)&cur_offset, 4);
            cur_offset += (uint32_t)row_bytes(lines[i]);
        }

        // 写数据区。
        for (uint32_t i = 0; i < count; ++i) {
            const std::string& line = lines[i];
            std::string py = get_pinyin_from_line(line);
            uint8_t seg_count = 0;
            for_each_segment(py, [&seg_count](const char*, size_t) { ++seg_count; });
            f.write((const char*)&seg_count, 1);
            for_each_segment(py, [&f](const char* s, size_t n) {
                uint8_t slen = (uint8_t)n;
                f.write((const char*)&slen, 1);
                f.write(s, (std::streamsize)n);
            });
        }
        f.close();
    }
    std::filesystem::rename(tmp, path, ec);
}

// unmap 并关闭句柄（不删文件）。
static void unmap_parts_cache() {
    if (g_parts_base) {
        munmap((void*)g_parts_base, g_parts_map_size);
        g_parts_base = nullptr;
        g_parts_map_size = 0;
    }
    if (g_parts_fd >= 0) {
        close(g_parts_fd);
        g_parts_fd = -1;
    }
    g_parts_line_count = 0;
}

// mmap 映射 parts cache 文件；返回是否成功。
static bool map_parts_cache(uint32_t expect_count) {
    std::string path = get_parts_cache_path();
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return false;
    }

    void* base = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (base == MAP_FAILED) {
        close(fd);
        return false;
    }

    // 校验头。
    if (st.st_size >= 24) {
        uint32_t magic = *(const uint32_t*)base;
        uint32_t count = *(const uint32_t*)((const uint8_t*)base + 4);
        if (magic == kPartsMagic && count == expect_count) {
            g_parts_fd = fd;
            g_parts_base = (const uint8_t*)base;
            g_parts_map_size = (size_t)st.st_size;
            g_parts_line_count = count;
            return true;
        }
    }
    munmap(base, (size_t)st.st_size);
    close(fd);
    return false;
}

// 构建/校验 parts 缓存。
// 校验与文件生成在 map 之前执行，mmap 替换只需毫秒级。
void build_parts_cache() {
    const std::vector<std::string>& lines = g_user_dict_lines;
    std::string path = get_parts_cache_path();

    // 检查现有 cache 是否有效（文件存在 + 行数匹配 + 词库指纹一致）。
    bool need_rebuild = true;
    {
        std::ifstream f(path, std::ios::binary);
        if (f.is_open()) {
            uint32_t magic = 0, count = 0;
            uint64_t cached_mtime = 0, cached_size = 0, cur_mtime = 0, cur_size = 0;
            f.read((char*)&magic, 4);
            f.read((char*)&count, 4);
            f.read((char*)&cached_mtime, 8);
            f.read((char*)&cached_size, 8);
            bool fp_ok = user_dict_file_fingerprint(cur_mtime, cur_size);
            if (magic == kPartsMagic && count == (uint32_t)lines.size() &&
                (!fp_ok ||
                 (cached_mtime == cur_mtime && cached_size == cur_size))) {
                need_rebuild = false;
            }
        }
    }
    if (need_rebuild) {
        generate_parts_cache_file(lines);
    }

    unmap_parts_cache();
    map_parts_cache((uint32_t)lines.size());
}

void invalidate_parts_cache() {
    unmap_parts_cache();
    // cache 失效后，匹配回退按需 split；下次 init_pinyin_data 重建。
}

const char* get_parts_cached_line(int line_index) {
    if (!g_parts_base || line_index < 0 || line_index >= (int)g_parts_line_count) return nullptr;
    const uint32_t* offsets = (const uint32_t*)(g_parts_base + 24);
    return (const char*)(g_parts_base + offsets[line_index]);
}

uint8_t get_parts_cached_seg_count(const char* p) {
    if (!p) return 0;
    return (uint8_t)*p;
}

const char* get_parts_cached_seg(const char* p, int seg_index, int& seg_len) {
    if (!p) { seg_len = 0; return nullptr; }
    uint8_t count = (uint8_t)*p;
    if (seg_index < 0 || seg_index >= (int)count) { seg_len = 0; return nullptr; }
    const uint8_t* cur = (const uint8_t*)p + 1;  // 跳过段数
    for (int i = 0; i < seg_index; ++i) {
        uint8_t len = *cur++;
        cur += len;
    }
    uint8_t len = *cur++;
    seg_len = (int)len;
    return (const char*)cur;
}

void load_file_and_build_index(const std::string& file_path, 
                               std::vector<std::string>& lines, 
                               std::vector<PinyinIndexItem>& index) {
    /**
     * 通用“加载文件 + 构建首字母索引”流程：
     * 1) 读取非空行到 lines；
     * 2) 按行首字符分段记录 start_line/end_line 到 index。
     */
    lines.clear();
    index.clear();

    std::ifstream file(file_path);
    if (!file.is_open()) {
        std::cerr << "Failed to open file: " << file_path << std::endl;
        return;
    }

    std::string line;
    int current_line_num = 1;
    char current_char = '\0';
    int start_line = -1;

    while (std::getline(file, line)) {
        if (line.empty()) continue;
        // 兼容 CRLF 行尾（如从 Windows 侧生成/复制的词库文件）。
        if (line.back() == '\r') line.pop_back();
        lines.push_back(line);

        char first_char = line[0];
        if (first_char != current_char) {
            // 如果不是第一个发现的字符，关闭上一个字符的索引范围
            if (current_char != '\0') {
                index.push_back({(int)current_char, start_line, current_line_num - 1});
            }
            current_char = first_char;
            start_line = current_line_num;
        }
        current_line_num++;
    }

    // 处理最后一个字符
    if (current_char != '\0') {
        index.push_back({(int)current_char, start_line, current_line_num - 1});
    }

    file.close();
}

void write_and_update_index(const std::string& file_path, 
                            const std::string& content,
                            std::vector<std::string>& lines, 
                            std::vector<PinyinIndexItem>& index) {
    insert_line_keep_ascii_sorted(file_path, content, lines, index);
}

void write_and_update_index(const std::string& file_path,
                            const CandidateItem& candidate,
                            std::vector<std::string>& lines,
                            std::vector<PinyinIndexItem>& index) {
    insert_line_keep_ascii_sorted(file_path, candidate.toString(), lines, index);
}

bool delete_user_dict_line(int line_number) {
    if (line_number <= 0) return false;
    int idx = line_number - 1;
    if (idx < 0 || idx >= (int)g_user_dict_lines.size()) return false;

    g_user_dict_lines.erase(g_user_dict_lines.begin() + idx);
    invalidate_parts_cache();
    mark_dict_dirty(WeightTargetFile::UserDict);
    build_index_from_lines(g_user_dict_lines, g_user_dict_index);
    return true;
}

int get_weight_by_line(WeightTargetFile target_file, int line_number) {
    if (line_number <= 0) return 0;
    const std::vector<std::string>* lines = (target_file == WeightTargetFile::CharFreq) ? &g_char_freq_lines
                                                                                        : &g_user_dict_lines;
    int idx = line_number - 1;
    if (idx < 0 || idx >= (int)lines->size()) return 0;

    const std::string& raw_line = (*lines)[idx];
    std::istringstream iss(raw_line);
    std::vector<std::string> parts;
    std::string token;
    while (iss >> token) {
        parts.push_back(token);
    }
    if (parts.size() < 2) return 0;

    if (parts.size() >= 3 && is_all_digits(parts.back())) {
        return std::stoi(parts.back());
    }
    return 1;
}

void update_timestamp_by_line(WeightTargetFile target_file, int line_number) {
    if (line_number <= 0) return;
    std::vector<std::string>* lines = (target_file == WeightTargetFile::CharFreq) ? &g_char_freq_lines
                                                                                  : &g_user_dict_lines;

    int idx = line_number - 1;
    if (idx < 0 || idx >= (int)lines->size()) return;

    long long now = (long long)time(nullptr);

    const std::string& raw_line = (*lines)[idx];
    std::istringstream iss(raw_line);
    std::vector<std::string> parts;
    std::string token;
    while (iss >> token) {
        parts.push_back(token);
    }
    if (parts.size() < 2) return;

    // 兼容 2/3/4 列：最后一个数字列视为 count。
    int count = 1;
    if (parts.size() >= 3 && is_all_digits(parts.back())) {
        count = std::stoi(parts.back()) + 1;
    } else {
        count = 2;
    }

    std::string new_line = parts[0] + " " + parts[1] + " " + std::to_string(now) + " " + std::to_string(count);

    (*lines)[idx] = new_line;

    // 首字母未变，索引无需重建；同步单字库查找表（词库无查找表，无需更新）。
    mark_dict_dirty(target_file);
    if (target_file == WeightTargetFile::CharFreq) {
        update_char_freq_lookup_for_line(new_line, idx + 1);
    }
}
