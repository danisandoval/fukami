// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
// Adapted from PCSX2 fd9d310ccbb6b8b62c976da8886a3c8fd3a10ff3,
// pcsx2/Counters.cpp and Counters.h (rcntCanCount, gate, write and test paths).
#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace rrv::guest_time {

// Asset-free, detached EE counter model. The caller owns the virtual EE-cycle
// coordinate, ordered blank edges and the shared INTC register integration.
// At a cycle shared by blank edges and MMIO, supply all edges in producer event
// order before any read/write/advance at that cycle; ties cannot be inferred here.
class EeTimers {
public:
    enum class Register { Count, Mode, Compare, Hold };
    enum class Edge { HBlankStart, HBlankEnd, VBlankStart, VBlankEnd };

    EeTimers() = default;
    uint64_t cycle() const { return cycle_; }
    void advance_to(uint64_t cycle);
    void edge_at(uint64_t cycle, Edge edge);
    // The blank level changes but no gate signal reaches the counters, as in
    // pinned PCSX2 d5f75c9e4 Counters.cpp while SMODE1.SINT is set:
    // rcntUpdate_hScanline/VSyncStart/VSyncEnd still toggle the Mode that
    // rcntCanCount reads, but skip rcntStartGate/rcntEndGate. So no HBLNK
    // clock tick and no gate reset or gate-edge sync happen here.
    void level_at(uint64_t cycle, Edge edge);
    uint16_t read_at(uint64_t cycle, unsigned timer, Register reg);
    void write_at(uint64_t cycle, unsigned timer, Register reg, uint32_t value);
    // Earliest cycle at which an enabled, unlatched EE-clock compare/overflow
    // can assert, assuming no intervening mode writes or blank edges. A pending
    // assertion reports the current cycle until taken. HBlank-clock deadlines
    // are supplied by the producer's externally ordered HBlank-start edges.
    std::optional<uint64_t> next_irq_deadline() const;

    // Call only when the producer's one IntcState has determined an eligible
    // INTC exception and its STAT bit 1 (SBUS) is set. Mirrors pinned
    // PCSX2 Hw.cpp:70-72 without owning STAT, MASK or exception eligibility.
    void latch_holds_on_eligible_sbus_intc_exception_at(uint64_t cycle);

    // Newly asserted counter causes on INTC lines 9..12. The producer ORs
    // these into its one authoritative INTC_STAT; this is not a second STAT
    // and consuming causes does not acknowledge guest-visible interrupt state.
    uint32_t pending_irq_assertions() const { return pending_irq_assertions_; }
    uint32_t take_irq_assertions() {
        const uint32_t asserted = pending_irq_assertions_;
        pending_irq_assertions_ = 0;
        return asserted;
    }

private:
    struct Counter {
        uint32_t count = 0;
        uint16_t mode = 0;
        uint16_t target = 0xffff;
        uint16_t hold = 0; // MMIO-mapped for T0/T1 only
        bool target_future = false;
    };
    std::array<Counter, 4> counters_{};
    uint64_t cycle_ = 0;
    uint32_t pending_irq_assertions_ = 0;
    bool hblank_ = false;
    bool vblank_ = false;

    static unsigned divisor(uint16_t mode);
    bool can_count(const Counter& counter) const;
    void advance_ticks(unsigned index, uint64_t ticks);
    void one_event_step(unsigned index, uint64_t& ticks);
    void test_target(unsigned index);
    void test_overflow(unsigned index);
    void gate_edge(bool vblank_source, bool start);
    void apply_edge(uint64_t cycle, Edge edge, bool gate_signal);
    void require_no_skipped_irq(uint64_t cycle) const;
    static Counter& at(std::array<Counter, 4>& counters, unsigned timer);
};

} // namespace rrv::guest_time
