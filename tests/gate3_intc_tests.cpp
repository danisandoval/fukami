#include "../src/guest-time/rrv_intc.h"

#include <cstdio>
#include <cstdlib>

using rrv::guest_time::IntcState;

static void Check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}

int main() {
  constexpr uint32_t cp0_enabled = IntcState::kStatusIe |
                                   IntcState::kStatusEie |
                                   IntcState::kStatusIntcEnable;
  constexpr uint32_t vblank = IntcState::kVBlankStartBit;

  // The integration supplies explicit cold-boot STAT and HLE/MMIO MASK values.
  IntcState intc({0u, 0u});
  Check(intc.Read().stat == 0u && intc.Read().mask == 0u,
        "explicit initial state");
  Check(!intc.InterruptEligible(cp0_enabled), "no pending interrupt at boot");

  // Original SyncV entry ACK, polling, hardware assertion, and exit ACK.
  intc.AcknowledgeVBlankStart();
  Check(!intc.VBlankStartPending(), "entry ACK of clear bit is inert");
  intc.RaiseVBlankStart();
  Check(intc.VBlankStartPending(), "hardware bit 2 visible to polling");
  Check(intc.Read().pending == 0u, "masked bit remains latched");
  intc.RaiseVBlankStart();
  Check(intc.ReadStat() == vblank, "repeated hardware assertion is sticky");

  intc.WriteMask(vblank);
  Check(intc.ReadMask() == vblank && intc.Read().pending == vblank,
        "MMIO toggle exposes existing pending bit");
  Check(intc.InterruptEligible(cp0_enabled), "enabled pending bit eligible");
  Check(!intc.InterruptEligible(cp0_enabled & ~IntcState::kStatusEie),
        "DI suppresses eligibility without losing pending bit");
  Check(intc.InterruptEligible(cp0_enabled), "EI restores eligibility");
  Check(!intc.InterruptEligible(cp0_enabled & ~IntcState::kStatusIe),
        "IE gate suppresses eligibility");
  Check(!intc.InterruptEligible(cp0_enabled & ~IntcState::kStatusIntcEnable),
        "INTC CP0 mask suppresses eligibility");
  Check(!intc.InterruptEligible(cp0_enabled | IntcState::kStatusExl),
        "EXL suppresses eligibility");
  Check(!intc.InterruptEligible(cp0_enabled | IntcState::kStatusErl),
        "ERL suppresses eligibility");
  Check(intc.ReadStat() == vblank, "eligibility query never ACKs");

  intc.WriteStat(0u);
  Check(intc.VBlankStartPending(), "STAT zero write does not clear");
  intc.WriteStat(1u);
  Check(intc.VBlankStartPending(), "unrelated W1C bit does not clear VBlank");
  intc.AcknowledgeVBlankStart();
  Check(!intc.VBlankStartPending() && !intc.InterruptEligible(cp0_enabled),
        "SyncV exit ACK clears status and eligibility");
  intc.AcknowledgeVBlankStart();
  Check(intc.ReadStat() == 0u, "repeated ACK remains clear");

  intc.WriteMask(vblank);
  Check(intc.ReadMask() == 0u, "second MASK write toggles off");
  intc.WriteMask(vblank);
  Check(intc.ReadMask() == vblank, "third MASK write toggles on");
  intc.DisableHle(IntcState::kVBlankStartCause);
  Check(intc.ReadMask() == 0u, "HLE disable visible through MMIO read");
  intc.DisableHle(IntcState::kVBlankStartCause);
  Check(intc.ReadMask() == 0u, "HLE disable is idempotent");
  intc.RaiseVBlankStart();
  intc.EnableHle(IntcState::kVBlankStartCause);
  Check(intc.ReadMask() == vblank && intc.InterruptEligible(cp0_enabled),
        "HLE enable makes MMIO pending request eligible");
  intc.EnableHle(IntcState::kVBlankStartCause);
  Check(intc.ReadMask() == vblank, "HLE enable is idempotent");
  intc.WriteMask(vblank);
  Check(intc.ReadMask() == 0u && intc.ReadStat() == vblank,
        "MMIO disable is visible to HLE while pending remains stable");

  IntcState upper({0x80000001u, 0x80000001u});
  upper.WriteMask(0xffff0000u);
  Check(upper.ReadMask() == 0x80000001u,
        "MASK MMIO write ignores upper 16 bits");
  upper.WriteStat(0x80000000u);
  Check(upper.ReadStat() == 1u, "STAT W1C applies to full 32-bit value");
  upper.DisableHle(0u);
  Check(!upper.InterruptEligible(cp0_enabled),
        "HLE disable and MMIO W1C share pending calculation");
  upper.EnableHle(32u);
  upper.RaiseCause(32u);
  Check(upper.Read().stat == 1u && upper.Read().mask == 0x80000000u,
        "out-of-range HLE and hardware causes are inert");

  std::puts("PASS gate3_intc_tests");
  return 0;
}
