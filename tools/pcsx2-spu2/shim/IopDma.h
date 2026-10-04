// SPDX-License-Identifier: GPL-3.0+
// RRV shim for PCSX2 pcsx2/IopDma.h (2.8.2): the three SPU2 -> IOP interrupt
// sinks. Implemented by the RRV glue, which turns them into timestamped events.
#pragma once

extern void spu2DMA4Irq();
extern void spu2DMA7Irq();
extern void spu2Irq();
