// SPDX-License-Identifier: GPL-3.0+
// RRV shim for PCSX2 pcsx2/IopMem.h (2.8.2). iopPhysMem() maps into a private
// 2 MiB buffer that the RRV glue uses as DMA staging memory (payloads are
// copied there at submit time, so the SPU2 core's lazy DMAPtr stays valid).
#pragma once
#include "common/Pcsx2Types.h"

static constexpr u32 RRV_SPU2_IOP_RAM_SIZE = 0x200000;
extern u8 rrv_spu2_iopRam[RRV_SPU2_IOP_RAM_SIZE];

static inline u8* iopPhysMem(u32 addr)
{
	return &rrv_spu2_iopRam[addr & (RRV_SPU2_IOP_RAM_SIZE - 1)];
}
