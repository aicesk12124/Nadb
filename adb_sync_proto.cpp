#include "adb_sync_proto.h"
#include "adb_socket_proto.h"

#include <cstring>
#include <ctime>
#include <fstream>
#include <vector>

namespace adbproto {

namespace {

constexpr size_t kSyncChunkMax = 64 * 1024; // "must not be larger than 64k" (SYNC.TXT)

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

} // namespace

SyncResult sync_push(SOCKET s, const std::string& local_path, const std::string& remote_path) {
    SyncResult res;

    std::ifstream in(local_path, std::ios::binary);
    if (!in) {
        res.message = "не удалось открыть локальный файл: " + local_path;
        return res;
    }

    // "Remote file name is split ... by the last comma. Second part is decimal
    //  encoded file mode" (SYNC.TXT). TODO: брать реальные unix-права/exec-бит
    // исходного файла вместо фиксированного 0100644 (обычный файл, rw-r--r--).
    const uint32_t file_mode = 0100644;
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

    // TODO: использовать реальное время модификации local_path, а не "сейчас".
    uint32_t mtime = static_cast<uint32_t>(std::time(nullptr));
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
        std::string err(len, '\0');
        recv_exact(s, err.data(), len);
        res.message = "устройство отклонило файл: " + err;
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

    // TODO: для больших файлов писать чанки сразу на диск по мере получения —
    // сейчас всё ок, т.к. ofstream::write вызывается сразу на каждый чанк,
    // в памяти не накапливается весь файл целиком.
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
            chunk.resize(len);
            if (len > 0 && !recv_exact(s, chunk.data(), len)) {
                res.message = "обрыв соединения при получении чанка";
                return res;
            }
            out.write(chunk.data(), static_cast<std::streamsize>(len));
            total += len;
        } else if (std::memcmp(id, "DONE", 4) == 0) {
            res.ok = true;
            res.bytes_transferred = total;
            res.message = "получено " + std::to_string(total) + " байт -> " + local_path;
            return res;
        } else if (std::memcmp(id, "FAIL", 4) == 0) {
            std::string err(len, '\0');
            recv_exact(s, err.data(), len);
            res.message = "устройство вернуло ошибку: " + err;
            return res;
        } else {
            res.message = "неизвестный sync-ответ от устройства";
            return res;
        }
    }
}

} // namespace adbproto
