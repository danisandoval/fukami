// SPDX-License-Identifier: GPL-3.0+
// Tests for the host RSPU2DRV re-implementation (src/audio/rspu2_driver.cpp)
// on the PCSX2 SPU2 core. All inputs are synthetic (no game data).
//
//   rspu2_driver_test unit | stream | determinism | cost | all
//
// Exit code 0 = pass.
#include "rspu2_driver.h"

#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace rrv::rspu2;

namespace
{
int g_fail = 0;
#define CHECK(cond, ...)                                                   \
    do                                                                     \
    {                                                                      \
        if (!(cond))                                                       \
        {                                                                  \
            g_fail++;                                                      \
            std::printf("  FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond); \
            std::printf(__VA_ARGS__);                                      \
            std::printf("\n");                                             \
        }                                                                  \
    } while (0)

constexpr uint32_t kBase = 0x01A00000u;
constexpr uint32_t kWin = 0x00500000u;
constexpr uint32_t kBlock = kBase + kWin - 0x1000u;
constexpr uint64_t kFrame = kIopHz / 60; // 614400

Config makeCfg(bool threaded)
{
    Config c;
    c.iop_window_base = kBase;
    c.iop_window_size = kWin;
    c.driver_block = kBlock;
    c.threaded = threaded;
    c.output_ring_frames = 1 << 15;
    return c;
}

// Voice attribute struct (spec 5), 64 bytes.
struct VA
{
    uint32_t voice = 0, mask = 0;
    uint16_t volL = 0, volR = 0, volmodeL = 0, volmodeR = 0, pitch = 0, note = 0, sn = 0;
    uint32_t addr = 0, loop = 0, amode = 0, smode = 0, rmode = 0;
    uint16_t ar = 0, dr = 0, sr = 0, rr = 0, sl = 0, adsr1 = 0, adsr2 = 0;
};
void put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
void put32(uint8_t* p, uint32_t v) { put16(p, uint16_t(v)); put16(p + 2, uint16_t(v >> 16)); }
std::vector<uint32_t> pack(const VA& a)
{
    uint8_t s[64] = {};
    put32(s + 0, a.voice); put32(s + 4, a.mask);
    put16(s + 8, a.volL); put16(s + 0xA, a.volR); put16(s + 0xC, a.volmodeL); put16(s + 0xE, a.volmodeR);
    put16(s + 0x14, a.pitch); put16(s + 0x16, a.note); put16(s + 0x18, a.sn);
    put32(s + 0x1C, a.addr); put32(s + 0x20, a.loop);
    put32(s + 0x24, a.amode); put32(s + 0x28, a.smode); put32(s + 0x2C, a.rmode);
    put16(s + 0x30, a.ar); put16(s + 0x32, a.dr); put16(s + 0x34, a.sr); put16(s + 0x36, a.rr); put16(s + 0x38, a.sl);
    put16(s + 0x3A, a.adsr1); put16(s + 0x3C, a.adsr2);
    std::vector<uint32_t> w(16);
    std::memcpy(w.data(), s, 64);
    return w;
}

struct Rig
{
    std::unique_ptr<Driver> d;
    uint64_t t = 0;
    std::vector<AsyncReply> replies;
    std::vector<Callback> cbs;
    std::vector<IopWrite> writes;
    std::vector<Callback> allCbs;     // everything sync() returned, for determinism checks
    std::vector<IopWrite> allWrites;

    explicit Rig(bool threaded) : d(std::make_unique<Driver>(makeCfg(threaded))) {}

    uint32_t rpcW(uint32_t fno, std::vector<uint32_t> args, uint64_t* end = nullptr)
    {
        std::vector<uint32_t> buf;
        buf.push_back(fno);
        buf.insert(buf.end(), args.begin(), args.end());
        uint32_t r = 0;
        const uint64_t e = d->rpc(t, fno, buf.data(), uint32_t(buf.size() * 4), &r, 4);
        if (end)
            *end = e;
        if (e > t)
            t = e;
        return r;
    }
    uint32_t rpcStruct(uint32_t fno, const std::vector<uint32_t>& s)
    {
        uint32_t r = 0;
        const uint64_t e = d->rpc(t, fno, s.data(), uint32_t(s.size() * 4), &r, 4);
        if (e > t)
            t = e;
        return r;
    }
    std::vector<uint8_t> batch(std::vector<uint32_t> words, uint64_t* end = nullptr)
    {
        std::vector<uint8_t> r(0x40);
        const uint64_t e = d->rpc(t, 0xFFFE, words.data(), uint32_t(words.size() * 4), r.data(), 0x40);
        if (end)
            *end = e;
        if (e > t)
            t = e;
        return r;
    }
    std::vector<uint8_t> keyStatus()
    {
        std::vector<uint8_t> r(24);
        uint32_t w = 0x6418;
        const uint64_t e = d->rpc(t, 0x6418, &w, 4, r.data(), 24);
        if (e > t)
            t = e;
        return r;
    }
    void write(uint32_t addr, const void* p, uint32_t n) { d->iopWrite(t, addr, p, n); }
    void step(uint64_t dt)
    {
        t += dt;
        d->advance(t);
        d->sync(t, replies, cbs, writes);
    }
};

// PS-ADPCM filter 0, shift 0 sine; flags 0 (the driver patches them).
std::vector<uint8_t> adpcmSine(int blocks, int period, int& phase, int amp = 7)
{
    std::vector<uint8_t> out(size_t(blocks) * 16, 0);
    for (int b = 0; b < blocks; b++)
    {
        uint8_t* blk = &out[size_t(b) * 16];
        for (int i = 0; i < 28; i++, phase++)
        {
            int q = int(std::lround(std::sin(2.0 * M_PI * phase / period) * amp));
            q = q < -8 ? -8 : (q > 7 ? 7 : q);
            blk[2 + i / 2] |= uint8_t((uint8_t(q) & 0xF) << ((i & 1) ? 4 : 0));
        }
    }
    return out;
}

double rms(const std::vector<float>& s)
{
    double a = 0;
    for (float x : s)
        a += double(x) * x;
    return s.empty() ? 0 : std::sqrt(a / double(s.size()));
}

// ===========================================================================
// 1. Unit checks
// ===========================================================================
void testUnitBasics()
{
    std::printf("[unit] basics\n");
    Rig r(false);
    CHECK(r.d->ok(), "driver");
    // Stale / previous-value returns.
    CHECK(r.rpcW(0x0002, {1}) == 0, "SetCore prev");
    CHECK(r.rpcW(0x0002, {0}) == 1, "SetCore prev 1");
    const uint64_t t0 = r.t;
    uint64_t end = 0;
    CHECK(r.rpcW(0x4008, {}, &end) == 1, "SsInit must leave the stale word (1)");
    CHECK(end > t0, "SsInit busy-waits on the SPU2 timeline (end %" PRIu64 ")", end);
    std::printf("  SsInit took %" PRIu64 " IOP cycles\n", end - t0);
    {
        struct { uint32_t reg; uint16_t want; const char* what; } regs[] = {
            {0x19A, 0xC000, "ATTR core0"}, {0x59A, 0xC001, "ATTR core1"}, {0x33C, 0xE, "EEA core0"},
            {0x73C, 0xF, "EEA core1"}, {0x588, 0xFFFF, "VMIXL core1"}, {0x58A, 0xFF, "VMIXL hi core1"},
            {0x198, 0xFFF, "MMIX core0"}, {0x760, 0x3FFF, "MVOLL core0 (SsInit)"}, {0x788, 0, "MVOLL core1"},
            {0x790, 0x7FFF, "0x790"}, {0x04, 0x1000, "pitch c0v0 (SsInit)"}, {0x404, 0x3FFF, "pitch c1v0 (SpuInit)"},
            {0x06, 0x80FF, "ADSR1 c0v0"}, {0x08, 0x4000, "ADSR2 c0v0"}, {0x1C0 + 12 * 7 + 2, 0x2800, "SSA lo c0v7"},
            {0x5C0 + 12 * 7 + 2, 0x2800, "SSA lo c1v7"}, {0x18C, 0, "VMIXEL core0 (SsInit flush)"},
            {0x2E0, 0xE, "ESA hi core0"}, {0x2E2, 0xFFF8, "ESA lo core0 (EEA 0x1DFFFF - 14)"}};
        for (const auto& x : regs)
        {
            const uint16_t got = r.d->debugReadSpu2(r.t, x.reg);
            CHECK(got == x.want, "%s: 0x%04x want 0x%04x", x.what, got, x.want);
        }
    }
    CHECK(r.rpcW(0x0203, {1}) == 0, "StSetCore prev 0");
    CHECK(r.rpcW(0x0203, {0}) == 1, "StSetCore prev 1");
    CHECK(r.rpcW(0x5555, {7, 7}) == 1, "unknown command keeps the stale word");
    CHECK(r.d->stats().unknown_commands == 1, "unknown counted");
    CHECK(r.rpcW(0x0200, {0}) == kBlock + kMirrorOffset, "SpuStInit returns the mirror address");
    CHECK(r.rpcW(0x0024, {}) == 3, "StGetStatus IDLE");
    CHECK(r.rpcW(0x0201, {}) == 1, "StQuit");
    CHECK(r.rpcW(0x0201, {}) == 0xFFFFFFFDu, "StQuit again -3");
    CHECK(r.rpcW(0x0024, {}) == 0, "StGetStatus 0");
    // SsSetMVol writes MVOL = (x*0x81)&0x7FFF.
    CHECK(r.rpcW(0x404A, {64, 127}) == 0, "SsSetMVol stale");
    CHECK(r.d->debugReadSpu2(r.t, 0x762) == 0x3FFF, "MVOLR 0x%04x", r.d->debugReadSpu2(r.t, 0x762));
    {
        // SpuSetCommonAttr: MVOLL with mode 1 (clamped to 0x7F | 0x8000), MVOLR raw,
        // ATTR bits from cdReverb/cdMix/extReverb/extMix.
        uint8_t st[40] = {};
        put32(st, 0x1 | 0x4 | 0x2 | 0x100 | 0x200 | 0x1000 | 0x2000 | 0x40);
        put16(st + 4, 0x200);
        put16(st + 8, 1);
        put16(st + 6, 0x1234);
        put32(st + 0x14, 1);
        put32(st + 0x18, 0);
        put32(st + 0x20, 1);
        put32(st + 0x24, 1);
        put16(st + 0x10, 0x4321);
        std::vector<uint32_t> w(10);
        std::memcpy(w.data(), st, 40);
        r.rpcStruct(0x7128, w);
        CHECK(r.d->debugReadSpu2(r.t, 0x790) == 0x4321, "cdVolL -> 0x790");
        CHECK((r.d->debugReadSpu2(r.t, 0x19A) & 0xF) == (4 | 8 | 2), "ATTR bits 0x%x", r.d->debugReadSpu2(r.t, 0x19A));
        std::printf("  MVOL after CommonAttr: L=0x%04x (want 0x807F) R=0x%04x (want 0x1234)\n",
                    r.d->debugReadSpu2(r.t, 0x760), r.d->debugReadSpu2(r.t, 0x762));
        CHECK(r.d->debugReadSpu2(r.t, 0x762) == 0x1234, "MVOLR raw");
        CHECK(r.d->debugReadSpu2(r.t, 0x760) == 0x807F, "MVOLL mode 1 clamped");
    }

    // SpuMalloc (spec 8).
    std::printf("[unit] SpuMalloc\n");
    const uint32_t a = r.rpcW(0x0101, {0x1000});
    const uint32_t b = r.rpcW(0x0101, {0x801});
    const uint32_t c = r.rpcW(0x0101, {0x100});
    CHECK(a == 0x5010 && b == 0x6010 && c == 0x6810, "a=%x b=%x c=%x", a, b, c);
    CHECK(r.rpcW(0x1011, {b}) == c, "SpuFree returns stale");
    const uint32_t d1 = r.rpcW(0x0101, {0x400});
    const uint32_t e1 = r.rpcW(0x0101, {0x400});
    const uint32_t f1 = r.rpcW(0x0101, {0x7FFFFF});
    const uint32_t g1 = r.rpcW(0x0101, {0x100});
    CHECK(d1 == 0x6010 && e1 == 0x6410 && f1 == 0xFFFFFFFFu && g1 == 0x6910, "d=%x e=%x f=%x g=%x", d1, e1, f1, g1);

    // Reverb commands.
    std::printf("[unit] reverb\n");
    CHECK(r.rpcW(0x0006, {1}) == 1, "SetReverb on");
    CHECK(r.rpcW(0x0006, {5}) == 1, "SetReverb other value returns flag");
    CHECK(r.rpcW(0x0006, {0}) == 0, "SetReverb off");
    CHECK(r.rpcW(0x000B, {1, 0x5}) == 0x5, "RevVoice on");
    CHECK(r.rpcW(0x000B, {0, 0x1}) == 0x4, "RevVoice off");
    CHECK(r.rpcW(0x000B, {8, 0x30}) == 0x30, "RevVoice set");
    CHECK(r.rpcW(0x000B, {2, 0xFF}) == 0x30, "RevVoice other: read mask");
    CHECK(r.d->debugReadSpu2(r.t, 0x18C) == 0x30, "VMIXEL lo 0x%x", r.d->debugReadSpu2(r.t, 0x18C));
    {
        const uint64_t t1 = r.t;
        uint32_t s[5] = {1, 1 | 0x100, 0, 0, 0}; // mode ROOM + clear work area
        uint32_t rep = 0;
        const uint64_t e = r.d->rpc(r.t, 0x7314, s, sizeof(s), &rep, 4);
        r.t = e;
        CHECK(e > t1 + 10000, "work-area clear busy-waits (%" PRIu64 " cycles)", e - t1);
        std::printf("  ROOM work-area clear took %" PRIu64 " IOP cycles\n", e - t1);
        const uint32_t off = 0x1DFFFFu - (0x4D8u * 8 - 2); // EEA 0xE
        const uint16_t hi = r.d->debugReadSpu2(r.t, 0x2E0), lo = r.d->debugReadSpu2(r.t, 0x2E2);
        CHECK(hi == (off >> 17) && lo == ((off >> 1) & 0xFFFF), "ESA %04x:%04x want %04x:%04x", hi, lo,
              off >> 17, (off >> 1) & 0xFFFF);
        uint32_t bad[5] = {1, 12, 0, 0, 0};
        r.d->rpc(r.t, 0x7314, bad, sizeof(bad), &rep, 4);
        CHECK(rep == 0x30, "0x7314 is stale even when it rejects the mode (%x)", rep);
    }

    // Note to pitch through SpuSetVoiceAttr.
    std::printf("[unit] voice attr / note2pitch\n");
    {
        VA v;
        v.voice = 1u << 5;
        v.mask = 0x40 | 0x20;
        v.sn = 0x3C00;
        v.note = 0x3C00;
        r.rpcStruct(0x7240, pack(v));
        CHECK(r.d->debugReadSpu2(r.t, 0x10 * 5 + 4) == 0x1000, "pitch same note 0x%x", r.d->debugReadSpu2(r.t, 0x54));
        v.mask = 0x20;
        v.note = 0x4800;
        r.rpcStruct(0x7240, pack(v));
        CHECK(r.d->debugReadSpu2(r.t, 0x54) == 0x2000, "pitch +12 0x%x", r.d->debugReadSpu2(r.t, 0x54));
        v.note = 0x3000;
        r.rpcStruct(0x7240, pack(v));
        CHECK(r.d->debugReadSpu2(r.t, 0x54) == 0x0800, "pitch -12 0x%x", r.d->debugReadSpu2(r.t, 0x54));
        v.note = 0x7F00;
        r.rpcStruct(0x7240, pack(v));
        CHECK(r.d->debugReadSpu2(r.t, 0x54) == 0x3FFF, "pitch clamp 0x%x", r.d->debugReadSpu2(r.t, 0x54));
        // ADSR component RMW: AR with exp mode, SR mode 7, RR mode 7, SL.
        v.mask = 0x20000 | 0x40000;
        v.adsr1 = 0;
        v.adsr2 = 0;
        r.rpcStruct(0x7240, pack(v));
        v.mask = 0x800 | 0x100 | 0x1000 | 0x2000 | 0x200 | 0x4000 | 0x400 | 0x8000;
        v.ar = 0x90; v.amode = 5; v.dr = 3; v.sr = 0x11; v.smode = 7; v.rr = 0x40; v.rmode = 7; v.sl = 9;
        r.rpcStruct(0x7240, pack(v));
        const uint16_t a1 = r.d->debugReadSpu2(r.t, 0x56), a2 = r.d->debugReadSpu2(r.t, 0x58);
        CHECK(a1 == uint16_t(((0x7F | 0x80) << 8) | (3 << 4) | 9), "ADSR1 0x%04x", a1);
        CHECK(a2 == uint16_t(((0x11 | 0x300) << 6) | 0x1F | 0x20), "ADSR2 0x%04x", a2);
    }
}

void testUnitKeysAndBatch()
{
    std::printf("[unit] keys / batch\n");
    Rig r(false);
    r.rpcW(0x4008, {});
    // Slow release on voice 3 (core 0 and core 1).
    VA v;
    v.voice = 1u << 3;
    v.mask = 0x1 | 0x2 | 0x20000 | 0x40000;
    v.volL = v.volR = 0x3FFF;
    v.adsr1 = 0x000F;
    v.adsr2 = 0x1FC0 | 0x0C; // linear release, ~170 ms
    r.rpcStruct(0x7240, pack(v));
    r.rpcW(0x0002, {1});
    r.rpcStruct(0x7240, pack(v));
    r.rpcW(0x0002, {0});

    // Key on then status at the same cycle: ENVX still 0 -> 3 (ON, envelope off).
    r.rpcW(0x0005, {1, 1u << 3});
    auto ks = r.keyStatus();
    CHECK(ks[3] == 3, "status right after key-on %d", ks[3]);
    r.step(768 * 4);
    ks = r.keyStatus();
    CHECK(ks[3] == 1, "status after ticks %d", ks[3]);
    r.rpcW(0x0005, {0, 1u << 3});
    r.step(768 * 2);
    ks = r.keyStatus();
    CHECK(ks[3] == 2, "status after key-off, still releasing %d", ks[3]);
    for (int i = 0; i < 60 && ks[3] != 0; i++)
    {
        r.step(kFrame);
        ks = r.keyStatus();
    }
    CHECK(ks[3] == 0, "status after the release ends %d", ks[3]);

    // Batch: core 1 key-on, status of both cores, reply layout.
    const uint64_t tb = r.t;
    uint64_t end = 0;
    auto rep = r.batch({0x0002, 1, 0x0005, 1, 1u << 3, 0x6418, 0, 0x0002, 0, 0x6418, 0, 0xFFFFFFFFu}, &end);
    CHECK(end == tb + 1106, "one 30 us post-delay: end-start=%" PRIu64, end - tb);
    CHECK(rep[24 + 3] == 1, "core1 voice 3 on %d", rep[24 + 3]);
    CHECK(rep[3] == 0, "core0 voice 3 off %d", rep[3]);
    bool zeros = true;
    for (int i = 48; i < 64; i++)
        zeros &= rep[i] == 0;
    CHECK(zeros, "bytes 48..63 zero");
    // Batch with only core-0 status keeps the old core-1 bytes (persisting).
    rep = r.batch({0x6418, 0, 0xFFFFFFFFu});
    CHECK(rep[24 + 3] == 1, "core1 status persists between batches %d", rep[24 + 3]);
    // Unknown command skipped by one word: its "args" run as commands.
    const uint64_t u0 = r.d->stats().unknown_commands;
    r.batch({0x1234, 0x0002, 1, 0xFFFFFFFFu});
    CHECK(r.d->stats().unknown_commands == u0 + 1, "unknown in batch counted");
    CHECK(r.rpcW(0x0002, {0}) == 1, "the unknown's arg word ran as select-core 1");
    // Batch replies with more than 0x40 bytes continue into the env mirror.
    uint8_t mir[8] = {0xA1, 0xA2, 0xA3, 0xA4, 0xB1, 0xB2, 0xB3, 0xB4};
    r.write(kBlock + kMirrorOffset, mir, 8);
    std::vector<uint8_t> big(0x48);
    uint32_t words[1] = {0xFFFFFFFFu};
    r.d->rpc(r.t, 0xFFFE, words, 4, big.data(), 0x48);
    CHECK(big[0x3F] == 0 && big[0x40] == 0xA1 && big[0x47] == 0xB4, "reply tail = mirror");
    // Batch timing: SetKey, depth, reverb voice each add 1106 cycles.
    const uint64_t tc = r.t;
    r.batch({0x0005, 0, 0, 0x000A, 0x10, 0x10, 0x000B, 1, 0, 0x0002, 0, 0xFFFFFFFFu}, &end);
    CHECK(end == tc + 3 * 1106, "three post-delays: %" PRIu64, end - tc);
}

void testUnitVab()
{
    std::printf("[unit] SsVab\n");
    Rig r(false);
    r.rpcW(0x4008, {});
    const uint32_t hdr = kBase + 0x10000, body = kBase + 0x40000;
    std::vector<uint8_t> h(0xE20, 0);
    put32(&h[0], 0x56414270u); // "pBAV"
    put32(&h[4], 7);           // version > 4: 0x80 programs, sizes << 3
    put16(&h[0x12], 2);        // ps
    h[0x16] = 3;               // vs
    for (int k = 0; k < 0x80; k++)
        put32(&h[0x20 + 16 * k + 8], 0xDEADBEEF);
    h[0x20 + 0] = 1;
    h[0x20 + 16] = 2;
    const uint32_t sizes = 0x20 + 0x800 + 2 * 512;
    put16(&h[sizes + 0], 0);
    put16(&h[sizes + 2], 0x10);
    put16(&h[sizes + 4], 0x20);
    put16(&h[sizes + 6], 0x08);
    r.write(hdr, h.data(), uint32_t(h.size()));
    std::vector<uint8_t> bodyBytes(0x1C0, 0x11);
    r.write(body, bodyBytes.data(), uint32_t(bodyBytes.size()));

    CHECK(r.rpcW(0x4065, {0}) == 1, "TransCompleted when idle = 1");
    CHECK(r.rpcW(0x4063, {hdr, 0xFFFF}) == 0, "OpenHead auto id 0");
    CHECK(r.rpcW(0x4059, {0, 1}) == 0xFFFFFFFFu, "GetVagAddr before TransBody");
    CHECK(r.rpcW(0x4063, {hdr, 0xFFFF}) == 0xFFFFFFFFu, "OpenHead while busy");
    r.step(1);
    // OpenHead writes into IOP memory.
    const uint32_t prog = hdr + 0x20;
    int progWrites = 0, bad = 0;
    uint16_t vagw[4] = {};
    for (const IopWrite& w : r.writes)
    {
        const uint32_t off = w.addr - prog;
        if (w.bytes.size() == 4 && (off & 15) == 8)
        {
            const uint32_t k = off / 16;
            const uint32_t want = k == 0 ? 0 : (k == 1 ? 1 : 2);
            uint32_t got;
            std::memcpy(&got, w.bytes.data(), 4);
            bad += got != want;
            progWrites++;
        }
        else if (w.bytes.size() == 2)
        {
            const uint32_t idx = (off / 16) * 2 + ((off & 15) == 14 ? 1 : 0);
            if (idx < 4)
                vagw[idx] = uint16_t(w.bytes[0] | w.bytes[1] << 8);
        }
    }
    CHECK(progWrites == 0x80 && bad == 0, "program +8 writes: %d (bad %d)", progWrites, bad);
    CHECK(vagw[0] == 0x501 && vagw[1] == 0x509 && vagw[2] == 0x519 && vagw[3] == 0x51D,
          "VAG table %x %x %x %x", vagw[0], vagw[1], vagw[2], vagw[3]);

    const uint32_t mirror = r.rpcW(0x0200, {0});
    CHECK(r.rpcW(0x4066, {body, 0}) == mirror, "TransBody reply is stale");
    const uint64_t tBody = r.t;
    CHECK(r.rpcW(0x4065, {0}) == 0, "TransCompleted(0) right after TransBody = 0");
    uint64_t tDone = 0;
    for (int i = 0; i < 400; i++)
    {
        r.t += 256;
        if (r.rpcW(0x4065, {0}) == 1)
        {
            tDone = r.t;
            break;
        }
    }
    CHECK(tDone > tBody, "TransCompleted became 1 after the DMA-done event");
    std::printf("  body DMA (0x1C0 bytes) completed %" PRIu64 " cycles after TransBody\n", tDone - tBody);
    CHECK(r.rpcW(0x4065, {0}) == 1, "then idle: 1");
    CHECK(r.rpcW(0x4059, {0, 1}) == 0x5010, "vag1");
    CHECK(r.rpcW(0x4059, {0, 2}) == 0x5090, "vag2");
    CHECK(r.rpcW(0x4059, {0, 3}) == 0x5190, "vag3");
    CHECK(r.rpcW(0x4059, {0, 0}) == 0x5090, "vag0 reads index 1");
    CHECK(r.rpcW(0x4059, {1, 1}) == 0xFFFFFFFFu, "closed slot");

    // TransCompleted(1) busy-waits on the SPU2 timeline for the second VAB.
    CHECK(r.rpcW(0x4063, {hdr, 1}) == 1, "explicit id 1");
    r.rpcW(0x4066, {body, 1});
    uint64_t end = 0;
    const uint64_t t1 = r.t;
    CHECK(r.rpcW(0x4065, {1}, &end) == 1, "TransCompleted(1)");
    CHECK(end > t1, "TransCompleted(1) waited %" PRIu64 " cycles", end - t1);
    CHECK(r.rpcW(0x4059, {1, 1}) == 0x5010 + 0x1C0, "vab 1 base 0x%x", r.rpcW(0x4059, {1, 1}));

    // Close and reopen: the freed block is reused.
    r.rpcW(0x4062, {0});
    CHECK(r.rpcW(0x4063, {hdr, 0xFFFF}) == 0, "reopen slot 0");
    r.step(1);
    // TransCompleted(1) between OpenHead and TransBody would spin forever.
    const uint64_t h0 = r.d->stats().hangs_avoided;
    CHECK(r.rpcW(0x4065, {1}) == 1, "hang case returns 1");
    CHECK(r.d->stats().hangs_avoided == h0 + 1, "hang counted");
    // Bad magic: -1, and the transfer is idle again afterwards.
    uint32_t junk = 0x12345678;
    r.write(hdr + 0x100000, &junk, 4);
    CHECK(r.rpcW(0x4063, {hdr + 0x100000, 0xFFFF}) == 0xFFFFFFFFu, "bad magic");
    CHECK(r.rpcW(0x4063, {hdr, 0xFFFF}) == 2, "open after failure -> slot 2 (0 and 1 still open)");
}

// ===========================================================================
// 2. Streaming
// ===========================================================================
struct StreamResult
{
    int prepare = 0, transfer = 0, finish6 = 0, finish8 = 0;
    uint32_t prepA1 = 0, prepA2 = 0, finA1 = 0;
    std::vector<uint64_t> xferT;
    double rmsPlay = 0;
    double leftHz = 0;
    uint32_t statusAfterPrep = 0, statusAfterStart = 0, statusEnd = 0;
    std::vector<uint8_t> keysAfterStart;
};

constexpr uint32_t kData0 = kBase + 0x80000, kData1 = kBase + 0x81000;
constexpr uint32_t kHalf = 0x800;

void writeEnv(Rig& r, uint32_t buf0, uint32_t buf1, uint8_t status, int32_t lastSize)
{
    std::vector<uint8_t> env(kEnvSize, 0);
    put32(&env[0], 2 * kHalf);
    for (int v = 0; v < 24; v++)
        env[8 + 16 * v] = 6;
    const uint32_t bufs[2] = {buf0, buf1}, data[2] = {kData0, kData1};
    for (int v = 0; v < 2; v++)
    {
        uint8_t* e = &env[8 + 16 * v];
        e[0] = status;
        put32(e + 4, uint32_t(lastSize));
        put32(e + 8, bufs[v]);
        put32(e + 12, data[v]);
    }
    r.write(kBlock + kMirrorOffset, env.data(), kEnvSize);
}

StreamResult runStream(Rig& r, int roundsBeforeEnd, uint32_t seedAttrs, std::vector<float>* allOut,
                       std::vector<uint64_t>* hashes, std::vector<std::vector<uint8_t>>* replyLog)
{
    StreamResult res;
    auto log = [&](uint32_t w) {
        if (replyLog)
            replyLog->push_back({uint8_t(w), uint8_t(w >> 8), uint8_t(w >> 16), uint8_t(w >> 24)});
    };
    log(r.rpcW(0x4008, {}));
    // SsInit leaves core 1's master volume at 0 (the final mix runs through
    // core 1): set it like a game would.
    log(r.rpcW(0x0002, {1}));
    r.rpcW(0x404A, {127, 127});
    log(r.rpcW(0x0002, {0}));
    log(r.rpcW(0xE621, {}));
    log(r.rpcW(0x0200, {0}));
    const uint32_t buf0 = r.rpcW(0x0101, {0x1000}), buf1 = r.rpcW(0x0101, {0x1000});
    log(buf0);
    log(buf1);
    for (int v = 0; v < 2; v++)
    {
        VA a;
        a.voice = 1u << v;
        a.mask = 0x1 | 0x2 | 0x10 | 0x80 | 0x20000 | 0x40000;
        a.volL = v == 0 ? 0x3FFF : 0x1000;
        a.volR = v == 0 ? 0x1000 : 0x3FFF;
        a.pitch = 0x1000;
        a.addr = v == 0 ? buf0 : buf1;
        a.adsr1 = 0x000F;
        a.adsr2 = 0x1FC0;
        r.rpcStruct(0x7240, pack(a));
    }
    uint32_t vagAddr = 0x5000;
    if (seedAttrs)
    {
        // One looping VAG (0x400 bytes) through SsVab for the random voices.
        const uint32_t hdr = kBase + 0x30000, body = kBase + 0x38000;
        std::vector<uint8_t> h(0xE20, 0);
        put32(&h[0], 0x56414270u);
        put32(&h[4], 7);
        put16(&h[0x12], 1);
        h[0x16] = 1;
        h[0x20] = 1;
        put16(&h[0x20 + 0x800 + 512 + 2], 0x400 >> 3);
        r.write(hdr, h.data(), uint32_t(h.size()));
        int ph = 0;
        auto vag = adpcmSine(0x400 / 16, 80, ph, 5);
        vag[1] = 0x04;
        vag[0x400 - 16 + 1] = 0x03;
        r.write(body, vag.data(), 0x400);
        log(r.rpcW(0x4063, {hdr, 0xFFFF}));
        r.rpcW(0x4066, {body, 0});
        log(r.rpcW(0x4065, {1}));
        vagAddr = r.rpcW(0x4059, {0, 1});
        log(vagAddr);
    }
    int phase0 = 0, phase1 = 0;
    auto refill = [&] {
        auto d0 = adpcmSine(kHalf / 16, 64, phase0);
        auto d1 = adpcmSine(kHalf / 16, 96, phase1);
        r.write(kData0, d0.data(), kHalf);
        r.write(kData1, d1.data(), kHalf);
    };
    refill();
    writeEnv(r, buf0, buf1, 6, 0);
    log(r.rpcW(0x0202, {4, 3}));
    res.statusAfterPrep = r.rpcW(0x0024, {});

    uint32_t rng = seedAttrs ? seedAttrs : 1;
    auto rnd = [&] {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        return rng;
    };
    bool started = false, ended = false, endMarked = false;
    std::vector<float> out(size_t(kFrame / 768 + 64) * 2);
    std::vector<float> playing;
    for (int frame = 0; frame < 60 * 12 && !ended; frame++)
    {
        r.step(kFrame);
        for (const Callback& c : r.cbs)
        {
            if (c.type == 1)
            {
                res.prepare++;
                res.prepA1 = c.a1;
                res.prepA2 = c.a2;
            }
            else if (c.type == 2)
            {
                res.transfer++;
                res.xferT.push_back(c.t);
                refill();
                if (res.transfer >= roundsBeforeEnd && !endMarked)
                {
                    writeEnv(r, buf0, buf1, 2, kHalf);
                    endMarked = true;
                }
            }
            else if (c.type == 3)
            {
                (c.a2 == 8 ? res.finish8 : res.finish6)++;
                res.finA1 = c.a1;
                if (c.a2 == 8)
                    ended = true;
            }
            r.d->callbackDelivered(r.t, c.type);
        }
        r.allCbs.insert(r.allCbs.end(), r.cbs.begin(), r.cbs.end());
        r.allWrites.insert(r.allWrites.end(), r.writes.begin(), r.writes.end());
        r.cbs.clear();
        r.writes.clear();
        if (!started && res.prepare > 0)
        {
            log(r.rpcW(0x0202, {5, 3}));
            res.statusAfterStart = r.rpcW(0x0024, {});
            res.keysAfterStart = r.keyStatus();
            started = true;
        }
        if (seedAttrs)
        {
            // A per-frame batch like RR5's: random attrs on voices 4..11, key status.
            std::vector<uint32_t> w;
            for (int k = 0; k < 3; k++)
            {
                VA a;
                a.voice = 1u << (4 + rnd() % 8);
                a.mask = 0x1 | 0x2 | 0x10 | 0x80 | 0x20000 | 0x40000;
                a.addr = vagAddr;
                a.adsr1 = 0x000F;
                a.adsr2 = 0x1FC0 | 0x0C;
                a.volL = uint16_t(rnd() & 0x3FFF);
                a.volR = uint16_t(rnd() & 0x3FFF);
                a.pitch = uint16_t(0x400 + (rnd() & 0x1FFF));
                w.push_back(0x7240);
                auto p = pack(a);
                w.insert(w.end(), p.begin(), p.end());
            }
            if ((rnd() & 7) == 0)
            {
                w.push_back(0x0005);
                w.push_back(rnd() & 1);
                w.push_back(1u << (4 + rnd() % 8));
            }
            w.push_back(0x6418);
            w.push_back(0);
            w.push_back(0xFFFFFFFFu);
            if (frame & 1)
            {
                r.d->rpcAsync(r.t, 0xFFFE, w.data(), uint32_t(w.size() * 4), 0x40);
            }
            else
            {
                auto rep = r.batch(w);
                if (replyLog)
                    replyLog->push_back(rep);
            }
        }
        for (const AsyncReply& a : r.replies)
            if (replyLog)
            {
                std::vector<uint8_t> e(a.bytes);
                for (int i = 0; i < 8; i++)
                    e.push_back(uint8_t(a.t >> (8 * i)));
                replyLog->push_back(e);
            }
        r.replies.clear();
        const size_t got = r.d->pullOutput(out.data(), kFrame / 768);
        if (allOut)
            allOut->insert(allOut->end(), out.begin(), out.begin() + ptrdiff_t(got * 2));
        if (started && !endMarked)
            playing.insert(playing.end(), out.begin(), out.begin() + ptrdiff_t(got * 2));
        if (hashes && frame % 10 == 0)
            hashes->push_back(r.d->debugSpu2StateHash(r.t));
    }
    res.statusEnd = r.rpcW(0x0024, {});
    res.rmsPlay = rms(playing);
    int zc = 0;
    for (size_t i = 2; i < playing.size(); i += 2)
        zc += (playing[i - 2] < 0) != (playing[i] < 0);
    if (!playing.empty())
        res.leftHz = zc / 2.0 / (double(playing.size() / 2) / 48000.0);
    return res;
}

void testStream()
{
    std::printf("[stream]\n");
    Rig r(false);
    const StreamResult s = runStream(r, 12, 0, nullptr, nullptr, nullptr);
    CHECK(s.statusAfterPrep == 4, "status after PREPARE %u", s.statusAfterPrep);
    CHECK(s.prepare == 1 && s.prepA1 == 3 && s.prepA2 == 4, "prepare cb n=%d a1=%u a2=%u", s.prepare, s.prepA1, s.prepA2);
    CHECK(s.statusAfterStart == 7, "status after START %u", s.statusAfterStart);
    CHECK(s.keysAfterStart.size() == 24 && (s.keysAfterStart[0] & 1) && (s.keysAfterStart[1] & 1),
          "voices keyed on (%d %d)", s.keysAfterStart.empty() ? -1 : s.keysAfterStart[0],
          s.keysAfterStart.empty() ? -1 : s.keysAfterStart[1]);
    CHECK(s.transfer >= 12, "transfer callbacks %d", s.transfer);
    CHECK(s.finish8 == 1 && s.finA1 == 3, "final cb type 3 status 8 (n=%d a1=%u)", s.finish8, s.finA1);
    CHECK(s.statusEnd == 3, "status at end %u", s.statusEnd);
    const uint64_t roundCycles = uint64_t(kHalf / 16) * 28 * 768;
    int badGap = 0;
    for (size_t i = 2; i < s.xferT.size(); i++)
    {
        const uint64_t g = s.xferT[i] - s.xferT[i - 1];
        if (g + 4 * 768 < roundCycles || g > roundCycles + 4 * 768)
            badGap++;
    }
    CHECK(badGap == 0, "round spacing (expect %" PRIu64 ")", roundCycles);
    if (s.xferT.size() > 3)
        std::printf("  rounds=%d, spacing %" PRIu64 " / %" PRIu64 " cycles (expected %" PRIu64 "), finish6=%d\n",
                    s.transfer, s.xferT[2] - s.xferT[1], s.xferT[3] - s.xferT[2], roundCycles, s.finish6);
    CHECK(s.rmsPlay > 0.01, "audible output rms %.4f", s.rmsPlay);
    std::printf("  output rms while streaming %.4f, left-channel zero-crossing frequency %.1f Hz (voice 0: 750 Hz)\n",
                s.rmsPlay, s.leftHz);
    CHECK(s.leftHz > 700 && s.leftHz < 800, "left frequency %.1f", s.leftHz);
    const auto st = r.d->stats();
    std::printf("  dma=%" PRIu64 " callbacks=%" PRIu64 " frames=%" PRIu64 " spu2_overlaps=%" PRIu64
                " hangs_avoided=%" PRIu64 " oob=%" PRIu64 "\n",
                st.dma_transfers, st.callbacks, st.frames_mixed, st.spu2_dma_overlaps, st.hangs_avoided,
                st.iop_out_of_window);
    CHECK(st.spu2_dma_overlaps == 0 && st.iop_out_of_window == 0 && st.spu2_clamp_violations == 0, "clean");
}

// ===========================================================================
// 3. Determinism
// ===========================================================================
struct Session
{
    std::vector<std::vector<uint8_t>> replies;
    std::vector<Callback> cbs;
    std::vector<IopWrite> writes;
    std::vector<uint64_t> hashes;
    std::vector<float> out;
    uint64_t outHash = 0, finalHash = 0;
    StreamResult sr;
};

Session runSession(bool threaded)
{
    Session s;
    Rig r(threaded);
    // Keep all callbacks / writes: wrap by running the stream with logging.
    s.sr = runStream(r, 20, 0xC0FFEEu, &s.out, &s.hashes, &s.replies);
    // A VAB round trip + reverb at the end.
    const uint32_t hdr = kBase + 0x20000;
    std::vector<uint8_t> h(0xE20, 0);
    put32(&h[0], 0x56414270u);
    put32(&h[4], 5);
    put16(&h[0x12], 1);
    h[0x16] = 2;
    put16(&h[0x20 + 0x800 + 512 + 2], 0x40);
    put16(&h[0x20 + 0x800 + 512 + 4], 0x40);
    r.write(hdr, h.data(), uint32_t(h.size()));
    s.replies.push_back({uint8_t(r.rpcW(0x4063, {hdr, 0xFFFF}))});
    r.rpcW(0x4066, {kData0, 0});
    s.replies.push_back({uint8_t(r.rpcW(0x4065, {1}))});
    uint32_t rv[5] = {0, 7, 0x40, 0x40, 0x30};
    uint32_t rep;
    r.t = r.d->rpc(r.t, 0x7314, rv, sizeof(rv), &rep, 4);
    std::vector<AsyncReply> ar;
    for (int i = 0; i < 30; i++)
    {
        r.t += kFrame;
        r.d->advance(r.t);
    }
    r.d->sync(r.t, ar, r.cbs, r.writes);
    r.allCbs.insert(r.allCbs.end(), r.cbs.begin(), r.cbs.end());
    r.allWrites.insert(r.allWrites.end(), r.writes.begin(), r.writes.end());
    s.cbs = r.allCbs;
    s.writes = r.allWrites;
    std::vector<float> tail(size_t(30 * kFrame / 768 + 64) * 2);
    const size_t got = r.d->pullOutput(tail.data(), 30 * kFrame / 768);
    s.out.insert(s.out.end(), tail.begin(), tail.begin() + ptrdiff_t(got * 2));
    s.finalHash = r.d->debugSpu2StateHash(r.t);
    s.outHash = r.d->debugOutputHash();
    return s;
}

void testDeterminism()
{
    std::printf("[determinism] inline vs threaded\n");
    const Session a = runSession(false);
    const Session b = runSession(true);
    CHECK(a.replies.size() == b.replies.size() && a.replies == b.replies, "replies (%zu vs %zu)", a.replies.size(),
          b.replies.size());
    CHECK(a.sr.xferT == b.sr.xferT, "transfer callback cycles");
    CHECK(a.cbs.size() == b.cbs.size(), "callbacks");
    CHECK(a.writes.size() == b.writes.size(), "iop writes");
    bool cbEq = a.cbs.size() == b.cbs.size();
    for (size_t i = 0; cbEq && i < a.cbs.size(); i++)
        cbEq = a.cbs[i].t == b.cbs[i].t && a.cbs[i].type == b.cbs[i].type && a.cbs[i].a1 == b.cbs[i].a1 &&
               a.cbs[i].a2 == b.cbs[i].a2;
    CHECK(cbEq, "callback contents");
    bool wEq = a.writes.size() == b.writes.size();
    for (size_t i = 0; wEq && i < a.writes.size(); i++)
        wEq = a.writes[i].t == b.writes[i].t && a.writes[i].addr == b.writes[i].addr && a.writes[i].bytes == b.writes[i].bytes;
    CHECK(wEq, "iop write contents");
    CHECK(a.hashes == b.hashes && !a.hashes.empty(), "SPU2 state hashes (%zu)", a.hashes.size());
    CHECK(a.finalHash == b.finalHash, "final state hash");
    CHECK(a.out.size() == b.out.size() && std::memcmp(a.out.data(), b.out.data(), a.out.size() * 4) == 0,
          "output samples (%zu vs %zu floats)", a.out.size(), b.out.size());
    CHECK(a.outHash == b.outHash, "output hash");
    CHECK(a.sr.finish8 == 1 && a.sr.transfer >= 20, "session streamed (%d rounds)", a.sr.transfer);
    std::printf("  callbacks=%zu iopWrites=%zu\n", a.cbs.size(), a.writes.size());
    std::printf("  replies=%zu cbs(stream)=%d rounds, hashes=%zu, samples=%zu, outHash=%016" PRIx64
                " stateHash=%016" PRIx64 "\n",
                a.replies.size(), a.sr.transfer, a.hashes.size(), a.out.size() / 2, a.outHash, a.finalHash);
}

// ===========================================================================
// 4. Main-thread cost
// ===========================================================================
void spinNs(uint64_t ns)
{
    const auto t0 = std::chrono::steady_clock::now();
    while (uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count()) < ns)
    {
    }
}

struct Cost
{
    double blockingUs, asyncUs;
    uint64_t barrierWaits, barrierNs;
};

Cost measureCost(bool threaded, uint64_t gameWorkNs, int frames)
{
    Rig r(threaded);
    // Streaming running in the background plus a typical per-frame batch.
    r.rpcW(0x4008, {});
    r.rpcW(0xE621, {});
    r.rpcW(0x0200, {0});
    const uint32_t b0 = r.rpcW(0x0101, {0x1000}), b1 = r.rpcW(0x0101, {0x1000});
    int ph = 0;
    auto d0 = adpcmSine(kHalf / 16, 64, ph);
    r.write(kData0, d0.data(), kHalf);
    r.write(kData1, d0.data(), kHalf);
    writeEnv(r, b0, b1, 6, 0);
    r.rpcW(0x0202, {4, 3});
    r.step(kFrame);
    r.rpcW(0x0202, {5, 3});
    std::vector<uint32_t> w;
    for (int v = 0; v < 24; v += 3)
    {
        VA a;
        a.voice = 1u << v;
        a.mask = 0x1 | 0x2 | 0x10;
        a.volL = 0x2000;
        a.volR = 0x2000;
        a.pitch = uint16_t(0x800 + v * 64);
        w.push_back(0x7240);
        auto p = pack(a);
        w.insert(w.end(), p.begin(), p.end());
    }
    w.insert(w.end(), {0x0005, 1, 0x100, 0x6418, 0, 0xFFFFFFFFu});
    std::vector<float> out(2048 * 2);
    std::vector<AsyncReply> ar;
    std::vector<Callback> cb;
    std::vector<IopWrite> wr;
    Cost c{};
    double tb = 0, ta = 0;
    // (a) blocking batch at t, then advance to the next frame.
    for (int f = 0; f < frames; f++)
    {
        const auto t0 = std::chrono::steady_clock::now();
        uint8_t rep[0x40];
        r.t = std::max(r.t, r.d->rpc(r.t, 0xFFFE, w.data(), uint32_t(w.size() * 4), rep, 0x40));
        r.t += kFrame;
        r.d->advance(r.t);
        tb += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        r.d->pullOutput(out.data(), 1600);
        spinNs(gameWorkNs);
    }
    // (b) sync(t) + deliver callbacks, async batch at t, advance.
    for (int f = 0; f < frames; f++)
    {
        const auto t0 = std::chrono::steady_clock::now();
        r.d->sync(r.t, ar, cb, wr);
        for (const Callback& x : cb)
            r.d->callbackDelivered(r.t, x.type);
        ar.clear();
        cb.clear();
        wr.clear();
        r.d->rpcAsync(r.t, 0xFFFE, w.data(), uint32_t(w.size() * 4), 0x40);
        r.t += kFrame;
        r.d->advance(r.t);
        ta += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        r.d->pullOutput(out.data(), 1600);
        spinNs(gameWorkNs);
    }
    const auto st = r.d->stats();
    c.blockingUs = tb / frames;
    c.asyncUs = ta / frames;
    c.barrierWaits = st.barrier_waits;
    c.barrierNs = st.barrier_wait_ns;
    return c;
}

void testCost()
{
    std::printf("[cost] caller time per 1/60 s of guest time (streaming active, 9-cmd batch)\n");
    const int frames = 300;
    for (uint64_t work : {uint64_t(0), uint64_t(4000000)})
    {
        const Cost in = measureCost(false, work, frames);
        const Cost th = measureCost(true, work, frames);
        std::printf("  game work %4.1f ms/frame: inline: blocking batch+advance %.1f us, sync+async batch+advance %.1f us;"
                    " threaded: %.1f us, %.1f us (barrier waits %" PRIu64 ", avg %.1f us)\n",
                    double(work) / 1e6, in.blockingUs, in.asyncUs, th.blockingUs, th.asyncUs, th.barrierWaits,
                    th.barrierWaits ? double(th.barrierNs) / double(th.barrierWaits) / 1000.0 : 0.0);
    }
}
} // namespace

int main(int argc, char** argv)
{
    const std::string what = argc > 1 ? argv[1] : "all";
    if (what == "unit" || what == "all")
    {
        testUnitBasics();
        testUnitKeysAndBatch();
        testUnitVab();
    }
    if (what == "stream" || what == "all")
        testStream();
    if (what == "determinism" || what == "all")
        testDeterminism();
    if (what == "cost" || what == "all")
        testCost();
    std::printf("%s (%d failures)\n", g_fail ? "FAIL" : "PASS", g_fail);
    return g_fail ? 1 : 0;
}
