#pragma once
#include <string>
#include <vector>
#include <map>
#include "sdk_cache.h"

class FuzzyMatcher {
public:
    // Найти наиболее похожую команду из SDK
    // Возвращает пустую строку если ничего похожего нет
    static std::string find_closest(const std::string& input,
                                    const std::map<std::string, SdkEntry>& commands,
                                    int max_distance = 4);

    // Расстояние Левенштейна между двумя строками
    static int levenshtein(const std::string& a, const std::string& b);

    // Найти все команды подходящей категории
    static std::vector<std::string> find_by_category(
        const std::string& category,
        const std::map<std::string, SdkEntry>& commands);
};
