// SPDX-License-Identifier: GPL-3.0+
// RRV shim for PCSX2 pcsx2/GS/GSVector.h (2.8.2).
//
// Only SPU2/ReverbResample.cpp uses it, and only these GSVector4i members:
// load<aligned>, mul16hrs, adds16, hadds16, I16[]. This is a portable scalar
// implementation with the exact lane semantics of the SSE4 path upstream uses
// on x86 without AVX2 (pmulhrsw / paddsw / phaddsw), so every host (ARM64,
// x86-64) produces the same bits. `_M_SSE` is left undefined, so the AVX
// (GSVector8i) variants are compiled out and ReverbDownsample/ReverbUpsample
// resolve to the `_sse` variants.
//
// Note: upstream x86 builds on AVX2 hosts select the `_avx` variant, whose
// saturating-add order differs; results can differ from ours only when an
// intermediate 16-bit partial sum saturates.
#pragma once
#include "common/Pcsx2Types.h"
#include <cstring>

class GSVector4i
{
public:
	union
	{
		s16 I16[8];
		u16 U16[8];
		s32 I32[4];
	};

	template <bool aligned>
	static GSVector4i load(const void* p)
	{
		GSVector4i r;
		std::memcpy(r.I16, p, 16);
		return r;
	}

	static s16 sat16(s32 v) { return static_cast<s16>(v < -32768 ? -32768 : (v > 32767 ? 32767 : v)); }

	// pmulhrsw: ((a * b) >> 14 + 1) >> 1
	GSVector4i mul16hrs(const GSVector4i& v) const
	{
		GSVector4i r;
		for (int i = 0; i < 8; i++)
		{
			const s32 p = static_cast<s32>(I16[i]) * static_cast<s32>(v.I16[i]);
			r.I16[i] = static_cast<s16>(((p >> 14) + 1) >> 1);
		}
		return r;
	}

	// paddsw
	GSVector4i adds16(const GSVector4i& v) const
	{
		GSVector4i r;
		for (int i = 0; i < 8; i++)
			r.I16[i] = sat16(static_cast<s32>(I16[i]) + static_cast<s32>(v.I16[i]));
		return r;
	}

	// phaddsw(this, v): [a0+a1, a2+a3, a4+a5, a6+a7, b0+b1, b2+b3, b4+b5, b6+b7], saturating
	GSVector4i hadds16(const GSVector4i& v) const
	{
		GSVector4i r;
		for (int i = 0; i < 4; i++)
		{
			r.I16[i] = sat16(static_cast<s32>(I16[2 * i]) + static_cast<s32>(I16[2 * i + 1]));
			r.I16[4 + i] = sat16(static_cast<s32>(v.I16[2 * i]) + static_cast<s32>(v.I16[2 * i + 1]));
		}
		return r;
	}
};
