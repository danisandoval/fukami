// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
// Interface of rrv_intc.cpp. Register semantics adapted from PCSX2 fd9d310ccbb6b8b62c976da8886a3c8fd3a10ff3:
// pcsx2/HwWrite.cpp (hwWrite32: INTC_STAT write-one-to-clear, INTC_MASK toggle),
// pcsx2/Hw.cpp (hwIntcIrq, intcInterrupt) and pcsx2/R5900.cpp (cpuIntsEnabled).

#pragma once

#include <cstdint>

#ifndef RRV_GATE4_QUIET_GENERATION_V1
#define RRV_GATE4_QUIET_GENERATION_V1
#include <atomic>
#include <cstdint>
namespace rrv::guest_time {
// Gate-4 lazy checkpoint: bumped by every change to a checkpoint quiet input.
inline std::atomic<uint64_t> quiet_generation{1};
inline void QuietBump() { quiet_generation.fetch_add(1, std::memory_order_relaxed); }
// The same bump without the bus lock, for code that only ever runs in the
// serialized producer domain (the guest-time owner, the INTC, the hardware
// commit): there it is `lock inc` five times per commit, about 0.4 ms per
// VBlank start on a Steam Deck (profile 2026-10-02). Every reader of the
// generation is in that domain too, so a reader never runs between this load
// and this store. Other threads keep bumping with QuietBump(); if one lands in
// between, the two bumps become one, and that is enough: a reader only asks
// whether the value differs from the one it armed with, the value it armed
// with is at most the value loaded here, and what is stored is one more.
inline void QuietBumpProducer() {
    quiet_generation.store(quiet_generation.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}
}
#endif

namespace rrv::guest_time {

// A producer-owned, pure EE INTC register model. The caller must supply both
// cold-boot registers; in particular, the old HLE 0xffffffff enable default is
// not silently adopted. MMIO and HLE must call the same instance.
class IntcState {
 public:
  static constexpr uint32_t kStatAddress = 0x1000f000u;
  static constexpr uint32_t kMaskAddress = 0x1000f010u;
  static constexpr uint32_t kVBlankStartCause = 2u;
  static constexpr uint32_t kVBlankStartBit = 1u << kVBlankStartCause;

  // R5900 COP0.Status bits used by pinned PCSX2 cpuIntsEnabled(0x400).
  static constexpr uint32_t kStatusIe = 1u;
  static constexpr uint32_t kStatusExl = 1u << 1;
  static constexpr uint32_t kStatusErl = 1u << 2;
  static constexpr uint32_t kStatusIntcEnable = 1u << 10;
  static constexpr uint32_t kStatusEie = 1u << 16;

  struct InitialState {
    uint32_t stat;
    uint32_t mask;  // Also the explicit initial HLE enabled-cause mask.
  };

  struct Snapshot {
    uint32_t stat;
    uint32_t mask;
    uint32_t pending;  // Sticky STAT intersected with enabled MASK.
  };

  explicit IntcState(InitialState initial);

  Snapshot Read() const;
  uint32_t ReadStat() const { return stat_; }
  uint32_t ReadMask() const { return mask_; }
  bool VBlankStartPending() const { return (stat_ & kVBlankStartBit) != 0; }

  // MMIO 32-bit register operations. STAT is write-one-to-clear; MASK toggles
  // only its low 16 bits, including on repeated writes. Width extraction and
  // address decoding belong to the memory bus adapter.
  Snapshot WriteStat(uint32_t value);
  Snapshot WriteMask(uint32_t value);

  // HLE operations set/clear bits idempotently in the very same MASK register.
  // A cause outside [0,31] is ignored, as in the compatible-v2 HLE.
  Snapshot EnableHle(uint32_t cause);
  Snapshot DisableHle(uint32_t cause);

  // Hardware assertion is sticky even while masked or CP0 interrupts are
  // disabled. The caller commits this transition before invoking any handler.
  Snapshot RaiseCause(uint32_t cause);
  Snapshot RaiseVBlankStart() { return RaiseCause(kVBlankStartCause); }
  Snapshot AcknowledgeVBlankStart() { return WriteStat(kVBlankStartBit); }

  // CP0 Status is owned by the EE. EI/DI changes EIE there; this query never
  // consumes pending state or runs guest callbacks.
  bool InterruptEligible(uint32_t cp0_status) const;

 private:
  uint32_t stat_;
  uint32_t mask_;
};

}  // namespace rrv::guest_time
