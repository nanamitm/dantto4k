#include "mmtTlvDemuxer.h"
#include "demuxerHandler.h"
#include <cstdio>
#include <stdexcept>

void subtitleDebugLog(const std::string&) {}
using namespace MmtTlv;
static void require(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
static std::vector<uint8_t> packet(const std::vector<uint8_t>& extension, uint16_t type = 0, bool present = true) {
    Common::WriteStream w;
    w.put8U(present ? 2 : 0); w.put8U(0x3F); w.putBe16U(0x100);
    w.putBe32U(0); w.putBe32U(1);
    if (present) { w.putBe16U(type); w.putBe16U(static_cast<uint16_t>(extension.size())); w.write(extension); }
    w.put8U(0x42);
    return w.getData();
}
class Decryptor : public CasHandler {
public:
    int calls = 0;
    bool decrypt(Mmtp& p) override {
        require(p.extensionHeaderScrambling && p.extensionHeaderScrambling->encryptionFlag == EncryptionFlag::EVEN,
                "must use the scrambling element, not another extension's data");
        ++calls; p.payload[0] = 0x24; return true;
    }
};
static void checkDump(const std::vector<uint8_t>& extension, size_t flags) {
    Common::WriteStream body; body.putBe16U(0); body.put8U(0x61); body.write(packet(extension));
    Common::WriteStream tlv; tlv.put8U(0x7F); tlv.put8U(3); tlv.putBe16U(static_cast<uint16_t>(body.getData().size())); tlv.write(body.getData());
    auto expected = tlv.getData();
    // TLV + compressed IP + MMTP fixed header + extension type/length.
    expected[4 + 3 + 12 + 4 + flags] &= ~0x18u;
    expected.back() = 0x24;
    MmtTlvDemuxer demux; DemuxerHandler handler; demux.setDemuxerHandler(handler);
    auto decryptor = std::make_unique<Decryptor>(); auto* raw = decryptor.get(); demux.setCasHandler(std::move(decryptor));
    std::vector<uint8_t> dump; demux.setDecodedDumpCallback([&](const uint8_t* p, size_t n) { dump.assign(p,p+n); });
    Common::ReadStream in(tlv.getData());
    require(demux.demux(in) == DemuxStatus::Ok && raw->calls == 1 && dump == expected,
            "decoded TLV must patch only the actual scrambling flags and payload");
    demux.setAssumeDescrambled(true); Common::ReadStream unchanged(tlv.getData()); demux.demux(unchanged);
    require(raw->calls == 1 && dump == tlv.getData(), "frontend-descrambled packets must remain byte-exact");
}
int main() {
 try {
    const std::vector<uint8_t> first = {0,1,0,1,0x14, 0x80,2,0,4,1,2,3,4};
    const std::vector<uint8_t> last = {0,2,0,4,1,2,3,4, 0x80,1,0,1,0x14};
    Mmtp p;
    for (const auto* ext : {&first,&last}) {
        auto bytes=packet(*ext); Common::ReadStream r(bytes);
        require(p.unpack(r) && p.extensionHeaderScrambling && p.payload == std::vector<uint8_t>{0x42}, "scrambling must be found in either position");
        require(p.scramblingFlagsOffset == (ext == &first ? 4 : 12), "flags offset must address the matched element");
    }
    checkDump(first,4); checkDump(last,12);
    auto opaque=packet(first,1); Common::ReadStream o(opaque);
    require(p.unpack(o) && !p.extensionHeaderScrambling && !p.scramblingFlagsOffset, "unknown outer extension must not be parsed as multi-type");
    auto plain=packet({},0,false); Common::ReadStream n(plain);
    require(p.unpack(n) && !p.extensionHeaderScrambling && !p.scramblingFlagsOffset, "reused parser must not keep scrambling state");
    // Malformed elements end extension parsing but must not drop the packet.
    for (const std::vector<uint8_t> bad : {std::vector<uint8_t>{0x80,1,0,2,0x10}, {0x80,1,0,0}, {0x80,1,0}}) {
        auto bytes=packet(bad); Common::ReadStream r(bytes);
        require(p.unpack(r) && !p.extensionHeaderScrambling && !p.scramblingFlagsOffset && p.payload == std::vector<uint8_t>{0x42},
                "malformed multi-type elements must keep the payload without scrambling info");
    }
    const std::vector<uint8_t> twice = {0,1,0,1,0x10, 0x80,1,0,1,0x18};
    auto twiceBytes=packet(twice); Common::ReadStream t(twiceBytes);
    require(p.unpack(t) && p.extensionHeaderScrambling && p.extensionHeaderScrambling->encryptionFlag == EncryptionFlag::EVEN &&
            p.scramblingFlagsOffset == 4, "the first scrambling element must win");
    checkDump(twice,4);
    const std::vector<uint8_t> truncatedTail = {0,1,0,1,0x14, 0,2,0};
    auto tailBytes=packet(truncatedTail); Common::ReadStream tail(tailBytes);
    require(p.unpack(tail) && p.extensionHeaderScrambling && p.scramblingFlagsOffset == 4 && p.payload == std::vector<uint8_t>{0x42},
            "a truncated element after the scrambling element must not discard it");
    std::puts("PASS: MMTP multi-type parsing, reuse, lenient malformed lengths and decoded dump patching"); return 0;
 } catch(const std::exception& e) { std::fprintf(stderr,"FAIL: %s\n",e.what()); return 1; }
}
