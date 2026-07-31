#pragma once
// adb_sync_proto — реализация sync-подпротокола (SYNC.TXT) поверх уже
// открытого сокета в sync mode ("sync:" запрос уже отправлен и получен OKAY).
//
// Реализовано: SEND (push одного файла), RECV (pull одного файла).
// TODO: LIST (листинг директорий), STAT (метаданные без скачивания),
//       рекурсивный push/pull директорий, сохранение реальных unix-прав
//       и mtime исходного файла вместо текущего времени.

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>

#include <cstdint>
#include <string>

namespace adbproto {

struct SyncResult {
    bool ok = false;
    std::string message;            // человекочитаемое сообщение об успехе/ошибке
    uint64_t bytes_transferred = 0;
};

// Отправить локальный файл на устройство по remote_path.
// Сокет должен уже быть переключён в sync mode.
SyncResult sync_push(SOCKET s, const std::string& local_path, const std::string& remote_path);

// Забрать файл с устройства (remote_path) в локальный файл (local_path).
// Сокет должен уже быть переключён в sync mode.
SyncResult sync_pull(SOCKET s, const std::string& remote_path, const std::string& local_path);

} // namespace adbproto
