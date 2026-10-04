// dev_hw.cpp — sceDevGif / sceDevVif / sceDevVu / sceVu0Mem HLE stubs.
//
// These are low-level hardware FIFO/register ops that bypass libdma.
// Phase 2 strategy:
//   - sceDevGifPutFifo  → submit to runtime's GIF arbiter (Path3)
//   - sceDevVif1PutFifo → route to VIF1 interpreter (geometry path)
//   - Sync/GetCnd       → return 0 (synchronous HLE, always done)
//   - Everything else   → silent no-op (return 0, no throw)

#include "rrv_hle.h"
#include "ps2_runtime.h"
#include "runtime/ps2_gif_arbiter.h"
#include "runtime/ps2_memory.h"
#include "rrv_m2_causal_trace.h"
#include "rrv_m2_dma_provenance.h"
#include "rrv_m2_guest_provenance.h"
#include <cstring>
#include <cstdio>

namespace rrv_hle {

// ── GIF ──────────────────────────────────────────────────────────────────

void devGifPutFifo_stub(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    // sceDevGifPutFifo(void* data, int qwc)
    // data pointer in $a0, quadword count in $a1.
    const uint32_t dataAddr = getRegU32(ctx, 4);
    const uint32_t qwc      = getRegU32(ctx, 5);

    if (runtime && dataAddr && qwc) {
        const uint8_t* src = rdram + (dataAddr & PS2_RAM_MASK);
        runtime->gifArbiter().submit(GifPathId::Path3, src, qwc * 16u);
    }
    setReturnS32(ctx, 0);
}

void devGifSync_stub(uint8_t*, R5900Context* ctx, PS2Runtime* runtime) {
    // sceDevGifSync() — wait until GIF is idle; synchronous in our HLE.
    if (runtime) runtime->gifArbiter().drain();
    setReturnS32(ctx, 0);
}
void devGifGetCnd_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    // Returns GIF condition (0 = ready).
    setReturnS32(ctx, 0);
}
void devGifContinue_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    setReturnS32(ctx, 0);
}
void devGifPutImtMode_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    setReturnS32(ctx, 0);
}
void devGifPutP3msk_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    setReturnS32(ctx, 0);
}

// ── VIF0 (EE→VU0 micro-mode DMA) ─────────────────────────────────────────
// VIF0 is used rarely on RRV (VU0 macro-mode is decoded inline).
// No-op for Phase 2; will revisit if needed.

void devVif0PutFifo_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) { setReturnS32(ctx, 0); }
void devVif0Sync_stub   (uint8_t*, R5900Context* ctx, PS2Runtime*) { setReturnS32(ctx, 0); }
void devVif0GetCnd_stub (uint8_t*, R5900Context* ctx, PS2Runtime*) { setReturnS32(ctx, 0); }
void devVif0Continue_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) { setReturnS32(ctx, 0); }
void devVif0Pause_stub  (uint8_t*, R5900Context* ctx, PS2Runtime*) { setReturnS32(ctx, 0); }
void devVif0PutErr_stub (uint8_t*, R5900Context* ctx, PS2Runtime*) { setReturnS32(ctx, 0); }

// ── VIF1 (EE→VU1 geometry DMA) ───────────────────────────────────────────
// sceDevVif1PutFifo: sends a VIF packet to VIF1 FIFO.
// On real hardware this goes to VU1 which processes it and kicks GIF.
// For Phase 2 we forward raw data through the runtime's VIF1 interpreter.

void devVif1PutFifo_stub(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime) {
    // sceDevVif1PutFifo(void* data, int qwc)
    const uint32_t dataAddr = getRegU32(ctx, 4);
    const uint32_t qwc      = getRegU32(ctx, 5);

    if (runtime && dataAddr && qwc) {
        // Hand the whole VIF packet to the runtime's VIF1 interpreter, which
        // handles MPG (microcode upload), UNPACK (VU1 data writes), DIRECT
        // (GIF Path2) and MSCAL/MSCNT (VU1 kick → XGKICK → GS).  This mirrors
        // exactly what submitDmaSend does for the VIF1 DMA channel.
        //
        // NOTE: the previous implementation wrote one 32-bit word per quadword
        // to the VIF1 FIFO IO register (0x10005000).  That dropped 12 of every
        // 16 bytes AND never triggered processVIF1Data — writing the FIFO
        // register does not run the interpreter — so VU1 never kicked and no
        // geometry reached the GS.
        PS2Memory& mem = runtime->memory();
        const uint8_t* src = rdram + (dataAddr & PS2_RAM_MASK);
        rrv::m2causal::Scope causalFifo(rrv::m2causal::EventType::Vif1FifoEnter,
                                       rrv::m2causal::EventType::Vif1FifoExit,
                                       rrv::m2causal::Source::Vif1,
                                       dataAddr, qwc, ctx->pc);
        const uint64_t m2Origin = rrv::m2prov::createFifoOrigin(ctx->pc, getRegU32(ctx, 31), 3u);
        {
            rrv::m2prov::ProvenanceScope m2Work(m2Origin);
            rrv::m2prov::observeInput(m2Origin, dataAddr & PS2_RAM_MASK, src, qwc * 16u);
            mem.processVIF1Data(src, qwc * 16u);
        }
        rrv::m2prov::release(m2Origin);
    }
    setReturnS32(ctx, 0);
}

void devVif1Sync_stub   (uint8_t*, R5900Context* ctx, PS2Runtime* r) {
    if (r) r->gifArbiter().drain();
    setReturnS32(ctx, 0);
}
void devVif1GetCnd_stub (uint8_t*, R5900Context* ctx, PS2Runtime*) { setReturnS32(ctx, 0); }
void devVif1GetFifo_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) { setReturnS32(ctx, 0); }
void devVif1Continue_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) { setReturnS32(ctx, 0); }
void devVif1Pause_stub  (uint8_t*, R5900Context* ctx, PS2Runtime*) { setReturnS32(ctx, 0); }
void devVif1PutErr_stub (uint8_t*, R5900Context* ctx, PS2Runtime*) { setReturnS32(ctx, 0); }
void devVif1Reset_stub  (uint8_t*, R5900Context* ctx, PS2Runtime*) { setReturnS32(ctx, 0); }

// ── VU0/VU1 condition/sync ops ────────────────────────────────────────────
// In HLE, VU execution is synchronous; these always report done.

void devVu0Sync_stub   (uint8_t*, R5900Context* ctx, PS2Runtime*) { setReturnS32(ctx, 0); }
void devVu0GetCnd_stub (uint8_t*, R5900Context* ctx, PS2Runtime*) { setReturnS32(ctx, 0); }
void devVu0Pause_stub  (uint8_t*, R5900Context* ctx, PS2Runtime*) { setReturnS32(ctx, 0); }
void devVu0PutCnd_stub (uint8_t*, R5900Context* ctx, PS2Runtime*) { setReturnS32(ctx, 0); }
void devVu0PutDBit_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) { setReturnS32(ctx, 0); }
void devVu0PutTBit_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) { setReturnS32(ctx, 0); }

void devVu1Sync_stub   (uint8_t*, R5900Context* ctx, PS2Runtime*) { setReturnS32(ctx, 0); }
void devVu1GetCnd_stub (uint8_t*, R5900Context* ctx, PS2Runtime*) { setReturnS32(ctx, 0); }
void devVu1GetTpc_stub (uint8_t*, R5900Context* ctx, PS2Runtime*) { setReturnU32(ctx, 0); }
void devVu1Pause_stub  (uint8_t*, R5900Context* ctx, PS2Runtime*) { setReturnS32(ctx, 0); }
void devVu1PutCnd_stub (uint8_t*, R5900Context* ctx, PS2Runtime*) { setReturnS32(ctx, 0); }
void devVu1PutDBit_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) { setReturnS32(ctx, 0); }
void devVu1PutTBit_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) { setReturnS32(ctx, 0); }

// ── sceVu0Mem / sceVuCheckBusy ───────────────────────────────────────────
// sceVu0MemReadQ / sceVu0MemWriteQ: read/write 128-bit VU0 data memory.
// sceVuCheckBusy: returns 0 when VU is idle.

void vu0MemReadQ_stub(uint8_t* rdram, R5900Context* ctx, PS2Runtime*) {
    // sceVu0MemReadQ(void* dest, const void* src, int qwc)
    // dest=$a0, src=$a1, qwc=$a2
    const uint32_t dest = getRegU32(ctx, 4);
    const uint32_t src  = getRegU32(ctx, 5);
    const uint32_t qwc  = getRegU32(ctx, 6);
    if (rdram && dest && src && qwc) {
        const uint32_t bytes = qwc * 16u;
        uint8_t* d = rdram + (dest & PS2_RAM_MASK);
        const uint8_t* s = rdram + (src  & PS2_RAM_MASK);
        rrv::m2guest::WriteScope rrvGuestWrite(rdram, ctx->pc, dest, dest & PS2_RAM_MASK, bytes, 3u);
        std::memcpy(d, s, bytes);
        rrvGuestWrite.finish();
    }
    setReturnS32(ctx, 0);
}

void vu0MemWriteQ_stub(uint8_t* rdram, R5900Context* ctx, PS2Runtime*) {
    // sceVu0MemWriteQ(void* dest, const void* src, int qwc)
    const uint32_t dest = getRegU32(ctx, 4);
    const uint32_t src  = getRegU32(ctx, 5);
    const uint32_t qwc  = getRegU32(ctx, 6);
    if (rdram && dest && src && qwc) {
        const uint32_t bytes = qwc * 16u;
        uint8_t* d = rdram + (dest & PS2_RAM_MASK);
        const uint8_t* s = rdram + (src  & PS2_RAM_MASK);
        rrv::m2guest::WriteScope rrvGuestWrite(rdram, ctx->pc, dest, dest & PS2_RAM_MASK, bytes, 3u);
        std::memcpy(d, s, bytes);
        rrvGuestWrite.finish();
    }
    setReturnS32(ctx, 0);
}

void vuCheckBusy_stub(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    setReturnS32(ctx, 0); // always ready
}

// ── Shared ret0 ───────────────────────────────────────────────────────────
void ret0(uint8_t*, R5900Context* ctx, PS2Runtime*) {
    setReturnS32(ctx, 0);
}

} // namespace rrv_hle
