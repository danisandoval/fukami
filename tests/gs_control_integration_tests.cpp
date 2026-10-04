// SPDX-License-Identifier: GPL-3.0+
// Asset-free product integration tests. Expectations come from PCSX2
// fd9d310ccbb6b8b62c976da8886a3c8fd3a10ff3 Gif_HandlerAD/Gif_FinishIRQ,
// GS.cpp gsCSRwrite/IMRwrite and Vif_Codes.cpp DIRECT/FLUSH/MSCALF.
// A byte-collecting sink substitutes only GS rendering. Memory, GIF arbitration,
// VIF parsing, MMIO, DMA completion and guest INTC dispatch are product code.
#include "ps2_runtime.h"
#include "Syscalls/Interrupt.h"
#include "Syscalls/System.h"
#include "rrv_gs_worker.h"
#include <algorithm>
#include <atomic>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace ps2_syscalls { bool dispatchGsControlInterrupt(uint8_t*, PS2Runtime*); uint32_t gsControlIntcMask(uint32_t, bool); }
namespace {
constexpr uint32_t CSR = 0x12001000, IMR = 0x12001010, ID = 0x12001080;
constexpr uint32_t GIF_DMA = 0x1000a000, VIF_DMA = 0x10009000, DSTAT = 0x1000e010;
void check(bool yes, const char* why) { if (!yes) throw std::runtime_error(why); }
void put32(std::vector<uint8_t>& bytes, uint32_t value) {
    for (unsigned i=0; i<4; ++i) bytes.push_back(uint8_t(value >> (i*8)));
}
void put64(std::vector<uint8_t>& bytes, uint64_t value) {
    for (unsigned i=0; i<8; ++i) bytes.push_back(uint8_t(value >> (i*8)));
}
struct AD { uint8_t reg; uint64_t value; };
std::vector<uint8_t> packet(std::initializer_list<AD> regs, bool eop=true) {
    std::vector<uint8_t> bytes;
    put64(bytes, uint64_t(regs.size()) | (uint64_t(eop) << 15) | (uint64_t(1) << 60));
    put64(bytes, 0xe); // PACKED, one A+D descriptor.
    for (auto reg: regs) { put64(bytes,reg.value); put64(bytes,reg.reg); }
    return bytes;
}
std::vector<uint8_t> direct(const std::vector<uint8_t>& gif, bool hl=false) {
    std::vector<uint8_t> bytes;
    put32(bytes, (uint32_t(hl ? 0x51 : 0x50) << 24) | uint32_t(gif.size()/16));
    bytes.insert(bytes.end(),gif.begin(),gif.end());
    return bytes;
}
uint64_t id(uint32_t value, uint32_t mask=~uint32_t(0)) { return uint64_t(value) | (uint64_t(mask)<<32); }
struct IrqSeen { uint64_t csr, siglbl; };
std::vector<IrqSeen>* irqLog;
void guestIrq(uint8_t*, R5900Context* ctx, PS2Runtime* runtime) {
    irqLog->push_back({runtime->memory().read64(CSR), runtime->memory().read64(ID)});
    check((_mm_extract_epi32(ctx->r[4],0) == 0), "actual INTC cause is GS=0");
    runtime->memory().writeIORegister(0x1000f000,1);
    ctx->pc=0;
}
struct Fixture {
    PS2Runtime runtime;
    std::unique_ptr<rrv::gs::Worker> worker;
    std::vector<uint8_t> rendered;
    std::vector<IrqSeen> interrupts;
    std::vector<uint32_t> dma;
    unsigned resets=0;
    std::thread::id producer=std::this_thread::get_id(), owner{};
    explicit Fixture(bool sync) {
        check(runtime.memory().initialize(), "memory initialize");
        if(sync) worker=std::make_unique<rrv::gs::Worker>();
        irqLog=&interrupts;
        runtime.registerFunction(0x200000,guestIrq);
        ps2_syscalls::IrqHandlerSnapshotCounters counters{};
        counters.enabledIntcMask=1; counters.nextIntcHandlerId=2; counters.nextDmacHandlerId=1;
        ps2_syscalls::importIrqHandlerState({{0,1,0,0x200000,0,0,0,1,0}},counters);
        auto& m=runtime.memory(); auto& a=runtime.gifArbiter();
        m.setGifArbiter(&a);
        m.gsControl().configure(true, {
            [&] { m.latchGsControlIrq(); },
            [&] { a.drain(); m.resumeGsControlWork(); },
            [&] { ++resets; }
        });
        m.m_gsIntcMask=ps2_syscalls::gsControlIntcMask;
        m.setGsControlIrqCallback([&] { return ps2_syscalls::dispatchGsControlInterrupt(m.getRDRAM(),&runtime); });
        m.setDmacCompletionCallback([&](uint32_t cause) { dma.push_back(cause); });
        a.setControl(&m.gsControl());
        a.setProcessPacketFn([&](GifPathId, const uint8_t* data, uint32_t bytes, uint32_t, const GifPacketDiagnostic*) {
            std::vector<uint8_t> owned(data,data+bytes);
            auto consume=[this,owned=std::move(owned)] {
                owner=std::this_thread::get_id();
                rendered.insert(rendered.end(),owned.begin(),owned.end());
            };
            if(worker) worker->invoke(std::move(consume),bytes);
            else consume();
        });
    }
    ~Fixture() {
        if(worker) worker->stop();
        runtime.memory().setGsControlIrqCallback({});
        ps2_syscalls::importIrqHandlerState({},{});
    }
    PS2Memory& m() { return runtime.memory(); }
    void send(const std::vector<uint8_t>& p, GifPathId path=GifPathId::Path3) {
        m().submitGifPacket(path,p.data(),uint32_t(p.size()));
    }
    void kick(uint32_t channel, const std::vector<uint8_t>& bytes, uint32_t at=0x10000) {
        check(bytes.size()%16==0,"DMA test payload is qword-aligned");
        std::memcpy(m().getRDRAM()+at,bytes.data(),bytes.size());
        runtime.Store32(m().getRDRAM(),&runtime.cpu(),channel+0x10,at);
        runtime.Store32(m().getRDRAM(),&runtime.cpu(),channel+0x20,uint32_t(bytes.size()/16));
        runtime.Store32(m().getRDRAM(),&runtime.cpu(),channel,0x101);
    }
    void stalled() {
        send(packet({{0x60,id(1)},{0x60,id(2)},{0x62,id(3)}}));
        check(m().gsControl().stalled(),"duplicate SIGNAL stalls accepted stream");
        check(m().read64(ID)==1,"duplicate identifier and later LABEL not prematurely visible");
    }
};

void expectResultRejected(Fixture& f, rrv::gs::ProducerWork expected, const char* state) {
    const auto boundary = f.m().gsResultBoundary();
    check((static_cast<uint32_t>(boundary.work) & static_cast<uint32_t>(expected)) != 0u, state);
    const uint64_t csr = f.m().read64(CSR);
    const uint64_t siglblid = f.m().read64(ID);
    const size_t deferred = f.m().gsDeferredVif1Bytes();
    const size_t blocked = f.m().gsBlockedGifBytes();
    const size_t held = f.m().m_heldVif1Transfers.size();
    const size_t maskedPath3 = f.m().m_path3MaskedFifo.size();
    const uint32_t directRemaining = f.m().m_gsDirectBytesRemaining;
    const uint32_t pendingImage = f.m().m_vif1PendingPath2ImageQwc;
    const auto rendered = f.rendered;
    std::array<uint8_t, 16> destination{};
    destination.fill(0x5au);
    std::vector<uint8_t> snapshot{0x11u, 0x22u, 0x33u};
    bool readRejected = false, snapshotRejected = false, restoreRejected = false;
    try { (void)f.runtime.readActiveGsBackendLocalMemory(destination.data(), destination.size(), 0, 0, 0); }
    catch (const std::runtime_error&) { readRejected = true; }
    try { (void)f.runtime.snapshotActiveGsBackendLocalMemory(snapshot); }
    catch (const std::runtime_error&) { snapshotRejected = true; }
    try { (void)f.runtime.restoreActiveGsBackendLocalMemory(nullptr, 0u); }
    catch (const std::runtime_error&) { restoreRejected = true; }
    check(readRejected && snapshotRejected && restoreRejected,
          "non-quiescent producer admitted a result operation");
    check(std::all_of(destination.begin(), destination.end(), [](uint8_t byte) { return byte == 0x5au; }),
          "rejected local-memory read mutated guest destination");
    check(snapshot == std::vector<uint8_t>({0x11u, 0x22u, 0x33u}),
          "rejected local-memory snapshot mutated caller state");
    check(f.m().read64(CSR) == csr && f.m().read64(ID) == siglblid &&
          f.m().gsDeferredVif1Bytes() == deferred && f.m().gsBlockedGifBytes() == blocked &&
          f.m().m_heldVif1Transfers.size() == held &&
          f.m().m_path3MaskedFifo.size() == maskedPath3 &&
          f.m().m_gsDirectBytesRemaining == directRemaining &&
          f.m().m_vif1PendingPath2ImageQwc == pendingImage && f.rendered == rendered,
          "rejected result operation consumed or acknowledged producer work");
}

void expectResultPermittedWithoutBackend(Fixture& f) {
    std::array<uint8_t, 16> destination{};
    destination.fill(0x5au);
    std::vector<uint8_t> snapshot{0x11u, 0x22u, 0x33u};
    bool threw = false;
    try {
        check(!f.runtime.readActiveGsBackendLocalMemory(destination.data(), destination.size(), 0, 0, 0),
              "inactive fixture unexpectedly read local memory");
        check(!f.runtime.snapshotActiveGsBackendLocalMemory(snapshot),
              "inactive fixture unexpectedly snapshotted local memory");
        check(!f.runtime.restoreActiveGsBackendLocalMemory(nullptr, 0u),
              "inactive fixture unexpectedly restored local memory");
    } catch (const std::runtime_error&) { threw = true; }
    check(!threw && std::all_of(destination.begin(), destination.end(), [](uint8_t byte) { return byte == 0x5au; }) &&
          snapshot == std::vector<uint8_t>({0x11u, 0x22u, 0x33u}),
          "quiescent result operation did not preserve inactive-backend semantics");
}

void maskedAndIrq(bool sync) {
    Fixture f(sync);
    f.m().write64(ID,0x1234567887654321ULL);
    f.m().write16(IMR,0x7f00);
    f.send(packet({{0x60,id(0xa5a5a5a5,0x00ff00ff)},{0x62,id(0xabcdef01,0xff00ff00)}}));
    check(f.m().read64(ID)==0xab34ef7887a543a5ULL,"masked SIGNAL/LABEL identifiers match source equation");
    check(f.interrupts.empty(),"IMR masked SIGNAL must not dispatch");
    check(f.m().read64(CSR)&1,"masked SIGNAL still latches CSR");
    f.m().write32(IMR,0x7e00);
    check(f.interrupts.size()==1,"unmask of pending SIGNAL routes actual guest INTC handler");
    check(f.interrupts[0].csr&1,"IRQ callback runs after CSR mutation");
    check(f.interrupts[0].siglbl==0xab34ef7887a543a5ULL,"IRQ sees completed accepted event state");
    f.m().write8(CSR,1);
    check(!(f.m().read8(CSR)&1),"8-bit CSR acknowledgement");
    if(!sync) check(f.owner==f.producer,"inline consumer owner");
    if(sync) check(f.owner!=f.producer,"worker-sync dedicated consumer owner");
}
void repeatAndDma(bool sync) {
    Fixture f(sync); f.stalled();
    auto later=packet({{0x62,id(9)}});
    f.kick(GIF_DMA,later);
    check(f.m().readIORegister(GIF_DMA)&0x100,"new stalled GIF DMA retains STR");
    check(!(f.m().readIORegister(DSTAT)&4),"new stalled GIF DMA has no D_STAT completion");
    check(f.dma.empty(),"new stalled GIF DMA has no callback");
    std::memset(f.m().getRDRAM()+0x10000,0xff,later.size());
    check(f.m().gsBlockedGifBytes()==later.size(),"blocked DMA owns bounded payload");
    expectResultRejected(f, rrv::gs::ProducerWork::BlockedGifDma,
                         "blocked GIF DMA was not classified as producer work");
    f.m().write32(CSR,1);
    check(!f.m().gsControl().stalled(),"ack releases duplicate SIGNAL");
    check(f.m().read64(ID)==0x0000000900000002ULL,"pending SIGNAL then LABEL and blocked owned DMA exact order");
    check(f.dma==std::vector<uint32_t>{2},"blocked GIF DMA completes exactly once after resume");
    check((f.m().readIORegister(DSTAT)&4) && !(f.m().readIORegister(GIF_DMA)&0x100),"resumed DMA flags");
    check(f.m().gsBlockedGifBytes()==0,"blocked DMA bytes released");
    f.m().write32(CSR,1);
    check(!(f.m().read64(CSR)&1),"second ack clears delivered duplicate");
    check(f.interrupts.size()==2,"one GS interrupt for first and pending SIGNAL");
}
void acceptedDma(bool sync) {
    Fixture f(sync);
    f.kick(GIF_DMA,packet({{0x60,id(1)},{0x60,id(2)},{0x62,id(3)}}));
    check(f.m().gsControl().stalled(),"accepted DMA stalls GIF progression");
    check(f.dma==std::vector<uint32_t>{2},"initial copied transfer DMA completion is independent of subsequent SIGNAL stall");
    check(!(f.m().readIORegister(GIF_DMA)&0x100),"accepted copied transfer clears STR");
    f.m().write32(CSR,1);
    check(f.m().read64(ID)==0x300000002ULL,"accepted DMA suffix resumes without duplication");
}
void vifDeferred(bool sync) {
    Fixture f(sync); f.stalled();
    auto input=direct(packet({{0x62,id(0x77)}}));
    put32(input,0x07001234); // MARK after DIRECT.
    input.resize((input.size()+15)&~size_t(15),0);
    f.kick(VIF_DMA,input);
    check(!f.m().m_heldVif1Transfers.empty(), "default latency did not retain VIF DMA work");
    expectResultRejected(f, rrv::gs::ProducerWork::HeldVif1,
                         "held VIF DMA was not classified as producer work");
    // This guest frame-end flush is the legitimate latency release point; it
    // does not acknowledge the stalled DIRECT that follows it.
    f.m().flushHeldVif1AtGuestFrameEnd();
    check(f.m().gsDeferredVif1Bytes()==input.size(),"VIF retains command and all following bytes");
    check(f.m().vif1_regs.mark==0,"later VIF MARK does not escape deferred DIRECT");
    check(f.dma.empty() && (f.m().readIORegister(VIF_DMA)&0x100),"deferred VIF DMA does not complete");
    check(f.m().m_gsVif1CompletionPending,
          "deferred VIF retained its completion state");
    expectResultRejected(f, rrv::gs::ProducerWork::DeferredVif1,
                         "deferred VIF was not classified as producer work");
    expectResultRejected(f, rrv::gs::ProducerWork::VifCompletion,
                         "deferred VIF completion was not classified as producer work");
    std::memset(f.m().getRDRAM()+0x10000,0xee,input.size());
    f.m().write32(CSR,1);
    check(f.m().read64(ID)==0x7700000002ULL,"VIF owned DIRECT resumed once after pending SIGNAL");
    check(f.m().vif1_regs.mark==0x1234,"exact VIF cursor resumes following MARK");
    check(f.dma==std::vector<uint32_t>{1},"VIF completion retained across retry");
    check(f.m().gsDeferredVif1Bytes()==0,"VIF owned tail released");
}
void fragmentedDirect(bool sync) {
    Fixture f(sync);
    auto input=direct(packet({{0x60,id(0x111)},{0x62,id(0x222)}}),true);
    input.resize(64,0); // 4+48 bytes then three NOPs.
    std::vector<uint8_t> first(input.begin(),input.begin()+16), second(input.begin()+16,input.end());
    f.kick(VIF_DMA,first);
    f.m().flushHeldVif1AtGuestFrameEnd();
    check(f.m().read64(ID)==0,"12-byte GIF tag fragment has no AD side effect");
    check(f.m().m_gsDirectPartial.size()==12,"DIRECT carries partial qword");
    expectResultRejected(f, rrv::gs::ProducerWork::PartialDirect,
                         "partial DIRECT was not classified as producer work");
    f.kick(VIF_DMA,second,0x20000);
    f.m().flushHeldVif1AtGuestFrameEnd();
    check(f.m().read64(ID)==0x22200000111ULL,"fragmented DIRECTHL executes both AD events exactly once");
    check(f.m().gsControl().counters().signals==1 && f.m().gsControl().counters().labels==1,"fragmented event counts");
    check(f.m().m_gsDirectPartial.empty() && f.m().m_gsDirectBytesRemaining==0,"DIRECT carry fully drained");
    check(f.dma==std::vector<uint32_t>({1,1}),"each accepted source DMA completes independently");
}
void heldVif1Latency(bool sync) {
    Fixture f(sync);
    std::vector<uint8_t> held;
    put32(held, 0x07001234u); // MARK, harmless until the guest latency flush.
    held.resize(16u, 0u);
    f.kick(VIF_DMA, held);
    check(f.m().m_heldVif1Transfers.size() == 1u && f.m().vif1_regs.mark == 0u,
          "production-default latency did not retain the VIF1 chain");
    expectResultRejected(f, rrv::gs::ProducerWork::HeldVif1,
                         "held VIF1 work admitted a result operation");
    f.m().flushHeldVif1AtGuestFrameEnd();
    check(f.m().m_heldVif1Transfers.empty() && f.m().vif1_regs.mark == 0x1234u,
          "guest latency flush did not complete held VIF1 work");
    check(f.m().gsResultBoundary().admits(rrv::gs::ResultOperation::SnapshotLocalMemory),
          "completed held VIF1 work left the result boundary closed");
    expectResultPermittedWithoutBackend(f);
}
void pendingPath2Image(bool sync) {
    Fixture f(sync);
    std::vector<uint8_t> image;
    // IMAGE NLOOP=2 with just the tag inline: both IMAGE qwords must arrive
    // in a later VIF transfer before a local-memory result may observe GS.
    put64(image, 2u | 0x8000u | (uint64_t(2u) << 58u) | (uint64_t(1u) << 60u));
    put64(image, 0u);
    auto input = direct(image);
    input.resize(32u, 0u);
    f.m().processVIF1Data(input.data(), static_cast<uint32_t>(input.size()));
    check(f.m().m_vif1PendingPath2ImageQwc == 2u,
          "truncated DIRECT IMAGE did not retain its PATH2 continuation");
    expectResultRejected(f, rrv::gs::ProducerWork::PendingPath2Image,
                         "pending PATH2 IMAGE admitted a result operation");
    std::array<uint8_t, 32> continuation{};
    f.m().processVIF1Data(continuation.data(), static_cast<uint32_t>(continuation.size()));
    check(f.m().m_vif1PendingPath2ImageQwc == 0u,
          "later PATH2 IMAGE continuation did not clear pending state");
    check(f.m().gsResultBoundary().admits(rrv::gs::ResultOperation::RestoreLocalMemory),
          "completed PATH2 IMAGE left the result boundary closed");
    expectResultPermittedWithoutBackend(f);
}
void maskedPath3(bool sync) {
    Fixture f(sync);
    std::vector<uint8_t> mask;
    put32(mask, 0x06008000u); // MSKPATH3, IMMEDIATE bit 15 set.
    f.m().processVIF1Data(mask.data(), static_cast<uint32_t>(mask.size()));
    check(f.m().isPath3Masked(), "MSKPATH3 enable did not reach producer state");

    const auto first = packet({{0x62,id(0x44)}});
    const auto second = packet({{0x62,id(0x55)}});
    std::vector<uint8_t> expected = first;
    expected.insert(expected.end(), second.begin(), second.end());
    f.send(first, GifPathId::Path3);
    f.send(second, GifPathId::Path3);
    check(f.m().m_path3MaskedFifo.size() == 2u && f.rendered.empty(),
          "masked PATH3 did not retain FIFO-owned packets");
    expectResultRejected(f, rrv::gs::ProducerWork::MaskedPath3,
                         "masked PATH3 FIFO admitted a result operation");

    std::vector<uint8_t> unmask;
    put32(unmask, 0x06000000u); // MSKPATH3 clear flushes retained PATH3 in order.
    f.m().processVIF1Data(unmask.data(), static_cast<uint32_t>(unmask.size()));
    check(!f.m().isPath3Masked() && f.m().m_path3MaskedFifo.empty(),
          "MSKPATH3 unmask did not release retained PATH3 FIFO");
    check(f.rendered == expected && f.m().read64(ID) == 0x5500000000ULL,
          "MSKPATH3 flush did not preserve PATH3 packet order");
    f.m().flushMaskedPath3Packets();
    check(f.rendered == expected && f.m().m_path3MaskedFifo.empty(),
          "MSKPATH3 retained PATH3 FIFO drained more than once");
    check(f.m().gsResultBoundary().admits(rrv::gs::ResultOperation::ReadLocalMemory),
          "flushed MSKPATH3 FIFO left result boundary closed");
    expectResultPermittedWithoutBackend(f);
}
void resultBoundaryLocksBeforeInspection(bool sync) {
    Fixture f(sync);
    f.stalled();
    std::vector<uint8_t> snapshot{0x11u};
    std::atomic<bool> snapshotDone{false}, snapshotRejected{false};
    std::thread snapshotThread;
    bool snapshotWaited = false;
    {
        PS2Runtime::GuestExecutionScope held(&f.runtime);
        snapshotThread = std::thread([&] {
            try { (void)f.runtime.snapshotActiveGsBackendLocalMemory(snapshot); }
            catch (const std::runtime_error&) { snapshotRejected.store(true, std::memory_order_release); }
            snapshotDone.store(true, std::memory_order_release);
        });
        for (unsigned spin = 0u; spin != 100000u &&
             f.runtime.guestExecutionWaiterCountForTesting() == 0u; ++spin)
            std::this_thread::yield();
        snapshotWaited = f.runtime.guestExecutionWaiterCountForTesting() != 0u &&
                         !snapshotDone.load(std::memory_order_acquire);
    }
    snapshotThread.join();
    check(snapshotWaited && snapshotRejected.load(std::memory_order_acquire) && snapshot == std::vector<uint8_t>({0x11u}),
          "locked snapshot did not reject non-quiescent producer work without mutation");

    std::atomic<bool> restoreDone{false}, restoreRejected{false};
    std::thread restoreThread;
    bool restoreWaited = false;
    {
        PS2Runtime::GuestExecutionScope held(&f.runtime);
        restoreThread = std::thread([&] {
            try { (void)f.runtime.restoreActiveGsBackendLocalMemory(nullptr, 0u); }
            catch (const std::runtime_error&) { restoreRejected.store(true, std::memory_order_release); }
            restoreDone.store(true, std::memory_order_release);
        });
        for (unsigned spin = 0u; spin != 100000u &&
             f.runtime.guestExecutionWaiterCountForTesting() == 0u; ++spin)
            std::this_thread::yield();
        restoreWaited = f.runtime.guestExecutionWaiterCountForTesting() != 0u &&
                        !restoreDone.load(std::memory_order_acquire);
    }
    restoreThread.join();
    check(restoreWaited && restoreRejected.load(std::memory_order_acquire),
          "locked restore did not reject non-quiescent producer work");
}
void finishAndReset(bool sync) {
    Fixture f(sync);
    f.m().write32(IMR,0x7f00);
    f.send(packet({{0x61,0}}));
    check((f.m().read64(CSR)&2) && !f.m().gsControl().finishPending(),"FINISH eligible at completed accepted GIF execute");
    check(f.interrupts.empty(),"masked FINISH no IRQ");
    f.m().write32(IMR,0x7d00);
    check(f.interrupts.size()==1,"pending FINISH unmask sends guest IRQ");
    f.m().write128(CSR,_mm_set_epi64x(-1,2));
    check(!(f.m().read64(CSR)&2),"128-bit FINISH acknowledgement uses low control word once");
    check(f.m().gsControl().counters().finishAcks==1,"128-bit write not split into duplicate acknowledgements");
    f.stalled();
    f.m().gs().csr|=0x2000;
    f.m().write16(CSR,0x200);
    check(f.resets==1 && !f.m().gsControl().stalled(),"CSR reset clears queued event and resets renderer once");
    check(f.m().read64(CSR)==0x551b6000 && f.m().read64(IMR)==0x7f00,"CSR reset constants with existing FIELD owner preserved");
    check(f.runtime.gifArbiter().controlPending(),"CSR RESET preserves pending GIF suffix");
    f.runtime.gifArbiter().drain();
    check(f.m().read64(ID)==0x300000000ULL,"reset resumes after consumed duplicate without redelivering its ID");
}
void widthsAndUnrelated(bool sync) {
    Fixture f(sync);
    f.m().gs().csr=0xaabbccdd551b6003ULL;
    f.m().write8(CSR,1);
    check(f.m().read64(CSR)==0xaabbccdd551b6002ULL,"CSR ack preserves revision FIELD and upper fields");
    f.m().write16(CSR,2);
    f.m().write64(ID,0x1122334455667788ULL);
    check(f.m().read8(ID+1)==0x77 && f.m().read16(ID+2)==0x5566,"narrow GS bank reads");
    check(f.m().read64(ID|0xa0000000)==0x1122334455667788ULL,"uncached GS read uses authoritative bank");
    f.m().write32(0x1000,0x12345678);
    check(f.m().read32(0x1000)==0x12345678,"ordinary RAM at GS offset is not a control write");
    check(f.m().read64(CSR)==0xaabbccdd551b6000ULL,"RAM offset cannot affect CSR");
    bool threw=false; try { f.m().write16(CSR+1,1); } catch(const std::runtime_error&) { threw=true; }
    check(threw,"existing alignment exception preserved");
    threw=false; try { f.m().initialize(); } catch(const std::logic_error&) { threw=true; }
    check(threw && f.m().read64(ID)==0x1122334455667788ULL,"enabled lifetime reinitialization rejected before memory cleanup");
}
void flushGate(bool sync) {
    Fixture f(sync);
    f.send(packet({{0x60,id(1)},{0x60,id(2)},{0x62,id(3)}}),GifPathId::Path1);
    expectResultRejected(f, rrv::gs::ProducerWork::RendererVisibleGif,
                         "retained GIF suffix was not classified as renderer-visible producer work");
    std::vector<uint8_t> commands; put32(commands,0x11000000); put32(commands,0x0700abcd);
    f.m().processVIF1Data(commands.data(),uint32_t(commands.size()));
    check(f.m().gsDeferredVif1Bytes()==8 && f.m().vif1_regs.mark==0,"FLUSH retains exact VIF command cursor while PATH1 pending");
    f.m().write32(CSR,1);
    check(f.m().vif1_regs.mark==0xabcd && f.m().gsDeferredVif1Bytes()==0,"FLUSH resumes after acknowledged PATH1 drain");
}
void intcWidths(bool sync) {
    Fixture f(sync);
    f.m().writeIORegister(0x1000f010,1); // toggle GS INTC off.
    check(!(f.m().readIORegister(0x1000f010)&1),"actual INTC mask toggles GS cause");
    f.send(packet({{0x60,id(0x44)}}));
    check(f.interrupts.empty() && (f.m().readIORegister(0x1000f000)&1),"masked INTC retains GS request");
    f.m().write8(0x1000f001,0xff);
    f.m().write16(0x1000f002,0xffff);
    check(f.m().readIORegister(0x1000f000)&1,"narrow upper-byte/halfword W1C cannot clear GS INTC bit");
    f.m().write8(0x1000f011,1);
    f.m().write16(0x1000f012,1);
    check(!(f.m().readIORegister(0x1000f010)&1),"narrow upper-byte/halfword mask writes cannot toggle GS bit");
    R5900Context ctx{};
    ctx.r[4]=_mm_setzero_si128();
    ps2_syscalls::EnableIntc(f.m().getRDRAM(),&ctx,&f.runtime);
    check(f.interrupts.size()==1,"HLE EnableIntc retries latched request through actual guest handler");
    check(f.m().readIORegister(0x1000f010)&1,"HLE enable shares authoritative MMIO GS mask");
    check(!(f.m().readIORegister(0x1000f000)&1),"guest IRQ handler acknowledges INTC_STAT");
}
void hleImr(bool sync) {
    // System.cpp GsPutIMR/iGsPutIMR must reach the same IMR transition as MMIO.
    // GS.cpp IMRwrite uses low32 mask bits, preserves upper32, forces bits13/14.
    for (auto put : {ps2_syscalls::GsPutIMR, ps2_syscalls::iGsPutIMR}) {
        Fixture f(sync);
        f.m().write16(IMR,0x7f00);
        f.m().write32(IMR+4,0x12345678);
        f.send(packet({{0x60,id(0x44)}}));
        check(f.interrupts.empty(),"HLE IMR setup has masked pending SIGNAL");
        const auto writesBefore=f.m().gsControl().counters().imrWrites;
        R5900Context ctx{};
        ctx.r[4]=_mm_set_epi64x(0,0xdeadbe00);
        ctx.r[5]=_mm_set_epi64x(0,0xfedcba98);
        put(f.m().getRDRAM(),&ctx,&f.runtime);
        check(static_cast<uint64_t>(_mm_extract_epi64(ctx.r[2],0))==0x1234567800007f00ULL,
              "GsPutIMR/iGsPutIMR returns full previous IMR in v0");
        check(static_cast<uint32_t>(_mm_extract_epi32(ctx.r[3],0))==0x12345678,
              "GsPutIMR/iGsPutIMR preserves existing v1 high32 return convention");
        check(f.m().read64(IMR)==0x1234567800007e00ULL,
              "GsPutIMR/iGsPutIMR uses reference writable mask and forced bits");
        check(f.m().gsControl().counters().imrWrites==writesBefore+1,
              "HLE IMR write reaches authoritative control exactly once");
        check(f.interrupts.size()==1 && f.interrupts[0].siglbl==0x44,
              "HLE SIGNAL unmask dispatches actual guest GS IRQ");
    }
}
std::vector<uint8_t> equivalent(bool sync) {
    Fixture f(sync);
    f.send(packet({{0x60,id(7)},{0x60,id(8)},{0x62,id(9)}}));
    f.m().write32(CSR,1); f.m().write32(CSR,1);
    auto input=direct(packet({{0x61,0},{0x62,id(10)}}));
    f.m().processVIF1Data(input.data(),uint32_t(input.size()));
    check(f.m().read64(ID)==0xa00000008ULL,"equivalent sequence guest registers");
    check(f.interrupts.size()==3,"equivalent sequence guest IRQ count");
    return f.rendered;
}
} // namespace
int main() {
    // Exercise the production-default latency path. The test never enables a
    // private timing mode; held VIF1 work is part of result admission.
    unsetenv("RRV_GIF_PATH_LATENCY");
    unsetenv("RRV_GIF_PATH3_INTERLEAVE");
    int failures=0, passed=0;
    for(bool sync:{false,true}) {
        for(auto test : {maskedAndIrq,repeatAndDma,acceptedDma,vifDeferred,fragmentedDirect,heldVif1Latency,pendingPath2Image,maskedPath3,resultBoundaryLocksBeforeInspection,finishAndReset,widthsAndUnrelated,flushGate,intcWidths,hleImr}) {
            try { test(sync); ++passed; }
            catch(const std::exception& error) { ++failures; std::cerr<<(sync?"worker-sync":"inline")<<" FAIL "<<error.what()<<'\n'; }
        }
    }
    try { check(equivalent(false)==equivalent(true),"equivalent inline and worker-sync renderer command bytes"); ++passed; }
    catch(const std::exception& error) { ++failures; std::cerr<<"equivalence FAIL "<<error.what()<<'\n'; }
    std::cout<<"GS product integration: "<<passed<<" passed, "<<failures<<" failed\n";
    return failures?1:0;
}
