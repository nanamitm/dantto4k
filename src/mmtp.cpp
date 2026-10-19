#include "mmtp.h"
#include "extensionHeaderScrambling.h"

namespace MmtTlv {

bool Mmtp::unpack(Common::ReadStream& stream) {
	try {
		uint8_t uint8 = stream.get8U();
		version = (uint8 & 0b11000000) >> 6;
		packetCounterFlag = (uint8 & 0b00100000) >> 5;
		fecType = (uint8 & 0b00011000) >> 3;
		reserved1 = (uint8 & 0b00000100) >> 2;
		extensionHeaderFlag = (uint8 & 0b00000010) >> 1;
		rapFlag = uint8 & 0b00000001;

		uint8 = stream.get8U();
		reserved2 = (uint8 & 0b11000000) >> 6;
		payloadType = static_cast<PayloadType>(uint8 & 0b00111111);

		packetId = stream.getBe16U();
		deliveryTimestamp = stream.getBe32U();
		packetSequenceNumber = stream.getBe32U();

		if (packetCounterFlag) {
			if (stream.leftBytes() < 4) {
				return false;
			}
			packetCounter = stream.getBe32U();
		}

		// The demuxer reuses one Mmtp for every packet, so a value left over
		// from an earlier packet would be read as this packet's own. Only the
		// scrambling sub-type below fills this in; every other extension header
		// must leave it empty.
		extensionHeaderScrambling = std::nullopt;
		scramblingFlagsOffset.reset();

		if (extensionHeaderFlag) {
			if (stream.leftBytes() < 4) {
				return false;
			}
			extensionHeaderType = stream.getBe16U();
			extensionHeaderLength = stream.getBe16U();

			if (stream.leftBytes() < extensionHeaderLength) {
				return false;
			}
			extensionHeaderField.resize(extensionHeaderLength);
			stream.read(extensionHeaderField.data(), extensionHeaderLength);

            // STD-B60 Table 6-8: extension_type=0 contains a sequence of
            // independently typed/length-delimited elements, in any order.
            if (extensionHeaderType == 0) {
                Common::ReadStream extensions(extensionHeaderField);
                while (!extensions.isEof()) {
                    const auto type = extensions.getBe16U();
                    const auto length = extensions.getBe16U();
                    Common::ReadStream element(extensions, length);
                    if ((type & 0x7FFF) == 1) {
                        // Multiple scrambling elements would make the flags
                        // to decrypt/patch ambiguous. Do not guess.
                        if (extensionHeaderScrambling) {
                            return false;
                        }
                        ExtensionHeaderScrambling scrambling;
                        if (!scrambling.unpack(element, extensionHeaderType, length)) {
                            return false;
                        }
                        scramblingFlagsOffset = extensions.getPos();
                        extensionHeaderScrambling = scrambling;
                    }
                    extensions.skip(length);
                    if (type & 0x8000) {
                        break;
                    }
                }
            }
		}

		payload.resize(stream.leftBytes());
		stream.read(payload.data(), stream.leftBytes());
	}
	catch (const std::out_of_range&) {
		return false;
	}

	return true;
}

}