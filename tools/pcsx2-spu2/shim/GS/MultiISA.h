// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
// RRV shim for PCSX2 pcsx2/GS/MultiISA.h (2.8.2), non-multi-ISA branch only:
// everything lives in namespace isa_native, exactly as upstream builds it when
// MULTI_ISA_UNSHARED_COMPILATION is not defined (e.g. ARM64 builds).
// Provenance: PCSX2 2.8.2 (fd9d310ccbb6b8b62c976da8886a3c8fd3a10ff3), pcsx2/GS/MultiISA.h;
// symbols: CURRENT_ISA, MULTI_ISA_COMPILE_ONCE, namespace isa_native. Licence GPL-3.0+.
#pragma once
#include "common/Pcsx2Defs.h"
#include "common/Pcsx2Types.h"

#define CURRENT_ISA isa_native
#define MULTI_ISA_COMPILE_ONCE 1
#define MULTI_ISA_UNSHARED_START namespace CURRENT_ISA {
#define MULTI_ISA_UNSHARED_END }
#define MULTI_ISA_UNSHARED_IMPL using namespace CURRENT_ISA
#define MULTI_ISA_DEF(...) namespace isa_native { __VA_ARGS__ }
#define MULTI_ISA_FRIEND(klass) friend class isa_native::klass;
#define MULTI_ISA_SELECT(fn) (isa_native::fn)
