#include "appItemAssembler.h"
#include "dataUnit.h"
#include "mmtStream.h"
#include "mpu.h"
#include "mpuProcessorBase.h"

namespace MmtTlv {

std::optional<AppItemAssembler::Item> AppItemAssembler::push(const MmtStream& mmtStream, const Mpu& mpu, const DataUnit& dataUnit, const MfuData& mfuData) {
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
        return Item{ componentTag, packetId, mpu.mpuSequenceNumber, dataUnit.itemId, mfuData.data };

    case FragmentationIndicator::FirstFragment:
        if (pending != pendingByPacketId.end()) {
            abandonedItemCount++;
        }
        pendingByPacketId[packetId] = Item{ componentTag, packetId, mpu.mpuSequenceNumber, dataUnit.itemId, mfuData.data };
        return std::nullopt;

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
        return std::nullopt;
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

    if (mpu.fragmentationIndicator != FragmentationIndicator::LastFragment) {
        return std::nullopt;
    }

    auto completed = std::move(item);
    pendingByPacketId.erase(pending);
    return completed;
}

void AppItemAssembler::clear() {
    pendingByPacketId.clear();
}

}
