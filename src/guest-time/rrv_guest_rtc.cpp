#include "rrv_guest_rtc.h"

#include <limits>
#include <stdexcept>

namespace rrv::guest_time {
namespace {

constexpr int64_t kSecondsPerDay = 86400;
constexpr int64_t kUnixEpochDaysFromCivil = 719468;

struct CivilDate {
  int64_t year;
  unsigned month;
  unsigned day;
};

CivilDate CivilFromDays(int64_t days_since_epoch) {
  const int64_t z = days_since_epoch + kUnixEpochDaysFromCivil;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t year = static_cast<int64_t>(yoe) + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  const unsigned day = doy - (153 * mp + 2) / 5 + 1;
  const unsigned month = mp < 10 ? mp + 3 : mp - 9;
  year += month <= 2;
  return {year, month, day};
}

uint8_t Bcd(unsigned value) {
  return static_cast<uint8_t>(((value / 10) << 4) | (value % 10));
}

int64_t CheckedAdd(int64_t value, int64_t increment) {
  if ((increment > 0 && value > std::numeric_limits<int64_t>::max() - increment) ||
      (increment < 0 && value < std::numeric_limits<int64_t>::min() - increment))
    throw std::out_of_range("guest RTC time is outside the representable range");
  return value + increment;
}

void SplitDays(int64_t seconds, int64_t& days, int64_t& seconds_in_day) {
  days = seconds / kSecondsPerDay;
  seconds_in_day = seconds % kSecondsPerDay;
  if (seconds_in_day < 0) {
    --days;
    seconds_in_day += kSecondsPerDay;
  }
}

}  // namespace

GuestRtc::GuestRtc(int64_t initial_unix_utc_seconds,
                   int32_t timezone_offset_minutes)
    : initial_unix_utc_seconds_(initial_unix_utc_seconds),
      timezone_offset_minutes_(timezone_offset_minutes) {
  if (timezone_offset_minutes < -1439 || timezone_offset_minutes > 1439)
    throw std::invalid_argument("timezone offset must be within ±23:59");
}

int64_t GuestRtc::LocalSeconds(uint64_t ee_cycles) const {
  const uint64_t elapsed = ee_cycles / kEeCyclesPerSecond;
  if (elapsed > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
    throw std::out_of_range("EE cycle time exceeds representable Unix time");
  const int64_t utc = CheckedAdd(initial_unix_utc_seconds_,
                                 static_cast<int64_t>(elapsed));
  return CheckedAdd(utc, static_cast<int64_t>(timezone_offset_minutes_) * 60);
}

GuestRtc::ClockBytes GuestRtc::Read(uint64_t ee_cycles) const {
  const int64_t local = LocalSeconds(ee_cycles);
  int64_t days;
  int64_t seconds;
  SplitDays(local, days, seconds);
  const CivilDate date = CivilFromDays(days);
  const unsigned hour = static_cast<unsigned>(seconds / 3600);
  const unsigned minute = static_cast<unsigned>((seconds / 60) % 60);
  const unsigned second = static_cast<unsigned>(seconds % 60);
  return {0, Bcd(second), Bcd(minute), Bcd(hour), 0, Bcd(date.day),
          Bcd(date.month), Bcd(static_cast<unsigned>(date.year % 100))};
}

}  // namespace rrv::guest_time
