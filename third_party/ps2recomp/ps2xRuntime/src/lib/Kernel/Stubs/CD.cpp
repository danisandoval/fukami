#include "Common.h"
#include "CD.h"

namespace ps2_stubs
{
    namespace
    {
        bool gate3ReadCdHost(uint32_t lbn, uint32_t sectors, uint8_t* destination,
                            size_t bytes, PS2Runtime* runtime, const char* route)
        {
            runtime->gate3DiagCdHostV1(); // Qualification-only host delay/miss injection; off by default.
            for (;;) {
                if (readCdSectors(lbn, sectors, destination, bytes)) return true;
                const bool resolvable = sectors == 0 ||
                    (isResolvableCdLbn(lbn) && isResolvableCdLbn(lbn + sectors - 1));
                if (!runtime->gate3ShouldWaitCdHostV1(route, lbn, sectors, resolvable)) return false;
                runtime->gate3WaitCdHostV1(); // Host wait; guest virtual time is unchanged.
            }
        }

        // CD read-completion callback registered by the game via sceCdCallback.
        // On real hardware the CDVD interrupt invokes it after each async read
        // finishes; RRV's handler (0x296EB0) advances its stream read-completion
        // queue only when the reason argument == 1 (sceCdFuncRead), which is what
        // drains pending stream descriptors. We deliver it deferred from the VBlank
        // ISR (mirroring dispatchGsSyncVCallback) to avoid completing a read before
        // the game has finished recording the request.
        std::atomic<uint32_t> g_cdReadCbFunc{0u};
        std::atomic<uint32_t> g_cdReadCbGp{0u};
        std::atomic<uint32_t> g_cdReadCbStackTop{0u};
        std::atomic<int32_t> g_cdReadCbPending{0};

        void invokeCdReadCallback(uint8_t *rdram, PS2Runtime *runtime, int32_t reason)
        {
            const uint32_t cb = g_cdReadCbFunc.load(std::memory_order_relaxed);
            if (cb == 0u || !runtime->hasFunction(cb))
            {
                return;
            }

            uint32_t stackTop = g_cdReadCbStackTop.load(std::memory_order_relaxed);
            if (stackTop == 0u)
            {
                const uint32_t top = runtime->reserveAsyncCallbackStack(0x4000u, 16u);
                if (top != 0u)
                {
                    g_cdReadCbStackTop.store(top, std::memory_order_relaxed);
                    stackTop = top;
                }
            }

            if (stackTop == 0u)
            {
                std::cerr << "[CD] async callback-stack reservation failed" << std::endl;
                runtime->requestStop();
                return;
            }

            try
            {
                R5900Context cbCtx{};
                SET_GPR_U32(&cbCtx, 28, g_cdReadCbGp.load(std::memory_order_relaxed));
                SET_GPR_U32(&cbCtx, 29, stackTop);
                SET_GPR_U32(&cbCtx, 31, 0u);
                SET_GPR_U32(&cbCtx, 4, static_cast<uint32_t>(reason));
                cbCtx.pc = cb;

                uint32_t steps = 0u;
                while (cbCtx.pc != 0u && !runtime->isStopRequested() && steps < 8192u)
                {
                    if (!runtime->hasFunction(cbCtx.pc))
                    {
                        cbCtx.pc = 0u;
                        break;
                    }
                    auto step = runtime->lookupFunction(cbCtx.pc);
                    if (!step)
                    {
                        break;
                    }
                    ++steps;
                    step(rdram, &cbCtx, runtime);
                }
            }
            catch (const std::exception &)
            {
            }
        }
    } // namespace

    // Called once per VBlank from the interrupt worker: deliver any CD read
    // completions queued since the last tick (reason 1 == sceCdFuncRead).
    //
    // invokeCdReadCallback() always (re-)enters the registered handler at its
    // TOP-LEVEL entry point with a fresh, zeroed R5900Context -- it has no
    // notion of "the next queued item", it just runs the guest's own
    // completion scan once (RRV's handler walks its own fixed set of stream
    // descriptor slots start to finish every time it is called; see
    // sub_0029B580 / config/output for the double loop). Calling it `pending`
    // times back-to-back within the SAME vblank therefore does not drain
    // `pending` distinct items -- it re-runs that same full scan up to 64
    // times with zero elapsed guest time between runs, which real CDVD
    // interrupt delivery never does (one interrupt per completed transfer,
    // spread over real transfer latency). That burst was reproducibly
    // observed to corrupt guest state and hand a garbage value to the EE
    // dispatcher as a jump target (docs/instrumentation, "CD-callback burst
    // dispatch" investigation, 2026-08-22) -- ~50% hit rate under
    // RRV_FRONTEND_DIAG=1 solo, always preceded by "Warning: Function at
    // address 0x... not found" cycling through the same handler's call graph
    // (0x296710/0x2967d8/0x29b668). dispatchGsSyncVCallback (this function's
    // own doc-comment model) is never called more than once per tick either.
    // Match that: deliver at most one invocation per tick regardless of how
    // many reads completed since the last one -- the handler's own internal
    // scan is what actually drains the backlog.
    void pumpCdReadCallbacks(uint8_t *, PS2Runtime *)
    {
        throw std::logic_error("Gate3 legacy CD callback pump is disabled");
    }

    static void sceCdReadRouted(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime,
                                bool iopDestination)
    {
        frontendDiagInit();
        frontendDiagCountSif("sceCdRead");
        frontendDiagInit();
        const uint32_t a0 = getRegU32(ctx, 4); // usually lbn
        const uint32_t a1 = getRegU32(ctx, 5); // usually sector count
        const uint32_t a2 = getRegU32(ctx, 6); // usually destination buffer
        frontendDiagCountCd("sceCdRead");
        frontendDiagAddCdRead(a0, a1, a2);

        struct CdReadArgs
        {
            uint32_t lbn = 0;
            uint32_t sectors = 0;
            uint32_t buf = 0;
            const char *tag = "";
        };

        std::vector<uint8_t> completedPayload;
        auto tryRead = [&](const CdReadArgs &args) -> bool
        {
            const uint64_t requested = static_cast<uint64_t>(args.sectors) * kCdSectorSize;
            const uint32_t addr = iopDestination ? (args.buf & ~0x40000000u)
                                                 : (args.buf & PS2_RAM_MASK);
            if (requested > PS2_RAM_SIZE ||
                (iopDestination ? !ownsIopHeapRange(addr, requested)
                                : requested > PS2_RAM_SIZE - addr))
            {
                g_lastCdError = -1;
                return false;
            }
            const size_t bytes = static_cast<size_t>(requested);
            if (bytes == 0)
            {
                return true;
            }
            std::vector<uint8_t> staged(bytes);
            if (!gate3ReadCdHost(args.lbn, args.sectors, staged.data(), bytes, runtime,
                                 iopDestination ? "sceCdReadIOPm" : "sceCdRead"))
            {
                return false;
            }
            if (iopDestination)
            {
                if (!writeIopHeapBytes(addr, staged.data(), bytes))
                {
                    g_lastCdError = -1;
                    return false;
                }
            }
            else
            {
                std::memcpy(rdram + addr, staged.data(), bytes);
            }
            completedPayload = std::move(staged);
            return true;
        };

        CdReadArgs selected{a0, a1, a2, "a0/a1/a2"};
        bool ok = tryRead(selected);

        if (!ok)
        {
            // Some game-side wrappers use a nonstandard register layout.
            // If primary decode does not resolve to a known LBN, try safe alternatives.
            constexpr uint32_t kMaxReasonableSectors = PS2_RAM_SIZE / kCdSectorSize;
            if (!isResolvableCdLbn(selected.lbn))
            {
                const std::array<CdReadArgs, 5> alternatives = {
                    CdReadArgs{a2, a1, a0, "a2/a1/a0"},
                    CdReadArgs{a0, a2, a1, "a0/a2/a1"},
                    CdReadArgs{a1, a0, a2, "a1/a0/a2"},
                    CdReadArgs{a1, a2, a0, "a1/a2/a0"},
                    CdReadArgs{a2, a0, a1, "a2/a0/a1"}};

                for (const CdReadArgs &candidate : alternatives)
                {
                    if (candidate.sectors > kMaxReasonableSectors)
                    {
                        continue;
                    }
                    if (!isResolvableCdLbn(candidate.lbn))
                    {
                        continue;
                    }

                    if (tryRead(candidate))
                    {
                        static uint32_t recoverLogCount = 0;
                        if (recoverLogCount < 16)
                        {
                            RUNTIME_LOG("[sceCdRead] recovered with alternate args " << candidate.tag
                                                                                     << " (pc=0x" << std::hex << ctx->pc
                                                                                     << " ra=0x" << getRegU32(ctx, 31)
                                                                                     << " a0=0x" << a0
                                                                                     << " a1=0x" << a1
                                                                                     << " a2=0x" << a2 << std::dec << ")" << std::endl);
                            ++recoverLogCount;
                        }
                        selected = candidate;
                        ok = true;
                        break;
                    }
                }
            }

            if (!ok)
            {
                static uint32_t unresolvedLogCount = 0;
                if (unresolvedLogCount < 32)
                {
                    std::cerr << "[sceCdRead] unresolved request pc=0x" << std::hex << ctx->pc
                              << " ra=0x" << getRegU32(ctx, 31)
                              << " a0=0x" << a0
                              << " a1=0x" << a1
                              << " a2=0x" << a2 << std::dec << std::endl;
                    ++unresolvedLogCount;
                }
            }
        }

        runtime->gate3RecordCdReadV1(
            iopDestination ? "sceCdReadIOPm" : "sceCdRead",
            selected.lbn, selected.sectors, iopDestination, ok, completedPayload);
        if (ok)
        {
            g_cdStreamingLbn = selected.lbn + selected.sectors;
            setReturnS32(ctx, 1); // command accepted/success
            return;
        }

        setReturnS32(ctx, 0);
    }

    void sceCdRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        sceCdReadRouted(rdram, ctx, runtime, false);
    }

    void sceCdSync(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        frontendDiagInit();
        frontendDiagCountSif("sceCdSync");
        setReturnS32(ctx, 0); // 0 = completed/not busy
    }

    void sceCdGetError(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        frontendDiagInit();
        frontendDiagCountSif("sceCdGetError");
        setReturnS32(ctx, g_lastCdError);
    }

    void sceCdRI(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        frontendDiagInit();
        frontendDiagCountSif("sceCdRI");
        TODO_NAMED("sceCdRI", rdram, ctx, runtime);
    }

    void sceCdRM(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        frontendDiagInit();
        frontendDiagCountSif("sceCdRM");
        TODO_NAMED("sceCdRM", rdram, ctx, runtime);
    }

    void sceCdApplyNCmd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        frontendDiagInit();
        frontendDiagCountSif("sceCdApplyNCmd");
        setReturnS32(ctx, 1);
    }

    void sceCdBreak(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        frontendDiagInit();
        frontendDiagCountSif("sceCdBreak");
        setReturnS32(ctx, 1);
    }

    void sceCdCallback(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        frontendDiagInit();
        frontendDiagCountCd("sceCdCallback");
        // Register the EE-side CD completion handler. Returns the previous one.
        const uint32_t func = getRegU32(ctx, 4);
        const uint32_t prev = g_cdReadCbFunc.exchange(func, std::memory_order_relaxed);
        g_cdReadCbGp.store(getRegU32(ctx, 28), std::memory_order_relaxed);
        runtime->gate3SetCdCallbackV1(func, getRegU32(ctx, 28));
        RUNTIME_LOG("[sceCdCallback] registered func=0x" << std::hex << func
                                                         << " gp=0x" << getRegU32(ctx, 28) << std::dec << std::endl);
        setReturnS32(ctx, static_cast<int32_t>(prev));
    }

    void sceCdChangeThreadPriority(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        frontendDiagInit();
        frontendDiagCountSif("sceCdChangeThreadPriority");
        setReturnS32(ctx, 1);
    }

    void sceCdDelayThread(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        frontendDiagInit();
        frontendDiagCountSif("sceCdDelayThread");
        setReturnS32(ctx, 0);
    }

    void sceCdDiskReady(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        frontendDiagInit();
        frontendDiagCountSif("sceCdDiskReady");
        setReturnS32(ctx, 2);
    }

    void sceCdGetDiskType(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        frontendDiagInit();
        frontendDiagCountSif("sceCdGetDiskType");
        // SCECdPS2DVD
        setReturnS32(ctx, 0x14);
    }

    void sceCdGetReadPos(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        frontendDiagInit();
        frontendDiagCountSif("sceCdGetReadPos");
        setReturnU32(ctx, g_cdStreamingLbn);
    }

    void sceCdGetToc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        frontendDiagInit();
        frontendDiagCountSif("sceCdGetToc");
        uint32_t tocAddr = getRegU32(ctx, 4);
        if (uint8_t *toc = getMemPtr(rdram, tocAddr))
        {
            std::memset(toc, 0, 1024);
        }
        setReturnS32(ctx, 1);
    }

    void sceCdInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        frontendDiagInit();
        frontendDiagCountSif("sceCdInit");
        g_cdInitialized = true;
        g_lastCdError = 0;
        setReturnS32(ctx, 1);
    }

    void sceCdInitEeCB(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        frontendDiagInit();
        frontendDiagCountSif("sceCdInitEeCB");
        setReturnS32(ctx, 1);
    }

    void sceCdIntToPos(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        frontendDiagInit();
        frontendDiagCountSif("sceCdIntToPos");
        uint32_t lsn = getRegU32(ctx, 4);
        uint32_t posAddr = getRegU32(ctx, 5);
        uint8_t *pos = getMemPtr(rdram, posAddr);
        if (!pos)
        {
            setReturnS32(ctx, 0);
            return;
        }

        uint32_t adjusted = lsn + 150;
        const uint32_t minutes = adjusted / (60 * 75);
        adjusted %= (60 * 75);
        const uint32_t seconds = adjusted / 75;
        const uint32_t sectors = adjusted % 75;

        pos[0] = toBcd(minutes);
        pos[1] = toBcd(seconds);
        pos[2] = toBcd(sectors);
        pos[3] = 0;
        setReturnS32(ctx, 1);
    }

    void sceCdMmode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        frontendDiagInit();
        frontendDiagCountSif("sceCdMmode");
        g_cdMode = getRegU32(ctx, 4);
        setReturnS32(ctx, 1);
    }

    void sceCdNcmdDiskReady(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        frontendDiagInit();
        frontendDiagCountSif("sceCdNcmdDiskReady");
        setReturnS32(ctx, 2);
    }

    void sceCdPause(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        frontendDiagInit();
        frontendDiagCountSif("sceCdPause");
        setReturnS32(ctx, 1);
    }

    void sceCdPosToInt(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        frontendDiagInit();
        frontendDiagCountSif("sceCdPosToInt");
        uint32_t posAddr = getRegU32(ctx, 4);
        const uint8_t *pos = getConstMemPtr(rdram, posAddr);
        if (!pos)
        {
            setReturnS32(ctx, -1);
            return;
        }

        const uint32_t minutes = fromBcd(pos[0]);
        const uint32_t seconds = fromBcd(pos[1]);
        const uint32_t sectors = fromBcd(pos[2]);
        const uint32_t absolute = (minutes * 60 * 75) + (seconds * 75) + sectors;
        const int32_t lsn = static_cast<int32_t>(absolute) - 150;
        setReturnS32(ctx, lsn);
    }

    void sceCdReadChain(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        frontendDiagInit();
        frontendDiagCountCd("sceCdReadChain");
        uint32_t chainAddr = getRegU32(ctx, 4);
        bool ok = true;
        bool anySegment = false;

        for (int i = 0; i < 64; ++i)
        {
            uint32_t *entry = reinterpret_cast<uint32_t *>(getMemPtr(rdram, chainAddr + (i * 16)));
            if (!entry)
            {
                ok = false;
                break;
            }

            const uint32_t lbn = entry[0];
            const uint32_t sectors = entry[1];
            const uint32_t buf = entry[2];
            if (lbn == 0xFFFFFFFFu || sectors == 0)
            {
                break;
            }

            uint32_t offset = buf & PS2_RAM_MASK;
            const uint64_t bytes = static_cast<uint64_t>(sectors) * kCdSectorSize;
            if (bytes > PS2_RAM_SIZE - offset)
            {
                runtime->gate3RecordCdReadV1("sceCdReadChain", lbn, sectors, false, false, {});
                g_lastCdError = -1;
                ok = false;
                break;
            }

            if (!gate3ReadCdHost(lbn, sectors, rdram + offset, bytes, runtime, "sceCdReadChain"))
            {
                runtime->gate3RecordCdReadV1("sceCdReadChain", lbn, sectors, false, false, {});
                ok = false;
                break;
            }

            runtime->gate3RecordCdReadV1("sceCdReadChain", lbn, sectors, false, true,
                std::vector<uint8_t>(rdram + offset, rdram + offset + bytes));
            anySegment = true;
            g_cdStreamingLbn = lbn + sectors;
        }

        if (ok && !anySegment)
            runtime->gate3RecordCdReadV1("sceCdReadChain", 0u, 0u, false, true, {});
        runtime->gate3CdRouteCompletedV1("sceCdReadChain", ok);
        setReturnS32(ctx, ok ? 1 : 0);
    }

    void sceCdReadClock(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint8_t* target = getMemPtr(rdram, getRegU32(ctx, 4));
        if (!target) { setReturnS32(ctx, 0); return; }
        const auto clock = runtime->gate3ReadRtcV1();
        std::memcpy(target, clock.data(), clock.size());
        setReturnS32(ctx, 1);
    }

    void sceCdReadIOPm(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        frontendDiagInit();
        frontendDiagCountSif("sceCdReadIOPm");
        sceCdReadRouted(rdram, ctx, runtime, true);
    }

    void sceCdSearchFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        frontendDiagInit();
        frontendDiagCountSif("sceCdSearchFile");
        uint32_t fileAddr = getRegU32(ctx, 4);
        uint32_t pathAddr = getRegU32(ctx, 5);
        const std::string path = readPs2CStringBounded(rdram, pathAddr, 260);
        const std::string normalizedPath = normalizeCdPathNoPrefix(path);
        static uint32_t traceCount = 0;
        const uint32_t callerRa = getRegU32(ctx, 31);
        const bool shouldTrace = (traceCount < 128u) || ((traceCount % 512u) == 0u);
        if (shouldTrace)
        {
            RUNTIME_LOG("[sceCdSearchFile] pc=0x" << std::hex << ctx->pc
                                                  << " ra=0x" << callerRa
                                                  << " file=0x" << fileAddr
                                                  << " pathAddr=0x" << pathAddr
                                                  << " path=\"" << sanitizeForLog(path) << "\""
                                                  << std::dec << std::endl);
        }
        ++traceCount;

        if (path.empty())
        {
            static uint32_t emptyPathCount = 0;
            if (emptyPathCount < 64 || (emptyPathCount % 512u) == 0u)
            {
                std::ostringstream preview;
                preview << std::hex;
                for (uint32_t i = 0; i < 16; ++i)
                {
                    const uint8_t byte = *getConstMemPtr(rdram, pathAddr + i);
                    preview << (i == 0 ? "" : " ") << static_cast<uint32_t>(byte);
                }
                std::cerr << "[sceCdSearchFile] empty path at 0x" << std::hex << pathAddr
                          << " preview=" << preview.str()
                          << " ra=0x" << callerRa << std::dec << std::endl;
            }
            ++emptyPathCount;
            g_lastCdError = -1;
            setReturnS32(ctx, 0);
            return;
        }

        if (normalizedPath.empty())
        {
            static uint32_t emptyNormalizedCount = 0;
            if (emptyNormalizedCount < 64u || (emptyNormalizedCount % 512u) == 0u)
            {
                std::cerr << "sceCdSearchFile failed: " << sanitizeForLog(path)
                          << " (normalized path is empty, root: " << getCdRootPath().string() << ")"
                          << std::endl;
            }
            ++emptyNormalizedCount;
            g_lastCdError = -1;
            setReturnS32(ctx, 0);
            return;
        }

        CdFileEntry entry;
        bool found = registerCdFile(path, entry);
        CdFileEntry resolvedEntry = entry;
        std::string resolvedPath;

        if (!found)
        {
            static std::string lastFailedPath;
            static uint32_t samePathFailCount = 0;
            if (path == lastFailedPath)
            {
                ++samePathFailCount;
            }
            else
            {
                lastFailedPath = path;
                samePathFailCount = 1;
            }

            if (samePathFailCount <= 16u || (samePathFailCount % 512u) == 0u)
            {
                std::cerr << "sceCdSearchFile failed: " << sanitizeForLog(path)
                          << " (root: " << getCdRootPath().string()
                          << ", repeat=" << samePathFailCount << ")" << std::endl;
            }
            setReturnS32(ctx, 0);
            return;
        }

        if (!writeCdSearchResult(rdram, fileAddr, path, resolvedEntry))
        {
            g_lastCdError = -1;
            setReturnS32(ctx, 0);
            return;
        }

        g_cdStreamingLbn = resolvedEntry.baseLbn;
        if (shouldTrace)
        {
            RUNTIME_LOG("[sceCdSearchFile:ok] path=\"" << sanitizeForLog(path)
                                                       << "\" lsn=0x" << std::hex << resolvedEntry.baseLbn
                                                       << " size=0x" << resolvedEntry.sizeBytes
                                                       << " sectors=0x" << resolvedEntry.sectors
                                                       << std::dec << std::endl);
        }
        setReturnS32(ctx, 1);
    }

    void sceCdSeek(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        frontendDiagInit();
        frontendDiagCountSif("sceCdSeek");
        g_cdStreamingLbn = getRegU32(ctx, 4);
        setReturnS32(ctx, 1);
    }

    void sceCdStandby(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        frontendDiagInit();
        frontendDiagCountSif("sceCdStandby");
        setReturnS32(ctx, 1);
    }

    void sceCdStatus(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        frontendDiagInit();
        frontendDiagCountSif("sceCdStatus");
        setReturnS32(ctx, g_cdInitialized ? 6 : 0);
    }

    void sceCdStInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        frontendDiagInit();
        frontendDiagCountSif("sceCdStInit");
        setReturnS32(ctx, 1);
    }

    void sceCdStop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        frontendDiagInit();
        frontendDiagCountSif("sceCdStop");
        setReturnS32(ctx, 1);
    }

    void sceCdStPause(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        frontendDiagInit();
        frontendDiagCountSif("sceCdStPause");
        setReturnS32(ctx, 1);
    }

    void sceCdStRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        frontendDiagInit();
        frontendDiagCountSif("sceCdStRead");
        uint32_t sectors = getRegU32(ctx, 4);
        uint32_t buf = getRegU32(ctx, 5);
        uint32_t errAddr = getRegU32(ctx, 7);

        uint32_t offset = buf & PS2_RAM_MASK;
        const uint64_t bytes = static_cast<uint64_t>(sectors) * kCdSectorSize;
        if (bytes > PS2_RAM_SIZE - offset)
        {
            runtime->gate3RecordCdReadV1("sceCdStRead", g_cdStreamingLbn, sectors, false, false, {});
            g_lastCdError = -1;
            if (int32_t *err = reinterpret_cast<int32_t *>(getMemPtr(rdram, errAddr)); err)
            {
                *err = g_lastCdError;
            }
            setReturnS32(ctx, 0);
            return;
        }

        const uint32_t lbn = g_cdStreamingLbn;
        const bool ok = gate3ReadCdHost(lbn, sectors, rdram + offset, bytes, runtime, "sceCdStRead");
        runtime->gate3RecordCdReadV1("sceCdStRead", lbn, sectors, false, ok,
            ok ? std::vector<uint8_t>(rdram + offset, rdram + offset + bytes) : std::vector<uint8_t>{});
        if (ok)
        {
            g_cdStreamingLbn += sectors;
        }

        if (int32_t *err = reinterpret_cast<int32_t *>(getMemPtr(rdram, errAddr)); err)
        {
            *err = ok ? 0 : g_lastCdError;
        }

        setReturnS32(ctx, ok ? static_cast<int32_t>(sectors) : 0);
    }

    void sceCdStream(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        frontendDiagInit();
        frontendDiagCountSif("sceCdStream");
        setReturnS32(ctx, 1);
    }

    void sceCdStResume(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        frontendDiagInit();
        frontendDiagCountSif("sceCdStResume");
        setReturnS32(ctx, 1);
    }

    void sceCdStSeek(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        frontendDiagInit();
        frontendDiagCountSif("sceCdStSeek");
        g_cdStreamingLbn = getRegU32(ctx, 4);
        setReturnS32(ctx, 1);
    }

    void sceCdStSeekF(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        frontendDiagInit();
        frontendDiagCountSif("sceCdStSeekF");
        g_cdStreamingLbn = getRegU32(ctx, 4);
        setReturnS32(ctx, 1);
    }

    void sceCdStStart(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        frontendDiagInit();
        frontendDiagCountSif("sceCdStStart");
        g_cdStreamingLbn = getRegU32(ctx, 4);
        setReturnS32(ctx, 1);
    }

    void sceCdStStat(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        frontendDiagInit();
        frontendDiagCountSif("sceCdStStat");
        setReturnS32(ctx, 0);
    }

    void sceCdStStop(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        frontendDiagInit();
        frontendDiagCountSif("sceCdStStop");
        setReturnS32(ctx, 1);
    }

    void sceCdSyncS(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        frontendDiagInit();
        frontendDiagCountSif("sceCdSyncS");
        setReturnS32(ctx, 0);
    }

    void sceCdTrayReq(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        frontendDiagInit();
        frontendDiagCountSif("sceCdTrayReq");
        uint32_t statusPtr = getRegU32(ctx, 5);
        if (uint32_t *status = reinterpret_cast<uint32_t *>(getMemPtr(rdram, statusPtr)); status)
        {
            *status = 0;
        }
        setReturnS32(ctx, 1);
    }
}
