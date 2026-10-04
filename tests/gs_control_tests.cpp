// Source-backed semantic expectations: PCSX2 fd9d310ccbb6b8b62c976da8886a3c8fd3a10ff3,
// pcsx2/Gif_Unit.cpp:49-81,175-187; pcsx2/Gif_Unit.h:847-848;
// pcsx2/GS.cpp:31-108,110-293. Numeric assertions are independent expected
// transitions, not a second wrapper around Control. Real GIF/MMIO/INTC routing
// is tested separately by the product integration target.
#include "../src/gs-control/rrv_gs_control.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using rrv::gs::Control;

static void check(bool good, const char* message) {
    if (!good) { std::cerr << "FAIL: " << message << '\n'; std::abort(); }
}

static void maskedIdentifiers() {
    std::array<uint64_t, 19> bank{};
    bank[15] = 0xcafebabe551b7000;
    bank[16] = 0x7f00;
    bank[18] = 0x5566778811223344;
    Control control(bank.data());
    unsigned irqs = 0;
    control.configure(true, {[&] { ++irqs; }, {}, {}});
    check(!control.onAD(0x60, 0xffff0000aabbccdd), "initial SIGNAL does not stall");
    check(bank[18] == 0x55667788aabb3344, "SIGNAL applies ID mask to low half only");
    check(bank[15] == 0xcafebabe551b7001, "SIGNAL preserves unrelated CSR fields");
    check(irqs == 0, "SIGMSK suppresses IRQ, not signal latch");
    check(!control.onAD(0x62, 0x00ff00ff12345678), "LABEL never stalls");
    check(bank[18] == 0x55347778aabb3344, "LABEL applies mask to upper half only");
    control.onAD(0x62, 0x00000000ffffffff);
    check(bank[18] == 0x55347778aabb3344 && irqs == 0, "zero mask and LABEL produce no IRQ");
    control.onAD(0x7f, ~uint64_t(0));
    check(bank[15] == 0xcafebabe551b7001, "non-event A+D has no event effect");
}

static void repeatedSignalAndResumeOrder() {
    std::array<uint64_t, 19> bank{};
    bank[16] = 0x6000;
    Control control(bank.data());
    std::vector<std::string> order;
    unsigned resumes = 0;
    control.configure(true, {
        [&] { order.push_back("irq"); },
        [&] {
            order.push_back("resume");
            check(!control.stalled(), "pending SIGNAL cleared before resume hook");
            ++resumes;
            if (resumes == 1) {
                check(bank[18] == 0x22 && (bank[15] & 1), "queued ID visible before resume");
                // The next accepted QW in the retained tail is a third SIGNAL.
                check(control.onAD(0x60, 0xffffffff00000033), "resume can queue another SIGNAL");
            }
        }, {}});
    check(!control.onAD(0x60, 0xffffffff00000011), "first SIGNAL accepted");
    check(control.onAD(0x60, 0xffffffff00000022), "second SIGNAL creates stall");
    check(control.stalled() && bank[18] == 0x11, "stalled signal does not update ID yet");
    check(control.write(0x12001000, 32, 1), "CSR acknowledgement routed");
    check(control.stalled() && bank[18] == 0x22, "reentrant stall survives first acknowledgement");
    control.write(0x1000, 8, 1);
    check(!control.stalled() && bank[18] == 0x33 && (bank[15] & 1), "next acknowledgement delivers exact pending ID");
    control.write(0x1000, 64, 1);
    check(!(bank[15] & 1), "acknowledgement without queued signal clears latch");
    control.write(0x1000, 128, 1);
    check(order == std::vector<std::string>({"irq", "irq", "resume", "irq", "resume", "resume"}),
          "IRQ requests and synchronous resume follow reference order");
    check(control.counters().signalStalls == 2 && control.counters().resumes == 3,
          "repeat and resume aggregate counters");
}

static void finishAndMaskTransitions() {
    std::array<uint64_t, 19> bank{};
    bank[16] = 0x7f00;
    Control control(bank.data());
    unsigned irqs = 0;
    control.configure(true, {[&] { ++irqs; }, {}, {}});
    control.onAD(0x61, 0);
    check(control.finishPending() && !control.finishFired() && bank[15] == 0,
          "FINISH token only records pending work");
    control.finishEligible(true);
    check(control.finishPending() && bank[15] == 0, "other eligible path work defers FINISH");
    control.finishEligible(false);
    check(!control.finishPending() && !control.finishFired() && bank[15] == 2 && irqs == 0,
          "eligible masked FINISH latches without IRQ");
    control.write(0x1010, 32, 0x7d00);
    check(irqs == 1 && bank[16] == 0x7d00 && !control.finishFired(),
          "IMR unmask requests IRQ but reference leaves FINISH fired state alone");
    control.finishEligible(false);
    check(irqs == 2 && control.finishFired(), "next eligibility check sets FINISH fired and requests IRQ");
    control.finishEligible(false);
    check(irqs == 2, "eligible FINISH IRQ fires once after fired flag");
    control.write(0x1000, 16, 2);
    check(bank[15] == 0 && !control.finishPending() && !control.finishFired(), "FINISH acknowledgement clears all pending state");
    control.onAD(0x61, 0);
    control.write(0x1000, 32, 2);
    control.finishEligible(false);
    check(bank[15] == 0 && irqs == 2, "acknowledgement cancels pending FINISH before eligibility");
    bank[15] = 0x1f;
    bank[16] = 0x7f00;
    control.write(0x1010, 128, 0xffffffffffff0000ULL);
    check(irqs == 3 && bank[16] == 0x6000, "one IMR write unmasking many events makes one IRQ request");
    control.write(0x1010, 64, 0);
    check(irqs == 3, "already-unmasked write does not request another IRQ");
}

static void combinedAcknowledgementOrder() {
    std::array<uint64_t, 19> bank{};
    Control control(bank.data());
    std::vector<std::string> order;
    control.configure(true, {
        [&] { order.push_back("irq"); },
        [&] {
            order.push_back("resume");
            control.onAD(0x61, 0);
            control.finishEligible(false);
            check(bank[15] == 3, "resumed FINISH delivered before enclosing CSR FINISH ack");
        }, {}});
    control.onAD(0x60, 0xffffffff00000001);
    control.onAD(0x60, 0xffffffff00000002);
    control.write(0x1000, 32, 3);
    check(bank[15] == 1 && !control.finishPending() && !control.finishFired(),
          "combined SIGNAL/FINISH write acknowledges FINISH processed during resume");
    check(order == std::vector<std::string>({"irq", "irq", "resume", "irq"}),
          "combined acknowledgement preserves reference callback ordering");
}

static void mmioWidthsAndReset() {
    for (unsigned width : {8U, 16U, 32U, 64U, 128U}) {
        std::array<uint64_t, 19> bank{};
        bank[15] = 0x12345678551b701f;
        bank[16] = 0x7f00;
        Control control(bank.data());
        control.configure(true);
        check(control.write(0x12001000, width, 0x1f), "all CSR widths accepted");
        check(bank[15] == 0x12345678551b7000, "W1C clears event bits and preserves CSR identity/parity/FIFO");
        control.write(0x1002, 16, 0xffff);
        control.write(0x1003, 8, 0xff);
        check(bank[15] == 0x12345678551b7000, "CSR high action bytes cannot modify hardwired identity");
    }
    std::array<uint64_t, 19> bank{};
    bank[15] = 0x551b7001;
    bank[16] = 0xabcd000000007f00;
    Control control(bank.data());
    unsigned irqs = 0, resets = 0, resumes = 0;
    control.configure(true, {[&] { ++irqs; }, [&] { ++resumes; }, [&] {
        ++resets;
        check(!control.stalled() && !control.finishPending() && control.finishFired(), "reset hook observes cleared pending state");
        check(bank[15] == 0x551b6000 && bank[16] == 0x7f00, "reset hook observes initialized producer bank");
    }});
    control.write(0x1011, 8, 0);
    check(bank[16] == 0xabcd000000000000 && irqs == 0, "raw IMR byte write neither normalizes nor requests IRQ");
    control.write(0x1010, 16, 0x1f00);
    check(bank[16] == 0xabcd000000007f00, "normalized IMR write preserves upper bank word");
    control.write(0x1080, 64, 0x1122334455667788);
    control.write(0x1081, 8, 0xaa);
    control.write(0x1082, 16, 0xbbcc);
    control.write(0x1084, 32, 0xddeeff00);
    check(bank[18] == 0xddeeff00bbccaa88, "SIGLBLID raw subword writes preserve adjacent bytes");
    control.write(0x1080, 128, 0x123456789abcdef0);
    check(bank[18] == 0x123456789abcdef0, "SIGLBLID 128-bit low bank half retained");
    check(!control.write(0x1011, 16, 0), "unaligned unsupported access rejected");
    check(!control.write(0x1000, 24, 0), "unknown access width rejected");
    check(!control.write(0x1040, 64, 0), "other register stays with existing MMIO owner");
    control.onAD(0x60, 0xffffffff11111111);
    control.onAD(0x61, 0);
    bank[0] = 0xdeadbeef;
    control.write(0x1001, 8, 2);
    check(resets == 1 && resumes == 0 && bank[0] == 0 && bank[18] == 0,
          "CSR RESET byte clears privileged bank and pending events without resetting parser or resuming");
    control.finishEligible(false);
    check(irqs == 0 && bank[15] == 0x551b6000, "reset cannot resurrect pending FINISH");
}

static void legacyOff() {
    std::array<uint64_t, 19> bank{};
    bank[15] = 0xabcdef;
    const auto original = bank;
    Control control(bank.data());
    control.configure(false, {[] { std::abort(); }, [] { std::abort(); }, [] { std::abort(); }});
    check(!control.onAD(0x60, ~uint64_t(0)), "disabled event processing falls through");
    check(!control.write(0x1000, 32, 0x200), "disabled MMIO remains legacy-owned");
    control.finishEligible(false);
    check(bank == original, "disabled mode changes no guest state");
}

int main() {
    maskedIdentifiers();
    repeatedSignalAndResumeOrder();
    finishAndMaskTransitions();
    combinedAcknowledgementOrder();
    mmioWidthsAndReset();
    legacyOff();
    std::cout << "GS control source-backed semantic tests passed\n";
}
