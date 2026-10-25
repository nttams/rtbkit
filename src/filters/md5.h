// Small MD5 (RFC 1321). Only used by the UserPartition filter.
#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace RTBKIT {

inline std::array<uint8_t, 16> md5(const std::string& msg)
{
    static const int shifts[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

    static const std::array<uint32_t, 64> k = [] {
        std::array<uint32_t, 64> t{};
        for (int i = 0; i < 64; ++i)
            t[i] = uint32_t(std::floor(std::fabs(std::sin(double(i + 1))) * 4294967296.0));
        return t;
    }();

    uint32_t a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;

    std::vector<uint8_t> data(msg.begin(), msg.end());
    data.push_back(0x80);
    while (data.size() % 64 != 56) data.push_back(0);
    uint64_t bits = uint64_t(msg.size()) * 8;
    for (int i = 0; i < 8; ++i) data.push_back(uint8_t(bits >> (8 * i)));

    for (size_t off = 0; off < data.size(); off += 64) {
        uint32_t m[16];
        for (int i = 0; i < 16; ++i) {
            const uint8_t* p = &data[off + i * 4];
            m[i] = uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
        }

        uint32_t a = a0, b = b0, c = c0, d = d0;
        for (int i = 0; i < 64; ++i) {
            uint32_t f;
            int g;
            if (i < 16)      { f = (b & c) | (~b & d); g = i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
            else if (i < 48) { f = b ^ c ^ d;          g = (3 * i + 5) % 16; }
            else             { f = c ^ (b | ~d);       g = (7 * i) % 16; }

            f += a + k[i] + m[g];
            a = d; d = c; c = b;
            b += std::rotl(f, shifts[i]);
        }
        a0 += a; b0 += b; c0 += c; d0 += d;
    }

    std::array<uint8_t, 16> out;
    const uint32_t words[4] = {a0, b0, c0, d0};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) out[i * 4 + j] = uint8_t(words[i] >> (8 * j));
    return out;
}

} // namespace RTBKIT
