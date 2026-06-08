#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace MmtTlv {
class MmtStream;
class Mpt;
struct MfuData;
}

namespace MmtsRecorder {

enum ActualMode : uint32_t {
    ActualModeDecoded = 1,
    ActualModeRawFallback = 2,
    ActualModeFailed = 3,
};

bool Start(const wchar_t* path, bool overwrite, uint32_t* sessionId);
void Stop(uint32_t sessionId);
bool GetStatus(uint32_t sessionId, uint32_t* actualMode, bool* failed, bool* fallbackUsed);
void WriteDecoded(const uint8_t* data, size_t size);
void MarkDecodeFailure();
void OnVideoData(const MmtTlv::MmtStream& stream, const MmtTlv::MfuData& mfu);
void OnAudioData(const MmtTlv::MmtStream& stream, const MmtTlv::MfuData& mfu);
void OnSubtitleData(const MmtTlv::MmtStream& stream, const MmtTlv::MfuData& mfu);
void OnMpt(const MmtTlv::Mpt& mpt);

} // namespace MmtsRecorder
