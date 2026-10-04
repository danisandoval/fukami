#include "rrv_temporal_owner.h"
#include <limits>
namespace rrv::guest_time {
namespace {
ActionPolicies SelectedPolicies() {
    using F = FieldEffect; using S = StatusEffect; using P = PhaseEffect;
    return {{F::Clear,F::Clear,S::Clear,S::Preserve,P::Preserve,{0,0}},
            {F::Set,F::Preserve,S::Preserve,S::Preserve,P::Preserve,{0,0}},
            {F::Preserve,F::Preserve,S::Preserve,S::Preserve,P::Preserve,{0,0}},
            {F::Set,F::Preserve,S::Preserve,S::Preserve,P::Preserve,{0,0}}};
}
constexpr uint64_t kLineNumerator = 2342912;
constexpr uint64_t kRenderNumerator = 14039675, kRenderDenominator = 16777216;
constexpr uint64_t kHDenominator = 125 * kRenderDenominator;
}
TemporalOwner::TemporalOwner(const TemporalProfile& profile)
    : profile_(profile), video_({0,{0,0},true,false,false,true,SelectedPolicies()}),
      intc_({profile.intc_stat, profile.intc_mask}) {
    video_.SetTerminalOrdinal(profile.terminal_starts);
    if (profile.alarm_hsync_ticks && profile.alarm_cycles_per_tick)
        throw std::logic_error("Gate3 alarm unit is ambiguous (H-SYNC and fixed cycles)");
}
uint64_t TemporalOwner::HDeadline() const {
    auto result = (h_numerator_ + kHDenominator - 1) / kHDenominator;
    if (result > UINT64_MAX) throw std::overflow_error("Gate3 HBlank deadline");
    return static_cast<uint64_t>(result);
}
uint64_t TemporalOwner::NextDeadline() const {
    uint64_t next = video_.NextDeadline().value_or(UINT64_MAX);
    next = std::min(next, HDeadline());
    if (auto timer = timers_.next_irq_deadline()) next = std::min(next, *timer);
    for (const auto& [id, alarm] : alarms_) next = std::min(next, alarm.deadline);
    if (!admissions_.empty()) next = std::min(next, admissions_.front().cycle);
    return next;
}
void TemporalOwner::CollectTimerCauses() {
    uint32_t pending = timers_.take_irq_assertions();
    for (unsigned cause = 9; cause <= 12; ++cause)
        if (pending & (1u<<cause)) intc_.RaiseCause(cause);
}
void TemporalOwner::OnVideoEvent(const Event& event) {
    switch (event.kind) {
    case EventKind::VBlankStart:
        last_start_cycle_ = event.cycle;
        intc_.RaiseVBlankStart(); timers_.edge_at(event.cycle, EeTimers::Edge::VBlankStart); break;
    case EventKind::VBlankEnd:
        intc_.RaiseCause(3); timers_.edge_at(event.cycle, EeTimers::Edge::VBlankEnd); break;
    default: break;
    }
    // Hardware publication only. Callbacks are never invoked by this sink.
    if (publish_video) publish_video(event);
}
void TemporalOwner::HardwareTo(uint64_t target) {
    if (target < now_) throw std::logic_error("Gate3 time reversal");
    quiet_until_ = 0;
    if (cut()) return;
    while (NextDeadline() <= target) {
        const uint64_t at = NextDeadline(); now_ = at;
        video_.AdvanceTo(at, *this);
        std::vector<std::pair<int,Alarm>> due;
        // Pinned rcntUpdate order: VSync phase, HScanline gates, counters.
        if (HDeadline() == at) {
            if (h_start_ && !video_.sint()) {
                for (auto it=alarms_.begin(); it!=alarms_.end();) {
                    if (it->second.remaining && --it->second.remaining == 0) {
                        it->second.deadline = at; due.push_back(*it); it=alarms_.erase(it);
                    } else ++it;
                }
            }
            // Pinned rcntUpdate_hScanline: rcntStartGate/rcntEndGate(false)
            // run only while SINT is clear; hsyncCounter.Mode always toggles.
            const auto edge = h_start_ ? EeTimers::Edge::HBlankStart : EeTimers::Edge::HBlankEnd;
            if (video_.sint()) timers_.level_at(at, edge);
            else timers_.edge_at(at, edge);
            h_numerator_ += __uint128_t(kLineNumerator) *
                (h_start_ ? kRenderDenominator-kRenderNumerator : kRenderNumerator);
            h_start_ = !h_start_;
        }
        timers_.advance_to(at); CollectTimerCauses();
        for (auto it=alarms_.begin(); it!=alarms_.end();) {
            if (it->second.deadline == at) { due.push_back(*it); it=alarms_.erase(it); }
            else ++it;
        }
        std::sort(due.begin(),due.end(),[](const auto& a,const auto& b){return a.second.sequence<b.second.sequence;});
        ready_alarms_.insert(ready_alarms_.end(),due.begin(),due.end());
        if (cut()) return; // Full hardware batch, before data or guest callbacks.
        while (!admissions_.empty() && admissions_.front().cycle == at) {
            auto admission=std::move(admissions_.front()); admissions_.erase(admissions_.begin());
            admission.commit(); // Data readiness may block host here; time stays at at.
        }
        if (cut()) return; // Entire same-cycle hardware batch precedes the cut.
    }
    now_=target; timers_.advance_to(target); CollectTimerCauses();
    video_.AdvanceTo(target,*this);
    quiet_until_ = cut() ? 0 : NextDeadline();
}
bool TemporalOwner::TimerAddress(uint32_t address,unsigned& index,EeTimers::Register& reg) {
    if (address < 0x10000000u || address > 0x10001830u) return false;
    const auto offset=address-0x10000000u; index=offset/0x800;
    if (index>=4) return false;
    switch (offset%0x800) {
    case 0: reg=EeTimers::Register::Count; return true;
    case 0x10: reg=EeTimers::Register::Mode; return true;
    case 0x20: reg=EeTimers::Register::Compare; return true;
    case 0x30: if(index<2){reg=EeTimers::Register::Hold;return true;} return false;
    default:return false;
    }
}
bool TemporalOwner::Read(uint32_t address,unsigned width,uint64_t& value) {
    // Scratchpad (0x70000000) is not a KSEG alias: masking it to a physical
    // address would land on the EE I/O page (timers at 0x10000000-0x10001830).
    if ((address>>28)==7u) return false;
    const uint32_t physical=address&0x1fffffffU, base=physical&~3U;
    unsigned index; EeTimers::Register reg;
    uint64_t raw;
    if (TimerAddress(base,index,reg)) raw=timers_.read_at(now_,index,reg);
    else if(base==IntcState::kStatAddress) raw=intc_.ReadStat();
    else if(base==IntcState::kMaskAddress) raw=intc_.ReadMask();
    else return false;
    value=raw>>((physical&3)*8);
    if(width<8)value&=(uint64_t(1)<<(width*8))-1;
    return true;
}
bool TemporalOwner::Write(uint32_t address,unsigned width,uint64_t value) {
    // Scratchpad (0x70000000) is not a KSEG alias: masking it to a physical
    // address would land on the EE I/O page (timers at 0x10000000-0x10001830).
    if ((address>>28)==7u) return false;
    const uint32_t physical=address&0x1fffffffU, base=physical&~3U;
    unsigned index; EeTimers::Register reg;
    const unsigned shift=(physical&3)*8;
    uint32_t bits=static_cast<uint32_t>(value<<shift);
    if(TimerAddress(base,index,reg)) {
        if(width<4 && reg!=EeTimers::Register::Mode) {
            uint32_t mask=((1u<<(width*8))-1)<<shift;
            bits=(timers_.read_at(now_,index,reg)&~mask)|(bits&mask);
        }
        quiet_until_=0; timers_.write_at(now_,index,reg,bits); CollectTimerCauses(); return true;
    }
    if(base==IntcState::kStatAddress){intc_.WriteStat(bits);return true;}
    if(base==IntcState::kMaskAddress){intc_.WriteMask(bits);return true;}
    return false;
}
void TemporalOwner::ActionAt(Action action) {
    quiet_until_=0; SyncVideo();
    const auto& policy=video_.ApplyAction(action);
    if(policy.intc_stat==StatusEffect::Clear) intc_.WriteStat(UINT32_MAX);
    // CSR RESET clears GS registers, not the selected counter-mode identity.
}
void TemporalOwner::Program(uint64_t smode1,uint64_t smode2,uint64_t syncv,bool forced) {
    // This candidate implements only its selected effective timing route.
    if((smode1&0x6000)!=0x4000 || !(smode2&1) || !(syncv&1))
        throw std::runtime_error("Gate3 unbound video route: requires a distinct explicit profile");
    // Mode identity is not the raw register image (SINT and FFMD are not
    // counter mode changes). CSR RESET preserves the selected counter mode.
    quiet_until_=0; SyncVideo();
    const bool changed=((smode1&0x6000)!=(smode1_&0x6000) || (smode2&1)!=(smode2_&1));
    UpdateGsRoute(smode1,syncv);
    video_.ApplyAction(forced ? Action::ForceMode : changed ? Action::ProgramMode : Action::RepeatMode);
    smode1_=smode1;smode2_=smode2;syncv_=syncv;
}
int TemporalOwner::AddAlarm(uint16_t ticks,uint32_t handler,uint32_t arg,uint32_t gp) {
    quiet_until_=0;
    if(profile_.alarm_hsync_ticks){
        // Timer 3 target = count + ticks; a zero request keeps the prior one-tick floor.
        int id=next_alarm_++;
        alarms_.emplace(id,Alarm{UINT64_MAX,++sequence_,handler,arg,gp,ticks,std::max<uint32_t>(ticks,1)});
        return id;
    }
    if(!profile_.alarm_cycles_per_tick || !*profile_.alarm_cycles_per_tick)
        throw std::logic_error("Gate3 alarm tick profile is unbound");
    const __uint128_t deadline=__uint128_t(now_)+std::max<uint16_t>(ticks,1)*__uint128_t(*profile_.alarm_cycles_per_tick);
    if(deadline>UINT64_MAX)throw std::overflow_error("Gate3 alarm deadline");
    int id=next_alarm_++; alarms_.emplace(id,Alarm{uint64_t(deadline),++sequence_,handler,arg,gp,ticks});return id;
}
bool TemporalOwner::CancelAlarm(int id){
    quiet_until_=0;
    bool erased=alarms_.erase(id)!=0;
    auto end=std::remove_if(ready_alarms_.begin(),ready_alarms_.end(),[&](const auto& a){return a.first==id;});
    erased=erased||end!=ready_alarms_.end();ready_alarms_.erase(end,ready_alarms_.end());return erased;
}
std::optional<std::pair<int,TemporalOwner::Alarm>> TemporalOwner::PopAlarm(){
    if(ready_alarms_.empty())return {};auto next=ready_alarms_.front();ready_alarms_.erase(ready_alarms_.begin());return next;
}
void TemporalOwner::Admit(Admission admission){
    quiet_until_=0;
    if(admission.cycle<now_)throw std::logic_error("Gate3 late admission");
    for(const auto& existing:admissions_)if(existing.sequence==admission.sequence)throw std::logic_error("Gate3 duplicate admission identity");
    admissions_.push_back(std::move(admission));
    std::stable_sort(admissions_.begin(),admissions_.end(),[](const auto& a,const auto& b){return std::tie(a.cycle,a.sequence)<std::tie(b.cycle,b.sequence);});
}
}
