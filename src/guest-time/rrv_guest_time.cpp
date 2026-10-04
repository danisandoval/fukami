#include "rrv_guest_time.h"

#include <limits>
#include <stdexcept>

namespace rrv::guest_time {
namespace {

RationalCycle Add(RationalCycle a, RationalCycle b) {
  if (a.remainder >= kCycleDenominator || b.remainder >= kCycleDenominator)
    throw std::invalid_argument("invalid rational cycle remainder");
  const uint32_t sum = a.remainder + b.remainder;
  const uint64_t carry = sum / kCycleDenominator;
  const uint64_t maximum = std::numeric_limits<uint64_t>::max();
  if (b.cycle > maximum - a.cycle ||
      carry > maximum - a.cycle - b.cycle)
    throw std::overflow_error("virtual EE cycle overflow");
  return {a.cycle + b.cycle + carry, sum % kCycleDenominator};
}

RationalCycle AddNumerator(RationalCycle a, uint64_t numerator) {
  return Add(a, {numerator / kCycleDenominator,
                 static_cast<uint32_t>(numerator % kCycleDenominator)});
}

uint64_t DueCycle(RationalCycle deadline) {
  if (deadline.remainder >= kCycleDenominator)
    throw std::invalid_argument("invalid rational cycle remainder");
  if (deadline.remainder && deadline.cycle == std::numeric_limits<uint64_t>::max())
    throw std::overflow_error("virtual EE cycle overflow");
  return deadline.cycle + (deadline.remainder != 0);
}

int Rank(EventKind kind) {
  switch (kind) {
    case EventKind::ScheduledField: return 0;
    case EventKind::VBlankStart: return 1;
    case EventKind::VBlankStartSuppressed: return 1;
    case EventKind::FieldTransition: return 2;
    case EventKind::FieldTransitionSuppressed: return 2;
    case EventKind::VBlankEnd: return 3;
    case EventKind::VBlankEndSuppressed: return 3;
  }
  throw std::invalid_argument("unknown video event kind");
}

void ValidatePolicy(const ActionPolicy& policy) {
  if (policy.restart_delay.remainder >= kCycleDenominator)
    throw std::invalid_argument("invalid action policy remainder");
  for (FieldEffect effect : {policy.field, policy.sint}) {
    switch (effect) {
      case FieldEffect::Preserve: case FieldEffect::Clear: case FieldEffect::Set: break;
      default: throw std::invalid_argument("unknown field action policy");
    }
  }
  for (StatusEffect effect : {policy.gs_vsint, policy.intc_stat}) {
    switch (effect) {
    case StatusEffect::Preserve: case StatusEffect::Clear: break;
    default: throw std::invalid_argument("unknown status action policy");
    }
  }
  switch (policy.phase) {
    case PhaseEffect::Preserve: case PhaseEffect::Restart: break;
    default: throw std::invalid_argument("unknown phase action policy");
  }
}

}  // namespace

bool VideoSchedule::Later::operator()(const ScheduledEvent& a,
                                      const ScheduledEvent& b) const {
  if (a.cycle != b.cycle) return a.cycle > b.cycle;
  if (a.deadline.cycle != b.deadline.cycle)
    return a.deadline.cycle > b.deadline.cycle;
  if (a.deadline.remainder != b.deadline.remainder)
    return a.deadline.remainder > b.deadline.remainder;
  if (Rank(a.kind) != Rank(b.kind)) return Rank(a.kind) > Rank(b.kind);
  return a.sequence > b.sequence;
}

VideoSchedule::VideoSchedule(const ColdBoot& boot)
    : now_(boot.initial_cycle), field_(boot.initial_field),
      sint_(boot.initial_sint),
      start_assertion_enabled_(boot.start_assertion_enabled),
      vsint_(boot.initial_vsint),
      actions_(boot.actions) {
  ValidatePolicy(actions_.csr_reset);
  ValidatePolicy(actions_.program_mode);
  ValidatePolicy(actions_.repeat_mode);
  ValidatePolicy(actions_.force_mode);
  if (DueCycle(boot.first_start) < now_ || boot.first_start.cycle < now_)
    throw std::invalid_argument("first VBlank deadline precedes cold boot");
  Schedule(EventKind::ScheduledField, boot.first_start, 1);
}

void VideoSchedule::Schedule(EventKind kind, RationalCycle deadline,
                             uint64_t field_id) {
  if (sequence_ == std::numeric_limits<uint64_t>::max())
    throw std::overflow_error("video event sequence overflow");
  queue_.push({kind, deadline, DueCycle(deadline), field_id, ++sequence_});
}

const ActionPolicy& VideoSchedule::ApplyAction(Action action) {
  if (cut_ || cut_pending_)
    throw std::logic_error("video schedule already at terminal cut");
  const ActionPolicy* policy = nullptr;
  switch (action) {
    case Action::CsrReset: policy = &actions_.csr_reset; break;
    case Action::ProgramMode: policy = &actions_.program_mode; break;
    case Action::RepeatMode: policy = &actions_.repeat_mode; break;
    case Action::ForceMode: policy = &actions_.force_mode; break;
  }
  if (!policy) throw std::invalid_argument("unknown video action");
  RationalCycle restarted{};
  if (policy->phase == PhaseEffect::Restart)
    restarted = Add({now_, 0}, policy->restart_delay);

  switch (policy->field) {
    case FieldEffect::Preserve: break;
    case FieldEffect::Clear: field_ = false; break;
    case FieldEffect::Set: field_ = true; break;
  }
  switch (policy->sint) {
    case FieldEffect::Preserve: break;
    case FieldEffect::Clear: sint_ = false; break;
    case FieldEffect::Set: sint_ = true; break;
  }
  if (policy->gs_vsint == StatusEffect::Clear) vsint_ = false;
  if (policy->phase == PhaseEffect::Restart) {
    queue_ = {};
    if (scheduled_field_id_ == std::numeric_limits<uint64_t>::max())
      throw std::overflow_error("scheduled field identity overflow");
    Schedule(EventKind::ScheduledField, restarted, scheduled_field_id_ + 1);
  }
  return *policy;
}

void VideoSchedule::SetSint(bool value) {
  if (cut_ || cut_pending_)
    throw std::logic_error("video schedule already at terminal cut");
  sint_ = value;
}

void VideoSchedule::SetFieldToggle(bool enabled) {
  if (cut_ || cut_pending_)
    throw std::logic_error("video schedule already at terminal cut");
  toggle_field_ = enabled;
}

void VideoSchedule::SetStartAssertionEnabled(bool value) {
  if (cut_ || cut_pending_)
    throw std::logic_error("video schedule already at terminal cut");
  start_assertion_enabled_ = value;
}

void VideoSchedule::AcknowledgeVsint() {
  if (cut_ || cut_pending_)
    throw std::logic_error("video schedule already at terminal cut");
  vsint_ = false;
}

std::optional<uint64_t> VideoSchedule::NextDeadline() const {
  if (cut_ || cut_pending_ || queue_.empty()) return std::nullopt;
  return queue_.top().cycle;
}

void VideoSchedule::Commit(const ScheduledEvent& due, EventSink& sink) {
  now_ = due.cycle;
  EventKind delivered_kind = due.kind;
  switch (due.kind) {
    case EventKind::ScheduledField: {
      scheduled_field_id_ = due.field_id;
      if (start_assertion_enabled_ && !sint_)
        Schedule(EventKind::VBlankStart, due.deadline, due.field_id);
      Schedule(EventKind::FieldTransition,
               AddNumerator(due.deadline, kFieldOffsetNumerator), due.field_id);
      Schedule(EventKind::VBlankEnd,
               AddNumerator(due.deadline, kEndOffsetNumerator), due.field_id);
      if (due.field_id == std::numeric_limits<uint64_t>::max())
        throw std::overflow_error("scheduled field identity overflow");
      Schedule(EventKind::ScheduledField,
               AddNumerator(due.deadline, kFieldPeriodNumerator),
               due.field_id + 1);
      break;
    }
    case EventKind::VBlankStart:
      if (sint_) {
        delivered_kind = EventKind::VBlankStartSuppressed;
        break;
      }
      if (effective_start_generation_ == std::numeric_limits<uint64_t>::max())
        throw std::overflow_error("effective VBlank generation overflow");
      ++effective_start_generation_;
      if (terminal_ordinal_ != kNoTerminal && effective_start_generation_ == terminal_ordinal_) {
        cut_pending_ = true;
        cut_cycle_ = due.cycle;
      }
      break;
    case EventKind::FieldTransition:
      if (sint_) {
        delivered_kind = EventKind::FieldTransitionSuppressed;
      } else {
        field_ = toggle_field_ ? !field_ : true;
        vsint_ = true;
      }
      break;
    case EventKind::VBlankEnd:
      if (sint_) delivered_kind = EventKind::VBlankEndSuppressed;
      break;
    case EventKind::FieldTransitionSuppressed:
    case EventKind::VBlankStartSuppressed:
    case EventKind::VBlankEndSuppressed:
      throw std::logic_error("suppressed transition cannot be queued");
  }
  sink.OnVideoEvent({delivered_kind, due.cycle, due.deadline, due.field_id,
                     effective_start_generation_, field_});
}

void VideoSchedule::SetTerminalOrdinal(uint64_t ordinal) {
  if (ordinal == 0 || cut_ || cut_pending_ || effective_start_generation_ >= ordinal)
    throw std::logic_error("terminal ordinal must be positive and still ahead");
  terminal_ordinal_ = ordinal;
}

AdvanceResult VideoSchedule::AdvanceTo(uint64_t target_cycle, EventSink& sink) {
  if (target_cycle < now_)
    throw std::invalid_argument("virtual EE time cannot move backwards");
  if (cut_) return AdvanceResult::CutAtTerminalVBlankStart;
  while (!queue_.empty()) {
    const ScheduledEvent next = queue_.top();
    if (next.cycle > target_cycle &&
        (!cut_pending_ || next.cycle > cut_cycle_)) break;
    if (cut_pending_ && next.cycle > cut_cycle_) break;
    queue_.pop();
    Commit(next, sink);
    // A synchronous sink may advance virtual time recursively while retaining
    // its caller's stack. Its progress and terminal state take precedence.
    if (cut_) return AdvanceResult::CutAtTerminalVBlankStart;
  }
  if (cut_pending_) {
    cut_ = true;
    return AdvanceResult::CutAtTerminalVBlankStart;
  }
  if (now_ < target_cycle) now_ = target_cycle;
  return AdvanceResult::ReachedTarget;
}

}  // namespace rrv::guest_time
