#pragma once

#include <condition_variable>
#include <vector>
#include "ps2_syscalls.h"

namespace ps2_syscalls
{
    // Developer-snapshot support (docs/SNAPSHOTS.md). The INTC/DMAC handler
    // tables are host-side HLE state: the guest registers them once during boot
    // through AddIntcHandler/AddDmacHandler, and nothing else ever fills them.
    // A snapshot restore does not run boot, so without these two calls the
    // tables stay empty for the life of the restored process and every
    // interrupt is silently dropped — including the DMA-completion handler RRV
    // uses to launch the next GIF display-list chain.
    struct IrqHandlerSnapshotRecord
    {
        uint32_t table;   // 0 = INTC, 1 = DMAC
        int32_t id;
        uint32_t cause;
        uint32_t handler;
        uint32_t arg;
        uint32_t gp;
        uint32_t sp;
        uint32_t enabled;
        int32_t order;
    };

    struct IrqHandlerSnapshotCounters
    {
        uint32_t enabledIntcMask;
        uint32_t enabledDmacMask;
        int32_t nextIntcHandlerId;
        int32_t nextDmacHandlerId;
        int32_t intcHeadOrder;
        int32_t intcTailOrder;
        int32_t dmacHeadOrder;
        int32_t dmacTailOrder;
    };

    void exportIrqHandlerState(std::vector<IrqHandlerSnapshotRecord> &records,
                               IrqHandlerSnapshotCounters &counters);
    void importIrqHandlerState(const std::vector<IrqHandlerSnapshotRecord> &records,
                               const IrqHandlerSnapshotCounters &counters);

    namespace interrupt_state
    {
        struct VSyncFlagRegistration
        {
            uint32_t flagAddr;
            uint32_t tickAddr;
        };

        extern std::mutex g_irq_handler_mutex;
        extern std::mutex g_irq_worker_mutex;
        extern std::condition_variable g_irq_worker_cv;
        extern std::mutex g_vsync_flag_mutex;
        extern std::condition_variable g_vsync_cv;
        extern std::atomic<bool> g_irq_worker_stop;
        extern std::atomic<bool> g_irq_worker_running;
        extern uint32_t g_enabled_intc_mask;
        extern uint32_t g_enabled_dmac_mask;
        // The field index (docs/DESIGN_GS_FIELD_MODEL.md M2). Advanced exactly
        // once per emulated vblank field, inside signalVSyncFlag(), under
        // g_vsync_flag_mutex -- there is exactly one counter; nothing else may
        // increment or re-anchor it. GetCurrentField() below is the coherent
        // accessor; callers must not read this variable directly.
        extern uint64_t g_vsync_tick_counter;
        extern VSyncFlagRegistration g_vsync_registration;
    }

    // docs/DESIGN_GS_FIELD_MODEL.md M2/§4: one authoritative field state, owned
    // by signalVSyncFlag(). Parity is always fieldIndex & 1 -- there is no
    // epoch and nothing may re-base it (sceGsResetGraph included: on real
    // hardware CSR.FIELD is a property of continuous CRTC scan-out and is
    // immune to software register writes). Every guest-visible consumer
    // (sceGsSyncV, WaitForNextVSyncTick, the guest flag/tick words, the SyncV
    // callback, INTC VBLANK dispatch, and the GS CSR bit-13 mirror) derives
    // from the SAME fieldIndex value captured in one critical section -- never
    // from two independent reads, which is exactly how a straddled increment
    // would produce an incoherent pair.
    //
    // GsFieldState / GetCurrentField() / ParityForField() / GetCurrentFieldParity()
    // are declared in ps2_syscalls.h (included above), not re-declared here:
    // this header already pulls that one in, and GS.cpp reaches the same
    // declarations through it without needing this kernel-internal header, so
    // there is exactly one place they are spelled out.
    //
    // Milestone M4 (docs/DESIGN_GS_FIELD_MODEL.md §6): the VSyncFlagRegistration
    // the guest installed via SetVSyncFlag is host-side HLE state, exactly like
    // the INTC/DMAC handler tables -- a restored snapshot did not re-run the
    // boot code that registered it, so signalVSyncFlag's guest-RAM writes go
    // nowhere until this is restored too.
    interrupt_state::VSyncFlagRegistration GetVSyncFlagRegistration();
    void SetVSyncFlagRegistrationForReplay(interrupt_state::VSyncFlagRegistration reg);

    void dispatchDmacHandlersForCause(uint8_t *rdram, PS2Runtime *runtime, uint32_t cause);
    void EnsureVSyncWorkerRunning(uint8_t *rdram, PS2Runtime *runtime);
    uint64_t GetCurrentVSyncTick();
    // Milestone B4: deterministic tick injection for headless replay only.
    // See Interrupt.cpp for the full rationale. Never call this from live/game
    // code paths.
    void SetCurrentVSyncTickForReplay(uint64_t tick);
    void stopInterruptWorker();
    uint64_t WaitForNextVSyncTick(uint8_t *rdram, PS2Runtime *runtime);
    void WaitVSyncTick(uint8_t *rdram, PS2Runtime *runtime);
    void SetVSyncFlag(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void EnableIntc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void iEnableIntc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void DisableIntc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void iDisableIntc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void AddIntcHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void AddIntcHandler2(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void RemoveIntcHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void AddDmacHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void AddDmacHandler2(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void RemoveDmacHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void EnableIntcHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void DisableIntcHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void EnableDmacHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void DisableDmacHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void EnableDmac(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void iEnableDmac(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void DisableDmac(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void iDisableDmac(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
}
