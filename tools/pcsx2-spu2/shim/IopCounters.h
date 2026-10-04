// SPDX-License-Identifier: GPL-3.0+
// RRV shim for PCSX2 pcsx2/IopCounters.h (2.8.2). spu2sys.cpp CounterUpdate()
// schedules counter 6 as "call SPU2async() at the DMA deadline". The RRV glue
// computes that deadline itself from Cores[].LastClock/DMAICounter, so these
// are write-only sinks.
#pragma once
#include "common/Pcsx2Types.h"

struct psxCounter
{
	u64 count, target;
	u32 rate, interrupt;
	u64 startCycle;
	s32 deltaCycles;
};

#define NUM_COUNTERS 8
extern psxCounter psxCounters[NUM_COUNTERS];
