#pragma once
#include "sdk_cache.h"
#include "adb_bridge.h"
#include <string>
#include <vector>

class PhoneInfo {
public:
    PhoneInfo(const AdbBridge& adb, const DeviceSdk& sdk);

    // Вывести neofetch-стиль: лого слева, инфо справа
    void print_overview() const;

private:
    const AdbBridge& adb_;
    const DeviceSdk& sdk_;

    // Загрузить лого из файла рядом с exe
    static std::vector<std::string> load_logo();

    // Получить строки информации об устройстве
    std::vector<std::pair<std::string, std::string>> get_info_lines() const;

    // Быстрый getprop
    std::string prop(const std::string& key) const;
};
