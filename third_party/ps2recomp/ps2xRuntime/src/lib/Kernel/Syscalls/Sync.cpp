#include "Common.h"
#include "Sync.h"

namespace ps2_syscalls
{
    static bool looksLikeGuestPointerOrNull(uint32_t value)
    {
        if (value == 0u)
        {
            return true;
        }
        const uint32_t normalized = value & 0x1FFFFFFFu;
        return normalized < PS2_RAM_SIZE;
    }

    static bool readGuestU32Safe(const uint8_t *rdram, uint32_t addr, uint32_t &out)
    {
        const uint8_t *b0 = getConstMemPtr(rdram, addr + 0u);
        const uint8_t *b1 = getConstMemPtr(rdram, addr + 1u);
        const uint8_t *b2 = getConstMemPtr(rdram, addr + 2u);
        const uint8_t *b3 = getConstMemPtr(rdram, addr + 3u);
        if (!b0 || !b1 || !b2 || !b3)
        {
            out = 0u;
            return false;
        }

        out = static_cast<uint32_t>(*b0) |
              (static_cast<uint32_t>(*b1) << 8) |
              (static_cast<uint32_t>(*b2) << 16) |
              (static_cast<uint32_t>(*b3) << 24);
        return true;
    }

    struct DecodedSemaParams
    {
        int init = 0;
        int max = 1;
        uint32_t attr = 0;
        uint32_t option = 0;
    };

    static DecodedSemaParams decodeCreateSemaParams(const uint32_t *param, uint32_t availableWords)
    {
        DecodedSemaParams out{};
        if (!param || availableWords == 0u)
        {
            return out;
        }

        // EE layout (kernel.h):
        // [0]=count [1]=max_count [2]=init_count [3]=wait_threads [4]=attr [5]=option
        const bool hasEeLayout = availableWords >= 3u;
        const int eeMax = hasEeLayout ? static_cast<int>(param[1]) : 1;
        const int eeInit = hasEeLayout ? static_cast<int>(param[2]) : 0;
        const uint32_t eeAttr = (availableWords >= 5u) ? param[4] : 0u;
        const uint32_t eeOption = (availableWords >= 6u) ? param[5] : 0u;

        // Legacy layout (IOP-style):
        // [0]=attr [1]=option [2]=init [3]=max
        const bool hasLegacyLayout = availableWords >= 4u;
        const int legacyMax = hasLegacyLayout ? static_cast<int>(param[3]) : 1;
        const int legacyInit = hasLegacyLayout ? static_cast<int>(param[2]) : 0;
        const uint32_t legacyAttr = hasLegacyLayout ? param[0] : 0u;
        const uint32_t legacyOption = hasLegacyLayout ? param[1] : 0u;

        auto countLooksPlausible = [](int value) -> bool
        {
            return value > 0 && value <= 0x10000;
        };

        bool useLegacyLayout = hasLegacyLayout && !hasEeLayout;
        if (hasLegacyLayout && hasEeLayout && countLooksPlausible(legacyMax) && !countLooksPlausible(eeMax))
        {
            useLegacyLayout = true;
        }
        else if (hasLegacyLayout && hasEeLayout && countLooksPlausible(legacyMax) && countLooksPlausible(eeMax))
        {
            // If both max values look valid, prefer the layout whose option field
            // looks like a pointer/NULL payload.
            const bool eeOptionLooksValid = looksLikeGuestPointerOrNull(eeOption);
            const bool legacyOptionLooksValid = looksLikeGuestPointerOrNull(legacyOption);
            if (!eeOptionLooksValid && legacyOptionLooksValid)
            {
                useLegacyLayout = true;
            }
        }

        if (useLegacyLayout && hasLegacyLayout)
        {
            out.max = legacyMax;
            out.init = legacyInit;
            out.attr = legacyAttr;
            out.option = legacyOption;
        }
        else
        {
            if (!hasEeLayout)
            {
                return out;
            }
            out.max = eeMax;
            out.init = eeInit;
            out.attr = eeAttr;
            out.option = eeOption;
        }

        return out;
    }

    void CreateSema(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t paramAddr = getRegU32(ctx, 4); // $a0
        if (paramAddr == 0u)
        {
            setReturnS32(ctx, KE_ERROR);
            return;
        }

        uint32_t rawParams[6] = {};
        uint32_t availableWords = 0u;
        for (uint32_t i = 0; i < 6u; ++i)
        {
            if (!readGuestU32Safe(rdram, paramAddr + (i * 4u), rawParams[i]))
            {
                break;
            }
            availableWords = i + 1u;
        }

        if (availableWords < 3u)
        {
            setReturnS32(ctx, KE_ERROR);
            return;
        }

        const DecodedSemaParams decoded = decodeCreateSemaParams(rawParams, availableWords);
        int init = decoded.init;
        int max = decoded.max;
        uint32_t attr = decoded.attr;
        uint32_t option = decoded.option;

        if (max <= 0)
        {
            max = 1;
        }
        if (init < 0)
        {
            init = 0;
        }
        if (init > max)
        {
            init = max;
        }

        int id = 0;
        auto info = std::make_shared<SemaInfo>();
        info->count = init;
        info->maxCount = max;
        info->initCount = init;
        info->attr = attr;
        info->option = option;

        {
            std::lock_guard<std::mutex> lock(g_sema_map_mutex);
            for (int attempts = 0; attempts < 0x7FFF; ++attempts)
            {
                if (g_nextSemaId <= 0)
                {
                    g_nextSemaId = 1;
                }

                const int candidate = g_nextSemaId++;
                if (candidate <= 0)
                {
                    continue;
                }

                if (g_semas.find(candidate) == g_semas.end())
                {
                    id = candidate;
                    break;
                }
            }

            if (id <= 0)
            {
                setReturnS32(ctx, KE_ERROR);
                return;
            }

            g_semas.emplace(id, info);
        }
        RUNTIME_LOG("[CreateSema] id=" << id << " init=" << init << " max=" << max);
        setReturnS32(ctx, id);
    }

    void DeleteSema(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int sid = static_cast<int>(getRegU32(ctx, 4));
        auto sema = lookupSemaInfo(sid);
        if (!sema) { setReturnS32(ctx, KE_UNKNOWN_SEMID); return; }
        { std::lock_guard lock(g_sema_map_mutex); g_semas.erase(sid); }
        { std::lock_guard lock(sema->m);
          sema->deleted = true;
          gate3Sort(sema->gate3Waits, runtime);
          for (auto& w : sema->gate3Waits) gate3Resolve(w.ticket, runtime, KE_WAIT_DELETE);
          sema->gate3Waits.clear(); sema->waiters = 0; }
        setReturnS32(ctx, sid);
    }

    void iDeleteSema(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        DeleteSema(rdram, ctx, runtime);
    }

    void SignalSema(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int sid = static_cast<int>(getRegU32(ctx, 4));
        auto sema = lookupSemaInfo(sid);
        if (!sema) { setReturnS32(ctx, KE_UNKNOWN_SEMID); return; }
        int result = sid;
        {
            std::lock_guard lock(sema->m);
            gate3Sort(sema->gate3Waits, runtime);
            if (!sema->gate3Waits.empty()) {
                gate3Resolve(sema->gate3Waits.front().ticket, runtime, sid);
                sema->gate3Waits.erase(sema->gate3Waits.begin());
                sema->waiters = sema->gate3Waits.size();
            } else if (sema->count == sema->maxCount) result = KE_SEMA_OVF;
            else ++sema->count;
        }
        setReturnS32(ctx, result);
    }

    void iSignalSema(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        SignalSema(rdram, ctx, runtime);
    }

    void WaitSema(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int sid = static_cast<int>(getRegU32(ctx, 4));
        auto sema = lookupSemaInfo(sid);
        if (!sema) { setReturnS32(ctx, KE_UNKNOWN_SEMID); return; }
        rrv::guest_time::Scheduler::Ticket ticket;
        {
            std::lock_guard lock(sema->m);
            if (sema->count) { --sema->count; setReturnS32(ctx, sid); return; }
            ticket = gate3Wait(ctx, runtime, TSW_SEMA, sid);
            sema->gate3Waits.push_back({ticket, 0, 0});
            ++sema->waiters;
        }
        gate3Park(ticket, runtime);
        { std::lock_guard lock(sema->m); gate3Prune(sema->gate3Waits); sema->waiters = sema->gate3Waits.size(); }
        setReturnS32(ctx, ticket->result);
    }

    void PollSema(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        int sid = static_cast<int>(getRegU32(ctx, 4));
        auto sema = lookupSemaInfo(sid);
        if (!sema)
        {
            setReturnS32(ctx, KE_UNKNOWN_SEMID);
            return;
        }

        std::lock_guard<std::mutex> lock(sema->m);
        if (sema->count > 0)
        {
            sema->count--;
            setReturnS32(ctx, sid);  // PS2 EE BIOS returns sid on success.
            return;
        }

        setReturnS32(ctx, KE_SEMA_ZERO);
    }

    void iPollSema(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        PollSema(rdram, ctx, runtime);
    }

    void ReferSemaStatus(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        int sid = static_cast<int>(getRegU32(ctx, 4));
        uint32_t statusAddr = getRegU32(ctx, 5);

        auto sema = lookupSemaInfo(sid);
        if (!sema)
        {
            setReturnS32(ctx, KE_UNKNOWN_SEMID);
            return;
        }

        ee_sema_t *status = reinterpret_cast<ee_sema_t *>(getMemPtr(rdram, statusAddr));
        if (!status)
        {
            setReturnS32(ctx, KE_ERROR);
            return;
        }

        std::lock_guard<std::mutex> lock(sema->m);
        status->count = sema->count;
        status->max_count = sema->maxCount;
        status->init_count = sema->initCount;
        status->wait_threads = sema->waiters;
        status->attr = sema->attr;
        status->option = sema->option;
        setReturnS32(ctx, KE_OK);
    }

    void iReferSemaStatus(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ReferSemaStatus(rdram, ctx, runtime);
    }

    constexpr uint32_t WEF_OR = 1;
    constexpr uint32_t WEF_CLEAR = 0x10;
    constexpr uint32_t WEF_CLEAR_ALL = 0x20;
    constexpr uint32_t WEF_MODE_MASK = WEF_OR | WEF_CLEAR | WEF_CLEAR_ALL;
    constexpr uint32_t EA_MULTI = 0x2;

    void CreateEventFlag(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t paramAddr = getRegU32(ctx, 4); // $a0
        const uint32_t *param = reinterpret_cast<const uint32_t *>(getConstMemPtr(rdram, paramAddr));

        auto info = std::make_shared<EventFlagInfo>();
        if (param)
        {
            info->attr = param[0];
            info->option = param[1];
            info->initBits = param[2];
            info->bits = info->initBits;
        }

        int id = 0;
        {
            std::lock_guard<std::mutex> mapLock(g_event_flag_map_mutex);
            id = g_nextEventFlagId++;
            g_eventFlags[id] = info;
        }
        setReturnS32(ctx, id);
    }

    void DeleteEventFlag(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int eid = static_cast<int>(getRegU32(ctx, 4));
        auto event = lookupEventFlagInfo(eid);
        if (!event) { setReturnS32(ctx, KE_UNKNOWN_EVFID); return; }
        { std::lock_guard lock(g_event_flag_map_mutex); g_eventFlags.erase(eid); }
        { std::lock_guard lock(event->m);
          event->deleted = true;
          gate3Sort(event->gate3Waits, runtime);
          for (auto& w : event->gate3Waits) gate3Resolve(w.ticket, runtime, KE_WAIT_DELETE);
          event->gate3Waits.clear(); event->waiters = 0; }
        setReturnS32(ctx, KE_OK);
    }

    void SetEventFlag(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int eid = static_cast<int>(getRegU32(ctx, 4));
        auto event = lookupEventFlagInfo(eid);
        if (!event) { setReturnS32(ctx, KE_UNKNOWN_EVFID); return; }
        {
            std::lock_guard lock(event->m);
            event->bits |= getRegU32(ctx, 5);
            gate3Sort(event->gate3Waits, runtime);
            for (auto& w : event->gate3Waits) {
                const bool ok = w.mode & WEF_OR ? (event->bits & w.bits) != 0 : (event->bits & w.bits) == w.bits;
                if (!ok) continue;
                gate3Resolve(w.ticket, runtime, KE_OK, event->bits);
                if (w.mode & WEF_CLEAR_ALL) event->bits = 0;
                else if (w.mode & WEF_CLEAR) event->bits &= ~w.bits;
            }
            gate3Prune(event->gate3Waits);
            event->waiters = event->gate3Waits.size();
        }
        setReturnS32(ctx, KE_OK);
    }

    void iSetEventFlag(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        SetEventFlag(rdram, ctx, runtime);
    }

    void ClearEventFlag(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        int eid = static_cast<int>(getRegU32(ctx, 4));
        uint32_t bits = getRegU32(ctx, 5);
        auto info = lookupEventFlagInfo(eid);
        if (!info)
        {
            setReturnS32(ctx, KE_UNKNOWN_EVFID);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(info->m);
            info->bits &= bits;
        }
        info->cv.notify_all();
        setReturnS32(ctx, KE_OK);
    }

    void iClearEventFlag(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ClearEventFlag(rdram, ctx, runtime);
    }

    void WaitEventFlag(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const int eid = static_cast<int>(getRegU32(ctx, 4));
        const uint32_t bits = getRegU32(ctx, 5), mode = getRegU32(ctx, 6), address = getRegU32(ctx, 7);
        if (mode & ~WEF_MODE_MASK) { setReturnS32(ctx, KE_ILLEGAL_MODE); return; }
        if (!bits) { setReturnS32(ctx, KE_EVF_ILPAT); return; }
        auto event = lookupEventFlagInfo(eid);
        if (!event) { setReturnS32(ctx, KE_UNKNOWN_EVFID); return; }
        rrv::guest_time::Scheduler::Ticket ticket;
        uint32_t resultBits = 0;
        {
            std::lock_guard lock(event->m);
            gate3Prune(event->gate3Waits);
            if (!(event->attr & EA_MULTI) && !event->gate3Waits.empty()) { setReturnS32(ctx, KE_EVF_MULTI); return; }
            const bool satisfied = mode & WEF_OR ? (event->bits & bits) != 0 : (event->bits & bits) == bits;
            if (satisfied) {
                resultBits = event->bits;
                if (mode & WEF_CLEAR_ALL) event->bits = 0;
                else if (mode & WEF_CLEAR) event->bits &= ~bits;
            } else {
                ticket = gate3Wait(ctx, runtime, TSW_EVENT, eid);
                event->gate3Waits.push_back({ticket, bits, mode});
                ++event->waiters;
            }
        }
        int result = KE_OK;
        if (ticket) {
            gate3Park(ticket, runtime);
            { std::lock_guard lock(event->m); gate3Prune(event->gate3Waits); event->waiters = event->gate3Waits.size(); }
            result = ticket->result; resultBits = ticket->bits;
        }
        if (result == KE_OK && address) {
            auto ptr = getMemPtr(rdram, address);
            if (ptr) std::memcpy(ptr, &resultBits, sizeof(resultBits));
        }
        setReturnS32(ctx, result);
    }

    void PollEventFlag(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        int eid = static_cast<int>(getRegU32(ctx, 4));
        uint32_t waitBits = getRegU32(ctx, 5);
        uint32_t mode = getRegU32(ctx, 6);
        uint32_t resBitsAddr = getRegU32(ctx, 7);

        if ((mode & ~WEF_MODE_MASK) != 0)
        {
            setReturnS32(ctx, KE_ILLEGAL_MODE);
            return;
        }

        if (waitBits == 0)
        {
            setReturnS32(ctx, KE_EVF_ILPAT);
            return;
        }

        auto info = lookupEventFlagInfo(eid);
        if (!info)
        {
            setReturnS32(ctx, KE_UNKNOWN_EVFID);
            return;
        }

        uint32_t *resBitsPtr = resBitsAddr ? reinterpret_cast<uint32_t *>(getMemPtr(rdram, resBitsAddr)) : nullptr;

        std::lock_guard<std::mutex> lock(info->m);
        if ((info->attr & EA_MULTI) == 0 && info->waiters > 0)
        {
            setReturnS32(ctx, KE_EVF_MULTI);
            return;
        }

        bool ok = false;
        if (mode & WEF_OR)
        {
            ok = (info->bits & waitBits) != 0;
        }
        else
        {
            ok = (info->bits & waitBits) == waitBits;
        }

        if (!ok)
        {
            setReturnS32(ctx, KE_EVF_COND);
            return;
        }

        if (resBitsPtr)
        {
            *resBitsPtr = info->bits;
        }

        if (mode & WEF_CLEAR_ALL)
        {
            info->bits = 0;
        }
        else if (mode & WEF_CLEAR)
        {
            info->bits &= ~waitBits;
        }

        setReturnS32(ctx, KE_OK);
    }

    void iPollEventFlag(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        PollEventFlag(rdram, ctx, runtime);
    }

    void ReferEventFlagStatus(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        int eid = static_cast<int>(getRegU32(ctx, 4));
        uint32_t infoAddr = getRegU32(ctx, 5);

        struct Ps2EventFlagInfo
        {
            uint32_t attr;
            uint32_t option;
            uint32_t initBits;
            uint32_t currBits;
            int32_t numThreads;
            int32_t reserved1;
            int32_t reserved2;
        };

        auto info = lookupEventFlagInfo(eid);
        if (!info)
        {
            setReturnS32(ctx, KE_UNKNOWN_EVFID);
            return;
        }

        Ps2EventFlagInfo *out = infoAddr ? reinterpret_cast<Ps2EventFlagInfo *>(getMemPtr(rdram, infoAddr)) : nullptr;
        if (!out)
        {
            setReturnS32(ctx, -1);
            return;
        }

        std::lock_guard<std::mutex> lock(info->m);
        out->attr = info->attr;
        out->option = info->option;
        out->initBits = info->initBits;
        out->currBits = info->bits;
        out->numThreads = info->waiters;
        out->reserved1 = 0;
        out->reserved2 = 0;
        setReturnS32(ctx, 0);
    }

    void iReferEventFlagStatus(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ReferEventFlagStatus(rdram, ctx, runtime);
    }

    void SetAlarm(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
{
        runtime->gate3RequireAlarmPolicyV1();
        const auto ticks=static_cast<uint16_t>(getRegU32(ctx,4));
        const auto handler=getRegU32(ctx,5);
        if(!runtime->hasFunction(handler)){setReturnS32(ctx,KE_ERROR);return;}
        setReturnS32(ctx,runtime->gate3TemporalV1().AddAlarm(ticks,handler,getRegU32(ctx,6),getRegU32(ctx,28)));
    }
    }

    void InitAlarm(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void iSetAlarm(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        SetAlarm(rdram, ctx, runtime);
    }

    void CancelAlarm(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
{ setReturnS32(ctx,runtime->gate3TemporalV1().CancelAlarm(static_cast<int32_t>(getRegU32(ctx,4))) ? KE_OK : KE_ERROR); }
    }

    void iCancelAlarm(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        CancelAlarm(rdram, ctx, runtime);
    }

    void ReleaseAlarm(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        CancelAlarm(rdram, ctx, runtime);
    }

    void iReleaseAlarm(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        iCancelAlarm(rdram, ctx, runtime);
    }
}
