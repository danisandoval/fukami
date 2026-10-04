#include "rrv_guest_terminal_outcome.h"

#include <iostream>
#include <thread>
#include <vector>

namespace
{
int failures = 0;

void check(bool value, const char *message)
{
    if (!value)
    {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

rrv::guestoutcome::GuestContext context(uint32_t pc, uint32_t ra, uint32_t sp)
{
    return {pc, ra, sp};
}

void rejectedDispatchRetainsFirstState()
{
    rrv::guestoutcome::RuntimeTerminalOutcomeState state;
    rrv::guestoutcome::begin(state);
    rrv::guestoutcome::recordFailure(
        state, rrv::guestoutcome::TerminalKind::RejectedMainDispatch,
        context(0x00123450u, 0x00567890u, 0x01fff000u), "unregistered main target");
    rrv::guestoutcome::recordFailure(
        state, rrv::guestoutcome::TerminalKind::HostRuntimeException,
        context(0u, 0u, 0u), "later host cleanup failure");
    const auto outcome = rrv::guestoutcome::snapshot(state);
    check(outcome.kind == rrv::guestoutcome::TerminalKind::RejectedMainDispatch,
          "rejected main target is the terminal failure");
    check(outcome.context.pc == 0x00123450u && outcome.context.ra == 0x00567890u &&
              outcome.context.sp == 0x01fff000u,
          "rejected target preserves its pre-recovery context");
    check(std::string_view(outcome.detail.data()) == "unregistered main target",
          "rejected target preserves its first diagnostic label");
}

void guestAndHostExceptionsAreTerminalAndSticky()
{
    rrv::guestoutcome::RuntimeTerminalOutcomeState state;
    rrv::guestoutcome::begin(state);
    rrv::guestoutcome::recordFailure(
        state, rrv::guestoutcome::TerminalKind::MainGuestException,
        context(0x00200000u, 0x00200004u, 0x01ffe000u), "main dispatch exception");
    rrv::guestoutcome::recordFailure(
        state, rrv::guestoutcome::TerminalKind::HostRuntimeException,
        context(0u, 0u, 0u), "host intervention");
    const auto outcome = rrv::guestoutcome::snapshot(state);
    check(outcome.kind == rrv::guestoutcome::TerminalKind::MainGuestException,
          "guest exception is not overwritten by host recovery");
    check(!rrv::guestoutcome::permitsQualificationSuccess(outcome),
          "guest exception cannot qualify as success");
}

void mainCompletionAlwaysFailsEvenAfterTheHostDeclaredAControl()
{
    rrv::guestoutcome::RuntimeTerminalOutcomeState premature;
    rrv::guestoutcome::begin(premature);
    rrv::guestoutcome::recordMainCompletion(premature, context(0u, 0u, 0x01fff000u));
    auto outcome = rrv::guestoutcome::snapshot(premature);
    check(outcome.kind == rrv::guestoutcome::TerminalKind::MainGuestReturned,
          "main completion without a control is premature");

    rrv::guestoutcome::RuntimeTerminalOutcomeState controlled;
    rrv::guestoutcome::begin(controlled);
    rrv::guestoutcome::markControlledStop(
        controlled, rrv::guestoutcome::ControlledStopKind::BoundedPresentLimit);
    rrv::guestoutcome::recordMainCompletion(controlled, context(0u, 0u, 0x01fff000u));
    outcome = rrv::guestoutcome::snapshot(controlled);
    check(outcome.kind == rrv::guestoutcome::TerminalKind::MainGuestReturned,
          "PC-zero main completion remains fatal after a controlled marker");
    check(!rrv::guestoutcome::permitsQualificationSuccess(outcome),
          "a controlled marker cannot convert premature main completion to success");

    rrv::guestoutcome::RuntimeTerminalOutcomeState none;
    rrv::guestoutcome::begin(none);
    rrv::guestoutcome::markControlledStop(none, rrv::guestoutcome::ControlledStopKind::None);
    rrv::guestoutcome::markRunFinished(none);
    outcome = rrv::guestoutcome::snapshot(none);
    check(outcome.kind == rrv::guestoutcome::TerminalKind::StopRequested,
          "an unspecified control marker remains an ordinary requested stop");
    check(!rrv::guestoutcome::permitsQualificationSuccess(outcome),
          "ControlledStopKind::None cannot qualify success");
}

void registeredReentryAndWorkerFailuresHaveSeparateSemantics()
{
    rrv::guestoutcome::RuntimeTerminalOutcomeState reentry;
    rrv::guestoutcome::begin(reentry);
    rrv::guestoutcome::markControlledStop(
        reentry, rrv::guestoutcome::ControlledStopKind::BoundedSemanticInterval);
    const auto reentryOutcome = rrv::guestoutcome::snapshot(reentry);
    check(reentryOutcome.kind == rrv::guestoutcome::TerminalKind::ControlledStop,
          "registered re-entry has no failure record of its own");
    check(rrv::guestoutcome::permitsQualificationSuccess(reentryOutcome),
          "a named bounded control remains eligible when no failure occurred");

    rrv::guestoutcome::RuntimeTerminalOutcomeState worker;
    rrv::guestoutcome::begin(worker);
    rrv::guestoutcome::recordFailure(
        worker, rrv::guestoutcome::TerminalKind::RejectedWorkerDispatch,
        context(0x00300000u, 0u, 0x01ffd000u), "unregistered worker target");
    rrv::guestoutcome::recordFailure(
        worker, rrv::guestoutcome::TerminalKind::WorkerGuestException,
        context(0x00300004u, 0u, 0x01ffd000u), "later worker exception");
    const auto workerOutcome = rrv::guestoutcome::snapshot(worker);
    check(workerOutcome.kind == rrv::guestoutcome::TerminalKind::RejectedWorkerDispatch,
          "worker lookup bypass records its own first failure");

    rrv::guestoutcome::RuntimeTerminalOutcomeState host;
    rrv::guestoutcome::begin(host);
    rrv::guestoutcome::recordFailure(
        host, rrv::guestoutcome::TerminalKind::HostRuntimeException,
        {}, "host loop exception without a synchronized guest snapshot");
    const auto hostOutcome = rrv::guestoutcome::snapshot(host);
    check(hostOutcome.kind == rrv::guestoutcome::TerminalKind::HostRuntimeException &&
              hostOutcome.context.pc == 0u && hostOutcome.context.ra == 0u && hostOutcome.context.sp == 0u,
          "host failure retains an explicitly unavailable guest context without racing guest state");
}

void laterConcurrentReportsCannotReplaceARecordedFailure()
{
    rrv::guestoutcome::RuntimeTerminalOutcomeState state;
    rrv::guestoutcome::begin(state);
    rrv::guestoutcome::recordFailure(
        state, rrv::guestoutcome::TerminalKind::WorkerGuestException,
        context(0x00400000u, 0u, 0x01ffc000u), "first worker exception");
    std::vector<std::thread> reporters;
    for (uint32_t index = 0; index != 8u; ++index)
    {
        reporters.emplace_back([&state, index] {
            rrv::guestoutcome::recordFailure(
                state, rrv::guestoutcome::TerminalKind::HostRuntimeException,
                context(0x00500000u + index, 0u, 0u), "concurrent cleanup");
        });
    }
    for (auto &reporter : reporters)
        reporter.join();
    const auto outcome = rrv::guestoutcome::snapshot(state);
    check(outcome.kind == rrv::guestoutcome::TerminalKind::WorkerGuestException &&
              outcome.context.pc == 0x00400000u,
          "mutex-protected first worker failure survives concurrent cleanup reports");
}

void detachedReporterRetainsTheStateHandle()
{
    auto handle = rrv::guestoutcome::makeRuntimeTerminalOutcome();
    rrv::guestoutcome::begin(*handle);
    auto lateReporter = std::thread([handle] {
        rrv::guestoutcome::recordFailure(
            *handle, rrv::guestoutcome::TerminalKind::WorkerGuestException,
            context(0x00600000u, 0u, 0x01ffb000u), "late worker report");
    });
    auto observer = handle;
    handle.reset();
    lateReporter.join();
    const auto outcome = rrv::guestoutcome::snapshot(*observer);
    check(outcome.kind == rrv::guestoutcome::TerminalKind::WorkerGuestException &&
              outcome.context.pc == 0x00600000u,
          "late reporter owns a safe terminal-state handle after runtime handle release");
}
} // namespace

int main()
{
    rejectedDispatchRetainsFirstState();
    guestAndHostExceptionsAreTerminalAndSticky();
    mainCompletionAlwaysFailsEvenAfterTheHostDeclaredAControl();
    registeredReentryAndWorkerFailuresHaveSeparateSemantics();
    laterConcurrentReportsCannotReplaceARecordedFailure();
    detachedReporterRetainsTheStateHandle();
    return failures == 0 ? 0 : 1;
}
