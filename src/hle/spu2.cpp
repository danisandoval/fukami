// spu2.cpp — Minimal host HLE for sceSpu2Remote (RSPU2DRV register RPC).
//
// Why this exists:
//   The real sceSpu2Remote (game addr 0x2C5AC0) marshals a vararg command
//   list and dispatches it to the SPU2 IOP driver over SIF RPC (client
//   0x316C68, sid 0x80000601), returning the register value the driver writes
//   back.  Our runtime acknowledges the SIF RPC but does not run the IOP SPU2
//   driver, so the readback is stale and the game's boot-time SPU2 verify
//   (sub_295490) never passes — the EE main thread busy-waits forever in
//   sub_29C1D8 at 0x29c27c before ANY geometry is submitted.
//
//   We override 0x2C5AC0 with this host function (registered in patches.cpp).
//   It models just enough of the register protocol to satisfy the boot verify:
//   a small register file with echo-on-write semantics.  Sound stays silent
//   (real SPU2/audio is Phase 4); this only unblocks boot so the renderer can
//   be exercised.
//
// Calling convention (decoded from the recompiled body 0x2C5AC0):
//   sceSpu2Remote(int a0, u16 cmd /*a1*/, u32 data /*a2*/, u32 a3, ...)
//   The first vararg ($a1) is the command/register word; $a2 is the data.
//   The function returns the (post-op) register value; callers compare its
//   low 16 bits against the value they expect to read back.

#include "rrv_hle.h"
#include "ps2_runtime.h"
#include "runtime/ps2_memory.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>

namespace rrv_hle {

void spu2Remote_hle(uint8_t* rdram, R5900Context* ctx, PS2Runtime* /*runtime*/) {
    const uint32_t a0 = getRegU32(ctx, 4);
    const uint32_t cmd = getRegU32(ctx, 5);   // $a1 — command / register word
    const uint32_t data = getRegU32(ctx, 6);  // $a2 — data payload
    const uint32_t a3 = getRegU32(ctx, 7);

    // Per-command register file: a write stores the value, so a subsequent
    // read-back of the same command returns what was written (the pattern the
    // boot verify relies on).
    static std::unordered_map<uint32_t, uint32_t> regFile;

    // Echo-on-write: remember the data for this command and return it, so the
    // game's "write X then expect to read X" verify succeeds.
    regFile[cmd] = data;
    uint32_t ret = data;

    // Command 0x4065 is the sound-DMA "transfer status" poll (in sub_295490):
    //   while (sceSpu2Remote(0x4065) != [gp-0x6320]) { ... }
    // We model SPU2 voice-DMA as instantaneous, so the status must already
    // equal the phase the game expects ([gp-0x6320]).  Return that value so
    // the wait completes immediately.
    uint32_t s0probe = 0;
    if ((cmd & 0xFFFFu) == 0x4065u) {
        const uint32_t gp = getRegU32(ctx, 28);
        const uint32_t addr = (gp - 0x6320u) & PS2_RAM_MASK;
        std::memcpy(&s0probe, rdram + addr, sizeof(s0probe));
        ret = s0probe;
    }

    // Log the first handful of calls (the boot SPU2 init handshake), then go
    // quiet so the per-frame sound streaming doesn't spam the console.
    static int s_log = 0;
    if (s_log < 24) {
        std::fprintf(stderr, "[rrv:spu2Remote] a0=%u cmd=0x%04x data=0x%08x a3=0x%08x -> 0x%08x\n",
                     a0, cmd & 0xFFFFu, data, a3, ret);
        ++s_log;
    }

    setReturnS32(ctx, static_cast<int32_t>(ret));

    // Emulate `jr $ra`.  Required because sceSpu2Remote is also reached via
    // indirect (function-pointer) calls, where dispatchLoop dispatches this
    // address directly and re-reads ctx->pc afterwards — if we leave pc at the
    // entry it re-dispatches forever ("PC not updating at 0x2c5ac0").  Setting
    // pc=$ra also satisfies the inline-call fast path (ra == return address).
    ctx->pc = getRegU32(ctx, 31);
}

} // namespace rrv_hle
