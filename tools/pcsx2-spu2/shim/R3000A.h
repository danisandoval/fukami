// SPDX-License-Identifier: GPL-3.0+
// RRV shim for PCSX2 pcsx2/R3000A.h (2.8.2). There is no IOP CPU: the only
// field the SPU2 core reads is psxRegs.cycle, which the RRV glue sets to the
// guest IOP cycle stamped on each command (never host time).
#pragma once
#include "common/Pcsx2Types.h"

struct psxRegisters
{
	u64 cycle;
};

extern psxRegisters psxRegs;
extern s32 psxNextDeltaCounter;
extern u64 psxNextStartCounter;
