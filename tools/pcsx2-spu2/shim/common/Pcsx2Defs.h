// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
// RRV shim for PCSX2 common/Pcsx2Defs.h (2.8.2): the handful of macros the
// SPU2 core uses. Release semantics (IsDevBuild = false, no PCSX2_DEVBUILD).
// Provenance: PCSX2 2.8.2 (fd9d310ccbb6b8b62c976da8886a3c8fd3a10ff3), common/Pcsx2Defs.h;
// symbols: IsDevBuild, IsDebugBuild, PCSX2_DEVBUILD. Licence GPL-3.0+.
#pragma once

#ifdef PCSX2_DEVBUILD
#error "rrv_pcsx2_spu2 builds the SPU2 core in release (non-devbuild) configuration only"
#endif

static constexpr bool IsDevBuild = false;
static constexpr bool IsDebugBuild = false;

#if defined(_MSC_VER)
#define __forceinline_odr __forceinline
#else
#ifndef __forceinline
#define __forceinline __attribute__((always_inline, unused))
#endif
#define __forceinline_odr __forceinline inline
#endif
#define __fi __forceinline
#define __ri __fi
#define __noinline __attribute__((noinline))

#if defined(_MSC_VER)
#define ASSUME(x) __assume(x)
#else
#define ASSUME(x) do { if (!(x)) __builtin_unreachable(); } while (0)
#endif

// jNO_DEFAULT: upstream marks the default case unreachable.
#define jNO_DEFAULT \
	default: \
		ASSUME(0); \
		break;

#define ArraySize(x) (sizeof(x) / sizeof((x)[0]))
