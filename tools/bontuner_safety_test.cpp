#include "casProxyClient.h"
#include "bonDriverContext.h"
#include "ntp.h"
#include <array>
#include <cstdio>
#include <stdexcept>

BonDriverContext g_bonDriverContext;

static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

class FakeDriver : public CBonTuner {
public:
    std::vector<uint8_t> bytes{0x7f, 0xff, 0, 0};
    bool available = true;
    const bool GetTsStream(uint8_t** data, uint32_t* size, uint32_t* remain) override {
        // Leave stale raw data in outputs even when returning false.
        *data = bytes.data(); *size = static_cast<uint32_t>(bytes.size()); *remain = 0;
        const bool result = available;
        available = false;
        return result;
    }
    const bool SetChannel(uint32_t, uint32_t) override { return true; }
    void PurgeTsStream() override {}
};

class Tuner : public CBonTuner {
public:
    void attach(IBonDriver2& driver) { pBonDriver2 = &driver; }
    size_t bufferedInput() const { return inputBuffer.size(); }
};

static MmtTlv::NTPv4 clockAt(uint32_t seconds) {
    MmtTlv::NTPv4 ntp{};
    ntp.transmit_timestamp.seconds = 2208988800u + seconds;
    return ntp;
}

int main() {
    try {
        FakeDriver driver;
        Tuner tuner;
        uint8_t* data = reinterpret_cast<uint8_t*>(1);
        uint32_t size = 99, remain = 99;
        require(!tuner.GetTsStream(&data, &size, &remain) && !data && size == 0 && remain == 0,
                "uninitialized tuner must return empty outputs");
        tuner.attach(driver);
        data = reinterpret_cast<uint8_t*>(1); size = 99; remain = 99;
        require(!tuner.GetTsStream(&data, &size, &remain) && !data && size == 0 && remain == 0,
                "MMTS without remuxed TS must never escape through pointer outputs");
        std::array<uint8_t, 188> destination;
        destination.fill(0xcc);
        const auto sentinel = destination;
        driver.available = true;
        require(!tuner.GetTsStream(destination.data(), &size, &remain) && destination == sentinel && size == 0,
                "failed copy variant must leave destination untouched");
        std::vector<uint8_t> expected(188, 0xaa);
        expected[0] = 0x47;
        g_bonDriverContext.remuxOutput = expected;
        require(tuner.GetTsStream(destination.data(), &size, &remain) && size == 188 &&
                std::equal(expected.begin(), expected.end(), destination.begin()),
                "successful copy variant must return only remuxed TS");

        driver.bytes = {0x7f, 3, 0}; // incomplete TLV retained until more input
        driver.available = true;
        require(!tuner.GetTsStream(&data, &size, &remain) && tuner.bufferedInput() == 3,
                "incomplete TLV must stay internal");
        int pcrPackets = 0;
        g_bonDriverContext.handler.setOutputCallback([&](const uint8_t* packet, size_t count) {
            if (count == 188 && (((packet[1] & 0x1f) << 8) | packet[2]) == PCR_PID) ++pcrPackets;
        });
        g_bonDriverContext.handler.onNtp(clockAt(10000));
        require(pcrPackets == 1, "initial clock must emit PCR");
        tuner.PurgeTsStream();
        require(tuner.bufferedInput() == 0, "purge must discard incomplete input");
        g_bonDriverContext.handler.onNtp(clockAt(100));
        require(pcrPackets == 2, "purge must reset PCR suppression for an earlier clock");
        require(tuner.SetChannel(0, 1), "fake channel selection failed");
        g_bonDriverContext.handler.onNtp(clockAt(10));
        require(pcrPackets == 3, "channel change must reset PCR suppression");
        std::puts("PASS: empty failure outputs, untouched copy buffer, TS output and purge/channel PCR resets");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
