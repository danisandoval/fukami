#pragma once

// Product qualification must distinguish an intentional bounded stop from a
// guest/runtime failure.  This is deliberately a small, payload-free record:
// it preserves execution coordinates and a host-owned label, never guest RAM
// or other game material.  The product-specialized PS2Runtime retains a shared
// state handle, and detached reporters capture that handle by value so the
// mutex-protected, first-failure-wins outcome outlives the member storage.

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string_view>

namespace rrv::guestoutcome
{
enum class TerminalKind : uint8_t
{
    Running,
    ControlledStop,
    StopRequested,
    RejectedMainDispatch,
    RejectedWorkerDispatch,
    MainGuestReturned,
    MainGuestException,
    WorkerGuestException,
    HostRuntimeException,
    ShutdownTimeout,
};

enum class ControlledStopKind : uint8_t
{
    None,
    BoundedPresentLimit,
    BoundedSemanticInterval,
};

struct GuestContext
{
    uint32_t pc = 0u;
    uint32_t ra = 0u;
    uint32_t sp = 0u;
};

struct TerminalOutcome
{
    TerminalKind kind = TerminalKind::Running;
    ControlledStopKind controlledStop = ControlledStopKind::None;
    GuestContext context{};
    std::array<char, 96> detail{};
};

struct RuntimeTerminalOutcomeState
{
    mutable std::mutex mutex;
    bool controlledStopRequested = false;
    bool runFinished = false;
    ControlledStopKind controlledStopKind = ControlledStopKind::None;
    TerminalOutcome firstFailure{};
};

using RuntimeTerminalOutcomeHandle = std::shared_ptr<RuntimeTerminalOutcomeState>;

inline RuntimeTerminalOutcomeHandle makeRuntimeTerminalOutcome()
{
    return std::make_shared<RuntimeTerminalOutcomeState>();
}

inline bool isFailure(TerminalKind kind)
{
    switch (kind)
    {
    case TerminalKind::RejectedMainDispatch:
    case TerminalKind::RejectedWorkerDispatch:
    case TerminalKind::MainGuestReturned:
    case TerminalKind::MainGuestException:
    case TerminalKind::WorkerGuestException:
    case TerminalKind::HostRuntimeException:
    case TerminalKind::ShutdownTimeout:
        return true;
    case TerminalKind::Running:
    case TerminalKind::ControlledStop:
    case TerminalKind::StopRequested:
        return false;
    }
    return true;
}

inline const char *name(TerminalKind kind)
{
    switch (kind)
    {
    case TerminalKind::Running: return "running";
    case TerminalKind::ControlledStop: return "controlled-stop";
    case TerminalKind::StopRequested: return "stop-requested";
    case TerminalKind::RejectedMainDispatch: return "rejected-main-dispatch";
    case TerminalKind::RejectedWorkerDispatch: return "rejected-worker-dispatch";
    case TerminalKind::MainGuestReturned: return "premature-main-return";
    case TerminalKind::MainGuestException: return "main-guest-exception";
    case TerminalKind::WorkerGuestException: return "worker-guest-exception";
    case TerminalKind::HostRuntimeException: return "host-runtime-exception";
    case TerminalKind::ShutdownTimeout: return "guest-shutdown-timeout";
    }
    return "unknown";
}

inline void copyDetail(std::array<char, 96> &destination, std::string_view detail)
{
    const size_t count = detail.size() < destination.size() - 1u ? detail.size() : destination.size() - 1u;
    for (size_t index = 0; index < count; ++index)
        destination[index] = detail[index];
    destination[count] = '\0';
}

inline void begin(RuntimeTerminalOutcomeState &state)
{
    std::lock_guard<std::mutex> lock(state.mutex);
    state.controlledStopRequested = false;
    state.runFinished = false;
    state.controlledStopKind = ControlledStopKind::None;
    state.firstFailure = {};
}

inline void markControlledStop(RuntimeTerminalOutcomeState &state, ControlledStopKind kind)
{
    if (kind == ControlledStopKind::None)
        return;
    std::lock_guard<std::mutex> lock(state.mutex);
    state.controlledStopRequested = true;
    state.controlledStopKind = kind;
}

inline void recordFailure(RuntimeTerminalOutcomeState &state, TerminalKind kind,
                          GuestContext context, std::string_view detail)
{
    if (!isFailure(kind))
        return;
    std::lock_guard<std::mutex> lock(state.mutex);
    if (isFailure(state.firstFailure.kind))
        return;
    state.firstFailure.kind = kind;
    state.firstFailure.context = context;
    copyDetail(state.firstFailure.detail, detail);
}

inline void recordMainCompletion(RuntimeTerminalOutcomeState &state, GuestContext context)
{
    std::lock_guard<std::mutex> lock(state.mutex);
    if (isFailure(state.firstFailure.kind))
        return;
    state.firstFailure.kind = TerminalKind::MainGuestReturned;
    state.firstFailure.context = context;
    copyDetail(state.firstFailure.detail, "main dispatch completed without a controlled stop");
}

inline void markRunFinished(RuntimeTerminalOutcomeState &state)
{
    std::lock_guard<std::mutex> lock(state.mutex);
    state.runFinished = true;
}

inline TerminalOutcome snapshot(const RuntimeTerminalOutcomeState &state)
{
    std::lock_guard<std::mutex> lock(state.mutex);
    if (isFailure(state.firstFailure.kind))
        return state.firstFailure;
    TerminalOutcome result{};
    if (state.controlledStopRequested)
    {
        result.kind = TerminalKind::ControlledStop;
        result.controlledStop = state.controlledStopKind;
        copyDetail(result.detail, "explicit bounded test stop");
    }
    else if (state.runFinished)
    {
        result.kind = TerminalKind::StopRequested;
        copyDetail(result.detail, "runtime stopped without a bounded test receipt");
    }
    return result;
}

inline bool permitsQualificationSuccess(const TerminalOutcome &outcome)
{
    return outcome.kind == TerminalKind::ControlledStop &&
           outcome.controlledStop != ControlledStopKind::None;
}
} // namespace rrv::guestoutcome
