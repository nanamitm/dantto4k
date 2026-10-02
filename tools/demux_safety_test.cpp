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
    void onMpt(const MmtTlv::Mpt&) override { ++mptCount; }
    void onEcm(const MmtTlv::Ecm&) override { ++ecmCount; }
};

static std::vector<uint8_t> mpt(uint16_t packetId, bool truncated) {
    MmtTlv::Common::WriteStream out;
    out.put8U(0x20); // MPT
    out.put8U(0); out.putBe16U(0); out.put8U(0);
    out.put8U(2); out.putBe16U(1); // package ID
    out.putBe16U(0); // descriptors
    out.put8U(truncated ? 2 : 1); // missing second asset in damaged input
    out.put8U(0); out.putBe32U(0); out.put8U(0);
    out.putBe32U(MmtTlv::AssetType::hev1);
    out.put8U(0); out.put8U(1);
    out.put8U(0); out.putBe16U(packetId);
    out.putBe16U(0);
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
        std::puts("PASS: malformed MPT/ECM rejection and subsequent valid stream updates");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
