#pragma once

#include <cstddef>
#include <cstdint>

using uint8 = std::uint8_t;
using sint8 = std::int8_t;
using int8 = std::int8_t;
using uint16 = std::uint16_t;
using sint16 = std::int16_t;
using int16 = std::int16_t;
using uint32 = std::uint32_t;
using sint32 = std::int32_t;
using int32 = std::int32_t;
using uint64 = std::uint64_t;
using sint64 = std::int64_t;
using int64 = std::int64_t;

using uint8_ptr = uint8*;
using sint8_ptr = sint8*;
using int8_ptr = int8*;
using uint16_ptr = uint16*;
using sint16_ptr = sint16*;
using int16_ptr = int16*;
using uint32_ptr = uint32*;
using sint32_ptr = sint32*;
using int32_ptr = int32*;
using uint64_ptr = uint64*;
using sint64_ptr = sint64*;
using int64_ptr = int64*;

using uint8_const_ptr = uint8 const*;
using sint8_const_ptr = sint8 const*;
using int8_const_ptr = int8 const*;
using uint16_const_ptr = uint16 const*;
using sint16_const_ptr = sint16 const*;
using int16_const_ptr = int16 const*;
using uint32_const_ptr = uint32 const*;
using sint32_const_ptr = sint32 const*;
using int32_const_ptr = int32 const*;
using uint64_const_ptr = uint64 const*;
using sint64_const_ptr = sint64 const*;
using int64_const_ptr = int64 const*;

constexpr uint8 max_uint8 = 0xFFU;
constexpr sint8 min_int8 = sint8(0x80);
constexpr sint8 max_int8 = sint8(0x7F);
constexpr uint16 max_uint16 = 0xFFFFU;
constexpr sint16 min_int16 = sint16(0x8000);
constexpr sint16 max_int16 = sint16(0x7FFF);
constexpr uint32 max_uint32 = 0xFFFFFFFFU;
constexpr sint32 min_int32 = sint32(0x80000000);
constexpr sint32 max_int32 = sint32(0x7FFFFFFF);
constexpr uint64 max_uint64 = 0xFFFFFFFFFFFFFFFFULL;
constexpr sint64 min_int64 = sint64(0x8000000000000000LL);
constexpr sint64 max_int64 = sint64(0x7FFFFFFFFFFFFFFFLL);

template <int N>
struct UIntTraits {};
template <>
struct UIntTraits<8> { using Type = uint8; };
template <>
struct UIntTraits<16> { using Type = uint16; };
template <>
struct UIntTraits<32> { using Type = uint32; };
template <>
struct UIntTraits<64> { using Type = uint64; };
template <int N>
using UInt = typename UIntTraits<N>::Type;
