#include "appDataDumper.h"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string_view>
#include "damt.h"
#include "dataUnit.h"
#include "dcct.h"
#include "ddmt.h"
#include "indexItem.h"
#include "mhAit.h"
#include "mhApplicationDescriptor.h"
#include "mhSimpleApplicationLocationDescriptor.h"
#include "mhTransportProtocolDescriptor.h"
#include "mmtStream.h"
#include "mpu.h"
#include "zlibInflate.h"

namespace MmtTlv {

namespace {

constexpr uint8_t kNoCompression = 0xFF;
constexpr size_t kMaxIndexItemSize = 1024 * 1024;

std::filesystem::path pathFromUtf8(const std::string& s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

std::string pathToUtf8(const std::filesystem::path& path) {
    const auto u8 = path.generic_u8string();
    return std::string(u8.begin(), u8.end());
}

size_t hashBytes(const std::vector<uint8_t>& data) {
    return std::hash<std::string_view>{}(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
}

// Turns a path taken from the broadcast into a relative path that can only
// name something inside the output directory.
std::filesystem::path sanitizeRelativePath(const std::string& path) {
    std::filesystem::path result;
    std::string component;
    auto flush = [&]() {
        if (!component.empty() && component != "." && component != "..") {
            for (auto& c : component) {
                const auto u = static_cast<unsigned char>(c);
                if (u < 0x20 || c == '<' || c == '>' || c == ':' || c == '"' || c == '\\' || c == '|' || c == '?' || c == '*') {
                    c = '_';
                }
            }
            result /= pathFromUtf8(component);
        }
        component.clear();
    };
    for (char c : path) {
        if (c == '/') {
            flush();
        }
        else {
            component += c;
        }
    }
    flush();
    return result;
}

std::string hex(uint64_t value, int width) {
    std::ostringstream ss;
    ss << "0x" << std::hex << std::setw(width) << std::setfill('0') << value;
    return ss.str();
}

} // namespace

AppDataDumper::AppDataDumper(std::filesystem::path outputDirectory)
    : outputDirectory(std::move(outputDirectory)) {
}

void AppDataDumper::onApplicationData(const MmtStream& mmtStream, const Mpu& mpu, const DataUnit& dataUnit, const MfuData& mfuData) {
    fragmentCount++;
    const uint16_t packetId = mmtStream.getPacketId();
    const int32_t componentTag = mmtStream.getComponentTag();
    auto pending = pendingByPacketId.find(packetId);

    switch (mpu.fragmentationIndicator) {
    case FragmentationIndicator::NotFragmented:
        if (pending != pendingByPacketId.end()) {
            abandonedItemCount++;
            pendingByPacketId.erase(pending);
        }
        completeItem(componentTag, PendingItem{ mpu.mpuSequenceNumber, dataUnit.itemId, mfuData.data });
        return;

    case FragmentationIndicator::FirstFragment:
        if (pending != pendingByPacketId.end()) {
            abandonedItemCount++;
        }
        pendingByPacketId[packetId] = PendingItem{ mpu.mpuSequenceNumber, dataUnit.itemId, mfuData.data };
        return;

    case FragmentationIndicator::MiddleFragment:
    case FragmentationIndicator::LastFragment:
        break;
    }

    if (pending == pendingByPacketId.end() || pending->second.mpuSequenceNumber != mpu.mpuSequenceNumber) {
        orphanFragmentCount++;
        if (pending != pendingByPacketId.end()) {
            abandonedItemCount++;
            pendingByPacketId.erase(pending);
        }
        return;
    }

    auto& item = pending->second;
    // DataUnit::unpack always consumes a 4-byte item_id. If continuation
    // fragments do not repeat it, those four bytes were payload: put them back.
    if (dataUnit.itemId == item.itemId) {
        continuationWithItemId++;
    }
    else {
        continuationWithoutItemId++;
        const uint32_t v = dataUnit.itemId;
        const uint8_t bytes[4] = {
            static_cast<uint8_t>(v >> 24), static_cast<uint8_t>(v >> 16),
            static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v) };
        item.data.insert(item.data.end(), bytes, bytes + 4);
    }
    item.data.insert(item.data.end(), mfuData.data.begin(), mfuData.data.end());

    if (mpu.fragmentationIndicator == FragmentationIndicator::LastFragment) {
        auto completed = std::move(item);
        pendingByPacketId.erase(pending);
        completeItem(componentTag, std::move(completed));
    }
}

void AppDataDumper::completeItem(int32_t componentTag, PendingItem&& item) {
    auto& versions = items[{ componentTag, item.mpuSequenceNumber, item.itemId }];
    const size_t hash = hashBytes(item.data);
    for (auto& version : versions) {
        if (version.hash == hash && version.data == item.data) {
            version.seenCount++;
            return;
        }
    }
    versions.push_back(ItemVersion{ std::move(item.data), hash, 1 });
}

void AppDataDumper::onMhAit(const MhAit& mhAit) {
    std::ostringstream ss;
    ss << "MH-AIT application_type=" << hex(mhAit.applicationType, 4)
       << " version=" << static_cast<int>(mhAit.versionNumber) << "\n";
    for (const auto& app : mhAit.applications) {
        ss << "  app organization_id=" << hex(app.applicationIdentifier.organizationId, 8)
           << " application_id=" << hex(app.applicationIdentifier.applicationId, 4)
           << " control_code=" << static_cast<int>(app.applicationControlCode) << "\n";
        for (const auto& descriptor : app.descriptors.list) {
            ss << "    descriptor " << hex(descriptor->getDescriptorTag(), 4);
            if (auto location = dynamic_cast<const MhSimpleApplicationLocationDescriptor*>(descriptor.get())) {
                ss << " simple_application_location initial_path=\"" << location->initialPath << "\"";
            }
            else if (auto transport = dynamic_cast<const MhTransportProtocolDescriptor*>(descriptor.get())) {
                ss << " transport_protocol protocol_id=" << hex(transport->protocolId, 4)
                   << " label=" << static_cast<int>(transport->transportProtocolLabel)
                   << " selector=";
                for (auto b : transport->selector) {
                    ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
                }
                ss << std::dec;
            }
            else if (auto application = dynamic_cast<const MhApplicationDescriptor*>(descriptor.get())) {
                ss << " application visibility=" << static_cast<int>(application->visibility)
                   << " priority=" << static_cast<int>(application->applicationPriority);
                for (const auto& profile : application->applicationProfiles) {
                    ss << " profile=" << hex(profile.applicationProfile, 4)
                       << "/" << static_cast<int>(profile.versionMajor)
                       << "." << static_cast<int>(profile.versionMinor)
                       << "." << static_cast<int>(profile.versionMicro);
                }
            }
            ss << "\n";
        }
    }
    const auto text = ss.str();
    if (std::find(aitLog.begin(), aitLog.end(), text) == aitLog.end()) {
        aitLog.push_back(text);
    }
}

void AppDataDumper::onDamt(const Damt& damt) {
    for (const auto& mpu : damt.mpus) {
        DamtMpu info;
        info.sessionId = damt.dataTransmissionSessionId;
        info.tableVersion = damt.versionNumber;
        info.mpuSize = mpu.mpuSize;
        info.indexItemFlag = mpu.indexItemFlag;
        if (mpu.indexItemFlag && mpu.indexItemIdFlag) {
            info.indexItemId = mpu.indexItemId;
        }
        info.indexItemCompressionFlag = mpu.indexItemCompressionFlag;
        info.nodeTag = mpu.nodeTag;
        for (const auto& item : mpu.items) {
            info.items.push_back(DamtItem{ item.nodeTag, item.itemId, item.itemSize, item.itemVersion });
        }
        damtMpus[{ damt.componentTag, mpu.mpuSequenceNumber }] = std::move(info);
    }
}

void AppDataDumper::onDdmt(const Ddmt& ddmt) {
    const uint8_t session = ddmt.dataTransmissionSessionId;
    ddmtBaseDirectories[session] = ddmt.baseDirectoryPath;
    for (const auto& node : ddmt.nodes) {
        ddmtNodes[{ session, node.nodeTag }] = DdmtNode{ node.directoryNodePath, node.directoryNodeVersion };
        for (const auto& file : node.files) {
            ddmtFiles[{ session, file.nodeTag }] = DdmtFile{ node.nodeTag, file.fileName };
        }
    }
}

void AppDataDumper::onDcct(const Dcct& dcct) {
    std::ostringstream ss;
    ss << "DCCT session=" << static_cast<int>(dcct.dataTransmissionSessionId)
       << " content_id=" << dcct.contentId
       << " content_version=" << static_cast<int>(dcct.contentVersion)
       << " content_size=" << dcct.contentSize
       << " pus=" << dcct.pus.size()
       << " node_tags=" << dcct.nodeTags.size() << "\n";
    for (const auto& pu : dcct.pus) {
        ss << "  pu tag=" << pu.puTag << " nodes=";
        for (auto tag : pu.nodeTags) {
            ss << tag << ",";
        }
        ss << "\n";
    }
    const auto text = ss.str();
    if (std::find(dcctLog.begin(), dcctLog.end(), text) == dcctLog.end()) {
        dcctLog.push_back(text);
    }
}

AppDataDumper::IndexInfo AppDataDumper::loadIndexItem(int32_t componentTag, uint32_t mpuSequenceNumber, const DamtMpu& damtMpu) const {
    IndexInfo result;

    // Without index_item_id the index item is not named by the DAMT; try every
    // item of the MPU and take the first one that parses as an index.
    std::vector<uint32_t> candidates;
    if (damtMpu.indexItemId) {
        candidates.push_back(*damtMpu.indexItemId);
    }
    else {
        auto it = items.lower_bound({ componentTag, mpuSequenceNumber, 0 });
        for (; it != items.end() && std::get<0>(it->first) == componentTag && std::get<1>(it->first) == mpuSequenceNumber; ++it) {
            candidates.push_back(std::get<2>(it->first));
        }
    }

    auto parse = [&](const std::vector<uint8_t>& data) {
        Common::ReadStream stream(data);
        IndexItem indexItem;
        if (!indexItem.unpack(stream) || indexItem.items.empty() || !stream.isEof()) {
            return false;
        }
        for (const auto& entry : indexItem.items) {
            result.entries[entry.itemId] = IndexEntry{ entry.fileName, entry.itemType, entry.itemSize, entry.compressionType, entry.compressionType != kNoCompression ? entry.originalSize : 0 };
        }
        return true;
    };

    for (auto itemId : candidates) {
        auto it = items.find({ componentTag, mpuSequenceNumber, itemId });
        if (it == items.end() || it->second.empty()) {
            continue;
        }
        // index_item_compression_flag=3 has been seen on plain index items, so
        // try the bytes as they are before inflating.
        const auto& data = it->second.back().data;
        bool parsed = parse(data);
        if (!parsed) {
            std::vector<uint8_t> inflated;
            parsed = zlibInflate(data, inflated, kMaxIndexItemSize) && parse(inflated);
        }
        if (parsed) {
            result.indexItemId = itemId;
            return result;
        }
    }
    return result;
}

AppDataDumper::Resolved AppDataDumper::resolvePath(const ItemKey& key) const {
    const auto [componentTag, mpuSequenceNumber, itemId] = key;
    const std::string componentDirectory = "ct_" + hex(componentTag, 2).substr(2);
    Resolved resolved;
    resolved.path = std::filesystem::path("_unresolved") / componentDirectory
        / ("mpu_" + std::to_string(mpuSequenceNumber))
        / ("item_" + std::to_string(itemId) + ".bin");
    resolved.unresolved = true;

    auto damt = damtMpus.find({ componentTag, mpuSequenceNumber });
    if (damt == damtMpus.end()) {
        resolved.note += " no_damt";
        return resolved;
    }
    const auto& damtMpu = damt->second;
    const uint8_t session = damtMpu.sessionId;

    std::filesystem::path base;
    if (auto it = ddmtBaseDirectories.find(session); it != ddmtBaseDirectories.end()) {
        base = sanitizeRelativePath(it->second);
    }

    if (damtMpu.indexItemFlag) {
        auto index = indexes.find({ componentTag, mpuSequenceNumber });
        if (index == indexes.end() || !index->second.indexItemId) {
            resolved.note += " index_item=missing";
            return resolved;
        }
        if (*index->second.indexItemId == itemId) {
            resolved.note += " (index item)";
            resolved.path = std::filesystem::path("_index") / componentDirectory / ("mpu_" + std::to_string(mpuSequenceNumber) + ".idx");
            resolved.unresolved = false;
            return resolved;
        }
        auto entry = index->second.entries.find(itemId);
        if (entry == index->second.entries.end()) {
            resolved.note += " not_in_index";
            return resolved;
        }
        std::filesystem::path directory;
        if (damtMpu.nodeTag) {
            if (auto node = ddmtNodes.find({ session, *damtMpu.nodeTag }); node != ddmtNodes.end()) {
                directory = sanitizeRelativePath(node->second.path);
            }
            else {
                resolved.note += " no_ddmt_node=" + std::to_string(*damtMpu.nodeTag);
            }
        }
        else {
            resolved.note += " no_mpu_node_tag";
        }
        resolved.note += " type=\"" + entry->second.itemType + "\" index_size=" + std::to_string(entry->second.itemSize);
        if (entry->second.compressionType != kNoCompression) {
            resolved.note += " compression=" + std::to_string(entry->second.compressionType) + " original_size=" + std::to_string(entry->second.originalSize);
        }
        resolved.indexEntry = entry->second;
        resolved.path = base / directory / sanitizeRelativePath(entry->second.fileName);
        resolved.unresolved = false;
        return resolved;
    }

    for (const auto& item : damtMpu.items) {
        if (item.itemId != itemId) {
            continue;
        }
        resolved.note += " damt_size=" + std::to_string(item.itemSize) + " item_version=" + std::to_string(item.itemVersion);
        auto file = ddmtFiles.find({ session, item.nodeTag });
        if (file == ddmtFiles.end()) {
            resolved.note += " no_ddmt_file=" + std::to_string(item.nodeTag);
            return resolved;
        }
        std::filesystem::path directory;
        if (auto node = ddmtNodes.find({ session, file->second.directoryNodeTag }); node != ddmtNodes.end()) {
            directory = sanitizeRelativePath(node->second.path);
        }
        resolved.path = base / directory / sanitizeRelativePath(file->second.fileName);
        resolved.unresolved = false;
        return resolved;
    }
    resolved.note += " not_in_damt";
    return resolved;
}

bool AppDataDumper::finish() {
    std::error_code ec;
    std::filesystem::create_directories(outputDirectory, ec);
    if (ec) {
        std::cerr << "Unable to create output directory: " << pathToUtf8(outputDirectory) << std::endl;
        return false;
    }

    std::ofstream manifest(outputDirectory / "manifest.txt", std::ios::binary);
    if (!manifest) {
        std::cerr << "Unable to write manifest.txt" << std::endl;
        return false;
    }

    manifest << "# dantto4k application data dump (experimental)\n";
    manifest << "fragments=" << fragmentCount
             << " orphan_fragments=" << orphanFragmentCount
             << " abandoned_items=" << abandonedItemCount
             << " continuation_with_item_id=" << continuationWithItemId
             << " continuation_without_item_id=" << continuationWithoutItemId << "\n\n";

    manifest << "[MH-AIT]\n";
    for (const auto& text : aitLog) {
        manifest << text;
    }
    manifest << "\n[DCCT]\n";
    for (const auto& text : dcctLog) {
        manifest << text;
    }
    manifest << "\n[DDMT]\n";
    for (const auto& [session, base] : ddmtBaseDirectories) {
        manifest << "session=" << static_cast<int>(session) << " base=\"" << base << "\"\n";
    }
    for (const auto& [key, node] : ddmtNodes) {
        manifest << "  node session=" << static_cast<int>(key.first) << " tag=" << key.second
                 << " version=" << static_cast<int>(node.version) << " path=\"" << node.path << "\"\n";
    }
    for (const auto& [key, file] : ddmtFiles) {
        manifest << "  file session=" << static_cast<int>(key.first) << " tag=" << key.second
                 << " dir_tag=" << file.directoryNodeTag << " name=\"" << file.fileName << "\"\n";
    }
    manifest << "\n[DAMT]\n";
    for (const auto& [key, mpu] : damtMpus) {
        manifest << "component_tag=" << hex(key.first, 2) << " mpu=" << key.second
                 << " session=" << static_cast<int>(mpu.sessionId)
                 << " version=" << static_cast<int>(mpu.tableVersion)
                 << " mpu_size=" << mpu.mpuSize
                 << " index_item=" << (mpu.indexItemFlag ? (mpu.indexItemId ? std::to_string(*mpu.indexItemId) : std::string("yes")) : std::string("no"))
                 << " index_compression=" << static_cast<int>(mpu.indexItemCompressionFlag)
                 << " node_tag=" << (mpu.nodeTag ? std::to_string(*mpu.nodeTag) : std::string("-"))
                 << " items=" << mpu.items.size() << "\n";
    }

    for (const auto& [key, damtMpu] : damtMpus) {
        if (damtMpu.indexItemFlag) {
            indexes[key] = loadIndexItem(key.first, key.second, damtMpu);
        }
    }

    manifest << "\n[ITEMS]\n";
    uint64_t written = 0;
    uint64_t unresolvedCount = 0;
    uint64_t inflateFailures = 0;
    for (const auto& [key, versions] : items) {
        const auto resolved = resolvePath(key);
        if (resolved.unresolved) {
            unresolvedCount++;
        }

        for (size_t i = 0; i < versions.size(); i++) {
            const auto& version = versions[i];
            std::vector<uint8_t> data = version.data;
            auto path = outputDirectory / resolved.path;
            std::string note = resolved.note;

            if (resolved.indexEntry && resolved.indexEntry->compressionType != kNoCompression) {
                std::vector<uint8_t> inflated;
                // compression_type 0 has always been zlib so far; anything else
                // is kept as transmitted.
                if (resolved.indexEntry->compressionType == 0 &&
                    zlibInflate(data, inflated, resolved.indexEntry->originalSize)) {
                    data = std::move(inflated);
                    if (data.size() != resolved.indexEntry->originalSize) {
                        note += " SIZE_MISMATCH";
                    }
                }
                else {
                    inflateFailures++;
                    note += " INFLATE_FAILED";
                    path += ".compressed-" + std::to_string(resolved.indexEntry->compressionType);
                }
            }
            if (resolved.indexEntry && version.data.size() != resolved.indexEntry->itemSize) {
                note += " ITEM_SIZE_MISMATCH";
            }

            // The latest copy keeps the plain name; older copies get a suffix.
            if (i + 1 < versions.size()) {
                path += ".~" + std::to_string(i + 1);
            }
            std::filesystem::create_directories(path.parent_path(), ec);
            std::ofstream out(path, std::ios::binary);
            if (!out) {
                manifest << "WRITE FAILED " << pathToUtf8(path) << "\n";
                continue;
            }
            out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
            written++;

            manifest << hex(std::get<0>(key), 2) << " mpu=" << std::get<1>(key) << " item=" << std::get<2>(key)
                     << " size=" << version.data.size() << " seen=" << version.seenCount
                     << " -> " << pathToUtf8(std::filesystem::relative(path, outputDirectory, ec))
                     << note << "\n";
        }
    }

    for (const auto& [packetId, pending] : pendingByPacketId) {
        manifest << "INCOMPLETE packet_id=" << hex(packetId, 4) << " mpu=" << pending.mpuSequenceNumber
                 << " item=" << pending.itemId << " bytes=" << pending.data.size() << "\n";
    }

    std::cerr << "Application data: items=" << items.size()
              << " files_written=" << written
              << " unresolved=" << unresolvedCount
              << " inflate_failed=" << inflateFailures
              << " -> " << pathToUtf8(outputDirectory) << std::endl;
    return true;
}

}
