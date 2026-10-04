#include "../src/guest-time/rrv_guest_rtc.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>

using rrv::guest_time::GuestRtc;

namespace {

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

constexpr uint64_t Cycles(uint64_t seconds) {
  return seconds * GuestRtc::kEeCyclesPerSecond;
}

void CheckClock(const GuestRtc& rtc, uint64_t seconds,
                const GuestRtc::ClockBytes& expected, const char* message) {
  Check(rtc.Read(Cycles(seconds)) == expected, message);
}

void TestCycleTimeAndPause() {
  const GuestRtc rtc(1704067199, 0);  // 2023-12-31 23:59:59 UTC
  const auto initial = rtc.Read(0);
  Check(rtc.Read(0) == initial, "same-cycle reads must match");
  Check(rtc.Read(GuestRtc::kEeCyclesPerSecond - 1) == initial,
        "subsecond cycles retain their remainder");
  CheckClock(rtc, 1, {0, 0x00, 0x00, 0x00, 0, 0x01, 0x01, 0x24},
             "one-second boundary and year rollover");
  Check(rtc.Read(0) == initial, "paused virtual cycles do not advance RTC");
}

void TestCalendarRollovers() {
  const GuestRtc minute(1704067259, 0);  // 2024-01-01 00:00:59
  CheckClock(minute, 1, {0, 0x00, 0x01, 0x00, 0, 0x01, 0x01, 0x24},
             "minute rollover");

  const GuestRtc month(1706745599, 0);  // 2024-01-31 23:59:59
  CheckClock(month, 1, {0, 0x00, 0x00, 0x00, 0, 0x01, 0x02, 0x24},
             "month rollover");

  const GuestRtc leap(1709251199, 0);  // 2024-02-29 23:59:59
  CheckClock(leap, 1, {0, 0x00, 0x00, 0x00, 0, 0x01, 0x03, 0x24},
             "leap day advances to March");

  const GuestRtc year(1735689599, 0);  // 2024-12-31 23:59:59
  CheckClock(year, 1, {0, 0x00, 0x00, 0x00, 0, 0x01, 0x01, 0x25},
             "year rollover");

  const GuestRtc century(4102444799, 0);  // 2099-12-31 23:59:59
  CheckClock(century, 1, {0, 0x00, 0x00, 0x00, 0, 0x01, 0x01, 0x00},
             "2100 year is encoded modulo 100");
}

void TestFixedOffsetAndReservedBytes() {
  const GuestRtc rtc(1704067200, 330);  // 2024-01-01 05:30:00 local
  CheckClock(rtc, 0, {0, 0x00, 0x30, 0x05, 0, 0x01, 0x01, 0x24},
             "fixed timezone offset and compatible-v2 byte layout");
}

template <typename Function>
void CheckThrows(Function function, const char* message) {
  bool threw = false;
  try {
    function();
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  Check(threw, message);
}

template <typename Function>
void CheckRangeThrows(Function function, const char* message) {
  bool threw = false;
  try {
    function();
  } catch (const std::out_of_range&) {
    threw = true;
  }
  Check(threw, message);
}

void TestInvalidInputs() {
  CheckThrows([] { GuestRtc(1704067200, 1440); }, "reject offset above range");
  CheckThrows([] { GuestRtc(1704067200, -1440); }, "reject offset below range");

  const GuestRtc before_epoch(-1, 0);
  CheckClock(before_epoch, 0, {0, 0x59, 0x59, 0x23, 0, 0x31, 0x12, 0x69},
             "negative epoch uses floor calendar day");
  const GuestRtc max_positive(std::numeric_limits<int64_t>::max(), 1);
  CheckRangeThrows([&] { max_positive.Read(0); },
                   "positive offset overflow is rejected");
  const GuestRtc max_negative(std::numeric_limits<int64_t>::min(), -1);
  CheckRangeThrows([&] { max_negative.Read(0); },
                   "negative offset underflow is rejected");
  const GuestRtc min_zero(std::numeric_limits<int64_t>::min(), 0);
  Check(min_zero.Read(0)[1] <= 0x59,
        "minimum epoch safely normalizes negative day remainder");
  const GuestRtc positive(std::numeric_limits<int64_t>::max(), 0);
  CheckRangeThrows([&] { positive.Read(GuestRtc::kEeCyclesPerSecond); },
                   "elapsed-time overflow is rejected");
}

}  // namespace

void TestLocalSeconds() {
  const GuestRtc rtc(972518400, -90);  // 2000-10-26 00:00:00 UTC, UTC-01:30
  Check(rtc.LocalSeconds(0) == 972518400 - 5400, "local seconds apply the fixed offset");
  Check(rtc.LocalSeconds(Cycles(2) + 1) == 972518400 - 5400 + 2,
        "local seconds follow modeled cycles only");
  Check(rtc.timezone_offset_minutes() == -90, "offset accessor");
}

int main() {
  try {
    TestLocalSeconds();
    TestCycleTimeAndPause();
    TestCalendarRollovers();
    TestFixedOffsetAndReservedBytes();
    TestInvalidInputs();
  } catch (const std::exception& error) {
    std::cerr << "gate3_guest_rtc_tests FAIL: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "gate3_guest_rtc_tests PASS\n";
  return EXIT_SUCCESS;
}
