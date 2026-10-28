#pragma once
#include "mmtTableBase.h"

namespace MmtTlv {

// Entitlement updates (0x84). 0x85 carries messages, not entitlement updates.
class Emm : public MmtTableBase {
public:
    bool unpack(Common::ReadStream& stream) override;
    bool currentNextIndicator = false;
    std::vector<std::vector<uint8_t>> messages;
};

}
