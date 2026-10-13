#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>
#include "demuxerHandler.h"

namespace MmtTlv {

// Experimental: collects the data broadcasting (ARIB STD-B62 multimedia)
// application files carried in aapp assets and writes them out as a directory
// tree, using DDMT/DAMT/index items to recover the original paths.
class AppDataDumper : public DemuxerHandler {
public:
    explicit AppDataDumper(std::filesystem::path outputDirectory);

    void onApplicationData(const MmtStream& mmtStream, const Mpu& mpu, const DataUnit& dataUnit, const MfuData& mfuData) override;
    void onMhAit(const MhAit& mhAit) override;
    void onDamt(const Damt& damt) override;
    void onDdmt(const Ddmt& ddmt) override;
    void onDcct(const Dcct& dcct) override;

    // Resolves paths and writes files plus manifest.txt. Returns false if the
    // output directory could not be written.
    bool finish();

private:
    struct PendingItem {
        uint32_t mpuSequenceNumber{};
        uint32_t itemId{};
        std::vector<uint8_t> data;
    };

    struct ItemVersion {
        std::vector<uint8_t> data;
        size_t hash{};
        uint64_t seenCount{};
    };

    // component_tag, mpu_sequence_number, item_id
    using ItemKey = std::tuple<int32_t, uint32_t, uint32_t>;

    struct DamtItem {
        uint16_t nodeTag{};
        uint32_t itemId{};
        uint32_t itemSize{};
        uint8_t itemVersion{};
    };

    struct DamtMpu {
        uint8_t sessionId{};
        uint8_t tableVersion{};
        uint32_t mpuSize{};
        bool indexItemFlag{};
        std::optional<uint32_t> indexItemId;
        uint8_t indexItemCompressionFlag{};
        std::optional<uint16_t> nodeTag;
        std::vector<DamtItem> items;
    };

    struct DdmtNode {
        std::string path;
        uint8_t version{};
    };

    struct DdmtFile {
        uint16_t directoryNodeTag{};
        std::string fileName;
    };

    struct IndexEntry {
        std::string fileName;
        std::string itemType;
        uint32_t itemSize{};
        uint8_t compressionType{0xFF};
        uint32_t originalSize{};
    };

    struct IndexInfo {
        std::optional<uint32_t> indexItemId;
        std::map<uint32_t, IndexEntry> entries;
    };

    struct Resolved {
        std::filesystem::path path;
        bool unresolved{};
        std::optional<IndexEntry> indexEntry;
        std::string note;
    };

    void completeItem(int32_t componentTag, PendingItem&& item);
    IndexInfo loadIndexItem(int32_t componentTag, uint32_t mpuSequenceNumber, const DamtMpu& damtMpu) const;
    Resolved resolvePath(const ItemKey& key) const;

    std::filesystem::path outputDirectory;

    std::unordered_map<uint16_t, PendingItem> pendingByPacketId;
    std::map<ItemKey, std::vector<ItemVersion>> items;

    // (data_transmission_session_id, node_tag)
    std::map<std::pair<uint8_t, uint16_t>, DdmtNode> ddmtNodes;
    std::map<std::pair<uint8_t, uint16_t>, DdmtFile> ddmtFiles;
    std::map<uint8_t, std::string> ddmtBaseDirectories;
    // (component_tag, mpu_sequence_number)
    std::map<std::pair<int32_t, uint32_t>, DamtMpu> damtMpus;
    std::map<std::pair<int32_t, uint32_t>, IndexInfo> indexes;

    std::vector<std::string> aitLog;
    std::vector<std::string> dcctLog;

    uint64_t fragmentCount{};
    uint64_t orphanFragmentCount{};
    uint64_t abandonedItemCount{};
    uint64_t continuationWithItemId{};
    uint64_t continuationWithoutItemId{};
};

}
