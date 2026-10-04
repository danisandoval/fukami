#pragma once
#include "rrv_guest_time.h"
#include "rrv_ee_timers.h"
#include "rrv_intc.h"
#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace rrv::guest_time {
struct TemporalCut : std::exception {
    const char* what() const noexcept override { return "Gate3 terminal effective VBlank-start cut"; }
};
// The register words are bound to compatible-v2 GS.cpp's selected measured
// NTSC programming table. Reset state is distinct from that transaction.
struct TemporalProfile {
    static constexpr uint64_t Smode1 = 0x0000000740814504ULL;
    static constexpr uint64_t Smode2 = 3;
    static constexpr uint64_t Syncv = 0x00c7800601a01801ULL;
    static constexpr uint64_t Synch1 = 0x0007f5b61f06f040ULL;
    static constexpr uint64_t Synch2 = 0x0033a4d8ULL;
    static constexpr uint64_t Srfsh = 8;
    uint32_t intc_stat;
    uint32_t intc_mask;
    // An explicit workload binding, never the old host 64us approximation.
    // Synthetic fixed-cycle ticks exist for asset-free tests only.
    std::optional<uint64_t> alarm_cycles_per_tick;
    int64_t rtc_epoch;
    int32_t rtc_timezone_minutes;
    // Source-bound unit: ps2sdk ee/kernel/include/kernel.h "Alarm value is in
    // H-SYNC ticks" (kernel Timer 3, HBLNK clock). Pinned PCSX2 Counters.cpp
    // clocks HBLNK counters at HBlank start only while SMODE1.SINT is clear.
    bool alarm_hsync_ticks = false;
    // Workload terminal: cut after this effective VBlank start; kNoTerminal never cuts.
    uint64_t terminal_starts = 3000;
};
class TemporalOwner final : private EventSink {
public:
    using Callback = std::function<void()>;
    struct Admission { uint64_t cycle, sequence; Callback commit; };
    // H-SYNC alarms have deadline UINT64_MAX and count `remaining` HBlank starts.
    struct Alarm { uint64_t deadline, sequence; uint32_t handler, arg, gp; uint16_t ticks; uint32_t remaining = 0; };
    explicit TemporalOwner(const TemporalProfile& profile);
    void HardwareTo(uint64_t target);
    uint64_t NextDeadline() const;
    // Gate-4 quiet commit: when `target` is before the deadline cached by the
    // last HardwareTo and no deadline input changed since, HardwareTo(target)
    // would commit no event, timer cause, alarm or admission. Move now() to
    // `target` and return true. EE timers and the video coordinate catch up
    // lazily: timers at their next access (path-independent between
    // deadlines), video at the next HardwareTo or before a video action.
    // Otherwise return false; the caller runs the full path.
    bool TryQuietTo(uint64_t target) {
        if (target < now_ || target >= quiet_until_) return false;
        now_ = target; return true;
    }
    bool Read(uint32_t address, unsigned width, uint64_t& value);
    bool Write(uint32_t address, unsigned width, uint64_t value);
    void ActionAt(Action action);
    void UpdateGsRoute(uint64_t smode1, uint64_t syncv) {
        rrv::guest_time::QuietBump(); quiet_until_ = 0;
        video_.SetSint((smode1 & (1ULL<<17)) != 0);
        video_.SetFieldToggle((syncv & 1) && (smode1 & 0x6000));
    }
    void SetSint(bool value) { rrv::guest_time::QuietBump(); quiet_until_ = 0; video_.SetSint(value); }
    void AcknowledgeVsint() { rrv::guest_time::QuietBump(); quiet_until_ = 0; video_.AcknowledgeVsint(); }
    void Program(uint64_t smode1, uint64_t smode2, uint64_t syncv, bool forced);
    int AddAlarm(uint16_t ticks, uint32_t handler, uint32_t arg, uint32_t gp);
    bool CancelAlarm(int id);
    void Admit(Admission admission);
    std::optional<std::pair<int, Alarm>> PopAlarm();
    bool HasAdmissions() const { return !admissions_.empty(); }
    bool HasAlarms() const { return !alarms_.empty() || !ready_alarms_.empty(); }
    bool HasReadyAlarms() const { return !ready_alarms_.empty(); }
    bool sint() const { return video_.sint(); }
    uint64_t now() const { return now_; }
    // Gate-4 lazy checkpoint: the quiet deadline and the clock it moves.
    uint64_t quiet_until() const { return quiet_until_; }
    uint64_t* lazy_now() { return &now_; }
    bool cut() const { return video_.cut(); }
    uint64_t terminal_starts() const { return video_.terminal_ordinal(); }
    bool has_terminal() const { return video_.has_terminal(); }
    uint64_t starts() const { return video_.effective_start_generation(); }
    std::optional<uint64_t> lastStartCycle() const { return last_start_cycle_; }
    bool field() const { return video_.field(); }
    uint64_t csr() const { return (uint64_t(video_.field())<<13) | (uint64_t(video_.vsint())<<3); }
    IntcState& intc() { return intc_; }
    // Mutable timer access may change the next timer deadline.
    EeTimers& timers() { rrv::guest_time::QuietBump(); quiet_until_ = 0; return timers_; }
    const EeTimers& timers() const { return timers_; }
    std::function<void(const Event&)> publish_video;
private:
    void OnVideoEvent(const Event& event) override;
    void CollectTimerCauses();
    // Only quiet commits leave the video coordinate behind now_, and no video
    // event is due before a quiet deadline, so this commits no event.
    void SyncVideo() { if (video_.now() < now_) video_.AdvanceTo(now_, *this); }
    uint64_t HDeadline() const;
    static bool TimerAddress(uint32_t address, unsigned& index, EeTimers::Register& reg);
    TemporalProfile profile_;
    VideoSchedule video_;
    EeTimers timers_;
    IntcState intc_;
    uint64_t now_ = 0, sequence_ = 0;
    // NextDeadline() as of the last HardwareTo; 0 = unknown (full path).
    uint64_t quiet_until_ = 0;
    std::optional<uint64_t> last_start_cycle_;
    // Exact rational extension of the pinned float hRender ratio, explicitly
    // declared in the candidate profile; not bit-identical counter rounding.
    __uint128_t h_numerator_ = 0;
    bool h_start_ = true;
    uint64_t smode1_ = TemporalProfile::Smode1;
    uint64_t smode2_ = TemporalProfile::Smode2;
    uint64_t syncv_ = TemporalProfile::Syncv;
    int next_alarm_ = 1;
    std::map<int, Alarm> alarms_;
    std::vector<std::pair<int, Alarm>> ready_alarms_;
    std::vector<Admission> admissions_;
};
}
