#include "mmtTlvDemuxer.h"
#include "remuxerHandler.h"
#include "config.h"
#include "adtsConverter.h"
#include "demuxerTeeHandler.h"
#include "mmtsMapWriter.h"
#include "tsVerifier.h"
#include "ntp.h"
#include <sstream>
#include <iterator>
#include <cstdio>
#include <stdexcept>

using namespace MmtTlv;
static void require(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
static std::vector<uint8_t> rawAudio() { return {0x40,0,0x11,0x90,0,8,0,0x20,0,0,0,0}; }
static void feedPacket(MmtTlvDemuxer& demux, uint16_t id, uint32_t seq, bool signaling, const std::vector<uint8_t>& payload) {
    Common::WriteStream body; body.putBe16U(0); body.put8U(0x61);
    body.put8U(0); body.put8U(signaling ? 2 : 0); body.putBe16U(id); body.putBe32U(0); body.putBe32U(seq); body.write(payload);
    Common::WriteStream tlv; tlv.put8U(0x7F); tlv.put8U(3); tlv.putBe16U(static_cast<uint16_t>(body.getData().size())); tlv.write(body.getData());
    Common::ReadStream r(tlv.getData()); require(demux.demux(r) == DemuxStatus::Ok,"synthetic packet must parse");
}
static void registerAudio(MmtTlvDemuxer& demux, bool surround = false, uint16_t service = 1, uint16_t id = 0x100, uint32_t seq = 1) {
    Common::WriteStream desc;
    desc.putBe16U(0x8011); desc.put8U(2); desc.putBe16U(0x10);
    desc.putBe16U(1); desc.put8U(12); desc.putBe32U(1); desc.putBe64U(2208988801ULL << 32);
    desc.putBe16U(0x8026); desc.put8U(1+4+2+8+8); desc.put8U(0xFB); desc.putBe32U(48000); desc.putBe16U(1024);
    desc.putBe32U(1); desc.put8U(0); desc.putBe16U(0); desc.put8U(4); for(int i=0;i<4;++i)desc.putBe16U(0);
    if (surround) {
        desc.putBe16U(0x8014); desc.put8U(10); desc.put8U(2); desc.put8U(0x11); desc.putBe16U(0x10);
        desc.put8U(0x11); desc.put8U(0); desc.put8U(0x0E); desc.write({0x6A,0x70,0x6E});
    }
    Common::WriteStream table;
    table.put8U(0x20); table.put8U(0); table.putBe16U(static_cast<uint16_t>(7+17+desc.getData().size()));
    table.put8U(0); table.put8U(2); table.putBe16U(service); table.putBe16U(0); table.put8U(1);
    table.put8U(0); table.putBe32U(0); table.put8U(0); table.putBe32U(AssetType::mp4a);
    table.put8U(0); table.put8U(1); table.put8U(0); table.putBe16U(id);
    table.putBe16U(static_cast<uint16_t>(desc.getData().size())); table.write(desc.getData());
    // PA message with zero table-summary entries; its body holds this MPT.
    Common::WriteStream signaling; signaling.put8U(0); signaling.put8U(0);
    signaling.putBe16U(0); signaling.put8U(0); signaling.putBe32U(static_cast<uint32_t>(1+table.getData().size()));
    signaling.put8U(0); signaling.write(table.getData()); feedPacket(demux,0,seq,true,signaling.getData());
    require(demux.getStream(id),"audio MPT registration failed");
}
static void audio(MmtTlvDemuxer& demux, uint32_t seq, FragmentationIndicator fi, const std::vector<uint8_t>& raw) {
    Common::WriteStream mpu;
    mpu.putBe16U(static_cast<uint16_t>(6+14+raw.size())); mpu.put8U(static_cast<uint8_t>(0x28 | (static_cast<unsigned>(fi)<<1)));
    mpu.put8U(0); mpu.putBe32U(1); mpu.putBe32U(0); mpu.putBe32U(1); mpu.putBe32U(0);
    mpu.put8U(0); mpu.put8U(0); mpu.write(raw); feedPacket(demux,0x100,seq,false,mpu.getData());
}
static std::vector<std::vector<uint8_t>> pesPackets(const std::vector<uint8_t>& ts, uint16_t pid) {
    require(ts.size()%188==0,"TS must contain whole packets");
    std::vector<std::vector<uint8_t>> result;
    for(size_t i=0;i<ts.size();i+=188) {
        const auto* p=ts.data()+i; require(p[0]==0x47,"TS sync lost");
        if ((((p[1]&0x1F)<<8)|p[2]) != pid || !(p[3]&0x10))continue;
        size_t off=4; if(p[3]&0x20)off+=1+p[4]; require(off<=188,"invalid adaptation field");
        if(p[1]&0x40)result.emplace_back(); require(!result.empty(),"PES continuation before start");
        result.back().insert(result.back().end(),p+off,p+188);
    }
    return result;
}
static uint64_t pts(const std::vector<uint8_t>& p) {
    return (uint64_t((p[9]>>1)&7)<<30) | (uint64_t(p[10])<<22) | (uint64_t(p[11]>>1)<<15) | (uint64_t(p[12])<<7) | (p[13]>>1);
}
static void checkPes(const std::vector<uint8_t>& pes, const std::vector<uint8_t>& expected, uint64_t timestamp) {
    require(pes.size()>=14 && pes[0]==0 && pes[1]==0 && pes[2]==1 && pes[3]==0xC0,"invalid audio PES header");
    require(((pes[4]<<8)|pes[5])==pes.size()-6,"audio PES length must equal actual size and must not be zero");
    require((pes[7]&0xC0)==0x80 && pts(pes)==timestamp,"audio timestamp must be captured at the first fragment");
    const size_t off=9+pes[8]; require(off<=pes.size(),"PES header length");
    require(std::vector<uint8_t>(pes.begin()+off,pes.end())==expected,"audio payload must be complete and byte-exact");
}
static std::vector<uint8_t> expectedAudio(bool loas) {
    auto raw=rawAudio(); std::vector<uint8_t> framed={0x56,0xE0,static_cast<uint8_t>(raw.size())}; framed.insert(framed.end(),raw.begin(),raw.end());
    if(loas)return framed;
    ADTSConverter converter;std::vector<uint8_t> result;require(converter.convert(framed.data(),framed.size(),result),"fixture must convert to ADTS");return result;
}
static void testSplit(size_t split, bool loas, bool surround) {
    config.disableADTSConversion=loas; MmtTlvDemuxer demux; RemuxerHandler remux(demux); demux.setDemuxerHandler(remux);
    registerAudio(demux,surround);std::vector<uint8_t> output;
    remux.setOutputCallback([&](const uint8_t* p,size_t n){output.insert(output.end(),p,p+n);});
    const auto raw=rawAudio(); audio(demux,1,FragmentationIndicator::FirstFragment,{raw.begin(),raw.begin()+split});
    require(output.empty(),"incomplete audio must not escape to remuxer");
    audio(demux,2,FragmentationIndicator::LastFragment,{raw.begin()+split,raw.end()});
    audio(demux,3,FragmentationIndicator::NotFragmented,raw);
    auto packets=pesPackets(output,demux.getStream(0x100)->getMpeg2PacketId());
    require(packets.size()==2 && remux.getAdtsDropCount()==0,"split and complete frames must both produce audio");
    auto expected=expectedAudio(loas||surround);checkPes(packets[0],expected,90000);checkPes(packets[1],expected,91920);
}
static void testMultiServicePmtAndMap() {
    config.disableADTSConversion=false;
    MmtTlvDemuxer demux; RemuxerHandler remux(demux); MmtsMapWriter writer; writer.setDemuxer(demux);
    const auto mapPath=std::filesystem::current_path()/"audio-transport-test.mmtsmap.txt";
    require(writer.open(mapPath,MmtsMapWriter::Format::Text),"map open failed");
    DemuxerTeeHandler tee(remux,writer); demux.setDemuxerHandler(tee);
    MhSdt sdt{}; sdt.tlvStreamId=1; sdt.currentNextIndicator=true;
    sdt.services.push_back(std::make_shared<MhSdt::Service>()); sdt.services.back()->serviceId=1;
    remux.onMhSdtActual(sdt);
    Plt plt{};
    for(uint16_t service : {1,2}) {
        Plt::Entry entry{}; entry.mmtPackageIdByte={0,static_cast<uint8_t>(service)};
        entry.locationInfos.locationType=0; plt.entries.push_back(std::move(entry));
    }
    remux.onPlt(plt);
    std::vector<uint8_t> output;remux.setOutputCallback([&](const uint8_t* p,size_t n){output.insert(output.end(),p,p+n);});
    registerAudio(demux,false,1,0x100,1);registerAudio(demux,false,2,0x200,2);registerAudio(demux,false,1,0x100,3);
    const auto* first=demux.getStream(0x100);const auto* second=demux.getStream(0x200);
    require(first && second && first->getMpeg2PacketId()!=second->getMpeg2PacketId(),"service media PIDs must be distinct");
    for(const auto& [pid,stream] : std::vector<std::pair<uint16_t,const MmtStream*>>{{0x1000,first},{0x1001,second}}) {
        const auto sections=pesPackets(output,pid);require(!sections.empty(),"PMT missing for service");
        const auto& payload=sections.back();const auto off=1+payload[0];require(payload.size()>off+16,"PMT too short");
        const auto* section=payload.data()+off;require(section[0]==2,"expected PMT section");
        const size_t es=12+(((section[10]&15)<<8)|section[11]);
        require(off+es+5<=payload.size() && section[es]==0x0F &&
            (((section[es+1]&31)<<8)|section[es+2])==stream->getMpeg2PacketId(),"PMT must resolve its own package's stream by packet_id");
    }
    writer.close();std::ifstream file(mapPath);const std::string map((std::istreambuf_iterator<char>(file)),{});file.close();std::filesystem::remove(mapPath);
    require(map.find("streamIndex=0 packetId=0x100")!=std::string::npos &&
            map.find("streamIndex=1 packetId=0x200")!=std::string::npos &&
            map.find("streamIndex=0 packetId=0x200")==std::string::npos,"map indices must match globally registered streams");
}
static void testMiddleAndMalformed() {
    config.disableADTSConversion=false;MmtTlvDemuxer demux;RemuxerHandler remux(demux);demux.setDemuxerHandler(remux);registerAudio(demux);
    std::vector<uint8_t> output;remux.setOutputCallback([&](const uint8_t* p,size_t n){output.insert(output.end(),p,p+n);});
    const auto raw=rawAudio();audio(demux,1,FragmentationIndicator::FirstFragment,{raw.begin(),raw.begin()+1});
    // An unrelated service's MPT must not discard this partially received AU.
    registerAudio(demux,false,2,0x200,2);
    audio(demux,2,FragmentationIndicator::MiddleFragment,{raw.begin()+1,raw.begin()+6});require(output.empty(),"middle fragment must be buffered");
    audio(demux,3,FragmentationIndicator::LastFragment,{raw.begin()+6,raw.end()});
    auto packets=pesPackets(output,demux.getStream(0x100)->getMpeg2PacketId());require(packets.size()==1,"three-fragment frame missing");checkPes(packets[0],expectedAudio(false),90000);
    output.clear();audio(demux,4,FragmentationIndicator::FirstFragment,{raw.begin(),raw.begin()+6});
    audio(demux,5,FragmentationIndicator::LastFragment,{raw.begin()+6,raw.end()-1});require(output.empty(),"truncated frame must be discarded");
    audio(demux,6,FragmentationIndicator::NotFragmented,raw);packets=pesPackets(output,demux.getStream(0x100)->getMpeg2PacketId());
    require(packets.size()==1,"valid frame must recover after truncation");checkPes(packets[0],expectedAudio(false),93840);
}
// Exercise the exact 13-bit LOAS limit, including a multi-byte LATM length.
static void testMaximumFrame() {
    config.disableADTSConversion=true; MmtTlvDemuxer demux; RemuxerHandler remux(demux); demux.setDemuxerHandler(remux);registerAudio(demux);
    std::vector<uint8_t> raw={0x40,0,0x11,0x90,0,8,0};size_t bit=53;
    auto put=[&](uint8_t byte){for(int shift=7;shift>=0;--shift){if(bit/8>=raw.size())raw.push_back(0);raw[bit/8]|=((byte>>shift)&1)<<(7-bit%8);++bit;}};
    size_t length=8152;while(length>=255){put(255);length-=255;}put(static_cast<uint8_t>(length));
    for(size_t i=0;i<8152;++i)put(0);require(raw.size()==8191,"maximum-size fixture mismatch");
    std::vector<uint8_t> output;remux.setOutputCallback([&](const uint8_t* p,size_t n){output.insert(output.end(),p,p+n);});
    audio(demux,1,FragmentationIndicator::FirstFragment,{raw.begin(),raw.begin()+7});
    audio(demux,2,FragmentationIndicator::MiddleFragment,{raw.begin()+7,raw.begin()+14});
    audio(demux,3,FragmentationIndicator::LastFragment,{raw.begin()+14,raw.end()});
    auto packets=pesPackets(output,demux.getStream(0x100)->getMpeg2PacketId());require(packets.size()==1,"8191-byte frame must survive");
    std::vector<uint8_t> expected={0x56,0xFF,0xFF};expected.insert(expected.end(),raw.begin(),raw.end());checkPes(packets[0],expected,90000);
}
static void testRecovery() {
    config.disableADTSConversion=false; MmtTlvDemuxer demux;RemuxerHandler remux(demux);demux.setDemuxerHandler(remux);registerAudio(demux);
    std::vector<uint8_t> output;remux.setOutputCallback([&](const uint8_t* p,size_t n){output.insert(output.end(),p,p+n);});
    const auto raw=rawAudio();
    audio(demux,1,FragmentationIndicator::FirstFragment,{raw.begin(),raw.begin()+2});
    // Sequence 2 is lost. The incomplete frame must never be emitted.
    audio(demux,3,FragmentationIndicator::LastFragment,{raw.begin()+2,raw.end()});
    require(output.empty(),"packet loss must discard the partial audio frame");
    audio(demux,4,FragmentationIndicator::NotFragmented,raw);
    auto packets=pesPackets(output,demux.getStream(0x100)->getMpeg2PacketId());require(packets.size()==1,"audio must recover after loss");checkPes(packets[0],expectedAudio(false),91920);
    output.clear();audio(demux,5,FragmentationIndicator::NotFragmented,std::vector<uint8_t>(8192));
    require(output.empty(),"oversized frame must be rejected");
    audio(demux,6,FragmentationIndicator::NotFragmented,raw);
    packets=pesPackets(output,demux.getStream(0x100)->getMpeg2PacketId());require(packets.size()==1,"audio must recover after an oversized frame");checkPes(packets[0],expectedAudio(false),95760);
}
// STD-B60 8.2.1: the LOAS length is the MFU size, so trailing bytes after the
// payload and frames that reuse the previous StreamMuxConfig must pass.
static void testLenientFrames() {
    for(bool loas : {false,true}) {
        config.disableADTSConversion=loas; MmtTlvDemuxer demux;RemuxerHandler remux(demux);demux.setDemuxerHandler(remux);registerAudio(demux);
        std::vector<uint8_t> output;remux.setOutputCallback([&](const uint8_t* p,size_t n){output.insert(output.end(),p,p+n);});
        auto padded=rawAudio();padded.push_back(0);
        audio(demux,1,FragmentationIndicator::FirstFragment,{padded.begin(),padded.begin()+4});
        audio(demux,2,FragmentationIndicator::LastFragment,{padded.begin()+4,padded.end()});
        auto packets=pesPackets(output,demux.getStream(0x100)->getMpeg2PacketId());require(packets.size()==1,"padded frame must survive");
        std::vector<uint8_t> expected=expectedAudio(false);
        if(loas){expected={0x56,0xE0,static_cast<uint8_t>(padded.size())};expected.insert(expected.end(),padded.begin(),padded.end());}
        checkPes(packets[0],expected,90000);
    }
    config.disableADTSConversion=true; MmtTlvDemuxer demux;RemuxerHandler remux(demux);demux.setDemuxerHandler(remux);registerAudio(demux);
    std::vector<uint8_t> output;remux.setOutputCallback([&](const uint8_t* p,size_t n){output.insert(output.end(),p,p+n);});
    // useSameStreamMux=1, PayloadLengthInfo=3, payload AA BB CC.
    const std::vector<uint8_t> same={0x81,0xD5,0x5D,0xE6,0x00};
    audio(demux,1,FragmentationIndicator::NotFragmented,same);
    audio(demux,2,FragmentationIndicator::NotFragmented,{same.begin(),same.end()-1});
    auto packets=pesPackets(output,demux.getStream(0x100)->getMpeg2PacketId());
    require(packets.size()==1,"useSameStreamMux frame must survive and its truncation must not");
    std::vector<uint8_t> expected={0x56,0xE0,5};expected.insert(expected.end(),same.begin(),same.end());checkPes(packets[0],expected,90000);
}
static Plt pltOf(std::initializer_list<uint16_t> services) {
    Plt plt{};
    for(uint16_t service : services) {
        Plt::Entry entry{}; entry.mmtPackageIdByte={static_cast<uint8_t>(service>>8),static_cast<uint8_t>(service)};
        entry.locationInfos.locationType=0; plt.entries.push_back(std::move(entry));
    }
    return plt;
}
static void announceStream(RemuxerHandler& remux) {
    MhSdt sdt{}; sdt.tlvStreamId=1; sdt.currentNextIndicator=true;
    sdt.services.push_back(std::make_shared<MhSdt::Service>()); sdt.services.back()->serviceId=1;
    remux.onMhSdtActual(sdt);
}
static void testPmtPidSurvivesPltChanges() {
    config.disableADTSConversion=false;
    MmtTlvDemuxer demux; RemuxerHandler remux(demux); demux.setDemuxerHandler(remux);
    announceStream(remux);
    remux.onPlt(pltOf({1,2}));
    std::vector<uint8_t> output;remux.setOutputCallback([&](const uint8_t* p,size_t n){output.insert(output.end(),p,p+n);});
    remux.onPlt(pltOf({2,1}));
    registerAudio(demux,false,1,0x100,1);
    require(!pesPackets(output,0x1000).empty() && pesPackets(output,0x1001).empty(),"a reordered PLT must not move a service's PMT");
    output.clear();
    remux.onPlt(pltOf({2}));remux.onPlt(pltOf({3,2,1}));
    registerAudio(demux,false,1,0x100,2);
    require(!pesPackets(output,0x1000).empty(),"a service that returns to the PLT must get its PMT PID back");
    remux.clear();announceStream(remux);output.clear();
    remux.onPlt(pltOf({2,1}));registerAudio(demux,false,1,0x100,3);
    require(!pesPackets(output,0x1001).empty(),"clear must forget the assigned PMT PIDs");
}
static void testVerifierAcceptsRemuxerOutput() {
    for(bool loas : {false,true}) {
        config.disableADTSConversion=loas;
        MmtTlvDemuxer demux; RemuxerHandler remux(demux); demux.setDemuxerHandler(remux);
        auto verifier=std::make_unique<TsVerifier>();remux.setOutputCallback([&](const uint8_t* p,size_t n){verifier->feed(p,n);});
        announceStream(remux);remux.onPlt(pltOf({1}));registerAudio(demux);
        audio(demux,1,FragmentationIndicator::NotFragmented,rawAudio());
        std::ostringstream report;verifier->print(report);
        const auto* stats=verifier->getPid(demux.getStream(0x100)->getMpeg2PacketId());
        require(stats && stats->pes==1 && stats->listedInPmt,report.str().c_str());
        require(stats->ccErrors==0 && stats->pesLengthErrors==0 && stats->pesHeaderErrors==0 && stats->frameErrors==0,report.str().c_str());
        require(verifier->getPid(0)->crcErrors==0 && verifier->getPid(0x1000)->crcErrors==0,report.str().c_str());
    }
}
static void testConversionReport() {
    config.disableADTSConversion=false;
    MmtTlvDemuxer demux; RemuxerHandler remux(demux); demux.setDemuxerHandler(remux);
    announceStream(remux);remux.onPlt(pltOf({1}));
    Mpt mpt{};mpt.mmtPackageIdByte={0,1};
    Mpt::Asset unknown{};unknown.assetType=0x61626364;
    MmtGeneralLocationInfo location{};location.locationType=0;location.packetId=0x777;
    unknown.locationInfos.push_back(location);mpt.assets.push_back(std::move(unknown));
    remux.onMpt(mpt);
    std::ostringstream out;remux.printConversionReport(out);const auto text=out.str();
    require(text.find("service 0x0001 packet_id 0x0777 abcd: unsupported asset type")!=std::string::npos,"an asset left out of the PMT must be named with its reason");
    require(text.find("dropped")==std::string::npos,"a drop that did not happen must not be listed");
}
static void testPcrIntervalStaysWithinLimit() {
    for(double frameMs : {1000.0/59.94,1000.0/29.97,1000.0/23.976}) {
        MmtTlvDemuxer demux; RemuxerHandler remux(demux);
        auto verifier=std::make_unique<TsVerifier>();remux.setOutputCallback([&](const uint8_t* p,size_t n){verifier->feed(p,n);});
        for(int frame=0;frame<600;++frame) {
            const double ms=frame*frameMs;
            NTPv4 ntp{};
            ntp.transmit_timestamp.seconds=2208988800u+1000u+static_cast<uint32_t>(ms/1000);
            ntp.transmit_timestamp.fraction=static_cast<uint32_t>((ms/1000-static_cast<uint32_t>(ms/1000))*4294967296.0);
            remux.onNtp(ntp);
        }
        const auto* pcr=verifier->getPid(PCR_PID);
        require(pcr && pcr->pcrCount>1,"a regular clock must emit PCR");
        require(pcr->largestPcrInterval90k<=TsVerifier::kPcrWarningInterval90k && pcr->pcrOverWarning==0,"PCR interval must stay within 100 ms");
        require(pcr->largestPcrInterval90k>=90*50,"PCR must stay sparse: at least 50 ms apart");
    }
}
int main() {
 try {
    for(size_t split=1;split<rawAudio().size();++split) {
        testSplit(split,false,false);testSplit(split,true,false);testSplit(split,false,true);
    }
    testRecovery();testMiddleAndMalformed();testMaximumFrame();testMultiServicePmtAndMap();testLenientFrames();testPmtPidSurvivesPltChanges();testVerifierAcceptsRemuxerOutput();testConversionReport();testPcrIntervalStaysWithinLimit();
    std::puts("PASS: fragmented ADTS/LOAS audio, 22.2ch routing, timestamps, PES lengths and loss/size recovery");return 0;
 } catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
