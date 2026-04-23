#ifndef QUERY_FORMS_H
#define QUERY_FORMS_H

#include <string>
#include <unordered_set>
#include <vector>

struct PinyinQueryForms {
    std::string mergeLogic;
    std::string mergeDisplay;
    std::vector<std::string> logicForms;
    std::vector<std::string> displayForms;
};

PinyinQueryForms buildPinyinQueryForms(
    const std::string &input,
    const std::unordered_set<std::string> &mapSyllables);

#endif // QUERY_FORMS_H
