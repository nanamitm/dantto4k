#include "zlibInflate.h"
#include <array>
#include <stdexcept>

namespace MmtTlv {

namespace {

constexpr int kMaxBits = 15;
constexpr int kMaxLengthCodes = 286;
constexpr int kMaxDistanceCodes = 30;
constexpr int kFixedLengthCodes = 288;

struct InflateError : std::runtime_error {
    InflateError() : std::runtime_error("inflate") {}
};

struct Huffman {
    std::array<uint16_t, kMaxBits + 1> count{};
    std::array<uint16_t, kFixedLengthCodes> symbol{};
};

class Inflater {
public:
    Inflater(const std::vector<uint8_t>& input, size_t position, std::vector<uint8_t>& output, size_t maxOutputSize)
        : input(input), position(position), output(output), maxOutputSize(maxOutputSize) {}

    void run() {
        bool last;
        do {
            last = bits(1) != 0;
            switch (bits(2)) {
            case 0: stored(); break;
            case 1: fixed(); break;
            case 2: dynamic(); break;
            default: throw InflateError();
            }
        } while (!last);
    }

    // Position of the first whole byte after the deflate stream.
    size_t endPosition() const { return position; }

private:
    uint32_t bits(int need) {
        uint32_t value = bitBuffer;
        while (bitCount < need) {
            if (position >= input.size()) {
                throw InflateError();
            }
            value |= static_cast<uint32_t>(input[position++]) << bitCount;
            bitCount += 8;
        }
        bitBuffer = value >> need;
        bitCount -= need;
        return value & ((1u << need) - 1);
    }

    void put(uint8_t byte) {
        if (output.size() >= maxOutputSize) {
            throw InflateError();
        }
        output.push_back(byte);
    }

    void stored() {
        bitBuffer = 0;
        bitCount = 0;
        if (input.size() - position < 4) {
            throw InflateError();
        }
        const uint32_t length = input[position] | (input[position + 1] << 8);
        const uint32_t complement = input[position + 2] | (input[position + 3] << 8);
        position += 4;
        if (length != (~complement & 0xffff) || input.size() - position < length) {
            throw InflateError();
        }
        for (uint32_t i = 0; i < length; i++) {
            put(input[position++]);
        }
    }

    // Returns the number of unused codes: negative when over-subscribed,
    // positive when incomplete, zero for a complete code.
    static int construct(Huffman& h, const uint16_t* lengths, int n) {
        h.count.fill(0);
        for (int symbol = 0; symbol < n; symbol++) {
            h.count[lengths[symbol]]++;
        }
        if (h.count[0] == n) {
            return 0;
        }
        int left = 1;
        for (int len = 1; len <= kMaxBits; len++) {
            left <<= 1;
            left -= h.count[len];
            if (left < 0) {
                return left;
            }
        }
        std::array<uint16_t, kMaxBits + 1> offsets{};
        for (int len = 1; len < kMaxBits; len++) {
            offsets[len + 1] = offsets[len] + h.count[len];
        }
        for (int symbol = 0; symbol < n; symbol++) {
            if (lengths[symbol] != 0) {
                h.symbol[offsets[lengths[symbol]]++] = static_cast<uint16_t>(symbol);
            }
        }
        return left;
    }

    int decode(const Huffman& h) {
        int code = 0;
        int first = 0;
        int index = 0;
        for (int len = 1; len <= kMaxBits; len++) {
            code |= static_cast<int>(bits(1));
            const int count = h.count[len];
            if (code - count < first) {
                return h.symbol[index + (code - first)];
            }
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        throw InflateError();
    }

    void codes(const Huffman& lengthCode, const Huffman& distanceCode) {
        static constexpr uint16_t lengthBase[29] = {
            3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
            35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
        static constexpr uint8_t lengthExtra[29] = {
            0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
            3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
        static constexpr uint16_t distanceBase[30] = {
            1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
            257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
            8193, 12289, 16385, 24577 };
        static constexpr uint8_t distanceExtra[30] = {
            0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
            7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

        while (true) {
            int symbol = decode(lengthCode);
            if (symbol < 256) {
                put(static_cast<uint8_t>(symbol));
                continue;
            }
            if (symbol == 256) {
                return;
            }
            symbol -= 257;
            if (symbol >= 29) {
                throw InflateError();
            }
            const size_t length = lengthBase[symbol] + bits(lengthExtra[symbol]);
            symbol = decode(distanceCode);
            if (symbol >= 30) {
                throw InflateError();
            }
            const size_t distance = distanceBase[symbol] + bits(distanceExtra[symbol]);
            if (distance > output.size()) {
                throw InflateError();
            }
            for (size_t i = 0; i < length; i++) {
                put(output[output.size() - distance]);
            }
        }
    }

    void fixed() {
        static const auto tables = [] {
            std::pair<Huffman, Huffman> t;
            uint16_t lengths[kFixedLengthCodes];
            int symbol = 0;
            for (; symbol < 144; symbol++) lengths[symbol] = 8;
            for (; symbol < 256; symbol++) lengths[symbol] = 9;
            for (; symbol < 280; symbol++) lengths[symbol] = 7;
            for (; symbol < kFixedLengthCodes; symbol++) lengths[symbol] = 8;
            construct(t.first, lengths, kFixedLengthCodes);
            for (symbol = 0; symbol < kMaxDistanceCodes; symbol++) lengths[symbol] = 5;
            construct(t.second, lengths, kMaxDistanceCodes);
            return t;
        }();
        codes(tables.first, tables.second);
    }

    void dynamic() {
        static constexpr uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
        const int lengthCount = static_cast<int>(bits(5)) + 257;
        const int distanceCount = static_cast<int>(bits(5)) + 1;
        const int codeCount = static_cast<int>(bits(4)) + 4;
        if (lengthCount > kMaxLengthCodes || distanceCount > kMaxDistanceCodes) {
            throw InflateError();
        }

        uint16_t lengths[kMaxLengthCodes + kMaxDistanceCodes]{};
        int index = 0;
        for (; index < codeCount; index++) {
            lengths[order[index]] = static_cast<uint16_t>(bits(3));
        }
        for (; index < 19; index++) {
            lengths[order[index]] = 0;
        }

        Huffman lengthCode;
        Huffman distanceCode;
        if (construct(lengthCode, lengths, 19) != 0) {
            throw InflateError();
        }

        index = 0;
        while (index < lengthCount + distanceCount) {
            int symbol = decode(lengthCode);
            if (symbol < 16) {
                lengths[index++] = static_cast<uint16_t>(symbol);
                continue;
            }
            uint16_t length = 0;
            if (symbol == 16) {
                if (index == 0) {
                    throw InflateError();
                }
                length = lengths[index - 1];
                symbol = 3 + static_cast<int>(bits(2));
            }
            else if (symbol == 17) {
                symbol = 3 + static_cast<int>(bits(3));
            }
            else {
                symbol = 11 + static_cast<int>(bits(7));
            }
            if (index + symbol > lengthCount + distanceCount) {
                throw InflateError();
            }
            while (symbol--) {
                lengths[index++] = length;
            }
        }

        if (lengths[256] == 0) {
            throw InflateError();
        }
        int left = construct(lengthCode, lengths, lengthCount);
        if (left < 0 || (left > 0 && lengthCount - lengthCode.count[0] != 1)) {
            throw InflateError();
        }
        left = construct(distanceCode, lengths + lengthCount, distanceCount);
        if (left < 0 || (left > 0 && distanceCount - distanceCode.count[0] != 1)) {
            throw InflateError();
        }
        codes(lengthCode, distanceCode);
    }

    const std::vector<uint8_t>& input;
    size_t position;
    std::vector<uint8_t>& output;
    size_t maxOutputSize;
    uint32_t bitBuffer{};
    int bitCount{};
};

uint32_t adler32(const std::vector<uint8_t>& data) {
    uint32_t a = 1;
    uint32_t b = 0;
    for (uint8_t byte : data) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

} // namespace

bool zlibInflate(const std::vector<uint8_t>& input, std::vector<uint8_t>& output, size_t maxOutputSize) {
    output.clear();
    if (input.size() < 6) {
        return false;
    }
    const uint8_t cmf = input[0];
    const uint8_t flg = input[1];
    // Deflate, window <= 32K, valid check bits, no preset dictionary.
    if ((cmf & 0x0f) != 8 || (cmf >> 4) > 7 || ((cmf << 8) | flg) % 31 != 0 || (flg & 0x20) != 0) {
        return false;
    }

    try {
        Inflater inflater(input, 2, output, maxOutputSize);
        inflater.run();
        const size_t end = inflater.endPosition();
        if (input.size() - end < 4) {
            return false;
        }
        const uint32_t expected = (static_cast<uint32_t>(input[end]) << 24) | (input[end + 1] << 16) | (input[end + 2] << 8) | input[end + 3];
        return expected == adler32(output);
    }
    catch (const InflateError&) {
        output.clear();
        return false;
    }
}

}
