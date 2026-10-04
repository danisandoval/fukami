#include "rrv_m2_causal_trace.h"
#include "rrv_m2_guest_provenance.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <unistd.h>

namespace causal = rrv::m2causal;
namespace guest = rrv::m2guest;
namespace
{
int failures = 0;
void check(bool value, const char *message)
{
    if (!value) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
std::vector<uint8_t> bytes(const std::filesystem::path &path)
{
    std::ifstream file(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)), {});
}
uint32_t u32(const std::vector<uint8_t> &data, size_t offset)
{
    return data[offset] | (uint32_t{data[offset + 1u]} << 8u) |
           (uint32_t{data[offset + 2u]} << 16u) | (uint32_t{data[offset + 3u]} << 24u);
}
uint64_t u64(const std::vector<uint8_t> &data, size_t offset)
{
    uint64_t value = 0u;
    for (uint32_t index = 0u; index < 8u; ++index)
        value |= uint64_t{data[offset + index]} << (index * 8u);
    return value;
}
size_t count(const std::vector<uint8_t> &data, guest::Event event)
{
    size_t result = 0u;
    for (size_t offset = 80u; offset + 48u <= data.size(); offset += 48u)
        result += u32(data, offset + 8u) == static_cast<uint32_t>(event) ? 1u : 0u;
    return result;
}
uint64_t firstA(const std::vector<uint8_t> &data, guest::Event event)
{
    for (size_t offset = 80u; offset + 48u <= data.size(); offset += 48u)
        if (u32(data, offset + 8u) == static_cast<uint32_t>(event)) return u64(data, offset + 24u);
    return UINT64_MAX;
}
uint64_t firstC(const std::vector<uint8_t> &data, guest::Event event)
{
    for (size_t offset = 80u; offset + 48u <= data.size(); offset += 48u)
        if (u32(data, offset + 8u) == static_cast<uint32_t>(event)) return u64(data, offset + 40u);
    return UINT64_MAX;
}
uint64_t firstB(const std::vector<uint8_t> &data, guest::Event event)
{
    for (size_t offset = 80u; offset + 48u <= data.size(); offset += 48u)
        if (u32(data, offset + 8u) == static_cast<uint32_t>(event)) return u64(data, offset + 32u);
    return UINT64_MAX;
}
uint64_t nthC(const std::vector<uint8_t> &data, guest::Event event, size_t ordinal = 0u)
{
    for (size_t offset = 80u; offset + 48u <= data.size(); offset += 48u)
        if (u32(data, offset + 8u) == static_cast<uint32_t>(event) && ordinal-- == 0u)
            return u64(data, offset + 40u);
    return UINT64_MAX;
}
bool sawHleAlias(const std::vector<uint8_t> &data)
{
    for (size_t offset = 80u; offset + 48u <= data.size(); offset += 48u)
    {
        if (u32(data, offset + 8u) == static_cast<uint32_t>(guest::Event::WriteBegin) &&
            uint32_t(u64(data, offset + 32u)) == 0u &&
            uint32_t(u64(data, offset + 32u) >> 32u) == 0xa0347cefu)
            return true;
    }
    return false;
}

struct RegisterView
{
    uint32_t ra;
    uint32_t sp;
    bool valid;
};

bool readRegisters(const void *context, uint32_t &ra, uint32_t &sp) noexcept
{
    const auto *const view = static_cast<const RegisterView *>(context);
    if (!view || !view->valid)
        return false;
    ra = view->ra;
    sp = view->sp;
    return true;
}

void testEntryExitWriteAndControl(const std::filesystem::path &directory)
{
    const auto output = directory / "guest.bin";
    check(causal::resetForTest(true, output.c_str(), 1u), "causal initialization");
    check(guest::resetForTest(true) && guest::configured(), "guest provenance enabled");
    std::vector<uint8_t> rdram(guest::kSourceRangeStart + guest::kSourceRangeBytes + 16u, 0x11u);
    causal::beginReplay();
    uint64_t outerId = guest::kUnknownId;
    uint32_t livePc = 0x00295e38u;
    {
        guest::DispatchScope outer(0x00295e38u, 0x00295e38u, 0x00295e50u, 0x1000u,
                                  rdram.data(), guest::DispatchReason::Unknown, &livePc);
        outerId = outer.id();
        check(outerId != guest::kUnknownId && guest::currentId() == outerId, "entry owns TLS dispatch ID");
        guest::controlInputs(0x00295e38u, 1u, 2u, 0x1122334455667788ull, 0x00340000u, 0x7fu, true);
        guest::preemptCheck(9u, 2u, 100u);
        guest::preemptSuppressed(1u, 3u);
        guest::sourceSnapshot(77u, 0u, guest::kSourceRangeStart - 2u,
                              rdram.data() + guest::kSourceRangeStart - 2u, 4u);
        guest::sourceSnapshot(78u, 1u, guest::kSourceRangeStart,
                              rdram.data() + guest::kSourceRangeStart, 1u);
        {
            guest::DispatchScope nested(0x0029c460u, 0x0029c460u, 0x0029c46cu, 0x2000u, rdram.data());
            check(nested.id() != outerId && guest::currentId() == nested.id(), "nested dispatch replaces TLS ID");
            nested.finish(0x0029c460u, 0x0029c46cu, 0x2000u);
        }
        check(guest::currentId() == outerId, "nested dispatch restores TLS ID");
        {
            guest::WriteScope write(rdram.data(), 0x00295e38u, guest::kSourceRangeStart - 2u,
                                    guest::kSourceRangeStart - 2u, 320u, 3u);
            check(write.id() != guest::kUnknownId, "overlapping write gets provenance ID");
            rdram[guest::kSourceRangeStart] = 0x22u;
            rdram[guest::kSourceRangeStart + 1u] = 0x33u;
            write.finish();
        }
        {
            // HLE may expose a KSEG guest address while passing the exact
            // RDRAM physical offset. The physical range, not guest alias,
            // decides whether this narrow diagnostic applies.
            guest::WriteScope hle(rdram.data(), 0u, 0xa0347cefu,
                                  guest::kSourceRangeStart + 319u, 8u, 4u);
            check(hle.id() != guest::kUnknownId, "physical HLE alias overlaps source range");
            rdram[guest::kSourceRangeStart + 319u] = 0x44u;
            hle.finish();
        }
        outer.finish(0x00295e38u, 0x00295e50u, 0x1000u);
    }
    check(guest::currentId() == guest::kUnknownId, "outer dispatch restores empty TLS ID");
    causal::completeField(1u, 1u, 0u);
    check(causal::dumpDeferred(), "deferred guest trace export");
    const auto data = bytes(output);
    check(count(data, guest::Event::DispatchEntry) == 2u && count(data, guest::Event::DispatchExit) == 2u,
          "entry and actual exit are distinct records");
    check(count(data, guest::Event::EntryRangeSnapshot) == 2u && count(data, guest::Event::ExitRangeSnapshot) == 2u,
          "fixed 320-byte entry/exit snapshots recorded");
    check(count(data, guest::Event::WriteBegin) == 2u && count(data, guest::Event::WriteBefore) == 2u &&
              count(data, guest::Event::WriteAfter) == 2u && sawHleAlias(data),
          "physical-alias writes retain PC=0 and route-specific provenance");
    check(count(data, guest::Event::ControlRegisters) == 1u && count(data, guest::Event::ControlS7) == 1u &&
              count(data, guest::Event::ControlByte) == 1u,
          "only supplied static-slice control inputs recorded");
    check(firstB(data, guest::Event::ControlS7) == 0x1122334455667788ull,
          "control s7 preserves its full 64-bit GPR value");
    check(count(data, guest::Event::PreemptCheck) == 1u && count(data, guest::Event::PreemptSuppressed) == 1u,
          "selected-scope preemption decisions recorded from supplied inputs");
    check(count(data, guest::Event::SourceSnapshot) == 1u && (firstB(data, guest::Event::SourceSnapshot) >> 32u) == 2u,
          "actual RDRAM source snapshot is clipped; scratchpad source is excluded");
    check((firstC(data, guest::Event::WriteBegin) >> 32u) == 320u &&
              (firstC(data, guest::Event::WriteAfter) >> 32u) == 318u,
          "wide boundary write reads only its clipped 318-byte source range");
}

void testUnknownEntryAndAbortedWrite(const std::filesystem::path &directory)
{
    const auto output = directory / "partial.bin";
    check(causal::resetForTest(true, output.c_str(), 1u), "partial causal initialization");
    check(guest::resetForTest(true), "partial guest initialization");
    std::vector<uint8_t> rdram(guest::kSourceRangeStart + guest::kSourceRangeBytes, 0u);
    {
        guest::DispatchScope prearm(1u, 2u, 3u, 4u, rdram.data());
        check(prearm.id() == guest::kUnknownId, "pre-arm dispatch has no retained ID");
        causal::beginReplay();
        prearm.finish(5u, 6u, 7u);
        {
            guest::WriteScope write(rdram.data(), 8u, guest::kSourceRangeStart,
                                    guest::kSourceRangeStart, 1u, 1u);
            check(write.id() != guest::kUnknownId, "aborted write started while collecting");
        }
    }
    causal::completeField(1u, 1u, 0u);
    check(causal::dumpDeferred(), "partial deferred trace export");
    const auto data = bytes(output);
    check(count(data, guest::Event::DispatchEntry) == 0u && firstA(data, guest::Event::DispatchExit) == 0u,
          "post-arm pre-entry exit remains explicitly unknown");
    check((firstB(data, guest::Event::DispatchSelection) >> 32u) == 2u,
          "unknown pre-arm entry is flagged rather than promoted to normal entry");
    check(count(data, guest::Event::WriteFailed) == 1u, "un-finished write fails explicitly");
}

void testCarryInAndHelperInputs(const std::filesystem::path &directory)
{
    const auto output = directory / "carry-in.bin";
    check(causal::resetForTest(true, output.c_str(), 1u), "carry-in causal initialization");
    check(guest::resetForTest(true), "carry-in guest initialization");
    std::vector<uint8_t> rdram(0x02000000u, 0u);
    constexpr uint32_t a1 = 0xa0010000u;
    for (uint32_t index = 0u; index < 8u; ++index)
    {
        rdram[0x10020u + index] = static_cast<uint8_t>(index + 1u);
        rdram[0x10050u + index] = static_cast<uint8_t>(0xa0u + index);
    }
    uint32_t livePc = 0x00295e38u;
    const RegisterView registers{0x00295e50u, 0x01fffdf0u, true};
    {
        // Scope enters before the selector arm.  It has no retained entry or
        // dispatch ID, but can report real carry-in preemption after arm.
        guest::DispatchScope carryIn(0x0020eec8u, 0x0020eec8u, 0u, 0u, rdram.data(),
                                     guest::DispatchReason::Unknown, &livePc,
                                     &registers, readRegisters);
        check(carryIn.id() == 0u, "carry-in scope has no fabricated entry ID");
        causal::beginReplay();
        guest::preemptCheck(63u, 2u, 64u);
        guest::preemptSuppressed(1u, 7u);
        guest::preemptYield();
        guest::helperEntry(0x00295e50u, 0x12345678u, a1, 0x01fffdf0u, rdram.data());
        // Both input addresses are outside the 32 MiB physical RDRAM domain.
        guest::helperEntry(0u, 0u, 0x1fffffa0u, 0u, rdram.data());
        {
            // A partial scalar write observes only slot zero and retains the
            // full eight-byte before/after word around its exact 4-byte span.
            guest::WriteScope partial(rdram.data(), 0x00200010u, 0x00346fa4u,
                                      0x00346fa4u, 4u, 8u);
            rdram[0x346fa4u] = 0x55u;
            partial.finish();
        }
        {
            // One original bulk write crosses both isolated source slots.
            guest::WriteScope bulk(rdram.data(), 0x00200020u, 0x00346f9cu,
                                   0x00346f9cu, 0x3cu, 9u);
            rdram[0x346fa0u] = 0x66u;
            rdram[0x346fd0u] = 0x77u;
            bulk.finish();
        }
        {
            // The gap is deliberately unobserved; the slots are never
            // widened into their containing packet/output range.
            guest::WriteScope gap(rdram.data(), 0x00200030u, 0x00346fb0u,
                                  0x00346fb0u, 8u, 10u);
            gap.finish();
        }
        {
            guest::WriteScope aborted(rdram.data(), 0x00200040u, 0x00346fd0u,
                                      0x00346fd0u, 1u, 11u);
        }
        carryIn.finish(0x0020eec8u, 0x0020ee6cu, 0x01ffff00u);
    }
    causal::completeField(1u, 1u, 0u);
    check(causal::dumpDeferred(), "carry-in deferred trace export");
    const auto data = bytes(output);
    check(count(data, guest::Event::DispatchEntry) == 0u, "carry-in records no invented entry");
    check(count(data, guest::Event::PreemptCheck) == 1u && firstA(data, guest::Event::PreemptCheck) == 0u,
          "unselected carry-in preemption retains ID zero");
    check(count(data, guest::Event::PreemptContext) == 2u &&
              firstB(data, guest::Event::PreemptContext) ==
                  (uint64_t{0x00295e50u} | (uint64_t{0x01fffdf0u} << 32u)),
          "carry-in context reports callback RA/SP adjacent to each preemption decision");
    check((firstC(data, guest::Event::PreemptContext) >> 32u) == 1u,
          "carry-in context marks callback register validity");
    check(count(data, guest::Event::PreemptSuppressed) == 1u &&
              count(data, guest::Event::PreemptYield) == 1u &&
              firstA(data, guest::Event::PreemptYield) == 0u,
          "carry-in suppression and successful yield retain unknown entry identity");
    check(count(data, guest::Event::HelperEntry) == 2u && count(data, guest::Event::HelperLink) == 2u &&
              count(data, guest::Event::HelperInput20) == 1u && count(data, guest::Event::HelperInput50) == 1u,
          "helper records exact valid inputs and explicitly omits out-of-RDRAM words");
    check(firstB(data, guest::Event::HelperEntry) ==
              (uint64_t{0x00295e50u} | (uint64_t{0x12345678u} << 32u)) &&
              firstC(data, guest::Event::HelperEntry) ==
                  (uint64_t{a1} | (uint64_t{0x01fffdf0u} << 32u)),
          "helper tuple preserves caller RA/a0/a1/SP without host pointers");
    check(firstC(data, guest::Event::HelperInput20) == 0x0807060504030201ull &&
              firstC(data, guest::Event::HelperInput50) == 0xa7a6a5a4a3a2a1a0ull,
          "helper inputs are serialized as exact little-endian eight-byte words");
    check((nthC(data, guest::Event::HelperLink, 1u) & 0xffffffffu) == 0u,
          "out-of-RDRAM helper inputs retain an explicit zero validity mask");
    check(count(data, guest::Event::InputWriteBegin) == 4u &&
              count(data, guest::Event::InputWriteBefore) == 4u &&
              count(data, guest::Event::InputWriteAfter) == 3u &&
              count(data, guest::Event::InputWriteFailed) == 1u,
          "two fixed source watches preserve partial, dual-slot bulk, and aborted write chains");
    check(firstB(data, guest::Event::InputWriteBegin) ==
              (uint64_t{0x00200010u} | (uint64_t{0x00346fa4u} << 32u)) &&
              firstC(data, guest::Event::InputWriteBegin) ==
                  (uint64_t{0x00346fa4u} | (uint64_t{4u} << 32u)),
          "source writer begin retains exact guest PC, virtual/physical address, and original width");
    check(firstB(data, guest::Event::InputWriteOverlap) ==
              (uint64_t{0x00346fa0u} | (uint64_t{4u} << 32u)) &&
              firstC(data, guest::Event::InputWriteOverlap) == 4u,
          "source writer overlap retains isolated slot address, offset, and clipped width");
}

void testDisabled(const std::filesystem::path &directory)
{
    const auto output = directory / "disabled.bin";
    check(causal::resetForTest(false, output.c_str(), 1u), "disabled causal initialization");
    check(guest::resetForTest(false) && !guest::configured(), "disabled guest initialization");
    std::vector<uint8_t> rdram(guest::kSourceRangeStart + guest::kSourceRangeBytes, 0u);
    guest::DispatchScope dispatch(1u, 2u, 3u, 4u, rdram.data());
    guest::WriteScope write(rdram.data(), 5u, guest::kSourceRangeStart,
                            guest::kSourceRangeStart, 1u, 0u);
    guest::controlInputs(0x00295e38u, 1u, 2u, 3u, 4u, 5u, true);
    guest::preemptCheck(1u, 2u, 3u);
    guest::preemptSuppressed(1u, 2u);
    guest::preemptYield();
    guest::helperEntry(6u, 7u, 8u, 9u, rdram.data());
    dispatch.finish(6u, 7u, 8u);
    write.finish();
    check(guest::currentId() == guest::kUnknownId && causal::dumpDeferred(), "disabled hooks stay inert");
    check(!std::filesystem::exists(output), "disabled hooks do not export a trace");
}
} // namespace

int main()
{
    std::string pattern = (std::filesystem::temp_directory_path() / "rrv-m2-guest-provenance-XXXXXX").string();
    char *created = mkdtemp(pattern.data());
    if (!created) return 1;
    const std::filesystem::path directory(created);
    std::error_code error;
    testEntryExitWriteAndControl(directory);
    testUnknownEntryAndAbortedWrite(directory);
    testCarryInAndHelperInputs(directory);
    testDisabled(directory);
    std::filesystem::remove_all(directory, error);
    return failures;
}
