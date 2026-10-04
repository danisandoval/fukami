// SPDX-License-Identifier: GPL-3.0+
// RRV shim for PCSX2 common/Console.h (2.8.2): logging sinks are no-ops.
#pragma once

struct RrvNullConsole
{
	template <typename... A> void WriteLn(A&&...) const {}
	template <typename... A> void Warning(A&&...) const {}
	template <typename... A> void Error(A&&...) const {}
	template <typename... A> void Write(A&&...) const {}
	template <typename... A> void WriteLnFmt(A&&...) const {}
	template <typename... A> void WarningFmt(A&&...) const {}
	template <typename... A> void ErrorFmt(A&&...) const {}
};

inline constexpr RrvNullConsole Console{};
inline constexpr RrvNullConsole DevCon{};
