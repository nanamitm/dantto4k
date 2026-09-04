#include "tsduckNames.h"

#ifdef _WIN32

#include "resource.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace {

// FNV-1a. The directory is named after the contents, so a binary carrying a
// different name table cannot read one left behind by another build, and two
// versions can run side by side without fighting over the same file.
uint64_t contentHash(const void* data, size_t size)
{
    uint64_t hash = 0xcbf29ce484222325ull;
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash = (hash ^ bytes[i]) * 0x100000001b3ull;
    }
    return hash;
}

// The resource lives in whichever binary this code was linked into, which is
// the executable for dantto4k and the DLL for the BonDriver.
bool findEmbeddedNames(const void*& data, size_t& size)
{
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&contentHash), &module)) {
        return false;
    }

    // RT_RCDATA is an integer identifier dressed as a narrow string, which the
    // wide entry point wants dressed as a wide one. Both projects build without
    // UNICODE, so it needs the cast.
    const HRSRC found = FindResourceW(module, MAKEINTRESOURCEW(IDR_TSDUCK_DTV_NAMES),
                                      reinterpret_cast<LPCWSTR>(RT_RCDATA));
    if (!found) {
        return false;
    }
    const DWORD bytes = SizeofResource(module, found);
    const HGLOBAL loaded = LoadResource(module, found);
    if (bytes == 0 || !loaded) {
        return false;
    }
    const void* locked = LockResource(loaded);
    if (!locked) {
        return false;
    }

    data = locked;
    size = bytes;
    return true;
}

std::wstring temporaryDirectory()
{
    std::vector<wchar_t> buffer(MAX_PATH + 1);
    const DWORD length = GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    return std::wstring(buffer.data(), length);
}

bool fileHasSize(const std::wstring& path, uint64_t size)
{
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) {
        return false;
    }
    const uint64_t actual = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    return actual == size;
}

// Written under a private name and renamed over the target, so a second
// dantto4k starting at the same moment cannot read a half-written file.
bool writeAtomically(const std::wstring& path, const void* data, size_t size)
{
    const std::wstring temporary = path + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    const HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD written = 0;
    const bool ok = WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr) != FALSE &&
                    written == size;
    CloseHandle(file);

    if (!ok || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

// Appended rather than prepended: a TSPLUGINS_PATH the user set on purpose
// keeps its priority over the copy we carry.
void appendToSearchPath(const std::wstring& directory)
{
    std::wstring value;
    const DWORD needed = GetEnvironmentVariableW(L"TSPLUGINS_PATH", nullptr, 0);
    if (needed > 0) {
        value.resize(needed);
        value.resize(GetEnvironmentVariableW(L"TSPLUGINS_PATH", value.data(), needed));
    }
    if (!value.empty()) {
        value += L';';
    }
    value += directory;
    SetEnvironmentVariableW(L"TSPLUGINS_PATH", value.c_str());
}

// Every step is allowed to fail quietly. Losing the name table costs nothing
// but the stderr complaint this exists to avoid, and a read-only or full
// temporary directory is not a reason to refuse to convert a stream.
void unpackNames()
{
    const void* data = nullptr;
    size_t size = 0;
    if (!findEmbeddedNames(data, size)) {
        return;
    }

    const std::wstring temporary = temporaryDirectory();
    if (temporary.empty()) {
        return;
    }

    wchar_t suffix[17] = {};
    swprintf_s(suffix, L"%016llx", static_cast<unsigned long long>(contentHash(data, size)));

    const std::wstring directory = temporary + L"dantto4k-tsduck-" + suffix;
    if (!CreateDirectoryW(directory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        return;
    }

    const std::wstring path = directory + L"\\tsduck.dtv.names";
    if (!fileHasSize(path, size) && !writeAtomically(path, data, size)) {
        return;
    }

    appendToSearchPath(directory);
}

} // namespace

void ensureTsduckNamesAvailable()
{
    static std::once_flag once;
    std::call_once(once, unpackNames);
}

#ifndef DANTTO4KDLL_EXPORTS

// The first statement of main() is too late: TSDuck asks for the name table
// from its own static initialization, so the executable had already printed the
// error twice by the time it got there. init_seg(lib) puts the constructor
// below in the CRT slot reserved for libraries, which runs ahead of the one
// ordinary globals - TSDuck's included - are initialized from.
//
// The DLL is deliberately left out of this. Its static initializers run under
// the loader lock, where touching the file system can deadlock, so the
// BonDriver unpacks from its entry point instead and accepts that TSDuck may
// already have complained to a stderr nobody is reading.
#pragma warning(push)
#pragma warning(disable : 4073) // initializers put in library initialization area
#pragma init_seg(lib)
#pragma warning(pop)

namespace {
struct TsduckNamesInitializer {
    TsduckNamesInitializer() { ensureTsduckNamesAvailable(); }
};
const TsduckNamesInitializer tsduckNamesInitializer;
} // namespace

#endif // DANTTO4KDLL_EXPORTS

#else

void ensureTsduckNamesAvailable()
{
}

#endif
