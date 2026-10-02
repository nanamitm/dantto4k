#pragma once
#include <filesystem>
#include <cwchar>

// Compare file identity as well as spelling: hard links and relative paths
// must not let an output stream truncate its own input.
inline bool sameFilePath(const std::filesystem::path& first,
                         const std::filesystem::path& second) {
    std::error_code ec;
    if (std::filesystem::equivalent(first, second, ec)) {
        return true;
    }
    const auto a = std::filesystem::weakly_canonical(first);
    const auto b = std::filesystem::weakly_canonical(second);
#ifdef _WIN32
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
#else
    return a == b;
#endif
}
