#include "applicationServiceDescriptor.h"

namespace MmtTlv {

bool ApplicationServiceDescriptor::unpack(Common::ReadStream& stream) {
    try {
        if (!MmtDescriptorTemplate::unpack(stream)) {
            return false;
        }

        Common::ReadStream nstream(stream, descriptorLength);

        uint8_t uint8 = nstream.get8U();
        applicationFormat = (uint8 & 0b11110000) >> 4;

        uint8 = nstream.get8U();
        documentResolution = (uint8 & 0b11110000) >> 4;
        defaultAitFlag = (uint8 & 0b00000100) >> 2;
        dtMessageFlag = (uint8 & 0b00000010) >> 1;

        uint8 = nstream.get8U();
        const uint8_t emtNum = uint8 & 0b00001111;

        if (!aitLocationInfo.unpack(nstream)) {
            return false;
        }

        dtMessageLocationInfo.reset();
        if (dtMessageFlag) {
            MmtGeneralLocationInfo locationInfo;
            if (!locationInfo.unpack(nstream)) {
                return false;
            }
            dtMessageLocationInfo = std::move(locationInfo);
        }

        emts.clear();
        for (int i = 0; i < emtNum; i++) {
            Emt emt;
            emt.emtTag = nstream.get8U();
            if (!emt.locationInfo.unpack(nstream)) {
                return false;
            }
            emts.push_back(std::move(emt));
        }

        stream.skip(descriptorLength);
    }
    catch (const std::out_of_range&) {
        return false;
    }

    return true;
}

}
