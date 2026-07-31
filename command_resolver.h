#pragma once
#include "sdk_cache.h"
#include "adb_bridge.h"
#include <string>
#include <optional>

struct ResolveResult {
    bool found = false;
    bool executed = false;      // false если команда найдена, но не была выполнена
                                 // (например, writable-команде не хватило значения)
    bool needs_value = false;   // true если команда требует value, а его не передали
    bool invalid_value = false; // true если переданное value не прошло валидацию
    std::string value_hint;     // подсказка по допустимым значениям (если needs_value/invalid_value)
    std::string adb_command;    // итоговая adb команда
    std::string output;         // вывод после выполнения
    std::string error;
};

class CommandResolver {
public:
    CommandResolver(const AdbBridge& adb, const DeviceSdk& sdk);

    // Разрезолвить и выполнить nadb команду
    // Пример: resolve("get.android.version")
    // Для writable команд (set.*) передаётся value:
    // resolve("set.battery.percent", "50")
    // toggle.* команды игнорируют value — они сами читают и переключают состояние
    ResolveResult resolve(const std::string& nadb_command, const std::string& value = "") const;

    // Только разрезолвить (без выполнения) — для отладки
    std::optional<SdkEntry> lookup(const std::string& nadb_command) const;

private:
    const AdbBridge& adb_;
    const DeviceSdk& sdk_;

    // Постобработка вывода (убрать лишние пробелы, grep результат и т.д.)
    static std::string clean_output(const std::string& raw);

    // Выполнить итоговую adb-команду (с учётом pipe/&&/;) и вернуть stdout/stderr
    void execute_adb_command(const std::string& final_cmd, std::string& out, std::string& err) const;

    // Обработка toggle.* команд: читает текущее состояние и переключает его
    ResolveResult resolve_toggle(const SdkEntry& entry, const std::string& nadb_command) const;
};
