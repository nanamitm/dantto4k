#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace MmtTlv {

// Bounds-checked zlib (RFC 1950/1951) decoder for untrusted broadcast data.
// Fails instead of producing more than maxOutputSize bytes, and verifies the
// Adler-32 trailer. Small and slow; meant for the data broadcasting files,
// which are tens of kilobytes.
bool zlibInflate(const std::vector<uint8_t>& input, std::vector<uint8_t>& output, size_t maxOutputSize);

}
