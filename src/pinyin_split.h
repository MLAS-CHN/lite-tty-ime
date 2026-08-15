/**
 * pinyin_split.h
 * 拼音拆分模块接口声明。
 * 定义拆分入口、合并接口及最终拆分方案导出函数。
 */
#ifndef PINYIN_SPLIT_H
#define PINYIN_SPLIT_H

#include <string>
#include <vector>

/**
 * 激进拆分主入口 (处理带 ' 的混合输入)
 */
std::string aggressivePinyinSplitMain(const std::string& raw);

/**
 * 保守拆分主入口
 */
std::string conservativePinyinSplitMain(const std::string& raw);

/**
 * 将初步拆分的拼音进行两轮合并
 */
std::vector<std::string> mergeConservativePinyin(const std::string& input);

/**
 * 总拆分导出函数
 * 整合激进拆分与保守拆分的结果
 */
std::vector<std::vector<std::string>> splitConservativePinyin(const std::string& input);

/**
 * 拆分结果：方案列表。
 */
struct SplitResult {
    std::vector<std::vector<std::string>> options;
};

SplitResult splitConservativePinyinEx(const std::string& input);

#endif // PINYIN_SPLIT_H
