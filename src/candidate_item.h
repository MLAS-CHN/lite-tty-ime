#ifndef CANDIDATE_ITEM_H
#define CANDIDATE_ITEM_H

#include <string>
#include <vector>

/**
 * 候选项数据结构
 * - pinyin_parts: 拼音分段数组
 * - text: 对应词/字
 * - timestamp: 最近一次选中时间戳（Unix 秒），用于时间衰减评分
 * - count: 累计选中次数（权重）
 */
class CandidateItem {
public:
    /** 默认构造：空拼音、空文本、零时间戳、计数 1。 */
    CandidateItem() : pinyin_parts_(), text_(""), timestamp_(0), count_(1) {}

    /**
     * 完整构造函数。
     * @param pinyin_parts 拼音分段数组。
     * @param text 对应候选文本。
     * @param timestamp 最近一次选中时间戳（Unix 秒）。
     * @param count 累计选中次数。
     */
    CandidateItem(const std::vector<std::string>& pinyin_parts,
                  const std::string& text,
                  long long timestamp,
                  int count)
        : pinyin_parts_(pinyin_parts), text_(text), timestamp_(timestamp), count_(count) {}

    /** 获取拼音分段数组。 */
    const std::vector<std::string>& getPinyinParts() const {
        return pinyin_parts_;
    }

    /** 设置拼音分段数组。 */
    void setPinyinParts(const std::vector<std::string>& pinyin_parts) {
        pinyin_parts_ = pinyin_parts;
    }

    /** 获取拼音分段长度。 */
    size_t getPinyinLength() const {
        return pinyin_parts_.size();
    }

    /** 获取候选文本。 */
    const std::string& getText() const {
        return text_;
    }

    /** 设置候选文本。 */
    void setText(const std::string& text) {
        text_ = text;
    }

    /** 获取累计选中次数。 */
    int getCount() const {
        return count_;
    }

    /** 设置累计选中次数。 */
    void setCount(int count) {
        count_ = count;
    }

    /** 获取最近一次选中时间戳（Unix 秒）。 */
    long long getTimestamp() const {
        return timestamp_;
    }

    /** 设置最近一次选中时间戳（Unix 秒）。 */
    void setTimestamp(long long timestamp) {
        timestamp_ = timestamp;
    }

    /**
     * 时间衰减评分：count 与距最近一次使用的时间成反比放大。
     * 最近用过的词分值更高，久未使用的词逐渐回归 count 本身。
     *
     * @param now 当前 Unix 秒。
     * @return 评分。
     */
    long long computeScore(long long now) const {
        long long gap = now - timestamp_;
        if (gap < 0) gap = 0;
        long long gap_minutes = gap / 60;
        const long long DECAY = 100000;
        long long multiplier = 1 + DECAY / (1 + gap_minutes);
        return (long long)count_ * multiplier;
    }

    std::string toString() const;
    std::string getSourceFileName() const;
    int findSourceLineNumber() const;

    /**
     * 根据字库行号构建候选元素（预留接口，当前留空）。
     *
     * @param line_number 字库真实行号（1-based）。
     * @return 候选元素对象。
     */
    static CandidateItem fromCharDictLineNumber(int line_number);

    /**
     * 根据词库行号构建候选元素。
     * 支持三种行格式：
     * - pinyin_csv text timestamp count
     * - pinyin_csv text count
     * - pinyin_csv text
     * 缺省时 timestamp=0，count=1。
     *
     * @param line_number 词库真实行号（1-based）。
     * @return 候选元素对象。
     */
    static CandidateItem fromWordDictLineNumber(int line_number);

    static CandidateItem mergeCandidateItems(const std::vector<CandidateItem>& items);

    /**
     * 按时间戳降序对候选数组执行排序（原地修改）。
     *
     * @param candidates 待排序的候选元素数组。
     */
    static void quickSortByTimestampDesc(std::vector<CandidateItem>& candidates);

private:
    std::vector<std::string> pinyin_parts_;
    std::string text_;
    long long timestamp_;
    int count_;
};

#endif // CANDIDATE_ITEM_H
