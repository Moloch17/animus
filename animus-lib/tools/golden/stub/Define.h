// The two AzerothCore headers MlpPolicy.cpp includes, reduced to what it uses, so the golden check builds the reader
// alone (golden_check.cpp). Not part of the module's build.
#ifndef ANIMUS_GOLDEN_STUB_DEFINE_H
#define ANIMUS_GOLDEN_STUB_DEFINE_H

#include <cstdint>

using int8 = std::int8_t;
using int16 = std::int16_t;
using int32 = std::int32_t;
using int64 = std::int64_t;
using uint8 = std::uint8_t;
using uint16 = std::uint16_t;
using uint32 = std::uint32_t;
using uint64 = std::uint64_t;

#endif
