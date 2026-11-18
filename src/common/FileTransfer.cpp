#include "FileTransfer.h"
#include "Logger.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cwctype>

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

void CreateDirectoryRecursiveW(const std::wstring& path) {
    if (path.empty()) return;
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        return;
    }

    size_t start = 0;
    if (path.size() >= 2 && path[1] == L':') {
        start = 3;
    } else if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') {
        size_t s1 = path.find(L'\\', 2);
        size_t s2 = (s1 != std::wstring::npos) ? path.find(L'\\', s1 + 1) : std::wstring::npos;
        start = (s2 != std::wstring::npos) ? (s2 + 1) : path.size();
    }

    for (size_t i = start; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == L'\\') {
            if (i > start) {
                std::wstring sub = path.substr(0, i);
                if (!sub.empty()) {
                    CreateDirectoryW(sub.c_str(), nullptr);
                }
            }
        }
    }
}

bool IsWindowsReservedName(const std::wstring& segment) {
    if (segment.empty()) return false;
    size_t dot = segment.find(L'.');
    std::wstring stem = (dot == std::wstring::npos) ? segment : segment.substr(0, dot);
    while (!stem.empty() && stem.back() == L' ') {
        stem.pop_back();
    }
    if (stem.empty()) return false;
    for (wchar_t& ch : stem) {
        ch = static_cast<wchar_t>(std::towupper(ch));
    }
    if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL") {
        return true;
    }
    if (stem.size() == 4 &&
        (stem.compare(0, 3, L"COM") == 0 || stem.compare(0, 3, L"LPT") == 0) &&
        stem[3] >= L'1' && stem[3] <= L'9') {
        return true;
    }
    return false;
}

std::wstring SanitizeSegment(const std::wstring& raw, const wchar_t* fallback) {
    std::wstring cleaned;
    cleaned.reserve(raw.size());
    for (wchar_t ch : raw) {
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
        return fallback ? std::wstring(fallback) : std::wstring();
    }
    if (IsWindowsReservedName(cleaned)) {
        cleaned = L"_" + cleaned;
    }
    if (cleaned.size() > 120) {
        cleaned.resize(120);
        while (!cleaned.empty() && (cleaned.back() == L' ' || cleaned.back() == L'.')) {
            cleaned.pop_back();
        }
    }
    return cleaned;
}

void CollectItemsRecursive(const std::wstring& path,
                           const std::wstring& relSubdir,
                           std::vector<SendQueueItem>& outItems,
                           int depth) {
    if (path.empty() || depth > 16 || outItems.size() >= 10000) return;

    std::wstring cleanPath = path;
    while (cleanPath.size() > 3 && (cleanPath.back() == L'\\' || cleanPath.back() == L'/')) {
        cleanPath.pop_back();
    }

    DWORD attr = GetFileAttributesW(cleanPath.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return;
    if (attr & FILE_ATTRIBUTE_REPARSE_POINT) return; // Skip junctions/symlinks to prevent loops

    if ((attr & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        outItems.push_back({ cleanPath, FileTransfer::SanitizeRelativePath(relSubdir) });
        return;
    }

    size_t slash = cleanPath.find_last_of(L"\\/");
    std::wstring rawFolderName = (slash == std::wstring::npos) ? cleanPath : cleanPath.substr(slash + 1);
    if (rawFolderName.size() >= 2 && rawFolderName[1] == L':') {
        rawFolderName = L"Laufwerk_" + std::wstring(1, rawFolderName[0]);
    }
    std::wstring safeFolder = FileTransfer::SanitizeFilename(rawFolderName);
    std::wstring nextRel = relSubdir.empty() ? safeFolder : (relSubdir + L"\\" + safeFolder);
    nextRel = FileTransfer::SanitizeRelativePath(nextRel);
    if (nextRel.empty()) return;

    std::wstring searchPattern = cleanPath;
    if (!searchPattern.empty() && searchPattern.back() != L'\\' && searchPattern.back() != L'/') {
        searchPattern += L'\\';
    }
    searchPattern += L'*';

    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW(searchPattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
            continue;
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            continue;
        }
        std::wstring child = cleanPath;
        if (!child.empty() && child.back() != L'\\') child += L'\\';
        child += fd.cFileName;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            CollectItemsRecursive(child, nextRel, outItems, depth + 1);
        } else {
            outItems.push_back({ child, nextRel });
        }
    } while (FindNextFileW(hFind, &fd) && outItems.size() < 10000);

    FindClose(hFind);
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
    size_t pos = rawName.find_last_of(L"/\\");
    std::wstring name = (pos == std::wstring::npos) ? rawName : rawName.substr(pos + 1);

    if (name.size() >= 2 &&
        ((name[0] >= L'A' && name[0] <= L'Z') || (name[0] >= L'a' && name[0] <= L'z')) &&
        name[1] == L':') {
        name = name.substr(2);
    }

    return SanitizeSegment(name, L"received_file.bin");
}

std::wstring FileTransfer::SanitizeRelativePath(const std::wstring& rawRelPath) {
    if (rawRelPath.empty()) return L"";
    if (rawRelPath.size() >= 2 && rawRelPath[0] == L'\\' && rawRelPath[1] == L'\\') return L"";
    if (rawRelPath.size() >= 2 && rawRelPath[0] == L'/' && rawRelPath[1] == L'/') return L"";
    if (rawRelPath.size() >= 2 &&
        ((rawRelPath[0] >= L'A' && rawRelPath[0] <= L'Z') || (rawRelPath[0] >= L'a' && rawRelPath[0] <= L'z')) &&
        rawRelPath[1] == L':') {
        return L"";
    }

    std::vector<std::wstring> cleanParts;
    size_t i = 0;
    size_t totalLen = 0;
    while (i < rawRelPath.size()) {
        while (i < rawRelPath.size() && (rawRelPath[i] == L'\\' || rawRelPath[i] == L'/')) {
            ++i;
        }
        if (i >= rawRelPath.size()) break;
        size_t j = i;
        while (j < rawRelPath.size() && rawRelPath[j] != L'\\' && rawRelPath[j] != L'/') {
            ++j;
        }
        std::wstring seg = rawRelPath.substr(i, j - i);
        i = j;

        if (seg.empty() || seg == L".") continue;
        if (seg == L"..") return L""; // Strictly reject any path traversal

        std::wstring safeSeg = SanitizeSegment(seg, nullptr);
        if (safeSeg.empty()) continue;

        if (cleanParts.size() >= 16) break;
        size_t added = safeSeg.size() + (cleanParts.empty() ? 0 : 1);
        if (totalLen + added > 240) break;
        totalLen += added;
        cleanParts.push_back(std::move(safeSeg));
    }

    std::wstring out;
    out.reserve(totalLen);
    for (size_t k = 0; k < cleanParts.size(); ++k) {
        if (k > 0) out.push_back(L'\\');
        out += cleanParts[k];
    }
    return out;
}

std::wstring FileTransfer::ResolveUniqueDestPath(const std::wstring& dir, const std::wstring& safeFilename) {
    CreateDirectoryRecursiveW(dir);
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

    std::lock_guard<std::mutex> lifeLock(m_lifecycleMutex);
    m_stop = false;
    m_cancelCurrentSend = false;
    m_cancelCurrentRecv = false;
    m_activeSendId = 0;
    m_activeRecvId = 0;
    m_sock = std::move(sock);
    m_sock.TuneLowLatency(2 * 1024 * 1024, 2 * 1024 * 1024);
    m_sock.SetTimeouts(5000, 1000);

    m_downloadDir = customDownloadDir.empty() ? DefaultDownloadDir() : customDownloadDir;
    CreateDirectoryRecursiveW(m_downloadDir);

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
    m_cancelCurrentSend = true;
    m_cancelCurrentRecv = true;
    m_queueCv.notify_all();
    m_sock.Close();

    std::thread sendToJoin;
    std::thread recvToJoin;
    {
        std::lock_guard<std::mutex> lifeLock(m_lifecycleMutex);
        if (m_sendThread.joinable()) sendToJoin = std::move(m_sendThread);
        if (m_recvThread.joinable()) recvToJoin = std::move(m_recvThread);
    }

    if (sendToJoin.joinable()) {
        if (sendToJoin.get_id() == std::this_thread::get_id()) sendToJoin.detach();
        else sendToJoin.join();
    }
    if (recvToJoin.joinable()) {
        if (recvToJoin.get_id() == std::this_thread::get_id()) recvToJoin.detach();
        else recvToJoin.join();
    }

    {
        std::lock_guard<std::mutex> qLock(m_queueMutex);
        m_sendQueue.clear();
    }
    {
        std::lock_guard<std::mutex> sLock(m_statsMutex);
        m_stats.active = false;
        m_stats.transferring = false;
        m_stats.queuedFiles = 0;
        m_stats.currentSpeedMBps = 0.0;
    }
}

size_t FileTransfer::QueueFiles(const std::vector<std::wstring>& paths) {
    return QueuePaths(paths);
}

size_t FileTransfer::QueuePaths(const std::vector<std::wstring>& paths) {
    if (!m_connected.load() || paths.empty()) return 0;

    std::vector<SendQueueItem> collected;
    for (const auto& p : paths) {
        CollectItemsRecursive(p, L"", collected, 0);
    }
    if (collected.empty()) return 0;

    size_t totalQueued = 0;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        for (auto& item : collected) {
            m_sendQueue.push_back(std::move(item));
        }
        totalQueued = m_sendQueue.size();
    }
    {
        std::lock_guard<std::mutex> sLock(m_statsMutex);
        m_stats.queuedFiles = static_cast<uint32_t>(totalQueued);
    }
    m_queueCv.notify_one();
    return collected.size();
}

void FileTransfer::CancelCurrent() {
    uint32_t sendId = 0;
    uint32_t recvId = m_activeRecvId.load();
    {
        std::lock_guard<std::mutex> qLock(m_queueMutex);
        m_sendQueue.clear();
        sendId = m_activeSendId.load();
        if (sendId != 0) {
            m_cancelCurrentSend = true;
        }
    }
    if (recvId != 0) {
        m_cancelCurrentRecv = true;
    }
    {
        std::lock_guard<std::mutex> sLock(m_statsMutex);
        m_stats.queuedFiles = 0;
        m_stats.lastEvent = L"Uebertragung abgebrochen";
    }

    if (m_connected.load()) {
        std::lock_guard<std::mutex> sockLock(m_sendSockMutex);
        if (m_sock.IsValid()) {
            if (sendId != 0) {
                FileCancelPayload fc{ sendId, 1 };
                m_sock.SendPacket(MsgType::FileCancel, &fc, sizeof(fc));
            }
            if (recvId != 0) {
                FileCancelPayload fc{ recvId, 2 };
                m_sock.SendPacket(MsgType::FileCancel, &fc, sizeof(fc));
            }
        }
    }
}

FileTransferStats FileTransfer::GetStats() const {
    std::lock_guard<std::mutex> lock(m_statsMutex);
    return m_stats;
}

void FileTransfer::SendWorker() {
    while (!m_stop && m_connected.load()) {
        SendQueueItem nextItem;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_queueCv.wait(lock, [this]() {
                return m_stop.load() || !m_connected.load() || !m_sendQueue.empty();
            });
            if (m_stop || !m_connected.load()) break;
            nextItem = std::move(m_sendQueue.front());
            m_sendQueue.pop_front();
            m_cancelCurrentSend = false;
            m_activeSendId = m_nextId.fetch_add(1);
            std::lock_guard<std::mutex> sLock(m_statsMutex);
            m_stats.queuedFiles = static_cast<uint32_t>(m_sendQueue.size());
        }
        if (!SendSingleFile(nextItem)) {
            if (!m_cancelCurrentSend.load() && !m_stop) {
                Logger::Instance().Warn("FILE-TX", L"Failed sending file: " + nextItem.fullPath);
            }
        }
        m_activeSendId = 0;
        m_cancelCurrentSend = false;
    }
}

bool FileTransfer::SendSingleFile(const SendQueueItem& item) {
    const uint32_t transferId = m_activeSendId.load();
    HANDLE hFile = CreateFileW(item.fullPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
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
    std::wstring baseName = SanitizeFilename(item.fullPath);
    std::wstring safeRel = SanitizeRelativePath(item.relSubdir);
    std::string utf8Name = WideToUtf8(baseName);
    std::string utf8Rel = WideToUtf8(safeRel);
    if (utf8Name.empty() || utf8Name.size() > 1024 || utf8Rel.size() > 2048) {
        CloseHandle(hFile);
        return false;
    }

    // 1. Send FileMeta
    {
        FileMetaHeader meta{};
        meta.transferId = transferId;
        meta.fileSize = fileSize;
        meta.nameUtf8Bytes = static_cast<uint32_t>(utf8Name.size());
        meta.relPathUtf8Bytes = static_cast<uint32_t>(utf8Rel.size());

        std::vector<uint8_t> pkt(sizeof(meta) + utf8Name.size() + utf8Rel.size());
        std::memcpy(pkt.data(), &meta, sizeof(meta));
        std::memcpy(pkt.data() + sizeof(meta), utf8Name.data(), utf8Name.size());
        if (!utf8Rel.empty()) {
            std::memcpy(pkt.data() + sizeof(meta) + utf8Name.size(), utf8Rel.data(), utf8Rel.size());
        }

        std::lock_guard<std::mutex> sockLock(m_sendSockMutex);
        if (!m_sock.SendPacket(MsgType::FileMeta, pkt.data(), static_cast<uint32_t>(pkt.size()))) {
            CloseHandle(hFile);
            m_connected = false;
            return false;
        }
    }

    std::wstring displayName = safeRel.empty() ? baseName : (safeRel + L"\\" + baseName);
    {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        m_stats.transferring = true;
        m_stats.isSending = true;
        m_stats.currentFileBytes = 0;
        m_stats.currentFileTotal = fileSize;
        m_stats.currentSpeedMBps = 0.0;
        m_stats.currentFileName = displayName;
        m_stats.lastEvent = L"Sende: " + displayName + L"...";
    }

    // 2. Stream chunks
    std::vector<uint8_t> ioBuf(kFileChunkSize);
    std::vector<uint8_t> pkt(sizeof(FileChunkHeader) + kFileChunkSize);
    uint64_t offset = 0;
    uint32_t totalCrc = 0;
    bool readErr = false;
    bool canceled = false;

    auto speedWinStart = std::chrono::steady_clock::now();
    uint64_t speedWinBytes = 0;
    double curSpeedMBps = 0.0;

    while (!m_stop && offset < fileSize) {
        if (m_cancelCurrentSend.load()) {
            canceled = true;
            break;
        }

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

        {
            std::lock_guard<std::mutex> sockLock(m_sendSockMutex);
            if (!m_sock.SendPacket(MsgType::FileChunk, pkt.data(), static_cast<uint32_t>(pkt.size()))) {
                CloseHandle(hFile);
                m_connected = false;
                std::lock_guard<std::mutex> sLock(m_statsMutex);
                m_stats.transferring = false;
                m_stats.currentSpeedMBps = 0.0;
                return false;
            }
        }

        offset += got;
        speedWinBytes += got;

        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - speedWinStart).count();
        if (dt >= 0.25) {
            curSpeedMBps = (static_cast<double>(speedWinBytes) / (1024.0 * 1024.0)) / dt;
            speedWinStart = now;
            speedWinBytes = 0;
        }

        std::lock_guard<std::mutex> lock(m_statsMutex);
        m_stats.bytesSent += got;
        m_stats.currentFileBytes = offset;
        if (curSpeedMBps > 0.0) {
            m_stats.currentSpeedMBps = curSpeedMBps;
        }
    }

    CloseHandle(hFile);

    if (m_cancelCurrentSend.load()) {
        canceled = true;
    }

    // 3. Send FileDone (ok=0 on cancel/readErr so peer immediately cleans up .part_)
    FileDonePayload done{};
    done.transferId = transferId;
    done.totalBytes = offset;
    done.totalCrc32 = totalCrc;
    done.ok = (!canceled && !readErr && offset == fileSize) ? 1 : 0;

    {
        std::lock_guard<std::mutex> sockLock(m_sendSockMutex);
        if (!m_sock.SendPacket(MsgType::FileDone, &done, sizeof(done))) {
            m_connected = false;
            std::lock_guard<std::mutex> lock(m_statsMutex);
            m_stats.transferring = false;
            m_stats.currentSpeedMBps = 0.0;
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        m_stats.transferring = false;
        m_stats.currentSpeedMBps = 0.0;
        m_stats.currentFileBytes = 0;
        m_stats.currentFileTotal = 0;
        m_stats.currentFileName.clear();
        if (done.ok) {
            m_stats.filesSent++;
            m_stats.lastEvent = L"Gesendet: " + displayName;
            Logger::Instance().Info("FILE-TX", L"Sent file '" + displayName + L"' (" + std::to_wstring(fileSize) + L" bytes)");
        } else if (canceled) {
            m_stats.lastEvent = L"Abgebrochen: " + displayName;
        }
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
        std::wstring relSubdir;
        std::wstring displayName;
        std::wstring targetDir;
        std::wstring tempPath;
        HANDLE hFile = INVALID_HANDLE_VALUE;
        std::chrono::steady_clock::time_point speedWinStart{};
        uint64_t speedWinBytes = 0;
    } cur;

    auto abortCurrent = [&](const std::wstring& reason) {
        if (cur.hFile != INVALID_HANDLE_VALUE) {
            CloseHandle(cur.hFile);
            cur.hFile = INVALID_HANDLE_VALUE;
        }
        if (!cur.tempPath.empty()) {
            DeleteFileW(cur.tempPath.c_str());
            cur.tempPath.clear();
        }
        bool wasActive = (cur.transferId != 0);
        cur.transferId = 0;
        m_activeRecvId = 0;
        if (wasActive || !reason.empty()) {
            std::lock_guard<std::mutex> lock(m_statsMutex);
            if (!m_stats.isSending || !m_stats.transferring) {
                m_stats.transferring = false;
                m_stats.currentFileBytes = 0;
                m_stats.currentFileTotal = 0;
                m_stats.currentSpeedMBps = 0.0;
                m_stats.currentFileName.clear();
            }
            if (!reason.empty()) {
                m_stats.lastEvent = reason;
            }
        }
    };

    while (!m_stop && m_sock.IsValid()) {
        if (m_cancelCurrentRecv.exchange(false) && cur.transferId != 0) {
            abortCurrent(L"Empfang abgebrochen: " + cur.displayName);
        }

        int wr = m_sock.WaitReadable(50);
        if (wr < 0) break;
        if (wr == 0) continue;

        PacketHeader hdr{};
        std::vector<uint8_t> payload;
        if (!m_sock.RecvPacket(hdr, payload)) {
            int err = WSAGetLastError();
            if (err == WSAETIMEDOUT) continue;
            break;
        }

        if (hdr.type == static_cast<uint16_t>(MsgType::FileMeta) && payload.size() >= sizeof(FileMetaHeader)) {
            abortCurrent(L"");
            m_cancelCurrentRecv = false;

            FileMetaHeader meta{};
            std::memcpy(&meta, payload.data(), sizeof(meta));
            if (meta.fileSize > kMaxFileTransferBytes || meta.nameUtf8Bytes == 0 ||
                meta.nameUtf8Bytes > 1024 || meta.relPathUtf8Bytes > 2048 ||
                payload.size() < sizeof(meta) + meta.nameUtf8Bytes + meta.relPathUtf8Bytes) {
                continue;
            }
            std::string utf8Name(reinterpret_cast<const char*>(payload.data() + sizeof(meta)), meta.nameUtf8Bytes);
            std::string utf8Rel;
            if (meta.relPathUtf8Bytes > 0) {
                utf8Rel.assign(reinterpret_cast<const char*>(payload.data() + sizeof(meta) + meta.nameUtf8Bytes),
                               meta.relPathUtf8Bytes);
            }

            cur.safeName = SanitizeFilename(Utf8ToWide(utf8Name));
            cur.relSubdir = SanitizeRelativePath(Utf8ToWide(utf8Rel));
            cur.displayName = cur.relSubdir.empty() ? cur.safeName : (cur.relSubdir + L"\\" + cur.safeName);
            cur.targetDir = cur.relSubdir.empty() ? m_downloadDir : (m_downloadDir + L"\\" + cur.relSubdir);
            CreateDirectoryRecursiveW(cur.targetDir);

            cur.transferId = meta.transferId;
            cur.expectedSize = meta.fileSize;
            cur.receivedBytes = 0;
            cur.runningCrc = 0;
            cur.tempPath = cur.targetDir + L"\\.part_" + std::to_wstring(meta.transferId) + L"_" + cur.safeName;
            cur.speedWinStart = std::chrono::steady_clock::now();
            cur.speedWinBytes = 0;

            cur.hFile = CreateFileW(cur.tempPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (cur.hFile == INVALID_HANDLE_VALUE) {
                cur.tempPath.clear();
                cur.transferId = 0;
                m_activeRecvId = 0;
            } else {
                m_activeRecvId = meta.transferId;
                std::lock_guard<std::mutex> lock(m_statsMutex);
                m_stats.transferring = true;
                m_stats.isSending = false;
                m_stats.currentFileBytes = 0;
                m_stats.currentFileTotal = cur.expectedSize;
                m_stats.currentSpeedMBps = 0.0;
                m_stats.currentFileName = cur.displayName;
                m_stats.lastEvent = L"Empfange: " + cur.displayName + L"...";
            }
        } else if (hdr.type == static_cast<uint16_t>(MsgType::FileChunk) && payload.size() >= sizeof(FileChunkHeader)) {
            FileChunkHeader ch{};
            std::memcpy(&ch, payload.data(), sizeof(ch));
            if (cur.hFile == INVALID_HANDLE_VALUE || ch.transferId != cur.transferId) continue;
            if (ch.chunkSize > kFileChunkSize || payload.size() < sizeof(ch) + ch.chunkSize) {
                abortCurrent(L"Ungueltiger Datei-Chunk");
                continue;
            }
            if (ch.offset != cur.receivedBytes || cur.receivedBytes + ch.chunkSize > cur.expectedSize) {
                abortCurrent(L"Ungueltiger Datei-Offset");
                continue;
            }

            const uint8_t* bytes = payload.data() + sizeof(ch);
            if (ComputeCrc32(bytes, ch.chunkSize) != ch.crc32) {
                Logger::Instance().Warn("FILE-RX", L"Chunk CRC32 mismatch for " + cur.displayName);
                abortCurrent(L"Pruefsummenfehler bei " + cur.displayName);
                continue;
            }

            DWORD written = 0;
            if (!WriteFile(cur.hFile, bytes, ch.chunkSize, &written, nullptr) || written != ch.chunkSize) {
                abortCurrent(L"Schreibfehler bei " + cur.displayName);
                continue;
            }

            cur.runningCrc = ComputeCrc32(bytes, ch.chunkSize, cur.runningCrc);
            cur.receivedBytes += ch.chunkSize;
            cur.speedWinBytes += ch.chunkSize;

            double curSpeedMBps = 0.0;
            auto now = std::chrono::steady_clock::now();
            double dt = std::chrono::duration<double>(now - cur.speedWinStart).count();
            if (dt >= 0.25) {
                curSpeedMBps = (static_cast<double>(cur.speedWinBytes) / (1024.0 * 1024.0)) / dt;
                cur.speedWinStart = now;
                cur.speedWinBytes = 0;
            }

            {
                std::lock_guard<std::mutex> lock(m_statsMutex);
                m_stats.bytesReceived += ch.chunkSize;
                m_stats.currentFileBytes = cur.receivedBytes;
                if (curSpeedMBps > 0.0) {
                    m_stats.currentSpeedMBps = curSpeedMBps;
                }
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
                std::wstring destPath = ResolveUniqueDestPath(cur.targetDir, cur.safeName);
                if (MoveFileExW(cur.tempPath.c_str(), destPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                    cur.tempPath.clear();
                    cur.transferId = 0;
                    m_activeRecvId = 0;
                    std::lock_guard<std::mutex> lock(m_statsMutex);
                    m_stats.filesReceived++;
                    m_stats.transferring = false;
                    m_stats.currentSpeedMBps = 0.0;
                    m_stats.currentFileBytes = 0;
                    m_stats.currentFileTotal = 0;
                    m_stats.currentFileName.clear();
                    m_stats.lastEvent = L"Empfangen: " + cur.displayName;
                    Logger::Instance().Info("FILE-RX", L"Received file -> " + destPath);
                    continue;
                }
            }
            abortCurrent(done.ok == 0 ? (L"Abgebrochen: " + cur.displayName) : (L"Fehler bei: " + cur.displayName));
        } else if (hdr.type == static_cast<uint16_t>(MsgType::FileCancel) && payload.size() >= sizeof(FileCancelPayload)) {
            FileCancelPayload fc{};
            std::memcpy(&fc, payload.data(), sizeof(fc));
            if (fc.transferId != 0 && fc.transferId == cur.transferId) {
                abortCurrent(L"Partner hat abgebrochen: " + cur.displayName);
            } else if (fc.transferId != 0 && fc.transferId == m_activeSendId.load()) {
                m_cancelCurrentSend = true;
            }
        }
    }

    abortCurrent(L"");
    m_connected = false;
    {
        std::lock_guard<std::mutex> lock(m_statsMutex);
        m_stats.active = false;
        m_stats.transferring = false;
        m_stats.currentSpeedMBps = 0.0;
    }
    m_queueCv.notify_all();
}

} // namespace s2p
