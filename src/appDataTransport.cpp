#include "appDataTransport.h"
#include <algorithm>
#include <string_view>
#include "applicationServiceDescriptor.h"
#include "mmtStream.h"
#include "mpt.h"
#include "mpuProcessorBase.h"

namespace MmtTlv {

namespace {

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
    std::erase_if(packetIdToEmtTag, [&](const auto& entry) { return !packetIdToService.contains(entry.first); });
    applicationServices.erase(serviceId);
    captionServices.erase(serviceId);

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
            if (emt.locationInfo.locationType == 0) {
                packetIdToEmtTag[emt.locationInfo.packetId] = emt.emtTag;
            }
        }
    }

    // Applications get the service's captions as well (addCaptionListener),
    // and receivers may show them as they are. Captions are sent for services
    // without applications too.
    for (const auto& asset : mpt.assets) {
        if (asset.assetType != static_cast<uint32_t>(AssetType::aapp) &&
            asset.assetType != static_cast<uint32_t>(AssetType::stpp)) {
            continue;
        }
        if (asset.assetType == static_cast<uint32_t>(AssetType::stpp)) {
            captionServices.insert(serviceId);
        }
        for (const auto& locationInfo : asset.locationInfos) {
            route(locationInfo);
        }
    }
}

bool AppDataTransport::hasApplicationService(uint16_t serviceId) const {
    return applicationServices.contains(serviceId);
}

bool AppDataTransport::carriesAppData(uint16_t serviceId) const {
    return applicationServices.contains(serviceId) || captionServices.contains(serviceId);
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

    // An EMT on its own does not say which EMT it is; applications select
    // event messages by that tag (event_message_tag), which only the MPT
    // carries. Send EMTs wrapped with their tag instead.
    if (data[0] == kEmtTableId) {
        auto tag = packetIdToEmtTag.find(packetId);
        if (tag == packetIdToEmtTag.end()) {
            unroutedCount++;
            return;
        }
        const std::vector<uint8_t> header = { tag->second };
        const std::vector<uint8_t> emt(data, data + size);
        for (const auto& section : buildSections(kTaggedEmtTableId, header, emt, emtSequence++)) {
            sectionCallback(*serviceId, section.data(), section.size());
        }
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

void AppDataTransport::onCaption(uint16_t packetId, const Caption& caption, const std::vector<uint8_t>& data) {
    if (!sectionCallback) {
        return;
    }
    const uint16_t* serviceId = findService(packetId);
    if (!serviceId || !captionServices.contains(*serviceId)) {
        unroutedCount++;
        return;
    }
    const auto sections = buildCaptionSections(caption, data, captionSequence);
    if (sections.empty()) {
        oversizedItemCount++;
        return;
    }
    captionSequence++;
    for (const auto& section : sections) {
        sectionCallback(*serviceId, section.data(), section.size());
    }
}

std::vector<std::vector<uint8_t>> AppDataTransport::buildItemSections(const AppItemAssembler::Item& item, uint16_t sequence) {
    std::vector<uint8_t> header;
    putBe16(header, static_cast<uint16_t>(item.componentTag));
    putBe32(header, item.mpuSequenceNumber);
    putBe32(header, item.itemId);
    putBe32(header, static_cast<uint32_t>(item.data.size()));
    return buildSections(kItemTableId, header, item.data, sequence);
}

std::vector<std::vector<uint8_t>> AppDataTransport::buildCaptionSections(const Caption& caption, const std::vector<uint8_t>& data, uint16_t sequence) {
    std::vector<uint8_t> header;
    putBe16(header, static_cast<uint16_t>(caption.componentTag));
    header.push_back(caption.dataType);
    header.push_back(caption.tmd);
    header.push_back(caption.pts90k ? 0x80 : 0x00);
    const uint64_t pts = caption.pts90k.value_or(0) & 0x1FFFFFFFFULL;
    header.push_back(static_cast<uint8_t>(pts >> 32));
    putBe32(header, static_cast<uint32_t>(pts));
    putBe32(header, static_cast<uint32_t>(data.size()));

    // Extension, preceded by its length so receivers can skip what they do
    // not know: language, resolution, reference start time, MFU header numbers.
    std::vector<uint8_t> extension;
    extension.insert(extension.end(), caption.languageCode.begin(), caption.languageCode.end());
    extension.push_back(caption.resolution);
    putBe32(extension, caption.referenceStartTimeSeconds);
    putBe32(extension, caption.referenceStartTimeFraction);
    extension.push_back(caption.subtitleSequenceNumber);
    extension.push_back(caption.subsampleNumber);
    extension.push_back(caption.lastSubsampleNumber);
    header.push_back(static_cast<uint8_t>(extension.size()));
    header.insert(header.end(), extension.begin(), extension.end());
    return buildSections(kCaptionTableId, header, data, sequence);
}

std::vector<std::vector<uint8_t>> AppDataTransport::buildSections(uint8_t tableId, const std::vector<uint8_t>& header, const std::vector<uint8_t>& data, uint16_t sequence) {
    const size_t total = header.size() + data.size();
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

        section.push_back(tableId);
        // section_syntax_indicator, private_indicator, reserved, section_length
        putBe16(section, static_cast<uint16_t>(0xF000 | sectionLength));
        putBe16(section, sequence);
        // reserved, version_number, current_next_indicator
        section.push_back(static_cast<uint8_t>(0xC0 | ((sequence & 0x1F) << 1) | 0x01));
        section.push_back(static_cast<uint8_t>(number));
        section.push_back(static_cast<uint8_t>(sectionCount - 1));

        if (number == 0) {
            section.insert(section.end(), header.begin(), header.end());
            payloadSize -= header.size();
        }
        section.insert(section.end(), data.begin() + offset, data.begin() + offset + payloadSize);
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
    packetIdToEmtTag.clear();
    applicationServices.clear();
    captionServices.clear();
    lastSent.clear();
    assembler.clear();
}

}
