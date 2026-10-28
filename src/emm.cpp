#include "emm.h"

namespace MmtTlv {

bool Emm::unpack(Common::ReadStream& stream) {
    messages.clear();
    currentNextIndicator = false;
    try {
        // Validate the entire section before publishing any of its messages.
        if (stream.leftBytes() < 3) return false;
        Common::ReadStream probe(stream);
        if (probe.get8U() != MmtTableId::Emm_0) return false;
        const auto flags = probe.getBe16U();
        const size_t length = flags & 0x0fff;
        if (!(flags & 0x8000) || length < 9 || length > 4093 || length > probe.leftBytes()) return false;
        std::vector<uint8_t> section(length + 3);
        stream.read(section.data(), section.size());
        uint32_t crc = 0xffffffff;
        for (const auto byte : section) {
            crc ^= static_cast<uint32_t>(byte) << 24;
            for (int bit = 0; bit < 8; ++bit) {
                crc = (crc << 1) ^ ((crc & 0x80000000) ? 0x04c11db7u : 0);
            }
        }
        if (crc != 0 || section[6] > section[7]) return false;
        std::vector<std::vector<uint8_t>> parsed;
        const size_t end = section.size() - 4;
        for (size_t pos = 8; pos < end;) {
            if (end - pos < 9) return false;
            const size_t size = 7 + section[pos + 6];
            // The card's short APDU accepts at most 255 bytes, including the ID.
            if (size < 9 || size > 255 || size > end - pos) return false;
            parsed.emplace_back(section.begin() + pos, section.begin() + pos + size);
            pos += size;
        }
        tableId = MmtTableId::Emm_0;
        currentNextIndicator = (section[5] & 1) != 0;
        messages = std::move(parsed);
        return true;
    } catch (const std::out_of_range&) {
        return false;
    }
}

}
