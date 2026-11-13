#include "FileTransfer.h"
#include "Logger.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace s2p {

namespace {

bool FileExistsW(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

void EnsureDirExistsW(const std::wstring& dir) {
    if (dir.empty()) return;
    CreateDirectoryW(dir.c_str(), nullptr);
}

} // namespace

FileTransfer::~FileTransfer() {
    Stop();
}

uint32_t FileTransfer::ComputeCrc32(const void* data, size_t size, uint32_t seed) {
    uint32_t crc = ~seed;
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        crc ^= bytes[i];
        for (int b = 0; b < 8; ++b) {
            uint32_t mask = (crc & 1u) ? 0xEDB88320u : 0u;
            crc = (crc >> 1) ^ mask;
        }
    }
    return ~crc;
}

std::wstring FileTransfer::DefaultDownloadDir() {
    wchar_t buf[MAX_PATH]{};
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
    std::wstring base = (n > 0 && n < MAX_PATH) ? std::wstring(buf) + L"\\Downloads" : L".";
    CreateDirectoryW(base.c_str(), nullptr);
    std::wstring dir = base + L"\\Screen2PC";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring FileTransfer::SanitizeFilename(const std::wstring& rawName) {
    // Take only component after last '/' or '\'
    size_t pos = rawName.find_last_of(L"/\\");
    std::wstring name = (pos == std::wstring::npos) ? rawName : rawName.substr(pos + 1);

    // Strip any leading drive prefix if present (e.g. "C:foo")
    size_t colon = name.find(L':');
    if (colon != std::wstring::npos) {
        name = name.substr(colon + 1);
    }

    std::wstring cleaned;
    cleaned.reserve(name.size());
    for (wchar_t ch : name) {
        if (ch < 32 || ch == L'<' || ch == L'>' || ch == L':' || ch == L'"' ||
            ch == L'/' || ch == L'\\' || ch == L'|' || ch == L'?' || ch == L'*') {
            cleaned.push_back(L'_');
        } else {
            cleaned.push_back(ch);
        }
    }

    while (!cleaned.empty() && (cleaned.back() == L' ' || cleaned.back() == L'.')) {
        cleaned.pop_back();
    }
    if (cleaned.empty() || cleaned == L"." || cleaned == L"..") {
        return L"received_file.bin";
    }
    return cleaned;
}

std::wstring FileTransfer::ResolveUniqueDestPath(const std::wstring& dir, const std::wstring& safeFilename) {
    EnsureDirExistsW(dir);
    std::wstring clean = SanitizeFilename(safeFilename);
    std::wstring candidate = dir + L"\\" + clean;
    if (!FileExistsW(candidate)) {
        return candidate;
    }

    size_t dot = clean.find_last_of(L'.');
    std::wstring stem = (dot == std::wstring::npos) ? clean : clean.substr(0, dot);
    std::wstring ext = (dot == std::wstring::npos) ? L"" : clean.substr(dot);

    for (int idx = 1; idx < 10000; ++idx) {
        std::wstring next = dir + L"\\" + stem + L" (" + std::to_wstring(idx) + L")" + ext;
        if (!FileExistsW(next)) {
            return next;
        }
    }
    return candidate;
}

void FileTransfer::Start(TcpSocket sock, const std::wstring& customDownloadDir) {
    Stop();
    if (!sock.IsValid()) return;

    m_stop = false;
    m_sock = std::move(sock);
    m_sock.TuneLowLatency(2 * 1024 * 1024);
    m_sock.SetTimeouts(5000, 1000);

    m_downloadDir = customDownloadDir.empty() ? DefaultDownloadDir() : customDownloadDir;
    EnsureDirExistsW(m_downloadDir);

    {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        m_stats = FileTransferStats{};
        m_stats.active = true;
        m_stats.downloadDir = m_downloadDir;
        m_stats.lastEvent = L"Dateiuebertragung bereit";
    }

    m_connected = true;
    m_recvThread = std::thread(&FileTransfer::RecvWorker, this);
    m_sendThread = std::thread(&FileTransfer::SendWorker, this);
}

void FileTransfer::Stop() {
    m_stop = true;
    m_connected = false;
    m_queueCv.notify_all();
    m_sock.Close();

    if (m_sendThread.joinable()) m_sendThread.join();
    if (m_recvThread.joinable()) m_recvThread.join();

    {
        std::lock_guard<std::mutex> qLock(m_queueMutex);
        m_sendQueue.clear();
    }
    {
        std::lock_guard<std::mutex> sLock(m_statsMutex);
        m_stats.active = false;
    }
}

size_t FileTransfer::QueueFiles(const std::vector<std::wstring>& paths) {
    if (!m_connected.load()) return 0;
    size_t queued = 0;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        for (const auto& p : paths) {
            if (!p.empty() && FileExistsW(p)) {
                m_sendQueue.push_back(p);
                ++queued;
            }
        }
    }
    if (queued > 0) {
        m_queueCv.notify_one();
    }
    return queued;
}

FileTransferStats FileTransfer::GetStats() const {
    std::lock_guard<std::mutex> lock(m_statsMutex);
    return m_stats;
}

void FileTransfer::SendWorker() {
    while (!m_stop && m_connected.load()) {
        std::wstring nextPath;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_queueCv.wait(lock, [this]() {
                return m_stop.load() || !m_connected.load() || !m_sendQueue.empty();
            });
            if (m_stop || !m_connected.load()) break;
            nextPath = std::move(m_sendQueue.front());
            m_sendQueue.pop_front();
        }
        if (!SendSingleFile(nextPath)) {
            Logger::Instance().Warn("FILE-TX", L"Failed sending file: " + nextPath);
        }
    }
}

bool FileTransfer::SendSingleFile(const std::wstring& fullPath) {
    HANDLE hFile = CreateFileW(fullPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER liSize{};
    if (!GetFileSizeEx(hFile, &liSize) || liSize.QuadPart < 0 ||
        static_cast<uint64_t>(liSize.QuadPart) > kMaxFileTransferBytes) {
        CloseHandle(hFile);
        return false;
    }

    uint64_t fileSize = static_cast<uint64_t>(liSize.QuadPart);
    std::wstring baseName = SanitizeFilename(fullPath);
    std::string utf8Name = WideToUtf8(baseName);
    if (utf8Name.empty() || utf8Name.size() > 1024) {
        CloseHandle(hFile);
        return false;
    }

    uint32_t transferId = m_nextId.fetch_add(1);

    // 1. Send FileMeta
    {
        FileMetaHeader meta{};
        meta.transferId = transferId;
        meta.fileSize = fileSize;
        meta.nameUtf8Bytes = static_cast<uint32_t>(utf8Name.size());

        std::vector<uint8_t> pkt(sizeof(meta) + utf8Name.size());
        std::memcpy(pkt.data(), &meta, sizeof(meta));
        std::memcpy(pkt.data() + sizeof(meta), utf8Name.data(), utf8Name.size());

        if (!m_sock.SendPacket(MsgType::FileMeta, pkt.data(), static_cast<uint32_t>(pkt.size()))) {
            CloseHandle(hFile);
            m_connected = false;
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        m_stats.lastEvent = L"Sende: " + baseName + L"...";
    }

    // 2. Stream chunks
    std::vector<uint8_t> ioBuf(kFileChunkSize);
    std::vector<uint8_t> pkt(sizeof(FileChunkHeader) + kFileChunkSize);
    uint64_t offset = 0;
    uint32_t totalCrc = 0;
    bool readErr = false;

    while (!m_stop && offset < fileSize) {
        DWORD toRead = static_cast<DWORD>(std::min<uint64_t>(kFileChunkSize, fileSize - offset));
        DWORD got = 0;
        if (!ReadFile(hFile, ioBuf.data(), toRead, &got, nullptr) || got == 0) {
            readErr = true;
            break;
        }

        uint32_t chunkCrc = ComputeCrc32(ioBuf.data(), got);
        totalCrc = ComputeCrc32(ioBuf.data(), got, totalCrc);

        FileChunkHeader ch{};
        ch.transferId = transferId;
        ch.offset = offset;
        ch.chunkSize = got;
        ch.crc32 = chunkCrc;

        pkt.resize(sizeof(ch) + got);
        std::memcpy(pkt.data(), &ch, sizeof(ch));
        std::memcpy(pkt.data() + sizeof(ch), ioBuf.data(), got);

        if (!m_sock.SendPacket(MsgType::FileChunk, pkt.data(), static_cast<uint32_t>(pkt.size()))) {
            CloseHandle(hFile);
            m_connected = false;
            return false;
        }

        offset += got;
        std::lock_guard<std::mutex> lock(m_statsMutex);
        m_stats.bytesSent += got;
    }

    CloseHandle(hFile);

    // 3. Send FileDone
    FileDonePayload done{};
    done.transferId = transferId;
    done.totalBytes = offset;
    done.totalCrc32 = totalCrc;
    done.ok = (!readErr && offset == fileSize) ? 1 : 0;

    if (!m_sock.SendPacket(MsgType::FileDone, &done, sizeof(done))) {
        m_connected = false;
        return false;
    }

    if (done.ok) {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        m_stats.filesSent++;
        m_stats.lastEvent = L"Gesendet: " + baseName;
        Logger::Instance().Info("FILE-TX", L"Sent file '" + baseName + L"' (" + std::to_wstring(fileSize) + L" bytes)");
    }
    return done.ok != 0;
}

void FileTransfer::RecvWorker() {
    struct ActiveRecv {
        uint32_t transferId = 0;
        uint64_t expectedSize = 0;
        uint64_t receivedBytes = 0;
        uint32_t runningCrc = 0;
        std::wstring safeName;
        std::wstring tempPath;
        HANDLE hFile = INVALID_HANDLE_VALUE;
    } cur;

    auto abortCurrent = [&]() {
        if (cur.hFile != INVALID_HANDLE_VALUE) {
            CloseHandle(cur.hFile);
            cur.hFile = INVALID_HANDLE_VALUE;
        }
        if (!cur.tempPath.empty()) {
            DeleteFileW(cur.tempPath.c_str());
            cur.tempPath.clear();
        }
        cur.transferId = 0;
    };

    while (!m_stop && m_sock.IsValid()) {
        int wr = m_sock.WaitReadable(100);
        if (wr < 0) break;
        if (wr == 0) continue;

        PacketHeader hdr{};
        std::vector<uint8_t> payload;
        if (!m_sock.RecvPacket(hdr, payload)) {
            int err = WSAGetLastError();
            if (err == WSAETIMEDOUT || err == 0) continue;
            break;
        }

        if (hdr.type == static_cast<uint16_t>(MsgType::FileMeta) && payload.size() >= sizeof(FileMetaHeader)) {
            abortCurrent();
            FileMetaHeader meta{};
            std::memcpy(&meta, payload.data(), sizeof(meta));
            if (meta.fileSize > kMaxFileTransferBytes || meta.nameUtf8Bytes == 0 ||
                meta.nameUtf8Bytes > 1024 || payload.size() < sizeof(meta) + meta.nameUtf8Bytes) {
                continue;
            }
            std::string utf8Name(reinterpret_cast<const char*>(payload.data() + sizeof(meta)), meta.nameUtf8Bytes);
            cur.safeName = SanitizeFilename(Utf8ToWide(utf8Name));
            cur.transferId = meta.transferId;
            cur.expectedSize = meta.fileSize;
            cur.receivedBytes = 0;
            cur.runningCrc = 0;
            cur.tempPath = m_downloadDir + L"\\.part_" + std::to_wstring(meta.transferId) + L"_" + cur.safeName;

            cur.hFile = CreateFileW(cur.tempPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (cur.hFile == INVALID_HANDLE_VALUE) {
                cur.tempPath.clear();
                cur.transferId = 0;
            } else {
                std::lock_guard<std::mutex> lock(m_statsMutex);
                m_stats.lastEvent = L"Empfange: " + cur.safeName + L"...";
            }
        } else if (hdr.type == static_cast<uint16_t>(MsgType::FileChunk) && payload.size() >= sizeof(FileChunkHeader)) {
            FileChunkHeader ch{};
            std::memcpy(&ch, payload.data(), sizeof(ch));
            if (cur.hFile == INVALID_HANDLE_VALUE || ch.transferId != cur.transferId) continue;
            if (ch.chunkSize > kFileChunkSize || payload.size() < sizeof(ch) + ch.chunkSize) {
                abortCurrent();
                continue;
            }
            if (ch.offset != cur.receivedBytes || cur.receivedBytes + ch.chunkSize > cur.expectedSize) {
                abortCurrent();
                continue;
            }

            const uint8_t* bytes = payload.data() + sizeof(ch);
            if (ComputeCrc32(bytes, ch.chunkSize) != ch.crc32) {
                Logger::Instance().Warn("FILE-RX", L"Chunk CRC32 mismatch for " + cur.safeName);
                abortCurrent();
                continue;
            }

            DWORD written = 0;
            if (!WriteFile(cur.hFile, bytes, ch.chunkSize, &written, nullptr) || written != ch.chunkSize) {
                abortCurrent();
                continue;
            }

            cur.runningCrc = ComputeCrc32(bytes, ch.chunkSize, cur.runningCrc);
            cur.receivedBytes += ch.chunkSize;
            {
                std::lock_guard<std::mutex> lock(m_statsMutex);
                m_stats.bytesReceived += ch.chunkSize;
            }
        } else if (hdr.type == static_cast<uint16_t>(MsgType::FileDone) && payload.size() >= sizeof(FileDonePayload)) {
            FileDonePayload done{};
            std::memcpy(&done, payload.data(), sizeof(done));
            if (cur.hFile == INVALID_HANDLE_VALUE || done.transferId != cur.transferId) continue;

            CloseHandle(cur.hFile);
            cur.hFile = INVALID_HANDLE_VALUE;

            bool valid = done.ok != 0 &&
                         cur.receivedBytes == cur.expectedSize &&
                         done.totalBytes == cur.expectedSize &&
                         done.totalCrc32 == cur.runningCrc;
            if (valid) {
                std::wstring destPath = ResolveUniqueDestPath(m_downloadDir, cur.safeName);
                if (MoveFileExW(cur.tempPath.c_str(), destPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                    cur.tempPath.clear();
                    cur.transferId = 0;
                    std::lock_guard<std::mutex> lock(m_statsMutex);
                    m_stats.filesReceived++;
                    m_stats.lastEvent = L"Empfangen: " + cur.safeName;
                    Logger::Instance().Info("FILE-RX", L"Received file -> " + destPath);
                    continue;
                }
            }
            abortCurrent();
        }
    }

    abortCurrent();
    m_connected = false;
    m_queueCv.notify_all();
}

} // namespace s2p
