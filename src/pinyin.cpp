#include "pinyin.h"
#include <algorithm>
#include <cctype>
#include <functional>
#include <unordered_set>
#include <vector>
#include <string>

// 工具函数：转小写
static std::string toLowerCopy(std::string s) {
    for (char &c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

// 工具函数：去除首尾空白
static std::string trim(const std::string &s) {
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) b++;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
    return s.substr(b, e - b);
}

// 激进拆分拼音实现
std::string aggressiveSplitPinyin(const std::string &normalizedInput,
                                  const std::unordered_set<std::string> &mapSyllables) {
    struct AggToken {
        std::string text;
        bool forced = false;
    };
    struct AggResult {
        std::vector<AggToken> toks;
        int forcedCount = 0;
        int tokenCount = 0;
    };

    auto solveChunk = [&](const std::string &chunk) -> std::vector<AggToken> {
        const size_t n = chunk.size();
        if (n == 0) return {};
        std::unordered_map<size_t, AggResult> memo;

        std::function<AggResult(size_t)> dfs = [&](size_t pos) -> AggResult {
            auto it = memo.find(pos);
            if (it != memo.end()) {
                return it->second;
            }
            if (pos >= n) {
                return AggResult{};
            }

            bool hasBest = false;
            AggResult best;
            auto takeBest = [&](const AggResult &cand) {
                if (!hasBest) {
                    best = cand;
                    hasBest = true;
                    return;
                }
                if (cand.forcedCount != best.forcedCount) {
                    if (cand.forcedCount < best.forcedCount) best = cand;
                    return;
                }
                // forced 数相同：更"激进"优先（分段更多）
                if (cand.tokenCount > best.tokenCount) {
                    best = cand;
                }
            };

            // 1) 先尝试合法音节分段（激进：从短到长）
            const size_t maxLen = std::min<size_t>(6, n - pos);
            for (size_t len = 1; len <= maxLen; ++len) {
                const std::string seg = chunk.substr(pos, len);
                if (mapSyllables.find(seg) == mapSyllables.end()) {
                    continue;
                }
                AggResult tail = dfs(pos + len);
                AggResult cand;
                cand.toks.reserve(1 + tail.toks.size());
                cand.toks.push_back(AggToken{seg, false});
                cand.toks.insert(cand.toks.end(), tail.toks.begin(), tail.toks.end());
                cand.forcedCount = tail.forcedCount;
                cand.tokenCount = tail.tokenCount + 1;
                takeBest(cand);
            }

            // 2) 若合法分段走不通/质量不佳，允许强制切 1 字符（用 ' 锁住边界）
            {
                AggResult tail = dfs(pos + 1);
                AggResult cand;
                cand.toks.reserve(1 + tail.toks.size());
                cand.toks.push_back(AggToken{chunk.substr(pos, 1), true});
                cand.toks.insert(cand.toks.end(), tail.toks.begin(), tail.toks.end());
                cand.forcedCount = tail.forcedCount + 1;
                cand.tokenCount = tail.tokenCount + 1;
                takeBest(cand);
            }

            memo[pos] = best;
            return best;
        };

        return dfs(0).toks;
    };

    // 手动分隔符 ' 是硬边界：各子串独立分配，回溯不跨越该边界。
    std::vector<std::string> chunks;
    {
        std::string cur;
        for (char ch : normalizedInput) {
            if (ch == '\'') {
                chunks.push_back(cur);
                cur.clear();
            } else {
                cur.push_back(ch);
            }
        }
        chunks.push_back(cur);
    }

    std::string out;
    bool firstChunk = true;
    for (const auto &chunk : chunks) {
        if (!firstChunk) {
            out += '\'';
        }
        firstChunk = false;
        const auto toks = solveChunk(chunk);
        if (toks.empty()) {
            continue;
        }
        out += toks[0].text;
        for (size_t i = 1; i < toks.size(); ++i) {
            const bool useManual = toks[i - 1].forced || toks[i].forced;
            out += useManual ? '\'' : ',';
            out += toks[i].text;
        }
    }
    return out;
}

// 保守拆分拼音实现
std::string conservativeSplitPinyin(const std::string &normalizedInput,
                                    const std::unordered_set<std::string> &mapSyllables) {
    auto hasPrefix = [&](const std::string &s) -> bool {
        if (s.empty()) return false;
        for (const auto &py : mapSyllables) {
            if (py.size() < s.size()) continue;
            if (py.compare(0, s.size(), s) == 0) return true;
        }
        return false;
    };

    std::vector<std::string> chunks;
    {
        std::string cur;
        for (char ch : normalizedInput) {
            if (ch == '\'') {
                chunks.push_back(cur);
                cur.clear();
            } else {
                cur.push_back(ch);
            }
        }
        chunks.push_back(cur);
    }

    std::string out;
    bool firstChunk = true;
    for (const auto &chunk : chunks) {
        if (!firstChunk) out += '\'';
        firstChunk = false;
        if (chunk.empty()) continue;

        std::vector<std::string> toks;
        std::string cur;
        size_t i = 0;
        while (i < chunk.size()) {
            const char ch = chunk[i];
            if (cur.empty()) {
                std::string one(1, ch);
                if (!hasPrefix(one)) {
                    // 新段首字母无法命中任何前缀：
                    // 不再删除，保留为独立段，避免类似 bani 里的 i 被吞掉。
                    toks.push_back(one);
                    ++i;
                    continue;
                }
                cur = one;
                ++i;
                continue;
            }

            const std::string nxt = cur + ch;
            if (hasPrefix(nxt)) {
                // 保守：尽量不分割，能继续就继续扩展当前段
                cur = nxt;
                ++i;
            } else {
                // 继续失败，先固定当前段；当前字符留给下一段重新处理
                toks.push_back(cur);
                cur.clear();
            }
        }
        if (!cur.empty()) {
            toks.push_back(cur);
        }

        if (toks.empty()) {
            continue;
        }
        out += toks[0];
        for (size_t ti = 1; ti < toks.size(); ++ti) {
            out += ",";
            out += toks[ti];
        }
    }
    return out;
}

// 合并拆分实现
std::vector<std::string> mergeSplitPinyin(const std::string &radStr,
                                          const std::unordered_set<std::string> &mapSyllables) {
    if (radStr.empty()) {
        return {};
    }
    auto hasPrefixOrExact = [&](const std::string &s) -> bool {
        if (s.empty()) return false;
        if (mapSyllables.find(s) != mapSyllables.end()) return true;
        for (const auto &py : mapSyllables) {
            if (py.size() < s.size()) continue;
            if (py.compare(0, s.size(), s) == 0) return true;
        }
        return false;
    };
    auto parse = [&](const std::string &in, std::vector<std::string> &tokens, std::vector<char> &seps) {
        tokens.clear();
        seps.clear();
        std::string cur;
        for (char ch : in) {
            if (ch == ',' || ch == '\'') {
                tokens.push_back(cur);
                cur.clear();
                seps.push_back(ch);
            } else {
                cur.push_back(ch);
            }
        }
        tokens.push_back(cur);
    };
    auto dump = [&](const std::vector<std::string> &tokens, const std::vector<char> &seps) {
        if (tokens.empty()) return std::string{};
        std::string out = tokens[0];
        for (size_t i = 0; i < seps.size() && i + 1 < tokens.size(); ++i) {
            out.push_back(seps[i]);
            out += tokens[i + 1];
        }
        return out;
    };
    auto onePassMerge = [&](std::vector<std::string> &tokens, std::vector<char> &seps) {
        if (tokens.size() <= 1 || seps.empty()) return false;
        bool changed = false;
        for (size_t i = 0; i + 1 < tokens.size();) {
            if (i >= seps.size()) break;
            if (seps[i] != ',') { // 手动分隔符 ' 为硬边界，不跨边界合并
                ++i;
                continue;
            }
            const std::string joined = tokens[i] + tokens[i + 1];
            if (hasPrefixOrExact(joined)) {
                tokens[i] = joined;
                tokens.erase(tokens.begin() + static_cast<std::ptrdiff_t>(i + 1));
                seps.erase(seps.begin() + static_cast<std::ptrdiff_t>(i));
                changed = true;
                // 合并后继续在当前位置尝试与新 i+1 再合并
            } else {
                ++i;
            }
        }
        return changed;
    };

    std::vector<std::string> tokens;
    std::vector<char> seps;
    parse(radStr, tokens, seps);

    std::vector<std::string> out;
    for (int pass = 0; pass < 2; ++pass) {
        const bool changed = onePassMerge(tokens, seps);
        out.push_back(dump(tokens, seps));
        if (!changed) break;
    }
    return out;
}

// 计算分段拼音输入
std::vector<std::string> computeSegmentedPinyinInputs(
    const std::string &rawInput,
    const std::unordered_set<std::string> &mapSyllables) {
    if (rawInput.empty()) {
        return {};
    }
    const std::string normalized = toLowerCopy(trim(rawInput));
    if (normalized.empty()) {
        return {};
    }

    const std::string aggressive = aggressiveSplitPinyin(normalized, mapSyllables);
    const std::string conservative = conservativeSplitPinyin(normalized, mapSyllables);
    const std::vector<std::string> merged = mergeSplitPinyin(aggressive, mapSyllables);

    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    auto addUnique = [&](const std::string &s) {
        if (s.empty()) return;
        if (seen.insert(s).second) {
            out.push_back(s);
        }
    };

    // 顺序：激进 -> 保守 -> 合并（与需求一致）
    addUnique(aggressive);
    addUnique(conservative);
    for (const auto &s : merged) {
        addUnique(s);
    }
    if (out.empty()) {
        out.push_back(normalized);
    }
    return out;
}
