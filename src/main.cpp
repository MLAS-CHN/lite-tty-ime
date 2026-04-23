/**
 * lite-tty-ime - 纯tty终端拼音输入法
 * 
 * 主程序文件结构：
 * 1. 头文件与常量定义 (1-46行)
 * 2. 日志类 (89-106行)
 * 3. 配置解析 (108-271行)
 * 4. 终端原始模式守卫 (273-305行)
 * 5. UTF-8工具函数 (307-330行)
 * 6. 字符串分割工具 (332-350行)
 * 7. 候选词数据结构与获取 (352-746行)
 * 8. 拼音算法兼容层 (748-769行) - 已抽离到pinyin模块
 * 9. UTF-8前缀工具 (771-785行)
 * 10. 逗号分隔键索引 (794-826行)
 * 11. 词典加载/保存 (828-1043行)
 * 12. UTF-8字符操作 (1073-1104行)
 * 13. 终端显示工具 (1107-1319行)
 * 14. 状态栏绘制 (1321-1441行)
 * 15. 帮助信息 (1443-1512行)
 * 16. Shell PTY设置 (1514-1551行)
 * 17. 主函数 (1553-2900行)
 */

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <future>
#include <unordered_set>
#include <ctime>
#include <cmath>
#include <thread>
#include <functional>
#include <sstream>
#include <string>
#include <termios.h>
#include <unordered_map>
#include <unistd.h>
#include <vector>

#include "esc_parser.h"
#include "input_handlers.h"
#include "input_state.h"
#include "learn_rules.h"
#include "pinyin.h"
#include "query_forms.h"

#if !defined(_WIN32)
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif
#endif

/** ============================================================
 * 区块1: 常量定义
 * 描述: 系统路径、版本号、全局显示模式标志
 * 抽离建议: 保留在此文件，属于全局配置常量
 * ============================================================ */
/** 与 .deb 安装布局一致：系统只读资源（基础词库与用户词库模板） */
static constexpr const char *kBundledDataDir = "/usr/share/lite-tty-ime/data";
static constexpr const char *kBaseDictPath = "/usr/share/lite-tty-ime/data/pinyin_map.txt";
static constexpr const char *kUserDictPath = "/usr/share/lite-tty-ime/data/user_dict.txt";
static constexpr const char *kVersion = "0.3.0";
static bool gSimpleImeDisplayMode = false;
static std::string gSimpleImeStatusText;

/** ============================================================
 * 区块2: 候选词统计结构
 * 描述: 用于词频排序的统计数据
 * 抽离建议: 可抽离到 utils.h 作为公共数据结构
 * ============================================================ */
struct CandidateStat {
    unsigned long long count = 0;
    time_t lastTs = 0;
};

/** ============================================================
 * 区块4: 状态栏位置枚举
 * 描述: 状态栏显示位置
 * 抽离建议: 保留在此
 * ============================================================ */

/** ============================================================
 * 区块5: 应用配置结构
 * 描述: 所有可配置项的集合
 * 抽离建议: 可抽离到 config.h，但依赖当前文件的解析函数
 * ============================================================ */
struct AppConfig {
    // 词库路径：基础词库 + 用户词库。用户词库会在运行期被写入。
    std::string dictPath = kBaseDictPath;
    std::string userDictPath = kUserDictPath;
    int toggleKey = 0x00; // Ctrl+Space
    int exitKey = -1;     // 默认不拦截退出键（Ctrl+C 透传给子程序）
    size_t candidatePageSize = 5; // 1..9
    KeyAction spaceAction = KeyAction::SelectFirst;
    KeyAction enterAction = KeyAction::CommitRaw;
    bool continuousBacktrackLongFirst = true;
    bool debugMode = false;
    bool showConStats = false;  // 连续输入"字,词"统计显示（默认隐藏）
    bool showStoreHint = false; // 连续输入落库提示显示（默认隐藏）
    std::string logFilePath;
};

/** ============================================================
 * 区块6: 日志类
 * 描述: 简单的日志记录工具
 * 抽离建议: 可抽离到 utils.h/cpp
 * ============================================================ */
class Logger {
public:
    explicit Logger(const std::string &path) {
        if (!path.empty()) {
            out_.open(path, std::ios::app);
        }
    }
    template <typename T>
    void info(const T &msg) {
        if (out_) out_ << "[INFO] " << msg << "\n";
    }
    template <typename T>
    void warn(const T &msg) {
        if (out_) out_ << "[WARN] " << msg << "\n";
    }
private:
    std::ofstream out_;
};

/** ============================================================
 * 区块7: 配置解析辅助函数
 * 描述: 类型转换、键名解析等
 * 抽离建议: 可抽离到 utils.h/cpp
 * ============================================================ */
static std::string trim(const std::string &s);

static std::string toLowerCopy(std::string s) {
    for (char &c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

static bool parseBool(const std::string &s, bool *out) {
    const std::string v = toLowerCopy(trim(s));
    if (v == "1" || v == "true" || v == "yes" || v == "on") {
        *out = true;
        return true;
    }
    if (v == "0" || v == "false" || v == "no" || v == "off") {
        *out = false;
        return true;
    }
    return false;
}

static bool parseInt(const std::string &s, int *out) {
    char *end = nullptr;
    long v = std::strtol(s.c_str(), &end, 10);
    if (end == s.c_str() || *end != '\0') {
        return false;
    }
    *out = static_cast<int>(v);
    return true;
}

static bool parseKeySpec(const std::string &in, int *out) {
    // 支持"可读键名 + 单字符"两套写法，便于 CLI/配置复用同一解析器。
    std::string v = toLowerCopy(trim(in));
    if (v == "ctrl+space" || v == "c-space") { *out = 0x00; return true; }
    if (v == "ctrl+c" || v == "c-c") { *out = 0x03; return true; }
    if (v == "ctrl+d" || v == "c-d") { *out = 0x04; return true; }
    if (v == "ctrl+]" || v == "c-]") { *out = 0x1d; return true; }
    if (v == "esc") { *out = 0x1b; return true; }
    if (v == "enter") { *out = '\n'; return true; }
    if (v == "space") { *out = ' '; return true; }
    if (v == "tab") { *out = '\t'; return true; }
    if (v.size() == 1) {
        *out = static_cast<unsigned char>(v[0]);
        return true;
    }
    return false;
}

static std::string keySpecToString(int key) {
    switch (key) {
    case 0x00: return "ctrl+space";
    case 0x03: return "ctrl+c";
    case 0x04: return "ctrl+d";
    case 0x1d: return "ctrl+]";
    case 0x1b: return "esc";
    case '\n': return "enter";
    case ' ': return "space";
    case '\t': return "tab";
    case -1: return "none";
    default:
        if (key >= 32 && key <= 126) {
            return std::string(1, static_cast<char>(key));
        }
        return std::to_string(key);
    }
}

static bool parseKeyAction(const std::string &in, KeyAction *out) {
    std::string v = toLowerCopy(trim(in));
    if (v == "select" || v == "select_first" || v == "candidate") {
        *out = KeyAction::SelectFirst;
        return true;
    }
    if (v == "commit_raw" || v == "raw" || v == "ascii") {
        *out = KeyAction::CommitRaw;
        return true;
    }
    if (v == "send_line" || v == "newline" || v == "enter_line") {
        *out = KeyAction::SendLineEnd;
        return true;
    }
    if (v == "none" || v == "off") {
        *out = KeyAction::None;
        return true;
    }
    return false;
}

/** ============================================================
 * 区块8: 用户词库路径检测
 * 描述: 当系统目录不可写时，检测可写用户目录
 * 抽离建议: 可抽离到 utils.h/cpp
 * ============================================================ */
/** 当 /usr/share/... 不可写时，用户词库与词频、学习短语的落盘路径 */
static std::string defaultWritableUserDictPath() {
    namespace fs = std::filesystem;
    const char *xdg = std::getenv("XDG_DATA_HOME");
    if (xdg && xdg[0] != '\0') {
        return (fs::path(xdg) / "lite-tty-ime" / "user_dict.txt").string();
    }
    const char *home = std::getenv("HOME");
    if (home && home[0] != '\0') {
        return (fs::path(home) / ".local" / "share" / "lite-tty-ime" / "user_dict.txt").string();
    }
    return std::string(kUserDictPath);
}

/** ============================================================
 * 区块9: 状态栏位置解析
 * 描述: 解析配置中的状态栏位置
 * 抽离建议: 可合并到区块7
 * ============================================================ */

/** ============================================================
 * 区块10: 配置文件加载
 * 描述: 解析 INI/TOML 风格的配置文件
 * 抽离建议: 可抽离到 config.cpp
 * ============================================================ */
static void loadConfigFile(const std::string &path, AppConfig *cfg) {
    // 兼容 INI/TOML 风格的 key=value；未知键静默忽略，降低升级破坏性。
    std::ifstream in(path);
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line.front() == '[' && line.back() == ']') continue; // ini section
        size_t p = line.find('=');
        if (p == std::string::npos) continue;
        std::string key = toLowerCopy(trim(line.substr(0, p)));
        std::string val = trim(line.substr(p + 1));
        if (!val.empty() && ((val.front() == '"' && val.back() == '"') ||
                             (val.front() == '\'' && val.back() == '\''))) {
            val = val.substr(1, val.size() - 2);
        }
        if (key == "dict" || key == "dict_path") cfg->dictPath = val;
        else if (key == "user_dict" || key == "user_dict_path") cfg->userDictPath = val;
        else if (key == "toggle_key") { int k = 0; if (parseKeySpec(val, &k)) cfg->toggleKey = k; }
        else if (key == "exit_key") {
            const std::string v = toLowerCopy(trim(val));
            if (v == "none" || v == "off" || v == "disable" || v == "disabled") {
                cfg->exitKey = -1;
            } else {
                int k = 0;
                if (parseKeySpec(val, &k)) cfg->exitKey = k;
            }
        }
        else if (key == "candidate_page_size" || key == "page_size") {
            int n = 9;
            if (parseInt(val, &n)) {
                if (n < 1) n = 1;
                if (n > 9) n = 9;
                cfg->candidatePageSize = static_cast<size_t>(n);
            }
        } else if (key == "space_action") {
            KeyAction a {};
            if (parseKeyAction(val, &a)) cfg->spaceAction = a;
        } else if (key == "enter_action") {
            KeyAction a {};
            if (parseKeyAction(val, &a)) cfg->enterAction = a;
        } else if (key == "continuous_backtrack") {
            const std::string v = toLowerCopy(trim(val));
            if (v == "long_first" || v == "long") {
                cfg->continuousBacktrackLongFirst = true;
            } else if (v == "short_first" || v == "short") {
                cfg->continuousBacktrackLongFirst = false;
            }
        } else if (key == "debug") {
            bool b = false;
            if (parseBool(val, &b)) cfg->debugMode = b;
        } else if (key == "show_con_stats" || key == "show_continuous_stats") {
            bool b = false;
            if (parseBool(val, &b)) cfg->showConStats = b;
        } else if (key == "show_store_hint" || key == "show_store_message") {
            bool b = false;
            if (parseBool(val, &b)) cfg->showStoreHint = b;
        } else if (key == "log_file") {
            cfg->logFilePath = val;
        }
    }
}

/** ============================================================
 * 区块11: 终端原始模式守卫
 * 描述: RAII 对象，进入/退出原始模式
 * 抽离建议: 可抽离到 shell.h/cpp
 * ============================================================ */
class RawModeGuard {
public:
    // 进入原始模式：关闭 ICANON/ECHO，按字节读取按键事件（含 ESC 序列）。
    RawModeGuard() : ok_(false) {
        if (tcgetattr(STDIN_FILENO, &old_) != 0) {
            return;
        }
        termios raw = old_;
        raw.c_lflag &= static_cast<unsigned>(~(ICANON | ECHO | ISIG));
        // 关闭软件流控，避免 Ctrl+S/Ctrl+Q 被终端拦截成 XOFF/XON。
        // 同时关闭 CR/LF 输入转换，保留原始按键字节：
        // - Enter 保持 '\r'
        // - Ctrl+J 保持 '\n'
        raw.c_iflag &= static_cast<unsigned>(~(IXON | IXOFF | IXANY | ICRNL | INLCR | IGNCR));
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) {
            ok_ = true;
        }
    }

    ~RawModeGuard() {
        if (ok_) {
            tcsetattr(STDIN_FILENO, TCSANOW, &old_);
        }
    }

    bool ok() const { return ok_; }

private:
    termios old_ {};
    bool ok_;
};

/** ============================================================
 * 区块12: UTF-8工具函数
 * 描述: UTF-8字符拆分、字符串修剪
 * 抽离建议: 可抽离到 utils.h/cpp
 * ============================================================ */
static std::vector<std::string> splitUtf8Chars(const std::string &s) {
    // 将 UTF-8 字节流拆成"字符片段"，后续用于安全截断/回退/宽度估算。
    std::vector<std::string> out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = 1;
        if ((c & 0x80u) == 0x00u) len = 1;
        else if ((c & 0xE0u) == 0xC0u) len = 2;
        else if ((c & 0xF0u) == 0xE0u) len = 3;
        else if ((c & 0xF8u) == 0xF0u) len = 4;
        if (i + len > s.size()) len = 1;
        out.push_back(s.substr(i, len));
        i += len;
    }
    return out;
}

static std::string trim(const std::string &s) {
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) b++;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
    return s.substr(b, e - b);
}

/** ============================================================
 * 区块13: 逗号分隔键分割
 * 描述: 将多音节键（如 "jin,tian"）拆分成音节数组
 * 抽离建议: 可合并到区块12
 * ============================================================ */
/** 逗号分隔键：按音节拆分（user_dict 多音节行）。空串返回空向量。各段 trim，避免行尾或空格破坏匹配。 */
static std::vector<std::string> splitComma(const std::string &s) {
    std::vector<std::string> out;
    if (s.empty()) {
        return out;
    }
    std::string cur;
    for (unsigned char uc : s) {
        char c = static_cast<char>(uc);
        if (c == ',') {
            out.push_back(trim(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(trim(cur));
    return out;
}

/** ============================================================
 * 区块14: 候选词数据结构
 * 描述: 候选词的存储结构
 * 抽离建议: 可抽离到 dict.h
 * ============================================================ */
// 新框架：候选词获取函数（占位）
// - 输入：已拆分完成的拼音数组（元素可为完整拼音或手动拆分串，如 "nan'gei"）
// - 约定：若未来需要多次执行，输出应按传入数组顺序稳定拼接
// - 当前：按 [merge,rad,cons] 顺序输入，返回正式候选词
struct DictMatchCand {
    std::string phrase;
    std::string key;
    int exactCount = 0;
    int prefixCount = 0;
    double weight = 0.0;
    size_t order = 0;
};

/** ============================================================
 * 区块15: 字符串分割与替换工具
 * 描述: 通用分隔符分割、字符替换
 * 抽离建议: 可合并到区块12
 * ============================================================ */
static std::vector<std::string> splitBySep(const std::string &s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : s) {
        if (ch == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(ch);
        }
    }
    out.push_back(cur);
    return out;
}

[[maybe_unused]] static std::string joinWithComma(const std::vector<std::string> &segs) {
    std::string out;
    for (size_t i = 0; i < segs.size(); ++i) {
        if (i) out += ",";
        out += segs[i];
    }
    return out;
}

/** ============================================================
 * 区块16: 字数与词数统计
 * 描述: 统计选中内容中的单字和词语数量
 * 抽离建议: 可抽离到 utils.h/cpp
 * ============================================================ */
static std::pair<size_t, size_t> countSelectedCharsAndWords(const std::vector<std::string> &selectedWords) {
    size_t charCount = 0; // 字：1 字
    size_t wordCount = 0; // 词：>=2 字
    for (const auto &w : selectedWords) {
        const size_t n = splitUtf8Chars(w).size();
        if (n >= 2) ++wordCount;
        else if (n == 1) ++charCount;
    }
    return {charCount, wordCount};
}

/** ============================================================
 * 区块17: 前缀匹配工具
 * 描述: 检查字符串是否为另一个字符串的前缀
 * 抽离建议: 可合并到区块12
 * ============================================================ */
static bool segPrefixMatch(const std::string &in, const std::string &full) {
    if (in.size() > full.size()) return false;
    return full.compare(0, in.size(), in) == 0;
}

/** ============================================================
 * 区块18: 候选词获取主逻辑（核心算法）
 * 描述: 根据拼音分段获取候选词，支持多段匹配、词频排序
 * 抽离建议: 可抽离到 dict.cpp，但逻辑复杂度高
 * ============================================================ */
static std::vector<std::string> get_candidate_words(
    const std::vector<std::string> &segmentedPinyinInputs,
    const std::unordered_map<std::string, std::vector<std::string>> &dict,
    const std::unordered_map<std::string, CandidateStat> &freqStats) {
    // 首字母索引：仅按 key 第一个字母分桶，降低遍历范围
    std::unordered_map<char, std::vector<std::pair<std::string, size_t>>> keyBuckets;
    {
        size_t ord = 0;
        for (const auto &kv : dict) {
            if (kv.first.empty()) {
                ++ord;
                continue;
            }
            const unsigned char c0 = static_cast<unsigned char>(kv.first[0]);
            if (c0 < 128u && std::isalpha(c0)) {
                keyBuckets[static_cast<char>(std::tolower(c0))].push_back({kv.first, ord});
            }
            ++ord;
        }
    }

    auto scoreChunkCandidates = [&](const std::vector<std::string> &chunkSegs, bool singleChunkMode) {
        std::vector<DictMatchCand> hits;
        if (chunkSegs.empty() || chunkSegs[0].empty()) return hits;
        const unsigned char c0 = static_cast<unsigned char>(chunkSegs[0][0]);
        if (!(c0 < 128u && std::isalpha(c0))) return hits;
        const char bucketKey = static_cast<char>(std::tolower(c0));
        const auto bit = keyBuckets.find(bucketKey);
        if (bit == keyBuckets.end()) return hits;

        for (const auto &ko : bit->second) {
            const std::string &k = ko.first;
            const size_t order = ko.second;
            const auto keySegs = splitComma(k);
            if (keySegs.size() != chunkSegs.size()) continue;

            int exactCnt = 0;
            int prefixCnt = 0;
            bool ok = true;
            for (size_t i = 0; i < chunkSegs.size(); ++i) {
                if (chunkSegs[i] == keySegs[i]) {
                    ++exactCnt;
                    continue;
                }
                if (segPrefixMatch(chunkSegs[i], keySegs[i])) {
                    ++prefixCnt;
                    continue;
                }
                ok = false;
                break;
            }
            if (!ok) continue;

            const auto dit = dict.find(k);
            if (dit == dict.end()) continue;
            for (const auto &ph : dit->second) {
                DictMatchCand dc;
                dc.phrase = ph;
                dc.key = k;
                dc.exactCount = exactCnt;
                dc.prefixCount = prefixCnt;
                dc.order = order;
                const auto fs = freqStats.find(k + "\t" + ph);
                dc.weight = (fs == freqStats.end()) ? 0.0 : static_cast<double>(fs->second.count);
                hits.push_back(std::move(dc));
            }
        }

        std::stable_sort(hits.begin(), hits.end(), [&](const DictMatchCand &a, const DictMatchCand &b) {
            if (a.exactCount != b.exactCount) return a.exactCount > b.exactCount;
            if (a.prefixCount != b.prefixCount) return a.prefixCount > b.prefixCount;
            if (a.weight != b.weight) return a.weight > b.weight;
            return a.order < b.order;
        });

        // 单 chunk 情况返回完整排序列表；多 chunk 情况外部只取第一项
        (void)singleChunkMode;
        return hits;
    };

    auto makeWordChunks = [&](const std::vector<std::string> &segs) {
        std::vector<std::vector<std::string>> chunks;
        if (segs.empty()) return chunks;
        size_t pos = 0;
        while (pos < segs.size()) {
            bool found = false;
            size_t bestLen = 0;
            // 从多到少尝试，至少 2 段；找不到则单段兜底
            for (size_t len = segs.size() - pos; len >= 2; --len) {
                std::vector<std::string> part(segs.begin() + static_cast<std::ptrdiff_t>(pos),
                                              segs.begin() + static_cast<std::ptrdiff_t>(pos + len));
                auto hits = scoreChunkCandidates(part, false);
                if (!hits.empty()) {
                    bestLen = len;
                    found = true;
                    break;
                }
                if (len == 2) break;
            }
            if (!found) bestLen = 1;
            chunks.emplace_back(segs.begin() + static_cast<std::ptrdiff_t>(pos),
                                segs.begin() + static_cast<std::ptrdiff_t>(pos + bestLen));
            pos += bestLen;
        }
        return chunks;
    };

    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    auto pushOut = [&](const std::string &s) {
        if (s.empty()) return;
        if (seen.insert(s).second) out.push_back(s);
    };

    for (const auto &inRaw : segmentedPinyinInputs) {
        if (inRaw.empty()) continue;
        std::vector<std::string> segs;
        {
            std::string cur;
            for (char ch : inRaw) {
                if (ch == '\'' || ch == ',') {
                    segs.push_back(cur);
                    cur.clear();
                } else {
                    cur.push_back(ch);
                }
            }
            segs.push_back(cur);
        }
        std::vector<std::string> cleanSegs;
        for (const auto &s : segs) if (!s.empty()) cleanSegs.push_back(s);
        if (cleanSegs.empty()) continue;

        const auto chunks = makeWordChunks(cleanSegs);
        if (chunks.empty()) continue;

        if (chunks.size() == 1) {
            // 仅一个 chunk：输出该 chunk 的完整候选列表（不舍弃）
            auto hits = scoreChunkCandidates(chunks[0], true);
            for (const auto &h : hits) pushOut(h.phrase);
            continue;
        }

        // 多 chunk：每段只取第一候选并拼句
        std::string phrase;
        bool okAll = true;
        for (const auto &ch : chunks) {
            auto hits = scoreChunkCandidates(ch, false);
            if (hits.empty()) {
                okAll = false;
                break;
            }
            phrase += hits[0].phrase;
        }
        if (okAll) pushOut(phrase);
    }
    return out;
}

/** ============================================================
 * 区块19: 连续输入首段提取
 * 描述: 从连续拼音中提取首段候选词
 * 抽离建议: 可抽离到 dict.cpp
 * ============================================================ */
// 连续输入首段提取：
// 输入 merge_str（例如 jin'tian'tian'qi'ye'bu'cuo），将其视为 chunk_char 序列。
// 从"全长 -> 逐步削减"检查前缀是否可在词库命中（前缀匹配 + 字数相等），
// 返回每个可行 chunk_phrase 的第一段（first_words），供候选栏暂时调试展示。
static std::vector<std::string> get_first_con_input(
    const std::string &merge_str,
    const std::unordered_map<std::string, std::vector<std::string>> &dict) {
    std::vector<std::string> chunksRaw = splitBySep(merge_str, '\'');
    std::vector<std::string> chunks;
    for (auto &s : chunksRaw) {
        std::string t = trim(toLowerCopy(s));
        if (!t.empty()) chunks.push_back(t);
    }
    if (chunks.empty()) return {};

    // 首字母索引：仅索引 key 的首字母到 key 列表
    std::unordered_map<char, std::vector<std::string>> bucket;
    for (const auto &kv : dict) {
        const std::string &k = kv.first;
        if (k.empty()) continue;
        const unsigned char c0 = static_cast<unsigned char>(k[0]);
        if (c0 < 128u && std::isalpha(c0)) {
            bucket[static_cast<char>(std::tolower(c0))].push_back(k);
        }
    }

    auto joinRange = [&](size_t l, size_t r) {
        std::string out;
        for (size_t i = l; i < r; ++i) out += chunks[i];
        return out;
    };
    auto prefixMatch = [&](const std::string &in, const std::string &full) {
        if (in.size() > full.size()) return false;
        return full.compare(0, in.size(), in) == 0;
    };
    auto hasPrefixChunkMatch = [&](size_t take) -> bool {
        if (take == 0 || take > chunks.size() || chunks[0].empty()) return false;
        const unsigned char c0 = static_cast<unsigned char>(chunks[0][0]);
        if (!(c0 < 128u && std::isalpha(c0))) return false;
        const char bk = static_cast<char>(std::tolower(c0));
        const auto bit = bucket.find(bk);
        if (bit == bucket.end()) return false;

        for (const auto &k : bit->second) {
            const auto segs = splitComma(k);
            if (segs.size() != take) continue;
            bool ok = true;
            for (size_t i = 0; i < take; ++i) {
                if (!prefixMatch(chunks[i], segs[i])) {
                    ok = false;
                    break;
                }
            }
            if (!ok) continue;
            const auto dit = dict.find(k);
            if (dit == dict.end()) continue;
            for (const auto &ph : dit->second) {
                if (splitUtf8Chars(ph).size() == take) return true;
            }
        }
        return false;
    };

    std::vector<std::string> phraseForms;
    std::vector<std::string> firstWords;
    auto pushUnique = [&](std::vector<std::string> &arr, const std::string &s) {
        if (s.empty()) return;
        if (std::find(arr.begin(), arr.end(), s) == arr.end()) arr.push_back(s);
    };

    const size_t n = chunks.size();
    // 全长匹配
    if (hasPrefixChunkMatch(n)) {
        const std::string full = joinRange(0, n);
        pushUnique(phraseForms, full);
        pushUnique(firstWords, full);
    }
    // 从 n-1 到 2 的削减匹配
    for (size_t len = n; len >= 2; --len) {
        const size_t take = len - 1;
        if (!hasPrefixChunkMatch(take)) {
            if (len == 2) break;
            continue;
        }
        const std::string lhs = joinRange(0, take);
        const std::string rhs = joinRange(take, n);
        pushUnique(phraseForms, rhs.empty() ? lhs : (lhs + "," + rhs));
        pushUnique(firstWords, lhs);
        if (len == 2) break;
    }
    // 最后加入 1 字 chunk_phrase
    const std::string one = chunks[0];
    const std::string rest = (n > 1) ? joinRange(1, n) : "";
    pushUnique(phraseForms, rest.empty() ? one : (one + "," + rest));
    pushUnique(firstWords, one);

    (void)phraseForms; // 后续可用于 debug 展示 chunk_phrase 列表
    return firstWords;
}

/** ============================================================
 * 区块20: 分块候选词获取
 * 描述: 为单个拼音分块获取候选词
 * 抽离建议: 可合并到区块18
 * ============================================================ */
// 给定"首段 chunk（若干 chunk_char）"，返回该段可选词（按匹配优先级 + 权重）
static std::vector<std::pair<std::string, std::string>> get_candidates_for_chunk(
    const std::vector<std::string> &chunkSegs,
    const std::unordered_map<std::string, std::vector<std::string>> &dict,
    const std::unordered_map<std::string, CandidateStat> &freqStats) {
    struct Row {
        std::string phrase;
        std::string key;
        int exactCount = 0;
        int prefixCount = 0;
        double weight = 0.0;
        size_t order = 0;
    };
    std::vector<Row> rows;
    if (chunkSegs.empty() || chunkSegs[0].empty()) return {};

    // 首字母分桶（局部快速索引）
    std::unordered_map<char, std::vector<std::pair<std::string, size_t>>> bucket;
    {
        size_t ord = 0;
        for (const auto &kv : dict) {
            const std::string &k = kv.first;
            if (!k.empty()) {
                const unsigned char c0 = static_cast<unsigned char>(k[0]);
                if (c0 < 128u && std::isalpha(c0)) {
                    bucket[static_cast<char>(std::tolower(c0))].push_back({k, ord});
                }
            }
            ++ord;
        }
    }

    const unsigned char c0 = static_cast<unsigned char>(chunkSegs[0][0]);
    if (!(c0 < 128u && std::isalpha(c0))) return {};
    const char bk = static_cast<char>(std::tolower(c0));
    const auto bit = bucket.find(bk);
    if (bit == bucket.end()) return {};

    for (const auto &ko : bit->second) {
        const std::string &k = ko.first;
        const size_t order = ko.second;
        const auto segs = splitComma(k);
        if (segs.size() != chunkSegs.size()) continue;

        int exactCnt = 0;
        int prefixCnt = 0;
        bool ok = true;
        for (size_t i = 0; i < segs.size(); ++i) {
            if (chunkSegs[i] == segs[i]) {
                ++exactCnt;
            } else if (segPrefixMatch(chunkSegs[i], segs[i])) {
                ++prefixCnt;
            } else {
                ok = false;
                break;
            }
        }
        if (!ok) continue;

        const auto dit = dict.find(k);
        if (dit == dict.end()) continue;
        for (const auto &ph : dit->second) {
            if (splitUtf8Chars(ph).size() != segs.size()) continue;
            Row r;
            r.phrase = ph;
            r.key = k;
            r.exactCount = exactCnt;
            r.prefixCount = prefixCnt;
            r.order = order;
            const auto fs = freqStats.find(k + "\t" + ph);
            r.weight = (fs == freqStats.end()) ? 0.0 : static_cast<double>(fs->second.count);
            rows.push_back(std::move(r));
        }
    }

    std::stable_sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
        if (a.exactCount != b.exactCount) return a.exactCount > b.exactCount;
        if (a.prefixCount != b.prefixCount) return a.prefixCount > b.prefixCount;
        if (a.weight != b.weight) return a.weight > b.weight;
        return a.order < b.order;
    });

    std::vector<std::pair<std::string, std::string>> out;
    std::unordered_set<std::string> seen;
    for (const auto &r : rows) {
        if (!seen.insert(r.phrase).second) continue;
        out.push_back({r.phrase, r.key});
    }
    return out;
}

/** ============================================================
 * 区块21: 拼音算法兼容层
 * 描述: 调用已抽离到 pinyin 模块的算法
 * 抽离建议: 保留在此，作为向后兼容的适配层
 * ============================================================ */
[[maybe_unused]] static std::vector<std::string> compute_segmented_pinyin_inputs(
    const std::string &rawInput,
    const std::unordered_set<std::string> &mapSyllables) {
    return computeSegmentedPinyinInputs(rawInput, mapSyllables);
}

/** ============================================================
 * 区块22: UTF-8前缀工具
 * 描述: 获取UTF-8字符前缀序列
 * 抽离建议: 可合并到区块12
 * ============================================================ */
static std::string utf8FirstChar(const std::string &s) {
    const auto parts = splitUtf8Chars(s);
    return parts.empty() ? std::string{} : parts[0];
}

static std::vector<std::string> utf8Prefixes(const std::string &s) {
    // 返回 UTF-8 字符前缀序列：z, zh, zho ...（支持多字节安全前缀）。
    std::vector<std::string> out;
    std::string cur;
    for (const auto &ch : splitUtf8Chars(s)) {
        cur += ch;
        out.push_back(cur);
    }
    return out;
}

// 伪代码同款：prefixSegs 是否"包含于" fullSegs（段数不超过，且每段为前缀）

// 最终候选收敛规则（多段）：
// - 前 n-1 段必须与目标段完全相等
// - 仅最后一段允许"前缀匹配"
// 例如：nang,zhong,xiu,s 可匹配 nang,zhong,xiu,se

/** ============================================================
 * 区块23: 逗号分隔键索引重建
 * 描述: 为多音节键建立首字母和首音节前缀索引
 * 抽离建议: 可抽离到 dict.cpp
 * ============================================================ */
/**
 * 为含逗号键建立索引：
 * 1) 首音节首 UTF-8 字符分桶（如 z）；
 * 2) 首音节连续前缀分桶（如 z, zh, zho, zhon, zhong）。
 *
 * 连续输入时先按「首音节连续前缀」收窄范围（z->zh->zho...），
 * 找不到再在匹配阶段隐式分隔并回溯。
 */
static void rebuildCommaKeyIndex(const std::unordered_map<std::string, std::vector<std::string>> &dict,
                                 std::unordered_map<std::string, std::vector<std::string>> &byFirstSyllChar,
                                 std::unordered_map<std::string, std::vector<std::string>> &byFirstSyllPrefix) {
    byFirstSyllChar.clear();
    byFirstSyllPrefix.clear();
    for (const auto &kv : dict) {
        const std::string &key = kv.first;
        if (key.find(',') == std::string::npos) {
            continue;
        }
        const auto sylls = splitComma(key);
        if (sylls.empty()) {
            continue;
        }
        const std::string c1 = utf8FirstChar(sylls[0]);
        if (!c1.empty()) {
            byFirstSyllChar[c1].push_back(key);
        }
        for (const auto &pf : utf8Prefixes(sylls[0])) {
            if (!pf.empty()) {
                byFirstSyllPrefix[pf].push_back(key);
            }
        }
    }
}

/** ============================================================
 * 区块24: 候选词合并
 * 描述: 向词典中合并新候选词（去重）
 * 抽离建议: 可抽离到 dict.cpp
 * ============================================================ */
static void mergeUniqueCandidate(std::unordered_map<std::string, std::vector<std::string>> &dict,
                                 const std::string &py,
                                 const std::string &phrase) {
    auto &v = dict[py];
    for (const auto &x : v) {
        if (x == phrase) {
            return;
        }
    }
    v.push_back(phrase);
}

/** ============================================================
 * 区块25: 词典加载
 * 描述: 从文件加载基础词库和用户词库
 * 抽离建议: 可抽离到 dict.cpp
 * ============================================================ */
static void loadBaseDict(const std::string &path,
                         std::unordered_map<std::string, std::vector<std::string>> &dict) {
    std::ifstream in(path);
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        auto p = line.find(' ');
        if (p == std::string::npos) continue;
        std::string py = trim(line.substr(0, p));
        std::string chars = trim(line.substr(p + 1));
        if (py.empty() || chars.empty()) continue;
        dict[py] = splitUtf8Chars(chars);
    }
}

static void loadUserDict(const std::string &path,
                         std::unordered_map<std::string, std::vector<std::string>> &dict) {
    std::ifstream in(path);
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        auto p = line.find(' ');
        if (p == std::string::npos) continue;
        std::string py = trim(line.substr(0, p));
        std::string phrase = trim(line.substr(p + 1));
        if (py.empty() || phrase.empty()) continue;
        mergeUniqueCandidate(dict, py, phrase);
    }
}

/** ============================================================
 * 区块26: 文件操作工具
 * 描述: 创建目录/文件、获取修改时间
 * 抽离建议: 可抽离到 utils.cpp
 * ============================================================ */
static bool ensureParentDirAndFile(const std::string &path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path p(path);
    if (p.has_parent_path()) {
        fs::create_directories(p.parent_path(), ec);
        if (ec) return false;
    }
    if (!fs::exists(p, ec)) {
        std::ofstream touch(path, std::ios::app);
        if (!touch) return false;
    }
    return true;
}

static bool getFileMtimeSec(const std::string &path, time_t *outSec) {
    struct stat st {};
    if (stat(path.c_str(), &st) != 0) {
        return false;
    }
    *outSec = st.st_mtime;
    return true;
}

/** ============================================================
 * 区块27: 词频键与原子写入
 * 描述: 词频文件操作、带锁的原子写入
 * 抽离建议: 可抽离到 dict.cpp
 * ============================================================ */
static std::string makeFreqKey(const std::string &py, const std::string &phrase) {
    return py + "\t" + phrase;
}

static bool saveTextAtomicWithLock(const std::string &path, const std::string &content) {
    // 原子写入策略：
    // 1) path.lock 文件锁防并发；2) 先写 tmp；3) rename 覆盖目标。
    // 这样即使异常中断，也尽量避免词库被写坏。
    if (!ensureParentDirAndFile(path)) {
        return false;
    }
    const std::string lockPath = path + ".lock";
    int lockFd = ::open(lockPath.c_str(), O_CREAT | O_RDWR, 0644);
    if (lockFd < 0) return false;

    struct flock lk {};
    lk.l_type = F_WRLCK;
    lk.l_whence = SEEK_SET;
    lk.l_start = 0;
    lk.l_len = 0;
    if (fcntl(lockFd, F_SETLKW, &lk) != 0) {
        ::close(lockFd);
        return false;
    }

    const std::string tmpPath = path + ".tmp";
    {
        std::ofstream out(tmpPath, std::ios::trunc);
        if (!out) {
            lk.l_type = F_UNLCK;
            fcntl(lockFd, F_SETLK, &lk);
            ::close(lockFd);
            return false;
        }
        out << content;
        out.flush();
        if (!out.good()) {
            lk.l_type = F_UNLCK;
            fcntl(lockFd, F_SETLK, &lk);
            ::close(lockFd);
            return false;
        }
    }
    if (std::rename(tmpPath.c_str(), path.c_str()) != 0) {
        std::remove(tmpPath.c_str());
        lk.l_type = F_UNLCK;
        fcntl(lockFd, F_SETLK, &lk);
        ::close(lockFd);
        return false;
    }

    lk.l_type = F_UNLCK;
    fcntl(lockFd, F_SETLK, &lk);
    ::close(lockFd);
    return true;
}

/** ============================================================
 * 区块28: 用户词典原子插入
 * 描述: 去重、排序、原子写入新条目
 * 抽离建议: 可抽离到 dict.cpp
 * ============================================================ */
static bool insertUserDictSortedAtomic(const std::string &path, const std::string &py, const std::string &phrase) {
    // 统一入口：去重 + 排序 + 原子落盘。用于 user_dict 与 learned_phrases 两类文件。
    if (!ensureParentDirAndFile(path)) {
        return false;
    }

    std::ifstream in(path);
    std::string line;
    std::vector<std::string> comments;
    std::vector<std::string> entries;
    std::unordered_set<std::string> seen;

    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') {
            comments.push_back(line);
            continue;
        }
        const size_t p = t.find(' ');
        if (p == std::string::npos) {
            // 非法行保留为注释区，避免丢数据
            comments.push_back(line);
            continue;
        }
        std::string normalized = trim(t.substr(0, p)) + " " + trim(t.substr(p + 1));
        if (normalized.empty()) continue;
        if (seen.insert(normalized).second) {
            entries.push_back(normalized);
        }
    }

    const std::string newEntry = py + " " + phrase;
    if (seen.insert(newEntry).second) {
        entries.push_back(newEntry);
    }

    std::sort(entries.begin(), entries.end(),
              [](const std::string &a, const std::string &b) {
                  return a < b; // 按字节序（ASCII 优先）排序
              });

    std::ostringstream out;
    for (const auto &c : comments) {
        out << c << "\n";
    }
    for (const auto &e : entries) {
        out << e << "\n";
    }
    return saveTextAtomicWithLock(path, out.str());
}

/** ============================================================
 * 区块29: 词频统计加载与保存
 * 描述: 词频文件的读写
 * 抽离建议: 可抽离到 dict.cpp
 * ============================================================ */
static void loadFreqStats(const std::string &path, std::unordered_map<std::string, CandidateStat> &stats) {
    stats.clear();
    std::ifstream in(path);
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        std::istringstream iss(line);
        std::string py;
        std::string phrase;
        unsigned long long cnt = 0;
        long long ts = 0;
        if (!(iss >> py >> phrase >> cnt >> ts)) continue;
        CandidateStat st;
        st.count = cnt;
        st.lastTs = static_cast<time_t>(ts);
        stats[makeFreqKey(py, phrase)] = st;
    }
}

static bool saveFreqStatsAtomic(const std::string &path,
                                const std::unordered_map<std::string, CandidateStat> &stats) {
    std::ostringstream out;
    for (const auto &kv : stats) {
        const std::string &key = kv.first;
        const CandidateStat &st = kv.second;
        const size_t p = key.find('\t');
        if (p == std::string::npos) continue;
        const std::string py = key.substr(0, p);
        const std::string phrase = key.substr(p + 1);
        out << py << " " << phrase << " " << st.count << " "
            << static_cast<long long>(st.lastTs) << "\n";
    }
    return saveTextAtomicWithLock(path, out.str());
}

/** ============================================================
 * 区块30: 词典重载
 * 描述: 并行加载基础词库和用户词库
 * 抽离建议: 可抽离到 dict.cpp
 * ============================================================ */
static void reloadAllDicts(const AppConfig &cfg,
                           std::unordered_map<std::string, std::vector<std::string>> &dict,
                           std::unordered_map<std::string, std::unordered_set<std::string>> &userSet,
                           bool useParallelLoad) {
    dict.clear();
    userSet.clear();
    std::unordered_map<std::string, std::vector<std::string>> baseDict;
    std::unordered_map<std::string, std::vector<std::string>> userDict;
    if (useParallelLoad) {
        // 双词库并行加载：减少启动/热重载等待时间。
        auto fb = std::async(std::launch::async, [&]() { loadBaseDict(cfg.dictPath, baseDict); });
        auto fu = std::async(std::launch::async, [&]() { loadUserDict(cfg.userDictPath, userDict); });
        fb.get();
        fu.get();
    } else {
        loadBaseDict(cfg.dictPath, baseDict);
        loadUserDict(cfg.userDictPath, userDict);
    }
    dict = std::move(baseDict);
    for (const auto &kv : userDict) {
        const std::string &py = kv.first;
        for (const auto &phrase : kv.second) {
            mergeUniqueCandidate(dict, py, phrase);
            userSet[py].insert(phrase);
        }
    }
}

/** ============================================================
 * 区块31: UTF-8退格操作
 * 描述: 安全回退一个UTF-8字符
 * 抽离建议: 可合并到区块12
 * ============================================================ */
static void popLastUtf8Char(std::string &s) {
    // UTF-8 安全退格：回退一个"字符"，而非一个字节。
    if (s.empty()) {
        return;
    }
    size_t end = s.size();
    size_t i = end;
    while (i > 0 && (static_cast<unsigned char>(s[i - 1]) & 0xC0u) == 0x80u) {
        i--;
    }
    if (i == 0) {
        s.clear();
        return;
    }
    const size_t start = i - 1;
    const unsigned char c = static_cast<unsigned char>(s[start]);
    size_t len = 1;
    if ((c & 0x80u) == 0) {
        len = 1;
    } else if ((c & 0xE0u) == 0xC0u) {
        len = 2;
    } else if ((c & 0xF0u) == 0xE0u) {
        len = 3;
    } else if ((c & 0xF8u) == 0xF0u) {
        len = 4;
    }
    if (start + len <= s.size()) {
        s.erase(start, len);
    } else {
        s.erase(start);
    }
}

/** ============================================================
 * 区块32: 显示字符串处理
 * 描述: 去除CR/LF、终端尺寸获取、字符串截断
 * 抽离建议: 可抽离到 ui.cpp
 * ============================================================ */
// composed：当前行已写入 PTY 的 UTF-8 字节序列（英文和选字按时间顺序拼接）
static void stripCrLfForDisplay(const std::string &in, std::string &out) {
    out.clear();
    out.reserve(in.size());
    for (unsigned char c : in) {
        if (c != '\r' && c != '\n') {
            out.push_back(static_cast<char>(c));
        }
    }
}

static void getTermSize(unsigned &rows, unsigned &cols) {
    rows = 24;
    cols = 80;
#if !defined(_WIN32)
    struct winsize ws {};
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        cols = static_cast<unsigned>(ws.ws_col);
        rows = static_cast<unsigned>(ws.ws_row);
    }
#endif
}

static std::string truncateUtf8Safe(const std::string &s, size_t maxBytes) {
    if (s.size() <= maxBytes) {
        return s;
    }
    std::string out = s.substr(0, maxBytes);
    while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0u) == 0x80u) {
        out.pop_back();
    }
    return out;
}

/** ============================================================
 * 区块33: 显示宽度计算
 * 描述: 估算UTF-8字符串的终端显示宽度
 * 抽离建议: 可合并到区块32
 * ============================================================ */
static size_t displayWidthUtf8(const std::string &s) {
    // 终端宽度近似：ASCII=1，非 ASCII=2。用于候选栏宽度裁剪。
    size_t w = 0;
    for (const auto &ch : splitUtf8Chars(s)) {
        if (ch.size() == 1 && static_cast<unsigned char>(ch[0]) < 0x80u) {
            w += 1;
        } else {
            w += 2; // 终端下中文等宽粗略按 2 计
        }
    }
    return w;
}

/** ============================================================
 * 区块34: 光标显示
 * 描述: 在字符串中插入可见光标
 * 抽离建议: 可抽离到 ui.cpp
 * ============================================================ */
static std::string withVisibleCursor(const std::string &s, size_t cursor) {
    if (cursor > s.size()) cursor = s.size();
    std::string out = s;
    out.insert(out.begin() + static_cast<std::string::difference_type>(cursor), '|');
    return out;
}

/** ============================================================
 * 区块35: 光标位置映射
 * 描述: 将原始输入光标映射到带分隔符的展示串位置
 * 抽离建议: 可合并到区块34
 * ============================================================ */
// 将"原始输入光标位置"映射到"带自动分隔符的展示串"位置：
// - 自动分隔符 ','：不占原始输入位置（光标会自然跳过）
// - 手动分隔符 '\''：来自原始输入，参与光标位置
static size_t mapRawCursorToDecorated(const std::string &rawInput,
                                      const std::string &decoratedInput,
                                      size_t rawCursor) {
    if (rawCursor > rawInput.size()) rawCursor = rawInput.size();
    size_t i = 0; // raw
    size_t j = 0; // decorated
    while (i < rawCursor && j < decoratedInput.size()) {
        const char dc = decoratedInput[j];
        if (dc == ',') { // 自动分隔符，跳过
            ++j;
            continue;
        }
        if (rawInput[i] == dc) {
            ++i;
            ++j;
            continue;
        }
        // 兜底：遇到不一致时，优先跳过展示侧的自动分隔符外字符，避免卡死
        ++j;
    }
    return j;
}

/** ============================================================
 * 区块36: 数字显示格式
 * 描述: 下标数字和圈号数字
 * 抽离建议: 可合并到区块34
 * ============================================================ */
static std::string subscriptDigit(int n) {
    static const char *kSub[10] = {"\xE2\x82\x80", "\xE2\x82\x81", "\xE2\x82\x82", "\xE2\x82\x83", "\xE2\x82\x84",
                                   "\xE2\x82\x85", "\xE2\x82\x86", "\xE2\x82\x87", "\xE2\x82\x88", "\xE2\x82\x89"};
    if (n < 0 || n > 9) return std::to_string(n);
    return kSub[n];
}

static std::string dottedDigit(int n) {
    static const char *kDotted[10] = {"", "\xE2\x92\x88", "\xE2\x92\x89", "\xE2\x92\x8A", "\xE2\x92\x8B",
                                      "\xE2\x92\x8C", "\xE2\x92\x8D", "\xE2\x92\x8E", "\xE2\x92\x8F", "\xE2\x92\x90"};
    if (n < 1 || n > 9) return std::to_string(n);
    return kDotted[n];
}

/** ============================================================
 * 区块37: 底栏内容构建
 * 描述: 构建状态栏/底栏的显示内容
 * 抽离建议: 可抽离到 ui.cpp
 * ============================================================ */
// 底栏：始终显示当前中英文状态（[中]/[EN]）。
static std::string buildBottomBarContent(bool imeOn,
                                         bool addWordMode,
                                         const std::string &input,
                                         size_t inputCursor,
                                         const std::vector<std::string> &cands,
                                         const std::vector<bool> &candFromFallback,
                                         size_t candPageStart,
                                         const std::string &addWordPhrase,
                                         size_t pageSize,
                                         size_t maxCols,
                                         bool debugMode,
                                         bool modeHintActive,
                                         bool modeHintIsZh) {
    (void)modeHintActive;
    (void)modeHintIsZh;
    if (!imeOn) return "[EN]";
    if (maxCols < 20) {
        maxCols = 20;
    }
    if (gSimpleImeDisplayMode) {
        std::string bottom = "[中]";
        if (addWordMode) {
            bottom += "[加词]";
        }
        if (!gSimpleImeStatusText.empty()) {
            bottom += " ";
            const size_t dispCursor = mapRawCursorToDecorated(input, gSimpleImeStatusText, inputCursor);
            bottom += withVisibleCursor(gSimpleImeStatusText, dispCursor);
        } else if (!input.empty()) {
            bottom += " ";
            bottom += withVisibleCursor(input, inputCursor);
        }
        if (pageSize < 1) pageSize = 1;
        if (pageSize > 9) pageSize = 9;
        size_t start = candPageStart;
        if (start >= cands.size()) start = 0;
        const int shown = std::min<int>(static_cast<int>(pageSize), static_cast<int>(cands.size() - start));
        for (int i = 0; i < shown; ++i) {
            const size_t idx = start + static_cast<size_t>(i);
            std::string item = " ";
            item += subscriptDigit(i + 1);
            item += cands[idx];
            if (displayWidthUtf8(bottom) + displayWidthUtf8(item) <= maxCols) {
                bottom += item;
                continue;
            }
            // 至少尽量显示第一条
            if (i == 0 && displayWidthUtf8(bottom) < maxCols) {
                for (const auto &ch : splitUtf8Chars(item)) {
                    const size_t cw = displayWidthUtf8(ch);
                    if (displayWidthUtf8(bottom) + cw > maxCols) break;
                    bottom += ch;
                }
            }
            break;
        }
        return bottom;
    }
    // 状态栏采用紧凑拼接：去掉多余空格，尽可能挤出候选可见长度。
    std::string bottom = "[中]";
    if (addWordMode) {
        bottom += "[加词]";
    }
    if (!input.empty()) {
        bottom += withVisibleCursor(input, inputCursor);
    }
    if (pageSize < 1) pageSize = 1;
    if (pageSize > 9) pageSize = 9;
    size_t start = candPageStart;
    if (start >= cands.size()) {
        start = 0;
    }
    const int shown = std::min<int>(static_cast<int>(pageSize), static_cast<int>(cands.size() - start));
    for (int i = 0; i < shown; ++i) {
        const size_t idx = start + static_cast<size_t>(i);
        const bool isFallback = (idx < candFromFallback.size() && candFromFallback[idx]);
        const std::string plainCand = cands[idx];
        // 标记规则：
        // - 普通候选：下标 ₁..₉
        // - debug 且连续输入候选：圈号点数 ⒈..⒐
        const std::string marker = (debugMode && isFallback) ? dottedDigit(i + 1) : subscriptDigit(i + 1);
        std::string item = marker;
        item += plainCand;
        item += " ";
        std::string plainItem = marker;
        plainItem += plainCand;
        plainItem += " ";
        if (displayWidthUtf8(bottom) + displayWidthUtf8(plainItem) <= maxCols) {
            bottom += item;
            continue;
        }
        // 当前候选过长时，至少尽量显示第一条的一部分
        if (i == 0 && displayWidthUtf8(bottom) < maxCols) {
            // 按显示宽度逐字符追加，避免中文双宽被字节长度误判
            for (const auto &ch : splitUtf8Chars(plainItem)) {
                const size_t cw = displayWidthUtf8(ch);
                if (displayWidthUtf8(bottom) + cw > maxCols) break;
                bottom += ch;
            }
        }
        break;
    }
    if (addWordMode && input.empty() && cands.empty() && !addWordPhrase.empty()) {
        const std::string hint = "[回车写入]";
        if (displayWidthUtf8(bottom) + displayWidthUtf8(hint) <= maxCols) {
            bottom += hint;
        }
    }
    return bottom;
}

/** ============================================================
 * 区块38: 状态栏绘制
 * 描述: 在第一行或最后一行绘制状态栏
 * 抽离建议: 可抽离到 ui.cpp
 * ============================================================ */
/**
 * 清空第一行（使用光标保存/恢复机制，不影响其他行的光标位置）
 * @param rows 终端行数（仅用于判断行数是否不足）
 */
[[maybe_unused]] static void clearTopRow(unsigned rows) {
    if (rows < 2) {
        // 终端行数不足，无法使用第一行，清除当前行
        std::cout << "\r\033[2K" << std::flush;
        return;
    }
    // 使用光标保存/恢复机制：
    // \0337：保存当前光标位置
    // \033[1;1H：移动到第一行第一列
    // \033[2K：清除第一行
    // \0338：恢复之前保存的光标位置
    std::cout << "\0337\033[1;1H\033[2K\0338" << std::flush;
}

[[maybe_unused]] static void paintStatusOnTopRow(const std::string &status, unsigned rows, unsigned cols) {
    if (rows < 2) {
        return;
    }
    if (cols < 8) {
        cols = 8;
    }
    const std::string line = status;
    std::cout << "\0337\033[1;1H\033[2K" << line << "\0338" << std::flush;
}

// 底行状态栏策略：
// 1) PTY 只看到内容区高度（总行数-1）；
// 2) 真实终端滚动区限制在第 1..N-1 行，底行预留给状态栏。

#if !defined(_WIN32)
static int g_masterFdAtExit = -1;
static unsigned short g_lastScrollRegionRows = 0;

static void applyBottomReservedScrollRegionIfNeeded(bool force) {
    struct winsize ws {};
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_row == 0 || ws.ws_col == 0) {
        return;
    }
    if (!force && g_lastScrollRegionRows == ws.ws_row) {
        return;
    }
    if (ws.ws_row <= 1) {
        std::cout << "\0337\033[r\0338" << std::flush;
        g_lastScrollRegionRows = ws.ws_row;
        return;
    }
    // 仅让 1..N-1 行参与滚动；第 N 行预留给状态栏。
    const unsigned short bottom = static_cast<unsigned short>(ws.ws_row - 1);
    // 保护光标位置：某些程序退出时会重置滚动区，重新应用时不要把光标硬挪走。
    std::cout << "\0337\033[1;" << bottom << "r\0338" << std::flush;
    g_lastScrollRegionRows = ws.ws_row;
}

static void restoreTerminalAfterIme() {
    // 复位滚动区、显示光标、默认 SGR（若曾误留 ANSI 状态可缓解）
    std::cout << "\033[r\033[?25h\033[0m" << std::flush;
    g_lastScrollRegionRows = 0;
    if (g_masterFdAtExit >= 0) {
        struct winsize ws {};
        if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
            ioctl(g_masterFdAtExit, TIOCSWINSZ, &ws);
        }
    }
}
#endif

static void prepareCursorForExit(bool shellMode, bool imeOn) {
    (void)shellMode;
    (void)imeOn;
    unsigned rows = 24;
    unsigned cols = 80;
    getTermSize(rows, cols);
    (void)cols;
    // 退出后清理整屏残留，再显示退出提示。
    std::cout << "\033[2J"
              << "\033[" << rows << ";1H\033[2K"
              << "\033[1;1H\033[2K[exited]\033[2;1H" << std::flush;
}

#if !defined(_WIN32)
static bool outputRequestsBottomBarRedraw(const char *buf, size_t n) {
    // 识别常见清屏触发：Ctrl+L, ESC c, CSI 2J/3J。
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(buf[i]);
        if (c == 0x0cu) return true; // Ctrl+L
        if (c == 0x1bu) {
            if (i + 1 < n && buf[i + 1] == 'c') return true; // RIS
            if (i + 3 < n && buf[i + 1] == '[' && (buf[i + 2] == '2' || buf[i + 2] == '3') && buf[i + 3] == 'J') {
                return true;
            }
        }
    }
    return false;
}

static bool containsEscSeq(const char *buf, size_t n, const char *seq) {
    if (!buf || !seq) return false;
    const size_t m = std::strlen(seq);
    if (m == 0 || n < m) return false;
    for (size_t i = 0; i + m <= n; ++i) {
        if (std::memcmp(buf + i, seq, m) == 0) {
            return true;
        }
    }
    return false;
}

static bool outputMayBreakBottomReservedRegion(const char *buf, size_t n) {
    // 这些序列常见于 TUI 退出或终端状态重置，可能导致滚动区/光标行为回到默认。
    return containsEscSeq(buf, n, "\x1b[r") ||         // reset scroll region
           containsEscSeq(buf, n, "\x1b[?1049l") ||    // leave alt screen
           containsEscSeq(buf, n, "\x1b[?1047l") ||
           containsEscSeq(buf, n, "\x1b[?47l");
}

static void moveCursorToContentAreaLastRow() {
    unsigned rows = 24;
    unsigned cols = 80;
    getTermSize(rows, cols);
    (void)cols;
    if (rows <= 1) {
        std::cout << "\033[1;1H" << std::flush;
        return;
    }
    // 底栏保留在最后一行，光标强制回到内容区最后一行，避免与状态栏抢位。
    std::cout << "\033[" << (rows - 1) << ";1H" << std::flush;
}
#endif

static void paintBottomOnLastRow(const std::string &bottom, unsigned rows, unsigned cols) {
    if (rows < 2) {
        return;
    }
    if (cols < 8) {
        cols = 8;
    }
    const std::string line = bottom;
    std::cout << "\0337\033[" << rows << ";1H\033[2K" << line << "\0338" << std::flush;
}

/** ============================================================
 * 区块39: 重绘行
 * 描述: 根据当前状态重绘显示行
 * 抽离建议: 可抽离到 ui.cpp
 * ============================================================ */
static void redrawLine(bool shellMode,
                       bool imeOn,
                       bool addWordMode,
                       const std::string &composed,
                       const std::string &input,
                       size_t inputCursor,
                       const std::vector<std::string> &cands,
                       const std::vector<bool> &candFromFallback,
                       size_t candPageStart,
                       const std::string &addWordPhrase,
                       const AppConfig &cfg,
                       bool modeHintActive,
                       bool modeHintIsZh) {
    unsigned rows = 24;
    unsigned cols = 80;
    getTermSize(rows, cols);
    if (cols < 20) {
        cols = 20;
    }

    std::string disp;
    stripCrLfForDisplay(composed, disp);
    if (imeOn && !input.empty()) {
        disp += " ";
        disp += input;
    }

    const std::string bottom =
        buildBottomBarContent(imeOn, addWordMode, input, inputCursor, cands, candFromFallback, candPageStart, addWordPhrase,
                              cfg.candidatePageSize, cols, cfg.debugMode, modeHintActive, modeHintIsZh);

    if (shellMode) {
        // shell 模式：始终绘制底栏，持续显示 [中]/[EN]。
        paintBottomOnLastRow(bottom, rows, cols);
        return;
    }

    // 非 shell 模式：清除当前行并显示内容（状态栏在最后一行）
    if (rows < 2) {
        std::cout << "\r\033[2K" << truncateUtf8Safe(disp + " | " + bottom, cols) << std::flush;
    } else {
        std::cout << "\r\033[2K" << truncateUtf8Safe(disp, cols) << std::flush;
        paintBottomOnLastRow(bottom, rows, cols);
    }
}

/** ============================================================
 * 区块40: 帮助信息
 * 描述: 打印使用帮助
 * 抽离建议: 保留在此，属于主程序逻辑
 * ============================================================ */
static void printUsage(const char *argv0) {
    std::cerr
        << "lite-tty-ime " << kVersion << "\n"
        << "\n"
        << "【一行用法】\n"
        << "  " << argv0 << " [选项]\n"
        << "\n"
        << "【这个程序是做什么的】\n"
        << "  这是一个在终端里用的拼音输入法：\n"
        << "  - 你输入拼音，程序给候选词；\n"
        << "  - 你按数字选词；\n"
        << "  - 会根据你的使用习惯调整词频排序；\n"
        << "  - 词库文件变更后可自动热重载（不用重启）。\n"
        << "\n"
        << "【最常用的 3 条命令】\n"
        << "  " << argv0 << " --help\n"
        << "  " << argv0 << " --version\n"
        << "  " << argv0 << " --config lite-tty-ime.ini\n"
        << "\n"
        << "【命令行参数详解】\n"
        << "  -h, --help\n"
        << "      显示这份帮助。\n"
        << "  -V, --version\n"
        << "      显示版本号并退出（不进入输入法）。\n"
        << "  -s, --startup-cmd \"CMD\"\n"
        << "      启动后先自动执行一条 shell 命令。\n"
        << "      例：-s \"cd ~/proj && ls\"\n"
        << "  --debug\n"
        << "      打开调试输出（用于排错）。\n"
        << "  --dict PATH\n"
        << "      指定基础词库路径。\n"
        << "      默认: " << kBaseDictPath << "\n"
        << "  --user-dict PATH\n"
        << "      指定用户词库路径（你的自定义词写到这里）。\n"
        << "      默认会优先尝试 " << kBundledDataDir << "/；如果不可写，会自动回退到用户目录。\n"
        << "      程序会自动创建目录/文件，并维护对应词频文件 PATH.freq。\n"
        << "  --config PATH\n"
        << "      指定配置文件路径（key=value）。\n"
        << "      注意：命令行参数优先级高于配置文件。\n"
        << "  --log-file PATH\n"
        << "      写运行日志到指定文件（启动、热加载、写词、退出等）。\n"
        << "  --toggle-key KEY\n"
        << "      设置中英文切换键。\n"
        << "      例：ctrl+space、ctrl+]、esc、tab、a\n"
        << "  --exit-key KEY\n"
        << "      设置快捷退出键。\n"
        << "      例：ctrl+]、ctrl+d；设为 none 表示禁用快捷退出（默认就是 none）。\n"
        << "\n"
        << "【键位（默认行为）】\n"
        << "  1..9   选择当前页候选\n"
        << "  +/=    候选下一页\n"
        << "  -      候选上一页\n"
        << "  '      手动分隔拼音（不跳过）\n"
        << "  Esc / 方向键序列  优先透传给 shell 内程序（输入法尽量不占用）\n"
        << "\n"
        << "【配置文件常用键】\n"
        << "  candidate_page_size = 5        # 候选页大小，范围 1..9\n"
        << "  space_action = select_first    # 空格动作：select_first/commit_raw/send_line/none\n"
        << "  enter_action = commit_raw      # 回车动作：select_first/commit_raw/send_line/none\n"
        << "  show_con_stats = false         # 是否显示连续输入统计\n"
        << "  show_store_hint = false        # 是否显示存词/存短语提示\n"
        << "\n"
        << "【完整示例】\n"
        << "  " << argv0 << " --config lite-tty-ime.ini\n"
        << "  " << argv0 << " --dict " << kBaseDictPath << " --user-dict /path/to/user_dict.txt\n"
        << "  " << argv0 << " --toggle-key ctrl+] --exit-key ctrl+d --log-file /tmp/lite-tty-ime.log\n"
        << "  " << argv0 << " -s \"cd ~/proj && ls -la\"\n";
}

/** ============================================================
 * 区块41: Shell PTY设置
 * 描述: 创建伪终端用于shell模式
 * 抽离建议: 可抽离到 shell.cpp
 * ============================================================ */
#if !defined(_WIN32)
static void buildContentAreaWinsize(struct winsize *wsOut) {
    struct winsize ws {};
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) != 0) {
        ws.ws_row = 24;
        ws.ws_col = 80;
    }
    // 顶行保留给状态栏：子进程只看到内容区高度。
    if (ws.ws_row > 1) {
        ws.ws_row = static_cast<unsigned short>(ws.ws_row - 1);
    } else {
        ws.ws_row = 1;
    }
    if (ws.ws_col == 0) {
        ws.ws_col = 80;
    }
    *wsOut = ws;
}

static bool setupShellPty(int *masterOut, pid_t *childOut) {
    struct winsize ws {};
    buildContentAreaWinsize(&ws);
    int master = -1;
    pid_t pid = forkpty(&master, nullptr, nullptr, &ws);
    if (pid < 0) {
        perror("forkpty");
        return false;
    }
    if (pid == 0) {
        const char *sh = std::getenv("SHELL");
        if (!sh || !*sh) {
            sh = "/bin/bash";
        }
        execl(sh, sh, static_cast<char *>(nullptr));
        perror("execl");
        _exit(127);
    }
    *masterOut = master;
    *childOut = pid;
    return true;
}

static void terminateChildPtyProcess(pid_t childPid) {
    if (childPid <= 0) {
        return;
    }

    // 先尝试优雅退出，避免直接强杀导致终端状态异常。
    kill(childPid, SIGTERM);
    for (int i = 0; i < 10; ++i) { // 最多等待约 100ms
        if (waitpid(childPid, nullptr, WNOHANG) == childPid) {
            return;
        }
        usleep(10 * 1000);
    }

    // 仍未退出则强杀，避免主程序卡在 waitpid 阻塞。
    kill(childPid, SIGKILL);
    waitpid(childPid, nullptr, 0);
}

static void syncPtyContentWinsizeIfNeeded(int masterFd, unsigned short *lastRows, unsigned short *lastCols) {
    if (masterFd < 0 || lastRows == nullptr || lastCols == nullptr) {
        return;
    }
    struct winsize ws {};
    buildContentAreaWinsize(&ws);
    if (ws.ws_row == *lastRows && ws.ws_col == *lastCols) {
        return;
    }
    ioctl(masterFd, TIOCSWINSZ, &ws);
    *lastRows = ws.ws_row;
    *lastCols = ws.ws_col;
}

static void writeRawFd(int fd, const char *p, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t w = ::write(fd, p + off, n - off);
        if (w <= 0) {
            break;
        }
        off += static_cast<size_t>(w);
    }
}
#endif

/** ============================================================
 * 区块42: 主函数
 * 描述: 程序入口，包含所有运行态逻辑
 * 抽离建议: 主函数本身保留，但可拆分成多个模块
 * ============================================================ */
int main(int argc, char **argv) {
    // ---------------------------
    // 42-1) 参数与配置解析
    // ---------------------------
    AppConfig cfg;
    bool addWordMode = false;
    bool shellMode = true;
    std::string configPath;
    std::string startupCommand;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            configPath = argv[i + 1];
            break;
        }
    }
    if (!configPath.empty()) {
        loadConfigFile(configPath, &cfg);
    }

    for (int i = 1; i < argc; ++i) {
        auto hasArgValue = [&](int idx) -> bool {
            if (idx + 1 >= argc) {
                return false;
            }
            return argv[idx + 1][0] != '-';
        };
        if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0) {
            printUsage(argv[0]);
            return 0;
        }
        if (std::strcmp(argv[i], "-V") == 0 || std::strcmp(argv[i], "--version") == 0) {
            std::cout << "lite-tty-ime " << kVersion << "\n";
            return 0;
        }
        if (std::strcmp(argv[i], "--debug") == 0) {
            cfg.debugMode = true;
            continue;
        }
        if (std::strcmp(argv[i], "-a") == 0 || std::strcmp(argv[i], "--add-word") == 0) {
            std::cerr << "参数已移除: " << argv[i] << "\n";
            return 1;
        }
        if (std::strcmp(argv[i], "--no-shell") == 0 ||
            std::strcmp(argv[i], "--no-auto-add-on-ime-off") == 0 ||
            std::strcmp(argv[i], "--fallback-only-when-empty") == 0) {
            std::cerr << "参数已移除: " << argv[i] << "\n";
            return 1;
        }
        if (std::strcmp(argv[i], "--config") == 0) {
            if (!hasArgValue(i)) {
                std::cout << configPath << "\n";
                return 0;
            }
            ++i; // 已在预扫描阶段加载配置；这里仅消费参数
            continue;
        }
        if (std::strcmp(argv[i], "--dict") == 0) {
            if (!hasArgValue(i)) {
                std::cout << cfg.dictPath << "\n";
                return 0;
            }
            cfg.dictPath = argv[++i];
            continue;
        }
        if (std::strcmp(argv[i], "--user-dict") == 0) {
            if (!hasArgValue(i)) {
                std::cout << cfg.userDictPath << "\n";
                return 0;
            }
            cfg.userDictPath = argv[++i];
            continue;
        }
        if (std::strcmp(argv[i], "--toggle-key") == 0) {
            if (!hasArgValue(i)) {
                std::cout << keySpecToString(cfg.toggleKey) << "\n";
                return 0;
            }
            int k = 0;
            if (!parseKeySpec(argv[++i], &k)) {
                std::cerr << "无效 --toggle-key\n";
                return 1;
            }
            cfg.toggleKey = k;
            continue;
        }
        if (std::strcmp(argv[i], "--exit-key") == 0) {
            if (!hasArgValue(i)) {
                std::cout << keySpecToString(cfg.exitKey) << "\n";
                return 0;
            }
            int k = 0;
            if (!parseKeySpec(argv[++i], &k)) {
                std::cerr << "无效 --exit-key\n";
                return 1;
            }
            cfg.exitKey = k;
            continue;
        }
        if (std::strcmp(argv[i], "--log-file") == 0) {
            if (!hasArgValue(i)) {
                std::cout << cfg.logFilePath << "\n";
                return 0;
            }
            cfg.logFilePath = argv[++i];
            continue;
        }
        if (std::strcmp(argv[i], "-s") == 0 || std::strcmp(argv[i], "--startup-cmd") == 0) {
            if (!hasArgValue(i)) {
                std::cout << startupCommand << "\n";
                return 0;
            }
            startupCommand = argv[++i];
            continue;
        }
        std::cerr << "未知参数: " << argv[i] << "\n";
        printUsage(argv[0]);
        return 1;
    }

    Logger logger(cfg.logFilePath);
    logger.info("start version=" + std::string(kVersion));
    const unsigned hwThreads = std::thread::hardware_concurrency();
    const bool useParallelLoad = hwThreads > 1;
    logger.info(std::string("hardware_concurrency=") + std::to_string(hwThreads));
    if (!ensureParentDirAndFile(cfg.userDictPath)) {
        if (cfg.userDictPath == kUserDictPath) {
            cfg.userDictPath = defaultWritableUserDictPath();
            std::cerr << "[lite-tty-ime] " << kBundledDataDir
                      << " 不可写，用户词库与词频改用: " << cfg.userDictPath << "\n";
        }
        if (!ensureParentDirAndFile(cfg.userDictPath)) {
            std::cerr << "无法创建用户词库文件: " << cfg.userDictPath << "\n";
            return 1;
        }
    }
    const std::string freqPath = cfg.userDictPath + ".freq";
    if (!ensureParentDirAndFile(freqPath)) {
        std::cerr << "无法创建词频文件: " << freqPath << "\n";
        return 1;
    }
    logger.info("user_dict path=" + cfg.userDictPath);

    // ---------------------------
    // 42-2) 词典与索引缓存
    // ---------------------------
    std::unordered_map<std::string, std::vector<std::string>> dict;
    std::unordered_map<std::string, std::unordered_set<std::string>> userSet;
    std::unordered_map<std::string, CandidateStat> freqStats;
    std::unordered_map<std::string, std::vector<std::string>> commaKeyByFirst;
    std::unordered_map<std::string, std::vector<std::string>> commaKeyByFirstPrefix;
    std::unordered_set<std::string> mapSyllables;
    size_t maxMapSyllableLen = 1;
    const std::string learnedPhrasePath =
        (std::filesystem::path(cfg.userDictPath).parent_path() / "learned_phrases.txt").string();
    std::unordered_map<std::string, std::vector<std::string>> learnedByInitials;
    // key: 去掉逗号后的纯拼音（如 jintian），value: (fullKey, phrase)
    std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> learnedByJoinedFull;
    std::unordered_set<std::string> knownPhrasesForLearn;

    // 完整拼音键 -> 首字母串（如 jin,tian -> jt）
    auto makeInitialsFromFullKey = [&](const std::string &fullKey) {
        std::string out;
        for (const auto &seg : splitComma(fullKey)) {
            if (!seg.empty()) {
                out.push_back(seg[0]);
            }
        }
        return out;
    };
    auto makeJoinedFromFullKey = [&](const std::string &fullKey) {
        // 按用户要求：直接去掉逗号得到"纯拼音"，再与输入做字符串精确对照。
        std::string out;
        out.reserve(fullKey.size());
        for (unsigned char c : fullKey) {
            if (c == ',') continue;
            if (c < 128u && std::isalpha(c)) {
                out.push_back(static_cast<char>(std::tolower(c)));
            } else if (c < 128u && std::isspace(c)) {
                continue;
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
        return out;
    };
    auto loadMapSyllables = [&]() {
        // 从基础词库收集"合法音节集合"，供连续输入分词校验使用。
        mapSyllables.clear();
        maxMapSyllableLen = 1;
        std::ifstream in(cfg.dictPath);
        if (!in) return;
        std::string line;
        while (std::getline(in, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;
            const size_t p = line.find(' ');
            if (p == std::string::npos) continue;
            std::string py = toLowerCopy(trim(line.substr(0, p)));
            if (py.empty()) continue;
            bool ok = true;
            for (unsigned char c : py) {
                if (!(c < 128u && std::isalpha(c))) {
                    ok = false;
                    break;
                }
            }
            if (ok) {
                mapSyllables.insert(py);
                if (py.size() > maxMapSyllableLen) {
                    maxMapSyllableLen = py.size();
                }
            }
        }
    };
    auto rebuildLearnedIndex = [&]() {
        // learned_phrases.txt 只用于"首字母短语回忆"索引，不直接替代主词典。
        learnedByInitials.clear();
        learnedByJoinedFull.clear();
        std::unordered_set<std::string> seen;
        std::unordered_set<std::string> seenFull;
        std::ifstream in(learnedPhrasePath);
        if (!in) return;
        std::string line;
        while (std::getline(in, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;
            const size_t p = line.find(' ');
            if (p == std::string::npos) continue;
            const std::string fullKey = trim(line.substr(0, p));
            const std::string phrase = trim(line.substr(p + 1));
            if (fullKey.empty() || phrase.empty()) continue;
            const std::string initials = makeInitialsFromFullKey(fullKey);
            if (initials.empty()) continue;
            const std::string uniq = initials + "\t" + phrase;
            if (seen.insert(uniq).second) {
                learnedByInitials[initials].push_back(phrase);
            }
            const std::string joined = makeJoinedFromFullKey(fullKey);
            if (!joined.empty()) {
                const std::string uniqFull = joined + "\t" + fullKey + "\t" + phrase;
                if (seenFull.insert(uniqFull).second) {
                    learnedByJoinedFull[joined].push_back(std::make_pair(fullKey, phrase));
                }
            }
        }
    };
    auto rebuildAllIndexes = [&]() {
        // 全量重建：在启动和热重载后调用，保证索引与词库一致。
        rebuildCommaKeyIndex(dict, commaKeyByFirst, commaKeyByFirstPrefix);
        knownPhrasesForLearn.clear();
        for (const auto &kv : dict) {
            const std::string &k = kv.first;
            if (k.empty()) {
                continue;
            }
            for (const auto &w : kv.second) {
                // 仅记录 >=2 字词，作为"是否应写入短语库"的子串命中来源。
                if (splitUtf8Chars(w).size() >= 2) {
                    knownPhrasesForLearn.insert(w);
                }
            }
        }
        loadMapSyllables();
        rebuildLearnedIndex();
    };
    ensureParentDirAndFile(learnedPhrasePath);
    reloadAllDicts(cfg, dict, userSet, useParallelLoad);
    rebuildAllIndexes();
    loadFreqStats(freqPath, freqStats);
    time_t baseMtime = 0;
    time_t userMtime = 0;
    time_t freqMtime = 0;
    time_t learnedMtime = 0;
    getFileMtimeSec(cfg.dictPath, &baseMtime);
    getFileMtimeSec(cfg.userDictPath, &userMtime);
    getFileMtimeSec(freqPath, &freqMtime);
    getFileMtimeSec(learnedPhrasePath, &learnedMtime);
    if (dict.empty()) {
        std::cerr << "词库为空: 请准备 " << cfg.dictPath << " 或 " << cfg.userDictPath << "\n";
        return 1;
    }

    int masterFd = -1;
#if !defined(_WIN32)
    pid_t childPid = -1;
    if (shellMode) {
        if (!setupShellPty(&masterFd, &childPid)) {
            return 1;
        }
        g_masterFdAtExit = masterFd;
        std::atexit(restoreTerminalAfterIme);
        int fl = fcntl(masterFd, F_GETFL, 0);
        if (fl >= 0) {
            fcntl(masterFd, F_SETFL, fl | O_NONBLOCK);
        }
    }
#endif

    RawModeGuard guard;
    if (!guard.ok()) {
        std::cerr << "无法切换终端到原始模式\n";
        return 1;
    }

    // 进入输入法后先清屏并将光标归位，避免历史输出残留影响操作。
    std::cout << "\033[2J\033[H" << std::flush;

    // ---------------------------
    // 42-3) 运行态状态变量
    // ---------------------------
    ImeRuntimeState st;
    /** 切换中英后闪现 [中]/[EN]；任意非开关键按下后清除 */
    bool &imeOn = st.imeOn;
    bool &imeModeHintActive = st.imeModeHintActive;
    bool &imeModeHintIsZh = st.imeModeHintIsZh;
    std::string &composed = st.composed;
    std::string &input = st.input;
    // 拼音输入框内的隐形光标位置（0..input.size()）
    size_t &inputCursor = st.inputCursor;
    std::vector<std::string> &cands = st.cands;
    std::vector<std::string> &candPyKeys = st.candPyKeys;
    std::vector<size_t> &candConsumeLens = st.candConsumeLens;
    std::vector<bool> &candFromFallback = st.candFromFallback;
    size_t &candPageStart = st.candPageStart;
    std::vector<std::string> &pinyinSegments = st.pinyinSegments;
    std::string &addWordPhrase = st.addWordPhrase;
    std::vector<std::string> &addWordCommittedPhrases = st.addWordCommittedPhrases;
    /** 普通中文态下每次选词累积：仅当用过连续候选时，才允许自动学习 */
    std::vector<std::string> &autoAddPinyinSegs = st.autoAddPinyinSegs;
    std::string &autoAddPhraseAcc = st.autoAddPhraseAcc;
    bool &autoAddUsedFallback = st.autoAddUsedFallback;
    // 调试型输入逻辑：仅展示"处理结果"，不走传统/连续候选生成与选词提交。
    const bool simpleDisplayMode = true;
    std::string &simplePendingPhrase = st.simplePendingPhrase;
    std::string &simplePendingPyKey = st.simplePendingPyKey;
    std::vector<std::string> &simpleConSelectedWords = st.simpleConSelectedWords;
    std::vector<std::string> &simpleConSelectedPyKeys = st.simpleConSelectedPyKeys;
    bool pendingBottomBarRedraw = false;
    gSimpleImeDisplayMode = simpleDisplayMode;
    gSimpleImeStatusText.clear();

    auto clearCandidateState = [&]() {
        cands.clear();
        candPyKeys.clear();
        candConsumeLens.clear();
        candFromFallback.clear();
        candPageStart = 0;
    };
    auto clearPreeditState = [&]() {
        input.clear();
        inputCursor = 0;
        clearCandidateState();
    };

    redrawLine(shellMode, imeOn, addWordMode, composed, input, inputCursor, cands, candFromFallback, candPageStart, addWordPhrase, cfg,
               imeModeHintActive, imeModeHintIsZh);

    // ---------------------------
    // 42-4) 学习函数
    // 描述: 学习新短语到 learned_phrases.txt
    // ---------------------------
    auto learnFullPhrase = [&](const std::string &pyKey, const std::string &phrase) {
        // 学习入库前置约束：
        // 1) 至少 2 字；2) pyKey 每段必须是完整合法音节（拒绝首字母串）。
        const auto chars = splitUtf8Chars(phrase);
        if (chars.size() < 2) {
            return;
        }
        const auto segs = splitComma(pyKey);
        if (segs.empty()) {
            return;
        }
        for (const auto &seg : segs) {
            if (mapSyllables.find(toLowerCopy(seg)) == mapSyllables.end()) {
                return; // 非完整拼音（可能是首字母）不学习到短语缩写库
            }
        }
        if (!insertUserDictSortedAtomic(learnedPhrasePath, pyKey, phrase)) {
            if (cfg.debugMode) {
                std::cout << "\r\033[2K[DEBUG] 学习短语写入失败: " << learnedPhrasePath
                          << " | " << pyKey << " -> " << phrase << "\n";
            }
            return;
        }
        if (cfg.showStoreHint) {
            std::cout << "\r\033[2K[存储] 短语: " << pyKey << " -> " << phrase << "\n";
        }
        const std::string initials = makeInitialsFromFullKey(pyKey);
        if (initials.empty()) {
            return;
        }
        auto &bucket = learnedByInitials[initials];
        if (std::find(bucket.begin(), bucket.end(), phrase) == bucket.end()) {
            bucket.push_back(phrase);
        }
    };
    // ---------------------------
    // 42-5) 候选刷新函数
    // 描述: 根据当前输入重新计算候选词
    // ---------------------------
    auto refreshCandidates = [&]() {
        if (simpleDisplayMode) {
            cands.clear();
            candPyKeys.clear();
            candConsumeLens.clear();
            candFromFallback.clear();
            candPageStart = 0;
            simplePendingPhrase.clear();
            simplePendingPyKey.clear();
            if (input.empty()) {
                gSimpleImeStatusText.clear();
                simpleConSelectedWords.clear();
                simpleConSelectedPyKeys.clear();
                return;
            }
            if (input.size() == 1 && (input[0] == 'i' || input[0] == 'u' || input[0] == 'v')) {
                // 单独 i/u/v：不出候选，保留原输入；space 可按 CommitRaw 直接上屏。
                gSimpleImeStatusText = input;
                return;
            }

            // 1) 先查 learned：fullKey 去逗号后与 input 精确比对，命中则显示短语。
            const auto litFull = learnedByJoinedFull.find(input);
            if (litFull != learnedByJoinedFull.end()) {
                std::unordered_set<std::string> seen;
                for (const auto &e : litFull->second) {
                    if (!seen.insert(e.second).second) continue;
                    cands.push_back(e.second);
                    candPyKeys.push_back(e.first);
                    candConsumeLens.push_back(input.size());
                    candFromFallback.push_back(false);
                }
            }

            // 2) 构造 [merge,rad,cons] 逻辑/展示串（已抽离到 query_forms 模块）
            const PinyinQueryForms forms = buildPinyinQueryForms(input, mapSyllables);
            const std::vector<std::string> &queryForms = forms.logicForms;
            const std::vector<std::string> &queryDisplayForms = forms.displayForms;

            // 3) 连续输入：由 get_first_con_input 给出首段候选拼音，再取可选词并显示
            const std::string firstForm = queryForms.empty() ? forms.mergeLogic : queryForms.front();
            const std::string firstDisplayForm =
                queryDisplayForms.empty() ? forms.mergeDisplay : queryDisplayForms.front();
            // 顶栏拼音显示与候选来源保持一致（展示串保留自动','与手动''）
            gSimpleImeStatusText = firstDisplayForm.empty() ? input : firstDisplayForm;
            if (cfg.showConStats) {
                const auto cnt = countSelectedCharsAndWords(simpleConSelectedWords);
                gSimpleImeStatusText += " ";
                gSimpleImeStatusText += std::to_string(cnt.first);
                gSimpleImeStatusText += ",";
                gSimpleImeStatusText += std::to_string(cnt.second);
            }
            const auto firstWords = get_first_con_input(firstForm, dict);
            if (!firstWords.empty()) {
                std::vector<std::string> mergeChunks;
                for (const auto &s : splitBySep(firstForm, '\'')) {
                    if (!s.empty()) mergeChunks.push_back(s);
                }
                for (const auto &fw : firstWords) {
                    // fw 对应 merge 前缀若干 chunk_char，推导 take 段数
                    size_t take = 0;
                    std::string acc;
                    for (size_t i = 0; i < mergeChunks.size(); ++i) {
                        acc += mergeChunks[i];
                        if (acc == fw) {
                            take = i + 1;
                            break;
                        }
                    }
                    if (take == 0) continue;
                    std::vector<std::string> chunkSegs(mergeChunks.begin(), mergeChunks.begin() + static_cast<std::ptrdiff_t>(take));
                    const auto chunkCands = get_candidates_for_chunk(chunkSegs, dict, freqStats);
                    if (chunkCands.empty()) continue;
                    size_t consumeLetters = 0;
                    for (size_t i = 0; i < take; ++i) consumeLetters += mergeChunks[i].size();
                    for (const auto &cp : chunkCands) {
                        cands.push_back(cp.first);
                        candPyKeys.push_back(cp.second);
                        candConsumeLens.push_back(consumeLetters);
                        candFromFallback.push_back(false);
                    }
                }
            }
            // 4) 在候选顶部补 1 条“整串拼接”的最佳长候选（若可构造）
            // 仅使用当前最终采用的拆分（firstForm），避免激进/保守分支污染首候选。
            {
                const std::vector<std::string> segmentedInputs = {firstForm};
                const auto stitched = get_candidate_words(segmentedInputs, dict, freqStats);
                if (!stitched.empty()) {
                    const std::string &bestPhrase = stitched.front();
                    auto it = std::find(cands.begin(), cands.end(), bestPhrase);
                    if (it != cands.end()) {
                        const auto idx = static_cast<size_t>(std::distance(cands.begin(), it));
                        cands.erase(cands.begin() + static_cast<std::ptrdiff_t>(idx));
                        if (idx < candPyKeys.size()) candPyKeys.erase(candPyKeys.begin() + static_cast<std::ptrdiff_t>(idx));
                        if (idx < candConsumeLens.size()) candConsumeLens.erase(candConsumeLens.begin() + static_cast<std::ptrdiff_t>(idx));
                        if (idx < candFromFallback.size()) candFromFallback.erase(candFromFallback.begin() + static_cast<std::ptrdiff_t>(idx));
                    }
                    cands.insert(cands.begin(), bestPhrase);
                    candPyKeys.insert(candPyKeys.begin(), input);
                    candConsumeLens.insert(candConsumeLens.begin(), input.size());
                    candFromFallback.insert(candFromFallback.begin(), false);
                }
            }
            return;
        }

        // 旧传统/连续候选链路已下线，仅保留 simpleDisplayMode 分支。
        cands.clear();
        candPyKeys.clear();
        candConsumeLens.clear();
        candFromFallback.clear();
        candPageStart = 0;
    };

    // ---------------------------
    // 42-6) 热重载函数
    // 描述: 检测词库文件变更并重新加载
    // ---------------------------
    auto maybeHotReloadDict = [&]() {
        time_t nb = 0;
        time_t nu = 0;
        time_t nf = 0;
        time_t nl = 0;
        bool changed = false;
        if (getFileMtimeSec(cfg.dictPath, &nb) && nb != baseMtime) {
            baseMtime = nb;
            changed = true;
        }
        if (getFileMtimeSec(cfg.userDictPath, &nu) && nu != userMtime) {
            userMtime = nu;
            changed = true;
        }
        if (getFileMtimeSec(freqPath, &nf) && nf != freqMtime) {
            freqMtime = nf;
            changed = true;
        }
        if (getFileMtimeSec(learnedPhrasePath, &nl) && nl != learnedMtime) {
            learnedMtime = nl;
            changed = true;
        }
        if (changed) {
            reloadAllDicts(cfg, dict, userSet, useParallelLoad);
            rebuildAllIndexes();
            loadFreqStats(freqPath, freqStats);
            refreshCandidates();
            logger.info("hot reload dictionary");
            redrawLine(shellMode, imeOn, addWordMode, composed, input, inputCursor, cands, candFromFallback, candPageStart,
                       addWordPhrase, cfg, imeModeHintActive, imeModeHintIsZh);
        }
    };

    // ---------------------------
    // 42-7) 字节写入函数
    // 描述: 向PTY或标准输出写入字节
    // ---------------------------
#if !defined(_WIN32)
    auto appendBytes = [&](const char *p, size_t n) {
        if (n == 0) {
            return;
        }
        composed.append(p, n);
        if (shellMode && masterFd >= 0) {
            writeRawFd(masterFd, p, n);
        } else {
            std::cout.write(p, n);
            std::cout.flush();
        }
    };
#else
    auto appendBytes = [&](const char *p, size_t n) {
        if (n == 0) {
            return;
        }
        composed.append(p, n);
        std::cout.write(p, n);
        std::cout.flush();
    };
#endif

    // ---------------------------
    // 42-8) 简单显示短语提交
    // 描述: 提交简单显示模式下的待上屏短语
    // ---------------------------
    auto maybeCommitSimplePhrase = [&]() {
        if (!simpleDisplayMode || simplePendingPhrase.empty()) {
            return;
        }
        appendBytes(simplePendingPhrase.data(), simplePendingPhrase.size());
        if (!simplePendingPyKey.empty()) {
            CandidateStat &st = freqStats[makeFreqKey(simplePendingPyKey, simplePendingPhrase)];
            st.count += 1;
            st.lastTs = std::time(nullptr);
            saveFreqStatsAtomic(freqPath, freqStats);
            getFileMtimeSec(freqPath, &freqMtime);
        }
        clearPreeditState();
        simplePendingPhrase.clear();
        simplePendingPyKey.clear();
        simpleConSelectedWords.clear();
        simpleConSelectedPyKeys.clear();
        gSimpleImeStatusText.clear();
    };

    // ---------------------------
    // 42-9) 连续输入落库判定
    // 描述: 判断连续输入应该存为短语还是词语
    // ---------------------------
    // 连续输入落库判定规则已抽离到 learn_rules 模块。

    // ---------------------------
    // 42-10) 候选提交函数
    // 描述: 提交选中的候选词
    // ---------------------------
    auto commitCandidate = [&](int index) {
        // 提交候选：输出文本、更新词频、按 consume 长度推进输入缓冲。
        const size_t actual = candPageStart + static_cast<size_t>(index);
        if (actual >= cands.size()) {
            return;
        }
        const std::string selectedPhrase = cands[actual];
        const std::string selectedPy =
            (actual < candPyKeys.size() && !candPyKeys[actual].empty()) ? candPyKeys[actual] : input;
        const size_t consumeLen =
            (actual < candConsumeLens.size() && candConsumeLens[actual] > 0) ? candConsumeLens[actual] : input.size();
        if (addWordMode) {
            pinyinSegments.push_back(selectedPy);
            addWordPhrase += selectedPhrase;
            addWordCommittedPhrases.push_back(selectedPhrase);
        } else if (imeOn) {
            if (simpleDisplayMode && !selectedPy.empty()) {
                simpleConSelectedWords.push_back(selectedPhrase);
                simpleConSelectedPyKeys.push_back(selectedPy);
            }
            autoAddPinyinSegs.push_back(selectedPy);
            autoAddPhraseAcc += selectedPhrase;
            if (actual < candFromFallback.size() && candFromFallback[actual]) {
                // 本轮确实使用了连续输入候选，后续才允许触发自动学习。
                autoAddUsedFallback = true;
            }
        }
        appendBytes(selectedPhrase.data(), selectedPhrase.size());
        if (!selectedPy.empty()) {
            CandidateStat &st = freqStats[makeFreqKey(selectedPy, selectedPhrase)];
            st.count += 1;
            st.lastTs = std::time(nullptr);
            saveFreqStatsAtomic(freqPath, freqStats);
            getFileMtimeSec(freqPath, &freqMtime);
        }
        if (consumeLen >= input.size()) {
            clearPreeditState();
            gSimpleImeStatusText.clear(); // 拼音耗尽后清空状态文本
            if (simpleDisplayMode && !addWordMode && !simpleConSelectedWords.empty()) {
                std::string fullPhrase;
                for (const auto &w : simpleConSelectedWords) fullPhrase += w;
                std::string fullPyKey;
                for (size_t i = 0; i < simpleConSelectedPyKeys.size(); ++i) {
                    if (i) fullPyKey += ",";
                    fullPyKey += simpleConSelectedPyKeys[i];
                }
                const auto chars = splitUtf8Chars(fullPhrase);
                if (!fullPyKey.empty() && chars.size() >= 2) {
                    const bool toLearned = shouldStoreContinuousAsPhrase(simpleConSelectedWords);
                    if (toLearned) {
                        learnFullPhrase(fullPyKey, fullPhrase);
                        getFileMtimeSec(learnedPhrasePath, &learnedMtime);
                        logger.info("simple-con store learned " + fullPyKey + " -> " + fullPhrase);
                        if (cfg.showStoreHint) {
                            std::cout << "\r\033[2K[存储] 短语: " << fullPyKey << " -> " << fullPhrase << "\n";
                        }
                    } else {
                        if (insertUserDictSortedAtomic(cfg.userDictPath, fullPyKey, fullPhrase)) {
                            mergeUniqueCandidate(dict, fullPyKey, fullPhrase);
                            userSet[fullPyKey].insert(fullPhrase);
                            knownPhrasesForLearn.insert(fullPhrase);
                            rebuildAllIndexes();
                            getFileMtimeSec(cfg.userDictPath, &userMtime);
                            logger.info("simple-con store user " + fullPyKey + " -> " + fullPhrase);
                            if (cfg.showStoreHint) {
                                std::cout << "\r\033[2K[存储] 词语: " << fullPyKey << " -> " << fullPhrase << "\n";
                            }
                        }
                    }
                }
                simpleConSelectedWords.clear();
                simpleConSelectedPyKeys.clear();
            }
        } else {
            input.erase(0, consumeLen);
            // 连续输入选词后仍有剩余拼音时，光标应落在末尾，便于继续退格/编辑
            inputCursor = input.size();
            refreshCandidates();
        }
    };

    // ---------------------------
    // 42-11) ESC序列处理状态
    // 描述: 处理方向键等转义序列
    // ---------------------------
    // 0: normal, 1: seen ESC, 6: CSI 收集中(\e[...末字节 0x40–0x7E（含 \e[1;2A、\e[A 等）), 7: SS3 等末字节(\eO + A/B/C/D)
    int imeEscState = 0;
    std::string escSeqBuf;

    // ---------------------------
    // 42-12) 行结束处理
    // 描述: 发送换行并清理输入状态
    // ---------------------------
    auto sendLineEnd = [&](char lineEndCh) {
        // 行结束：向 shell 发送换行并清理当前输入态。
#if !defined(_WIN32)
        if (shellMode && masterFd >= 0) {
            // 保留原始换行字节：Enter/ Ctrl+J 分别透传为 \r / \n，避免 TUI 快捷键错乱。
            writeRawFd(masterFd, &lineEndCh, 1);
            // 不在此处用 DSR(\e[6n) 查光标：会从 STDIN 读光标报告，
            // 在 nano 等 TUI 里会抢走按键、打乱序列，导致回车异常或乱提示。
        } else
#endif
        {
            std::cout.put('\n');
            std::cout.flush();
        }
        composed.clear();
        clearPreeditState();
        pinyinSegments.clear();
        addWordPhrase.clear();
        addWordCommittedPhrases.clear();
        autoAddPinyinSegs.clear();
        autoAddPhraseAcc.clear();
        autoAddUsedFallback = false;
        simplePendingPhrase.clear();
        simplePendingPyKey.clear();
        gSimpleImeStatusText.clear();
        imeEscState = 0;
        escSeqBuf.clear();
        redrawLine(shellMode, imeOn, addWordMode, composed, input, inputCursor, cands, candFromFallback, candPageStart, addWordPhrase, cfg, imeModeHintActive, imeModeHintIsZh);
    };

    // ---------------------------
    // 42-13) 自动学习函数
    // 描述: 普通中文态下自动学习新词
    // ---------------------------
    // 普通中文态自动学习（静默）：
    // 触发方负责保证"拼音已全部消耗"，这里仅做资格与落盘分流判定。
    auto tryAutoAddPhraseSilent = [&]() {
        if (addWordMode) {
            return;
        }
        if (!autoAddUsedFallback) {
            return; // 纯传统模式不自动存
        }
        std::string pyKey;
        for (size_t si = 0; si < autoAddPinyinSegs.size(); ++si) {
            if (si) {
                pyKey += ',';
            }
            pyKey += autoAddPinyinSegs[si];
        }
        const auto phU = splitUtf8Chars(autoAddPhraseAcc);
        if (!pyKey.empty() && phU.size() >= 2) {
            if (shouldStoreToLearnedByPhrase(autoAddPhraseAcc, knownPhrasesForLearn)) {
                // 命中已有词子串 -> 写短语库
                learnFullPhrase(pyKey, autoAddPhraseAcc);
                getFileMtimeSec(learnedPhrasePath, &learnedMtime);
                logger.info("auto-learned-phrase " + pyKey + " -> " + autoAddPhraseAcc);
                if (cfg.showStoreHint) {
                    std::cout << "\r\033[2K[存储] 短语: " << pyKey << " -> "
                              << autoAddPhraseAcc << "\n";
                }
            } else if (insertUserDictSortedAtomic(cfg.userDictPath, pyKey, autoAddPhraseAcc)) {
                // 否则写用户词库（并即时并入内存索引）
                mergeUniqueCandidate(dict, pyKey, autoAddPhraseAcc);
                userSet[pyKey].insert(autoAddPhraseAcc);
                knownPhrasesForLearn.insert(autoAddPhraseAcc);
                rebuildAllIndexes();
                getFileMtimeSec(cfg.userDictPath, &userMtime);
                logger.info("auto-add-on-fallback " + pyKey + " -> " + autoAddPhraseAcc);
                if (cfg.showStoreHint) {
                    std::cout << "\r\033[2K[存储] 词语: " << pyKey << " -> "
                              << autoAddPhraseAcc << "\n";
                }
            } else {
                logger.warn("auto-add-on-fallback write failed path=" + cfg.userDictPath);
                if (cfg.debugMode) {
                    std::cout << "\r\033[2K[DEBUG] 自动加词写入失败: " << cfg.userDictPath
                              << " | " << pyKey << " -> " << autoAddPhraseAcc << "\n";
                }
            }
        }
    };

    // ---------------------------
    // 42-14) 自动学习状态重置
    // 描述: 清空自动学习累积
    // ---------------------------
    auto resetAutoAddState = [&]() {
        autoAddPinyinSegs.clear();
        autoAddPhraseAcc.clear();
        autoAddUsedFallback = false;
    };

    auto redrawCurrentLine = [&]() {
        redrawLine(shellMode, imeOn, addWordMode, composed, input, inputCursor, cands, candFromFallback, candPageStart,
                   addWordPhrase, cfg, imeModeHintActive, imeModeHintIsZh);
    };

    auto applyImeAction = [&](KeyAction action, char lineEndCh) -> bool {
        return input_handlers::applyImeAction(
            action,
            lineEndCh,
            st,
            commitCandidate,
            tryAutoAddPhraseSilent,
            resetAutoAddState,
            clearPreeditState,
            sendLineEnd,
            appendBytes,
            redrawCurrentLine);
    };

    auto handleCandidatePaging = [&](char ch) -> bool {
        return input_handlers::handleCandidatePaging(ch, cfg.candidatePageSize, st, redrawCurrentLine);
    };

    auto handleDigitKey = [&](char ch) -> bool {
        auto writeByteToShell = [&](char b) {
            if (masterFd >= 0) {
                writeRawFd(masterFd, &b, 1);
            }
        };
        return input_handlers::handleDigitKey(
            ch,
            cfg.candidatePageSize,
            st,
            shellMode,
            writeByteToShell,
            commitCandidate,
            tryAutoAddPhraseSilent,
            resetAutoAddState,
            redrawCurrentLine);
    };

    auto handleFullWidthPunctuation = [&](char ch) -> bool {
        return input_handlers::handleFullWidthPunctuation(ch, st, appendBytes, redrawCurrentLine);
    };

    auto handleManualSeparator = [&](char ch) -> bool {
        return input_handlers::handleManualSeparator(ch, st, refreshCandidates, maybeCommitSimplePhrase, redrawCurrentLine);
    };

    auto handleAlphabetInput = [&](char ch) -> bool {
        return input_handlers::handleAlphabetInput(ch, st, refreshCandidates, maybeCommitSimplePhrase, redrawCurrentLine);
    };

    auto handleEnglishModeKey = [&](char ch) -> bool {
        auto popLastUtf8CharInPlace = [&](std::string &s) { popLastUtf8Char(s); };
        auto sendBackspaceToShell = [&]() {
#if !defined(_WIN32)
            if (masterFd >= 0) {
                writeRawFd(masterFd, "\x7f", 1);
            }
#endif
        };
        return input_handlers::handleEnglishModeKey(
            ch,
            st,
            shellMode,
            imeEscState,
            escSeqBuf,
            sendLineEnd,
            redrawCurrentLine,
            appendBytes,
            popLastUtf8CharInPlace,
            sendBackspaceToShell);
    };

    auto maybeForwardUnhandledCtrlToShell = [&](char ch) -> bool {
        auto writeByteToShell = [&](char b) {
#if !defined(_WIN32)
            if (masterFd >= 0) {
                writeRawFd(masterFd, &b, 1);
            }
#endif
        };
        return input_handlers::maybeForwardUnhandledCtrlToShell(ch, shellMode, writeByteToShell);
    };

    auto handleBackspaceKey = [&](char ch) -> bool {
        auto popLastUtf8CharInPlace = [&](std::string &s) { popLastUtf8Char(s); };
        auto sendBackspaceToShell = [&]() {
#if !defined(_WIN32)
            if (masterFd >= 0) {
                writeRawFd(masterFd, "\x7f", 1);
            }
#endif
        };
        return input_handlers::handleBackspaceKey(
            ch,
            st,
            addWordMode,
            shellMode,
            resetAutoAddState,
            refreshCandidates,
            maybeCommitSimplePhrase,
            redrawCurrentLine,
            popLastUtf8CharInPlace,
            sendBackspaceToShell);
    };

    auto handleAddWordEnter = [&](char ch) -> bool {
        auto storeAddWord = [&](const std::string &pyKey, const std::string &phrase) -> bool {
            if (!insertUserDictSortedAtomic(cfg.userDictPath, pyKey, phrase)) {
                return false;
            }
            mergeUniqueCandidate(dict, pyKey, phrase);
            userSet[pyKey].insert(phrase);
            learnFullPhrase(pyKey, phrase);
            rebuildAllIndexes();
            getFileMtimeSec(cfg.userDictPath, &userMtime);
            getFileMtimeSec(learnedPhrasePath, &learnedMtime);
            logger.info("add-word " + pyKey + " -> " + phrase);
            std::cout << "\r\033[2K已加入: " << pyKey << " -> " << phrase << "\n";
            return true;
        };
        auto onEmptyKey = [&]() {
            std::cout << "\r\033[2K[未写入] 拼音键为空\n";
        };
        auto onStoreFailed = [&]() {
            logger.warn("add-word write failed path=" + cfg.userDictPath);
            std::cout << "\r\033[2K[写入失败] " << cfg.userDictPath << "\n";
        };
        return input_handlers::handleAddWordEnter(
            ch,
            true,
            addWordMode,
            st,
            storeAddWord,
            onEmptyKey,
            onStoreFailed,
            redrawCurrentLine);
    };

    // ---------------------------
    // 42-15) 按键主处理函数
    // 描述: 处理所有按键事件的核心状态机
    // ---------------------------
    auto processChar = [&](char ch, bool *exitLoop) -> bool {
        // ---------------------------
        // 42-15-1) 退出键检测
        // ---------------------------
        *exitLoop = false;
        if (inputCursor > input.size()) {
            inputCursor = input.size();
        }

        if (cfg.exitKey >= 0 &&
            static_cast<unsigned char>(ch) == static_cast<unsigned char>(cfg.exitKey)) {
            *exitLoop = true;
            return true;
        }
        // 42-15-2) 状态栏常驻显示当前中英文状态，不再做闪现自动清除。

        // ---------------------------
        // 42-15-3) 中英切换
        // 描述: 切换输入法开关
        // ---------------------------
        if (static_cast<unsigned char>(ch) == static_cast<unsigned char>(cfg.toggleKey)) {
            // 切换中英时不再触发学习；仅清空自动学习缓冲，避免跨会话污染。
            imeOn = !imeOn;
            imeEscState = 0;
            escSeqBuf.clear();
            resetAutoAddState();
            simplePendingPhrase.clear();
            simplePendingPyKey.clear();
            simpleConSelectedWords.clear();
            simpleConSelectedPyKeys.clear();
            gSimpleImeStatusText.clear();
            if (!imeOn) {
                clearPreeditState();
            }
            imeModeHintActive = true;
            imeModeHintIsZh = imeOn;
            redrawLine(shellMode, imeOn, addWordMode, composed, input, inputCursor, cands, candFromFallback, candPageStart,
                        addWordPhrase, cfg, imeModeHintActive, imeModeHintIsZh);
            return true;
        }

        // ---------------------------
        // 42-15-4) 加词模式状态重置
        // ---------------------------
        if (addWordMode) {
            // 加词模式不使用普通中文态自动学习缓冲
            resetAutoAddState();
        }

        // ---------------------------
        // 42-15-5) 英文态处理
        // 描述: 中文输入法关闭时的按键处理
        // ---------------------------
        if (handleEnglishModeKey(ch)) {
            return true;
        }

        // ---------------------------
        // 42-15-6~8) ESC序列处理（已抽离解析器）
        // ---------------------------
        {
            const EscParseAction escAction = consumeEscSequenceChar(imeEscState, escSeqBuf, ch);
            if (escAction == EscParseAction::NeedMore) {
                return true;
            }
            if (escAction == EscParseAction::MoveCursorLeft || escAction == EscParseAction::MoveCursorRight) {
                if (imeOn) {
                    if (escAction == EscParseAction::MoveCursorLeft && inputCursor > 0) {
                        --inputCursor;
                    }
                    if (escAction == EscParseAction::MoveCursorRight && inputCursor < input.size()) {
                        ++inputCursor;
                    }
                    redrawLine(shellMode, imeOn, addWordMode, composed, input, inputCursor, cands, candFromFallback, candPageStart,
                               addWordPhrase, cfg, imeModeHintActive, imeModeHintIsZh);
                } else {
#if !defined(_WIN32)
                    if (shellMode && masterFd >= 0 && !escSeqBuf.empty()) {
                        writeRawFd(masterFd, escSeqBuf.data(), escSeqBuf.size());
                    }
#endif
                }
                escSeqBuf.clear();
                imeEscState = 0;
                return true;
            }
            if (escAction == EscParseAction::ForwardToShell) {
#if !defined(_WIN32)
                if (shellMode && masterFd >= 0 && !escSeqBuf.empty()) {
                    writeRawFd(masterFd, escSeqBuf.data(), escSeqBuf.size());
                }
#endif
                escSeqBuf.clear();
                imeEscState = 0;
                return true;
            }
            if (escAction == EscParseAction::ClearPreedit) {
                // 输入法不占用 Esc：将独立 Esc 透传给 shell，当前字符继续正常处理。
#if !defined(_WIN32)
                if (shellMode && masterFd >= 0) {
                    const char esc = 0x1B;
                    writeRawFd(masterFd, &esc, 1);
                }
#endif
            }
        }

        // ---------------------------
        // 42-15-9) ESC键检测
        // ---------------------------
        if (ch == 0x1B) {
            imeEscState = 1;
            escSeqBuf.clear();
            return true;
        }

        // ---------------------------
        // 42-15-10) 退格键处理
        // 描述: 回退拼音或已组成文本
        // ---------------------------
        if (handleBackspaceKey(ch)) {
            return true;
        }

        if (ch == ' ') {
            if (applyImeAction(cfg.spaceAction, ch)) {
                return true;
            }
        }
        if (ch == '\r' || ch == '\n') {
            if (applyImeAction(cfg.enterAction, ch)) {
                return true;
            }
        }

        // ---------------------------
        // 42-15-12) 加词模式回车
        // 描述: 在加词模式下写入用户词库
        // ---------------------------
        if (handleAddWordEnter(ch)) {
            return true;
        }

        // ---------------------------
        // 42-15-13) 回车换行
        // 描述: 无候选时回车直接换行
        // ---------------------------
        if ((ch == '\r' || ch == '\n') && input.empty() && cands.empty()) {
            sendLineEnd(ch);
            return true;
        }

        // ---------------------------
        // 42-15-14) 候选翻页
        // 描述: +/= 下一页，- 上一页
        // ---------------------------
        if (handleCandidatePaging(ch)) {
            return true;
        }

        // ---------------------------
        // 42-15-15) 数字键处理 - 选词/输入数字
        // 描述: 有候选时选词，无候选时输入数字
        // 注意: 这是用户要求修改的核心部分
        // ---------------------------
        if (handleDigitKey(ch)) {
            return true;
        }

        // ---------------------------
        // 42-15-16) 全角标点
        // 描述: 中文模式下输入全角标点符号
        // ---------------------------
        // 中文态保守全角标点：仅在无拼音/无候选时启用，避免影响选词与快捷键。
        if (handleFullWidthPunctuation(ch)) {
            return true;
        }

        // ---------------------------
        // 42-15-17) 手动分隔符
        // 描述: 输入'手动分隔拼音
        // ---------------------------
        if (handleManualSeparator(ch)) {
            return true;
        }

        // ---------------------------
        // 42-15-18) 拼音字母
        // 描述: 输入a-z字母作为拼音
        // ---------------------------
        if (handleAlphabetInput(ch)) {
            return true;
        }

        // ---------------------------
        // 42-15-19) 控制键透传
        // 描述: 未被接管的控制键原样传给shell
        // ---------------------------
        if (maybeForwardUnhandledCtrlToShell(ch)) {
            return true;
        }

        return true;
    };

    // ---------------------------
    // 42-16) 启动命令执行
    // ---------------------------
#if !defined(_WIN32)
    if (!startupCommand.empty()) {
        writeRawFd(masterFd, startupCommand.data(), startupCommand.size());
        writeRawFd(masterFd, "\n", 1);
        logger.info("startup command executed: " + startupCommand);
    }

    // ---------------------------
    // 42-17) Shell模式主循环
    // 描述: 使用select同时监听STDIN和PTY
    // ---------------------------
    unsigned short ptyRows = 0;
    unsigned short ptyCols = 0;
    syncPtyContentWinsizeIfNeeded(masterFd, &ptyRows, &ptyCols);
    applyBottomReservedScrollRegionIfNeeded(true);
    while (true) {
        int st = 0;
        if (waitpid(childPid, &st, WNOHANG) == childPid) {
#if !defined(_WIN32)
            restoreTerminalAfterIme();
            prepareCursorForExit(shellMode, imeOn);
#endif
            return 0;
        }
        // 窗口缩放后持续保持“内容区=总行数-1”。
        syncPtyContentWinsizeIfNeeded(masterFd, &ptyRows, &ptyCols);
        applyBottomReservedScrollRegionIfNeeded(false);
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        FD_SET(masterFd, &rfds);
        int maxfd = masterFd;
        if (STDIN_FILENO > maxfd) {
            maxfd = STDIN_FILENO;
        }
        timeval tv {};
        tv.tv_sec = 0;
        tv.tv_usec = 300000; // 300ms: 用于热加载轮询
        int r = select(maxfd + 1, &rfds, nullptr, nullptr, &tv);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (r == 0) {
            maybeHotReloadDict();
            if (pendingBottomBarRedraw) {
                redrawLine(shellMode, imeOn, addWordMode, composed, input, inputCursor, cands, candFromFallback, candPageStart,
                           addWordPhrase, cfg, imeModeHintActive, imeModeHintIsZh);
                pendingBottomBarRedraw = false;
            }
            continue;
        }

        if (FD_ISSET(masterFd, &rfds)) {
            char buf[4096];
            bool needBottomBarRedraw = false;
            bool needBottomReservedReapply = false;
            for (;;) {
                ssize_t n = read(masterFd, buf, sizeof buf);
                if (n > 0) {
                    std::cout.write(buf, static_cast<size_t>(n));
                    std::cout.flush();
                    if (outputRequestsBottomBarRedraw(buf, static_cast<size_t>(n))) {
                        needBottomBarRedraw = true;
                    }
                    if (outputMayBreakBottomReservedRegion(buf, static_cast<size_t>(n))) {
                        needBottomReservedReapply = true;
                    }
                    if (pendingBottomBarRedraw) {
                        needBottomBarRedraw = true;
                    }
                } else if (n == 0) {
#if !defined(_WIN32)
                    restoreTerminalAfterIme();
                    prepareCursorForExit(shellMode, imeOn);
#endif
                    return 0;
                } else {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        break;
                    }
                    break;
                }
            }
            if (needBottomReservedReapply) {
                applyBottomReservedScrollRegionIfNeeded(true);
                moveCursorToContentAreaLastRow();
                needBottomBarRedraw = true;
            }
            if (needBottomBarRedraw) {
                redrawLine(shellMode, imeOn, addWordMode, composed, input, inputCursor, cands, candFromFallback, candPageStart,
                           addWordPhrase, cfg, imeModeHintActive, imeModeHintIsZh);
                pendingBottomBarRedraw = false;
            }
        }

        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            char ch = 0;
            ssize_t n = read(STDIN_FILENO, &ch, 1);
            if (n <= 0) {
                break;
            }
            if (ch == 0x0c) { // Ctrl+L: 捕获 + 透传 + 标记重绘
                pendingBottomBarRedraw = true;
                writeRawFd(masterFd, &ch, 1);
                continue;
            }
            bool exitLoop = false;
            processChar(ch, &exitLoop);
            if (exitLoop) {
                logger.info("exit by key");
#if !defined(_WIN32)
                restoreTerminalAfterIme();
                prepareCursorForExit(shellMode, imeOn);
#endif
                terminateChildPtyProcess(childPid);
                return 0;
            }
        }
    }
#if !defined(_WIN32)
    restoreTerminalAfterIme();
    prepareCursorForExit(shellMode, imeOn);
#endif
    terminateChildPtyProcess(childPid);
    return 0;
#endif
    return 0;
}
