#include "appDataTransport.h"
#include "applicationServiceDescriptor.h"
#include "mpt.h"
#include "mpuProcessorBase.h"
#include <cstdio>
#include <stdexcept>

void subtitleDebugLog(const std::string&) {}

static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

static uint32_t crc32Mpeg2(const std::vector<uint8_t>& data) {
    uint32_t crc = 0xFFFFFFFF;
    for (uint8_t byte : data) {
        crc ^= static_cast<uint32_t>(byte) << 24;
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80000000) ? (crc << 1) ^ 0x04C11DB7 : (crc << 1);
        }
    }
    return crc;
}

// Application service descriptor as sent by NHK BSP4K, cut to two EMTs.
static const std::vector<uint8_t> kApplicationServiceDescriptor = {
    0x80, 0x34, 0x11, 0x1F, 0x1F, 0xF2,
    0x00, 0xFF, 0xA2,       // MH-AIT
    0x00, 0xFF, 0xD2,       // data transmission messages
    0x40, 0x00, 0xFE, 0xE1, // EMT
    0x60, 0x00, 0xFE, 0xE2, // EMT
};

static MmtTlv::Mpt mpt(uint16_t serviceId, bool withApplicationService,
    uint32_t assetType = MmtTlv::AssetType::aapp, uint16_t assetPacketId = 0xF340) {
    MmtTlv::Common::WriteStream out;
    out.put8U(0x20); // MPT
    out.put8U(0); out.putBe16U(0); out.put8U(0);
    out.put8U(2); out.putBe16U(serviceId); // package ID
    if (withApplicationService) {
        out.putBe16U(static_cast<uint16_t>(kApplicationServiceDescriptor.size()));
        for (uint8_t b : kApplicationServiceDescriptor) out.put8U(b);
    }
    else {
        out.putBe16U(0);
    }
    out.put8U(1); // one asset
    out.put8U(0); out.putBe32U(0); out.put8U(0);
    out.putBe32U(assetType);
    out.put8U(0); out.put8U(1);
    out.put8U(0); out.putBe16U(assetPacketId);
    out.putBe16U(0);

    auto data = out.getData();
    MmtTlv::Common::ReadStream stream(data);
    MmtTlv::Mpt table;
    require(table.unpack(stream), "test MPT parses");
    return table;
}

static std::vector<uint8_t> section(uint8_t tableId, uint8_t marker) {
    std::vector<uint8_t> s = { tableId, 0xB0, 0x09, 0x00, 0x00, 0xC1, 0x00, 0x00, marker };
    const uint32_t crc = crc32Mpeg2(s);
    s.push_back(static_cast<uint8_t>(crc >> 24)); s.push_back(static_cast<uint8_t>(crc >> 16));
    s.push_back(static_cast<uint8_t>(crc >> 8)); s.push_back(static_cast<uint8_t>(crc));
    return s;
}

static void testDescriptor() {
    auto table = mpt(0x0065, true);
    const MmtTlv::ApplicationServiceDescriptor* descriptor = nullptr;
    for (const auto& d : table.descriptors.list) {
        descriptor = dynamic_cast<const MmtTlv::ApplicationServiceDescriptor*>(d.get());
        if (descriptor) break;
    }
    require(descriptor, "application service descriptor is parsed");
    require(descriptor->applicationFormat == 1, "ARIB-HTML5 format");
    require(descriptor->documentResolution == 1, "3840x2160 resolution");
    require(descriptor->aitLocationInfo.packetId == 0xFFA2, "MH-AIT location");
    require(descriptor->dtMessageLocationInfo && descriptor->dtMessageLocationInfo->packetId == 0xFFD2, "DT message location");
    require(descriptor->emts.size() == 2 && descriptor->emts[1].emtTag == 0x60 && descriptor->emts[1].locationInfo.packetId == 0xFEE2, "EMT locations");
}

static void testRoutingAndResend() {
    MmtTlv::AppDataTransport transport;
    std::vector<std::pair<uint16_t, std::vector<uint8_t>>> sent;
    transport.setSectionCallback([&](uint16_t serviceId, const uint8_t* data, size_t size) {
        sent.emplace_back(serviceId, std::vector<uint8_t>(data, data + size));
    });

    transport.onMpt(0x0041, mpt(0x0041, false));
    require(!transport.hasApplicationService(0x0041), "no descriptor, no application service");
    transport.onMpt(0x0065, mpt(0x0065, true));
    require(transport.hasApplicationService(0x0065), "descriptor marks application service");

    const auto ait = section(0x9C, 1);
    transport.onTableSection(0xFFA2, ait.data(), ait.size(), 0);
    require(sent.size() == 1 && sent[0].first == 0x0065 && sent[0].second == ait, "AIT routed to its service unchanged");

    transport.onTableSection(0xFFA2, ait.data(), ait.size(), 45000);
    require(sent.size() == 1, "identical section held back within 1 s");
    transport.onTableSection(0xFFA2, ait.data(), ait.size(), 90000);
    require(sent.size() == 2, "identical section resent after 1 s");
    transport.onTableSection(0xFFA2, ait.data(), ait.size(), 10);
    require(sent.size() == 3, "clock going backwards resends");

    const auto changed = section(0x9C, 2);
    transport.onTableSection(0xFFA2, changed.data(), changed.size(), 20);
    require(sent.size() == 4, "changed section sent at once");

    const auto damt = section(0xA4, 1);
    const auto emt = section(0xA6, 1);
    transport.onTableSection(0xFFD2, damt.data(), damt.size(), 0);
    transport.onTableSection(0xFEE2, emt.data(), emt.size(), 0);
    require(sent.size() == 6, "DT message and EMT packet_ids are routed");
    // The EMT goes out wrapped with its EMT_tag (0x60 for packet_id 0xFEE2).
    const auto& wrapped = sent[5].second;
    require(wrapped[0] == MmtTlv::AppDataTransport::kTaggedEmtTableId && crc32Mpeg2(wrapped) == 0, "EMT is wrapped");
    require(wrapped[8] == 0x60, "EMT_tag precedes the EMT");
    require(std::equal(emt.begin(), emt.end(), wrapped.begin() + 9), "wrapped EMT is unchanged");

    transport.onTableSection(0x1234, damt.data(), damt.size(), 0);
    require(sent.size() == 6 && transport.getUnroutedCount() == 1, "unknown packet_id is dropped");

    transport.onMpt(0x0065, mpt(0x0065, false));
    transport.onTableSection(0xFFA2, changed.data(), changed.size(), 1000000);
    require(sent.size() == 6, "routing follows the latest MPT");
}

static void testItemSections() {
    for (size_t size : { size_t{0}, size_t{100}, size_t{4070}, size_t{4071}, size_t{200000} }) {
        MmtTlv::AppItemAssembler::Item item;
        item.componentTag = 0x60;
        item.mpuSequenceNumber = 1535705113;
        item.itemId = 0xDEADBEEF;
        item.data.resize(size);
        for (size_t i = 0; i < size; i++) item.data[i] = static_cast<uint8_t>(i * 7);

        const auto sections = MmtTlv::AppDataTransport::buildItemSections(item, 0x1234);
        require(!sections.empty(), "item fits");
        std::vector<uint8_t> body;
        for (size_t n = 0; n < sections.size(); n++) {
            const auto& s = sections[n];
            require(s.size() <= MmtTlv::AppDataTransport::kMaxSectionSize, "section size limit");
            require(s[0] == MmtTlv::AppDataTransport::kItemTableId, "item table_id");
            require(3 + (((s[1] & 0x0F) << 8) | s[2]) == s.size(), "section_length");
            require((s[1] & 0xC0) == 0xC0, "syntax and private indicators");
            require(((s[3] << 8) | s[4]) == 0x1234, "table_id_extension is the sequence");
            require(((s[5] >> 1) & 0x1F) == (0x1234 & 0x1F), "version follows the sequence");
            require(s[6] == n && s[7] == sections.size() - 1, "section numbering");
            require(crc32Mpeg2(s) == 0, "CRC_32");
            body.insert(body.end(), s.begin() + 8, s.end() - 4);
        }
        require(body.size() == 14 + size, "header plus item");
        require(((body[0] << 8) | body[1]) == 0x60, "component_tag");
        require(((uint32_t(body[6]) << 24) | (body[7] << 16) | (body[8] << 8) | body[9]) == 0xDEADBEEF, "item_id");
        require(((uint32_t(body[10]) << 24) | (body[11] << 16) | (body[12] << 8) | body[13]) == size, "item_size");
        require(std::equal(item.data.begin(), item.data.end(), body.begin() + 14), "payload round trip");
        if (size == 4070) require(sections.size() == 1, "4070 bytes fill one section");
        if (size == 4071) require(sections.size() == 2, "4071 bytes need two sections");
    }

    MmtTlv::AppItemAssembler::Item huge;
    huge.data.resize(4070 + 255 * 4084 + 1);
    require(MmtTlv::AppDataTransport::buildItemSections(huge, 0).empty(), "item over 256 sections is refused");
}

static void testCaptionSections() {
    const std::vector<uint8_t> ttml = { '<', 't', 't', '/', '>' };
    const uint64_t pts = 0x1ABCDEF01ULL; // 33 bits
    MmtTlv::AppDataTransport::Caption caption;
    caption.componentTag = 0x30;
    caption.dataType = 0;
    caption.tmd = 0b0010;
    caption.pts90k = pts;
    caption.languageCode = { 'e', 'n', 'g' };
    caption.resolution = 1;
    caption.referenceStartTimeSeconds = 0xEA000001;
    caption.referenceStartTimeFraction = 0x80000000;
    caption.subtitleSequenceNumber = 9;
    caption.subsampleNumber = 0;
    caption.lastSubsampleNumber = 2;
    auto sections = MmtTlv::AppDataTransport::buildCaptionSections(caption, ttml, 7);
    require(sections.size() == 1, "caption fits one section");
    const auto& s = sections[0];
    require(s[0] == MmtTlv::AppDataTransport::kCaptionTableId, "caption table_id");
    require(crc32Mpeg2(s) == 0, "caption CRC_32");
    require(((s[8] << 8) | s[9]) == 0x30 && s[10] == 0 && s[11] == 0b0010, "component_tag, data_type, tmd");
    require(s[12] == 0x80, "pts present");
    const uint64_t got = (uint64_t(s[13]) << 32) | (uint32_t(s[14]) << 24) | (s[15] << 16) | (s[16] << 8) | s[17];
    require(got == pts, "pts");
    require(((uint32_t(s[18]) << 24) | (s[19] << 16) | (s[20] << 8) | s[21]) == ttml.size(), "data_size");
    // Extension: length, language, resolution, reference start time, MFU header numbers.
    require(s[22] == 15, "extension length");
    require(s[23] == 'e' && s[24] == 'n' && s[25] == 'g' && s[26] == 1, "language and resolution");
    require(s[27] == 0xEA && s[30] == 0x01 && s[31] == 0x80 && s[34] == 0x00, "reference start time");
    require(s[35] == 9 && s[36] == 0 && s[37] == 2, "sequence and subsample numbers");
    require(std::equal(ttml.begin(), ttml.end(), s.begin() + 38), "caption payload");

    caption.dataType = 6;
    caption.tmd = 0b1111;
    caption.pts90k = std::nullopt;
    sections = MmtTlv::AppDataTransport::buildCaptionSections(caption, ttml, 8);
    require(sections[0][12] == 0x00 && sections[0][10] == 6, "resource without pts");

    // Captions follow the stpp asset's packet_id to an application service only.
    MmtTlv::AppDataTransport transport;
    int sent = 0;
    transport.setSectionCallback([&](uint16_t, const uint8_t*, size_t) { sent++; });
    transport.onMpt(0x0065, mpt(0x0065, true, MmtTlv::AssetType::stpp, 0xF330));
    caption.dataType = 0;
    caption.pts90k = pts;
    transport.onCaption(0xF330, caption, ttml);
    require(sent == 1, "caption of an application service is sent");
    transport.onMpt(0x0041, mpt(0x0041, false, MmtTlv::AssetType::stpp, 0xF331));
    transport.onCaption(0xF331, caption, ttml);
    require(sent == 1, "caption of a service without applications is not sent");
}

int main() {
    try {
        testDescriptor();
        testRoutingAndResend();
        testItemSections();
        testCaptionSections();
    }
    catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
    std::puts("PASS: application service descriptor, app data routing/resend, item and caption sections");
    return 0;
}
