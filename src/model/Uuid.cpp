#include "model/Uuid.h"

#include <cstdint>
#include <cstdio>
#include <random>

namespace silex {

Uuid newUuid()
{
    thread_local std::mt19937_64 rng{std::random_device{}() ^
                                     (static_cast<uint64_t>(std::random_device{}()) << 32)};
    uint64_t a = rng(), b = rng();
    a = (a & 0xFFFFFFFFFFFF0FFFull) | 0x0000000000004000ull;  // version 4
    b = (b & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull;  // variant 10
    char buf[37];
    std::snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%04x-%012llx",
                  static_cast<unsigned>(a >> 32), static_cast<unsigned>((a >> 16) & 0xFFFF),
                  static_cast<unsigned>(a & 0xFFFF), static_cast<unsigned>(b >> 48),
                  static_cast<unsigned long long>(b & 0xFFFFFFFFFFFFull));
    return buf;
}

}  // namespace silex
