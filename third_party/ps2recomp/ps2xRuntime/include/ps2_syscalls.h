#ifndef PS2_SYSCALLS_H
#define PS2_SYSCALLS_H

#include "ps2_runtime.h"
#include "ps2_call_list.h"
#include <mutex>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

std::string translatePs2Path(const char *ps2Path);

extern std::atomic<int> g_activeThreads;

inline std::mutex g_sys_fd_mutex;

namespace ps2_syscalls
{
#define PS2_DECLARE_SYSCALL(name) void name(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    PS2_SYSCALL_LIST(PS2_DECLARE_SYSCALL)
#undef PS2_DECLARE_SYSCALL

    void iDeleteSema(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void EnableIntcHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void DisableIntcHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void EnableDmacHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void DisableDmacHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);

    bool dispatchNumericSyscall(uint32_t syscallNumber, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
    void dispatchDmacHandlersForCause(uint8_t *rdram, PS2Runtime *runtime, uint32_t cause);
    void initializeGuestKernelState(uint8_t *rdram);
    // Boot arguments the EE kernel hands the ELF. SetupThread (syscall 0x3C)
    // publishes them into the guest's args block; see System.cpp.
    void setBootArguments(const std::vector<std::string> &args);
    void TODO(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t encodedSyscallId);
    void notifyRuntimeStop();
    void joinAllGuestHostThreads();
    void detachAllGuestHostThreads();
    void resetSoundDriverRpcState();
    void setSoundDriverCompatLayout(const PS2SoundDriverCompatLayout &layout);
    void clearSoundDriverCompatLayout();
    void setDtxCompatLayout(const PS2DtxCompatLayout &layout);
    void clearDtxCompatLayout();
    void EnsureVSyncWorkerRunning(uint8_t *rdram, PS2Runtime *runtime);
    uint64_t GetCurrentVSyncTick();
    // Milestone B4 (docs/MILESTONES.md GS-stream record/replay harness):
    // deterministic tick injection for headless replay only — see
    // Kernel/Syscalls/Interrupt.cpp for the full rationale. Never call this
    // from live/game code paths.
    void SetCurrentVSyncTickForReplay(uint64_t tick);
    uint64_t WaitForNextVSyncTick(uint8_t *rdram, PS2Runtime *runtime);
    void WaitVSyncTick(uint8_t *rdram, PS2Runtime *runtime);

    // docs/DESIGN_GS_FIELD_MODEL.md M2: one authoritative field state (see
    // Kernel/Syscalls/Interrupt.h for the full rationale). Declared here too,
    // matching the GetCurrentVSyncTick/WaitForNextVSyncTick pattern above, so
    // TUs that only pull in this umbrella header (e.g. Kernel/Stubs/GS.cpp via
    // Common.h) can reach it without including the kernel-internal header.
    struct GsFieldState
    {
        uint64_t fieldIndex;
        int32_t parity;
    };
    GsFieldState GetCurrentField();
    inline int32_t ParityForField(uint64_t fieldIndex) { return static_cast<int32_t>(fieldIndex & 1u); }
    int32_t GetCurrentFieldParity();
}

#endif // PS2_SYSCALLS_H

