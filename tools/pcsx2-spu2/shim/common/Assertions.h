// SPDX-License-Identifier: GPL-3.0+
// RRV shim for PCSX2 common/Assertions.h (2.8.2). Assertions compile to
// nothing (release semantics); pxAssume keeps its optimiser hint off to stay safe.
#pragma once
#include "common/Pcsx2Defs.h"

#define pxAssert(cond) ((void)0)
#define pxAssertMsg(cond, msg) ((void)0)
#define pxAssertRel(cond, msg) ((void)0)
#define pxAssume(cond) ((void)0)
#define pxAssumeMsg(cond, msg) ((void)0)
#define pxFail(msg) ((void)0)
#define pxFailRel(msg) ((void)0)
