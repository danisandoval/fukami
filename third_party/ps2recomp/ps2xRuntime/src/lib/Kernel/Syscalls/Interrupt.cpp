#include "Common.h"
#include "Interrupt.h"
#include "runtime/diag_counters.h"
#include "ps2_log.h"
#include "Stubs/GS.h"
#include "Stubs/CD.h"

namespace ps2_syscalls
{
    namespace interrupt_state
    {
        constexpr uint32_t kIntcVblankStart = 2u;
        constexpr uint32_t kIntcVblankEnd = 3u;
        constexpr auto kVblankPeriod = std::chrono::microseconds(16667);
        constexpr int kMaxCatchupTicks = 4;

        std::mutex g_irq_handler_mutex;
        std::mutex g_irq_worker_mutex;
        std::condition_variable g_irq_worker_cv;
        std::mutex g_vsync_flag_mutex;
        std::condition_variable g_vsync_cv;
        std::atomic<bool> g_irq_worker_stop{false};
        std::atomic<bool> g_irq_worker_running{false};
        uint32_t g_enabled_intc_mask = 0xFFFFFFFFu;
        uint32_t g_enabled_dmac_mask = 0xFFFFFFFFu;
        uint64_t g_vsync_tick_counter = 0u;
        VSyncFlagRegistration g_vsync_registration{};
    }

    using namespace interrupt_state;
// Inserted in namespace ps2_syscalls. The owner has committed all hardware first.
static std::atomic<uint64_t> gate3VisibleState{1};
void gate3PublishTemporal(uint64_t starts, bool field) {
    gate3VisibleState.store((starts<<1)|uint64_t(field),std::memory_order_relaxed);
}
void gate3RetireCallback(int id, PS2Runtime* runtime) {
    gate3Cancel(id,runtime,true);
    std::lock_guard lock(g_thread_map_mutex);g_threads.erase(id);
}
void gate3DeliverTemporal(uint8_t* rdram,R5900Context* ctx,PS2Runtime* runtime) {
    auto& owner=runtime->gate3TemporalV1();
    auto& intc=owner.intc();
    // Alarm callbacks are a separate HLE admission route, not an invented INTC line.
    if(!runtime->gate3AdmissionSuppressedV1(1))while(auto alarm=owner.PopAlarm()) {
        const auto& a=alarm->second;
        runtime->gate3RunCallbackV1(ctx,a.handler,a.gp,alarm->first,a.ticks,a.arg,false,1);
    }
    if(!intc.InterruptEligible(ctx->cop0_status))return;
    if(intc.ReadStat()&2)owner.timers().latch_holds_on_eligible_sbus_intc_exception_at(owner.now());
    std::vector<IrqHandlerInfo> handlers;
    const uint32_t pending=intc.Read().pending;
    { std::lock_guard lock(g_irq_handler_mutex);
      for(const auto& [id,entry]:g_intcHandlers)
        if(entry.enabled && entry.handler && entry.cause<32 && (pending&(1u<<entry.cause)))handlers.push_back(entry); }
    // Also covers handlers restored by snapshot import, not only AddIntcHandler.
    if(!handlers.empty())runtime->gate3RequireIntcHandlerPolicyV1();
    std::sort(handlers.begin(),handlers.end(),[](const auto&a,const auto&b){
        return std::tie(a.cause,a.order,a.id)<std::tie(b.cause,b.order,b.id);});
    int previousCause=-1;
    for(const auto& entry:handlers) {
        if(entry.cause!=previousCause){
            if(!intc.InterruptEligible(ctx->cop0_status) || !(intc.Read().pending&(1u<<entry.cause)))continue;
            previousCause=entry.cause;
        }
        runtime->gate3RunCallbackV1(ctx,entry.handler,entry.gp,entry.cause,entry.arg,0,true);
        if(runtime->gate3AtCutV1())return;
    }

}
int gate3TemporalWait(uint8_t*,R5900Context* ctx,PS2Runtime* runtime,std::function<bool()> ready) {
    while(!ready()) {
        auto ticket=gate3Wait(ctx,runtime,6,0);
        runtime->gate3AddTimedWaitV1(ticket,ready);
        gate3Park(ticket,runtime);
        if(ticket->result!=KE_OK)return ticket->result;
        // A handler or another admitted guest may acknowledge sticky status
        // before this context observes it. Hardware wake is not a reserved
        // VBlank generation; re-evaluate the original predicate under G.
    }
    return KE_OK;
}
void gate3SyncV(uint8_t* rdram,R5900Context* ctx,PS2Runtime* runtime) {
    runtime->gate3DiagSyncVV1(ctx,true);
    const uint32_t gp=getRegU32(ctx,28);
    const auto load=[&](uint32_t address){uint32_t value;std::memcpy(&value,getMemPtr(rdram,address),4);return value;};
    if(load(gp-0x5c80u)!=0)
        throw std::logic_error("Gate3 SyncV alternate syscall-73 FIELD binding unresolved");
    runtime->gate3HardwareV1();
    auto& owner=runtime->gate3TemporalV1();
    owner.intc().AcknowledgeVBlankStart();
    const int result=gate3TemporalWait(rdram,ctx,runtime,[&owner]{return owner.intc().VBlankStartPending();});
    if(result!=KE_OK){setReturnS32(ctx,result);runtime->gate3DiagSyncVV1(ctx,false);return;}
    owner.intc().AcknowledgeVBlankStart();
    // Original 0x2C3470: lh v1,-0x5C88(gp) - the interlace flag is a signed
    // halfword. RR5's next halfword is 2, so a 32-bit read never equals 1.
    int16_t interlace; std::memcpy(&interlace,getMemPtr(rdram,gp-0x5c88u),2);
    setReturnS32(ctx,interlace==1 ? int(owner.field()) : 1);
    runtime->gate3DiagSyncVV1(ctx,false);
}

bool gate3HasTemporalWake(R5900Context* ctx,PS2Runtime* runtime) {
    auto& owner=runtime->gate3TemporalV1();auto& intc=owner.intc();
    const uint32_t status=ctx->cop0_status;
    using I=rrv::guest_time::IntcState;
    if((status&(I::kStatusIe|I::kStatusEie|I::kStatusIntcEnable))!=(I::kStatusIe|I::kStatusEie|I::kStatusIntcEnable) ||
        (status&(I::kStatusExl|I::kStatusErl)))return false;
    uint32_t possible=intc.ReadStat();
    if(!owner.sint())possible|=(1u<<2)|(1u<<3);
    if(owner.timers().next_irq_deadline())possible|=0x1e00;
    possible&=intc.ReadMask();
    std::lock_guard lock(g_irq_handler_mutex);
    for(const auto& [id,entry]:g_intcHandlers)
        if(entry.enabled && entry.handler && entry.cause<32 && (possible&(1u<<entry.cause)))return true;
    return false;
}


    static void writeGuestU32NoThrow(uint8_t *rdram, uint32_t addr, uint32_t value)
    {
        if (addr == 0u)
        {
            return;
        }

        uint8_t *dst = getMemPtr(rdram, addr);
        if (!dst)
        {
            return;
        }
        std::memcpy(dst, &value, sizeof(value));
    }

    static void writeGuestU64NoThrow(uint8_t *rdram, uint32_t addr, uint64_t value)
    {
        if (addr == 0u)
        {
            return;
        }

        uint8_t *dst = getMemPtr(rdram, addr);
        if (!dst)
        {
            return;
        }
        std::memcpy(dst, &value, sizeof(value));
    }

    static uint32_t readGuestU32NoThrow(uint8_t *rdram, uint32_t addr)
    {
        if (addr == 0u)
        {
            return 0u;
        }

        uint8_t *src = getMemPtr(rdram, addr);
        if (!src)
        {
            return 0u;
        }

        uint32_t value = 0u;
        std::memcpy(&value, src, sizeof(value));
        return value;
    }

    static uint32_t getAsyncHandlerStackTop(PS2Runtime *runtime)
    {
        constexpr uint32_t kAsyncHandlerStackSize = 0x4000u;
        thread_local PS2Runtime *s_cachedRuntime = nullptr;
        thread_local uint32_t s_cachedStackTop = 0u;

        if (runtime == nullptr)
        {
            return 0u;
        }

        if (s_cachedRuntime != runtime || s_cachedStackTop == 0u)
        {
            s_cachedRuntime = runtime;
            s_cachedStackTop = runtime->reserveAsyncCallbackStack(kAsyncHandlerStackSize, 16u);
        }

        return s_cachedStackTop;
    }

    static void dispatchIntcHandlersForCause(uint8_t *rdram, PS2Runtime *runtime, uint32_t cause)
    {
        if (!rdram || !runtime)
        {
            return;
        }

        std::vector<IrqHandlerInfo> handlers;
        {
            std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
            if (cause < 32u && (g_enabled_intc_mask & (1u << cause)) == 0u)
            {
                return;
            }

            handlers.reserve(g_intcHandlers.size());
            for (const auto &[id, info] : g_intcHandlers)
            {
                (void)id;
                if (!info.enabled)
                {
                    continue;
                }
                if (info.cause != cause)
                {
                    continue;
                }
                if (info.handler == 0u)
                {
                    continue;
                }
                handlers.push_back(info);
            }
            std::sort(handlers.begin(), handlers.end(), [](const IrqHandlerInfo &a, const IrqHandlerInfo &b)
                      { return a.order < b.order; });
        }

        for (const IrqHandlerInfo &info : handlers)
        {
            if (!runtime->hasFunction(info.handler))
            {
                if (cause == kIntcVblankStart)
                {
                    PS2_IF_AGRESSIVE_LOGS({
                        static std::atomic<uint32_t> s_missingHandlerLogCount{0u};
                        const uint32_t logIndex = s_missingHandlerLogCount.fetch_add(1u, std::memory_order_relaxed);
                        if (logIndex < 32u)
                        {
                            auto flags = std::cout.flags();
                            std::cout << "[INTC:missing] cause=" << cause
                                      << " handler=0x" << std::hex << info.handler
                                      << std::dec
                                      << " id=" << info.id
                                      << std::endl;
                            std::cout.flags(flags);
                        }
                    });
                }
                continue;
            }

            try
            {
                const uint32_t stackTop = getAsyncHandlerStackTop(runtime);
                if (stackTop == 0u)
                {
                    std::cerr << "[INTC] async callback-stack reservation failed" << std::endl;
                    runtime->requestStop();
                    return;
                }
                R5900Context irqCtx{};
                SET_GPR_U32(&irqCtx, 28, info.gp);
                SET_GPR_U32(&irqCtx, 29, stackTop);
                SET_GPR_U32(&irqCtx, 31, 0u);
                SET_GPR_U32(&irqCtx, 4, cause);
                SET_GPR_U32(&irqCtx, 5, info.arg);
                SET_GPR_U32(&irqCtx, 6, 0u);
                SET_GPR_U32(&irqCtx, 7, 0u);
                irqCtx.pc = info.handler;

                while (irqCtx.pc != 0u && runtime && !runtime->isStopRequested())
                {
                    PS2Runtime::RecompiledFunction step = runtime->lookupFunction(irqCtx.pc);
                    if (!step)
                    {
                        break;
                    }
                    // Interrupt handlers must be able to preempt a guest thread that is
                    // spinning on interrupt-produced state, such as a vblank counter.
                    step(rdram, &irqCtx, runtime);
                }
            }
            catch (const ThreadExitException &)
            {
            }
            catch (const std::exception &e)
            {
                static uint32_t warnCount = 0;
                if (warnCount < 8u)
                {
                    std::cerr << "[INTC] handler 0x" << std::hex << info.handler
                              << " threw exception: " << e.what() << std::dec << std::endl;
                    ++warnCount;
                }
            }
        }
    }

    void exportIrqHandlerState(std::vector<IrqHandlerSnapshotRecord> &records,
                               IrqHandlerSnapshotCounters &counters)
    {
        std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
        records.clear();
        records.reserve(g_intcHandlers.size() + g_dmacHandlers.size());
        const auto append = [&records](uint32_t table,
                                       const std::unordered_map<int, IrqHandlerInfo> &table_)
        {
            for (const auto &[id, info] : table_)
            {
                records.push_back(IrqHandlerSnapshotRecord{
                    table, static_cast<int32_t>(id), info.cause, info.handler, info.arg,
                    info.gp, info.sp, info.enabled ? 1u : 0u,
                    static_cast<int32_t>(info.order)});
            }
        };
        append(0u, g_intcHandlers);
        append(1u, g_dmacHandlers);
        // Stable order: an unordered_map iterates arbitrarily, and a snapshot
        // that hashes differently run to run is useless for the byte-identity
        // gate (docs/SNAPSHOTS.md §6.1.3).
        std::sort(records.begin(), records.end(),
                  [](const IrqHandlerSnapshotRecord &a, const IrqHandlerSnapshotRecord &b)
                  { return (a.table != b.table) ? (a.table < b.table) : (a.id < b.id); });

        counters.enabledIntcMask = g_enabled_intc_mask;
        counters.enabledDmacMask = g_enabled_dmac_mask;
        counters.nextIntcHandlerId = static_cast<int32_t>(g_nextIntcHandlerId);
        counters.nextDmacHandlerId = static_cast<int32_t>(g_nextDmacHandlerId);
        counters.intcHeadOrder = static_cast<int32_t>(g_intc_head_order);
        counters.intcTailOrder = static_cast<int32_t>(g_intc_tail_order);
        counters.dmacHeadOrder = static_cast<int32_t>(g_dmac_head_order);
        counters.dmacTailOrder = static_cast<int32_t>(g_dmac_tail_order);
    }

    void importIrqHandlerState(const std::vector<IrqHandlerSnapshotRecord> &records,
                               const IrqHandlerSnapshotCounters &counters)
    {
        std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
        g_intcHandlers.clear();
        g_dmacHandlers.clear();
        for (const IrqHandlerSnapshotRecord &r : records)
        {
            IrqHandlerInfo info{};
            info.id = static_cast<int>(r.id);
            info.cause = r.cause;
            info.handler = r.handler;
            info.arg = r.arg;
            info.gp = r.gp;
            info.sp = r.sp;
            info.enabled = r.enabled != 0u;
            info.order = static_cast<int>(r.order);
            if (r.table == 0u)
                g_intcHandlers[info.id] = info;
            else
                g_dmacHandlers[info.id] = info;
        }

        g_enabled_intc_mask = counters.enabledIntcMask;
        g_enabled_dmac_mask = counters.enabledDmacMask;
        g_nextIntcHandlerId = static_cast<int>(counters.nextIntcHandlerId);
        g_nextDmacHandlerId = static_cast<int>(counters.nextDmacHandlerId);
        g_intc_head_order = static_cast<int>(counters.intcHeadOrder);
        g_intc_tail_order = static_cast<int>(counters.intcTailOrder);
        g_dmac_head_order = static_cast<int>(counters.dmacHeadOrder);
        g_dmac_tail_order = static_cast<int>(counters.dmacTailOrder);
    }

    void dispatchDmacHandlersForCause(uint8_t *rdram, PS2Runtime *runtime, uint32_t cause)
    {
        if (!rdram || !runtime)
        {
            return;
        }

        // B3 (docs/TESTING.md §B3): the guest launches display-list chains from
        // the DMA-completion handler, so "how many chains get kicked" is
        // downstream of "how many times this dispatch actually runs". Metadata
        // only. Shares the RRV_DMA_KICK_PC gate; default off.
        static const bool kickProbe = []
        {
            const char *v = std::getenv("RRV_DMA_KICK_PC");
            return v != nullptr && v[0] != '\0' && std::strcmp(v, "0") != 0;
        }();

        std::vector<IrqHandlerInfo> handlers;
        {
            std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
            if (cause < 32u && (g_enabled_dmac_mask & (1u << cause)) == 0u)
            {
                if (kickProbe)
                {
                    std::fprintf(stderr,
                                 "[dma-irq] cause=%u MASKED (enabled-mask=%08x, %zu handlers registered)\n",
                                 cause, g_enabled_dmac_mask, g_dmacHandlers.size());
                }
                return;
            }

            handlers.reserve(g_dmacHandlers.size());
            for (const auto &[id, info] : g_dmacHandlers)
            {
                (void)id;
                if (!info.enabled)
                {
                    continue;
                }
                if (info.cause != cause)
                {
                    continue;
                }
                if (info.handler == 0u)
                {
                    continue;
                }
                handlers.push_back(info);
            }
            std::sort(handlers.begin(), handlers.end(), [](const IrqHandlerInfo &a, const IrqHandlerInfo &b)
                      { return a.order < b.order; });
        }

        if (kickProbe)
        {
            std::fprintf(stderr, "[dma-irq] cause=%u dispatching %zu handler(s):", cause,
                         handlers.size());
            for (const IrqHandlerInfo &h : handlers)
                std::fprintf(stderr, " %08x", h.handler);
            std::fprintf(stderr, "\n");
        }


        for (const IrqHandlerInfo &info : handlers)
        {
            if (!runtime->hasFunction(info.handler))
            {
                continue;
            }

            try
            {
                const uint32_t stackTop = getAsyncHandlerStackTop(runtime);
                if (stackTop == 0u)
                {
                    std::cerr << "[DMAC] async callback-stack reservation failed" << std::endl;
                    runtime->requestStop();
                    return;
                }
                R5900Context irqCtx{};
                SET_GPR_U32(&irqCtx, 28, info.gp);
                SET_GPR_U32(&irqCtx, 29, stackTop);
                SET_GPR_U32(&irqCtx, 31, 0u);
                SET_GPR_U32(&irqCtx, 4, cause);
                SET_GPR_U32(&irqCtx, 5, info.arg);
                SET_GPR_U32(&irqCtx, 6, 0u);
                SET_GPR_U32(&irqCtx, 7, 0u);
                irqCtx.pc = info.handler;

                while (irqCtx.pc != 0u && runtime && !runtime->isStopRequested())
                {
                    PS2Runtime::RecompiledFunction step = runtime->lookupFunction(irqCtx.pc);
                    if (!step)
                    {
                        break;
                    }
                    step(rdram, &irqCtx, runtime);
                }
            }
            catch (const ThreadExitException &)
            {
            }
            catch (const std::exception &e)
            {
                static uint32_t warnCount = 0;
                if (warnCount < 8u)
                {
                    std::cerr << "[DMAC] handler 0x" << std::hex << info.handler
                              << " threw exception: " << e.what() << std::dec << std::endl;
                    ++warnCount;
                }
            }
        }
    }

    // docs/DESIGN_GS_FIELD_MODEL.md M1: per-tick coherence trace. Read-only,
    // default off, changes no behaviour. Prints what each of the five
    // guest-visible consumers observed for THIS tick: the guest flag/tick
    // words this call is about to write, the tick handed to
    // dispatchGsSyncVCallback, and (after the INTC dispatch below) the live GS
    // CSR bit-13 readback -- so a single line proves or disproves "one
    // coherent sequence" without correlating separate log files by hand.
    static bool fieldCoherenceDiagEnabled()
    {
        static const bool s_on = [] {
            const char *v = std::getenv("RRV_FIELD_COHERENCE_DIAG");
            return v && v[0] && v[0] != '0';
        }();
        return s_on;
    }

    static uint64_t signalVSyncFlag(uint8_t *rdram)
    {
        VSyncFlagRegistration reg{};
        uint64_t tickValue = 0u;
        {
            std::lock_guard<std::mutex> lock(g_vsync_flag_mutex);
            reg = g_vsync_registration;
            tickValue = ++g_vsync_tick_counter;
        }

        g_vsync_cv.notify_all();

        if (reg.flagAddr != 0u)
        {
            writeGuestU32NoThrow(rdram, reg.flagAddr, 1u);
        }
        if (reg.tickAddr != 0u)
        {
            writeGuestU64NoThrow(rdram, reg.tickAddr, tickValue);
        }

#if defined(_DEBUG)
        // CRITICAL CORRECTNESS TRAP (docs/DESIGN_GS_FIELD_MODEL.md): the
        // catch-up loop in interruptWorkerMain calls this function once per
        // missed tick, up to kMaxCatchupTicks in a row. fieldIndex must
        // advance exactly once per call -- if a burst ever advanced it by
        // more than one call's worth, parity would silently skip a field.
        // This is a per-call, not per-burst, monotonicity check: it can only
        // pass if every call in a burst produced a distinct, consecutive
        // value.
        {
            static std::atomic<uint64_t> s_lastFieldIndex{0};
            const uint64_t prev = s_lastFieldIndex.exchange(tickValue, std::memory_order_relaxed);
            if (prev != 0u && tickValue != prev + 1u)
            {
                std::fprintf(stderr,
                             "[field:BUG] fieldIndex is not advancing once per tick: "
                             "prev=%llu new=%llu (expected %llu)\n",
                             (unsigned long long)prev, (unsigned long long)tickValue,
                             (unsigned long long)(prev + 1u));
            }
        }
#endif

        return tickValue;
    }

    static void interruptWorkerMain(uint8_t *rdram, PS2Runtime *runtime)
    {
{ /* Guest owner replaces the host VSync worker. */ }
    }

    static void ensureInterruptWorkerRunning(uint8_t *rdram, PS2Runtime *runtime)
    {
{ /* Guest owner replaces the host VSync worker. */ }
    }

    void EnsureVSyncWorkerRunning(uint8_t *rdram, PS2Runtime *runtime)
    {
        ensureInterruptWorkerRunning(rdram, runtime);
    }

    uint64_t GetCurrentVSyncTick()
    {
{ return gate3VisibleState.load(std::memory_order_relaxed)>>1; }
    }

    // docs/DESIGN_GS_FIELD_MODEL.md M2: the one accessor. A single lock
    // acquisition, a single read of the one counter, parity derived from that
    // same value -- so no caller can compose an incoherent {fieldIndex,
    // parity} pair the way two independent reads would.
    GsFieldState GetCurrentField()
    {
{ const auto state=gate3VisibleState.load(std::memory_order_relaxed); return {state>>1,static_cast<int32_t>(state&1)}; }
    }

    int32_t GetCurrentFieldParity()
    {
        return GetCurrentField().parity;
    }

    interrupt_state::VSyncFlagRegistration GetVSyncFlagRegistration()
    {
        std::lock_guard<std::mutex> lock(g_vsync_flag_mutex);
        return g_vsync_registration;
    }

    void SetVSyncFlagRegistrationForReplay(interrupt_state::VSyncFlagRegistration reg)
    {
        std::lock_guard<std::mutex> lock(g_vsync_flag_mutex);
        g_vsync_registration = reg;
    }

    // --- Milestone B4 (docs/MILESTONES.md, GS-stream record/replay harness):
    // small, documented shim. GetCurrentVSyncTick() is normally advanced only
    // by the real-time interrupt-worker thread (a wall-clock ~60Hz ticker
    // started by EnsureVSyncWorkerRunning). A headless replay must NOT spin up
    // that thread (there is no game/EE loop driving it, and its pacing is wall-
    // clock, not deterministic) — instead replay injects the EXACT tick values
    // captured in the recording's Present stream events via this setter, so
    // GS::processGIFPacket's tile-flush check and GS::latchHostPresentationFrame's
    // field-parity computation see the identical sequence of tick values that
    // occurred during the live recording. No other caller should ever need
    // this; it exists solely for tools/gs-replay.
    void SetCurrentVSyncTickForReplay(uint64_t tick)
    {
        std::lock_guard<std::mutex> lock(g_vsync_flag_mutex);
        g_vsync_tick_counter = tick;
    }

    void stopInterruptWorker()
    {
{ /* Guest owner replaces the host VSync worker. */ }
    }

    uint64_t WaitForNextVSyncTick(uint8_t *rdram, PS2Runtime *runtime)
    {
{ throw std::logic_error("Gate3 unconverted host-tick wait"); }
    }

    void WaitVSyncTick(uint8_t *rdram, PS2Runtime *runtime)
    {
        (void)WaitForNextVSyncTick(rdram, runtime);
    }

    void SetVSyncFlag(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
{ throw std::logic_error("Gate3 alternate SyncV syscall-73 binding unresolved"); }
    }

    void EnableIntc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
{ runtime->gate3TemporalV1().intc().EnableHle(getRegU32(ctx,4)); setReturnS32(ctx,KE_OK); }
    }

    void iEnableIntc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        EnableIntc(rdram, ctx, runtime);
    }

    void DisableIntc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
{ runtime->gate3TemporalV1().intc().DisableHle(getRegU32(ctx,4)); setReturnS32(ctx,KE_OK); }
    }

    void iDisableIntc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        DisableIntc(rdram, ctx, runtime);
    }

    void AddIntcHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        runtime->gate3RequireIntcHandlerPolicyV1();
        IrqHandlerInfo info{};
        info.cause = getRegU32(ctx, 4);
        info.handler = getRegU32(ctx, 5);
        uint32_t next = getRegU32(ctx, 6);
        info.arg = getRegU32(ctx, 7);
        info.gp = getRegU32(ctx, 28);
        info.sp = getRegU32(ctx, 29);
        info.enabled = true;

        int handlerId = 0;
        {
            std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
            info.order = (next == 0) ? --g_intc_head_order : ++g_intc_tail_order;
            handlerId = g_nextIntcHandlerId++;
            info.id = handlerId;
            g_intcHandlers[handlerId] = info;
        }

        if (info.cause == kIntcVblankStart)
        {
            PS2_IF_AGRESSIVE_LOGS({
                static std::atomic<uint32_t> s_addHandlerLogCount{0u};
                const uint32_t logIndex = s_addHandlerLogCount.fetch_add(1u, std::memory_order_relaxed);
                if (logIndex < 32u)
                {
                    auto flags = std::cout.flags();
                    std::cout << "[AddIntcHandler] cause=" << info.cause
                              << " handler=0x" << std::hex << info.handler
                              << " arg=0x" << info.arg
                              << " gp=0x" << info.gp
                              << " sp=0x" << info.sp
                              << std::dec
                              << " id=" << handlerId
                              << std::endl;
                    std::cout.flags(flags);
                }
            });
        }

        ensureInterruptWorkerRunning(rdram, runtime);
        setReturnS32(ctx, handlerId);
    }

    void AddIntcHandler2(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        AddIntcHandler(rdram, ctx, runtime);
    }

    void RemoveIntcHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cause = getRegU32(ctx, 4);
        const int handlerId = static_cast<int>(getRegU32(ctx, 5));
        if (handlerId > 0)
        {
            std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
            auto it = g_intcHandlers.find(handlerId);
            if (it != g_intcHandlers.end() && it->second.cause == cause)
            {
                g_intcHandlers.erase(it);
            }
        }
        setReturnS32(ctx, KE_OK);
    }

    void AddDmacHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        IrqHandlerInfo info{};
        info.cause = getRegU32(ctx, 4);
        info.handler = getRegU32(ctx, 5);
        uint32_t next = getRegU32(ctx, 6);
        info.arg = getRegU32(ctx, 7);
        info.gp = getRegU32(ctx, 28);
        info.sp = getRegU32(ctx, 29);
        info.enabled = true;

        int handlerId = 0;
        {
            std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
            info.order = (next == 0) ? --g_dmac_head_order : ++g_dmac_tail_order;
            handlerId = g_nextDmacHandlerId++;
            info.id = handlerId;
            g_dmacHandlers[handlerId] = info;
        }
        setReturnS32(ctx, handlerId);
    }

    void AddDmacHandler2(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        AddDmacHandler(rdram, ctx, runtime);
    }

    void RemoveDmacHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cause = getRegU32(ctx, 4);
        const int handlerId = static_cast<int>(getRegU32(ctx, 5));
        if (handlerId > 0)
        {
            std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
            auto it = g_dmacHandlers.find(handlerId);
            if (it != g_dmacHandlers.end() && it->second.cause == cause)
            {
                g_dmacHandlers.erase(it);
            }
        }
        setReturnS32(ctx, KE_OK);
    }

    void EnableIntcHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int handlerId = static_cast<int>(getRegU32(ctx, 5));
        {
            std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
            if (auto it = g_intcHandlers.find(handlerId); it != g_intcHandlers.end())
            {
                it->second.enabled = true;
            }
        }
        setReturnS32(ctx, KE_OK);
    }

    void DisableIntcHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int handlerId = static_cast<int>(getRegU32(ctx, 5));
        {
            std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
            if (auto it = g_intcHandlers.find(handlerId); it != g_intcHandlers.end())
            {
                it->second.enabled = false;
            }
        }
        setReturnS32(ctx, KE_OK);
    }

    void EnableDmacHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int handlerId = static_cast<int>(getRegU32(ctx, 5));
        {
            std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
            if (auto it = g_dmacHandlers.find(handlerId); it != g_dmacHandlers.end())
            {
                it->second.enabled = true;
            }
        }
        setReturnS32(ctx, KE_OK);
    }

    void DisableDmacHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int handlerId = static_cast<int>(getRegU32(ctx, 5));
        {
            std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
            if (auto it = g_dmacHandlers.find(handlerId); it != g_dmacHandlers.end())
            {
                it->second.enabled = false;
            }
        }
        setReturnS32(ctx, KE_OK);
    }

    void EnableDmac(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cause = getRegU32(ctx, 4);
        if (cause < 32u)
        {
            std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
            g_enabled_dmac_mask |= (1u << cause);
        }
        setReturnS32(ctx, KE_OK);
    }

    void iEnableDmac(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        EnableDmac(rdram, ctx, runtime);
    }

    void DisableDmac(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t cause = getRegU32(ctx, 4);
        if (cause < 32u)
        {
            std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
            g_enabled_dmac_mask &= ~(1u << cause);
        }
        setReturnS32(ctx, KE_OK);
    }

    void iDisableDmac(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        DisableDmac(rdram, ctx, runtime);
    }
}
