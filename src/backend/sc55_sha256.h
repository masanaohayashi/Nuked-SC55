#pragma once
// SHA-256 digests, for recognizing ROMs and checking generated sound data.
// Uses the RFC 6234 reference implementation in sha/.

#include <array>
#include <cstdint>
#include <span>

extern "C"
{
#include "sha/sha.h"
}

namespace sc55
{
using Sha256 = std::array<uint8_t, 32>;

inline Sha256 sha256(std::span<const uint8_t> bytes)
{
    SHA256Context context;
    SHA256Reset(&context);
    SHA256Input(&context, bytes.data(), unsigned(bytes.size()));
    Sha256 out{};
    SHA256Result(&context, out.data());
    return out;
}

// A digest written as 64 lowercase hex digits.
constexpr Sha256 sha256Hex(const char (&hex)[65])
{
    Sha256 out{};
    for (std::size_t i = 0; i < out.size(); ++i) {
        const char high = hex[2 * i], low = hex[2 * i + 1];
        out[i] = uint8_t(((high <= '9' ? high - '0' : high - 'a' + 10) << 4) | (low <= '9' ? low - '0' : low - 'a' + 10));
    }
    return out;
}
}
