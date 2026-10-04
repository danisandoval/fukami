// SPDX-License-Identifier: GPL-3.0+
// RRV shim for the parts of PCSX2 pcsx2/SaveState.h (2.8.2) the SPU2 core uses.
#pragma once
#include "common/Pcsx2Types.h"

enum class FreezeAction
{
	Load,
	Save,
	Size,
};

struct freezeData
{
	int size;
	u8* data;
};
