/**
 * pinyin_composition.h
 * 拼音组合模块接口声明。
 * 对外提供候选元素聚合入口与预编辑展示文本构建入口。
 */
#ifndef PINYIN_COMPOSITION_H
#define PINYIN_COMPOSITION_H

#include <string>
#include <vector>
#include "candidate_item.h"

// 获取所有候选元素（按页拆分为二维数组）
// pinyin_buffer: 当前拼音输入缓冲（用于计算"消耗字母数"排序键）
std::vector<std::vector<CandidateItem>> getAllCandidateElements(
    const std::vector<std::vector<std::string>>& split_options,
    size_t candidate_page_size,
    const std::string& pinyin_buffer);

// 根据拼音缓冲区与虚拟光标位置计算状态栏展示文本（内部执行候选元素获取）
// 若 out_paged_candidates 非空，则同步返回本次获取到的按页拆分的候选元素二维数组
std::string buildComposingDisplayText(const std::string& pinyin_buffer,
                                      size_t virtual_cursor,
                                      size_t candidate_page,
                                      size_t candidate_page_size,
                                      std::vector<std::vector<CandidateItem>>* out_paged_candidates = nullptr);

#endif // PINYIN_COMPOSITION_H
