// SPDX-License-Identifier: GPL-3.0+
// RRV shim for PCSX2 common/Pcsx2Types.h (2.8.2): only the scalar aliases the
// SPU2 core uses. Same definitions as upstream.
#pragma once
#include <cstdint>

using s8 = int8_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using uptr = uintptr_t;
using sptr = intptr_t;
using uint = unsigned int;
