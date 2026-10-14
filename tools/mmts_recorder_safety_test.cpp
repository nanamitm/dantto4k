#include "mmtsRecorder.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <stdexcept>
#include <vector>

void subtitleDebugLog(const std::string&) {}
static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
static std::vector<uint8_t> read(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
int main() {
    const auto root = std::filesystem::temp_directory_path() /
        (L"dantto4k-recorder-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directory(root);
    uint32_t first = 0, second = 0;
    try {
        const auto media = root / L"record.mmts";
        const auto map = root / L"record.mmtsmap";
        require(MmtsRecorder::Start(media.c_str(), true, &first), "first recording failed");
        const auto initialMap = read(map);
        uint32_t rejected = 12345;
        require(!MmtsRecorder::Start(map.c_str(), true, &rejected), "media must not replace an active map");
        require(rejected == 12345 && read(map) == initialMap, "rejected start must preserve output and session ID");
        require(!MmtsRecorder::Start(media.c_str(), true, &rejected), "duplicate media must be rejected");
        require(!MmtsRecorder::Start((root / L"RECORD.MMTS").c_str(), true, &rejected), "case alias must be rejected");
        const auto alias = root / L"alias.mmts";
        std::filesystem::create_hard_link(map, alias);
        require(!MmtsRecorder::Start(alias.c_str(), true, &rejected), "hard link to active map must be rejected");
        require(MmtsRecorder::Start((root / L"other.mmts").c_str(), true, &second), "independent recording failed");
        const uint8_t bytes[] = {0x7f, 0xff, 0, 0};
        MmtsRecorder::WriteDecoded(bytes, sizeof(bytes));
        MmtsRecorder::Stop(second); second = 0;
        MmtsRecorder::Stop(first); first = 0;
        require(read(media) == std::vector<uint8_t>(bytes, bytes + sizeof(bytes)), "first recording corrupted");
        require(read(root / L"other.mmts") == read(media), "independent recording corrupted");
        require(MmtsRecorder::Start(map.c_str(), true, &first), "reuse after stop failed");
        require(!MmtsRecorder::Start(media.c_str(), true, &rejected), "map must not replace active media");
        MmtsRecorder::Stop(first); first = 0;
        for (const auto& entry : std::filesystem::directory_iterator(root)) std::filesystem::remove(entry.path());
        std::filesystem::remove(root);
        std::puts("PASS: active media/map collisions, case and hard-link aliases, independent recording and stopped-path reuse");
        return 0;
    } catch (const std::exception& e) {
        MmtsRecorder::Stop(first); MmtsRecorder::Stop(second);
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
