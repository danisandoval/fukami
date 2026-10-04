// SPDX-License-Identifier: GPL-3.0+
// Gate 8: host re-implementation of RR5's IOP sound driver (RSPU2DRV.IRX,
// librspu2 over SIF RPC SID 0x80000601) on top of the PCSX2 SPU2 core in
// tools/pcsx2-spu2. See rspu2_driver.h for the API contract and
// local/gate8/rspu2-driver-spec.md for the behaviour reproduced here ("spec
// N" below = section N). Deliberate deviations are marked DEVIATION.
#include "rspu2_driver.h"

#include "rrv_pcsx2_spu2.h"
#if defined(RRV_GATE8_SPU2)
#include "rrv_sdl_audio.h"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#if defined(__linux__)
#include <pthread.h>
#endif

namespace rrv::rspu2
{
namespace
{
// ---- timing constants (guest IOP cycles) ---------------------------------
// The batch's 30 us GetSystemTime busy-wait (spec 1): 30e-6 * 36.864e6.
constexpr uint64_t kBatchDelay = 1106;
// ASSUMPTION: 1 IOP cycle per instruction for the driver's empty loops.
// "delay" (60 iterations x 13 instructions) and the KON/KOFF spin in the
// voice init (0xC34 iterations x 8 instructions), read from the disassembly.
constexpr uint64_t kDelay60 = 60 * 13;
constexpr uint64_t kSpinC34 = 0xC34 * 8;
constexpr uint64_t kPollCycles = 8; // one register-poll iteration
constexpr uint32_t kPollCap = 0xF00;

constexpr uint32_t kRecvSize = 0x1000; // RPC receive buffer (drv+0x18940)
constexpr uint32_t kKeyBufSize = 0x40; // key status / batch reply (drv+0x19940)
// The receive buffer, key buffer and env mirror are contiguous in the real
// driver (0x18940, 0x19940, 0x19980); reads past one land in the next.
constexpr uint32_t kViewSize = kRecvSize + kKeyBufSize + kEnvSize;
constexpr uint32_t kMallocN = 32;

constexpr int kNoVoice = 0x18;

// A.1 reverb work-area sizes (x8 bytes) and presets (32 fields, mask 0).
constexpr uint32_t kRevSize[10] = {0x2, 0x4D8, 0x3E8, 0x908, 0xDFC, 0x15BC, 0x1ED8, 0x3008, 0x3008, 0x780};
constexpr uint16_t kRevPreset[10][32] = {
    {0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0001, 0x0001, 0x0000, 0x0000, 0x0000, 0x0000,
     0x0000, 0x0000, 0x0001, 0x0001, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000},
    {0x007d, 0x005b, 0x6d80, 0x54b8, 0xbed0, 0x0000, 0x0000, 0xba80, 0x5800, 0x5300, 0x04d6, 0x0333, 0x03f0, 0x0227, 0x0374, 0x01ef,
     0x0336, 0x01b7, 0x0335, 0x01b6, 0x0334, 0x01b5, 0x0334, 0x01b5, 0x0334, 0x01b5, 0x01b4, 0x0136, 0x00b8, 0x005c, 0x8000, 0x8000},
    {0x0033, 0x0025, 0x70f0, 0x4fa8, 0xbce0, 0x4410, 0xc0f0, 0x9c00, 0x5280, 0x4ec0, 0x03e4, 0x031b, 0x03a4, 0x02af, 0x0372, 0x0266,
     0x031c, 0x025d, 0x025c, 0x018e, 0x022f, 0x0135, 0x01d2, 0x00b7, 0x018f, 0x00b5, 0x00b4, 0x0080, 0x004c, 0x0026, 0x8000, 0x8000},
    {0x00b1, 0x007f, 0x70f0, 0x4fa8, 0xbce0, 0x4510, 0xbef0, 0xb4c0, 0x5280, 0x4ec0, 0x0904, 0x076b, 0x0824, 0x065f, 0x07a2, 0x0616,
     0x076c, 0x05ed, 0x05ec, 0x042e, 0x050f, 0x0305, 0x0462, 0x02b7, 0x042f, 0x0265, 0x0264, 0x01b2, 0x0100, 0x0080, 0x8000, 0x8000},
    {0x00e3, 0x00a9, 0x6f60, 0x4fa8, 0xbce0, 0x4510, 0xbef0, 0xa680, 0x5680, 0x52c0, 0x0dfb, 0x0b58, 0x0d09, 0x0a3c, 0x0bd9, 0x0973,
     0x0b59, 0x08da, 0x08d9, 0x05e9, 0x07ec, 0x04b0, 0x06ef, 0x03d2, 0x05ea, 0x031d, 0x031c, 0x0238, 0x0154, 0x00aa, 0x8000, 0x8000},
    {0x01a5, 0x0139, 0x6000, 0x5000, 0x4c00, 0xb800, 0xbc00, 0xc000, 0x6000, 0x5c00, 0x15ba, 0x11bb, 0x14c2, 0x10bd, 0x11bc, 0x0dc1,
     0x11c0, 0x0dc3, 0x0dc0, 0x09c1, 0x0bc4, 0x07c1, 0x0a00, 0x06cd, 0x09c2, 0x05c1, 0x05c0, 0x041a, 0x0274, 0x013a, 0x8000, 0x8000},
    {0x033d, 0x0231, 0x7e00, 0x5000, 0xb400, 0xb000, 0x4c00, 0xb000, 0x6000, 0x5400, 0x1ed6, 0x1a31, 0x1d14, 0x183b, 0x1bc2, 0x16b2,
     0x1a32, 0x15ef, 0x15ee, 0x1055, 0x1334, 0x0f2d, 0x11f6, 0x0c5d, 0x1056, 0x0ae1, 0x0ae0, 0x07a2, 0x0464, 0x0232, 0x8000, 0x8000},
    {0x0003, 0x0003, 0x7fff, 0x7fff, 0x0000, 0x0000, 0x0000, 0x8100, 0x0000, 0x0000, 0x1ffd, 0x0ffd, 0x1009, 0x0009, 0x0000, 0x0000,
     0x1009, 0x0009, 0x1fff, 0x1fff, 0x1ffe, 0x1ffe, 0x1ffe, 0x1ffe, 0x1ffe, 0x1ffe, 0x1008, 0x1004, 0x0008, 0x0004, 0x8000, 0x8000},
    {0x0003, 0x0003, 0x7fff, 0x7fff, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x1ffd, 0x0ffd, 0x1009, 0x0009, 0x0000, 0x0000,
     0x1009, 0x0009, 0x1fff, 0x1fff, 0x1ffe, 0x1ffe, 0x1ffe, 0x1ffe, 0x1ffe, 0x1ffe, 0x1008, 0x1004, 0x0008, 0x0004, 0x8000, 0x8000},
    {0x0017, 0x0013, 0x70f0, 0x4fa8, 0xbce0, 0x4510, 0xbef0, 0x8500, 0x5f80, 0x54c0, 0x0371, 0x02af, 0x02e5, 0x01df, 0x02b0, 0x01d7,
     0x0358, 0x026a, 0x01d6, 0x011e, 0x012d, 0x00b1, 0x011f, 0x0059, 0x01a0, 0x00e3, 0x0058, 0x0040, 0x0028, 0x0014, 0x8000, 0x8000},
};

// A.2 note-to-pitch tables.
constexpr uint16_t kOct[12] = {0x8000, 0x879c, 0x8fac, 0x9837, 0xa145, 0xaadc, 0xb504, 0xbfc8, 0xcb2f, 0xd744, 0xe411, 0xf1a1};
constexpr uint16_t kFine[128] = {
    0x8000, 0x800e, 0x801d, 0x802c, 0x803b, 0x804a, 0x8058, 0x8067, 0x8076, 0x8085, 0x8094, 0x80a3, 0x80b1, 0x80c0, 0x80cf, 0x80de,
    0x80ed, 0x80fc, 0x810b, 0x811a, 0x8129, 0x8138, 0x8146, 0x8155, 0x8164, 0x8173, 0x8182, 0x8191, 0x81a0, 0x81af, 0x81be, 0x81cd,
    0x81dc, 0x81eb, 0x81fa, 0x8209, 0x8218, 0x8227, 0x8236, 0x8245, 0x8254, 0x8263, 0x8272, 0x8282, 0x8291, 0x82a0, 0x82af, 0x82be,
    0x82cd, 0x82dc, 0x82eb, 0x82fa, 0x830a, 0x8319, 0x8328, 0x8337, 0x8346, 0x8355, 0x8364, 0x8374, 0x8383, 0x8392, 0x83a1, 0x83b0,
    0x83c0, 0x83cf, 0x83de, 0x83ed, 0x83fd, 0x840c, 0x841b, 0x842a, 0x843a, 0x8449, 0x8458, 0x8468, 0x8477, 0x8486, 0x8495, 0x84a5,
    0x84b4, 0x84c3, 0x84d3, 0x84e2, 0x84f1, 0x8501, 0x8510, 0x8520, 0x852f, 0x853e, 0x854e, 0x855d, 0x856d, 0x857c, 0x858b, 0x859b,
    0x85aa, 0x85ba, 0x85c9, 0x85d9, 0x85e8, 0x85f8, 0x8607, 0x8617, 0x8626, 0x8636, 0x8645, 0x8655, 0x8664, 0x8674, 0x8683, 0x8693,
    0x86a2, 0x86b2, 0x86c1, 0x86d1, 0x86e0, 0x86f0, 0x8700, 0x870f, 0x871f, 0x872e, 0x873e, 0x874e, 0x875d, 0x876d, 0x877d, 0x878c,
};

uint16_t note2pitch(uint32_t cen, uint32_t cfine, uint32_t nn, uint32_t fine)
{
    const uint32_t f = (fine + cfine) & 0xFFFF;
    const int32_t v = int16_t(uint16_t(nn + (f >> 7) - cen));
    const int16_t oct = int16_t(v / 12);
    int16_t sh = int16_t(oct - 2);
    int32_t rem = v % 12;
    if (rem < 0)
    {
        rem += 12;
        sh = int16_t(oct - 3);
    }
    if (sh >= 0)
        return 0x3FFF;
    const uint32_t s = uint32_t(-int32_t(sh));
    const uint32_t prod = (uint32_t(kOct[rem]) * uint32_t(kFine[(fine + cfine) & 0x7F])) >> 16;
    return uint16_t((prod + (1u << ((s - 1) & 31))) >> (s & 31));
}

uint32_t ld32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint16_t ld16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
void st32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24); }

int firstBit(uint32_t m)
{
    int i = 0;
    while (i < kNoVoice && !((m >> i) & 1))
        i++;
    return i;
}

int nextBit(uint32_t m, int from)
{
    int i = from;
    do
    {
        i++;
        if (i > 0x17)
            break;
    } while (!((m >> i) & 1));
    return i;
}

uint64_t hostNs()
{
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}

struct Counters
{
    std::atomic<uint64_t> rpc_count{0}, batch_entries{0}, barrier_waits{0}, barrier_wait_ns{0},
        queue_high_water{0}, clamp_violations{0}, unknown_commands{0}, last_unknown_fno{0},
        iop_oob{0}, dma_transfers{0}, callbacks{0}, callbacks_dropped{0}, hangs_avoided{0},
        spin_timeouts{0}, driver_cycle{0}, stale_refills{0}, slow_ops{0}, max_op_ns{0};
    static void inc(std::atomic<uint64_t>& a, uint64_t n = 1) { a.fetch_add(n, std::memory_order_relaxed); }
};

// ===========================================================================
// Engine: the driver proper. Runs on exactly one thread at a time (the driver
// thread, or the caller in inline mode) and owns the SPU2 core.
// ===========================================================================
class Engine
{
public:
    Engine(const Config& cfg, rrv_spu2* spu, Counters& ctr)
        : cfg_(cfg), spu_(spu), ctr_(ctr), iop_(cfg.iop_window_size, 0)
    {
        for (auto& core : sampleNote_)
            for (auto& sn : core)
                sn = 0xC000; // A.3: .data default for both cores
        std::memset(recv_, 0, sizeof(recv_));
        std::memset(keyBuf_, 0, sizeof(keyBuf_));
        std::memset(env_, 0, sizeof(env_));
        std::memset(saved16_, 0, sizeof(saved16_));
        std::memset(mt_, 0, sizeof(mt_));
    }

    uint32_t mirrorAddr() const { return cfg_.driver_block + kMirrorOffset; }

    // ---- ops ---------------------------------------------------------------
    void opIopWrite(uint64_t t, uint32_t addr, const std::vector<uint8_t>& data)
    {
        runTo(t);
        for (uint32_t i = 0; i < data.size(); i++)
            poke8(addr + i, data[i]);
    }

    uint64_t opRpc(uint64_t t, uint32_t fno, const std::vector<uint8_t>& data, uint32_t replySize,
                   std::vector<uint8_t>& reply)
    {
        runTo(t);
        const uint32_t n = std::min<uint32_t>(uint32_t(data.size()), kRecvSize);
        if (n)
            std::memcpy(recv_, data.data(), n); // bytes past n keep earlier RPCs' contents
        const bool keysBuf = dispatch(fno);
        buildReply(keysBuf, replySize, reply);
        publishCycle();
        return now_;
    }

    void opRpcAsync(uint64_t t, uint64_t id, uint32_t fno, const std::vector<uint8_t>& data, uint32_t replySize)
    {
        std::vector<uint8_t> reply;
        const uint64_t end = opRpc(t, fno, data, replySize, reply);
        replies_.push_back(AsyncReply{id, end, std::move(reply)});
    }

    void opCallbackDelivered(uint64_t t, uint32_t type)
    {
        runTo(t);
        if (type == 2 && xferUndelivered_ > 0)
            xferUndelivered_--;
        if (type >= 1 && type <= 3)
            copyMirrorToEnv();
    }

    void opAdvance(uint64_t t)
    {
        runTo(t);
        publishCycle();
    }

    void opSync(uint64_t t, std::vector<AsyncReply>& r, std::vector<Callback>& c, std::vector<IopWrite>& w)
    {
        runTo(t);
        while (!replies_.empty() && replies_.front().t <= t)
        {
            r.push_back(std::move(replies_.front()));
            replies_.pop_front();
        }
        while (!callbacks_.empty() && callbacks_.front().t <= t)
        {
            c.push_back(callbacks_.front());
            callbacks_.pop_front();
        }
        while (!writes_.empty() && writes_.front().t <= t)
        {
            w.push_back(std::move(writes_.front()));
            writes_.pop_front();
        }
        publishCycle();
    }

    uint64_t opHash(uint64_t t)
    {
        runTo(t);
        return rrv_spu2_state_hash(spu_, now_);
    }

    uint16_t opRead(uint64_t t, uint32_t reg)
    {
        runTo(t);
        return R(reg);
    }

private:
    enum class DmaCb : uint8_t { None, Type5, PrepDone, HalfDone };
    enum class IrqCb : uint8_t { None, Type6, StreamIrq, StreamFinish };
    struct MEnt
    {
        uint32_t w0, size;
    };
    static constexpr uint32_t kFree = 0x80000000u, kTail = 0x40000000u, kInvalid = 0x2FFFFFFFu, kAddrMask = 0x0FFFFFFFu;

    const Config cfg_;
    rrv_spu2* spu_;
    Counters& ctr_;

    // ---- timeline
    uint64_t now_ = 0;

    // ---- IOP window shadow + outputs
    std::vector<uint8_t> iop_;
    std::deque<AsyncReply> replies_;
    std::deque<Callback> callbacks_;
    std::deque<IopWrite> writes_;

    // ---- RPC buffers
    uint8_t recv_[kRecvSize];
    uint8_t keyBuf_[kKeyBufSize];
    uint32_t ret_ = 0;      // reply word drv+0x16620 (stale unless a command writes it)
    uint32_t cnt7600_ = 0;  // drv+0x16624

    // ---- libspu state
    int core_ = 0;
    uint32_t keyMask_[2] = {0, 0};
    uint16_t sampleNote_[2][24];
    bool handlers_ = false;   // interrupt handlers installed (SpuInit), removed by SpuQuit
    bool quitArmed_ = false;  // drv+0x16c9c
    bool intr9_ = true;       // the server thread enables intr 9 at start
    uint16_t attrIrq_[2] = {0, 0};
    DmaCb dmaCb_ = DmaCb::None;
    IrqCb irqCb_ = IrqCb::None;
    bool dmaDone_ = false;    // drv+0x1725c
    bool idle_ = true;        // drv+0x16bb8 == 1
    uint32_t transferMode_ = 0; // drv+0x16c0c (1 = PIO)
    uint32_t tsaWord_ = 0;    // drv+0x16bac
    bool dmaPending_ = false;
    std::vector<uint16_t> dmaRest_; // tail of a > 1 MiB transfer (issued as a continuation)
    bool cbThread_ = false;
    uint32_t mbA1_ = 0, mbA2_ = 0;
    uint32_t xferUndelivered_ = 0; // type-2 callbacks posted but not yet returned by the EE (diagnostic)

    // ---- reverb
    uint32_t revOn_ = 0, revMode_ = 0, revOffset_ = 0;
    int32_t revDelay_ = 0, revFeedback_ = 0;
    uint16_t revDepthL_ = 0, revDepthR_ = 0;
    uint32_t eeaStore_[2] = {0, 0};

    // ---- SpuMalloc
    MEnt mt_[kMallocN + 1];
    int mN_ = 0, mLast_ = 0;

    // ---- SsVab
    uint8_t vabState_[16] = {};
    uint32_t vabProg_[16] = {}, vabBase_[16] = {}, vabTotal_[16] = {};
    uint8_t vabHi_[16] = {};
    int vabCount_ = 0;
    uint32_t maxProgs_ = 0; // drv+0x17240 (global)

    // ---- SpuSt
    uint32_t S_ = 0, streamCore_ = 0;
    uint32_t A_ = 0, P_ = 0, F_ = 0, Fp_ = 0, PQ_ = 0, PR_ = 0, PD_ = 0, SQ_ = 0;
    int32_t half_ = 0;
    uint32_t H_ = 0;
    int cur_ = kNoVoice, curPrep_ = kNoVoice, firstActive_ = kNoVoice, firstFinish_ = kNoVoice;
    DmaCb savedDmaCb_ = DmaCb::None;
    IrqCb savedIrqCb_ = IrqCb::None;
    bool cbPrep_ = false, cbXfer_ = false, cbFin_ = false;
    uint32_t irqAddr_ = 0;
    uint8_t env_[kEnvSize];
    uint8_t saved16_[24][16];

    // =======================================================================
    // Timeline
    // =======================================================================
    void publishCycle() { ctr_.driver_cycle.store(now_, std::memory_order_relaxed); }

    bool needFine() const
    {
        if (dmaPending_)
            return true;
        return handlers_ && intr9_ && irqCb_ != IrqCb::None && (attrIrq_[0] | attrIrq_[1]);
    }

    // Advance the SPU2 timeline to T, running the driver's interrupt handlers
    // at each event. While a DMA is in flight or the SPU IRQ is armed the core
    // is stepped on 768-cycle (one-sample) boundaries, so a handler runs at
    // the end of the sample in which its event fired; mix-tick IRQs carry
    // exactly that cycle.
    void runTo(uint64_t T)
    {
        if (T < now_)
            T = now_;
        int sameCycleRounds = 0;
        for (;;)
        {
            uint64_t target = T;
            if (needFine())
            {
                const uint64_t q = (now_ / kCyclesPerSample + 1) * kCyclesPerSample;
                if (q < target)
                    target = q;
            }
            const uint64_t before = now_;
            bool handled = false;
            rrv_spu2_event ev[32];
            size_t n;
            do
            {
                n = rrv_spu2_sync(spu_, target, ev, 32);
                now_ = target;
                for (size_t i = 0; i < n; i++)
                    handled |= handleEvent(ev[i]);
            } while (n == 32);
            if (now_ == before)
            {
                if (!handled || ++sameCycleRounds > 64)
                    break;
            }
            else
                sameCycleRounds = 0;
            if (now_ >= T && !handled)
                break;
        }
    }

    void waitCycles(uint64_t n) { runTo(now_ + n); }

    // Busy-wait on a driver flag set by an interrupt handler, on the SPU2
    // timeline. Returns false if the flag can never be set (nothing in flight):
    // the real driver would spin forever there.
    template <typename Pred>
    bool waitFor(Pred pred)
    {
        while (!pred())
        {
            if (!dmaPending_)
            {
                Counters::inc(ctr_.hangs_avoided);
                return false;
            }
            runTo((now_ / kCyclesPerSample + 1) * kCyclesPerSample);
        }
        return true;
    }

    template <typename Pred>
    void pollReg(uint32_t reg, Pred ok, uint32_t cap)
    {
        for (uint32_t i = 1;; i++)
        {
            if (ok(R(reg)))
                return;
            if (i > cap)
            {
                Counters::inc(ctr_.spin_timeouts);
                return;
            }
            waitCycles(kPollCycles);
        }
    }

    bool handleEvent(const rrv_spu2_event& e)
    {
        if (e.kind == RRV_SPU2_EVENT_DMA7_DONE)
        {
            if (!dmaRest_.empty())
            {
                // Continuation of a transfer longer than the core's 1 MiB
                // staging area: same TSA flow, only the last part interrupts.
                std::vector<uint16_t> rest;
                rest.swap(dmaRest_);
                issueDma(std::move(rest));
                return true;
            }
            dmaPending_ = false;
            if (!handlers_)
                return false;
            ch7Isr();
            return true;
        }
        if (e.kind == RRV_SPU2_EVENT_SPU_IRQ)
        {
            if (!handlers_ || !intr9_)
                return false;
            switch (irqCb_)
            {
                case IrqCb::None: return false;
                case IrqCb::Type6: post(6, false, 0, 0); return true;
                case IrqCb::StreamIrq: streamIrq(); return true;
                case IrqCb::StreamFinish: streamFinish(); return true;
            }
        }
        return false; // DMA4 (core 0 autoDMA) is unused by RR5
    }

    // =======================================================================
    // SPU2 register access (offsets from 0x1F900000)
    // =======================================================================
    static uint32_t C(int c) { return c ? 0x400u : 0u; }
    static uint32_t X(int c) { return c ? 0x28u : 0u; }

    void W(uint32_t off, uint16_t v)
    {
        if (off == 0x19A)
            attrIrq_[0] = v & 0x40;
        else if (off == 0x59A)
            attrIrq_[1] = v & 0x40;
        rrv_spu2_write16(spu_, now_, 0x1F900000u | off, v);
    }
    uint16_t R(uint32_t off) { return rrv_spu2_read16(spu_, now_, 0x1F900000u | off); }

    // FUN_14d2c(..., 1): byte address -> (hi = a>>17, lo = a>>1).
    void addrBytes(uint32_t off, uint32_t a)
    {
        W(off, uint16_t(a >> 17));
        W(off + 2, uint16_t(a >> 1));
    }
    // FUN_14cf8: 8-byte units -> (hi = (v&0x3FFFFFFF)>>14, lo = v<<2).
    void addrUnits8(uint32_t off, uint32_t v)
    {
        W(off, uint16_t((v & 0x3FFFFFFFu) >> 14));
        W(off + 2, uint16_t(v << 2));
    }
    uint32_t eeaBytes(int c) { return (uint32_t(R(0x33C + C(c))) << 17) | 0x1FFFF; }

    void delay() { waitCycles(kDelay60); }

    // =======================================================================
    // IOP window shadow
    // =======================================================================
    bool inWin(uint32_t a) const { return a - cfg_.iop_window_base < cfg_.iop_window_size; }
    uint8_t peek8(uint32_t a)
    {
        if (!inWin(a))
        {
            Counters::inc(ctr_.iop_oob);
            return 0;
        }
        return iop_[a - cfg_.iop_window_base];
    }
    uint16_t peek16(uint32_t a) { return uint16_t(peek8(a) | peek8(a + 1) << 8); }
    uint32_t peek32(uint32_t a) { return uint32_t(peek16(a)) | uint32_t(peek16(a + 2)) << 16; }
    void poke8(uint32_t a, uint8_t v)
    {
        if (!inWin(a))
        {
            Counters::inc(ctr_.iop_oob);
            return;
        }
        iop_[a - cfg_.iop_window_base] = v;
    }
    // Driver-originated IOP write: shadow + record for the caller.
    void drvWrite(uint32_t a, const uint8_t* p, uint32_t n)
    {
        for (uint32_t i = 0; i < n; i++)
            poke8(a + i, p[i]);
        writes_.push_back(IopWrite{now_, a, std::vector<uint8_t>(p, p + n)});
    }
    void drvWrite8(uint32_t a, uint8_t v) { drvWrite(a, &v, 1); }
    void drvWrite16(uint32_t a, uint16_t v)
    {
        const uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)};
        drvWrite(a, b, 2);
    }
    void drvWrite32(uint32_t a, uint32_t v)
    {
        uint8_t b[4];
        st32(b, v);
        drvWrite(a, b, 4);
    }

    // The contiguous driver data the RPC arguments are read from: receive
    // buffer, key buffer, env mirror (IOP memory), then zero.
    uint8_t view8(uint32_t off)
    {
        if (off < kRecvSize)
            return recv_[off];
        off -= kRecvSize;
        if (off < kKeyBufSize)
            return keyBuf_[off];
        off -= kKeyBufSize;
        if (off < kEnvSize)
            return peek8(mirrorAddr() + off);
        return 0;
    }
    uint32_t arg(uint32_t word) { return uint32_t(view8(word * 4)) | uint32_t(view8(word * 4 + 1)) << 8 |
                                         uint32_t(view8(word * 4 + 2)) << 16 | uint32_t(view8(word * 4 + 3)) << 24; }
    void viewCopy(uint32_t off, uint8_t* out, uint32_t n)
    {
        for (uint32_t i = 0; i < n; i++)
            out[i] = view8(off + i);
    }

    void buildReply(bool keysBuf, uint32_t n, std::vector<uint8_t>& out)
    {
        out.assign(n, 0);
        if (!keysBuf)
        {
            // drv+0x16620: reply word, then the 0x7600 count. DEVIATION: the
            // driver globals that follow are returned as zero.
            uint8_t w[8];
            st32(w, ret_);
            st32(w + 4, cnt7600_);
            std::memcpy(out.data(), w, std::min<uint32_t>(n, 8));
            return;
        }
        // drv+0x19940: key buffer, then the env mirror (drv+0x19980).
        for (uint32_t i = 0; i < n; i++)
            out[i] = view8(kRecvSize + i);
    }

    // =======================================================================
    // Mailbox (spec 12). DEVIATION: every event is delivered; the real
    // single-slot mailbox can lose one that arrives before the callback
    // thread has finished the previous RPC.
    // =======================================================================
    void post(uint32_t type, bool args, uint32_t a1, uint32_t a2)
    {
        if (args)
        {
            mbA1_ = a1;
            mbA2_ = a2;
        }
        if (!cbThread_)
        {
            Counters::inc(ctr_.callbacks_dropped);
            return;
        }
        callbacks_.push_back(Callback{now_, type, mbA1_, mbA2_});
        if (type == 2)
            xferUndelivered_++;
        Counters::inc(ctr_.callbacks);
    }

    // =======================================================================
    // Transfer engine (spec 9)
    // =======================================================================
    void setTsa(uint32_t word) // FUN_148d0(2)
    {
        W(0x5AA, uint16_t(word));
        W(0x5A8, uint16_t(word >> 16));
    }
    void dmaModeAttr() { W(0x59A, uint16_t((R(0x59A) & 0xFFCF) | 0x20)); } // FUN_148d0(1)

    void issueDma(std::vector<uint16_t> hw)
    {
        constexpr size_t kMax = 0x80000; // library staging limit (halfwords)
        if (hw.size() > kMax)
        {
            dmaRest_.assign(hw.begin() + kMax, hw.end());
            hw.resize(kMax);
        }
        dmaPending_ = true;
        rrv_spu2_dma_write(spu_, now_, 1, RRV_SPU2_KEEP_TSA, hw.data(), uint32_t(hw.size()));
    }

    // FUN_148d0(3, src, bytes): the SPU receives ceil(bytes/64)*64 bytes.
    void dmaFromIop(uint32_t src, uint32_t bytes)
    {
        const uint32_t n = ((bytes >> 6) + ((bytes & 0x3F) != 0)) * 64;
        std::vector<uint16_t> hw(n / 2);
        for (uint32_t i = 0; i < n / 2; i++)
            hw[i] = peek16(src + 2 * i);
        Counters::inc(ctr_.dma_transfers);
        issueDma(std::move(hw));
    }
    void dmaZeros(uint32_t bytes)
    {
        const uint32_t n = ((bytes >> 6) + ((bytes & 0x3F) != 0)) * 64;
        Counters::inc(ctr_.dma_transfers);
        issueDma(std::vector<uint16_t>(n / 2, 0));
    }

    // IOP intr 0x28 handler (FUN_14534).
    void ch7Isr()
    {
        (void)R(0x744); // STATX DREQ wait: PCSX2 sets bit 7 at DMA7 completion
        W(0x59A, uint16_t(R(0x59A) & 0xFFCF));
        switch (dmaCb_)
        {
            case DmaCb::None: dmaDone_ = true; break;
            case DmaCb::Type5: post(5, false, 0, 0); break;
            case DmaCb::PrepDone: prepDone(); break;
            case DmaCb::HalfDone: halfDone(); break;
        }
    }

    // SsVabTransCompleted / FUN_10fa0.
    uint32_t transCompleted(int flag)
    {
        if (transferMode_ != 1 && !idle_)
        {
            const uint32_t r = dmaDone_ ? 1 : 0;
            if (flag == 1)
            {
                if (!dmaDone_)
                    waitFor([this] { return dmaDone_; }); // DEVIATION if it would hang: return 1
                dmaDone_ = false;
                idle_ = true;
                return 1;
            }
            if (r == 1)
            {
                dmaDone_ = false;
                idle_ = true;
            }
            return r;
        }
        return 1;
    }

    // =======================================================================
    // SpuInit / SsInit (spec 3)
    // =======================================================================
    void spuInit()
    {
        core_ = 0;
        // SPDIF (SSBUS/DPCR setup is IOP-side and not modelled).
        W(0x7C6, 0x0900);
        W(0x7C8, 0x0200);
        W(0x7CA, 0x0008);
        // reset(0)
        W(0x760, 0);
        W(0x762, 0);
        W(0xB60, 0); // outside the register map; PCSX2 decides what it hits
        W(0xB62, 0);
        W(0x7C0, 0);
        delay();
        delay();
        W(0x7C0, 0x8000);
        delay();
        for (int c = 0; c < 2; c++)
        {
            W(0x1B0 + C(c), 0);
            W(0x19A + C(c), 0);
            delay();
            delay();
            W(0x19A + C(c), 0x8000);
            W(0x760 + X(c), 0);
            W(0x762 + X(c), 0);
            pollReg(0x344 + C(c), [](uint16_t v) { return (v & 0x7FF) == 0; }, kPollCap);
            W(0x764 + X(c), 0);
            W(0x766 + X(c), 0);
            W(0x1A4 + C(c), 0xFFFF);
            W(0x1A6 + C(c), 0xFFFF);
            W(0x19A, uint16_t(R(0x19A) & 0xFF7F)); // always core 0
        }
        for (int c = 0; c < 2; c++)
        {
            W(0x180 + C(c), 0);
            W(0x182 + C(c), 0);
            W(0x184 + C(c), 0);
            W(0x186 + C(c), 0);
            W(0x790 + X(c), 0);
            W(0x792 + X(c), 0);
            W(0x794 + X(c), 0);
            W(0x796 + X(c), 0);
        }
        // Voice and dummy-block init.
        W(0x1A8, 0);
        W(0x1AA, 0x2800);
        for (int i = 0; i < 8; i++)
            W(0x1AC, 0x0707);
        for (int i = 0; i < 8; i++)
            W(0x1AC, 0x0000);
        W(0x19A, uint16_t(R(0x19A) & 0xFFCF));
        // Uncapped in the driver; DEVIATION: capped at 2^20 polls (counted).
        pollReg(0x344, [](uint16_t v) { return (v & 0x400) == 0; }, 1u << 20);
        for (int v = 0; v < 24; v++)
        {
            const uint32_t r1 = 0x400 + 0x10 * v, r0 = 0x10 * v;
            const uint16_t vals[5] = {0, 0, 0x3FFF, 0, 0};
            for (int k = 0; k < 5; k++)
            {
                W(r1 + 2 * k, vals[k]);
                W(r0 + 2 * k, R(r1 + 2 * k));
            }
            const uint32_t a1 = 0x5C0 + 12 * v, a0 = 0x1C0 + 12 * v;
            W(a1, 0);
            W(a0, R(a1));
            W(a1 + 2, 0x2800);
            W(a0 + 2, R(a1 + 2));
        }
        W(0x5A0, 0xFFFF);
        W(0x1A0, 0xFFFF);
        W(0x5A2, 0xFF);
        W(0x1A2, 0xFF);
        waitCycles(kSpinC34);
        W(0x5A4, 0xFFFF);
        W(0x1A4, 0xFFFF);
        W(0x5A6, 0xFF);
        W(0x1A6, 0xFF);
        waitCycles(kSpinC34);
        W(0x342, 0);
        W(0x340, 0);
        idle_ = true;
        dmaCb_ = DmaCb::None;
        irqCb_ = IrqCb::None;
        // Sample-note table of the current core (0).
        for (auto& sn : sampleNote_[core_])
            sn = 0xC000;
        // Interrupts.
        quitArmed_ = true;
        dmaDone_ = false;
        handlers_ = true;
        // Reverb globals; rev_offset from the EEA register *before* the defaults.
        revOn_ = 0;
        revMode_ = 0;
        revDepthL_ = revDepthR_ = 0;
        revDelay_ = revFeedback_ = 0;
        revOffset_ = eeaBytes(core_) - (kRevSize[0] * 8 - 2);
        addrBytes(0x2E0 + C(core_), revOffset_);
        keyMask_[0] = keyMask_[1] = 0;
        transferMode_ = 0;
        // Defaults (FUN_13b8c), exact order.
        W(0x7C0, 0xC032);
        W(0x19A, 0xC000);
        W(0x59A, 0xC001);
        const uint32_t mix[8] = {0x188, 0x18A, 0x190, 0x192, 0x18C, 0x18E, 0x194, 0x196};
        for (int c = 0; c < 2; c++)
            for (int i = 0; i < 8; i++)
                W(mix[i] + C(c), (i & 1) ? 0xFF : 0xFFFF);
        W(0x198, 0xFFF);
        W(0x598, 0xFFF);
        for (uint32_t r : {0x760u, 0x762u, 0x788u, 0x78Au, 0x764u, 0x766u, 0x78Cu, 0x78Eu, 0x768u, 0x76Au})
            W(r, 0);
        W(0x790, 0x7FFF);
        W(0x792, 0x7FFF);
        for (uint32_t r : {0x76Cu, 0x76Eu, 0x794u, 0x796u})
            W(r, 0);
        W(0x33C, 0xE);
        W(0x73C, 0xF);
    }

    void ssInit()
    {
        spuInit();
        W(0x760, 0x3FFF);
        W(0x762, 0x3FFF);
        for (uint32_t r = 0x764; r <= 0x77E; r += 2)
            W(r, 0);
        idle_ = true;
        mallocInit(kMallocN);
        std::memset(vabState_, 0, sizeof(vabState_));
        vabCount_ = 0;
        for (int v = 0; v < 24; v++)
        {
            uint8_t s[64] = {};
            st32(s + 0, 1u << v);
            st32(s + 4, 0x60093);
            s[0x14] = 0x00; s[0x15] = 0x10;           // pitch 0x1000
            st32(s + 0x1C, 0x5000);                   // SSA
            s[0x3A] = 0xFF; s[0x3B] = 0x80;           // ADSR1 0x80FF
            s[0x3C] = 0x00; s[0x3D] = 0x40;           // ADSR2 0x4000
            setVoiceAttr(s);
        }
        // SsVm flush: key-off every voice, key-on none, EON 0, NON 0.
        setKey(0, 0xFFFFFF);
        setKey(1, 0);
        reverbVoiceMask(8, 0, 0x18C, 0x18E);
        reverbVoiceMask(8, 0, 0x194, 0x196);
        reverbVoiceMask(8, 0, 0x184, 0x186);
        maxProgs_ = 0x80;
    }

    void spuQuit()
    {
        if (!quitArmed_)
            return;
        quitArmed_ = false;
        dmaCb_ = DmaCb::None;
        irqCb_ = IrqCb::None;
        handlers_ = false;
        intr9_ = false;
    }

    // =======================================================================
    // Attributes (spec 4, 5), keys (spec 6)
    // =======================================================================
    static uint16_t modeBits(uint16_t mode)
    {
        const int16_t k = int16_t(uint16_t(mode - 1));
        return (k >= 0 && k <= 6) ? uint16_t(0x8000 + 0x1000 * k) : 0;
    }
    static uint16_t volValue(uint16_t raw, uint16_t mb)
    {
        uint16_t v = raw & 0x7FFF;
        if (mb)
        {
            const int16_t s = int16_t(raw);
            v = s >= 0x80 ? 0x7F : (s < 0 ? 0 : raw);
        }
        return uint16_t((v & 0x7FFF) | mb);
    }

    void setVoiceAttr(const uint8_t* s)
    {
        const uint32_t voices = ld32(s), mask = ld32(s + 4);
        const bool all = mask == 0;
        auto has = [&](uint32_t b) { return all || (mask & b); };
        const int c = core_;
        for (int v = 0; v < 24; v++)
        {
            if (!((voices >> v) & 1))
                continue;
            const uint32_t r = C(c) + 0x10 * v;
            if (has(0x10))
                W(r + 4, ld16(s + 0x14));
            if (has(0x40))
                sampleNote_[c][v] = ld16(s + 0x18);
            if (has(0x20))
            {
                const uint16_t sn = sampleNote_[c][v], n = ld16(s + 0x16);
                W(r + 4, note2pitch(sn >> 8, sn & 0xFF, n >> 8, n & 0xFF));
            }
            if (has(0x01))
                W(r + 0, volValue(ld16(s + 0x08), has(0x04) ? modeBits(ld16(s + 0x0C)) : 0));
            if (has(0x02))
                W(r + 2, volValue(ld16(s + 0x0A), has(0x08) ? modeBits(ld16(s + 0x0E)) : 0));
            if (has(0x80))
                addrBytes(0x1C0 + C(c) + 12 * v, ld32(s + 0x1C) & ~0xFu);
            if (has(0x10000))
                addrBytes(0x1C4 + C(c) + 12 * v, ld32(s + 0x20) & ~0xFu);
            if (has(0x20000))
                W(r + 6, ld16(s + 0x3A));
            if (has(0x40000))
                W(r + 8, ld16(s + 0x3C));
            if (has(0x800))
            {
                const uint16_t ar = std::min<uint16_t>(ld16(s + 0x30), 0x7F);
                const uint16_t x = (has(0x100) && ld32(s + 0x24) == 5) ? 0x80 : 0;
                W(r + 6, uint16_t((R(r + 6) & 0x00FF) | ((ar | x) << 8)));
            }
            if (has(0x1000))
            {
                const uint16_t dr = std::min<uint16_t>(ld16(s + 0x32), 0xF);
                W(r + 6, uint16_t((R(r + 6) & 0xFF0F) | (dr << 4)));
            }
            if (has(0x2000))
            {
                const uint16_t sr = std::min<uint16_t>(ld16(s + 0x34), 0x7F);
                uint16_t m = 0x100;
                if (has(0x200))
                {
                    const uint32_t sm = ld32(s + 0x28);
                    if (sm == 5) m = 0x200;
                    else if (sm == 1) m = 0;
                    else if (sm == 7) m = 0x300;
                }
                W(r + 8, uint16_t((R(r + 8) & 0x003F) | ((sr | m) << 6)));
            }
            if (has(0x4000))
            {
                const uint16_t rr = std::min<uint16_t>(ld16(s + 0x36), 0x1F);
                const uint16_t y = (has(0x400) && ld32(s + 0x2C) == 7) ? 0x20 : 0;
                W(r + 8, uint16_t((R(r + 8) & 0xFFC0) | rr | y));
            }
            if (has(0x8000))
            {
                const uint16_t sl = std::min<uint16_t>(ld16(s + 0x38), 0xF);
                W(r + 6, uint16_t((R(r + 6) & 0xFFF0) | sl));
            }
        }
        // DEVIATION: the trailing 2-iteration empty loop (~26 cycles) is not timed.
    }

    void setCommonAttr(const uint8_t* s)
    {
        const uint32_t mask = ld32(s);
        const bool all = mask == 0;
        auto has = [&](uint32_t b) { return all || (mask & b); };
        const int c = core_;
        if (has(1))
            W(0x760 + X(c), volValue(ld16(s + 4), has(4) ? modeBits(ld16(s + 8)) : 0));
        if (has(2))
            W(0x762 + X(c), volValue(ld16(s + 6), has(8) ? modeBits(ld16(s + 0xA)) : 0));
        if (has(0x40)) W(0x790 + X(c), ld16(s + 0x10));
        if (has(0x80)) W(0x792 + X(c), ld16(s + 0x12));
        if (has(0x400)) W(0x794 + X(c), ld16(s + 0x1C));
        if (has(0x800)) W(0x796 + X(c), ld16(s + 0x1E));
        auto attrBit = [&](uint32_t off, uint16_t bit) {
            const uint16_t a = R(0x19A + C(c));
            W(0x19A + C(c), ld32(s + off) ? uint16_t(a | bit) : uint16_t(a & ~bit));
        };
        if (has(0x100)) attrBit(0x14, 4);
        if (has(0x200)) attrBit(0x18, 1);
        if (has(0x1000)) attrBit(0x20, 8);
        if (has(0x2000)) attrBit(0x24, 2);
    }

    void setKey(uint32_t on, uint32_t bits)
    {
        bits &= 0xFFFFFF;
        const int c = core_;
        if (on == 1)
        {
            W(0x1A0 + C(c), uint16_t(bits));
            W(0x1A2 + C(c), uint16_t(bits >> 16));
            keyMask_[c] |= bits;
        }
        else if (on == 0)
        {
            W(0x1A4 + C(c), uint16_t(bits));
            W(0x1A6 + C(c), uint16_t(bits >> 16));
            keyMask_[c] &= ~bits;
        }
    }

    void keyStatus(uint8_t* out)
    {
        const int c = core_;
        for (int v = 0; v < 24; v++)
        {
            const int16_t envx = int16_t(R(C(c) + 0x10 * v + 0xA));
            const bool on = (keyMask_[c] >> v) & 1;
            out[v] = on ? (envx ? 1 : 3) : (envx ? 2 : 0);
        }
    }

    // FUN_157c0: read-modify-write of a 24-bit voice mask (lo, hi&0xFF).
    uint32_t reverbVoiceMask(uint32_t on, uint32_t bits, uint32_t lo, uint32_t hi)
    {
        const int c = core_;
        const uint16_t h = R(hi + C(c)), l = R(lo + C(c));
        uint32_t cur = uint32_t(l) | uint32_t(h & 0xFF) << 16;
        if (on == 1)
        {
            W(lo + C(c), uint16_t(R(lo + C(c)) | uint16_t(bits)));
            W(hi + C(c), uint16_t(R(hi + C(c)) | ((bits >> 16) & 0xFF)));
            cur |= bits & 0xFFFFFF;
        }
        else if (on == 0)
        {
            W(lo + C(c), uint16_t(R(lo + C(c)) & ~uint16_t(bits)));
            W(hi + C(c), uint16_t(R(hi + C(c)) & ~((bits >> 16) & 0xFF)));
            cur &= ~(bits & 0xFFFFFF);
        }
        else if (on == 8)
        {
            W(lo + C(c), uint16_t(bits));
            W(hi + C(c), uint16_t((bits >> 16) & 0xFF));
            cur = bits & 0xFFFFFF;
        }
        return cur;
    }

    // =======================================================================
    // Reverb (spec 7)
    // =======================================================================
    uint32_t setReverb(uint32_t on)
    {
        const uint32_t a = 0x19A + C(core_);
        if (on == 0)
        {
            revOn_ = 0;
            W(a, uint16_t(R(a) & 0xFF7F));
        }
        else if (on == 1)
        {
            W(a, uint16_t(R(a) | 0x80));
            revOn_ = 1;
        }
        return revOn_;
    }

    struct Preset
    {
        uint32_t mask;
        int16_t f[32];
    };
    static Preset preset(uint32_t m)
    {
        Preset p{0, {}};
        for (int i = 0; i < 32; i++)
            p.f[i] = int16_t(kRevPreset[m][i]);
        return p;
    }

    void writePreset(const Preset& p)
    {
        const int c = core_;
        for (uint32_t n = 0; n < 32; n++)
        {
            if (p.mask != 0 && !((p.mask >> n) & 1))
                continue;
            if (n < 2)
                addrUnits8(0x2E4 + 4 * n + C(c), uint32_t(int32_t(p.f[n])));
            else if (n < 10)
                W(0x774 + 2 * (n - 2) + X(c), uint16_t(p.f[n]));
            else if (n < 30)
                addrUnits8(0x2EC + 4 * (n - 10) + C(c), uint32_t(int32_t(p.f[n])));
            else
                W(0x784 + 2 * (n - 30) + X(c), uint16_t(p.f[n]));
        }
    }

    // FUN_10400: zero the reverb work area with ch7 DMAs, busy-waiting each.
    void clearWorkArea(uint32_t mode)
    {
        if (mode >= 10)
            return;
        uint32_t size, word;
        if (mode == 0)
        {
            size = 0x20;
            word = 0x1FFFE0;
        }
        else
        {
            size = kRevSize[mode] * 8;
            word = (eeaBytes(core_) - size) >> 1;
        }
        const DmaCb saved = dmaCb_;
        dmaCb_ = DmaCb::None;
        for (bool more = true; more;)
        {
            uint32_t chunk = 0x400;
            if (size < 0x401)
            {
                more = false;
                chunk = size;
            }
            dmaDone_ = false;
            setTsa(word);
            dmaModeAttr();
            dmaZeros(chunk);
            waitFor([this] { return dmaDone_; });
            dmaDone_ = false;
            size -= 0x400;
            word += 0x200;
        }
        if (saved != DmaCb::None)
            dmaCb_ = saved;
    }

    uint32_t setReverbModeParam(const uint8_t* s)
    {
        const uint32_t mask = ld32(s);
        const bool all = mask == 0;
        bool modeSet = false, delaySet = false, fbSet = false, clearWA = false;
        Preset loc{0, {}};
        if (all || (mask & 1))
        {
            uint32_t m = ld32(s + 4);
            clearWA = (m & 0x100) != 0;
            if (clearWA)
                m &= ~0x100u;
            modeSet = true;
            if (m > 9)
                return 0xFFFFFFFFu;
            revMode_ = m;
            revOffset_ = eeaBytes(core_) - (kRevSize[m] * 8 - 2);
            loc = preset(m);
            revDelay_ = (m == 7 || m == 8) ? 0x7F : 0;
            revFeedback_ = (m == 7) ? 0x7F : 0;
        }
        const bool delayMode = revMode_ == 7 || revMode_ == 8;
        if ((all || (mask & 8)) && delayMode)
        {
            delaySet = true;
            if (!modeSet)
            {
                loc = preset(revMode_);
                loc.mask = 0x0C011C00;
            }
            revDelay_ = int32_t(ld32(s + 0xC));
            const int16_t t = int16_t((revDelay_ * 0x1000) / 0x7F);
            loc.f[10] = int16_t(int16_t((revDelay_ * 0x2000) / 0x7F) - loc.f[0]);
            loc.f[11] = int16_t(t - loc.f[1]);
            loc.f[16] = int16_t(loc.f[17] + t);
            loc.f[12] = int16_t(loc.f[13] + t);
            loc.f[27] = int16_t(loc.f[29] + t);
            loc.f[26] = int16_t(loc.f[28] + t);
        }
        if ((all || (mask & 0x10)) && delayMode)
        {
            fbSet = true;
            if (!modeSet)
            {
                if (delaySet)
                    loc.mask |= 0x80;
                else
                {
                    loc = preset(revMode_);
                    loc.mask = 0x80;
                }
            }
            revFeedback_ = int32_t(ld32(s + 0x10));
            loc.f[7] = int16_t((revFeedback_ * 0x8100) / 0x7F);
        }
        bool wasOn = false;
        const int c = core_;
        if (modeSet)
        {
            wasOn = (R(0x19A + C(c)) >> 7) & 1;
            if (wasOn)
                W(0x19A + C(c), uint16_t(R(0x19A + C(c)) & 0xFF7F));
            W(0x764 + X(c), 0);
            W(0x766 + X(c), 0);
            revDepthL_ = revDepthR_ = 0;
        }
        else
        {
            if (all || (mask & 2))
            {
                revDepthL_ = ld16(s + 8);
                W(0x764 + X(c), revDepthL_);
            }
            if (all || (mask & 4))
            {
                revDepthR_ = ld16(s + 0xA);
                W(0x766 + X(c), revDepthR_);
            }
        }
        if (modeSet || delaySet || fbSet)
            writePreset(loc);
        if (clearWA)
            clearWorkArea(revMode_);
        if (modeSet)
        {
            addrBytes(0x2E0 + C(c), revOffset_);
            if (wasOn)
                W(0x19A + C(c), uint16_t(R(0x19A + C(c)) | 0x80));
        }
        return 0;
    }

    // =======================================================================
    // SpuMalloc / SpuFree (spec 8)
    // =======================================================================
    void mallocInit(int n)
    {
        if (n < 1)
            return;
        mLast_ = 0;
        mN_ = n;
        mt_[0] = {0x40005010u, 0x1FAFF0u};
    }

    void mallocGc()
    {
        const int last = mLast_;
        // 1. coalesce adjacent FREE blocks
        for (int i = 0; i <= last;)
        {
            if (!(mt_[i].w0 & kFree))
            {
                i++;
                continue;
            }
            int j = i + 1;
            while (j <= int(kMallocN) && mt_[j].w0 == kInvalid)
                j++;
            if (j <= int(kMallocN) && (mt_[j].w0 & kFree) &&
                (mt_[j].w0 & kAddrMask) == (mt_[i].w0 & kAddrMask) + mt_[i].size)
            {
                mt_[j].w0 = kInvalid;
                mt_[i].size += mt_[j].size;
                continue; // retry i
            }
            i++;
        }
        // 2. size 0 -> INVALID
        for (int i = 0; i <= mLast_; i++)
            if (mt_[i].size == 0)
                mt_[i].w0 = kInvalid;
        // 3. sort the entries before the TAIL by address
        for (int i = 0; i <= mLast_; i++)
        {
            if (mt_[i].w0 & kTail)
                break;
            const int lim = mLast_;
            for (int j = i + 1; j <= lim; j++)
            {
                if (mt_[j].w0 & kTail)
                    break;
                if ((mt_[j].w0 & kAddrMask) < (mt_[i].w0 & kAddrMask))
                    std::swap(mt_[i], mt_[j]);
            }
        }
        // 4. first INVALID before the TAIL takes the TAIL
        for (int i = 0; i <= mLast_; i++)
        {
            if (mt_[i].w0 & kTail)
                break;
            if (mt_[i].w0 == kInvalid)
            {
                mt_[i] = mt_[mLast_];
                mLast_ = i;
                break;
            }
        }
        // 5. trailing FREE entries merge into the TAIL
        for (int k = mLast_ - 1; k >= 0; k--)
        {
            if (!(mt_[k].w0 & kFree))
                return;
            mt_[k].w0 = (mt_[k].w0 & kAddrMask) | kTail;
            mt_[k].size += mt_[mLast_].size;
            mLast_ = k;
        }
    }

    uint32_t spuMalloc(int32_t size)
    {
        if (mN_ == 0)
        {
            // DEVIATION: before SsInit the real table pointer is NULL.
            Counters::inc(ctr_.unknown_commands);
            return 0xFFFFFFFFu;
        }
        const uint32_t resv = 0; // reverb reserve (0x0100/0x0018 family) is never set by RR5
        const uint32_t req = uint32_t((size >> 1) * 2);
        int i;
        if (mt_[0].w0 & kTail)
            i = 0;
        else
        {
            mallocGc();
            i = -1;
            for (int k = 0; k < mN_; k++)
                if ((mt_[k].w0 & kTail) || ((mt_[k].w0 & kFree) && req <= mt_[k].size))
                {
                    i = k;
                    break;
                }
            if (i < 0)
                return 0xFFFFFFFFu;
        }
        if (!(mt_[i].w0 & kTail))
        {
            const uint32_t sz = mt_[i].size;
            if (req < sz && mLast_ < mN_)
            {
                const MEnt t = mt_[mLast_];
                mt_[mLast_] = {(mt_[i].w0 + req) | kFree, sz - req};
                mLast_++;
                mt_[mLast_] = t;
            }
            mt_[i].size = req;
            mt_[i].w0 &= kAddrMask;
            mallocGc();
            return mt_[i].w0;
        }
        if (i < mN_ && req <= mt_[i].size - resv)
        {
            mLast_ = i + 1;
            mt_[i + 1] = {((mt_[i].w0 & kAddrMask) + req) | kTail, mt_[i].size - req};
            mt_[i].size = req;
            mt_[i].w0 &= kAddrMask;
            mallocGc();
            return mt_[i].w0;
        }
        return 0xFFFFFFFFu;
    }

    void spuFree(uint32_t addr)
    {
        for (int i = 0; i < mN_; i++)
        {
            if (mt_[i].w0 & kTail)
                break;
            if (mt_[i].w0 == addr)
            {
                mt_[i].w0 = addr | kFree;
                break;
            }
        }
        mallocGc();
    }

    // =======================================================================
    // SsVab (spec 10)
    // =======================================================================
    int32_t vabOpenHead(uint32_t hdr, int16_t id)
    {
        if (!idle_)
            return -1;
        idle_ = false;
        int slot = -1;
        if (id == -1)
        {
            for (int k = 0; k < 16; k++)
                if (vabState_[k] == 0)
                {
                    slot = k;
                    break;
                }
        }
        else if (id >= 0 && id < 16 && vabState_[id] == 0)
            slot = id;
        if (slot < 0)
        {
            idle_ = true;
            return -1;
        }
        vabState_[slot] = 1;
        vabCount_++;
        auto fail = [&] {
            vabState_[slot] = 0;
            idle_ = true;
            vabCount_--;
            return -1;
        };
        const uint32_t magic = peek32(hdr);
        if ((magic >> 8) != 0x564142u)
            return fail();
        const int32_t version = int32_t(peek32(hdr + 4));
        maxProgs_ = ((magic & 0xFF) == 0x70 && version > 4) ? 0x80 : 0x40;
        const uint32_t ps = peek16(hdr + 0x12);
        if (ps > maxProgs_)
            return fail();
        const uint32_t prog = hdr + 0x20;
        vabProg_[slot] = prog;
        uint32_t running = 0;
        for (uint32_t k = 0; k < maxProgs_; k++)
        {
            drvWrite32(prog + 16 * k + 8, running);
            if (peek8(prog + 16 * k) != 0)
                running++;
        }
        const uint32_t vs = peek8(hdr + 0x16);
        const uint32_t sizes = prog + maxProgs_ * 16 + ps * 512;
        uint32_t sz[256];
        uint32_t total = 0;
        for (uint32_t k = 0; k <= vs; k++)
        {
            sz[k] = uint32_t(peek16(sizes + 2 * k)) << (version > 4 ? 3 : 2);
            total += sz[k];
        }
        const uint32_t alloc = (total + 0x3F) & ~0x3Fu;
        const uint32_t base = spuMalloc(int32_t(alloc));
        if (base == 0xFFFFFFFFu)
            return fail();
        vabHi_[slot] = base >= 0x100000 ? 1 : 0;
        if (base + alloc >= 0x1FAFF1)
            return fail(); // the SPU block leaks, as in the driver
        vabBase_[slot] = base;
        uint32_t cum = 0;
        for (uint32_t k = 0; k <= vs; k++)
        {
            cum += sz[k];
            drvWrite16(prog + (k / 2) * 16 + ((k & 1) ? 14 : 12), uint16_t((base + cum) >> 4));
        }
        vabTotal_[slot] = total;
        vabState_[slot] = 2;
        return slot;
    }

    void vabTransBody(uint32_t body, int16_t id)
    {
        const uint16_t uid = uint16_t(id);
        if (uid < 0x11 && uid < 16 && vabState_[uid] == 2)
        {
            transferMode_ = 0;
            const uint32_t base = vabBase_[uid];
            if (base - 0x5010u < 0x1FAFE9u)
            {
                tsaWord_ = base >> 1;
                // SpuWrite
                const uint32_t n = std::min<uint32_t>(vabTotal_[uid], 0x1FAFF0);
                setTsa(tsaWord_);
                dmaModeAttr();
                dmaFromIop(body, n);
                if (dmaCb_ == DmaCb::None)
                    idle_ = false;
                vabState_[uid] = 1;
                return;
            }
        }
        idle_ = true;
    }

    uint32_t vabGetVagAddr(int16_t vab, uint16_t vag)
    {
        if (uint16_t(vab) >= 16 || vabState_[vab] != 1 || !(0 < int32_t(maxProgs_)))
            return 0xFFFFFFFFu;
        const int32_t i = (int32_t(int16_t(vag)) - 1) / 2;
        const uint32_t off = (vag & 1) ? 12 : 14;
        const uint32_t u = peek16(vabProg_[vab] + uint32_t(i * 16) + off);
        return (u << 4) | (uint32_t(vabHi_[vab]) << 20);
    }

    void vabClose(int16_t id)
    {
        const uint16_t uid = uint16_t(id);
        if (uid < 16 && vabState_[uid] != 0 && vabState_[uid] < 3)
        {
            spuFree(vabBase_[uid]);
            vabState_[uid] = 0;
            vabCount_--;
            if (!idle_)
                idle_ = true;
        }
    }

    // =======================================================================
    // SpuSt streaming (spec 11)
    // =======================================================================
    uint8_t* envV(int v) { return env_ + 8 + 16 * v; }
    uint8_t envStatus(int v) { return v < 24 ? envV(v)[0] : 0; }
    int32_t envLast(int v) { return v < 24 ? int32_t(ld32(envV(v) + 4)) : 0; }
    uint32_t envBuf(int v) { return v < 24 ? ld32(envV(v) + 8) : 0; }
    uint32_t envData(int v) { return v < 24 ? ld32(envV(v) + 12) : 0; }

    void copyMirrorToEnv()
    {
        for (uint32_t i = 0; i < kEnvSize; i++)
            env_[i] = peek8(mirrorAddr() + i);
    }

    void stReset()
    {
        A_ = P_ = SQ_ = PQ_ = PR_ = PD_ = F_ = Fp_ = 0;
        cbPrep_ = cbXfer_ = cbFin_ = false;
        savedDmaCb_ = DmaCb::None;
        savedIrqCb_ = IrqCb::None;
        cur_ = curPrep_ = firstActive_ = firstFinish_ = kNoVoice;
        std::memset(env_, 0, sizeof(env_));
        for (int v = 0; v < 24; v++)
            envV(v)[0] = 6;
        half_ = 0;
        H_ = 0;
    }

    void patchH0(int v)
    {
        const uint32_t d = envData(v);
        drvWrite8(d + 1, 6);
        drvWrite8(d + 0x11, 2);
        drvWrite8(d + 1 + uint32_t(half_ - 0x10), 2);
    }
    void patchH1(int v)
    {
        const uint32_t d = envData(v);
        drvWrite8(d + 1, 2);
        drvWrite8(d + 0x11, 2);
        drvWrite8(d + 1 + uint32_t(half_ - 0x10), 3);
    }
    uint32_t endBlock(int v) { return envData(v) + uint32_t(envLast(v)) - 0x10; }
    void saveAndSilence(int v)
    {
        const uint32_t p = endBlock(v);
        for (uint32_t i = 0; i < 16; i++)
            saved16_[v][i] = peek8(p + i);
        uint8_t silent[16] = {0x00, 0x07};
        drvWrite(p, silent, 16);
    }
    void restoreEnd(int v) { drvWrite(endBlock(v), saved16_[v], 16); }

    void withStreamCore(void (Engine::*fn)(uint32_t), uint32_t a)
    {
        const int old = core_;
        core_ = int(streamCore_);
        (this->*fn)(a);
        core_ = old;
    }
    void spuSetIrq(uint32_t on) // FUN_10620 on the current core
    {
        const uint32_t a = 0x19A + C(core_);
        if (on == 0 || on == 3)
        {
            W(a, uint16_t(R(a) & 0xFFBF));
            intr9_ = false;
        }
        if (on == 1 || on == 3)
        {
            W(a, uint16_t(R(a) | 0x40));
            intr9_ = true;
        }
    }
    void setIrqAddr(uint32_t a) // FUN_107d0 on the current core
    {
        if (a < 0x1FFFF9)
            addrBytes(0x19C + C(core_), a);
    }
    void setLsax(int v, uint32_t a) // on the stream core
    {
        const int old = core_;
        core_ = int(streamCore_);
        addrBytes(0x1C4 + C(core_) + 12 * v, a);
        core_ = old;
    }
    void setKeyOnStreamCore(uint32_t bits)
    {
        const int old = core_;
        core_ = int(streamCore_);
        setKey(1, bits);
        core_ = old;
    }

    // FUN_11bd8: PREPARE transfer of voice v (first half, no end handling).
    void prepXfer(int v)
    {
        setTsa((envBuf(v) >> 4) << 3);
        patchH0(v);
        dmaModeAttr();
    }

    // FUN_11f08: round transfer of voice v.
    void roundXfer(int v)
    {
        uint32_t dst = envBuf(v) & ~0xFu;
        if (H_ == 0)
            patchH0(v);
        else
        {
            dst += uint32_t(half_);
            patchH1(v);
        }
        setTsa(uint32_t(int32_t(dst) >> 1));
        if (envStatus(v) == 2)
        {
            const uint32_t bit = 1u << v;
            A_ &= ~bit;
            F_ |= bit;
            saveAndSilence(v);
            if (A_ == 0)
            {
                firstActive_ = kNoVoice;
                firstFinish_ = firstBit(F_);
            }
            else
                firstActive_ = firstBit(A_);
        }
        dmaModeAttr();
    }

    uint32_t stPrepare(uint32_t bits)
    {
        const int v0 = firstBit(bits);
        const uint32_t s = S_ & 0xF0;
        if (s == 0x10)
        {
            S_ = 0x20;
            half_ = int32_t(ld32(env_)) >> 1;
            PQ_ = PR_ = bits;
            savedDmaCb_ = dmaCb_;
            dmaCb_ = DmaCb::PrepDone;
            H_ = 0;
            curPrep_ = v0;
            prepXfer(v0);
            S_ = 0x21;
            dmaFromIop(envData(v0), uint32_t(half_));
            return 1;
        }
        if (s == 0x30)
        {
            PQ_ = PR_ = bits;
            curPrep_ = v0;
            return 1;
        }
        return 0xFFFFFFFDu;
    }

    void prepDone() // FUN_11c38
    {
        PQ_ &= ~(1u << (curPrep_ & 31));
        if (PQ_ == 0)
        {
            dmaCb_ = savedDmaCb_;
            S_ = 0x22;
            if (cbPrep_)
            {
                post(1, true, PR_, 4);
                PR_ = 0;
            }
            return;
        }
        curPrep_ = nextBit(PQ_, curPrep_);
        prepXfer(curPrep_);
        dmaFromIop(envData(curPrep_), uint32_t(half_));
    }

    uint32_t stStart(uint32_t bits)
    {
        const uint32_t s = S_ & 0xF0;
        if (s == 0x20)
        {
            if (S_ != 0x22)
                return 0xFFFFFFFDu;
            S_ = 0x30;
            const int v0 = firstBit(bits);
            setKeyOnStreamCore(bits);
            Fp_ = F_ = 0;
            A_ = P_ = bits;
            savedDmaCb_ = dmaCb_;
            dmaCb_ = DmaCb::HalfDone;
            savedIrqCb_ = irqCb_;
            irqCb_ = IrqCb::StreamIrq;
            H_ = 1;
            firstActive_ = cur_ = v0;
            roundXfer(v0);
            withStreamCore(&Engine::spuSetIrq, 0);
            S_ = 0x31;
            dmaFromIop(envData(v0), uint32_t(half_));
            return 1;
        }
        if (s == 0x30)
        {
            SQ_ = bits;
            return 1;
        }
        return 0xFFFFFFFDu;
    }

    uint32_t stTransfer(uint32_t flag, uint32_t bits)
    {
        if (S_ == 0)
            return 0;
        if ((bits & 0xFFFFFF) == 0)
            return 0xFFFFFFFEu;
        if (flag == 4)
            return stPrepare(bits);
        if (flag == 5 || flag == 6)
            return stStart(bits);
        return 0xFFFFFFFEu;
    }

    void halfDone() // FUN_125f8
    {
        const int v = cur_;
        P_ &= ~(1u << (v & 31));
        if (envStatus(v) == 2)
        {
            restoreEnd(v);
            envV(v)[0] = 6;
        }
        setLsax(v, envBuf(v) & ~0xFu);
        if (P_ != 0)
        {
            cur_ = nextBit(P_, cur_);
            roundXfer(cur_);
            dmaFromIop(envData(cur_), uint32_t(half_));
            return;
        }
        if (H_ == 0 && cbPrep_ && PR_ != 0)
        {
            post(1, true, PR_, 6);
            PR_ = 0;
        }
        if (cbXfer_ && A_ != 0)
            post(2, true, A_, 6);
        const int idx = firstActive_ < kNoVoice ? firstActive_ : firstFinish_;
        irqAddr_ = envBuf(idx) & ~0xFu;
        H_ = (H_ != 1) ? 1 : 0;
        if (H_ != 1)
            irqAddr_ += uint32_t(half_);
        withStreamCore(&Engine::setIrqAddr, irqAddr_);
        withStreamCore(&Engine::spuSetIrq, 1);
        S_ = firstActive_ < kNoVoice ? 0x32 : 0x40;
    }

    void streamIrq() // FUN_1214c
    {
        if (xferUndelivered_ > 0)
            Counters::inc(ctr_.stale_refills); // the EE has not refilled since the last transfer callback
        int L = -1;
        int32_t maxLs = 0;
        withStreamCore(&Engine::spuSetIrq, 0);
        S_ = ((S_ & 0xF0) == 0x40) ? 0x42 : 0x33;
        if (F_ != 0)
        {
            for (int v = 0; v < 24; v++)
            {
                if (!((F_ >> v) & 1))
                    continue;
                const int32_t ls = envLast(v);
                uint32_t a = (envBuf(v) & ~0xFu) + uint32_t(ls) - 0x10;
                if (H_ == 0)
                    a += uint32_t(half_);
                setLsax(v, a & ~0xFu);
                if (maxLs < ls)
                {
                    maxLs = ls;
                    L = v;
                }
            }
        }
        if (A_ == 0)
        {
            if (maxLs < 0x11)
            {
                streamFinish();
                return;
            }
            irqCb_ = IrqCb::StreamFinish;
            irqAddr_ = (envBuf(L) & ~0xFu) + uint32_t(maxLs) - 0x10;
            if (H_ == 0)
                irqAddr_ += uint32_t(half_);
            withStreamCore(&Engine::setIrqAddr, irqAddr_);
        }
        if (cbFin_ && Fp_ != 0)
            post(3, true, Fp_, 6);
        if (A_ == 0)
        {
            S_ = 0x41;
            withStreamCore(&Engine::spuSetIrq, 1);
            return;
        }
        P_ = A_;
        Fp_ = F_;
        cur_ = firstActive_;
        if (H_ == 0)
        {
            if (PQ_ != 0)
            {
                P_ = A_ | PQ_;
                PD_ = PQ_;
                PR_ = PQ_;
                PQ_ = 0;
                cur_ = firstBit(P_);
            }
        }
        else if (SQ_ != 0 && PD_ != 0)
        {
            const uint32_t bits = SQ_ & PD_;
            setKeyOnStreamCore(bits);
            PD_ = 0;
            SQ_ = 0;
            A_ |= bits;
            P_ = A_;
            firstActive_ = cur_ = firstBit(A_);
        }
        F_ = 0;
        roundXfer(cur_);
        S_ = 0x31;
        dmaFromIop(envData(cur_), uint32_t(half_));
    }

    void streamFinish() // FUN_12070
    {
        withStreamCore(&Engine::spuSetIrq, 0);
        S_ = 0x43;
        irqCb_ = savedIrqCb_;
        dmaCb_ = savedDmaCb_;
        if (cbFin_ && F_ != 0)
            post(3, true, F_, 8);
        F_ = Fp_ = 0;
        S_ = 0x10;
    }

    uint32_t stGetStatus() const
    {
        switch (S_ & 0xF0)
        {
            case 0x00: return 0;
            case 0x10: return 3;
            case 0x20: return 4;
            case 0x30: return 7;
            case 0x40: return 8;
            default: return 0xFFFFFFFDu;
        }
    }

    // =======================================================================
    // Dispatch (spec 1, 2)
    // =======================================================================
    void unknown(uint32_t fno)
    {
        Counters::inc(ctr_.unknown_commands);
        ctr_.last_unknown_fno.store(fno, std::memory_order_relaxed);
    }

    // Returns true when the reply is the key buffer (0x6418, 0xFFFE).
    bool dispatch(uint32_t fno)
    {
        uint8_t st[64];
        switch (fno)
        {
            case 0xFFFE: batch(); keyBuf_[0x3F] = 0; return true;
            case 0x6418: keyStatus(keyBuf_); return true;
            case 0x0001: spuInit(); break;
            case 0x0002: { const uint32_t prev = uint32_t(core_); core_ = int(arg(1) & 1); ret_ = prev; break; }
            case 0x0005: setKey(arg(1), arg(2)); break;
            case 0x0006: ret_ = setReverb(arg(1)); break;
            case 0x0008: { const uint32_t e = (arg(1) >> 17) & 0xF; eeaStore_[core_] = e; W(0x33C + C(core_), uint16_t(e)); break; }
            case 0x000A: setReverbDepth(arg(1), arg(2)); break;
            case 0x000B: ret_ = setReverbVoice(arg(1), arg(2)); break;
            case 0x0024: ret_ = stGetStatus(); break;
            case 0x0101: ret_ = spuMalloc(int32_t(arg(1))); break;
            case 0x1011: spuFree(arg(1)); break;
            case 0x1030: cnt7600_ = arg(1); break;
            case 0x0200: S_ = 0x10; stReset(); ret_ = mirrorAddr(); cbPrep_ = cbXfer_ = cbFin_ = true; break;
            case 0x0201: if (S_ == 0x10) { S_ = 0; stReset(); ret_ = 1; } else ret_ = 0xFFFFFFFDu; break;
            case 0x0202: copyMirrorToEnv(); ret_ = stTransfer(arg(1), arg(2)); break;
            case 0x0203: { const uint32_t prev = streamCore_; streamCore_ = arg(1) & 1; ret_ = prev; break; }
            case 0x4002: break; // SsEnd: no-op without a tick (spec 12)
            case 0x4008: ssInit(); break;
            case 0x4015: spuQuit(); break;
            case 0x404A:
            {
                const int16_t l = int16_t(arg(1)), r = int16_t(arg(2));
                W(0x760 + X(core_), uint16_t(int16_t(l * 0x81)) & 0x7FFF);
                W(0x762 + X(core_), uint16_t(int16_t(r * 0x81)) & 0x7FFF);
                break;
            }
            case 0x4059: ret_ = vabGetVagAddr(int16_t(arg(1)), uint16_t(arg(2))); break;
            case 0x4062: vabClose(int16_t(arg(1))); break;
            case 0x4063: ret_ = uint32_t(int32_t(int16_t(vabOpenHead(arg(1), int16_t(arg(2)))))); break;
            case 0x4065: ret_ = uint32_t(int32_t(int16_t(transCompleted(int16_t(arg(1)))))); break;
            case 0x4066: vabTransBody(arg(1), int16_t(arg(2))); break;
            case 0x7128: viewCopy(0, st, 40); setCommonAttr(st); break;
            case 0x7240: viewCopy(0, st, 64); setVoiceAttr(st); break;
            case 0x7314: viewCopy(0, st, 20); (void)setReverbModeParam(st); break;
            case 0x8100: dmaCb_ = DmaCb::Type5; break;
            case 0x8200: irqCb_ = IrqCb::Type6; break;
            case 0x8600: break; // core-0 autoDMA callback: never fires (ch4 unused)
            case 0xE621: cbThread_ = true; break;
            default: unknown(fno); break; // "SPU driver error: unknown command", stale reply
        }
        return false;
    }

    void setReverbDepth(uint32_t l, uint32_t r)
    {
        revDepthL_ = uint16_t(l);
        revDepthR_ = uint16_t(r);
        W(0x764 + X(core_), revDepthL_);
        W(0x766 + X(core_), revDepthR_);
    }
    uint32_t setReverbVoice(uint32_t on, uint32_t bits)
    {
        (void)reverbVoiceMask(on, bits, 0x18C, 0x18E);
        return reverbVoiceMask(on, bits, 0x194, 0x196);
    }

    // 0xFFFE (spec 1). Each command runs at the driver's current cycle; the
    // 30 us post-delays advance the SPU2 timeline by kBatchDelay.
    void batch()
    {
        uint32_t i = 0;
        const uint32_t maxWord = kViewSize / 4;
        uint8_t st[64];
        for (bool more = true; more;)
        {
            if (i >= maxWord)
            {
                unknown(0xFFFE); // DEVIATION: runaway list stopped at the end of the driver data
                break;
            }
            const uint32_t cmd = arg(i);
            Counters::inc(ctr_.batch_entries);
            switch (cmd)
            {
                case 0xFFFFFFFFu: more = false; break;
                case 0x0002: core_ = int(arg(i + 1) & 1); i += 1; break;
                case 0x0005: setKey(arg(i + 1), arg(i + 2)); waitCycles(kBatchDelay); i += 2; break;
                case 0x0006: setReverb(arg(i + 1)); waitCycles(kBatchDelay); i += 1; break;
                case 0x000A: setReverbDepth(uint32_t(int16_t(arg(i + 1))), uint32_t(int16_t(arg(i + 2)))); waitCycles(kBatchDelay); i += 2; break;
                case 0x000B: setReverbVoice(arg(i + 1), arg(i + 2)); waitCycles(kBatchDelay); i += 2; break;
                case 0x0202: copyMirrorToEnv(); stTransfer(arg(i + 1), arg(i + 2)); waitCycles(kBatchDelay); i += 2; break;
                case 0x1020: unknown(cmd); i += 3; break; // not used by RR5 (DEVIATION: no effect)
                case 0x1021: unknown(cmd); break;
                case 0x4049: case 0x4052: break; // libsnd flag on both cores; no SPU2 effect
                case 0x6418: keyStatus(keyBuf_ + (core_ == 0 ? 0 : 24)); i += 1; break;
                case 0x7128: viewCopy((i + 1) * 4, st, 40); setCommonAttr(st); i += 10; break;
                case 0x7240: viewCopy((i + 1) * 4, st, 64); setVoiceAttr(st); i += 16; break;
                case 0x7314: viewCopy((i + 1) * 4, st, 20); setReverbModeParam(st); waitCycles(kBatchDelay); i += 5; break;
                case 0xFFE0: unknown(cmd); i += 4; break; // not used by RR5 (DEVIATION: no effect)
                default: unknown(cmd); break;           // skipped by one word only
            }
            i += 1;
        }
    }
};

} // namespace

// ===========================================================================
// Driver: queue + threading around the Engine.
// ===========================================================================
struct Driver::Impl
{
    enum class Kind : uint8_t { IopWr, Rpc, RpcAsync, CbDelivered, Advance, Sync, Hash, Read };
    struct Barrier
    {
        bool done = false;
        std::vector<uint8_t>* reply = nullptr;
        std::vector<AsyncReply>* replies = nullptr;
        std::vector<Callback>* callbacks = nullptr;
        std::vector<IopWrite>* writes = nullptr;
        uint64_t r64 = 0;
    };
    struct Op
    {
        Op(Kind k, uint64_t time) : kind(k), t(time) {}
        Kind kind;
        uint64_t t;
        uint32_t a = 0, b = 0;
        uint64_t id = 0;
        std::vector<uint8_t> data;
        Barrier* barrier = nullptr;
    };

    Config cfg;
    rrv_spu2* spu = nullptr;
    Counters ctr;
    std::unique_ptr<Engine> eng;

    uint64_t lastT = 0;
    uint64_t nextId = 1;

    std::mutex m;
    std::condition_variable cvWork, cvDone;
    std::deque<Op> q;
    bool stopFlag = false;
    bool sleeping = false;
    std::thread worker;

    uint64_t stamp(uint64_t t)
    {
        if (t < lastT)
        {
            Counters::inc(ctr.clamp_violations);
            return lastT;
        }
        lastT = t;
        return t;
    }

    void exec(Op& op)
    {
        static const bool profile = [] { const char* v = std::getenv("RRV_GATE8_AUDIO_PROFILE"); return v && v[0] == '1'; }();
        const uint64_t t0 = hostNs();
        execOp(op);
        const uint64_t dt = hostNs() - t0;
        if (dt > ctr.max_op_ns.load(std::memory_order_relaxed))
            ctr.max_op_ns.store(dt, std::memory_order_relaxed);
        if (dt > 1000000)
        {
            Counters::inc(ctr.slow_ops);
            if (profile)
                std::fprintf(stderr, "[gate8-slow] kind=%u a=0x%x t=%llu host_ms=%.2f\n", unsigned(op.kind), op.a,
                             static_cast<unsigned long long>(op.t), dt / 1e6);
        }
    }

    void execOp(Op& op)
    {
        switch (op.kind)
        {
            case Kind::IopWr: eng->opIopWrite(op.t, op.a, op.data); break;
            case Kind::Rpc: op.barrier->r64 = eng->opRpc(op.t, op.a, op.data, op.b, *op.barrier->reply); break;
            case Kind::RpcAsync: eng->opRpcAsync(op.t, op.id, op.a, op.data, op.b); break;
            case Kind::CbDelivered: eng->opCallbackDelivered(op.t, op.a); break;
            case Kind::Advance: eng->opAdvance(op.t); break;
            case Kind::Sync: eng->opSync(op.t, *op.barrier->replies, *op.barrier->callbacks, *op.barrier->writes); break;
            case Kind::Hash: op.barrier->r64 = eng->opHash(op.t); break;
            case Kind::Read: op.barrier->r64 = eng->opRead(op.t, op.a); break;
        }
    }

    void submit(Op&& op)
    {
        if (!cfg.threaded)
        {
            exec(op);
            if (op.barrier)
                op.barrier->done = true;
            return;
        }
        Barrier* b = op.barrier;
        std::unique_lock<std::mutex> lk(m);
        // A blocking op on an idle driver runs here, on the caller: the order
        // is the same (nothing is queued, the worker is parked on m), and it
        // skips two thread handoffs (~4 us). RR5's boot polls RPC 0x4065
        // ~124k times while a transfer completes; through the worker that ran
        // ~6x slower than real time (4-5 VBlanks of ~107 ms, Gate-8 A3).
        if (b && q.empty() && sleeping)
        {
            exec(op);
            b->done = true;
            return;
        }
        q.push_back(std::move(op));
        const uint64_t depth = q.size();
        if (depth > ctr.queue_high_water.load(std::memory_order_relaxed))
            ctr.queue_high_water.store(depth, std::memory_order_relaxed);
        const bool wake = sleeping;
        if (!b)
        {
            lk.unlock();
            if (wake)
                cvWork.notify_one();
            return;
        }
        if (wake)
            cvWork.notify_one();
        const uint64_t t0 = hostNs();
        cvDone.wait(lk, [b] { return b->done; });
        Counters::inc(ctr.barrier_waits);
        Counters::inc(ctr.barrier_wait_ns, hostNs() - t0);
    }

    void workerMain()
    {
#if defined(__linux__)
        pthread_setname_np(pthread_self(), "rrv-spu2"); // [cpu] log (Gate 5)
#endif
        std::deque<Op> batch;
        std::unique_lock<std::mutex> lk(m);
        for (;;)
        {
            while (q.empty() && !stopFlag)
            {
                sleeping = true;
                cvWork.wait(lk);
                sleeping = false;
            }
            if (q.empty() && stopFlag)
                break;
            batch.swap(q);
            lk.unlock();
            for (Op& op : batch)
            {
                exec(op);
                if (op.barrier)
                {
                    {
                        std::lock_guard<std::mutex> g(m);
                        op.barrier->done = true;
                    }
                    cvDone.notify_all();
                }
            }
            batch.clear();
            lk.lock();
        }
    }
};

Driver::Driver(const Config& cfg) : impl_(std::make_unique<Impl>())
{
    Impl& I = *impl_;
    I.cfg = cfg;
    rrv_spu2_config sc{};
    sc.threaded = 0; // the driver thread (or the inline caller) runs the core
    sc.output_ring_frames = cfg.output_ring_frames;
    I.spu = rrv_spu2_create(&sc);
    if (!I.spu)
        return;
    I.eng = std::make_unique<Engine>(cfg, I.spu, I.ctr);
    if (cfg.threaded)
        I.worker = std::thread([&I] { I.workerMain(); });
}

Driver::~Driver()
{
    Impl& I = *impl_;
    if (I.worker.joinable())
    {
        {
            std::lock_guard<std::mutex> lk(I.m);
            I.stopFlag = true;
        }
        I.cvWork.notify_one();
        I.worker.join();
    }
    I.eng.reset();
    if (I.spu)
        rrv_spu2_destroy(I.spu);
}

bool Driver::ok() const { return impl_->spu != nullptr; }

void Driver::iopWrite(uint64_t t, uint32_t addr, const void* src, uint32_t n)
{
    Impl& I = *impl_;
    if (!I.spu)
        return;
    Impl::Op op{Impl::Kind::IopWr, I.stamp(t)};
    op.a = addr;
    op.data.assign(static_cast<const uint8_t*>(src), static_cast<const uint8_t*>(src) + n);
    I.submit(std::move(op));
}

uint64_t Driver::rpc(uint64_t t, uint32_t fno, const void* recvbuf, uint32_t size, void* reply, uint32_t reply_size)
{
    Impl& I = *impl_;
    if (!I.spu)
        return t;
    Counters::inc(I.ctr.rpc_count);
    std::vector<uint8_t> r;
    Impl::Barrier b;
    b.reply = &r;
    Impl::Op op{Impl::Kind::Rpc, I.stamp(t)};
    op.a = fno;
    op.b = reply_size;
    if (recvbuf && size)
        op.data.assign(static_cast<const uint8_t*>(recvbuf), static_cast<const uint8_t*>(recvbuf) + size);
    op.barrier = &b;
    I.submit(std::move(op));
    if (reply && reply_size)
        std::memcpy(reply, r.data(), reply_size);
    return b.r64;
}

uint64_t Driver::rpcAsync(uint64_t t, uint32_t fno, const void* recvbuf, uint32_t size, uint32_t reply_size)
{
    Impl& I = *impl_;
    const uint64_t id = I.nextId++;
    if (!I.spu)
        return id;
    Counters::inc(I.ctr.rpc_count);
    Impl::Op op{Impl::Kind::RpcAsync, I.stamp(t)};
    op.a = fno;
    op.b = reply_size;
    op.id = id;
    if (recvbuf && size)
        op.data.assign(static_cast<const uint8_t*>(recvbuf), static_cast<const uint8_t*>(recvbuf) + size);
    I.submit(std::move(op));
    return id;
}

void Driver::callbackDelivered(uint64_t t, uint32_t type)
{
    Impl& I = *impl_;
    if (!I.spu)
        return;
    Impl::Op op{Impl::Kind::CbDelivered, I.stamp(t)};
    op.a = type;
    I.submit(std::move(op));
}

void Driver::advance(uint64_t t)
{
    Impl& I = *impl_;
    if (!I.spu)
        return;
    I.submit(Impl::Op{Impl::Kind::Advance, I.stamp(t)});
}

void Driver::sync(uint64_t t, std::vector<AsyncReply>& replies, std::vector<Callback>& callbacks,
                  std::vector<IopWrite>& writes)
{
    Impl& I = *impl_;
    if (!I.spu)
        return;
    Impl::Barrier b;
    b.replies = &replies;
    b.callbacks = &callbacks;
    b.writes = &writes;
    Impl::Op op{Impl::Kind::Sync, I.stamp(t)};
    op.barrier = &b;
    I.submit(std::move(op));
}

void Driver::syncUpTo(uint64_t upTo, std::vector<AsyncReply>& replies, std::vector<Callback>& callbacks,
                      std::vector<IopWrite>& writes)
{
    Impl& I = *impl_;
    if (!I.spu)
        return;
    Impl::Barrier b;
    b.replies = &replies;
    b.callbacks = &callbacks;
    b.writes = &writes;
    Impl::Op op{Impl::Kind::Sync, upTo}; // not stamped: may be behind lastT
    op.barrier = &b;
    I.submit(std::move(op));
}

size_t Driver::pullOutput(float* stereo, size_t frames)
{
    if (!impl_->spu)
    {
        std::memset(stereo, 0, frames * 2 * sizeof(float));
        return 0;
    }
    return rrv_spu2_pull_output(impl_->spu, stereo, frames);
}

Driver::Stats Driver::stats() const
{
    const Impl& I = *impl_;
    const Counters& c = I.ctr;
    auto L = [](const std::atomic<uint64_t>& a) { return a.load(std::memory_order_relaxed); };
    Stats s;
    if (I.spu)
    {
        rrv_spu2_stats ss{};
        rrv_spu2_get_stats(I.spu, &ss);
        s.frames_mixed = ss.frames_mixed;
        s.underrun_frames = ss.underrun_frames;
        s.overrun_frames = ss.overrun_frames;
        s.spu2_clamp_violations = ss.clamp_violations;
        s.spu2_dma_overlaps = ss.dma_overlaps;
    }
    s.rpc_count = L(c.rpc_count);
    s.batch_entries = L(c.batch_entries);
    s.barrier_waits = L(c.barrier_waits);
    s.barrier_wait_ns = L(c.barrier_wait_ns);
    s.queue_high_water = L(c.queue_high_water);
    s.clamp_violations = L(c.clamp_violations);
    s.unknown_commands = L(c.unknown_commands);
    s.last_unknown_fno = uint32_t(L(c.last_unknown_fno));
    s.iop_out_of_window = L(c.iop_oob);
    s.dma_transfers = L(c.dma_transfers);
    s.callbacks = L(c.callbacks);
    s.callbacks_dropped = L(c.callbacks_dropped);
    s.hangs_avoided = L(c.hangs_avoided);
    s.spin_timeouts = L(c.spin_timeouts);
    s.driver_cycle = L(c.driver_cycle);
    s.stale_refills = L(c.stale_refills);
    s.slow_ops = L(c.slow_ops);
    s.max_op_ns = L(c.max_op_ns);
    return s;
}

uint64_t Driver::debugSpu2StateHash(uint64_t t)
{
    Impl& I = *impl_;
    if (!I.spu)
        return 0;
    Impl::Barrier b;
    Impl::Op op{Impl::Kind::Hash, I.stamp(t)};
    op.barrier = &b;
    I.submit(std::move(op));
    return b.r64;
}

uint16_t Driver::debugReadSpu2(uint64_t t, uint32_t reg_offset)
{
    Impl& I = *impl_;
    if (!I.spu)
        return 0;
    Impl::Barrier b;
    Impl::Op op{Impl::Kind::Read, I.stamp(t)};
    op.a = reg_offset;
    op.barrier = &b;
    I.submit(std::move(op));
    return uint16_t(b.r64);
}

uint64_t Driver::debugOutputHash()
{
    if (!impl_->spu)
        return 0;
    rrv_spu2_stats ss{};
    rrv_spu2_get_stats(impl_->spu, &ss);
    return ss.output_hash;
}

// ---------------------------------------------------------------------------
// Process-wide instance
// ---------------------------------------------------------------------------
namespace
{
std::unique_ptr<Driver> g_driver;

#if defined(RRV_GATE8_SPU2)
size_t pullForSdl(void* user, float* stereo, size_t frames)
{
    return static_cast<Driver*>(user)->pullOutput(stereo, frames);
}
#endif
} // namespace

Driver* start(const Config& config)
{
    if (g_driver)
        return g_driver.get();
    auto d = std::make_unique<Driver>(config);
    if (!d->ok())
    {
        std::fprintf(stderr, "[gate8-audio] SPU2 core creation failed\n");
        return nullptr;
    }
    g_driver = std::move(d);
#if defined(RRV_GATE8_SPU2)
    rrv::audio::setStreamSource(&pullForSdl, g_driver.get());
#endif
    std::fprintf(stderr, "[gate8-audio] RSPU2 driver started (%s)\n", config.threaded ? "driver thread" : "inline");
    return g_driver.get();
}

Driver* instance() { return g_driver.get(); }

void stop()
{
    if (!g_driver)
        return;
#if defined(RRV_GATE8_SPU2)
    rrv::audio::setStreamSource(nullptr, nullptr);
#endif
    g_driver.reset();
}
} // namespace rrv::rspu2
