#include "rrv_m2_causal_trace.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

namespace causal = rrv::m2causal;
namespace {
int failures = 0;
void check(bool value, const char *message)
{
    if (!value) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
std::vector<std::uint8_t> bytes(const std::filesystem::path &p)
{
    std::ifstream f(p, std::ios::binary);
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)), {});
}
std::uint32_t u32(const std::vector<std::uint8_t> &b, size_t p)
{ return b[p] | (std::uint32_t(b[p+1])<<8) | (std::uint32_t(b[p+2])<<16) | (std::uint32_t(b[p+3])<<24); }
std::uint64_t u64(const std::vector<std::uint8_t> &b, size_t p)
{ std::uint64_t v=0; for (unsigned i=0;i<8;++i) v |= std::uint64_t(b[p+i]) << (8*i); return v; }
void makeFourFields()
{
    const std::array<std::uint8_t, 3> vector{'a', 'b', 'c'};
    causal::packetFingerprint(causal::Source::Gif, vector.data(), vector.size(), 99u);
    causal::beginSelectorEdge(0x20000u, 0x20005u, 77u);
    causal::event(causal::EventType::PathSubmission, causal::Source::Gif, 2u, 320u, 9u);
    causal::packetFingerprint(causal::Source::Gif, vector.data(), vector.size(), 99u);
    causal::originBoundary(100u, 500u, 0u);
    for (std::uint64_t i=0; i<4; ++i) {
        causal::beginCompleteField(101u+i, 501u+i, unsigned(i&1));
        causal::event(causal::EventType::Vif1DmaStart, causal::Source::Vif1, i, 20u, 0u);
        causal::event(causal::EventType::TransferAcknowledged, causal::Source::Gif, 2u, 320u, 20u);
        causal::event(causal::EventType::LocalMemoryRestoreAcknowledged, causal::Source::Backend, 64u, 4u, i);
        causal::event(causal::EventType::GsVsyncAcknowledged, causal::Source::Backend, 157u, 0u, i);
        causal::completeField(101u+i, 501u+i, unsigned(i&1));
    }
}
void testModes(const std::filesystem::path &dir)
{
    const auto a=dir/"a.bin", b=dir/"b.bin";
    check(causal::resetForTest(true,a.c_str(),4u), "mode C initialization");
    for (unsigned i=0; i<131100u; ++i)
        causal::event(causal::EventType::TickPublished, causal::Source::VblankWorker, i);
    check(causal::recordCount()==0u && !causal::overflowed(), "pre-arm events are not retained");
    check(!causal::dumpDeferred(), "pre-arm export must fail");
    check(!std::filesystem::exists(a), "receipt exists before measured interval closes");
    makeFourFields();
    check(causal::recordCount() > 0 && !causal::overflowed(), "normal trace state");
    check(causal::dumpDeferred(), "normal deferred dump");
    const auto first=bytes(a);
    const auto closedCount=causal::recordCount();
    causal::event(causal::EventType::TickPublished, causal::Source::VblankWorker, 999u);
    causal::beginSelectorEdge(0,0x20005,999u);
    check(causal::recordCount()==closedCount, "closed interval cannot rearm or grow");
    check(first.size() >= 80u && u32(first,8)==1u && u32(first,12)==80u &&
              u32(first,16)==48u && u32(first,20)==2u, "fixed LE header");
    check(u64(first,48)==1u && u64(first,56)==causal::recordCount(),
          "header sequence bounds");
    check(u32(first,64)==4u && u32(first,68)==4u, "completed/target field counts");
    check(first.size()==80u + causal::recordCount()*48u, "record size is fixed");
    bool sawFingerprint = false;
    for (size_t off=80; off+48<=first.size(); off+=48) {
        if (u32(first, off+8)==static_cast<std::uint32_t>(causal::EventType::Path2PayloadFingerprint)) {
            sawFingerprint = true;
            check(u64(first, off+24)==0xe71fa2190541574bull, "FNV-1a fingerprint vector");
        }
    }
    check(sawFingerprint, "fingerprint is recorded only after collection starts");
    check(causal::dumpDeferred(), "completed trace remains exportable after first dump");

    check(causal::resetForTest(true,b.c_str(),4u), "repeat initialization");
    makeFourFields(); check(causal::dumpDeferred(), "repeat deferred dump");
    check(first==bytes(b), "repeated trace bytes are identical");

    const auto disabled=dir/"disabled.bin";
    check(causal::resetForTest(false,disabled.c_str(),4u), "disabled initialization");
    makeFourFields(); check(!causal::enabled() && causal::dumpDeferred(), "disabled mode is inert and dump is a no-op");
    check(!std::filesystem::exists(disabled), "disabled mode does not export");
}
void testConcurrency(const std::filesystem::path &dir)
{
    const auto p=dir/"concurrent.bin";
    check(causal::resetForTest(true,p.c_str(),1u), "concurrency initialization");
    causal::beginReplay();
    std::thread x([] { for (unsigned i=0;i<100;++i) causal::event(causal::EventType::Vif0DmaStart, causal::Source::Vif0,i); });
    std::thread y([] { for (unsigned i=0;i<100;++i) causal::event(causal::EventType::Vif1DmaStart, causal::Source::Vif1,i); });
    x.join(); y.join(); causal::completeField(1u, 1u, 0u);
    check(causal::dumpDeferred(), "concurrent trace export");
    const auto raw=bytes(p); check(raw.size()==80u+202u*48u, "concurrent record count");
    std::set<std::uint64_t> seq; for (size_t off=80; off+48<=raw.size(); off+=48) seq.insert(u64(raw,off));
    check(seq.size()==202u, "concurrent sequences contain no duplicates");
}
void testOverflow(const std::filesystem::path &dir)
{
    const auto p=dir/"overflow.bin";
    check(causal::resetForTest(true,p.c_str(),1u), "overflow initialization"); causal::beginReplay();
    for (unsigned i=0;i<131100u;++i) causal::event(causal::EventType::CooperativeYield, causal::Source::Replay,i);
    causal::completeField(1u,1u,0u);
    check(causal::overflowed(), "overflow is explicit"); check(!causal::dumpDeferred(), "overflow export fails");
    check(!std::filesystem::exists(p), "overflow creates no artifact");
}
void testScopeCrossingArm(const std::filesystem::path &dir)
{
    const auto p=dir/"partial-scope.bin";
    check(causal::resetForTest(true,p.c_str(),1u), "partial scope initialization");
    {
        causal::Scope scope(causal::EventType::GuestDispatchEnter,
                            causal::EventType::GuestDispatchExit,
                            causal::Source::GuestExecution, 0x220100u);
        causal::beginSelectorEdge(0x20000u,0x20005u,100u);
    }
    check(causal::recordCount()==2u, "post-arm exit of pre-arm scope is retained");
    causal::originBoundary(100u,100u,0u);
    causal::completeField(101u,101u,1u);
    check(causal::dumpDeferred(), "partial scope export");
    const auto raw=bytes(p);
    check(u32(raw,80u+48u+8u)==22u, "second event is the enclosing dispatch exit");
}
}
int main()
{
    std::string pattern=(std::filesystem::temp_directory_path()/"rrv-m2-causal-tests-XXXXXX").string();
    char *created=mkdtemp(pattern.data());
    if (!created) return 1;
    const std::filesystem::path dir(created);
    std::error_code ec;
    testModes(dir); testConcurrency(dir); testOverflow(dir); testScopeCrossingArm(dir);
    std::filesystem::remove_all(dir,ec);
    return failures;
}
