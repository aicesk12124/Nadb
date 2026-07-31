#pragma once
#include "sdk_cache.h"
#include "adb_bridge.h"
#include <functional>

// Колбэк для отображения прогресса построения SDK
using ProgressCallback = std::function<void(const std::string& step, int current, int total)>;

class SdkBuilder {
public:
    explicit SdkBuilder(const AdbBridge& adb);

    // Построить полный SDK для устройства
    // Парсит --help всех утилит + getprop + builtin_map.json
    DeviceSdk build(const std::string& serial,
                    ProgressCallback on_progress = nullptr) const;

    // Загрузить builtin_map.json (базовые захардкоженные команды)
    void load_builtin_map(const std::string& path);

private:
    const AdbBridge& adb_;

    // Базовые команды из builtin_map.json
    std::map<std::string, SdkEntry> builtin_commands_;

    // ── Парсеры отдельных утилит ──

    // getprop — самый важный источник информации
    void parse_getprop(DeviceSdk& sdk) const;

    // pm (package manager)
    void parse_pm(DeviceSdk& sdk) const;

    // am (activity manager)
    void parse_am(DeviceSdk& sdk) const;

    // dumpsys — системная информация
    void parse_dumpsys(DeviceSdk& sdk) const;

    // settings get/put
    void parse_settings(DeviceSdk& sdk) const;

    // set.* — команды записи состояния устройства (battery, display, network, system)
    void parse_set_commands(DeviceSdk& sdk) const;

    // toggle.* — команды, переключающие текущее bool-состояние без явного value
    void parse_toggle_commands(DeviceSdk& sdk) const;

    // Получить метаинформацию об устройстве (serial, version, build)
    void fill_device_meta(DeviceSdk& sdk) const;

    // Добавить запись в SDK
    // writable=true и value_hint заполняются для "set.*" команд, использующих %value%
    // value_rule — машиночитаемое правило валидации: "range:MIN:MAX" или "enum:a,b,c"
    static void add_entry(DeviceSdk& sdk,
                          const std::string& nadb_cmd,
                          const std::string& adb_cmd,
                          const std::string& description,
                          const std::string& category,
                          bool writable = false,
                          const std::string& value_hint = "",
                          const std::string& value_rule = "");

    // Добавить toggle.* запись: переключает bool-состояние без явного value.
    // set_adb_cmd должен содержать %value%, которое подставится on_value/off_value.
    // read_command_key — ключ уже существующей get.* команды, чей вывод говорит о текущем состоянии.
    static void add_toggle_entry(DeviceSdk& sdk,
                                 const std::string& nadb_cmd,
                                 const std::string& set_adb_cmd,
                                 const std::string& read_command_key,
                                 const std::string& on_value,
                                 const std::string& off_value,
                                 const std::string& description,
                                 const std::string& category);
};
