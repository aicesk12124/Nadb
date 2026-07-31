#pragma once
#include <string>
#include <map>
#include <optional>

// Одна запись в SDK — маппинг nadb команды на adb команду
struct SdkEntry {
    std::string nadb_command;   // "get.android.version" / "set.battery.percent"
    std::string adb_command;    // "shell getprop ro.build.version.release"
                                 // Для writable-команд может содержать плейсхолдер %value%,
                                 // который подставляется значением, переданным пользователем:
                                 // "shell dumpsys battery set level %value%"
    std::string description;    // "Android version"
    std::string category;       // "android", "phone", "battery" ...
    bool writable = false;      // true для "set.*" команд, изменяющих состояние устройства
    std::string value_hint;     // подсказка по допустимым значениям, напр. "0-100", "0 | 1"

    // Машиночитаемое правило валидации значения (пусто = без валидации):
    //   "range:MIN:MAX"  — целое число в диапазоне, напр. "range:0:100"
    //   "enum:a,b,c"     — значение должно точно совпасть с одним из вариантов
    std::string value_rule;

    // ── toggle.* ──────────────────────────────────────────
    // Команды toggle.* сами читают текущее состояние через toggle_read_command
    // и переключают его на противоположное, не требуя от пользователя значения.
    bool toggleable = false;
    std::string toggle_read_command; // ключ get.* команды, чей вывод даёт текущее состояние
    std::string toggle_on_value;     // значение %value% при включении
    std::string toggle_off_value;    // значение %value% при выключении
};

// Весь SDK устройства
struct DeviceSdk {
    std::string serial;
    std::string android_version;
    std::string build_id;
    std::string sdk_version;       // API level
    std::string generated_at;      // ISO timestamp
    std::map<std::string, SdkEntry> commands; // ключ = nadb_command
};

class SdkCache {
public:
    explicit SdkCache(const std::string& cache_dir = "");

    // Загрузить SDK для устройства. Возвращает nullopt если нет кэша
    std::optional<DeviceSdk> load(const std::string& serial) const;

    // Сохранить SDK на диск
    bool save(const DeviceSdk& sdk) const;

    // Проверить что кэш актуален (сравниваем sdk_version + build_id)
    bool is_valid(const std::string& serial,
                  const std::string& sdk_version,
                  const std::string& build_id) const;

    // Удалить кэш для устройства
    void invalidate(const std::string& serial) const;

    // Путь к файлу кэша для устройства
    std::string cache_path(const std::string& serial) const;

private:
    std::string cache_dir_;
    static std::string get_default_cache_dir();
};
