#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <queue>
#include <vector>

namespace rrv::guest_time {

// Exact NTSC-interlaced candidate: 294912000 EE cycles/s, 60000/1001
// fields/s, 525 lines/interlaced frame (262.5 lines/field). All three video
// offsets reduce to /125 cycle.
constexpr uint32_t kCycleDenominator = 125;
constexpr uint64_t kFieldPeriodNumerator = 615014400;
constexpr uint64_t kFieldOffsetNumerator = 8200192;  // 3.5 lines
constexpr uint64_t kEndOffsetNumerator = 52715520;   // 22.5 lines

struct RationalCycle {
  uint64_t cycle;
  uint32_t remainder;  // [0, 125); never rounded between fields.
};

enum class FieldEffect { Preserve, Clear, Set };
enum class StatusEffect { Preserve, Clear };
enum class PhaseEffect { Preserve, Restart };

// The source-qualified effects are deliberately supplied by the caller. In
// particular, CSR reset, first mode programming and same-mode programming are
// separate policies; this core does not infer their still-unbound PS2 behavior.
struct ActionPolicy {
  FieldEffect field;
  FieldEffect sint;
  StatusEffect gs_vsint;
  StatusEffect intc_stat;  // Returned to the INTC owner; never applied here.
  PhaseEffect phase;
  RationalCycle restart_delay;  // Used only for Restart, relative to now().

  ActionPolicy(FieldEffect field_value, FieldEffect sint_value,
               StatusEffect gs_vsint_value, StatusEffect intc_stat_value,
               PhaseEffect phase_value, RationalCycle delay)
      : field(field_value), sint(sint_value), gs_vsint(gs_vsint_value),
        intc_stat(intc_stat_value), phase(phase_value), restart_delay(delay) {}
};

struct ActionPolicies {
  ActionPolicy csr_reset;
  ActionPolicy program_mode;
  ActionPolicy repeat_mode;
  ActionPolicy force_mode;

  ActionPolicies(ActionPolicy csr, ActionPolicy program, ActionPolicy repeat,
                 ActionPolicy force)
      : csr_reset(csr), program_mode(program), repeat_mode(repeat),
        force_mode(force) {}
};

struct ColdBoot {
  uint64_t initial_cycle;
  RationalCycle first_start;  // Absolute exact deadline, at/after initial_cycle.
  bool initial_field;
  bool initial_sint;
  bool initial_vsint;
  bool start_assertion_enabled;
  ActionPolicies actions;

  ColdBoot(uint64_t cycle, RationalCycle first, bool field, bool sint,
           bool vsint, bool assertion_enabled, ActionPolicies action_policies)
      : initial_cycle(cycle), first_start(first), initial_field(field),
        initial_sint(sint), initial_vsint(vsint),
        start_assertion_enabled(assertion_enabled),
        actions(action_policies) {}
};

enum class Action { CsrReset, ProgramMode, RepeatMode, ForceMode };
enum class EventKind { ScheduledField, VBlankStart, VBlankStartSuppressed,
                       FieldTransition, FieldTransitionSuppressed, VBlankEnd,
                       VBlankEndSuppressed };

struct Event {
  EventKind kind;
  uint64_t cycle;               // First integer EE cycle at/after deadline.
  RationalCycle exact_deadline; // The unrounded rational coordinate.
  uint64_t scheduled_field_id;
  uint64_t effective_start_generation; // 0 until an assertion is committed.
  bool field;                    // Authoritative sampled CSR.FIELD after event.
};

// Called synchronously after each hardware state transition. The integration
// layer may stage IRQs/callbacks here; it must not dispatch the next guest step
// from this notification, especially at the terminal VBlank batch.
class EventSink {
 public:
  virtual ~EventSink() = default;
  virtual void OnVideoEvent(const Event& event) = 0;
};

// terminal_ordinal() of a schedule that never cuts (unbounded product sessions).
inline constexpr uint64_t kNoTerminal = std::numeric_limits<uint64_t>::max();

enum class AdvanceResult { ReachedTarget, CutAtTerminalVBlankStart };

class VideoSchedule {
 public:
  explicit VideoSchedule(const ColdBoot& boot);

  // Guest actions occur at the current virtual cycle and use their own
  // required source-qualified policy. Restart drops uncommitted video events.
  const ActionPolicy& ApplyAction(Action action);
  // Every guest-control mutation is rejected once the terminal start has
  // committed, including from its synchronous event notification.
  void SetSint(bool value);
  // The raw GS SYNCV/SMODE1 progressive predicate selects whether a FIELD
  // transition toggles or holds FIELD=1. It does not change event deadlines.
  void SetFieldToggle(bool enabled);
  void SetStartAssertionEnabled(bool value);
  void AcknowledgeVsint();
  // Terminal effective VBlank-start ordinal (the workload binding; default
  // 3000). Settable only before that many starts have committed. kNoTerminal
  // (product workloads with persistent storage) means the schedule never cuts.
  void SetTerminalOrdinal(uint64_t ordinal);
  uint64_t terminal_ordinal() const { return terminal_ordinal_; }
  bool has_terminal() const { return terminal_ordinal_ != kNoTerminal; }

  AdvanceResult AdvanceTo(uint64_t target_cycle, EventSink& sink);

  uint64_t now() const { return now_; }
  bool field() const { return field_; }
  bool sint() const { return sint_; }
  bool start_assertion_enabled() const { return start_assertion_enabled_; }
  bool vsint() const { return vsint_; }
  std::optional<uint64_t> NextDeadline() const;
  uint64_t scheduled_field_id() const { return scheduled_field_id_; }
  uint64_t effective_start_generation() const { return effective_start_generation_; }
  bool cut() const { return cut_; }
  std::size_t pending_video_events() const { return queue_.size(); }

 private:
  struct ScheduledEvent {
    EventKind kind;
    RationalCycle deadline;
    uint64_t cycle;
    uint64_t field_id;
    uint64_t sequence;
  };
  struct Later {
    bool operator()(const ScheduledEvent& a, const ScheduledEvent& b) const;
  };

  void Schedule(EventKind kind, RationalCycle deadline, uint64_t field_id);
  void Commit(const ScheduledEvent& due, EventSink& sink);

  uint64_t now_;
  bool field_;
  bool sint_;
  bool toggle_field_ = true;
  bool start_assertion_enabled_;
  bool vsint_;
  uint64_t scheduled_field_id_ = 0;
  uint64_t effective_start_generation_ = 0;
  uint64_t sequence_ = 0;
  uint64_t terminal_ordinal_ = 3000;
  bool cut_pending_ = false;
  bool cut_ = false;
  uint64_t cut_cycle_ = 0;
  ActionPolicies actions_;
  std::priority_queue<ScheduledEvent, std::vector<ScheduledEvent>, Later> queue_;
};

}  // namespace rrv::guest_time
