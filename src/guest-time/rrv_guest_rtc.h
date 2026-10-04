#pragma once

#include <array>
#include <cstdint>

namespace rrv::guest_time {

class GuestRtc {
 public:
  static constexpr uint64_t kEeCyclesPerSecond = 294912000;
  using ClockBytes = std::array<uint8_t, 8>;

  // Inputs are fixed by the run manifest; no host clock or timezone database
  // participates in guest-visible time. The one exception is an unbounded
  // product workload (bound `rtc_source host_boot`), whose loader samples the
  // host clock once at boot and passes the result in here.
  GuestRtc(int64_t initial_unix_utc_seconds, int32_t timezone_offset_minutes);

  // Cycles are monotonic from the run's virtual-time origin.
  ClockBytes Read(uint64_t ee_cycles) const;

  // Local (offset-applied) Unix seconds at the same coordinate as Read().
  int64_t LocalSeconds(uint64_t ee_cycles) const;
  int32_t timezone_offset_minutes() const { return timezone_offset_minutes_; }

 private:
  int64_t initial_unix_utc_seconds_;
  int32_t timezone_offset_minutes_;
};

}  // namespace rrv::guest_time
