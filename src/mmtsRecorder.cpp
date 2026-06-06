#include "mmtsRecorder.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>

namespace MmtsRecorder {
namespace {

struct Session {
    uint32_t id = 0;
    std::filesystem::path path;
    std::filesystem::path rawPath;
    std::ofstream decodedStream;
    std::ofstream rawStream;
    bool failed = false;
    bool fallbackUsed = false;
    bool stopped = false;
};

std::mutex g_mutex;
std::map<uint32_t, std::unique_ptr<Session>> g_sessions;
uint32_t g_nextSessionId = 1;

uint32_t AllocateSessionId()
{
    for (uint32_t i = 0; i < UINT32_MAX; ++i) {
        uint32_t id = g_nextSessionId++;
        if (id == 0) {
            id = g_nextSessionId++;
        }
        if (g_sessions.find(id) == g_sessions.end()) {
            return id;
        }
    }
    return 0;
}

std::filesystem::path MakeRawPath(const std::filesystem::path& path)
{
    std::filesystem::path raw = path;
    std::wstring ext = raw.extension().wstring();
    if (ext.empty()) {
        raw += L".raw";
    } else {
        raw.replace_extension(L".raw" + ext);
    }
    return raw;
}

bool Exists(const std::filesystem::path& path)
{
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

bool PrepareParent(const std::filesystem::path& path)
{
    std::error_code ec;
    std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return false;
        }
    }
    return true;
}

void MarkSessionFailed(Session& session)
{
    session.failed = true;
}

void CloseStream(std::ofstream& stream)
{
    if (stream.is_open()) {
        stream.flush();
        stream.close();
    }
}

void FinishSession(Session& session)
{
    CloseStream(session.decodedStream);
    CloseStream(session.rawStream);

    std::error_code ec;
    if (session.fallbackUsed) {
        std::filesystem::remove(session.path, ec);
        ec.clear();
        std::filesystem::rename(session.rawPath, session.path, ec);
        if (ec) {
            session.failed = true;
        }
    } else {
        std::filesystem::remove(session.rawPath, ec);
    }
    session.stopped = true;
}

} // namespace

bool Start(const wchar_t* path, bool overwrite, uint32_t* sessionId)
{
    if (path == nullptr || path[0] == L'\0' || sessionId == nullptr) {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_mutex);

    std::filesystem::path savePath(path);
    std::filesystem::path rawPath = MakeRawPath(savePath);
    if (!PrepareParent(savePath) || !PrepareParent(rawPath)) {
        return false;
    }
    if (!overwrite && (Exists(savePath) || Exists(rawPath))) {
        return false;
    }

    uint32_t id = AllocateSessionId();
    if (id == 0) {
        return false;
    }

    if (overwrite) {
        std::error_code ec;
        std::filesystem::remove(savePath, ec);
        if (ec) {
            return false;
        }
        ec.clear();
        std::filesystem::remove(rawPath, ec);
        if (ec) {
            return false;
        }
    }

    auto session = std::make_unique<Session>();
    session->id = id;
    session->path = savePath;
    session->rawPath = rawPath;
    session->decodedStream.open(savePath, std::ios::binary);
    session->rawStream.open(rawPath, std::ios::binary);
    if (!session->decodedStream.is_open() || !session->rawStream.is_open()) {
        return false;
    }

    *sessionId = id;
    g_sessions.emplace(id, std::move(session));
    return true;
}

void Stop(uint32_t sessionId)
{
    std::unique_ptr<Session> session;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_sessions.find(sessionId);
        if (it == g_sessions.end()) {
            return;
        }
        session = std::move(it->second);
        g_sessions.erase(it);
    }

    FinishSession(*session);
}

bool GetStatus(uint32_t sessionId, uint32_t* actualMode, bool* failed, bool* fallbackUsed)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_sessions.find(sessionId);
    if (it == g_sessions.end()) {
        return false;
    }

    const Session& session = *it->second;
    if (actualMode) {
        *actualMode = session.failed ? ActualModeFailed :
            session.fallbackUsed ? ActualModeRawFallback : ActualModeDecoded;
    }
    if (failed) {
        *failed = session.failed;
    }
    if (fallbackUsed) {
        *fallbackUsed = session.fallbackUsed;
    }
    return true;
}

void WriteRaw(const uint8_t* data, size_t size)
{
    if (data == nullptr || size == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto& item : g_sessions) {
        Session& session = *item.second;
        session.rawStream.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        if (!session.rawStream) {
            MarkSessionFailed(session);
        }
    }
}

void WriteDecoded(const uint8_t* data, size_t size)
{
    if (data == nullptr || size == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto& item : g_sessions) {
        Session& session = *item.second;
        if (session.fallbackUsed) {
            continue;
        }
        session.decodedStream.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        if (!session.decodedStream) {
            MarkSessionFailed(session);
        }
    }
}

void MarkDecodeFailure()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto& item : g_sessions) {
        item.second->fallbackUsed = true;
    }
}

} // namespace MmtsRecorder
