#pragma once
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "appItemAssembler.h"

namespace MmtTlv {

class Mpt;

// Carries a service's data broadcasting (ARIB STD-B62) over to MPEG-2 TS as
// private sections, so TS-side receivers can run the application. See
// docs/app-data-ts-transport.md for the format.
//
// - MH-AIT, DDMT, DAMT, DCCT and EMT sections are passed through unchanged.
// - Each reassembled aapp item is sent in kItemTableId sections, split by
//   section_number. Items stay as broadcast (no inflate, no path resolution).
//
// Broadcasters repeat these far faster than a receiver needs (BS Fuji 4K sends
// its 835-byte index.html about 200 times a second), so identical content is
// only sent again after an interval of stream time.
class AppDataTransport {
public:
    static constexpr uint8_t kItemTableId = 0xC0;
    static constexpr uint32_t kFormatIdentifier = 0x44344B41; // 'D4KA'
    static constexpr size_t kMaxSectionSize = 4096;
    static constexpr uint64_t kTableResendInterval90k = 90000; // 1 s

    // Receives one complete section (CRC included) for the given service.
    using SectionCallback = std::function<void(uint16_t serviceId, const uint8_t* data, size_t size)>;

    void setSectionCallback(SectionCallback cb) { sectionCallback = std::move(cb); }
    void setItemResendInterval(uint64_t interval90k) { itemResendInterval90k = interval90k; }

    // Learns where the service's application tables and aapp assets are sent.
    void onMpt(uint16_t serviceId, const Mpt& mpt);
    // True once the service's MPT carried an application service descriptor.
    bool hasApplicationService(uint16_t serviceId) const;

    void onTableSection(uint16_t packetId, const uint8_t* data, size_t size, uint64_t now90k);
    void onApplicationData(const MmtStream& mmtStream, const Mpu& mpu, const DataUnit& dataUnit, const MfuData& mfuData, uint64_t now90k);
    void clear();

    // Builds the kItemTableId sections for one item. Empty if it does not fit
    // in 256 sections.
    static std::vector<std::vector<uint8_t>> buildItemSections(const AppItemAssembler::Item& item, uint16_t sequence);

    uint64_t getOversizedItemCount() const { return oversizedItemCount; }
    uint64_t getUnroutedCount() const { return unroutedCount; }

private:
    // Records the send and returns true if this content was not sent within
    // the interval. A stream clock that went backwards counts as elapsed.
    bool shouldSend(uint64_t key, uint64_t now90k, uint64_t interval90k);
    const uint16_t* findService(uint16_t packetId) const;

    SectionCallback sectionCallback;
    uint64_t itemResendInterval90k{10 * 90000};

    // MMT packet_id -> service_id, rebuilt from each MPT.
    std::unordered_map<uint16_t, uint16_t> packetIdToService;
    std::unordered_set<uint16_t> applicationServices;

    // Content key -> stream time it was last sent.
    std::unordered_map<uint64_t, uint64_t> lastSent;

    AppItemAssembler assembler;
    uint16_t itemSequence{};
    uint64_t oversizedItemCount{};
    uint64_t unroutedCount{};
};

}
