#include "rrv_guest_time.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace rrv::guest_time;

namespace {

void Check(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

uint64_t CeilNumerator(uint64_t numerator) {
  return numerator / kCycleDenominator +
         (numerator % kCycleDenominator != 0);
}

ActionPolicies Policies() {
  return {
      {FieldEffect::Clear, FieldEffect::Clear, StatusEffect::Clear,
       StatusEffect::Preserve, PhaseEffect::Preserve, {0, 0}},
      {FieldEffect::Set, FieldEffect::Preserve, StatusEffect::Preserve,
       StatusEffect::Preserve, PhaseEffect::Preserve, {0, 0}},
      {FieldEffect::Preserve, FieldEffect::Preserve, StatusEffect::Preserve,
       StatusEffect::Preserve, PhaseEffect::Preserve, {0, 0}},
      {FieldEffect::Set, FieldEffect::Preserve, StatusEffect::Preserve,
       StatusEffect::Preserve, PhaseEffect::Preserve, {0, 0}},
  };
}

ColdBoot Boot() { return {0, {0, 0}, true, false, false, true, Policies()}; }

struct Collector final : EventSink {
  std::vector<Event> events;
  void OnVideoEvent(const Event& event) override { events.push_back(event); }
};

template <typename Operation>
void ExpectTerminalRejection(Operation operation, const char* message) {
  bool rejected = false;
  try {
    operation();
  } catch (const std::logic_error&) {
    rejected = true;
  }
  Check(rejected, message);
}

void CheckTerminalState(const VideoSchedule& schedule, uint64_t cycle,
                        std::size_t pending) {
  Check(schedule.now() == cycle && schedule.sint() == false &&
            schedule.start_assertion_enabled() && schedule.vsint() &&
            schedule.effective_start_generation() == 3000 &&
            schedule.scheduled_field_id() == 3000 &&
            schedule.pending_video_events() == pending,
        "rejected terminal mutations must preserve guest state and queue");
}

void TestExact3000AndTerminalBatch() {
  // Independent dimensional expectations: 525 lines per two-field frame.
  // A field is 615014400/125 cycles; 3.5/262.5 and 22.5/262.5 of that
  // field are respectively 8200192/125 and 52715520/125 cycles.
  Check(kFieldPeriodNumerator == 615014400 &&
            kFieldOffsetNumerator == 8200192 &&
            kEndOffsetNumerator == 52715520,
        "NTSC interlaced rational constants");
  VideoSchedule schedule(Boot());
  Collector sink;
  Check(schedule.AdvanceTo(UINT64_MAX, sink) ==
            AdvanceResult::CutAtTerminalVBlankStart,
        "3000th effective VBlank must cut advancement");
  Check(schedule.cut() && schedule.effective_start_generation() == 3000,
        "cut generation must be 3000");
  Check(schedule.scheduled_field_id() == 3000,
        "scheduled field identity must be separate");
  const uint64_t final_numerator = 2999 * kFieldPeriodNumerator;
  Check(schedule.now() == CeilNumerator(final_numerator),
        "terminal virtual cycle must be exact-rate ceiling");
  Check(sink.events.size() == 11998,
        "cut must exclude FIELD and end for the terminal field");
  Check(sink.events[sink.events.size() - 2].kind ==
            EventKind::ScheduledField &&
            sink.events.back().kind == EventKind::VBlankStart,
        "same-time scheduled-field/start order must be stable");
  Check(schedule.pending_video_events() == 3,
        "terminal FIELD/end/next field must remain pending");
  for (uint64_t field = 0; field < 3000; ++field) {
    const auto& scheduled = sink.events[4 * field];
    const auto& start = sink.events[4 * field + 1];
    const uint64_t exact = field * kFieldPeriodNumerator;
    Check(scheduled.kind == EventKind::ScheduledField &&
              start.kind == EventKind::VBlankStart,
          "field/start event order");
    Check(start.scheduled_field_id == field + 1 &&
              start.effective_start_generation == field + 1,
          "field and effective generation identities");
    Check(start.cycle == CeilNumerator(exact) &&
              start.exact_deadline.cycle == exact / kCycleDenominator &&
              start.exact_deadline.remainder == exact % kCycleDenominator,
          "rational field deadline drift");
    if (field < 2999) {
      const auto& transition = sink.events[4 * field + 2];
      const auto& end = sink.events[4 * field + 3];
      Check(transition.kind == EventKind::FieldTransition &&
                transition.cycle ==
                    CeilNumerator(exact + 8200192) &&
                transition.exact_deadline.cycle ==
                    (exact + 8200192) / 125 &&
                transition.exact_deadline.remainder ==
                    (exact + 8200192) % 125,
            "3.5-line FIELD offset");
      Check(end.kind == EventKind::VBlankEnd &&
                end.cycle == CeilNumerator(exact + 52715520) &&
                end.exact_deadline.cycle ==
                    (exact + 52715520) / 125 &&
                end.exact_deadline.remainder ==
                    (exact + 52715520) % 125,
            "22.5-line VBlank-end offset");
      Check(transition.field == ((field & 1) != 0),
            "FIELD must toggle on its own deadline");
    }
  }
  Check(schedule.AdvanceTo(UINT64_MAX, sink) ==
            AdvanceResult::CutAtTerminalVBlankStart &&
            sink.events.size() == 11998,
        "terminal call must not drain pending work");
}

void TestAssertionIdentityAndSeparateVsint() {
  VideoSchedule schedule(Boot());
  Collector sink;
  schedule.SetStartAssertionEnabled(false);
  schedule.AdvanceTo(100000, sink);
  Check(schedule.scheduled_field_id() == 1 &&
            schedule.effective_start_generation() == 0 &&
            schedule.vsint(),
        "suppressed assertion must still consume scheduled field");
  schedule.AcknowledgeVsint();
  Check(!schedule.vsint(), "GS VSINT acknowledgement must clear only VSINT");
  schedule.SetStartAssertionEnabled(true);
  schedule.AdvanceTo(4930000, sink);
  Check(schedule.scheduled_field_id() == 2 &&
            schedule.effective_start_generation() == 1 &&
            !schedule.vsint(),
        "later assertion needs its own generation");
  schedule.AdvanceTo(10000000, sink);
  Check(schedule.effective_start_generation() == 2 &&
            schedule.vsint(),
        "FIELD transition, not VBlank start, must assert GS VSINT");
}

void TestCutCountsEffectiveAssertions() {
  VideoSchedule schedule(Boot());
  Collector sink;
  schedule.SetStartAssertionEnabled(false);
  schedule.AdvanceTo(100000, sink);
  schedule.SetStartAssertionEnabled(true);
  Check(schedule.AdvanceTo(UINT64_MAX, sink) ==
            AdvanceResult::CutAtTerminalVBlankStart,
        "effective assertion count must own terminal cut");
  Check(schedule.scheduled_field_id() == 3001 &&
            schedule.effective_start_generation() == 3000 &&
            schedule.now() == CeilNumerator(3000 * kFieldPeriodNumerator),
        "suppressed scheduled field must shift terminal field identity");
}

void TestTerminalMutationFreeze() {
  VideoSchedule schedule(Boot());
  struct TerminalProbe final : EventSink {
    VideoSchedule& schedule;
    std::size_t terminal_pending = 0;
    uint64_t terminal_cycle = 0;
    bool probed = false;
    explicit TerminalProbe(VideoSchedule& value) : schedule(value) {}
    void OnVideoEvent(const Event& event) override {
      if (event.kind != EventKind::VBlankStart ||
          event.effective_start_generation != 3000) return;
      terminal_cycle = event.cycle;
      terminal_pending = schedule.pending_video_events();
      ExpectTerminalRejection([&] { schedule.SetSint(true); },
                              "SINT mutation during terminal notification");
      ExpectTerminalRejection([&] { schedule.SetFieldToggle(false); },
                              "FIELD toggle mutation during terminal notification");
      ExpectTerminalRejection(
          [&] { schedule.SetStartAssertionEnabled(false); },
          "assertion-permit mutation during terminal notification");
      ExpectTerminalRejection([&] { schedule.AcknowledgeVsint(); },
                              "status acknowledgement during terminal notification");
      CheckTerminalState(schedule, terminal_cycle, terminal_pending);
      probed = true;
    }
  } sink(schedule);
  Check(schedule.AdvanceTo(UINT64_MAX, sink) ==
            AdvanceResult::CutAtTerminalVBlankStart && sink.probed &&
            schedule.cut(),
        "terminal notification must finish the same-cycle video batch");
  CheckTerminalState(schedule, sink.terminal_cycle, sink.terminal_pending);
  ExpectTerminalRejection([&] { schedule.SetSint(true); },
                          "SINT mutation after terminal cut");
  ExpectTerminalRejection([&] { schedule.SetFieldToggle(false); },
                          "FIELD toggle mutation after terminal cut");
  ExpectTerminalRejection([&] { schedule.SetStartAssertionEnabled(false); },
                          "assertion-permit mutation after terminal cut");
  ExpectTerminalRejection([&] { schedule.AcknowledgeVsint(); },
                          "status acknowledgement after terminal cut");
  CheckTerminalState(schedule, sink.terminal_cycle, sink.terminal_pending);
}

void TestSintAndExplicitModePolicies() {
  VideoSchedule schedule(Boot());
  Collector sink;
  schedule.SetSint(true);
  schedule.AdvanceTo(70000, sink);
  Check(sink.events.size() == 2 &&
            sink.events[1].kind == EventKind::FieldTransitionSuppressed &&
            schedule.field() && !schedule.vsint() &&
            schedule.effective_start_generation() == 0,
        "SINT must suppress start, FIELD transition and VSINT");
  schedule.AdvanceTo(500000, sink);
  Check(sink.events[2].kind == EventKind::VBlankEndSuppressed,
        "SINT must also suppress VBlank end");
  const auto original_deadline = schedule.NextDeadline();
  Check(original_deadline.has_value(), "future field deadline must remain queued");
  const ActionPolicy& csr = schedule.ApplyAction(Action::CsrReset);
  Check(!schedule.field() && !schedule.sint() && !schedule.vsint() &&
            csr.intc_stat == StatusEffect::Preserve &&
            schedule.NextDeadline() == original_deadline,
        "CSR reset must clear only bound video state and preserve phase/INTC");
  const ActionPolicy& program = schedule.ApplyAction(Action::ProgramMode);
  Check(schedule.field() && !schedule.sint() &&
            program.intc_stat == StatusEffect::Preserve &&
            schedule.NextDeadline() == original_deadline,
        "mode program must set FIELD without changing deadline or INTC");
  schedule.ApplyAction(Action::RepeatMode);
  schedule.ApplyAction(Action::ForceMode);
  Check(schedule.field() && schedule.NextDeadline() == original_deadline,
        "repeat and force must preserve phase");
  schedule.AdvanceTo(*original_deadline, sink);
  Check(schedule.scheduled_field_id() == 2 &&
            schedule.effective_start_generation() == 1,
        "unsuppressed next field must advance independent identities");
}

void TestRawRegisterFieldToggle() {
  VideoSchedule schedule(Boot());
  Collector sink;
  const auto initial_deadline = schedule.NextDeadline();
  schedule.SetFieldToggle(false);
  Check(schedule.NextDeadline() == initial_deadline,
        "raw GS route must not rephase the selected video schedule");

  schedule.AdvanceTo(CeilNumerator(kFieldOffsetNumerator), sink);
  Check(schedule.field() && schedule.vsint(),
        "progressive first FIELD transition must hold FIELD one and assert VSINT");
  schedule.AcknowledgeVsint();
  schedule.AdvanceTo(
      CeilNumerator(kFieldPeriodNumerator + kFieldOffsetNumerator), sink);
  Check(schedule.field() && schedule.vsint(),
        "successive progressive FIELD transitions must keep FIELD one");
  schedule.AcknowledgeVsint();

  const auto next_deadline = schedule.NextDeadline();
  schedule.SetFieldToggle(true);
  Check(schedule.NextDeadline() == next_deadline,
        "restoring the interlaced route must preserve the queued deadline");
  schedule.AdvanceTo(
      CeilNumerator(2 * kFieldPeriodNumerator + kFieldOffsetNumerator), sink);
  Check(!schedule.field() && schedule.vsint(),
        "interlaced FIELD transition must resume inversion");
  schedule.AcknowledgeVsint();

  schedule.SetSint(true);
  const uint64_t suppressed_cycle =
      CeilNumerator(3 * kFieldPeriodNumerator + kFieldOffsetNumerator);
  schedule.AdvanceTo(suppressed_cycle, sink);
  bool saw_suppressed = false;
  for (const auto& event : sink.events) {
    if (event.kind == EventKind::FieldTransitionSuppressed &&
        event.cycle == suppressed_cycle)
      saw_suppressed = true;
  }
  Check(saw_suppressed && !schedule.field() && !schedule.vsint(),
        "SINT must suppress FIELD and VSINT regardless of toggle route");
}

void TestIndependentStatusEffectsAndPendingStartSuppression() {
  auto boot = Boot();
  boot.actions.program_mode =
      {FieldEffect::Preserve, FieldEffect::Preserve, StatusEffect::Clear,
       StatusEffect::Clear, PhaseEffect::Preserve, {0, 0}};
  VideoSchedule schedule(boot);
  Collector sink;
  schedule.AdvanceTo(65602, sink);
  Check(schedule.vsint() && schedule.effective_start_generation() == 1,
        "FIELD must assert GS VSINT after VBlank start");
  bool intc_stat = true;
  const ActionPolicy& policy = schedule.ApplyAction(Action::ProgramMode);
  Check(!schedule.vsint() && intc_stat &&
            policy.intc_stat == StatusEffect::Clear,
        "video applies GS VSINT policy but returns INTC policy to its owner");
  if (policy.intc_stat == StatusEffect::Clear) intc_stat = false;
  Check(!intc_stat, "INTC owner can apply its distinct returned effect");

  VideoSchedule pending(Boot());
  struct SuppressInNotification final : EventSink {
    VideoSchedule& schedule;
    std::vector<EventKind> kinds;
    explicit SuppressInNotification(VideoSchedule& value) : schedule(value) {}
    void OnVideoEvent(const Event& event) override {
      kinds.push_back(event.kind);
      if (event.kind == EventKind::ScheduledField) schedule.SetSint(true);
    }
  } suppress(pending);
  pending.AdvanceTo(0, suppress);
  Check(suppress.kinds.size() == 2 &&
            suppress.kinds[1] == EventKind::VBlankStartSuppressed &&
            pending.effective_start_generation() == 0,
        "SINT set in same-cycle notification must suppress pending start");
}

void TestHostPauseInvariance() {
  VideoSchedule direct(Boot());
  VideoSchedule paused(Boot());
  Collector a, b;
  direct.AdvanceTo(UINT64_MAX, a);
  paused.AdvanceTo(65601, b); // Before the fractional FIELD deadline.
  Check(paused.now() == 65601 && b.events.size() == 2,
        "FIELD cannot occur before its fractional deadline");
  paused.AdvanceTo(65602, b);
  paused.AdvanceTo(UINT64_MAX, b);
  Check(a.events.size() == b.events.size() && direct.now() == paused.now(),
        "pause must preserve event count and terminal coordinate");
  for (std::size_t i = 0; i < a.events.size(); ++i) {
    const auto& x = a.events[i];
    const auto& y = b.events[i];
    Check(x.kind == y.kind && x.cycle == y.cycle &&
              x.exact_deadline.cycle == y.exact_deadline.cycle &&
              x.exact_deadline.remainder == y.exact_deadline.remainder &&
              x.scheduled_field_id == y.scheduled_field_id &&
              x.effective_start_generation == y.effective_start_generation &&
              x.field == y.field,
          "pause changed the deterministic event transcript");
  }
}

void TestSynchronousReentrantNotification() {
  VideoSchedule schedule(Boot());
  struct Reenter final : EventSink {
    VideoSchedule& schedule;
    std::vector<EventKind> order;
    bool caller_resumed = false;
    explicit Reenter(VideoSchedule& value) : schedule(value) {}
    void OnVideoEvent(const Event& event) override {
      order.push_back(event.kind);
      if (event.kind == EventKind::ScheduledField) {
        Check(schedule.scheduled_field_id() == 1,
              "scheduled field must commit before notification");
        schedule.AdvanceTo(1, *this);
        caller_resumed = true;
      } else if (event.kind == EventKind::VBlankStart) {
        Check(!schedule.vsint() &&
                  schedule.effective_start_generation() == 1,
              "VBlank assertion must commit before notification");
      }
    }
  } sink(schedule);
  schedule.AdvanceTo(0, sink);
  Check(sink.caller_resumed && schedule.now() == 1 &&
            sink.order.size() == 2 &&
            sink.order[0] == EventKind::ScheduledField &&
            sink.order[1] == EventKind::VBlankStart,
        "nested advancement must preserve the synchronous caller stack");
}

void TestInvalidInitialization() {
  auto boot = Boot();
  boot.first_start = {0, kCycleDenominator};
  try {
    VideoSchedule invalid(boot);
    (void)invalid;
    Check(false, "invalid first-deadline remainder was accepted");
  } catch (const std::invalid_argument&) {
  }
  boot = Boot();
  boot.actions.repeat_mode.restart_delay.remainder = kCycleDenominator;
  try {
    VideoSchedule invalid(boot);
    (void)invalid;
    Check(false, "invalid mode policy was accepted");
  } catch (const std::invalid_argument&) {
  }
  boot = Boot();
  boot.first_start = {0, 1};
  VideoSchedule fractional(boot);
  Collector sink;
  fractional.AdvanceTo(1, sink);
  Check(sink.events.size() == 2 && sink.events[0].cycle == 1 &&
            sink.events[0].exact_deadline.remainder == 1,
        "explicit fractional first deadline must use ceiling without losing phase");
}

}  // namespace

void TestBoundTerminalOrdinal() {
  VideoSchedule schedule(Boot());
  schedule.SetTerminalOrdinal(4500);
  Collector sink;
  Check(schedule.AdvanceTo(UINT64_MAX, sink) == AdvanceResult::CutAtTerminalVBlankStart &&
            schedule.effective_start_generation() == 4500 &&
            schedule.now() == CeilNumerator(4499 * kFieldPeriodNumerator),
        "bound terminal ordinal must cut after exactly that effective start");
  bool rejected = false;
  try { schedule.SetTerminalOrdinal(5000); } catch (const std::logic_error&) { rejected = true; }
  Check(rejected, "terminal ordinal cannot move after the cut");
  VideoSchedule zero(Boot());
  rejected = false;
  try { zero.SetTerminalOrdinal(0); } catch (const std::logic_error&) { rejected = true; }
  Check(rejected, "terminal ordinal zero must be rejected");
}

void TestNoTerminalNeverCuts() {
  VideoSchedule schedule(Boot());
  schedule.SetTerminalOrdinal(kNoTerminal);
  Check(!schedule.has_terminal(), "kNoTerminal must report no terminal");
  Collector sink;
  // Well past the old 1,000,000-start product cap (about 4.6 hours of guest time).
  const uint64_t target = CeilNumerator(1'200'000 * kFieldPeriodNumerator);
  Check(schedule.AdvanceTo(target, sink) == AdvanceResult::ReachedTarget &&
            schedule.effective_start_generation() > 1'000'000 && !schedule.cut(),
        "a schedule without a terminal must run past 1,000,000 starts without cutting");
  VideoSchedule bounded(Boot());
  Check(bounded.has_terminal(), "a bound terminal must report has_terminal");
}

int main() {
  try {
    TestExact3000AndTerminalBatch();
    TestNoTerminalNeverCuts();
    TestBoundTerminalOrdinal();
    TestAssertionIdentityAndSeparateVsint();
    TestCutCountsEffectiveAssertions();
    TestTerminalMutationFreeze();
    TestSintAndExplicitModePolicies();
    TestRawRegisterFieldToggle();
    TestIndependentStatusEffectsAndPendingStartSuppression();
    TestHostPauseInvariance();
    TestSynchronousReentrantNotification();
    TestInvalidInitialization();
  } catch (const std::exception& error) {
    std::cerr << "gate3_guest_time_tests FAIL: " << error.what() << '\n';
    return 1;
  }
  std::cout << "gate3_guest_time_tests PASS\n";
}
