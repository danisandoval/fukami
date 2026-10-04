// RRV product SDL host overlay
#include "Common.h"
#include "Thread.h"
#include "rrv_snapshot_hooks.h" // developer snapshots (docs/SNAPSHOTS.md)
#include "rrv_guest_terminal_outcome.h"

namespace ps2_syscalls
{
    static void applySuspendStatusLocked(ThreadInfo &info)
    {
        if (info.waitType != TSW_NONE)
        {
            info.status = THS_WAITSUSPEND;
        }
        else
        {
            info.status = THS_SUSPEND;
        }
    }

    static void runExitHandlersForThread(int tid, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
    if(runtime->gate3AtCutV1())return;
        if (!runtime || !ctx)
            return;

        std::vector<ExitHandlerEntry> handlers;
        {
            std::lock_guard<std::mutex> lock(g_exit_handler_mutex);
            auto it = g_exit_handlers.find(tid);
            if (it == g_exit_handlers.end())
                return;
            handlers = std::move(it->second);
            g_exit_handlers.erase(it);
        }

        for (const auto &handler : handlers)
        {
            if (!handler.func)
                continue;
            try
            {
                rpcInvokeFunction(rdram, ctx, runtime, handler.func, handler.arg, 0, 0, 0, nullptr);
            }
            catch (const ThreadExitException &)
            {
                // ignore
            }
            catch (const std::exception &)
            {
            }
        }
    }

    std::shared_ptr<ThreadInfo> gate3InitializeMainThread(R5900Context* ctx)
    { return ensureCurrentThreadInfo(ctx); }

    void gate3CompleteMainThread(uint8_t* rdram, R5900Context* ctx, PS2Runtime* runtime,
                                 const std::shared_ptr<ThreadInfo>& info)
    {
        runtime->gate3Scheduler.beginExit(1);
        runExitHandlersForThread(1, rdram, ctx, runtime);
        if (info) {
            {
                std::lock_guard lock(info->m);
                info->started = false; info->status = THS_DORMANT;
                info->waitType = TSW_NONE; info->waitId = 0;
                info->suspendCount = 0; info->wakeupCount = 0;
            }
            for (auto& ticket : info->gate3Joiners) gate3Resolve(ticket, runtime, KE_OK);
            info->gate3Joiners.clear();
        }
        runtime->gate3Scheduler.dormant(1);
    }

    void FlushCache(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, KE_OK);
    }

    void iFlushCache(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        FlushCache(rdram, ctx, runtime);
    }

    void EnableCache(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, KE_OK);
    }

    void DisableCache(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, KE_OK);
    }

    void ResetEE(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::cerr << "Syscall: ResetEE - requesting runtime stop" << std::endl;
        // runtime->requestStop();
        setReturnS32(ctx, KE_OK);
    }

    void SetMemoryMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, KE_OK);
    }

    void InitThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // This is a common ps2sdk helper that some games link against.
        setReturnS32(ctx, 1);
    }

    void CreateThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t paramAddr = getRegU32(ctx, 4); // $a0 points to ThreadParam
        if (paramAddr == 0u)
        {
            std::cerr << "CreateThread error: null ThreadParam pointer" << std::endl;
            setReturnS32(ctx, KE_ERROR);
            return;
        }

        const uint32_t *param = reinterpret_cast<const uint32_t *>(getConstMemPtr(rdram, paramAddr));

        if (!param)
        {
            std::cerr << "CreateThread error: invalid ThreadParam address 0x" << std::hex << paramAddr << std::dec << std::endl;
            setReturnS32(ctx, KE_ERROR);
            return;
        }

        auto info = std::make_shared<ThreadInfo>();
        info->attr = param[0];
        info->entry = param[1];
        info->stack = param[2];
        info->stackSize = param[3];

        auto looksLikeGuestPtr = [](uint32_t v) -> bool
        {
            if (v == 0)
            {
                return true;
            }
            const uint32_t norm = v & 0x1FFFFFFFu;
            return norm < PS2_RAM_SIZE && norm >= 0x10000u;
        };

        auto looksLikePriority = [](uint32_t v) -> bool
        {
            // Typical EE priorities are very small integers (1..127).
            return v <= 0x400u;
        };

        const uint32_t gpA = param[4];
        const uint32_t prioA = param[5];
        const uint32_t gpB = param[5];
        const uint32_t prioB = param[4];

        // Prefer the standard EE layout (gp at +0x10, priority at +0x14),
        // but keep a fallback for callsites that used the swapped decode.
        if (looksLikeGuestPtr(gpA) && looksLikePriority(prioA))
        {
            info->gp = gpA;
            info->priority = prioA;
        }
        else if (looksLikeGuestPtr(gpB) && looksLikePriority(prioB))
        {
            info->gp = gpB;
            info->priority = prioB;
        }
        else
        {
            info->gp = gpA;
            info->priority = prioA;
        }

        info->option = param[6];
        if (info->priority == 0)
        {
            info->priority = 1;
        }
        if (info->priority >= 128)
        {
            info->priority = 127;
        }
        info->currentPriority = static_cast<int>(info->priority);

        int id = 0;
        {
            std::lock_guard<std::mutex> lock(g_thread_map_mutex);
            // Keep IDs in the classic low range used by patched libkernel helpers.
            for (int attempts = 0; attempts < 0xFE; ++attempts)
            {
                if (g_nextThreadId < 2 || g_nextThreadId > 0xFF)
                {
                    g_nextThreadId = 2;
                }

                const int candidate = g_nextThreadId;
                g_nextThreadId = (g_nextThreadId >= 0xFF) ? 2 : (g_nextThreadId + 1);

                if (g_threads.find(candidate) == g_threads.end())
                {
                    id = candidate;
                    break;
                }
            }

            if (id == 0)
            {
                setReturnS32(ctx, KE_ERROR);
                return;
            }

            g_threads[id] = info;
        }

        RUNTIME_LOG("[CreateThread] id=" << id
                                         << " entry=0x" << std::hex << info->entry
                                         << " stack=0x" << info->stack
                                         << " size=0x" << info->stackSize
                                         << " gp=0x" << info->gp
                                         << " prio=" << std::dec << info->priority << std::endl);

        setReturnS32(ctx, id);
    }

    void DeleteThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        int tid = static_cast<int>(getRegU32(ctx, 4)); // $a0
        if (tid == 0)
        {
            setReturnS32(ctx, KE_ILLEGAL_THID);
            return;
        }

        auto info = lookupThreadInfo(tid);
        if (!info)
        {
            setReturnS32(ctx, KE_UNKNOWN_THID);
            return;
        }

        uint32_t autoStackToFree = 0;
        {
            std::lock_guard<std::mutex> lock(info->m);
            if (info->started || info->status != THS_DORMANT)
            {
                setReturnS32(ctx, KE_NOT_DORMANT);
                return;
            }

            if (info->ownsStack && info->stack != 0)
            {
                autoStackToFree = info->stack;
                info->stack = 0;
                info->stackSize = 0;
                info->ownsStack = false;
            }
        }

        {
            std::lock_guard<std::mutex> lock(g_thread_map_mutex);
            g_threads.erase(tid);
        }

        {
            std::lock_guard<std::mutex> lock(g_exit_handler_mutex);
            g_exit_handlers.erase(tid);
        }

        if (runtime && autoStackToFree != 0)
        {
            runtime->guestFree(autoStackToFree);
        }

        setReturnS32(ctx, KE_OK);
    }

    void StartThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        int tid = static_cast<int>(getRegU32(ctx, 4)); // $a0 = thread id
        uint32_t arg = getRegU32(ctx, 5);              // $a1 = user arg
        if (tid == 0)
        {
            setReturnS32(ctx, KE_ILLEGAL_THID);
            return;
        }

        auto info = lookupThreadInfo(tid);
        if (!info)
        {
            std::cerr << "StartThread error: unknown thread id " << tid << std::endl;
            setReturnS32(ctx, KE_UNKNOWN_THID);
            return;
        }

        if (!runtime || !runtime->hasFunction(info->entry))
        {
            if (runtime)
            {
                const auto rrvTerminalOutcome = runtime->terminalOutcomeHandle();
                if (rrvTerminalOutcome)
                {
                    rrv::guestoutcome::recordFailure(
                        *rrvTerminalOutcome, rrv::guestoutcome::TerminalKind::RejectedWorkerDispatch,
                        {info->entry, GPR_U32(ctx, 31), GPR_U32(ctx, 29)},
                        "unregistered StartThread initial entry");
                }
                runtime->requestStop();
            }
            setReturnS32(ctx, KE_ERROR);
            return;
        }
        if (runtime->isStopRequested())
        {
            setReturnS32(ctx, KE_ERROR);
            return;
        }

        joinHostThreadById(tid);

        const uint32_t callerSp = getRegU32(ctx, 29);
        const uint32_t callerGp = getRegU32(ctx, 28);

        {
            std::lock_guard<std::mutex> lock(info->m);
            if (info->started || info->status != THS_DORMANT)
            {
                setReturnS32(ctx, KE_NOT_DORMANT);
                return;
            }

            info->started = true;
            info->status = THS_READY;
            info->arg = arg;
            info->terminated = false;
            info->forceRelease = false;
            info->waitType = TSW_NONE;
            info->waitId = 0;
            info->wakeupCount = 0;
            info->suspendCount = 0;
            if (info->stack == 0 && info->stackSize != 0)
            {
                const uint32_t autoStack = runtime->guestMalloc(info->stackSize, 16u);
                if (autoStack != 0)
                {
                    info->stack = autoStack;
                    info->ownsStack = true;
                    RUNTIME_LOG("[StartThread] id=" << tid
                                                    << " auto-stack=0x" << std::hex << autoStack
                                                    << " size=0x" << info->stackSize << std::dec << std::endl);
                }
            }

            if (info->stack != 0 && info->stackSize == 0)
            {
                // Some games leave size zero in the thread param even though a stack
                // buffer is supplied; use a conservative default instead of caller SP.
                info->stackSize = 0x800u;
            }
        }

        const auto rrvTerminalOutcome = runtime->terminalOutcomeHandle();
        if (!runtime->gate3Scheduler.start(tid, info->currentPriority)) {
            std::lock_guard lock(info->m); info->started = false; info->status = THS_DORMANT;
            setReturnS32(ctx, KE_DORMANT); return;
        }
        g_activeThreads.fetch_add(1, std::memory_order_relaxed);
        try
        {
            std::thread worker([=]() mutable
                               {
            {
                std::string name = "PS2Thread_" + std::to_string(tid);
                ThreadNaming::SetCurrentThreadName(name);
            }
            g_currentThreadId = tid;
            PS2Runtime::GuestExecutionScope gate3Lifetime(runtime);
            R5900Context threadCtxCopy{};
            R5900Context *threadCtx = &threadCtxCopy;

            {
                std::lock_guard<std::mutex> lock(info->m);
                info->status = THS_RUN;
            }

            uint32_t threadSp = callerSp;
            if (info->stack)
            {
                const uint32_t stackSize = (info->stackSize != 0) ? info->stackSize : 0x800u;
                threadSp = (info->stack + stackSize) & ~0xFu;
            }
            uint32_t threadGp = info->gp;
            const uint32_t normalizedGp = threadGp & 0x1FFFFFFFu;
            if (threadGp == 0 || normalizedGp < 0x10000u || normalizedGp >= PS2_RAM_SIZE)
            {
                threadGp = callerGp;
            }

            SET_GPR_U32(threadCtx, 29, threadSp);
            SET_GPR_U32(threadCtx, 28, threadGp);
            SET_GPR_U32(threadCtx, 4, info->arg);
            SET_GPR_U32(threadCtx, 31, 0);
            threadCtx->pc = info->entry;

            g_currentThreadId = tid;

            // Developer snapshots (docs/SNAPSHOTS.md §7): publish this worker's
            // context, which otherwise exists only on this host stack, so a
            // capture can see that the thread is live and record it. Read-only
            // from the capturing thread, and only while it holds the guest
            // execution mutex. No-op unless a snapshot was armed.
            if (rrv::snapshot::enabled())
            {
                rrv::snapshot::hookGuestThreadStart(static_cast<int32_t>(tid), threadCtx);
            }

            RUNTIME_LOG("[StartThread] id=" << tid
                      << " entry=0x" << std::hex << info->entry
                      << " sp=0x" << GPR_U32(threadCtx, 29)
                      << " gp=0x" << GPR_U32(threadCtx, 28)
                      << " arg=0x" << info->arg << std::dec << std::endl);

            bool exited = false;
            try
            {
                uint32_t lastPc = 0xFFFFFFFFu;
                uint32_t samePcCount = 0;
                constexpr uint32_t kSamePcYieldMask = 0x3FFFu;
                constexpr uint32_t kSamePcWarnInterval = 0x20000u;
                uint64_t stepCount = 0u;

                while (runtime && !runtime->isStopRequested())
                {
                    ++stepCount;
                    if (info->terminated.load(std::memory_order_relaxed))
                    {
                        throw ThreadExitException();
                    }

                    waitWhileSuspended(info, runtime);

                    const uint32_t pc = threadCtx->pc;
                    if (pc == 0u)
                    {
                        break;
                    }

                    if ((stepCount & 0x1FFFFFu) == 0u)
                    {
                        RUNTIME_LOG("[StartThread] id=" << tid
                                  << " heartbeat pc=0x" << std::hex << pc
                                  << " ra=0x" << GPR_U32(threadCtx, 31)
                                  << " sp=0x" << GPR_U32(threadCtx, 29)
                                  << " gp=0x" << GPR_U32(threadCtx, 28)
                                  << std::dec << std::endl);
                    }

                    if (pc == lastPc)
                    {
                        ++samePcCount;
                        if ((samePcCount & kSamePcYieldMask) == 0u)
                        {
                            std::this_thread::yield();
                        }
                        if ((samePcCount % kSamePcWarnInterval) == 0u)
                        {
                            RUNTIME_LOG("[StartThread] id=" << tid
                                      << " spinning at pc=0x" << std::hex << pc
                                      << " ra=0x" << GPR_U32(threadCtx, 31)
                                      << std::dec << std::endl);
                        }
                    }
                    else
                    {
                        samePcCount = 0;
                        lastPc = pc;
                    }

                    if (!runtime->hasFunction(pc))
                    {
                        rrv::guestoutcome::recordFailure(
                            *rrvTerminalOutcome,
                            rrv::guestoutcome::TerminalKind::RejectedWorkerDispatch,
                            {pc, GPR_U32(threadCtx, 31), GPR_U32(threadCtx, 29)},
                            "unregistered StartThread later dispatch target");
                        runtime->requestStop();
                        std::cerr << "[StartThread] id=" << tid << " missing function for pc=0x"
                                  << std::hex << pc << std::dec << std::endl;
                        throw ThreadExitException();
                    }
                    PS2Runtime::RecompiledFunction step = runtime->lookupFunction(pc);
                    if (!step)
                    {
                        rrv::guestoutcome::recordFailure(
                            *rrvTerminalOutcome,
                            rrv::guestoutcome::TerminalKind::WorkerGuestException,
                            {pc, GPR_U32(threadCtx, 31), GPR_U32(threadCtx, 29)},
                            "registered StartThread dispatch had no callable step");
                        runtime->requestStop();
                        throw ThreadExitException();
                    }
                    {
                        PS2Runtime::GuestExecutionScope guestExecution(runtime);
                        step(rdram, threadCtx, runtime);
                    }
                }
            }
            catch (const ThreadExitException &)
            {
                exited = true;
            }
            catch (const rrv::guest_time::TemporalCut&) { runtime->requestStop(); }
            catch (const std::exception &e)
            {
                rrv::guestoutcome::recordFailure(
                    *rrvTerminalOutcome, rrv::guestoutcome::TerminalKind::WorkerGuestException,
                    {threadCtx->pc, GPR_U32(threadCtx, 31), GPR_U32(threadCtx, 29)}, e.what());
                runtime->requestStop();
                std::cerr << "[StartThread] id=" << tid << " exception: " << e.what() << std::endl;
            }

            if (!exited)
            {
                RUNTIME_LOG("[StartThread] id=" << tid << " returned (pc=0x"
                          << std::hex << threadCtx->pc << std::dec << ")" << std::endl);
            }

            runtime->gate3Scheduler.beginExit(tid);
            runExitHandlersForThread(tid, rdram, threadCtx, runtime);

            // Developer snapshots: exit handlers were the last guest code this
            // worker runs; threadCtx dies with this stack frame, so retract the
            // pointer before anything else can observe it.
            if (rrv::snapshot::enabled())
            {
                rrv::snapshot::hookGuestThreadExit(static_cast<int32_t>(tid));
            }

            uint32_t detachedAutoStack = 0;
            {
                std::lock_guard<std::mutex> lock(info->m);
                info->started = false;
                info->status = THS_DORMANT;
                info->waitType = TSW_NONE;
                info->waitId = 0;
                info->wakeupCount = 0;
                info->suspendCount = 0;
                info->forceRelease = false;
                info->terminated = false;
            }

            bool stillRegistered = false;
            {
                std::lock_guard<std::mutex> lock(g_thread_map_mutex);
                stillRegistered = (g_threads.find(tid) != g_threads.end());
            }
            if (!stillRegistered)
            {
                // ExitDeleteThread removes the record immediately; reclaim auto stack here.
                std::lock_guard<std::mutex> lock(info->m);
                if (info->ownsStack && info->stack != 0)
                {
                    detachedAutoStack = info->stack;
                    info->stack = 0;
                    info->stackSize = 0;
                    info->ownsStack = false;
                }
            }

            if (detachedAutoStack != 0 && runtime)
            {
                runtime->guestFree(detachedAutoStack);
            }

            for (auto& ticket : info->gate3Joiners) gate3Resolve(ticket, runtime, KE_OK);
            info->gate3Joiners.clear();
            runtime->gate3Scheduler.dormant(tid);
            // Notify anybody waiting for termination (like TerminateThread)
            info->cv.notify_all();

            g_activeThreads.fetch_sub(1, std::memory_order_relaxed); });
            registerHostThread(tid, std::move(worker));
        }
        catch (const std::exception &e)
        {
            std::cerr << "[StartThread] failed to spawn host thread for tid=" << tid << ": " << e.what() << std::endl;
            runtime->gate3Scheduler.dormant(tid);
            g_activeThreads.fetch_sub(1, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lock(info->m);
            info->started = false;
            info->status = THS_DORMANT;
            info->waitType = TSW_NONE;
            info->waitId = 0;
            info->wakeupCount = 0;
            info->suspendCount = 0;
            info->forceRelease = false;
            info->terminated = false;
            setReturnS32(ctx, KE_ERROR);
            return;
        }

        setReturnS32(ctx, KE_OK);
    }

    void ExitThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        RUNTIME_LOG("[ExitThread] Game requested thread exit! PC=0x" << std::hex << ctx->pc
                                                                     << " RA=0x" << getRegU32(ctx, 31) << std::dec << " tid=" << g_currentThreadId << std::endl);

        runExitHandlersForThread(g_currentThreadId, rdram, ctx, runtime);
        auto info = ensureCurrentThreadInfo(ctx);
        if (info)
        {
            std::lock_guard<std::mutex> lock(info->m);
            info->terminated = true;
            info->forceRelease = true;
            info->waitType = TSW_NONE;
            info->waitId = 0;
            info->wakeupCount = 0;
        }
        if (info)
        {
            info->cv.notify_all();
        }
        throw ThreadExitException();
    }

    void ExitDeleteThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        int tid = g_currentThreadId;
        RUNTIME_LOG("[ExitDeleteThread] Game requested thread exit & delete! PC=0x" << std::hex << ctx->pc
                                                                                    << " RA=0x" << getRegU32(ctx, 31) << std::dec << " tid=" << tid << std::endl);

        runExitHandlersForThread(tid, rdram, ctx, runtime);
        auto info = ensureCurrentThreadInfo(ctx);
        if (info)
        {
            std::lock_guard<std::mutex> lock(info->m);
            info->terminated = true;
            info->forceRelease = true;
            info->waitType = TSW_NONE;
            info->waitId = 0;
            info->wakeupCount = 0;
        }
        if (info)
        {
            info->cv.notify_all();
        }
        {
            std::lock_guard<std::mutex> lock(g_thread_map_mutex);
            g_threads.erase(tid);
        }
        throw ThreadExitException();
    }

    void TerminateThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        int tid = static_cast<int>(getRegU32(ctx, 4));
        if (!tid) tid = g_currentThreadId;
        auto info = tid == g_currentThreadId ? ensureCurrentThreadInfo(ctx) : lookupThreadInfo(tid);
        if (!info) { setReturnS32(ctx, KE_UNKNOWN_THID); return; }
        { std::lock_guard lock(info->m);
          if (info->status == THS_DORMANT) { setReturnS32(ctx, KE_DORMANT); return; }
          info->terminated = true; }
        gate3Cancel(tid, runtime, true);
        if (tid == g_currentThreadId) throw ThreadExitException();
        auto ticket = gate3Wait(ctx, runtime, 4, tid);
        info->gate3Joiners.push_back(ticket);
        gate3Park(ticket, runtime);
        setReturnS32(ctx, ticket->result);
    }

    void SuspendThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        int tid = static_cast<int>(getRegU32(ctx, 4));
        if (!tid) tid = g_currentThreadId;
        auto info = tid == g_currentThreadId ? ensureCurrentThreadInfo(ctx) : lookupThreadInfo(tid);
        if (!info) { setReturnS32(ctx, KE_UNKNOWN_THID); return; }
        { std::lock_guard lock(info->m);
          if (info->status == THS_DORMANT) { setReturnS32(ctx, KE_DORMANT); return; }
          ++info->suspendCount; applySuspendStatusLocked(*info); }
        runtime->gate3Scheduler.suspend(tid);
        if (tid == g_currentThreadId) {
            runtime->gate3CheckpointSchedulerV1();
            throwIfTerminated(info);
        }
        setReturnS32(ctx, KE_OK);
    }

    void ResumeThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        int tid = static_cast<int>(getRegU32(ctx, 4));
        if (!tid) tid = g_currentThreadId;
        auto info = lookupThreadInfo(tid);
        if (!info) { setReturnS32(ctx, KE_UNKNOWN_THID); return; }
        { std::lock_guard lock(info->m);
          if (info->status == THS_DORMANT) { setReturnS32(ctx, KE_DORMANT); return; }
          if (!info->suspendCount) { setReturnS32(ctx, KE_NOT_SUSPEND); return; }
          if (--info->suspendCount == 0) info->status = info->waitType ? THS_WAIT : THS_READY; }
        runtime->gate3Scheduler.resume(tid);
        setReturnS32(ctx, KE_OK);
    }

    void GetThreadId(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, g_currentThreadId);
    }

    void ReferThreadStatus(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        int tid = static_cast<int>(getRegU32(ctx, 4));
        uint32_t statusAddr = getRegU32(ctx, 5);

        if (tid == 0) // TH_SELF
        {
            tid = g_currentThreadId;
        }

        auto info = (tid == g_currentThreadId) ? ensureCurrentThreadInfo(ctx) : lookupThreadInfo(tid);
        if (!info)
        {
            setReturnS32(ctx, KE_UNKNOWN_THID);
            return;
        }

        ee_thread_status_t *status = reinterpret_cast<ee_thread_status_t *>(getMemPtr(rdram, statusAddr));
        if (!status)
        {
            setReturnS32(ctx, KE_ERROR);
            return;
        }

        std::lock_guard<std::mutex> lock(info->m);
        status->status = runtime->gate3Scheduler.status(tid);
        status->func = info->entry;
        status->stack = info->stack;
        status->stack_size = info->stackSize;
        status->gp_reg = info->gp;
        status->initial_priority = info->priority;
        status->current_priority = info->currentPriority;
        status->attr = info->attr;
        status->option = info->option;
        status->waitType = info->waitType;
        status->waitId = info->waitId;
        status->wakeupCount = info->wakeupCount;
        setReturnS32(ctx, KE_OK);
    }

    void iReferThreadStatus(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ReferThreadStatus(rdram, ctx, runtime);
    }

    void SleepThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        auto info = ensureCurrentThreadInfo(ctx);
        throwIfTerminated(info);
        { std::lock_guard lock(info->m);
          if (info->wakeupCount) { --info->wakeupCount; setReturnS32(ctx, KE_OK); return; } }
        auto ticket = gate3Wait(ctx, runtime, TSW_SLEEP, 0);
        gate3Park(ticket, runtime);
        setReturnS32(ctx, ticket->result);
    }

    void WakeupThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int tid = static_cast<int>(getRegU32(ctx, 4));
        if (!tid || tid == g_currentThreadId) { setReturnS32(ctx, KE_ILLEGAL_THID); return; }
        auto info = lookupThreadInfo(tid);
        if (!info) { setReturnS32(ctx, KE_UNKNOWN_THID); return; }
        bool sleep;
        { std::lock_guard lock(info->m);
          if (info->status == THS_DORMANT) { setReturnS32(ctx, KE_DORMANT); return; }
          sleep = info->waitType == TSW_SLEEP;
          if (!sleep) ++info->wakeupCount;
          else { info->waitType = TSW_NONE; info->waitId = 0;
                 info->status = info->suspendCount ? THS_SUSPEND : THS_READY; } }
        if (sleep) runtime->gate3Scheduler.cancel(tid, KE_OK);
        setReturnS32(ctx, KE_OK);
    }

    void iWakeupThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        WakeupThread(rdram, ctx, runtime);
    }

    void CancelWakeupThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        int tid = static_cast<int>(getRegU32(ctx, 4));
        if (tid == 0)
            tid = g_currentThreadId;

        auto info = (tid == g_currentThreadId) ? ensureCurrentThreadInfo(ctx) : lookupThreadInfo(tid);
        if (!info)
        {
            setReturnS32(ctx, KE_UNKNOWN_THID);
            return;
        }

        int previous = 0;
        {
            std::lock_guard<std::mutex> lock(info->m);
            previous = info->wakeupCount;
            info->wakeupCount = 0;
        }
        setReturnS32(ctx, previous);
    }

    void iCancelWakeupThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        int tid = static_cast<int>(getRegU32(ctx, 4));
        if (tid == 0)
        {
            setReturnS32(ctx, KE_ILLEGAL_THID);
            return;
        }

        auto info = lookupThreadInfo(tid);
        if (!info)
        {
            setReturnS32(ctx, KE_UNKNOWN_THID);
            return;
        }

        int previous = 0;
        {
            std::lock_guard<std::mutex> lock(info->m);
            previous = info->wakeupCount;
            info->wakeupCount = 0;
        }
        setReturnS32(ctx, previous);
    }

    void ChangeThreadPriority(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        int tid = static_cast<int>(getRegU32(ctx, 4));
        int newPrio = static_cast<int>(getRegU32(ctx, 5));

        if (tid == 0)
            tid = g_currentThreadId;

        auto info = (tid == g_currentThreadId) ? ensureCurrentThreadInfo(ctx) : lookupThreadInfo(tid);
        if (!info)
        {
            setReturnS32(ctx, KE_UNKNOWN_THID);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(info->m);
            if (info->status == THS_DORMANT)
            {
                setReturnS32(ctx, KE_DORMANT);
                return;
            }

            if (newPrio == 0)
            {
                newPrio = (info->currentPriority > 0) ? info->currentPriority : 1;
            }
            if (newPrio <= 0 || newPrio >= 128)
            {
                setReturnS32(ctx, KE_ILLEGAL_PRIORITY);
                return;
            }

            info->currentPriority = newPrio;
            runtime->gate3Scheduler.priority(tid, newPrio);
        }

        setReturnS32(ctx, KE_OK);
    }

    void iChangeThreadPriority(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ChangeThreadPriority(rdram, ctx, runtime);
    }

    void RotateThreadReadyQueue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        int priority = static_cast<int>(getRegU32(ctx, 4));
        if (!priority) {
            auto info = ensureCurrentThreadInfo(ctx);
            std::lock_guard lock(info->m);
            priority = info->currentPriority;
        }
        if (priority < 0 || priority >= 128) { setReturnS32(ctx, KE_ILLEGAL_PRIORITY); return; }
        runtime->gate3CheckpointSchedulerV1(priority);
        setReturnS32(ctx, KE_OK);
    }

    void iRotateThreadReadyQueue(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        RotateThreadReadyQueue(rdram, ctx, runtime);
    }

    void ReleaseWaitThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int tid = static_cast<int>(getRegU32(ctx, 4));
        if (!tid || tid == g_currentThreadId) { setReturnS32(ctx, KE_ILLEGAL_THID); return; }
        auto info = lookupThreadInfo(tid);
        if (!info) { setReturnS32(ctx, KE_UNKNOWN_THID); return; }
        { std::lock_guard lock(info->m);
          if (info->waitType == TSW_NONE) { setReturnS32(ctx, KE_NOT_WAIT); return; } }
        gate3Cancel(tid, runtime, false);
        setReturnS32(ctx, KE_OK);
    }

    void iReleaseWaitThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ReleaseWaitThread(rdram, ctx, runtime);
    }
}
