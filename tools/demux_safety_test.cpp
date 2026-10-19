#include "mmtTlvDemuxer.h"
#include "demuxerHandler.h"
#include <cstdio>
#include <stdexcept>

void subtitleDebugLog(const std::string&) {}

static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

class Handler : public MmtTlv::DemuxerHandler {
public:
    int mptCount = 0;
    int ecmCount = 0;
    int appCount = 0;
    void onApplicationData(const MmtTlv::MmtStream&, const MmtTlv::Mpu&, const MmtTlv::DataUnit&, const MmtTlv::MfuData&) override { ++appCount; }
    void onMpt(const MmtTlv::Mpt&) override { ++mptCount; }
    void onEcm(const MmtTlv::Ecm&) override { ++ecmCount; }
};

static std::vector<uint8_t> mpt(uint16_t packetId, bool truncated, uint32_t asset = MmtTlv::AssetType::hev1, uint16_t service = 1, int tag = -1) {
    MmtTlv::Common::WriteStream out;
    out.put8U(0x20); // MPT
    out.put8U(0); out.putBe16U(0); out.put8U(0);
    out.put8U(2); out.putBe16U(service); // package ID
    out.putBe16U(0); // descriptors
    out.put8U(truncated ? 2 : 1); // missing second asset in damaged input
    out.put8U(0); out.putBe32U(0); out.put8U(0);
    out.putBe32U(asset);
    out.put8U(0); out.put8U(1);
    out.put8U(0); out.putBe16U(packetId);
    out.putBe16U(tag < 0 ? 0 : 5);
    if (tag >= 0) {
        out.putBe16U(0x8011); out.put8U(2); out.putBe16U(static_cast<uint16_t>(tag));
    }
    return out.getData();
}

static void feed(MmtTlv::MmtTlvDemuxer& demuxer, const std::vector<uint8_t>& table, uint32_t seq) {
    MmtTlv::Common::WriteStream data;
    data.putBe16U(0); data.put8U(0x61); // compressed IP
    data.put8U(0); data.put8U(2); data.putBe16U(0x8000); // MMTP signaling
    data.putBe32U(0); data.putBe32U(seq);
    data.put8U(0); data.put8U(0); // unfragmented signaling
    data.putBe16U(0x8000); data.put8U(0);
    data.putBe16U(static_cast<uint16_t>(table.size()));
    data.write(table);
    MmtTlv::Common::WriteStream packet;
    packet.put8U(0x7f); packet.put8U(3);
    packet.putBe16U(static_cast<uint16_t>(data.getData().size()));
    packet.write(data.getData());
    MmtTlv::Common::ReadStream input(packet.getData());
    require(demuxer.demux(input) == MmtTlv::DemuxStatus::Ok, "TLV demux failed");
}

static void app(MmtTlv::MmtTlvDemuxer& demuxer, uint32_t packetSeq, uint32_t mpuSeq,
                uint8_t fragmentation, bool malformed = false) {
    MmtTlv::Common::WriteStream payload;
    payload.put8U(static_cast<uint8_t>(0x20 | (fragmentation << 1)));
    payload.put8U(0);
    payload.putBe32U(mpuSeq);
    if (!malformed) {
        payload.putBe32U(1); // item_id
        payload.put8U(0x41);
    }
    MmtTlv::Common::WriteStream data;
    data.putBe16U(0); data.put8U(0x61); // compressed IP
    data.put8U(0); data.put8U(0); data.putBe16U(0x100); // MPU packet
    data.putBe32U(0); data.putBe32U(packetSeq);
    data.putBe16U(static_cast<uint16_t>(payload.getData().size()));
    data.write(payload.getData());
    MmtTlv::Common::WriteStream packet;
    packet.put8U(0x7f); packet.put8U(3);
    packet.putBe16U(static_cast<uint16_t>(data.getData().size()));
    packet.write(data.getData());
    MmtTlv::Common::ReadStream input(packet.getData());
    require(demuxer.demux(input) == MmtTlv::DemuxStatus::Ok, "MPU demux failed");
}

int main() {
    try {
        Handler handler;
        MmtTlv::MmtTlvDemuxer demuxer;
        demuxer.setDemuxerHandler(handler);
        feed(demuxer, mpt(0x100, false), 1);
        require(handler.mptCount == 1 && demuxer.getStream(0x100), "valid MPT must register a stream");
        feed(demuxer, mpt(0x200, true), 2);
        require(handler.mptCount == 1 && demuxer.getStream(0x100) && !demuxer.getStream(0x200),
                "partial MPT must preserve existing streams and never reach handlers");
        feed(demuxer, {0x82}, 3);
        require(handler.ecmCount == 0, "truncated ECM must not reach handlers");
        feed(demuxer, mpt(0x200, false), 4);
        require(handler.mptCount == 2 && !demuxer.getStream(0x100) && demuxer.getStream(0x200),
                "valid MPT after damaged input must still update streams");
        for (uint32_t nextMpu : {2U, 9U, 0U}) {
            Handler appHandler;
            MmtTlv::MmtTlvDemuxer appDemux;
            appDemux.setDemuxerHandler(appHandler);
            feed(appDemux, mpt(0x100, false, MmtTlv::AssetType::aapp), 1);
            app(appDemux, 1, 1, 1);
            app(appDemux, 2, 1, 3, true); // malformed last fragment, no packet sequence gap
            app(appDemux, 3, nextMpu, 0);
            app(appDemux, 4, nextMpu + 1, 0);
            require(appHandler.appCount == 3, "valid MPUs must recover after malformed last fragment");
        }
        Handler fragments;
        MmtTlv::MmtTlvDemuxer fragmentDemux;
        fragmentDemux.setDemuxerHandler(fragments);
        feed(fragmentDemux, mpt(0x100, false, MmtTlv::AssetType::aapp), 1);
        app(fragmentDemux, 1, 1, 1);
        app(fragmentDemux, 2, 2, 1); // new MPU starts fragmented
        app(fragmentDemux, 3, 2, 2);
        app(fragmentDemux, 4, 2, 3);
        require(fragments.appCount == 4, "new fragmented MPU and its continuations must remain usable");
        Handler multiplexHandler;
        MmtTlv::MmtTlvDemuxer multiplex;
        multiplex.setDemuxerHandler(multiplexHandler);
        auto first = mpt(0x100, false, MmtTlv::AssetType::hev1, 1, 0);
        auto second = mpt(0x200, false, MmtTlv::AssetType::hev1, 2, 0);
        feed(multiplex, first, 1);
        auto* retained = multiplex.getStream(0x100);
        const auto index = retained->getStreamIndex();
        const auto pid = retained->getMpeg2PacketId();
        feed(multiplex, second, 2);
        require(multiplex.getStream(0x100) == retained && multiplex.getStream(0x200),
                "another package must not remove an existing service");
        require(multiplex.getStreamByIdx(index) == retained &&
                multiplex.getStream(0x200)->getStreamIndex() != index &&
                multiplex.getStream(0x200)->getMpeg2PacketId() != pid,
                "services with identical component tags need unique indices and TS PIDs");
        feed(multiplex, first, 3);
        require(retained->getStreamIndex() == index && retained->getMpeg2PacketId() == pid,
                "periodic MPT must keep stream indices and PIDs stable");
        feed(multiplex, mpt(0x300, false, MmtTlv::AssetType::hev1, 1), 4);
        require(!multiplex.getStream(0x100) && multiplex.getStream(0x200) && multiplex.getStream(0x300),
                "only assets removed from the updated package may be removed");
        feed(multiplex, mpt(0x300, false, MmtTlv::AssetType::hev1, 2), 5);
        auto emptyFirst = first; emptyFirst.resize(11); emptyFirst[10] = 0;
        feed(multiplex, emptyFirst, 6);
        require(multiplex.getStream(0x300) && !multiplex.getStream(0x200),
                "shared assets survive until their last package removes them");
        auto emptySecond = emptyFirst; emptySecond[7] = 2;
        feed(multiplex, emptySecond, 7);
        require(multiplex.mapStream.empty() && !multiplex.getStreamByIdx(index),
                "an empty complete MPT must remove the last owned assets");
        multiplex.clear();
        feed(multiplex, second, 1);
        require(multiplex.getStream(0x200)->getStreamIndex() == 0,
                "clear must reset package ownership and stream indices");
        std::puts("PASS: malformed MPT/ECM rejection and MPU fragment recovery across consecutive, skipped and backward sequences");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
