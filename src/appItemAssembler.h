#pragma once
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace MmtTlv {

class MmtStream;
class Mpu;
class DataUnit;
struct MfuData;

// Joins the MFU fragments of an aapp asset back into whole items (the files of
// an ARIB STD-B62 application, or a DAMT index item).
class AppItemAssembler {
public:
    struct Item {
        int32_t componentTag{};
        uint16_t packetId{};
        uint32_t mpuSequenceNumber{};
        uint32_t itemId{};
        std::vector<uint8_t> data;
    };

    // Returns the item once its last fragment has arrived.
    std::optional<Item> push(const MmtStream& mmtStream, const Mpu& mpu, const DataUnit& dataUnit, const MfuData& mfuData);
    void clear();
    // Items still waiting for fragments, by packet_id.
    const std::unordered_map<uint16_t, Item>& getPendingItems() const { return pendingByPacketId; }

    uint64_t fragmentCount{};
    uint64_t orphanFragmentCount{};
    uint64_t abandonedItemCount{};
    uint64_t continuationWithItemId{};
    uint64_t continuationWithoutItemId{};

private:
    std::unordered_map<uint16_t, Item> pendingByPacketId;
};

}
