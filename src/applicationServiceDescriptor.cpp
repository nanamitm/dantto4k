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

        // default_AIT_flag(1), DT_message_flag(1), reserved_future_use(2), EMT_num(4)
        uint8 = nstream.get8U();
        defaultAitFlag = (uint8 & 0b10000000) >> 7;
        dtMessageFlag = (uint8 & 0b01000000) >> 6;
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
