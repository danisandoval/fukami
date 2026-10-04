// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
// Adapted from PCSX2 fd9d310ccbb6b8b62c976da8886a3c8fd3a10ff3,
// pcsx2/Counters.cpp and Counters.h. See header for interface boundary.
#include "rrv_ee_timers.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace rrv::guest_time {
namespace {
constexpr uint16_t kGate = 0x004;
constexpr uint16_t kVBlankGate = 0x008;
constexpr uint16_t kZeroReturn = 0x040;
constexpr uint16_t kCounting = 0x080;
constexpr uint16_t kTargetIrq = 0x100;
constexpr uint16_t kOverflowIrq = 0x200;
constexpr uint16_t kTargetReached = 0x400;
constexpr uint16_t kOverflowReached = 0x800;
}

EeTimers::Counter& EeTimers::at(std::array<Counter, 4>& counters, unsigned timer) {
    if (timer >= counters.size()) throw std::out_of_range("EE timer index");
    return counters[timer];
}

unsigned EeTimers::divisor(uint16_t mode) {
    switch (mode & 3) {
    case 0: return 2;
    case 1: return 32;
    default: return 512;
    }
}

bool EeTimers::can_count(const Counter& c) const {
    if (!(c.mode & kCounting)) return false;
    if (!(c.mode & kGate)) return true;
    const unsigned gate_mode = (c.mode >> 4) & 3;
    if (!(c.mode & kVBlankGate))
        return (c.mode & 3) != 3 && (!hblank_ || gate_mode != 0);
    return !vblank_ || gate_mode != 0;
}

std::optional<uint64_t> EeTimers::next_irq_deadline() const {
    if (pending_irq_assertions_) return cycle_;
    std::optional<uint64_t> earliest;
    for (const Counter& c : counters_) {
        if (!can_count(c) || (c.mode & 3) == 3) continue;
        const bool target_irq = (c.mode & kTargetIrq) && !(c.mode & kTargetReached);
        const bool overflow_irq = (c.mode & kOverflowIrq) && !(c.mode & kOverflowReached);
        if (!target_irq && !overflow_irq) continue;

        const uint64_t to_overflow = 0x10000u - c.count;
        uint64_t ticks = std::numeric_limits<uint64_t>::max();
        if (target_irq) {
            if (c.target_future)
                ticks = to_overflow + c.target;
            else
                ticks = c.count < c.target ? c.target - c.count : 1;
        }
        // With zero-return at a live nonzero target, overflow cannot occur
        // before a target reset. If the target is postponed until overflow,
        // the first overflow remains reachable.
        const bool target_resets_before_overflow = (c.mode & kZeroReturn) &&
            c.target != 0 && !c.target_future && c.count < c.target;
        if (overflow_irq && !target_resets_before_overflow)
            ticks = std::min(ticks, to_overflow);
        if (ticks == std::numeric_limits<uint64_t>::max()) continue;

        const uint64_t rate = divisor(c.mode);
        const uint64_t tick_index = cycle_ / rate;
        if (ticks > std::numeric_limits<uint64_t>::max() / rate - tick_index)
            continue;
        const uint64_t deadline = (tick_index + ticks) * rate;
        if (!earliest || deadline < *earliest) earliest = deadline;
    }
    return earliest;
}

void EeTimers::require_no_skipped_irq(uint64_t cycle) const {
    const auto deadline = next_irq_deadline();
    if (deadline && cycle > *deadline)
        throw std::logic_error("EE timer IRQ deadline must be serviced before advancing");
}

void EeTimers::test_overflow(unsigned i) {
    Counter& c = counters_[i];
    if (c.count <= 0xffff) return;
    if ((c.mode & kOverflowIrq) && !(c.mode & kOverflowReached)) {
        c.mode |= kOverflowReached;
        pending_irq_assertions_ |= 1u << (9 + i);
    }
    c.count -= 0x10000;
    c.target_future = false;
}

void EeTimers::test_target(unsigned i) {
    Counter& c = counters_[i];
    if (c.target_future || c.count < c.target) return;
    if ((c.mode & kTargetIrq) && !(c.mode & kTargetReached)) {
        c.mode |= kTargetReached;
        pending_irq_assertions_ |= 1u << (9 + i);
    }
    if (c.mode & kZeroReturn) c.count -= c.target;
    else c.target_future = true;
}

void EeTimers::one_event_step(unsigned i, uint64_t& ticks) {
    Counter& c = counters_[i];
    uint64_t step = 0x10000u - c.count;
    if (!c.target_future) {
        const uint64_t to_target = c.count < c.target ? c.target - c.count : 1;
        step = std::min(step, to_target);
    }
    step = std::min(step, ticks);
    c.count += static_cast<uint32_t>(step);
    ticks -= step;
    // This is the order in rcntUpdate() and rcntStartGate(): overflow first.
    test_overflow(i);
    test_target(i);
}

void EeTimers::advance_ticks(unsigned i, uint64_t ticks) {
    Counter& c = counters_[i];
    while (ticks) {
        // Once at a clean period boundary, skip complete periods. Sticky flags
        // preserve PCSX2's one assertion until MODE W1C; INTC remains external.
        if (c.count == 0 && !c.target_future && c.target != 0) {
            const uint64_t period = (c.mode & kZeroReturn) ? c.target : 0x10000u;
            if (ticks >= period) {
                const uint64_t periods = ticks / period;
                ticks -= periods * period;
                // A full non-ZRET period passes both target and overflow.
                if ((c.mode & kTargetIrq) && !(c.mode & kTargetReached)) {
                    c.mode |= kTargetReached;
                    pending_irq_assertions_ |= 1u << (9 + i);
                }
                if (!(c.mode & kZeroReturn) && (c.mode & kOverflowIrq) &&
                    !(c.mode & kOverflowReached)) {
                    c.mode |= kOverflowReached;
                    pending_irq_assertions_ |= 1u << (9 + i);
                }
                continue;
            }
        }
        one_event_step(i, ticks);
    }
}

void EeTimers::advance_to(uint64_t cycle) {
    if (cycle < cycle_) throw std::invalid_argument("EE cycle moved backwards");
    require_no_skipped_irq(cycle);
    for (unsigned i = 0; i < counters_.size(); ++i) {
        const Counter& c = counters_[i];
        if (!can_count(c) || (c.mode & 3) == 3) continue;
        const unsigned rate = divisor(c.mode);
        const uint64_t ticks = cycle / rate - cycle_ / rate;
        if (ticks) advance_ticks(i, ticks);
    }
    cycle_ = cycle;
    // A gate can leave a coincident threshold deferred at this cycle. If a
    // same-cycle MODE write re-enables counting, the next scheduled counter
    // pass must service that threshold without requiring another clock tick.
    for (unsigned i = 0; i < counters_.size(); ++i) {
        const Counter& c = counters_[i];
        if ((c.mode & 3) == 3 || !can_count(c)) continue;
        test_overflow(i);
        test_target(i);
    }
}

void EeTimers::gate_edge(bool vblank_source, bool start) {
    for (Counter& c : counters_) {
        if (!(c.mode & kGate) || !!(c.mode & kVBlankGate) != vblank_source)
            continue;
        const unsigned gate_mode = (c.mode >> 4) & 3;
        if ((start && (gate_mode == 1 || gate_mode == 3)) ||
            (!start && (gate_mode == 2 || gate_mode == 3))) {
            c.count = 0;
            c.target_future = false;
        }
    }
}

void EeTimers::edge_at(uint64_t cycle, Edge edge) { apply_edge(cycle, edge, true); }

void EeTimers::level_at(uint64_t cycle, Edge edge) { apply_edge(cycle, edge, false); }

void EeTimers::apply_edge(uint64_t cycle, Edge edge, bool gate_signal) {
    if (cycle < cycle_) throw std::invalid_argument("EE cycle moved backwards");
    require_no_skipped_irq(cycle);
    const bool start = edge == Edge::HBlankStart || edge == Edge::VBlankStart;
    const bool vblank_source = edge == Edge::VBlankStart || edge == Edge::VBlankEnd;
    // PCSX2 handles VSync/HScanline gates before the ordinary rcntUpdate()
    // target/overflow pass. rcntSyncCounter() at a gate accumulates the exact
    // endpoint tick, but a reset (or mode-0 gate close) may discard/defer its
    // threshold before that pass. Earlier ticks retain their ordered tests.
    for (unsigned i = 0; i < counters_.size(); ++i) {
        Counter& c = counters_[i];
        if (!can_count(c) || (c.mode & 3) == 3) continue;
        const unsigned rate = divisor(c.mode);
        uint64_t ticks = cycle / rate - cycle_ / rate;
        const unsigned gate_mode = (c.mode >> 4) & 3;
        const bool matching_gate = gate_signal && (c.mode & kGate) &&
            (!!(c.mode & kVBlankGate) == vblank_source);
        const bool gate_preempts_test = matching_gate &&
            ((start && (gate_mode == 0 || gate_mode == 1 || gate_mode == 3)) ||
             (!start && (gate_mode == 2 || gate_mode == 3)));
        if (gate_preempts_test && cycle % rate == 0 && ticks) {
            advance_ticks(i, ticks - 1);
            ++c.count; // rcntSyncCounter, without the later threshold pass
        } else if (ticks) {
            advance_ticks(i, ticks);
        }
    }
    cycle_ = cycle;
    // rcntStartGate clocks HBlank-sourced counters before applying gate resets.
    // Without the gate signal neither happens; the blank level still changes.
    if (gate_signal && edge == Edge::HBlankStart) {
        for (unsigned i = 0; i < counters_.size(); ++i)
            if ((counters_[i].mode & 3) == 3 && can_count(counters_[i]))
                advance_ticks(i, 1);
    }
    if (gate_signal) gate_edge(vblank_source, start);
    if (vblank_source) vblank_ = start;
    else hblank_ = start;
    // The ordinary counter pass follows the blank edge in rcntUpdate().
    for (unsigned i = 0; i < counters_.size(); ++i) {
        Counter& c = counters_[i];
        if ((c.mode & 3) == 3 || !can_count(c)) continue;
        test_overflow(i);
        test_target(i);
    }
}

void EeTimers::latch_holds_on_eligible_sbus_intc_exception_at(uint64_t cycle) {
    advance_to(cycle);
    counters_[0].hold = static_cast<uint16_t>(counters_[0].count);
    counters_[1].hold = static_cast<uint16_t>(counters_[1].count);
}

uint16_t EeTimers::read_at(uint64_t cycle, unsigned timer, Register reg) {
    advance_to(cycle);
    const Counter& c = at(counters_, timer);
    if (reg == Register::Hold && timer >= 2)
        throw std::out_of_range("EE HOLD is mapped only for T0/T1");
    switch (reg) {
    case Register::Count: return static_cast<uint16_t>(c.count);
    case Register::Mode: return c.mode;
    case Register::Compare: return c.target;
    case Register::Hold: return c.hold;
    }
    throw std::invalid_argument("EE timer register");
}

void EeTimers::write_at(uint64_t cycle, unsigned timer, Register reg, uint32_t value) {
    advance_to(cycle);
    Counter& c = at(counters_, timer);
    if (reg == Register::Hold && timer >= 2)
        throw std::out_of_range("EE HOLD is mapped only for T0/T1");
    switch (reg) {
    case Register::Count:
        c.count = value & 0xffff;
        c.target_future = c.count >= c.target;
        break;
    case Register::Mode:
        c.mode = static_cast<uint16_t>((c.mode & kTargetReached & ~(value & kTargetReached)) |
            (c.mode & kOverflowReached & ~(value & kOverflowReached)) | (value & 0x3ff));
        break;
    case Register::Compare:
        c.target = value & 0xffff;
        c.target_future = c.target <= c.count;
        break;
    case Register::Hold:
        c.hold = value & 0xffff;
        break;
    }
}

} // namespace rrv::guest_time
