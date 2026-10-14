#include "bufferedOutput.h"
#include <sstream>
#include <cstdio>
#include <stdexcept>

static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
class FailedWrite : public std::streambuf {
    std::streamsize xsputn(const char*, std::streamsize) override { return 0; }
};
class FailedFlush : public std::stringbuf {
    int sync() override { return -1; }
};
int main() {
    try {
        const uint8_t bytes[] = {0x47, 1, 2, 3};
        std::ostringstream success;
        BufferedOutput tail(success);
        tail.write(bytes, sizeof(bytes));
        require(success.str().empty(), "small output should be buffered");
        tail.flush(); success.flush();
        require(success.str() == std::string(reinterpret_cast<const char*>(bytes), sizeof(bytes)), "final tail lost");
        FailedWrite writeFailure;
        std::ostream failedWrite(&writeFailure);
        BufferedOutput output(failedWrite);
        output.write(bytes, sizeof(bytes)); output.flush();
        require(!failedWrite, "final buffered write error must be observable");
        FailedFlush flushFailure;
        std::ostream failedFlush(&flushFailure);
        BufferedOutput output2(failedFlush);
        output2.write(bytes, sizeof(bytes)); output2.flush();
        require(static_cast<bool>(failedFlush), "write should succeed before sink flush");
        failedFlush.flush();
        require(!failedFlush, "sink flush error must be observable");
        std::puts("PASS: buffered tail preservation, short writes and sink flush failures");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
