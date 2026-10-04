// Gate 8: connects the Gate-3 candidate runtime to the host rewrite of RR5's
// IOP sound driver (src/audio/rspu2_driver.*). Linked only into the candidate
// built with --pcsx2-spu2-source; fills g_gate8AudioHooksV1 at load time.
//
// Time: guest IOP cycles = guest EE cycles / 8 (294.912 MHz / 36.864 MHz),
// taken only from the Gate-3 temporal owner. Guest-visible results depend on
// guest time and the guest's own order of calls, never on host timing:
//  * blocking RPCs are barriers on the driver thread;
//  * nowait RPC completions (the per-tick batch) and IOP->EE callbacks
//    (SID 0x80000603) are delivered from the admission service, in time
//    order: at each effective VBlank start (everything up to now), and every
//    1 ms of guest time in between (everything up to the previous 1 ms
//    point, which the driver thread has usually already reached). RR5's
//    SpuSt stream asks for a refill every ~10.15 ms; delivering only at
//    VBlank starts (16.7 ms) made the EE refill after the SPU had already
//    fetched the next half, so stale blocks replayed (Gate-8 A2 finding).
//    RRV_GATE8_AUDIO_FRAME_DELIVERY=1 restores VBlank-only delivery (A/B);
//  * guest writes into the IOP window reach the driver in guest order.
#include "ps2_runtime.h"
#include "runtime/ps2_memory.h"
#include "rspu2_driver.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include "rrv_sdl_audio.h"
#include <chrono>
#include <cstring>
#include <deque>
#include <memory>
#include <stdexcept>
#include <vector>

namespace
{
constexpr uint32_t kDriverSid = 0x80000601u;
constexpr uint32_t kCallbackSid = 0x80000603u;
constexpr uint32_t kSifRpcModeNowait = 0x01u;
constexpr uint32_t kIopWindowBase = 0x01A00000u;
constexpr uint32_t kIopWindowSize = 0x00500000u;
// The driver's IOP-visible data (reply buffers, SpuStEnv mirror) sits at the
// top of the window; the runtime's first-fit allocator starts at the bottom.
constexpr uint32_t kDriverBlock = 0x01EE0000u;
constexpr uint32_t kDriverBlockSize = 0x00020000u;
constexpr uint64_t kAdvanceQuantum = 36864u; // 1 ms of IOP cycles

struct PendingRpc
{
    uint64_t id;
    uint32_t client, recvBuf, recvSize, endFunc, endParam, gp;
};

struct State
{
    rrv::rspu2::Driver* driver = nullptr; // process-wide instance, feeds SDL
    PS2Runtime* runtime = nullptr;
    uint8_t* rdram = nullptr;
    uint32_t gp = 0;
    uint64_t lastAdvance = 0;
    uint64_t lastStart = ~0ull;
    bool delivering = false;
    std::deque<PendingRpc> pending;
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> earlyWrites; // before the driver exists
    uint64_t droppedCallbacks = 0;
    // RRV_GATE8_AUDIO_WAV=<path>: diagnostic dump. The guest thread drains the
    // output ring at every VBlank instead of SDL, so the file holds exactly the
    // mixed stream (48 kHz, float32 stereo).
    std::FILE* wav = nullptr;
    uint64_t wavFrames = 0;
    std::vector<float> wavScratch;
    // RRV_GATE8_AUDIO_PROFILE=1: host time the glue spends per VBlank start.
    uint64_t profSyncNs = 0, profGuestNs = 0, profRpcNs = 0, profSyncs = 0;
};

uint64_t hostNs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

bool envOn(const char* name)
{
    const char* v = std::getenv(name);
    return v && v[0] == '1';
}
State& state()
{
    static State s;
    return s;
}

bool traceOn()
{
    static const bool on = [] { const char* v = std::getenv("RRV_GATE8_AUDIO_TRACE"); return v && v[0] == '1'; }();
    return on;
}

uint32_t word(const std::vector<uint8_t>& bytes, size_t index)
{
    uint32_t v = 0;
    if (bytes.size() >= (index + 1) * 4)
        std::memcpy(&v, bytes.data() + index * 4, 4);
    return v;
}

// Host-side jitter buffer for the SDL callback thread (never touches guest
// state). The guest produces ~800 frames per field in bursts while SDL asks
// for 1024 at a time, so playback starts (and restarts after running dry)
// only once kPrimeFrames are queued; beyond kMaxFrames the oldest are dropped
// so latency cannot grow with host/guest drift.
struct Jitter
{
    static constexpr size_t kPrimeFrames = 2048; // ~43 ms
    static constexpr size_t kMaxFrames = 9600;   // 200 ms
    std::vector<float> fifo = std::vector<float>(kMaxFrames * 2 * 2);
    size_t head = 0, count = 0; // in frames
    bool priming = true;
    uint64_t silentFrames = 0, droppedFrames = 0, reprimes = 0;
    // Gate-8 A3: host ms (since the first pull) of the first 32 re-primes,
    // printed at exit, so boot-time gaps can be told from later ones.
    std::chrono::steady_clock::time_point firstPull{};
    bool pulled = false;
    uint32_t reprimeMs[32] = {};
    // RRV_GATE3_FAST_FORWARD=N: until start N the guest runs unpaced; its
    // audio is discarded (not queued, not counted as silence or re-primes),
    // so real-time playback starts primed and empty at start N.
    std::atomic<bool> fastForward{false};
    uint64_t fastForwardFrames = 0;
};
Jitter& jitter()
{
    static Jitter j;
    return j;
}

size_t jitterPull(void*, float* stereo, size_t frames)
{
    auto& j = jitter();
    auto& s = state();
    if (!j.pulled)
    {
        j.pulled = true;
        j.firstPull = std::chrono::steady_clock::now();
    }
    const size_t capacity = j.fifo.size() / 2;
    float chunk[512 * 2];
    if (j.fastForward.load(std::memory_order_acquire))
    {
        size_t got;
        while (s.driver && (got = s.driver->pullOutput(chunk, 512)) > 0)
            j.fastForwardFrames += got;
        j.head = j.count = 0;
        j.priming = true;
        j.pulled = false;
        return 0;
    }
    for (;;)
    {
        const size_t got = s.driver ? s.driver->pullOutput(chunk, 512) : 0;
        for (size_t i = 0; i < got; ++i)
        {
            if (j.count == capacity)
            {
                j.head = (j.head + 1) % capacity;
                --j.count;
                ++j.droppedFrames;
            }
            const size_t tail = (j.head + j.count) % capacity;
            j.fifo[tail * 2] = chunk[i * 2];
            j.fifo[tail * 2 + 1] = chunk[i * 2 + 1];
            ++j.count;
        }
        if (got < 512)
            break;
    }
    while (j.count > Jitter::kMaxFrames)
    {
        j.head = (j.head + 1) % capacity;
        --j.count;
        ++j.droppedFrames;
    }
    if (j.priming && j.count >= Jitter::kPrimeFrames)
        j.priming = false;
    size_t out = 0;
    if (!j.priming)
    {
        out = std::min(frames, j.count);
        for (size_t i = 0; i < out; ++i)
        {
            const size_t at = (j.head + i) % capacity;
            stereo[i * 2] = j.fifo[at * 2];
            stereo[i * 2 + 1] = j.fifo[at * 2 + 1];
        }
        j.head = (j.head + out) % capacity;
        j.count -= out;
        if (out < frames)
        {
            j.priming = true;
            if (j.reprimes < 32)
                j.reprimeMs[j.reprimes] = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - j.firstPull).count());
            ++j.reprimes;
        }
    }
    j.silentFrames += frames - out;
    return out;
}

uint64_t iopNow(PS2Runtime* runtime)
{
    return runtime->gate3TemporalV1().now() / 8u;
}

rrv::rspu2::Driver& ensureDriver(PS2Runtime* runtime)
{
    auto& s = state();
    if (!s.driver)
    {
        if (!ps2_stubs::gate8ReserveIopHeapV1(kDriverBlock, kDriverBlockSize))
            std::fprintf(stderr, "[gate8-audio] warning: driver IOP block 0x%08x is in use\n", kDriverBlock);
        rrv::rspu2::Config config{};
        config.iop_window_base = kIopWindowBase;
        config.iop_window_size = kIopWindowSize;
        config.driver_block = kDriverBlock;
        // RRV_GATE8_AUDIO_INLINE=1: the driver runs on the guest thread (A/B check;
        // guest-visible results must be identical to the driver thread).
        const char* inlineEnv = std::getenv("RRV_GATE8_AUDIO_INLINE");
        config.threaded = !(inlineEnv && inlineEnv[0] == '1');
        s.driver = rrv::rspu2::start(config);
        if (!s.driver)
            throw std::runtime_error("Gate8 host sound driver failed to start");
        const uint64_t t = iopNow(runtime);
        for (auto& [addr, bytes] : s.earlyWrites)
            s.driver->iopWrite(t, addr, bytes.data(), static_cast<uint32_t>(bytes.size()));
        s.earlyWrites.clear();
        s.lastAdvance = t;
        std::atexit([] {
            auto& st = state();
            if (!st.driver) return;
            const auto x = st.driver->stats();
            std::fprintf(stderr,
                         "[gate8-audio] stats mixed=%llu underrun=%llu overrun=%llu rpc=%llu batch=%llu barriers=%llu "
                         "barrier_ms=%.1f unknown=%llu(last 0x%x) dma=%llu callbacks=%llu dropped=%llu+%llu hangs=%llu "
                         "spins=%llu out_of_window=%llu sdl_silent=%llu sdl_dropped=%llu sdl_reprimes=%llu "
                         "stale_refills=%llu slow_ops=%llu max_op_ms=%.2f\n",
                         (unsigned long long)x.frames_mixed, (unsigned long long)x.underrun_frames,
                         (unsigned long long)x.overrun_frames, (unsigned long long)x.rpc_count,
                         (unsigned long long)x.batch_entries, (unsigned long long)x.barrier_waits,
                         x.barrier_wait_ns / 1e6, (unsigned long long)x.unknown_commands, x.last_unknown_fno,
                         (unsigned long long)x.dma_transfers, (unsigned long long)x.callbacks,
                         (unsigned long long)x.callbacks_dropped, (unsigned long long)st.droppedCallbacks,
                         (unsigned long long)x.hangs_avoided, (unsigned long long)x.spin_timeouts,
                         (unsigned long long)x.iop_out_of_window, (unsigned long long)jitter().silentFrames,
                         (unsigned long long)jitter().droppedFrames, (unsigned long long)jitter().reprimes,
                         (unsigned long long)x.stale_refills, (unsigned long long)x.slow_ops, x.max_op_ns / 1e6);
            if (const auto& jt = jitter(); jt.reprimes)
            {
                std::fprintf(stderr, "[gate8-audio] sdl re-primes at host ms since first pull:");
                for (uint64_t i = 0; i < std::min<uint64_t>(jt.reprimes, 32); ++i)
                    std::fprintf(stderr, " %u", jt.reprimeMs[i]);
                std::fprintf(stderr, "\n");
            }
            if (const auto& jt = jitter(); jt.fastForwardFrames)
                std::fprintf(stderr, "[gate8-audio] fast-forward discarded %llu frames\n",
                             (unsigned long long)jt.fastForwardFrames);
        });
        rrv::audio::setStreamSource(&jitterPull, nullptr); // replaced below in WAV mode
        if (const char* path = std::getenv("RRV_GATE8_AUDIO_WAV"); path && *path)
        {
            s.wav = std::fopen(path, "wb");
            if (s.wav)
            {
                rrv::audio::setStreamSource(nullptr, nullptr);
                std::vector<uint8_t> header(44, 0);
                std::fwrite(header.data(), 1, header.size(), s.wav); // patched at exit
                std::atexit([] {
                    auto& st = state();
                    if (!st.wav) return;
                    const uint32_t dataBytes = static_cast<uint32_t>(st.wavFrames * 8u);
                    auto le32 = [](uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i)); };
                    uint8_t h[44] = {'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,3,0,2,0,
                                     0,0,0,0,0,0,0,0,8,0,32,0,'d','a','t','a',0,0,0,0};
                    le32(h + 4, 36u + dataBytes); le32(h + 24, 48000u); le32(h + 28, 48000u * 8u); le32(h + 40, dataBytes);
                    std::fseek(st.wav, 0, SEEK_SET);
                    std::fwrite(h, 1, sizeof(h), st.wav);
                    std::fclose(st.wav);
                    st.wav = nullptr;
                    std::fprintf(stderr, "[gate8-audio] wav frames=%llu\n", static_cast<unsigned long long>(st.wavFrames));
                });
            }
        }
        std::fprintf(stderr, "[gate8-audio] host RSPU2 driver started at iop cycle %llu\n",
                     static_cast<unsigned long long>(t));
    }
    return *s.driver;
}

int onSifCallRpc(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime, uint32_t sid,
                 uint32_t clientPtr, uint32_t rpcNum, uint32_t mode, uint32_t sendBuf,
                 uint32_t sendSize, uint32_t recvBuf, uint32_t recvSize, uint32_t endFunc,
                 uint32_t endParam)
{
    if (sid != kDriverSid)
        return 0;
    auto& s = state();
    s.runtime = runtime;
    s.rdram = rdram;
    s.gp = getRegU32(ctx, 28);
    auto& driver = ensureDriver(runtime);
    const uint64_t t = iopNow(runtime);
    std::vector<uint8_t> send(sendSize);
    if (sendSize)
        if (const uint8_t* src = getConstMemPtr(rdram, sendBuf))
            std::memcpy(send.data(), src, sendSize);
    if (traceOn())
        std::fprintf(stderr, "[gate8-rpc] t=%llu fno=0x%x mode=%u size=%u w=%08x %08x %08x %08x\n",
                     static_cast<unsigned long long>(t), rpcNum, mode, sendSize, word(send, 0), word(send, 1),
                     word(send, 2), word(send, 3));
    if (traceOn() && rpcNum == 0xFFFEu)
    {
        std::fprintf(stderr, "[gate8-batch]");
        for (size_t i = 0; i < sendSize / 4 && i < 96; ++i)
        {
            const uint32_t w = word(send, i);
            std::fprintf(stderr, " %x", w);
            if (w == 0xFFFFFFFFu)
                break;
        }
        std::fputc('\n', stderr);
    }
    if ((mode & kSifRpcModeNowait) == 0u)
    {
        std::vector<uint8_t> reply(recvSize);
        const uint64_t h0 = hostNs();
        driver.rpc(t, rpcNum, send.data(), sendSize, reply.data(), recvSize);
        s.profRpcNs += hostNs() - h0;
        if (traceOn())
            std::fprintf(stderr, "[gate8-rpc]   ret=%08x\n", word(reply, 0));
        if (recvSize)
            if (uint8_t* dst = getMemPtr(rdram, recvBuf))
                std::memcpy(dst, reply.data(), recvSize);
        if (endFunc)
            std::fprintf(stderr, "[gate8-audio] warning: blocking RPC 0x%x with end callback 0x%x ignored\n",
                         rpcNum, endFunc);
        s.lastAdvance = t;
        return 1;
    }
    const uint64_t id = driver.rpcAsync(t, rpcNum, send.data(), sendSize, recvSize);
    s.pending.push_back({id, clientPtr, recvBuf, recvSize, endFunc, endParam, s.gp});
    s.lastAdvance = t;
    return 2;
}

void onIopWrite(uint32_t addr, const uint8_t* src, size_t size)
{
    auto& s = state();
    if (!s.driver || !s.runtime)
    {
        s.earlyWrites.emplace_back(addr, std::vector<uint8_t>(src, src + size));
        return;
    }
    s.driver->iopWrite(iopNow(s.runtime), addr, src, static_cast<uint32_t>(size));
}

// behind: collect only up to t (a time the driver was already told about),
// without moving its timeline; otherwise a full barrier at t.
void deliver(PS2Runtime* runtime, R5900Context* ctx, uint64_t t, bool behind)
{
    auto& s = state();
    std::vector<rrv::rspu2::AsyncReply> replies;
    std::vector<rrv::rspu2::Callback> callbacks;
    std::vector<rrv::rspu2::IopWrite> writes;
    const uint64_t h0 = hostNs();
    if (behind)
        s.driver->syncUpTo(t, replies, callbacks, writes);
    else
        s.driver->sync(t, replies, callbacks, writes);
    const uint64_t h1 = hostNs();
    s.profSyncNs += h1 - h0;
    ++s.profSyncs;
    struct GuestTime
    {
        uint64_t h1;
        ~GuestTime() { state().profGuestNs += hostNs() - h1; }
    } guestTime{h1};
    for (const auto& write : writes)
        ps2_stubs::gate8WriteIopHeapNoHookV1(write.addr, write.bytes.data(), write.bytes.size());

    // Replies and callbacks in guest-time order (replies first on a tie: the
    // RPC completed before the driver's later interrupt work).
    size_t r = 0, c = 0;
    while (r < replies.size() || c < callbacks.size())
    {
        if (runtime->gate3AtCutV1())
            return;
        const bool takeReply = c == callbacks.size() ||
                               (r < replies.size() && replies[r].t <= callbacks[c].t);
        if (takeReply)
        {
            const auto& reply = replies[r++];
            auto it = std::find_if(s.pending.begin(), s.pending.end(),
                                   [&](const PendingRpc& p) { return p.id == reply.id; });
            if (it == s.pending.end())
                continue;
            const PendingRpc p = *it;
            s.pending.erase(it);
            if (p.recvSize)
                if (uint8_t* dst = getMemPtr(s.rdram, p.recvBuf))
                    std::memcpy(dst, reply.bytes.data(), std::min<size_t>(p.recvSize, reply.bytes.size()));
            ps2_syscalls::gate8SetRpcBusyV1(p.client, false);
            if (p.endFunc)
                runtime->gate3RunCallbackV1(ctx, p.endFunc, p.gp, p.endParam, 0u, 0u, false, 2u);
        }
        else
        {
            const auto& cb = callbacks[c++];
            const uint32_t sd = ps2_syscalls::gate8RpcServerV1(kCallbackSid);
            const uint8_t* sdBytes = sd ? getConstMemPtr(s.rdram, sd) : nullptr;
            if (!sdBytes)
            {
                ++s.droppedCallbacks;
                continue;
            }
            uint32_t func = 0, buf = 0;
            std::memcpy(&func, sdBytes + 4, 4); // t_SifRpcServerData::func
            std::memcpy(&buf, sdBytes + 8, 4);  // t_SifRpcServerData::buf
            const uint32_t words[4] = {cb.type, cb.a1, cb.a2, 0u};
            if (traceOn())
                std::fprintf(stderr, "[gate8-cb] t=%llu at=%llu type=%u a1=0x%x a2=%u func=0x%x\n",
                             static_cast<unsigned long long>(cb.t), static_cast<unsigned long long>(iopNow(runtime)),
                             cb.type, cb.a1, cb.a2, func);
            if (uint8_t* dst = buf ? getMemPtr(s.rdram, buf) : nullptr)
                std::memcpy(dst, words, sizeof(words));
            if (func)
                runtime->gate3RunCallbackV1(ctx, func, s.gp, 0u, buf, sizeof(words), false, 2u);
            s.driver->callbackDelivered(iopNow(runtime), cb.type);
        }
    }
}

void onService(PS2Runtime* runtime, R5900Context* ctx)
{
    auto& s = state();
    s.runtime = runtime;
    if (!s.driver || s.delivering)
        return;
    const uint64_t t = iopNow(runtime);
    const uint64_t start = runtime->gate3TemporalV1().starts();
    if (start != s.lastStart)
    {
        static const bool profile = envOn("RRV_GATE8_AUDIO_PROFILE");
        if (profile && s.lastStart != ~0ull)
        {
            const uint64_t total = s.profSyncNs + s.profGuestNs + s.profRpcNs;
            if (total > 2000000)
                std::fprintf(stderr, "[gate8-prof] start=%llu sync_ms=%.2f syncs=%llu guest_cb_ms=%.2f rpc_ms=%.2f\n",
                             static_cast<unsigned long long>(s.lastStart), s.profSyncNs / 1e6,
                             static_cast<unsigned long long>(s.profSyncs), s.profGuestNs / 1e6, s.profRpcNs / 1e6);
        }
        s.profSyncNs = s.profGuestNs = s.profRpcNs = s.profSyncs = 0;
        s.lastStart = start;
        static const uint64_t fastForward = [] {
            const char* v = std::getenv("RRV_GATE3_FAST_FORWARD");
            return v && *v ? std::strtoull(v, nullptr, 10) : 0ull;
        }();
        jitter().fastForward.store(start < fastForward, std::memory_order_release);
        s.delivering = true;
        deliver(runtime, ctx, t, false);
        s.delivering = false;
        if (s.wav)
        {
            s.wavScratch.resize(4096 * 2);
            size_t got;
            while ((got = s.driver->pullOutput(s.wavScratch.data(), 4096)) > 0)
            {
                std::fwrite(s.wavScratch.data(), sizeof(float), got * 2, s.wav);
                s.wavFrames += got;
                if (got < 4096) break;
            }
        }
        s.lastAdvance = iopNow(runtime);
        return;
    }
    if (t - s.lastAdvance >= kAdvanceQuantum)
    {
        static const bool frameOnly = envOn("RRV_GATE8_AUDIO_FRAME_DELIVERY");
        if (!frameOnly)
        {
            s.delivering = true;
            deliver(runtime, ctx, s.lastAdvance, true);
            s.delivering = false;
            if (runtime->gate3AtCutV1())
                return;
        }
        const uint64_t now = iopNow(runtime);
        s.driver->advance(now); // lets the driver thread mix while the guest runs
        s.lastAdvance = now;
    }
}

// onService's own conditions, for a full checkpoint at `eeCycle` (the owner clock would be there).
bool onIdle(PS2Runtime* runtime, uint64_t eeCycle)
{
    const auto& s = state();
    if (!s.driver || s.delivering)
        return true;
    return runtime->gate3TemporalV1().starts() == s.lastStart && eeCycle / 8u - s.lastAdvance < kAdvanceQuantum;
}

struct Install
{
    Install()
    {
        g_gate8AudioHooksV1.sifCallRpc = &onSifCallRpc;
        g_gate8AudioHooksV1.iopWrite = &onIopWrite;
        g_gate8AudioHooksV1.service = &onService;
        g_gate8AudioHooksV1.idle = &onIdle;
    }
} g_install;
} // namespace
