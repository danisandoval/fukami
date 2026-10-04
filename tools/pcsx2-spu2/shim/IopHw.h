// SPDX-License-Identifier: GPL-3.0+
// RRV shim for PCSX2 pcsx2/IopHw.h (2.8.2): only the SPU2 DMA channel
// registers, backed by a private 64 KiB IOP hardware-register array.
#pragma once
#include "common/Pcsx2Types.h"

extern u8 rrv_spu2_iopHw[0x10000];

#define psxHu32(mem) (*(u32*)&rrv_spu2_iopHw[(mem) & 0xffff])

#define HW_DMA4_MADR (psxHu32(0x10c0)) // SPU DMA
#define HW_DMA4_BCR (psxHu32(0x10c4))
#define HW_DMA4_CHCR (psxHu32(0x10c8))
#define HW_DMA4_TADR (psxHu32(0x10cc))

#define HW_DMA7_MADR (psxHu32(0x1500)) // SPU2 DMA
#define HW_DMA7_BCR (psxHu32(0x1504))
#define HW_DMA7_CHCR (psxHu32(0x1508))
#define HW_DMA7_TADR (psxHu32(0x150C))
