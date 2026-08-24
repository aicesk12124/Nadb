#include "adb_sync_proto.h"
#include "adb_socket_proto.h"

#include <sys/types.h>
#include <sys/stat.h>

#include <cstring>
#include <ctime>
#include <fstream>
#include <vector>

namespace adbproto {

namespace {

constexpr size_t kSyncChunkMax = 64 * 1024; // "must not be larger than 64k" (SYNC.TXT)

// Верхняя граница для тела FAIL-сообщения. Длина приходит из сети, поэтому
// без ограничения std::string(len, '\0') — тот же способ уронить процесс,
// что и в read_shell_packet.
constexpr uint32_t kMaxSyncErrorLen = 64 * 1024;

// Заголовок sync-пакета: 4 ASCII-байта id + 4 байта LE длины (native — оба конца x86).
bool write_sync_header(SOCKET s, const char id4[4], uint32_t len) {
    char buf[8];
    std::memcpy(buf, id4, 4);
    std::memcpy(buf + 4, &len, 4);
    return send_all(s, buf, sizeof(buf));
}

bool read_sync_header(SOCKET s, char id4_out[4], uint32_t& len_out) {
    char buf[8];
    if (!recv_exact(s, buf, sizeof(buf))) return false;
    std::memcpy(id4_out, buf, 4);
    std::memcpy(&len_out, buf + 4, 4);
    return true;
}

// Безопасно читает текст ошибки после FAIL.
std::string read_fail_message(SOCKET s, uint32_t len) {
    if (len > kMaxSyncErrorLen) return "(некорректная длина сообщения об ошибке)";
    if (len == 0) return "(пустое сообщение об ошибке)";
    std::string err(len, '\0');
    if (!recv_exact(s, err.data(), len)) return "(обрыв соединения при чтении ошибки)";
    return err;
}

} // namespace

SyncResult sync_push(SOCKET s, const std::string& local_path, const std::string& remote_path) {
    SyncResult res;

    std::ifstream in(local_path, std::ios::binary);
    if (!in) {
        res.message = "не удалось открыть локальный файл: " + local_path;
        return res;
    }

    // Реальные метаданные исходного файла вместо захардкоженных значений.
    struct _stat64 st {};
    const bool have_stat = (_stat64(local_path.c_str(), &st) == 0);

    // "Remote file name is split ... by the last comma. Second part is decimal
    //  encoded file mode" (SYNC.TXT). Windows не хранит unix-права, поэтому
    // различаем только доступный на запись (0644) и read-only (0444) файл.
    uint32_t file_mode = 0100644;
    if (have_stat && !(st.st_mode & _S_IWRITE)) file_mode = 0100444;

    std::string path_and_mode = remote_path + "," + std::to_string(file_mode);

    if (!write_sync_header(s, "SEND", static_cast<uint32_t>(path_and_mode.size())) ||
        !send_all(s, path_and_mode.data(), path_and_mode.size())) {
        res.message = "ошибка при отправке SEND-заголовка";
        return res;
    }

    std::vector<char> buf(kSyncChunkMax);
    uint64_t total = 0;
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        std::streamsize got = in.gcount();
        if (got <= 0) break;

        if (!write_sync_header(s, "DATA", static_cast<uint32_t>(got)) ||
            !send_all(s, buf.data(), static_cast<size_t>(got))) {
            res.message = "обрыв соединения при передаче данных (" + std::to_string(total) + " байт отправлено)";
            return res;
        }
        total += static_cast<uint64_t>(got);
    }

    // Раньше ошибка чтения диска была неотличима от EOF и файл уезжал
    // на устройство обрезанным, но с рапортом об успехе.
    if (in.bad()) {
        res.message = "ошибка чтения локального файла: " + local_path;
        return res;
    }
    if (have_stat && total != static_cast<uint64_t>(st.st_size)) {
        res.message = "прочитано " + std::to_string(total) + " из " +
                      std::to_string(static_cast<uint64_t>(st.st_size)) + " байт — передача прервана";
        return res;
    }

    // Время модификации исходного файла, а не момент отправки.
    uint32_t mtime = have_stat ? static_cast<uint32_t>(st.st_mtime)
                               : static_cast<uint32_t>(std::time(nullptr));
    if (!write_sync_header(s, "DONE", mtime)) {
        res.message = "ошибка при отправке DONE";
        return res;
    }

    char id[4] = {};
    uint32_t len = 0;
    if (!read_sync_header(s, id, len)) {
        res.message = "нет ответа от устройства после DONE";
        return res;
    }
    if (std::memcmp(id, "OKAY", 4) == 0) {
        res.ok = true;
        res.bytes_transferred = total;
        res.message = "отправлено " + std::to_string(total) + " байт -> " + remote_path;
        return res;
    }
    if (std::memcmp(id, "FAIL", 4) == 0) {
        res.message = "устройство отклонило файл: " + read_fail_message(s, len);
        return res;
    }
    res.message = "неожиданный ответ устройства после DONE";
    return res;
}

SyncResult sync_pull(SOCKET s, const std::string& remote_path, const std::string& local_path) {
    SyncResult res;

    if (!write_sync_header(s, "RECV", static_cast<uint32_t>(remote_path.size())) ||
        !send_all(s, remote_path.data(), remote_path.size())) {
        res.message = "ошибка при отправке RECV-заголовка";
        return res;
    }

    // Чанки пишутся на диск сразу по мере получения, весь файл в памяти
    // не накапливается.
    std::ofstream out(local_path, std::ios::binary);
    if (!out) {
        res.message = "не удалось создать локальный файл: " + local_path;
        return res;
    }

    uint64_t total = 0;
    std::vector<char> chunk;
    while (true) {
        char id[4] = {};
        uint32_t len = 0;
        if (!read_sync_header(s, id, len)) {
            res.message = "обрыв соединения при получении данных (" + std::to_string(total) + " байт получено)";
            return res;
        }

        if (std::memcmp(id, "DATA", 4) == 0) {
            // SYNC.TXT: "Each chunk must not be larger than 64k".
            // Без этой проверки chunk.resize(len) выделял столько, сколько
            // скажет удалённая сторона — вплоть до 4 ГБ.
            if (len > kSyncChunkMax) {
                res.message = "устройство прислало чанк размером " + std::to_string(len) +
                              " байт при допустимых " + std::to_string(kSyncChunkMax) +
                              " — протокольный сбой";
                return res;
            }
            chunk.resize(len);
            if (len > 0 && !recv_exact(s, chunk.data(), len)) {
                res.message = "обрыв соединения при получении чанка";
                return res;
            }
            out.write(chunk.data(), static_cast<std::streamsize>(len));
            if (!out) {
                res.message = "ошибка записи в локальный файл: " + local_path;
                return res;
            }
            total += len;
        } else if (std::memcmp(id, "DONE", 4) == 0) {
            out.flush();
            if (!out) {
                res.message = "ошибка при сбросе данных на диск: " + local_path;
                return res;
            }
            res.ok = true;
            res.bytes_transferred = total;
            res.message = "получено " + std::to_string(total) + " байт -> " + local_path;
            return res;
        } else if (std::memcmp(id, "FAIL", 4) == 0) {
            res.message = "устройство вернуло ошибку: " + read_fail_message(s, len);
            return res;
        } else {
            res.message = "неизвестный sync-ответ от устройства";
            return res;
        }
    }
}

} // namespace adbproto
