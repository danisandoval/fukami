#include "../src/gs-control/rrv_gif_control_stream.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

using rrv::gs::Control;
using rrv::gs::Stream;
using Bytes = std::vector<uint8_t>;

static void check(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::abort(); }
}
template<class F> static bool throws(F f) {
    try { f(); } catch (const std::exception&) { return true; }
    return false;
}
static void qw(Bytes& data, uint64_t lo, uint64_t hi) {
    const auto size = data.size();
    data.resize(size + 16);
    std::memcpy(data.data() + size, &lo, 8);
    std::memcpy(data.data() + size + 8, &hi, 8);
}
static void tag(Bytes& data, uint32_t loops, uint8_t format, uint8_t nregs,
                uint64_t regs, bool eop = true) {
    // Gif_Unit.h HW_Gif_Tag bit layout; NREG=0 means 16. Expected payload
    // lengths below are independently specified from setTag's format rules.
    qw(data, loops | (uint64_t(eop) << 15) | (uint64_t(format) << 58) |
        (uint64_t(nregs) << 60), regs);
}
static uint64_t id(uint32_t value, uint32_t mask = 0xffffffff) {
    return value | (uint64_t(mask) << 32);
}
struct Harness {
    std::array<uint64_t, 19> bank{};
    Control control{bank.data()};
    Stream stream{control};
    Bytes rendered;
    std::vector<uint64_t> irqIds;
    std::vector<uint8_t> paths;
    uint32_t rendererRemaining = 0, rendererTags = 0;
    Harness() {
        control.configure(true, {[this] { irqIds.push_back(bank[18]); },
            [this] { stream.drain(); }, [this] { rendererRemaining = rendererTags = 0; }});
    }
    void submit(uint8_t path, const Bytes& bytes) {
        stream.submit(path, bytes.data(), static_cast<uint32_t>(bytes.size()),
            [this, path](const uint8_t* data, uint32_t size) {
                paths.push_back(path);
                rendered.insert(rendered.end(), data, data + size);
                // Independent minimal renderer framing oracle: its parser
                // resets on GS RESET, unlike the producer's retained cursor.
                for (uint32_t i = 0; i < size; i += 16) {
                    if (rendererRemaining) { --rendererRemaining; continue; }
                    uint64_t lo;
                    std::memcpy(&lo, data + i, 8);
                    const unsigned loops = lo & 0x7fff;
                    const unsigned format = (lo >> 58) & 3;
                    unsigned nregs = (lo >> 60) & 15;
                    if (!nregs) nregs = 16;
                    rendererRemaining = format == 0 ? loops * nregs :
                        format == 1 ? (loops * nregs + 1) / 2 : loops;
                    ++rendererTags;
                }
            });
    }
    void ack(uint64_t mask = 1) { check(control.write(0x12001000, 64, mask), "CSR MMIO handled"); }
};

static void repeatedSignalOwnedResume() {
    Harness h;
    Bytes packet;
    tag(packet, 4, 0, 1, 0xe);
    qw(packet, id(0x11223344), 0x60);
    qw(packet, id(0xaabbccdd, 0xffff), 0x60);
    qw(packet, id(0x55667788), 0x62);
    qw(packet, 0, 0x61);
    const Bytes original = packet;
    h.submit(3, packet);
    check(h.control.stalled() && h.stream.pendingPath(3), "repeat SIGNAL stalls active path");
    check(h.rendered.empty(), "incomplete repeated-SIGNAL packet retained before renderer");
    check(h.bank[18] == 0x11223344 && h.irqIds.size() == 1, "pending ID not prematurely delivered");
    check(h.stream.cursor().remainingQwords == 2, "cursor advances past repeated word");
    packet.assign(packet.size(), 0xff);
    packet.clear(); packet.shrink_to_fit();
    h.ack();
    check(!h.control.stalled() && !h.stream.pending(), "CSR ack resumes complete owned tail");
    check(h.rendered == original, "producer reuse cannot corrupt retained tail");
    check(h.bank[18] == 0x556677881122ccddULL, "masked pending SIGNAL and subsequent LABEL");
    check((h.bank[15] & 3) == 3 && h.irqIds.size() == 3, "pending SIGNAL then eligible FINISH IRQ");
    check(h.control.counters().signals == 2 && h.control.counters().labels == 1, "events parsed once");
    h.ack(3);
    check((h.bank[15] & 3) == 0, "second ack clears delivered SIGNAL and FINISH");
    check(h.stream.stats().submittedBytes == h.stream.stats().consumedBytes &&
        h.stream.stats().pendingBytes == 0, "all owned bytes released");
}

static void decodedFormatsAndFragmentation() {
    Harness h;
    Bytes packet;
    // PACKED nregs=2: only the SECOND word in each pair has A+D semantics.
    tag(packet, 2, 0, 2, 0xe1);
    qw(packet, id(0xdead), 0x60); // non-A+D payload must not SIGNAL
    qw(packet, id(0x1234), 0x62);
    qw(packet, id(0xbeef), 0x61); // non-A+D payload must not FINISH
    qw(packet, id(0x5678), 0x62);
    // REGLIST odd payload rounds up to two QWs, including padding.
    tag(packet, 1, 1, 3, 0xeee);
    qw(packet, id(0xdead), 0x60);
    qw(packet, id(0xbeef), 0x61);
    // Both IMAGE encodings treat all words as pixels, including 0x60..62.
    for (uint8_t format : {2, 3}) {
        tag(packet, 2, format, 0, 0);
        qw(packet, id(0xdead), 0x60);
        qw(packet, id(0xbeef), 0x62);
    }
    // PACKED NREG=0 is sixteen; the sixteenth entry is A+D.
    tag(packet, 1, 0, 0, 0xe111111111111111ULL);
    for (unsigned i = 0; i != 15; ++i) qw(packet, id(i), 0x60);
    qw(packet, id(0x9876), 0x62);
    for (size_t offset = 0; offset < packet.size(); offset += 16)
        h.submit(2, Bytes(packet.begin() + offset, packet.begin() + offset + 16));
    check(h.rendered == packet && !h.stream.pending(), "all formats persist cursor across QW fragments");
    check(h.bank[18] == 0x987600000000ULL, "only decoded A+D LABEL changes ID");
    check(h.control.counters().labels == 3 && h.control.counters().signals == 0 &&
        h.control.counters().finishes == 0, "no arbitrary payload event scanning");
}

static Bytes labelPacket(uint32_t value) {
    Bytes packet;
    tag(packet, 1, 0, 1, 0xe);
    qw(packet, id(value), 0x62);
    return packet;
}
static void activePathAndGlobalStall() {
    Harness h;
    Bytes start;
    tag(start, 2, 0, 1, 0xe, false);
    qw(start, id(1), 0x60);
    h.submit(3, start); // incomplete P3 tag
    const Bytes other = labelPacket(2);
    h.submit(1, other);
    check(h.rendered.empty() && h.stream.pendingPath(1), "incomplete tag staged, other path cannot enter flattened tag");
    Bytes continuation;
    qw(continuation, id(3), 0x60); // completes tag, repeated SIGNAL
    h.submit(3, continuation);
    check(h.control.stalled() && h.paths.empty(), "non-EOP final-word repeat keeps packet staged and globally stalls");
    check(h.stream.cursor().tagValid && h.stream.cursor().remainingQwords == 0,
        "non-EOP repeat retains reference zero-remaining valid cursor until ack");
    h.submit(2, labelPacket(4));
    Bytes end;
    tag(end, 0, 0, 1, 0xe); // explicit EOP, no payload
    h.submit(3, end);
    check(h.control.counters().labels == 0, "no other path bypasses stalled tail");
    h.ack();
    check(h.paths == std::vector<uint8_t>({3, 3, 3, 1, 2}), "active packet finishes before queued paths, accepted paths retain order");
    check(h.control.counters().labels == 2 && !h.stream.pending(), "all paths resume without loss");
    check(h.bank[18] == 0x400000003ULL, "global event ordering agrees with resumed packet order");
}

static void finishEligibilityAndReset() {
    Harness h;
    Bytes begin;
    tag(begin, 3, 0, 1, 0xe);
    h.submit(3, begin);
    h.submit(1, labelPacket(8)); // queued OTHER path blocks eligibility
    Bytes finish;
    qw(finish, 0, 0x61);
    h.submit(3, finish);
    check(h.control.finishPending() && !(h.bank[15] & 2), "FINISH waits while other path has accepted work");
    Bytes tail;
    qw(tail, id(1), 0x62); qw(tail, id(2), 0x62);
    h.submit(3, tail);
    check(!h.control.finishPending() && (h.bank[15] & 2), "FINISH eligible after other path processed");
    Harness single;
    single.submit(2, begin);
    single.submit(2, finish);
    check((single.bank[15] & 2) && single.stream.pendingPath(2), "active incomplete path alone does not block reference FINISH");

    Harness reset;
    Bytes repeated;
    tag(repeated, 3, 0, 1, 0xe);
    qw(repeated, id(1), 0x60); qw(repeated, id(2), 0x60); qw(repeated, id(3), 0x62);
    reset.submit(1, repeated);
    const auto cursor = reset.stream.cursor();
    reset.control.write(0x1000, 32, 0x200);
    check(!reset.control.stalled() && reset.stream.cursor().remainingQwords == cursor.remainingQwords,
        "CSR reset clears event stall without rewinding GIF cursor");
    check(reset.stream.stats().pendingBytes == repeated.size(), "reset retains owned unconsumed GIF tail");
    reset.stream.drain();
    check(reset.rendered == repeated && reset.bank[18] == 0x300000000ULL, "post-reset resume executes only retained tail");
    check(reset.rendererTags == 1 && reset.rendererRemaining == 0,
        "renderer reset receives retained original tag with tail, never naked payload");
}

static void batchEligibility() {
    Harness h;
    Bytes first;
    tag(first, 3, 0, 1, 0xe);
    qw(first, 0, 0x61); qw(first, id(1), 0x60); qw(first, id(2), 0x60);
    const auto sink = [&h](const uint8_t* data, uint32_t size) {
        h.rendered.insert(h.rendered.end(), data, data + size);
    };
    h.stream.enqueue(1, first.data(), first.size(), sink);
    Bytes other = labelPacket(4);
    h.stream.enqueue(2, other.data(), other.size(), sink);
    check(h.control.counters().finishes == 0, "batch admission alone causes no guest events");
    h.stream.drain();
    check(h.control.finishPending() && !(h.bank[15] & 2),
        "stalled first record FINISH sees later accepted batch path");
    const auto eligibility = h.control.counters().finishEligibilityChecks;
    h.stream.drain();
    check(h.control.counters().finishEligibilityChecks == eligibility,
        "already stalled Execute entry performs no FINISH eligibility check");
    h.ack();
    check((h.bank[15] & 2) && !h.stream.pending(), "batch FINISH becomes eligible only after queued path resumes");
}

static void admissionFailureAndLifecycle() {
    Harness h;
    Bytes packet = labelPacket(1);
    check(throws([&] { h.stream.submit(0, packet.data(), packet.size(), [](auto, auto) {}); }), "invalid path rejected");
    check(throws([&] { h.stream.submit(1, packet.data(), 17, [](auto, auto) {}); }), "partial QW rejected");
    check(throws([&] { h.stream.submit(1, packet.data(), Stream::CapacityBytes + 16, [](auto, auto) {}); }), "oversize rejected before payload access");
    check(h.stream.stats().submittedRecords == 0, "invalid admissions have no side effects");
    Bytes repeat;
    tag(repeat, 2, 0, 1, 0xe); qw(repeat, id(1), 0x60); qw(repeat, id(2), 0x60);
    h.submit(1, repeat);
    Bytes full(Stream::CapacityBytes, 0);
    h.submit(2, full);
    check(h.stream.stats().pendingBytes == Stream::CapacityBytes, "stalled payload bound can be filled exactly");
    check(throws([&] { h.submit(3, packet); }), "full pending bound rejects before state changes without host wait");
    check(h.stream.stats().highWaterBytes == Stream::CapacityBytes, "owned allocation highwater bounded");
    // Destruction while guest-stalled owns and destroys callbacks/payloads;
    // there is no host waiter which needs the guest's CSR acknowledgement.
    Harness failed;
    check(throws([&] { failed.stream.submit(1, packet.data(), packet.size(), [](auto, auto) {
        throw std::runtime_error("test sink failure"); }); }), "sink failure propagates");
    check(throws([&] { failed.stream.drain(); }), "failed stream cannot replay committed events");
    check(failed.control.counters().labels == 1, "sink failure cannot duplicate event");
}

int main() {
    repeatedSignalOwnedResume();
    decodedFormatsAndFragmentation();
    activePathAndGlobalStall();
    finishEligibilityAndReset();
    batchEligibility();
    admissionFailureAndLifecycle();
    std::cout << "GS control GIF stream tests passed\n";
}
