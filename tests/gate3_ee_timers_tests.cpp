#include "../src/guest-time/rrv_ee_timers.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <stdexcept>

#define REQUIRE(expr) do { if (!(expr)) { std::fprintf(stderr, "REQUIRE failed at %s:%d: %s\n", __FILE__, __LINE__, #expr); std::abort(); } } while (false)

using rrv::guest_time::EeTimers;
using R = EeTimers::Register;
using E = EeTimers::Edge;

static void clock_division_and_remainder() {
    EeTimers t;
    t.write_at(0, 0, R::Mode, 0x80); // EE / 2
    t.write_at(0, 1, R::Mode, 0x81); // EE / 32
    t.write_at(0, 2, R::Mode, 0x82); // EE / 512
    REQUIRE(t.read_at(31, 0, R::Count) == 15);
    REQUIRE(t.read_at(31, 1, R::Count) == 0);
    REQUIRE(t.read_at(31, 2, R::Count) == 0);
    REQUIRE(t.read_at(32, 1, R::Count) == 1);
    REQUIRE(t.read_at(511, 2, R::Count) == 0);
    REQUIRE(t.read_at(512, 2, R::Count) == 1);
    t.write_at(513, 1, R::Mode, 0x80); // previous /32 ticks accounted first
    REQUIRE(t.read_at(515, 1, R::Count) == 17);
    REQUIRE(t.read_at(516, 1, R::Count) == 18);
}

static void hblank_and_vblank_gates() {
    EeTimers t;
    t.write_at(0, 0, R::Mode, 0x83); // observed T0: HBlank clock
    t.edge_at(3, E::HBlankStart);
    t.edge_at(4, E::HBlankEnd);
    t.edge_at(9, E::HBlankStart);
    REQUIRE(t.read_at(9, 0, R::Count) == 2);

    t.write_at(9, 1, R::Mode, 0x9d); // observed T1: /32, VBlank reset on start
    REQUIRE(t.read_at(105, 1, R::Count) == 3);
    t.edge_at(105, E::VBlankStart);
    REQUIRE(t.read_at(105, 1, R::Count) == 0);
    REQUIRE(t.read_at(128, 1, R::Count) == 1);
    t.edge_at(129, E::VBlankEnd);
    REQUIRE(t.read_at(160, 1, R::Count) == 2);

    // Gate mode 0 counts while the selected signal is low, including the
    // fraction before an edge; no count leaks through the high interval.
    t.write_at(160, 2, R::Mode, 0x8c); // /2, VBlank gate, mode 0
    REQUIRE(t.read_at(164, 2, R::Count) == 2);
    t.edge_at(165, E::VBlankStart);
    REQUIRE(t.read_at(200, 2, R::Count) == 2);
    t.edge_at(200, E::VBlankEnd);
    REQUIRE(t.read_at(204, 2, R::Count) == 4);

    EeTimers g;
    g.write_at(0, 0, R::Mode, 0xbc); // /2, VBlank gate mode 3: both resets
    REQUIRE(g.read_at(10, 0, R::Count) == 5);
    g.edge_at(11, E::VBlankStart);
    REQUIRE(g.read_at(11, 0, R::Count) == 0);
    REQUIRE(g.read_at(15, 0, R::Count) == 2);
    g.edge_at(15, E::VBlankEnd);
    REQUIRE(g.read_at(15, 0, R::Count) == 0);
    g.write_at(15, 1, R::Mode, 0x87); // HBlank gate + HBlank clock: source disallows count
    g.edge_at(16, E::HBlankStart);
    REQUIRE(g.read_at(16, 1, R::Count) == 0);
}

static void compare_overflow_flags_and_irq() {
    EeTimers t;
    t.write_at(0, 0, R::Compare, 3);
    t.write_at(0, 0, R::Mode, 0x1c0); // /2, zero return, compare IRQ
    REQUIRE(t.next_irq_deadline() == 6);
    REQUIRE(t.read_at(5, 0, R::Count) == 2);
    bool skipped = false;
    try { t.advance_to(7); } catch (const std::logic_error&) { skipped = true; }
    REQUIRE(skipped);
    REQUIRE(t.cycle() == 5);
    REQUIRE(t.read_at(6, 0, R::Count) == 0);
    REQUIRE((t.read_at(6, 0, R::Mode) & 0x400) != 0);
    REQUIRE(t.pending_irq_assertions() == (1u << 9));
    REQUIRE(t.take_irq_assertions() == (1u << 9));
    REQUIRE(t.pending_irq_assertions() == 0);
    t.advance_to(12); // sticky compare flag prevents a second assertion
    REQUIRE(t.take_irq_assertions() == 0);
    t.write_at(12, 0, R::Mode, 0x5c0); // W1C compare flag
    REQUIRE((t.read_at(12, 0, R::Mode) & 0x400) == 0);
    REQUIRE(t.next_irq_deadline() == 18);
    t.advance_to(18);
    REQUIRE(t.take_irq_assertions() == (1u << 9));

    t.write_at(18, 1, R::Count, 0xfffe);
    t.write_at(18, 1, R::Mode, 0x280); // /2, overflow IRQ
    REQUIRE(t.next_irq_deadline() == 22);
    t.advance_to(22);
    REQUIRE(t.read_at(22, 1, R::Count) == 0);
    REQUIRE((t.read_at(22, 1, R::Mode) & 0x800) != 0);
    REQUIRE((t.take_irq_assertions() & (1u << 10)) != 0);
    t.write_at(22, 1, R::Mode, 0xa80); // W1C overflow flag
    REQUIRE((t.read_at(22, 1, R::Mode) & 0x800) == 0);

    EeTimers long_span;
    long_span.write_at(0, 0, R::Compare, 3);
    long_span.write_at(0, 0, R::Mode, 0x1c0);
    long_span.advance_to(6);
    REQUIRE(long_span.take_irq_assertions() == (1u << 9));
    long_span.advance_to(6000000);
    REQUIRE(long_span.read_at(6000000, 0, R::Count) == 0);
    REQUIRE(long_span.take_irq_assertions() == 0);
    long_span.write_at(6000000, 1, R::Mode, 0x380);
    REQUIRE(long_span.next_irq_deadline() == 6000000 + 0x1fffe);
    long_span.advance_to(6000000 + 0x1fffe);
    REQUIRE(long_span.take_irq_assertions() == (1u << 10));
    REQUIRE(long_span.next_irq_deadline() == 6000000 + 0x20000);
    long_span.advance_to(6000000 + 0x20000);
    REQUIRE(long_span.take_irq_assertions() == (1u << 10));
    REQUIRE(long_span.read_at(6000000 + 0x20000, 1, R::Count) == 0);
    REQUIRE((long_span.read_at(6000000 + 0x20000, 1, R::Mode) & 0xc00) == 0xc00);
}

static void simultaneous_target_and_overflow() {
    EeTimers t;
    t.write_at(0, 0, R::Compare, 0); // target deferred until overflow
    t.write_at(0, 0, R::Count, 0xfffe);
    t.write_at(0, 0, R::Mode, 0x380); // both IRQ enables
    REQUIRE(t.next_irq_deadline() == 4);
    t.advance_to(4);
    REQUIRE(t.read_at(4, 0, R::Count) == 0);
    REQUIRE((t.read_at(4, 0, R::Mode) & 0xc00) == 0xc00);
    REQUIRE(t.take_irq_assertions() == (1u << 9));
}

static void hblank_clock_irq_uses_external_edges() {
    EeTimers t;
    t.write_at(0, 0, R::Compare, 2);
    t.write_at(0, 0, R::Mode, 0x183); // T0 HBlank clock, target IRQ
    REQUIRE(!t.next_irq_deadline());
    t.advance_to(1000000); // no guessed HBlank cadence
    t.edge_at(1000000, E::HBlankStart);
    REQUIRE(t.read_at(1000000, 0, R::Count) == 1);
    REQUIRE(!t.next_irq_deadline());
    t.edge_at(1000001, E::HBlankEnd);
    t.edge_at(1000002, E::HBlankStart);
    REQUIRE(t.take_irq_assertions() == (1u << 9));
}

static void writes_and_target_future() {
    EeTimers t;
    t.write_at(0, 0, R::Mode, 0x180);
    t.write_at(0, 0, R::Compare, 5);
    t.write_at(0, 0, R::Count, 7); // compare behind COUNT: wait for overflow
    t.advance_to(16);
    REQUIRE((t.read_at(16, 0, R::Mode) & 0x400) == 0);
    t.write_at(16, 0, R::Count, 2);
    t.advance_to(22);
    REQUIRE((t.read_at(22, 0, R::Mode) & 0x400) != 0);
    REQUIRE(t.take_irq_assertions() == (1u << 9));
    t.write_at(22, 0, R::Hold, 0x12345);
    REQUIRE(t.read_at(22, 0, R::Hold) == 0x2345);
    t.write_at(22, 0, R::Mode, 0x500); // W1C; counting disabled, COUNT retained
    REQUIRE(t.read_at(100, 0, R::Count) == 5);
    REQUIRE((t.read_at(100, 0, R::Mode) & 0x400) == 0);
    t.write_at(100, 0, R::Compare, 9);
    t.write_at(100, 0, R::Mode, 0x180);
    REQUIRE(t.read_at(108, 0, R::Count) == 9);
}

static void same_cycle_order_is_caller_input() {
    EeTimers a, b;
    a.write_at(0, 0, R::Mode, 0x83);
    b.write_at(0, 0, R::Mode, 0x83);
    a.write_at(0, 1, R::Mode, 0x9d);
    b.write_at(0, 1, R::Mode, 0x9d);
    a.edge_at(32, E::HBlankStart);
    a.edge_at(32, E::VBlankStart);
    b.edge_at(32, E::VBlankStart);
    b.edge_at(32, E::HBlankStart);
    REQUIRE(a.read_at(32, 0, R::Count) == 1);
    REQUIRE(b.read_at(32, 0, R::Count) == 1);
    REQUIRE(a.read_at(32, 1, R::Count) == 0);
    REQUIRE(b.read_at(32, 1, R::Count) == 0);
    // Distinct same-cycle edge order is explicit at the API boundary.
    EeTimers c, d;
    c.write_at(0, 0, R::Mode, 0xaf); // VBlank gate, reset at end, HBlank clock
    d.write_at(0, 0, R::Mode, 0xaf);
    c.edge_at(1, E::HBlankStart);
    c.edge_at(1, E::VBlankEnd);
    d.edge_at(1, E::VBlankEnd);
    d.edge_at(1, E::HBlankStart);
    REQUIRE(c.read_at(1, 0, R::Count) == 0);
    REQUIRE(d.read_at(1, 0, R::Count) == 1);
    // At a gate reset, PCSX2's blank edge precedes its ordinary counter
    // threshold pass: the coincident /32 target is discarded by the reset.
    EeTimers gate_tie;
    gate_tie.write_at(0, 0, R::Compare, 1);
    gate_tie.write_at(0, 0, R::Mode, 0x19d);
    REQUIRE(gate_tie.next_irq_deadline() == 32);
    gate_tie.edge_at(32, E::VBlankStart);
    REQUIRE(gate_tie.read_at(32, 0, R::Count) == 0);
    REQUIRE(gate_tie.pending_irq_assertions() == 0);
    REQUIRE(gate_tie.next_irq_deadline() == 64);
    gate_tie.advance_to(64);
    REQUIRE(gate_tie.take_irq_assertions() == (1u << 9));
    bool rejected = false;
    try { c.advance_to(0); } catch (const std::invalid_argument&) { rejected = true; }
    REQUIRE(rejected);
}

static void eligible_intc_exception_latches_holds() {
    EeTimers t;
    t.write_at(0, 0, R::Mode, 0x80);
    t.write_at(0, 1, R::Mode, 0x81);
    t.edge_at(64, E::VBlankStart); // VBlank alone is not Hw.cpp's HOLD trigger
    REQUIRE(t.read_at(64, 0, R::Hold) == 0);
    REQUIRE(t.read_at(64, 1, R::Hold) == 0);
    t.latch_holds_on_eligible_sbus_intc_exception_at(64);
    REQUIRE(t.read_at(64, 0, R::Hold) == 32);
    REQUIRE(t.read_at(64, 1, R::Hold) == 2);
    REQUIRE(t.pending_irq_assertions() == 0);
    t.latch_holds_on_eligible_sbus_intc_exception_at(96);
    REQUIRE(t.read_at(96, 0, R::Hold) == 48);
    REQUIRE(t.read_at(96, 1, R::Hold) == 3);
    bool unmapped = false;
    try { (void)t.read_at(96, 2, R::Hold); } catch (const std::out_of_range&) { unmapped = true; }
    REQUIRE(unmapped);
}

static void same_cycle_gate_overflow_can_be_serviced() {
    EeTimers t;
    t.write_at(0, 0, R::Count, 0xffff);
    t.write_at(0, 0, R::Mode, 0x28c); // /2, VBlank gate mode 0, overflow IRQ
    REQUIRE(t.next_irq_deadline() == 2);
    t.edge_at(2, E::VBlankStart); // gate closes before ordinary threshold pass
    REQUIRE(t.read_at(2, 0, R::Count) == 0); // 0x10000 internally
    REQUIRE(t.pending_irq_assertions() == 0);
    t.write_at(2, 0, R::Mode, 0x280); // disable gate at this same cycle
    REQUIRE(t.next_irq_deadline() == 2);
    t.advance_to(2); // scheduled current-cycle service, no extra /2 tick
    REQUIRE(t.read_at(2, 0, R::Count) == 0);
    REQUIRE((t.read_at(2, 0, R::Mode) & 0x800) != 0);
    REQUIRE(t.take_irq_assertions() == (1u << 9));
    t.advance_to(3); // deadline is no longer stranded
}

// SMODE1.SINT set: the blank level changes, but pinned PCSX2 skips
// rcntStartGate/rcntEndGate, so no HBLNK clock tick and no gate reset.
static void level_without_gate_signal() {
    EeTimers t;
    t.write_at(0, 0, R::Mode, 0x83); // HBlank clock
    t.write_at(0, 1, R::Mode, 0x94); // /2, HBlank gate mode 1: reset on start
    t.write_at(0, 2, R::Mode, 0x84); // /2, HBlank gate mode 0: count while low
    t.edge_at(4, E::HBlankStart);
    REQUIRE(t.read_at(4, 0, R::Count) == 1);
    REQUIRE(t.read_at(4, 1, R::Count) == 0);
    REQUIRE(t.read_at(4, 2, R::Count) == 2);
    t.level_at(10, E::HBlankEnd);
    REQUIRE(t.read_at(10, 2, R::Count) == 2); // no count in blank
    t.level_at(20, E::HBlankStart);
    REQUIRE(t.read_at(20, 0, R::Count) == 1); // no HBLNK tick
    REQUIRE(t.read_at(20, 1, R::Count) == 8); // no reset
    REQUIRE(t.read_at(20, 2, R::Count) == 7); // level still toggled
    REQUIRE(t.read_at(30, 2, R::Count) == 7);
    t.level_at(30, E::HBlankEnd);
    t.edge_at(40, E::HBlankStart);
    REQUIRE(t.read_at(40, 0, R::Count) == 2);
    REQUIRE(t.read_at(40, 1, R::Count) == 0);
    REQUIRE(t.read_at(40, 2, R::Count) == 12);
    REQUIRE(t.take_irq_assertions() == 0);

    EeTimers v;
    v.write_at(0, 0, R::Mode, 0xbc); // /2, VBlank gate mode 3: both resets
    v.level_at(10, E::VBlankStart);
    v.level_at(20, E::VBlankEnd);
    REQUIRE(v.read_at(20, 0, R::Count) == 10);
}

// Gate-4 quiet commits advance timers in larger steps between deadlines.
// Before the next IRQ deadline, many small advances equal one large advance.
static void advance_is_split_invariant_before_irq_deadline() {
    uint64_t seed = 0x9e3779b97f4a7c15ull;
    const auto next = [&seed](uint64_t n) { seed = seed * 6364136223846793005ull + 1442695040888963407ull; return (seed >> 33) % n; };
    static const uint16_t modes[] = {0x80, 0x81, 0x82, 0xc0, 0xc1, 0x180, 0x280, 0x380, 0x3c0, 0x1c0, 0x2c1, 0x3c2, 0x84, 0x8c};
    for (int trial = 0; trial < 4000; ++trial) {
        EeTimers a, b;
        for (unsigned i = 0; i < 3; ++i) {
            const uint16_t mode = modes[next(sizeof(modes) / sizeof(modes[0]))];
            const uint16_t target = static_cast<uint16_t>(next(4) ? next(0x10000) : next(64));
            const uint16_t count = static_cast<uint16_t>(next(0x10000));
            for (EeTimers* t : {&a, &b}) {
                t->write_at(0, i, R::Compare, target);
                t->write_at(0, i, R::Count, count);
                t->write_at(0, i, R::Mode, mode);
            }
        }
        (void)a.take_irq_assertions(); (void)b.take_irq_assertions();
        const auto deadline = a.next_irq_deadline();
        REQUIRE(deadline == b.next_irq_deadline());
        const uint64_t limit = 1 + next(5000000);
        const uint64_t end = deadline ? std::min<uint64_t>(*deadline ? *deadline - 1 : 0, limit) : limit;
        for (uint64_t at = 0; at < end;) { at = std::min<uint64_t>(end, at + 1 + next(3000)); a.advance_to(at); }
        b.advance_to(end);
        for (unsigned i = 0; i < 4; ++i)
            for (R reg : {R::Count, R::Mode, R::Compare})
                REQUIRE(a.read_at(end, i, reg) == b.read_at(end, i, reg));
        REQUIRE(a.next_irq_deadline() == b.next_irq_deadline());
        REQUIRE(a.pending_irq_assertions() == b.pending_irq_assertions());
    }
}

int main() {
    clock_division_and_remainder();
    hblank_and_vblank_gates();
    compare_overflow_flags_and_irq();
    simultaneous_target_and_overflow();
    hblank_clock_irq_uses_external_edges();
    writes_and_target_future();
    same_cycle_order_is_caller_input();
    eligible_intc_exception_latches_holds();
    same_cycle_gate_overflow_can_be_serviced();
    level_without_gate_signal();
    advance_is_split_invariant_before_irq_deadline();
}
