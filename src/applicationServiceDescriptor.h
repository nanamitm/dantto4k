#pragma once
#include "mmtDescriptorBase.h"
#include "mmtGeneralLocationInfo.h"
#include <optional>
#include <vector>

namespace MmtTlv {

// ARIB STD-B60 7.4.3.39 Application service descriptor. Placed in the MPT of a
// service that carries data broadcasting, it tells where that service's MH-AIT,
// data transmission messages (DDMT/DAMT/DCCT) and EMTs are sent.
//
// The descriptor tag table and bit widths of the English translation extract
// misaligned from the PDF; the tag (0x8034) and layout here follow BS 4K
// broadcasts. The flag bits after document_resolution were all set in every
// sample, so their order within that nibble is not confirmed by data.
class ApplicationServiceDescriptor
    : public MmtDescriptorTemplate<0x8034> {
public:
    bool unpack(Common::ReadStream& stream) override;

    struct Emt {
        uint8_t emtTag;
        MmtGeneralLocationInfo locationInfo;
    };

    uint8_t applicationFormat;
    uint8_t documentResolution;
    bool defaultAitFlag;
    bool dtMessageFlag;
    MmtGeneralLocationInfo aitLocationInfo;
    std::optional<MmtGeneralLocationInfo> dtMessageLocationInfo;
    std::vector<Emt> emts;
};

}
