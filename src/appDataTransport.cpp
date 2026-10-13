#include "appDataTransport.h"
#include <algorithm>
#include <string_view>
#include "applicationServiceDescriptor.h"
#include "mmtStream.h"
#include "mpt.h"
#include "mpuProcessorBase.h"

namespace MmtTlv {

namespace {

// Size of component_tag, mpu_sequence_number, item_id and item_size at the
// start of the first item section.
constexpr size_t kItemHeaderSize = 2 + 4 + 4 + 4;
// table_id .. last_section_number, and CRC_32.
constexpr size_t kSectionOverhead = 8 + 4;
constexpr size_t kMaxSectionPayload = AppDataTransport::kMaxSectionSize - kSectionOverhead;
constexpr size_t kMaxSectionsPerItem = 256;
constexpr size_t kMaxRememberedSends = 8192;

uint32_t crc32Mpeg2(const uint8_t* data, size_t size) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < size; i++) {
        crc ^= static_cast<uint32_t>(data[i]) << 24;
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80000000) ? (crc << 1) ^ 0x04C11DB7 : (crc << 1);
        }
    }
    return crc;
}

uint64_t hashBytes(const uint8_t* data, size_t size) {
    return std::hash<std::string_view>{}(std::string_view(reinterpret_cast<const char*>(data), size));
}

uint64_t mix(uint64_t seed, uint64_t value) {
    return seed ^ (value + 0x9E3779B97F4A7C15ULL + (seed << 6) + (seed >> 2));
}

void putBe16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

void putBe32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v >> 24));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

} // namespace

void AppDataTransport::onMpt(uint16_t serviceId, const Mpt& mpt) {
    std::erase_if(packetIdToService, [&](const auto& entry) { return entry.second == serviceId; });
    applicationServices.erase(serviceId);

    auto route = [&](const MmtGeneralLocationInfo& locationInfo) {
        if (locationInfo.locationType == 0) {
            packetIdToService[locationInfo.packetId] = serviceId;
        }
    };

    for (const auto& descriptor : mpt.descriptors.list) {
        const auto* applicationService = dynamic_cast<const ApplicationServiceDescriptor*>(descriptor.get());
        if (!applicationService) {
            continue;
        }
        applicationServices.insert(serviceId);
        route(applicationService->aitLocationInfo);
        if (applicationService->dtMessageLocationInfo) {
            route(*applicationService->dtMessageLocationInfo);
        }
        for (const auto& emt : applicationService->emts) {
            route(emt.locationInfo);
        }
    }

    for (const auto& asset : mpt.assets) {
        if (asset.assetType != static_cast<uint32_t>(AssetType::aapp)) {
            continue;
        }
        for (const auto& locationInfo : asset.locationInfos) {
            route(locationInfo);
        }
    }
}

bool AppDataTransport::hasApplicationService(uint16_t serviceId) const {
    return applicationServices.contains(serviceId);
}

const uint16_t* AppDataTransport::findService(uint16_t packetId) const {
    auto it = packetIdToService.find(packetId);
    return it == packetIdToService.end() ? nullptr : &it->second;
}

void AppDataTransport::onTableSection(uint16_t packetId, const uint8_t* data, size_t size, uint64_t now90k) {
    if (!sectionCallback || size < 3 || size > kMaxSectionSize) {
        return;
    }
    const uint16_t* serviceId = findService(packetId);
    if (!serviceId) {
        unroutedCount++;
        return;
    }

    const uint64_t key = mix(mix(hashBytes(data, size), size), *serviceId);
    if (!shouldSend(key, now90k, kTableResendInterval90k)) {
        return;
    }
    sectionCallback(*serviceId, data, size);
}

void AppDataTransport::onApplicationData(const MmtStream& mmtStream, const Mpu& mpu, const DataUnit& dataUnit, const MfuData& mfuData, uint64_t now90k) {
    auto item = assembler.push(mmtStream, mpu, dataUnit, mfuData);
    if (!item || !sectionCallback) {
        return;
    }
    const uint16_t* serviceId = findService(item->packetId);
    if (!serviceId) {
        unroutedCount++;
        return;
    }

    uint64_t key = mix(hashBytes(item->data.data(), item->data.size()), item->data.size());
    key = mix(key, *serviceId);
    key = mix(key, static_cast<uint32_t>(item->componentTag));
    key = mix(key, item->mpuSequenceNumber);
    key = mix(key, item->itemId);
    if (!shouldSend(key, now90k, itemResendInterval90k)) {
        return;
    }

    const auto sections = buildItemSections(*item, itemSequence);
    if (sections.empty()) {
        oversizedItemCount++;
        return;
    }
    itemSequence++;
    for (const auto& section : sections) {
        sectionCallback(*serviceId, section.data(), section.size());
    }
}

std::vector<std::vector<uint8_t>> AppDataTransport::buildItemSections(const AppItemAssembler::Item& item, uint16_t sequence) {
    const size_t total = kItemHeaderSize + item.data.size();
    const size_t sectionCount = std::max<size_t>(1, (total + kMaxSectionPayload - 1) / kMaxSectionPayload);
    if (sectionCount > kMaxSectionsPerItem) {
        return {};
    }

    std::vector<std::vector<uint8_t>> sections;
    sections.reserve(sectionCount);
    size_t offset = 0;
    for (size_t number = 0; number < sectionCount; number++) {
        std::vector<uint8_t> section;
        section.reserve(kMaxSectionSize);

        size_t payloadSize = std::min(kMaxSectionPayload, total - number * kMaxSectionPayload);
        const size_t sectionLength = 5 + payloadSize + 4;

        section.push_back(kItemTableId);
        // section_syntax_indicator, private_indicator, reserved, section_length
        putBe16(section, static_cast<uint16_t>(0xF000 | sectionLength));
        putBe16(section, sequence);
        // reserved, version_number, current_next_indicator
        section.push_back(static_cast<uint8_t>(0xC0 | ((sequence & 0x1F) << 1) | 0x01));
        section.push_back(static_cast<uint8_t>(number));
        section.push_back(static_cast<uint8_t>(sectionCount - 1));

        if (number == 0) {
            putBe16(section, static_cast<uint16_t>(item.componentTag));
            putBe32(section, item.mpuSequenceNumber);
            putBe32(section, item.itemId);
            putBe32(section, static_cast<uint32_t>(item.data.size()));
            payloadSize -= kItemHeaderSize;
        }
        section.insert(section.end(), item.data.begin() + offset, item.data.begin() + offset + payloadSize);
        offset += payloadSize;

        putBe32(section, crc32Mpeg2(section.data(), section.size()));
        sections.push_back(std::move(section));
    }
    return sections;
}

bool AppDataTransport::shouldSend(uint64_t key, uint64_t now90k, uint64_t interval90k) {
    auto it = lastSent.find(key);
    if (it != lastSent.end() && now90k >= it->second && now90k - it->second < interval90k) {
        return false;
    }

    if (it == lastSent.end() && lastSent.size() >= kMaxRememberedSends) {
        // Forget content that would be due for a resend anyway.
        const uint64_t horizon = std::max(itemResendInterval90k, kTableResendInterval90k);
        std::erase_if(lastSent, [&](const auto& entry) {
            return now90k < entry.second || now90k - entry.second >= horizon;
        });
        if (lastSent.size() >= kMaxRememberedSends) {
            lastSent.clear();
        }
    }
    lastSent[key] = now90k;
    return true;
}

void AppDataTransport::clear() {
    packetIdToService.clear();
    applicationServices.clear();
    lastSent.clear();
    assembler.clear();
}

}
