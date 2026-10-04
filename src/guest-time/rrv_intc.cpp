// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
// Register transition and CP0 eligibility semantics adapted from pinned PCSX2
// fd9d310ccbb6b8b62c976da8886a3c8fd3a10ff3: pcsx2/HwWrite.cpp,
// pcsx2/Hw.cpp, and pcsx2/R5900.cpp. HLE set/clear operations follow the
// effective compatible-v2 PS2Recomp Kernel/Syscalls/Interrupt.cpp.

#include "rrv_intc.h"

namespace rrv::guest_time {

IntcState::IntcState(InitialState initial)
    : stat_(initial.stat), mask_(initial.mask) {}

IntcState::Snapshot IntcState::Read() const {
  return {stat_, mask_, stat_ & mask_};
}

IntcState::Snapshot IntcState::WriteStat(uint32_t value) {
  stat_ &= ~value;
  return Read();
}

IntcState::Snapshot IntcState::WriteMask(uint32_t value) {
  mask_ ^= static_cast<uint16_t>(value);
  return Read();
}

IntcState::Snapshot IntcState::EnableHle(uint32_t cause) {
  if (cause < 32u)
    mask_ |= 1u << cause;
  return Read();
}

IntcState::Snapshot IntcState::DisableHle(uint32_t cause) {
  if (cause < 32u)
    mask_ &= ~(1u << cause);
  return Read();
}

IntcState::Snapshot IntcState::RaiseCause(uint32_t cause) {
  if (cause < 32u)
    stat_ |= 1u << cause;
  return Read();
}

bool IntcState::InterruptEligible(uint32_t cp0_status) const {
  constexpr uint32_t enabled = kStatusIe | kStatusEie | kStatusIntcEnable;
  constexpr uint32_t blocked = kStatusExl | kStatusErl;
  return (cp0_status & enabled) == enabled &&
         (cp0_status & blocked) == 0 && (stat_ & mask_) != 0;
}

}  // namespace rrv::guest_time
