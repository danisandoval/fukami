#include <chrono>
#include <sstream>
#include <thread>
#if !defined(_WIN32)
#include <sys/resource.h>
#endif
namespace ps2_syscalls {
    void gate3DeliverTemporal(uint8_t*, R5900Context*, PS2Runtime*);
    void gate3RetireCallback(int, PS2Runtime*);
    void gate3PublishTemporal(uint64_t,bool);
    bool gate3HasTemporalWake(R5900Context*, PS2Runtime*);
}
namespace { thread_local std::map<PS2Runtime*,R5900Context*> gate3WaitContexts; }
// Qualification diagnostics. Env-gated and off by default. The trace reads
// guest state only; the pauses delay the host only. Neither changes guest time.
//  RRV_GATE3_SEMANTIC_TRACE=<path>: one line per video event (E), per effective
//    start a summary of the guest instruction stream and temporal MMIO reads
//    since the previous start (S), each temporal MMIO write (W), callback
//    entry/exit (C+/C-), sceGsSyncV service entry/return (V+/V-), CD request (D), pad
//    admission (P) and the terminal cut with timer state (X).
//  RRV_GATE3_HOST_PAUSE=<site,...>:<every>:<microseconds>: sleep the host at
//    every <every>-th hit of each listed site: vblank, field, backend, wait,
//    callback, syncv, io.
//  RRV_GATE3_CD_MISSES=<n>: report each CD read as host-missing <n> times, so
//    it goes through the bound host-data wait before the real read.
namespace {
enum Gate3PauseSiteV1 { kPauseVblank, kPauseField, kPauseBackend, kPauseWait, kPauseCallback,
    kPauseSyncv, kPauseIo, kPauseSites };
struct Gate3DiagV1 {
    static constexpr uint64_t kBasis=0x9e3779b97f4a7c15ull;
    FILE* trace=nullptr;
    uint32_t pauseMask=0; uint64_t pauseEvery=0,pauseUs=0;
    uint64_t pauseHits[kPauseSites]={},pauses[kPauseSites]={};
    unsigned cdMisses=0; uint64_t cdMissWaits=0;
    uint64_t checkpoints=0,pcHash=kBasis,reads=0,readHash=kBasis,writes=0;
};
Gate3DiagV1& gate3DiagV1() {
    static Gate3DiagV1 d=[]{Gate3DiagV1 d;
        if(const char* v=std::getenv("RRV_GATE3_SEMANTIC_TRACE");v&&*v){
            d.trace=std::fopen(v,"w");
            if(!d.trace)throw std::runtime_error("Gate3 semantic trace: cannot open output");
            std::setvbuf(d.trace,nullptr,_IOFBF,1<<20);
        }
        if(const char* v=std::getenv("RRV_GATE3_HOST_PAUSE");v&&*v){
            static const char* names[kPauseSites]={"vblank","field","backend","wait","callback","syncv","io"};
            const std::string spec(v); const auto a=spec.find(':'), b=spec.find(':',a==std::string::npos?a:a+1);
            if(a==std::string::npos||b==std::string::npos)throw std::runtime_error("RRV_GATE3_HOST_PAUSE: <sites>:<every>:<us>");
            std::string sites=spec.substr(0,a)+",";
            for(size_t at=0,next;(next=sites.find(',',at))!=std::string::npos;at=next+1){
                const std::string name=sites.substr(at,next-at); bool known=false;
                for(int i=0;i<kPauseSites;++i)if(name==names[i]){d.pauseMask|=1u<<i;known=true;}
                if(!known)throw std::runtime_error("RRV_GATE3_HOST_PAUSE: unknown site "+name);
            }
            d.pauseEvery=std::strtoull(spec.c_str()+a+1,nullptr,10);
            d.pauseUs=std::strtoull(spec.c_str()+b+1,nullptr,10);
            if(!d.pauseEvery||!d.pauseUs)throw std::runtime_error("RRV_GATE3_HOST_PAUSE: every and us must be positive");
        }
        if(const char* v=std::getenv("RRV_GATE3_CD_MISSES");v&&*v)d.cdMisses=unsigned(std::strtoul(v,nullptr,10));
        return d;}();
    return d;
}
// Gate-4 G4-I1 per-start host clock. RRV_GATE4_START_CLOCK=<path>: one
// steady_clock ns value at temporal configure, at each effective VBlank start
// (where the semantic trace writes its S summary) and at the cut, stored in an
// array sized at configure. Written once, at the cut (or at exit if no cut), as
// "configure <ns>", "<ordinal> <ns>" and "cut <ns>" lines. No I/O, lock or
// allocation per start; host-only and read-only to the guest; off by default.
// Beside it, the CPU time the producing thread (the game thread) has used at each start
// (CLOCK_THREAD_CPUTIME_ID), written as "<ordinal> <ns>" lines to start-cpu.txt next to
// start-clock.txt (or <path>.cpu): the game thread's cost per start where the wall clock
// cannot show it (a paced run, or a host that another thread or the display limits).
// And the CPU time the whole process has used (getrusage, user + system, every thread),
// to start-proc-cpu.txt: the cost of a start across the game, VU1 and GS threads, which
// is what a change to the owner thread moves.
struct Gate4StartClockV1 {
    std::string path; std::vector<uint64_t> ns,cpu,proc;
    uint64_t configureNs=0,cutNs=0,dropped=0; bool enabled=false,written=false;
    static uint64_t Now(){return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());}
    static uint64_t ThreadCpuNow(){timespec t{};
        return clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t)==0 ? uint64_t(t.tv_sec)*1000000000ull+uint64_t(t.tv_nsec) : 0;}
    static uint64_t ProcessCpuNow(){
#if !defined(_WIN32)
        rusage u{};
        if(getrusage(RUSAGE_SELF,&u)!=0)return 0;
        return (uint64_t(u.ru_utime.tv_sec)+uint64_t(u.ru_stime.tv_sec))*1000000000ull+
               (uint64_t(u.ru_utime.tv_usec)+uint64_t(u.ru_stime.tv_usec))*1000ull;
#else
        return 0;
#endif
    }
    void Record(uint64_t ordinal){if(!enabled)return; const uint64_t t=Now();
        if(ordinal<ns.size()){ns[ordinal]=t;cpu[ordinal]=ThreadCpuNow();proc[ordinal]=ProcessCpuNow();} else ++dropped;}
    std::string SiblingPath(const char* name,const char* suffix) const {
        static const std::string kName="start-clock.txt";
        if(path.size()>=kName.size() && path.compare(path.size()-kName.size(),kName.size(),kName)==0)
            return path.substr(0,path.size()-kName.size())+name;
        return path+suffix;
    }
    std::string CpuPath() const {return SiblingPath("start-cpu.txt",".cpu");}
    std::string ProcCpuPath() const {return SiblingPath("start-proc-cpu.txt",".proccpu");}
    void Write(){
        if(!enabled||written)return; written=true;
        FILE* f=std::fopen(path.c_str(),"w");
        if(!f){std::fprintf(stderr,"[gate4-clock] cannot open %s\n",path.c_str());return;}
        std::setvbuf(f,nullptr,_IOFBF,1<<20);
        std::fprintf(f,"configure %llu\n",(unsigned long long)configureNs);
        uint64_t n=0;
        for(uint64_t o=1;o<ns.size();++o)if(ns[o]){std::fprintf(f,"%llu %llu\n",(unsigned long long)o,(unsigned long long)ns[o]);++n;}
        if(cutNs)std::fprintf(f,"cut %llu\n",(unsigned long long)cutNs);
        std::fclose(f);
        if(FILE* c=std::fopen(CpuPath().c_str(),"w")){
            std::setvbuf(c,nullptr,_IOFBF,1<<20);
            for(uint64_t o=1;o<ns.size();++o)if(ns[o])std::fprintf(c,"%llu %llu\n",(unsigned long long)o,(unsigned long long)cpu[o]);
            std::fclose(c);
        }
        if(FILE* c=std::fopen(ProcCpuPath().c_str(),"w")){
            std::setvbuf(c,nullptr,_IOFBF,1<<20);
            for(uint64_t o=1;o<ns.size();++o)if(ns[o])std::fprintf(c,"%llu %llu\n",(unsigned long long)o,(unsigned long long)proc[o]);
            std::fclose(c);
        }
        std::fprintf(stderr,"[gate4-clock] starts=%llu dropped=%llu cut=%d path=%s\n",
            (unsigned long long)n,(unsigned long long)dropped,int(cutNs!=0),path.c_str());
    }
};
Gate4StartClockV1& gate4StartClockV1(){static Gate4StartClockV1 c;return c;}
// Product real-time pacing. RRV_GATE3_REALTIME=1: at each effective VBlank
// start the host waits until that start's real-time deadline, n x 1001/60000 s
// after the anchoring start, so the guest never runs faster than 60000/1001
// starts per host second. It only delays the host, at the same VBlank site as
// RRV_GATE3_HOST_PAUSE=vblank, which Gate-3 qualification showed changes no
// guest state. Off by default: qualification and Gate-4 timing runs stay
// unpaced.
// Catch-up (Gate-8 A3, owner decision 2026-09-29): a start that is late by at
// most RRV_GATE3_REALTIME_CATCHUP_MS (default 100) keeps its anchor, so the
// following starts run without waiting until the schedule is met again and
// guest time (and the audio it produces) does not fall behind real time after
// a short host hitch. Later than that, the pacer re-anchors at that start
// instead of bursting. The window is never shorter than one period;
// RRV_GATE3_REALTIME_CATCHUP_MS=0 restores the previous one-period rule.
// At exit it prints waits, catch-up starts and re-anchors (host-only).
// Spin (Gate 5, Steam Deck; `[timing] pacer_spin`): RRV_GATE3_REALTIME_SPIN_MS=N
// (0..17, default 0) sleeps until N ms before the deadline and busy-waits the
// rest; 17 busy-waits the whole wait. A game thread that sleeps between starts
// lets the Deck's CPU clock down from 3.5 to 2.4 GHz, and the same guest work
// then costs 30-45% more CPU (docs/evidence/GATE5_STEAMDECK_PROFILE_2026-10-02.md).
// Host-only like the sleep it replaces: the wait ends at the same deadline.
struct Gate3RealtimePacerV1;
Gate3RealtimePacerV1& gate3RealtimePacerV1();
struct Gate3RealtimePacerV1 {
    using Clock=std::chrono::steady_clock;
    static constexpr uint64_t kPeriodNum=1001000000000ull, kPeriodDen=60000ull; // ns = n*num/den
    bool enabled=false,anchored=false;
    Clock::time_point anchor{};
    Clock::duration catchup{},spin{};
    uint64_t n=0,waits=0,late=0,reanchors=0;
    // RRV_GATE3_FAST_FORWARD=N: the first N starts run unpaced, then pacing
    // anchors (replay debugging: reach a scene fast, then watch it live).
    uint64_t fastForward=0,starts=0;
    void Configure(){
        const char* v=std::getenv("RRV_GATE3_REALTIME");
        *this=Gate3RealtimePacerV1{}; enabled=v&&std::strcmp(v,"1")==0;
        if(const char* f=std::getenv("RRV_GATE3_FAST_FORWARD");f&&*f){
            char* end=nullptr; const unsigned long long k=std::strtoull(f,&end,10);
            if(end==f||*end)throw std::runtime_error("RRV_GATE3_FAST_FORWARD must be a start count");
            fastForward=k;
        }
        long ms=100;
        if(const char* c=std::getenv("RRV_GATE3_REALTIME_CATCHUP_MS");c&&*c){
            char* end=nullptr; ms=std::strtol(c,&end,10);
            if(end==c||*end||ms<0||ms>1000)throw std::runtime_error("RRV_GATE3_REALTIME_CATCHUP_MS must be 0..1000");
        }
        catchup=std::max<Clock::duration>(std::chrono::milliseconds(ms),Deadline(1)-anchor);
        long spinMs=0;
        if(const char* c=std::getenv("RRV_GATE3_REALTIME_SPIN_MS");c&&*c){
            char* end=nullptr; spinMs=std::strtol(c,&end,10);
            if(end==c||*end||spinMs<0||spinMs>17)throw std::runtime_error("RRV_GATE3_REALTIME_SPIN_MS must be 0..17");
        }
        spin=std::chrono::milliseconds(spinMs);
        if(!enabled)return;
        std::fprintf(stderr,"[gate3] real-time pacing: 60000/1001 effective starts per host second, catch-up %lld ms, spin %ld ms\n",
            (long long)std::chrono::duration_cast<std::chrono::milliseconds>(catchup).count(),spinMs);
        if(fastForward)std::fprintf(stderr,"[gate3] fast-forward: unpaced until start %llu\n",(unsigned long long)fastForward);
        static const bool atExit=[]{std::atexit([]{const auto& p=gate3RealtimePacerV1();
            if(p.enabled)std::fprintf(stderr,"[gate3] pacer waits=%llu late=%llu reanchors=%llu\n",
                (unsigned long long)p.waits,(unsigned long long)p.late,(unsigned long long)p.reanchors);});return true;}();
        (void)atExit;
    }
    Clock::time_point Deadline(uint64_t k)const{
        return anchor+std::chrono::nanoseconds(static_cast<int64_t>(k*kPeriodNum/kPeriodDen));}
    void AtStart(){
        if(!enabled)return;
        if(++starts<=fastForward){
            if(starts==fastForward)std::fprintf(stderr,"[gate3] fast-forward reached start %llu; pacing from here\n",
                (unsigned long long)starts);
            return;
        }
        const auto now=Clock::now();
        if(!anchored){anchored=true;anchor=now;n=0;return;}
        const auto due=Deadline(++n);
        if(now<due){
            ++waits;
            if(spin==Clock::duration::zero())std::this_thread::sleep_until(due);
            else{
                if(due-now>spin)std::this_thread::sleep_until(due-spin);
                while(Clock::now()<due)_mm_pause();
            }
        }
        else if(now-due>catchup){++reanchors;anchor=now;n=0;}
        else ++late;
    }
};
Gate3RealtimePacerV1& gate3RealtimePacerV1(){static Gate3RealtimePacerV1 p;return p;}
inline void gate3MixV1(uint64_t& h,uint64_t w){h=(h^w)*0x100000001b3ull;h^=h>>29;}
void gate3HostPauseV1(Gate3PauseSiteV1 site) {
    auto& d=gate3DiagV1();
    if(!(d.pauseMask&(1u<<site)) || ++d.pauseHits[site]%d.pauseEvery)return;
    std::this_thread::sleep_for(std::chrono::microseconds(d.pauseUs)); ++d.pauses[site];
}
}
void PS2Runtime::gate3DiagTextV1(const char* text) {
    auto& d=gate3DiagV1(); if(d.trace)std::fputs(text,d.trace);
}
void PS2Runtime::gate3DiagCdV1(const char* route,uint32_t lbn,uint32_t sectors,bool iop,bool success,
                               uint64_t bytes,uint64_t digest) {
    auto& d=gate3DiagV1();
    if(d.trace)std::fprintf(d.trace,"D %s lbn=%u sectors=%u iop=%d ok=%d bytes=%llu digest=%016llx cyc=%llu start=%llu\n",
        route,lbn,sectors,int(iop),int(success),(unsigned long long)bytes,(unsigned long long)digest,
        (unsigned long long)gate3TemporalV1().now(),(unsigned long long)gate3TemporalV1().starts());
}
void PS2Runtime::gate3DiagSyncVV1(R5900Context* ctx,bool entry) {
    // sceGsSyncV service entry (before its wait) and return, with the raw v0.
    auto& d=gate3DiagV1();
    if(entry)gate3HostPauseV1(kPauseSyncv);
    if(!d.trace)return;
    if(entry)std::fprintf(d.trace,"V+ ctx=%d ra=%x cyc=%llu stat=%x field=%d\n",rrv::guest_time::context_id,
        uint32_t(_mm_cvtsi128_si32(ctx->r[31])),(unsigned long long)gate3TemporalV1().now(),
        gate3TemporalV1().intc().ReadStat(),int(gate3TemporalV1().field()));
    else std::fprintf(d.trace,"V- ctx=%d v0=%llx cyc=%llu stat=%x field=%d\n",rrv::guest_time::context_id,
        (unsigned long long)_mm_cvtsi128_si64(ctx->r[2]),(unsigned long long)gate3TemporalV1().now(),
        gate3TemporalV1().intc().ReadStat(),int(gate3TemporalV1().field()));
}
void PS2Runtime::gate3RecordPadV1(uint64_t cycle) {
    // Recording is an input source, not a diagnostic: the admitted sample is
    // the one parsed back from the logged text, exactly as the loader parses
    // it, so a replay of the log admits identical state at the same cycle.
    static FILE* out=[]{const char* v=std::getenv("RRV_GATE3_RECORD_PAD");
        if(!v||!*v)return static_cast<FILE*>(nullptr);
        FILE* f=std::fopen(v,"w");
        if(!f)throw std::runtime_error("Gate3 pad recording: cannot open output");
        return f;}();
    if(!out || !m_gate3GuestWorkloadV1 || gate3AtCutV1())return;
    if(!m_gate3GuestWorkloadV1->pad.empty())
        throw std::logic_error("Gate3 pad recording needs a workload without pad admissions");
    static uint64_t sequence=0;
    for(unsigned port=0;port<m_gate3AdmittedPadV1.size();++port){
        const HostPadState host=m_padBackend.snapshot(int(port));
        const HostPadCapabilities caps=m_padBackend.capabilities(int(port));
        char text[320];
        std::snprintf(text,sizeof(text),"%d %llx %x %.9g %.9g %.9g %.9g %.9g %.9g %d %d %d %.9g",
            int(host.connected),(unsigned long long)host.deviceId,host.pressedButtons,
            host.leftX,host.leftY,host.rightX,host.rightY,host.leftTrigger,host.rightTrigger,
            int(caps.analogSticks),int(caps.analogTriggers),int(caps.rumble),m_padBackend.deadzone());
        std::istringstream words(text); std::vector<std::string> t; for(std::string w;words>>w;)t.push_back(w);
        Gate3PadSampleV1 sample;
        sample.state.connected=t[0]=="1"; sample.state.deviceId=std::stoull(t[1],nullptr,16);
        sample.state.pressedButtons=uint32_t(std::stoul(t[2],nullptr,16));
        float* axes[6]={&sample.state.leftX,&sample.state.leftY,&sample.state.rightX,&sample.state.rightY,
                        &sample.state.leftTrigger,&sample.state.rightTrigger};
        for(int i=0;i<6;++i)*axes[i]=std::stof(t[3+i]);
        sample.capabilities.analogSticks=t[9]=="1"; sample.capabilities.analogTriggers=t[10]=="1";
        sample.capabilities.rumble=t[11]=="1"; sample.deadzone=std::stof(t[12]);
        const auto& cur=m_gate3AdmittedPadV1[port];
        const bool same=cur.state.connected==sample.state.connected && cur.state.deviceId==sample.state.deviceId &&
            cur.state.pressedButtons==sample.state.pressedButtons && cur.state.leftX==sample.state.leftX &&
            cur.state.leftY==sample.state.leftY && cur.state.rightX==sample.state.rightX &&
            cur.state.rightY==sample.state.rightY && cur.state.leftTrigger==sample.state.leftTrigger &&
            cur.state.rightTrigger==sample.state.rightTrigger &&
            cur.capabilities.analogSticks==sample.capabilities.analogSticks &&
            cur.capabilities.analogTriggers==sample.capabilities.analogTriggers &&
            cur.capabilities.rumble==sample.capabilities.rumble && cur.deadzone==sample.deadzone;
        if(same)continue;
        m_gate3AdmittedPadV1[port]=sample;
        std::fprintf(out,"{\"cycle\":%llu,\"sequence\":%llu,\"port\":%u,\"sample\":\"%s\",\"start\":%llu}\n",
            (unsigned long long)cycle,(unsigned long long)++sequence,port,text,
            (unsigned long long)gate3TemporalV1().starts());
        std::fflush(out);
    }
}
void PS2Runtime::gate3DiagCdHostV1() {
    auto& d=gate3DiagV1();
    gate3HostPauseV1(kPauseIo);
    for(unsigned miss=0;miss<d.cdMisses;++miss){gate3WaitCdHostV1();++d.cdMissWaits;}
}
rrv::guest_time::TemporalOwner& PS2Runtime::gate3TemporalV1() {
    if(!m_gate3TemporalV1)throw std::logic_error("Gate3 temporal profile unbound");
    return *m_gate3TemporalV1;
}
void PS2Runtime::gate3PublishCsrV1() {
    auto& gs=m_memory.gs();
    // Owner-async: the interrupt check reads bridge-owned bits only if IMR
    // unmasks one (design §4.2 item 2); otherwise it needs no owner result.
    if(m_vu1gsModeV1==Vu1GsModeV1::OwnerAsync && (~(gs.imr>>8)&0x17u)!=0u)gate4FenceV1();
    rrv::gate4::ownerCsrPublish(gs.imr);
    rrv::guest_time::QuietBumpProducer();
    gs.csr=(gs.csr&~uint64_t(0x2008))|gate3TemporalV1().csr();
    if((gs.csr & ~(gs.imr>>8) & 0x1fu)!=0)gate3TemporalV1().intc().RaiseCause(0);
    ps2_syscalls::gate3PublishTemporal(gate3TemporalV1().starts(),gate3TemporalV1().field());
}
void PS2Runtime::gate3ConfigureTemporalV1(const rrv::guest_time::TemporalProfile& profile) {
    if(m_gate3TemporalV1 || gate3ModeledEeCyclesV1())throw std::logic_error("Gate3 cold boot already advanced");
    gate3ConfigureRtcV1(profile.rtc_epoch,profile.rtc_timezone_minutes);
    m_gate3TemporalV1=std::make_unique<rrv::guest_time::TemporalOwner>(profile);
    if(const char* v=std::getenv("RRV_GATE4_START_CLOCK");v&&*v){
        auto& c=gate4StartClockV1(); c=Gate4StartClockV1{};
        c.path=v; c.ns.assign((m_gate3TemporalV1->has_terminal()?m_gate3TemporalV1->terminal_starts():uint64_t(1)<<20)+2,0); c.cpu.assign(c.ns.size(),0); c.proc.assign(c.ns.size(),0); c.enabled=true;
        static const bool atExit=[]{std::atexit([]{gate4StartClockV1().Write();});return true;}(); (void)atExit;
        c.configureNs=Gate4StartClockV1::Now();
    }else gate4StartClockV1().enabled=false;
    gate3RealtimePacerV1().Configure();
    auto& gs=m_memory.gs();rrv::guest_time::QuietBump();
    gs.smode1=profile.Smode1;gs.smode2=profile.Smode2;gs.syncv=profile.Syncv;
    gs.synch1=profile.Synch1;gs.synch2=profile.Synch2;gs.srfsh=profile.Srfsh;
    gs.imr=0x7f00; // CSR RESET IMR value, not the old HLE unmasked default.
    gate3PublishCsrV1();
    m_gate3TemporalV1->publish_video=[this](const rrv::guest_time::Event& event){
        if(gate3DiagV1().trace)gate4FenceV1(); // the trace prints the published CSR
        gate3PublishCsrV1();
        if(event.kind==rrv::guest_time::EventKind::VBlankStart){gate4StartClockV1().Record(gate3TemporalV1().starts());
            rrv::gate4::ownerStart(gate3TemporalV1().starts());}
        if(auto& d=gate3DiagV1();d.trace){
            std::fprintf(d.trace,"E kind=%d cyc=%llu fid=%llu gen=%llu field=%d csr=%llx stat=%x mask=%x\n",int(event.kind),
                (unsigned long long)event.cycle,(unsigned long long)event.scheduled_field_id,
                (unsigned long long)event.effective_start_generation,int(event.field),(unsigned long long)m_memory.gs().csr,
                gate3TemporalV1().intc().ReadStat(),gate3TemporalV1().intc().ReadMask());
            if(event.kind==rrv::guest_time::EventKind::VBlankStart){
                std::fprintf(d.trace,"S start=%llu chk=%llu pc=%016llx rd=%llu rdh=%016llx wr=%llu\n",
                    (unsigned long long)gate3TemporalV1().starts(),(unsigned long long)d.checkpoints,(unsigned long long)d.pcHash,
                    (unsigned long long)d.reads,(unsigned long long)d.readHash,(unsigned long long)d.writes);
                d.checkpoints=d.reads=d.writes=0;d.pcHash=d.readHash=Gate3DiagV1::kBasis;
            }
        }
        // Read-only observation for phase anchors: the game's scene phase
        // (0x334E94) and attract cursor (0x3348AC) at each effective VBlank
        // start, printed on change. Nothing is written to guest state.
        if(event.kind==rrv::guest_time::EventKind::VBlankStart){
            const uint8_t* ram=m_memory.getRDRAM();
            const auto load=[ram](uint32_t address){uint32_t value;std::memcpy(&value,ram+address,4);return value;};
            const uint32_t phase=load(0x334E94u), cursor=load(0x3348ACu);
            const bool valid=cursor!=0u && (cursor&3u)==0u && cursor<=0x01FFFFF8u;
            const int32_t script=valid ? int32_t(load(cursor)) : -1, step=valid ? int32_t(load(cursor+4u)) : -1;
            static uint32_t lastPhase=UINT32_MAX; static int32_t lastScript=INT32_MIN,lastStep=INT32_MIN;
            // RRV_GATE3_OBSERVE_DIGEST=N: also print an RDRAM+scratchpad digest
            // every N effective starts, to locate the first divergent start
            // between two runs. Read-only; off by default.
            static const uint64_t digestEvery=[]{const char* v=std::getenv("RRV_GATE3_OBSERVE_DIGEST");
                return v&&*v ? std::strtoull(v,nullptr,10) : 0ull;}();
            if(digestEvery && m_gate3TemporalV1->starts()%digestEvery==0){
                uint64_t h=0x9e3779b97f4a7c15ull;
                const auto mix=[&h](const uint8_t* p,size_t n){for(size_t i=0;i<n;i+=8){uint64_t w;std::memcpy(&w,p+i,8);
                    h=(h^w)*0x100000001b3ull;h^=h>>29;}};
                mix(ram,PS2_RAM_SIZE);mix(m_memory.getScratchpad(),PS2_SCRATCHPAD_SIZE);
                std::fprintf(stderr,"[gate3-digest] start=%llu cycle=%llu ram=%016llx\n",
                    (unsigned long long)m_gate3TemporalV1->starts(),(unsigned long long)event.cycle,(unsigned long long)h);
            }
            // RRV_GATE3_DUMP_RAM=<start>:<path>: write RDRAM then scratchpad to a
            // local file at that effective start, to diff two runs. Read-only.
            static const std::pair<uint64_t,std::string> dumpAt=[]{const char* v=std::getenv("RRV_GATE3_DUMP_RAM");
                if(!v||!*v)return std::pair<uint64_t,std::string>{0,""};char* e=nullptr;const uint64_t n=std::strtoull(v,&e,10);
                return (e&&*e==':') ? std::pair<uint64_t,std::string>{n,e+1} : std::pair<uint64_t,std::string>{0,""};}();
            if(dumpAt.first && m_gate3TemporalV1->starts()==dumpAt.first){
                if(FILE* f=std::fopen(dumpAt.second.c_str(),"wb")){std::fwrite(ram,1,PS2_RAM_SIZE,f);
                    std::fwrite(m_memory.getScratchpad(),1,PS2_SCRATCHPAD_SIZE,f);std::fclose(f);}
            }
            if(phase!=lastPhase || script!=lastScript || step!=lastStep){
                std::fprintf(stderr,"[gate3-observe] start=%llu cycle=%llu phase=%u script=%d step=%d\n",
                    (unsigned long long)m_gate3TemporalV1->starts(),(unsigned long long)event.cycle,phase,script,step);
                lastPhase=phase;lastScript=script;lastStep=step;
            }
            gate3RecordPadV1(event.cycle);
            gate3HostPauseV1(kPauseVblank);
            gate3RealtimePacerV1().AtStart();
        }
        if(event.kind==rrv::guest_time::EventKind::FieldTransition){
            // Pinned GS VSync service follows the FIELD/VSINT event, not the
            // earlier guest INTC VBlank-start. This ID identifies GS service.
            gate3HostPauseV1(kPauseField);
            advanceActiveGsBackendField(++m_gate3GsServiceV1);
            gate3HostPauseV1(kPauseBackend);
            gate3PublishCsrV1();
        }
    };
    m_memory.gate3SetMmio(
        [this](uint32_t a,unsigned w,uint64_t& v){return gate3MmioReadV1(a,w,v);},
        [this](uint32_t a,unsigned w,uint64_t v){return gate3MmioWriteV1(a,w,v);});
    // Commit initial hardware only. No callback dispatch is permitted here.
    gate3HardwareV1();
}
void PS2Runtime::gate3ResolveTimedWaitsV1() {
    for(auto& wait:m_gate3TimedWaitsV1)
        if(!wait.ticket->resolved && wait.ready())gate3Scheduler.resolve(wait.ticket,0);
    rrv::guest_time::QuietBumpProducer();
    m_gate3TimedWaitsV1.erase(std::remove_if(m_gate3TimedWaitsV1.begin(),m_gate3TimedWaitsV1.end(),
        [](const auto& wait){return wait.ticket->resolved.load();}),m_gate3TimedWaitsV1.end());
}
void PS2Runtime::gate3HardwareV1() {
    m_gate3QuietV1.armed=false;rrv::guest_time::QuietBumpProducer(); // Every hardware commit outside the fast path re-validates.
    if(!m_gate3TemporalV1 || m_gate3HardwareActiveV1)return;
    m_gate3HardwareActiveV1=true;
    try {
        if(!gate3AtCutV1())gate3TemporalV1().UpdateGsRoute(m_memory.gs().smode1,m_memory.gs().syncv);
        gate3TemporalV1().HardwareTo(m_gate3ModeledEeCyclesV1);
        m_gate3ModeledEeCyclesV1=gate3TemporalV1().now();rrv::guest_time::QuietBumpProducer();
        gate3PublishCsrV1();gate3ResolveTimedWaitsV1();
        m_gate3HardwareActiveV1=false;
    }catch(...){m_gate3HardwareActiveV1=false;throw;}
    if(gate3AtCutV1()){
        if(!m_gate3CutSnapshotV1) {
            if(auto& c=gate4StartClockV1();c.enabled){c.cutNs=Gate4StartClockV1::Now();c.Write();}
            rrv::gate4::ownerTimeline().Write();
            std::fprintf(stderr,"[gate4-quiet] fast=%llu full=%llu lazy_arms=%llu miss=%llu,%llu,%llu,%llu,%llu\n",
                (unsigned long long)m_gate3QuietV1.fast,(unsigned long long)m_gate3QuietV1.full,
                (unsigned long long)m_gate3LazyV1.arms,(unsigned long long)m_gate3LazyV1.miss[0],(unsigned long long)m_gate3LazyV1.miss[1],
                (unsigned long long)m_gate3LazyV1.miss[2],(unsigned long long)m_gate3LazyV1.miss[3],(unsigned long long)m_gate3LazyV1.miss[4]);
            gate4FenceV1();
            const auto context=gate3WaitContexts.find(this);
            m_gate3CutSnapshotV1=Gate3CutSnapshotV1{
                gate3TemporalV1().now(),gate3TemporalV1().starts(),m_gate3GsServiceV1,
                m_memory.gs().csr,gate3TemporalV1().intc().ReadStat(),gate3TemporalV1().intc().ReadMask(),
                context==gate3WaitContexts.end() ? 0u : context->second->pc,
                rrv::guest_time::context_id,gate3TemporalV1().timers()};
            const auto& cut=*m_gate3CutSnapshotV1;
            std::fprintf(stderr,"[gate3-cut] cycle=%llu starts=%llu gs_services=%llu csr=0x%llx intc_stat=0x%x intc_mask=0x%x pc=0x%x context=%d\n",
                (unsigned long long)cut.cycle,(unsigned long long)cut.effective_starts,(unsigned long long)cut.gs_services,
                (unsigned long long)cut.csr,cut.intc_stat,cut.intc_mask,cut.pc,cut.context);
            auto& d=gate3DiagV1();
            if(d.trace){
                auto timers=cut.timers;
                std::fprintf(d.trace,"X cyc=%llu starts=%llu gs=%llu csr=%llx stat=%x mask=%x pc=%x ctx=%d next=%llu",
                    (unsigned long long)cut.cycle,(unsigned long long)cut.effective_starts,(unsigned long long)cut.gs_services,
                    (unsigned long long)cut.csr,cut.intc_stat,cut.intc_mask,cut.pc,cut.context,
                    (unsigned long long)gate3TemporalV1().NextDeadline());
                using R=rrv::guest_time::EeTimers::Register;
                for(unsigned t=0;t<4;++t)std::fprintf(d.trace," t%u=%x/%x/%x/%x",t,timers.read_at(cut.cycle,t,R::Count),
                    timers.read_at(cut.cycle,t,R::Mode),timers.read_at(cut.cycle,t,R::Compare),t<2?timers.read_at(cut.cycle,t,R::Hold):0u);
                std::fprintf(d.trace," tail_chk=%llu tail_pc=%016llx tail_rd=%llu tail_rdh=%016llx tail_wr=%llu\n",
                    (unsigned long long)d.checkpoints,(unsigned long long)d.pcHash,(unsigned long long)d.reads,
                    (unsigned long long)d.readHash,(unsigned long long)d.writes);
                std::fflush(d.trace);
            }
            if(d.pauseMask||d.cdMisses){
                static const char* names[kPauseSites]={"vblank","field","backend","wait","callback","syncv","io"};
                for(int i=0;i<kPauseSites;++i)if(d.pauseMask&(1u<<i))
                    std::fprintf(stderr,"[gate3-pause] site=%s hits=%llu pauses=%llu us=%llu\n",names[i],
                        (unsigned long long)d.pauseHits[i],(unsigned long long)d.pauses[i],(unsigned long long)d.pauseUs);
                if(d.cdMisses)std::fprintf(stderr,"[gate3-pause] cd-miss waits=%llu\n",(unsigned long long)d.cdMissWaits);
            }
        }
        gate3Scheduler.freeze();throw rrv::guest_time::TemporalCut();
    }
}
namespace {
// RRV_GATE3_WATCH=<address>[:<first effective start>]: report every change of
// the 8-byte guest RAM word at <address>, attributed to the guest instruction
// before this checkpoint or to the checkpoint's own service. Read-only; off by default.
struct Gate3WatchV1 { uint32_t address=0; uint64_t fromStart=0; bool enabled=false; bool primed=false;
    uint64_t value=0; uint32_t lastPc=0; int lastContext=0; };
Gate3WatchV1& gate3WatchV1() {
    static Gate3WatchV1 watch=[]{Gate3WatchV1 w; const char* v=std::getenv("RRV_GATE3_WATCH");
        if(!v||!*v)return w; char* end=nullptr; w.address=uint32_t(std::strtoul(v,&end,0))&~7u;
        if(end&&*end==':')w.fromStart=std::strtoull(end+1,nullptr,10);
        w.enabled=(w.address>=0x70000000u && w.address+8<=0x70000000u+PS2_SCRATCHPAD_SIZE) || w.address+8<=PS2_RAM_SIZE; return w;}();
    return watch;
}
void gate3WatchSampleV1(PS2Runtime* runtime,R5900Context* ctx,const char* where) {
    auto& w=gate3WatchV1();
    if(!w.enabled || runtime->gate3TemporalV1().starts()<w.fromStart)return;
    uint64_t value; std::memcpy(&value,w.address>=0x70000000u ? runtime->memory().getScratchpad()+(w.address-0x70000000u)
        : runtime->memory().getRDRAM()+w.address,8);
    if(w.primed && value!=w.value)
        std::fprintf(stderr,"[gate3-watch] addr=0x%x %016llx->%016llx at=%s start=%llu cycle=%llu prev_pc=0x%x prev_ctx=%d pc=0x%x ctx=%d ra=0x%x sp=0x%x\n",
            w.address,(unsigned long long)w.value,(unsigned long long)value,where,
            (unsigned long long)runtime->gate3TemporalV1().starts(),(unsigned long long)runtime->gate3TemporalV1().now(),
            w.lastPc,w.lastContext,ctx->pc,rrv::guest_time::context_id,
            uint32_t(_mm_cvtsi128_si32(ctx->r[31])),uint32_t(_mm_cvtsi128_si32(ctx->r[29])));
    if(w.primed && rrv::guest_time::context_id!=w.lastContext)
        std::fprintf(stderr,"[gate3-switch] at=%s start=%llu cycle=%llu from_ctx=%d from_pc=0x%x to_ctx=%d to_pc=0x%x\n",where,
            (unsigned long long)runtime->gate3TemporalV1().starts(),(unsigned long long)runtime->gate3TemporalV1().now(),
            w.lastContext,w.lastPc,rrv::guest_time::context_id,ctx->pc);
    w.value=value; w.primed=true; w.lastPc=ctx->pc; w.lastContext=rrv::guest_time::context_id;
}
}
// RRV_GATE3_TRACE_PC=<lo>:<hi>:<start>: print pc, v1, a2, t4 at every guest
// checkpoint inside [lo,hi] during that effective start. Read-only; off by default.
static void gate3TracePcV1(PS2Runtime* runtime,R5900Context* ctx) {
    struct T{uint32_t lo=0,hi=0;uint64_t start=0;bool on=false;};
    static const T t=[]{T t;const char* v=std::getenv("RRV_GATE3_TRACE_PC");if(!v||!*v)return t;char* e=nullptr;
        t.lo=uint32_t(std::strtoul(v,&e,0));if(!e||*e!=':')return t;t.hi=uint32_t(std::strtoul(e+1,&e,0));
        if(!e||*e!=':')return t;t.start=std::strtoull(e+1,nullptr,10);t.on=true;return t;}();
    // RRV_GATE3_PCHASH=<start>:<block>: rolling hash of (pc, context, v0, v1)
    // per guest checkpoint, printed every <block> checkpoints of that start.
    struct H{uint64_t start=0,block=0;bool on=false;};
    static const H h=[]{H h;const char* v=std::getenv("RRV_GATE3_PCHASH");if(!v||!*v)return h;char* e=nullptr;
        h.start=std::strtoull(v,&e,10);if(!e||*e!=':')return h;h.block=std::strtoull(e+1,nullptr,10);h.on=h.block>0;return h;}();
    if(h.on && runtime->gate3TemporalV1().starts()==h.start){
        static uint64_t acc=0x9e3779b97f4a7c15ull,n=0;
        const uint64_t w[4]={ctx->pc,uint64_t(uint32_t(rrv::guest_time::context_id)),
            uint64_t(_mm_cvtsi128_si64(ctx->r[2])),uint64_t(_mm_cvtsi128_si64(ctx->r[3]))};
        for(uint64_t x:w){acc=(acc^x)*0x100000001b3ull;acc^=acc>>29;}
        if(++n%h.block==0)std::fprintf(stderr,"[gate3-pchash] n=%llu pc=0x%x ctx=%d cycle=%llu hash=%016llx\n",
            (unsigned long long)n,ctx->pc,rrv::guest_time::context_id,(unsigned long long)runtime->gate3TemporalV1().now(),(unsigned long long)acc);
    }
    if(!t.on||ctx->pc<t.lo||ctx->pc>t.hi||runtime->gate3TemporalV1().starts()!=t.start)return;
    const auto fnv=[](const void* p,size_t n){uint64_t h=1469598103934665603ull;const auto* b=static_cast<const uint8_t*>(p);
        for(size_t i=0;i<n;++i){h^=b[i];h*=1099511628211ull;}return h;};
    std::fprintf(stderr,"[gate3-trace] pc=0x%x ctx=%d v1=0x%x a1=0x%x a2=0x%x t4=0x%x gpr=%016llx fpr=%016llx vu0=%016llx fcr=%08x cycle=%llu\n",
        ctx->pc,rrv::guest_time::context_id,
        uint32_t(_mm_cvtsi128_si32(ctx->r[3])),uint32_t(_mm_cvtsi128_si32(ctx->r[5])),uint32_t(_mm_cvtsi128_si32(ctx->r[6])),
        uint32_t(_mm_cvtsi128_si32(ctx->r[12])),(unsigned long long)fnv(ctx->r,sizeof(ctx->r)),
        (unsigned long long)fnv(ctx->f,sizeof(ctx->f)),(unsigned long long)fnv(ctx->vu0_vf,sizeof(ctx->vu0_vf)),ctx->fcr31,
        (unsigned long long)runtime->gate3TemporalV1().now());
}
// Gate-4 fast path: the previous full checkpoint serviced nothing and none
// of its inputs changed; the owner confirms no deadline is at or before the
// modeled cycle. The full path would then only move time forward.
// G4-7: always inlined into gate3CheckpointAllV1, with the semantic-trace
// bookkeeping out of line, so the common path has one call frame and no
// spills. The predicate is unchanged; cdReady is kept as a byte span so the
// check needs no division.
__attribute__((noinline)) void PS2Runtime::gate3QuietTraceV1(R5900Context* ctx,int context) {
    auto& d=gate3DiagV1();++d.checkpoints;
    gate3MixV1(d.pcHash,(uint64_t(ctx->pc)<<32)|uint32_t(uint32_t(context)));
}
__attribute__((always_inline)) inline bool PS2Runtime::gate3QuietTemporalV1(R5900Context* ctx,int context) {
    auto& q=m_gate3QuietV1;
    if(!q.armed || q.ctx!=ctx || q.context!=context || q.cop0!=ctx->cop0_status ||
       !m_gate3TimedWaitsV1.empty() || q.cdReadyBytes!=gate3CdReadyBytesV1())return false;
    const auto& gs=m_memory.gs(); auto& owner=*m_gate3TemporalV1; auto& intc=owner.intc();
    if(q.smode1!=gs.smode1 || q.syncv!=gs.syncv || q.csr!=gs.csr || q.imr!=gs.imr ||
       q.stat!=intc.ReadStat() || q.mask!=intc.ReadMask() || !owner.TryQuietTo(m_gate3ModeledEeCyclesV1))return false;
    if(__builtin_expect(q.trace,false))gate3QuietTraceV1(ctx,context);
    ++q.fast; return true;
}
void PS2Runtime::gate3CheckpointAllV1(R5900Context* ctx) {
    const int context=rrv::guest_time::context_id;
    if(!gate3QuietTemporalV1(ctx,context))gate3FullTemporalV1(ctx);
    if(!gate3Scheduler.quiet(context))gate3CheckpointSchedulerV1();
}
void PS2Runtime::gate3TemporalCheckpointV1(R5900Context* ctx) {
    if(!gate3QuietTemporalV1(ctx,rrv::guest_time::context_id))gate3FullTemporalV1(ctx);
}
void PS2Runtime::gate3FullTemporalV1(R5900Context* ctx) {
    if(auto& d=gate3DiagV1();d.trace){++d.checkpoints;
        gate3MixV1(d.pcHash,(uint64_t(ctx->pc)<<32)|uint32_t(uint32_t(rrv::guest_time::context_id)));}
    ++m_gate3QuietV1.full;
    gate3TracePcV1(this,ctx);
    gate3WatchSampleV1(this,ctx,"guest");
    gate3WaitContexts[this]=ctx;
    gate3HardwareV1();
    if(m_gate3TemporalV1) {
        gate3ServiceAdmissionsV1(ctx);
        ps2_syscalls::gate3DeliverTemporal(m_memory.getRDRAM(),ctx,this);
    }
    gate3ResolveTimedWaitsV1();
    gate3WatchSampleV1(this,ctx,"checkpoint");
    gate3ArmQuietV1(ctx);
}
uint64_t PS2Runtime::gate3QuietFastV1() const { return m_gate3QuietV1.fast; }
uint64_t PS2Runtime::gate3QuietFullV1() const { return m_gate3QuietV1.full; }
void PS2Runtime::gate3ArmQuietV1(R5900Context* ctx) {
    // Arm only when a repeat of the full path at a later, pre-deadline cycle
    // would do nothing but move time: no diagnostics that sample every
    // checkpoint, no timed wait, no ready alarm, no due CD group, no
    // deliverable or SBUS-latching INTC state, no asserted GS interrupt level.
    auto& q=m_gate3QuietV1; q.armed=false;rrv::guest_time::QuietBumpProducer();
    static const bool diagnostics=gate3WatchV1().enabled ||
        (std::getenv("RRV_GATE3_TRACE_PC")&&*std::getenv("RRV_GATE3_TRACE_PC")) ||
        (std::getenv("RRV_GATE3_PCHASH")&&*std::getenv("RRV_GATE3_PCHASH"));
    if(diagnostics || !m_gate3TemporalV1 || m_gate3HardwareActiveV1 || gate3AtCutV1() || !m_gate3TimedWaitsV1.empty())return;
    auto& owner=*m_gate3TemporalV1; auto& intc=owner.intc(); const auto& gs=m_memory.gs();
    if(owner.HasReadyAlarms())return;
    if(m_gate3NextCdCallbackV1<m_gate3CdReadyV1.size() &&
       m_gate3CdReadyV1[m_gate3NextCdCallbackV1].start<=owner.starts())return;
    if(intc.InterruptEligible(ctx->cop0_status) && (intc.Read().pending || (intc.ReadStat()&2)))return;
    if((gs.csr & ~(gs.imr>>8) & 0x1fu)!=0)return;
    if(gs.csr!=((gs.csr&~uint64_t(0x2008))|owner.csr()))return;
    q.ctx=ctx; q.context=rrv::guest_time::context_id; q.cop0=ctx->cop0_status;
    q.smode1=gs.smode1; q.syncv=gs.syncv; q.csr=gs.csr; q.imr=gs.imr;
    q.stat=intc.ReadStat(); q.mask=intc.ReadMask(); q.cdReadyBytes=gate3CdReadyBytesV1();
    q.trace=gate3DiagV1().trace!=nullptr;
    q.armed=true;
}
void PS2Runtime::gate3SetWaitContextV1(R5900Context* ctx){m_gate3QuietV1.armed=false;rrv::guest_time::QuietBump();gate3WaitContexts[this]=ctx;}
void PS2Runtime::gate3AddTimedWaitV1(rrv::guest_time::Scheduler::Ticket ticket,std::function<bool()> ready){
    m_gate3QuietV1.armed=false;rrv::guest_time::QuietBump();
    m_gate3TimedWaitsV1.push_back({std::move(ticket),std::move(ready)});rrv::guest_time::QuietBump();
}
void PS2Runtime::gate3IdleV1(){
    if(!m_gate3TemporalV1)return;
    bool stalled=false; uint64_t stallCycles=0,stallNow=0; int stallCallback=0;
    while(!gate3Scheduler.hasRunnable()){
        gate3HostPauseV1(kPauseWait);
        // A callback can park while another callback from this hardware batch
        // is already staged. Service that batch before moving the clock.
        gate3HardwareV1();
        gate3ServiceAdmissionsV1(gate3WaitContexts.at(this));
        ps2_syscalls::gate3DeliverTemporal(m_memory.getRDRAM(),gate3WaitContexts.at(this),this);
        gate3ResolveTimedWaitsV1();
        if(gate3Scheduler.hasRunnable())break;
        // Guest work above (callback tails, a same-cycle timer write) can leave
        // counted cycles or an assertion at now() uncommitted. Commit it at the
        // loop head before judging the next deadline; fail only on no progress.
        const uint64_t now=gate3TemporalV1().now();
        if(m_gate3ModeledEeCyclesV1>now || gate3TemporalV1().NextDeadline()<=now){
            if(stalled && stallCycles==m_gate3ModeledEeCyclesV1 && stallNow==now &&
               stallCallback==m_gate3NextCallbackV1)
                throw std::logic_error("Gate3 idle deadline did not advance");
            stalled=true;stallCycles=m_gate3ModeledEeCyclesV1;stallNow=now;stallCallback=m_gate3NextCallbackV1;
            continue;
        }
        stalled=false;
        if(m_gate3TimedWaitsV1.empty() && !gate3TemporalV1().HasAdmissions() && !gate3TemporalV1().HasAlarms() &&
           !ps2_syscalls::gate3HasTemporalWake(gate3WaitContexts.at(this),this))
            throw std::logic_error("Gate3 all blocked: no admitted temporal wake source");
        const auto next=gate3TemporalV1().NextDeadline();
        if(next==UINT64_MAX)throw std::logic_error("Gate3 all contexts blocked without modeled deadline");
        m_gate3ModeledEeCyclesV1=next;rrv::guest_time::QuietBump(); // next > now == counted cycles: strictly forward.
    }
}
void PS2Runtime::gate3RunCallbackV1(R5900Context* interrupted,uint32_t handler,uint32_t gp,
                                  uint32_t a0,uint32_t a1,uint32_t a2,bool irq,uint32_t deferAdmissions){
    if(!hasFunction(handler))throw std::runtime_error("Gate3 missing admitted callback target");
    m_gate3QuietV1.armed=false;rrv::guest_time::QuietBump();
    const int parent=rrv::guest_time::context_id, child=m_gate3NextCallbackV1--;
    const uint32_t stack=guestMalloc(0x4000u,16u);
    struct StackLease { PS2Runtime* owner; uint32_t base; ~StackLease(){ if(base)owner->guestFree(base); } } stackLease{this,stack};
    if(!stack)throw std::runtime_error("Gate3 callback stack allocation");
    R5900Context callback{};
    callback.cop0_status=interrupted->cop0_status|(irq ? rrv::guest_time::IntcState::kStatusExl : 0);
    callback.r[28]=_mm_cvtsi32_si128(gp);callback.r[29]=_mm_cvtsi32_si128(stack+0x4000u-16u);
    callback.r[4]=_mm_cvtsi32_si128(a0);callback.r[5]=_mm_cvtsi32_si128(a1);callback.r[6]=_mm_cvtsi32_si128(a2);
    callback.r[31]=_mm_setzero_si128();callback.pc=handler;
    const auto inherited=m_gate3DeferredAdmissionsV1.find(parent);
    const uint32_t deferred=inherited!=m_gate3DeferredAdmissionsV1.end() &&
        inherited->second.second==gate3TemporalV1().now() ? inherited->second.first : 0;
    m_gate3DeferredAdmissionsV1[child]={deferred|deferAdmissions,gate3TemporalV1().now()};
    if(auto& d=gate3DiagV1();d.trace)std::fprintf(d.trace,"C+ ctx=%d parent=%d handler=%x a0=%x a1=%x a2=%x irq=%d cyc=%llu\n",
        child,parent,handler,a0,a1,a2,int(irq),(unsigned long long)gate3TemporalV1().now());
    gate3HostPauseV1(kPauseCallback);
    gate3Scheduler.beginCallback(parent,child);rrv::guest_time::context_id=child;rrv::guest_time::QuietBump();
    auto finish=[&]{
        if(auto& d=gate3DiagV1();d.trace)std::fprintf(d.trace,"C- ctx=%d cyc=%llu\n",child,(unsigned long long)gate3TemporalV1().now());
        ps2_syscalls::gate3RetireCallback(child,this);
        gate3Scheduler.endCallback(parent,child);rrv::guest_time::context_id=parent;rrv::guest_time::QuietBump();
        m_gate3DeferredAdmissionsV1.erase(child);
        m_gate3QuietV1.armed=false;rrv::guest_time::QuietBump();
        gate3WaitContexts[this]=interrupted;
    };
    try {
        while(callback.pc && !isStopRequested()){
            gate3TemporalCheckpointV1(&callback);
            auto step=lookupFunction(callback.pc);
            if(!step)throw std::runtime_error("Gate3 callback missing continuation");
            step(m_memory.getRDRAM(),&callback,this);
        }
    }catch(...){finish();throw;}
    finish();
}
void PS2Runtime::gate3ChargeHleV1(uint64_t cycles){
    if(!m_gate3TemporalV1)throw std::logic_error("Gate3 HLE cost profile unbound");
    if(cycles>UINT64_MAX-m_gate3ModeledEeCyclesV1)throw std::overflow_error("Gate3 HLE cycle overflow");
    const uint64_t target=m_gate3ModeledEeCyclesV1+cycles;
    // Visit each earlier deadline. HardwareTo never skips it, even for opaque work.
    m_gate3ModeledEeCyclesV1=target;rrv::guest_time::QuietBumpProducer();gate3HardwareV1();
}
// ---- Work inside the armed lazy window that used to end it (Gate-5 speed work, 2026-10-02) ----
//
// gate3HardwareV1 commits the hardware model to the current cycle and ends the lazy window: the next
// checkpoint is then a full one (hardware, admissions, deliveries, timed waits) and re-arms. Two
// kinds of guest work did that thousands of times a field although, inside the window, the commit
// and the full checkpoint behind it provably do nothing but move the owner clock:
//
//   * a bus access to a plain address (gate3PlainMmioQuietV1): the VU0 code and data windows
//     (0x11000000-0x11007FFF) and the registers of DMA channels 8 (fromSPR) and 9 (toSPR). That is
//     memory, and a synchronous copy between RAM and the scratchpad whose handler
//     (PS2Memory::writeIORegister) reads no guest time; the temporal model neither reads nor
//     writes these addresses. RR5's car shadow builder (func_223F18) reads its shadow volume out of
//     VU0 data memory one word at a time, and both car builders drive the two scratchpad channels;
//   * a named HLE charge that ends before the window does (gate3LazyChargeV1): the libvu0 matrix
//     helpers, called about forty times per car per field.
//
// Host `sample`, 8-car race, runtime v153: gate3HardwareV1 was 20.7% of the game thread and the
// full checkpoints 18.8% (overlapping).
//
// gate3LazyQuietV1(cycle) is the test: gate3CheckpointV1's own, for the context the window was
// armed for, at the cycle the left-out full checkpoint would run at. By the window's predicate
// (gate3LazyPredicateV1) a full checkpoint there would only move the owner clock, which the next
// instruction's checkpoint does anyway. Anything else (no window, the deadline reached, the
// generation moved, RRV_GATE4_LAZY_VERIFY) takes the full path, as before.
//
// One thing the window's predicate does not cover: the sound driver. It is serviced at full
// checkpoints only (gate3ServiceAdmissionsV1) and acts at the first one about a guest millisecond
// after its last turn, so once a millisecond the full checkpoint behind a commit is the one that
// hands sound replies and callbacks to the guest. That one is never left out: the test also needs
// the driver's answer that a service call at `cycle` would do nothing (Gate8AudioHooksV1::idle).
//
// RRV_GATE4_LAZY_MMIO=0 and RRV_GATE4_LAZY_HLE=0 turn the two off for A/B runs.
static uint64_t g_gate3LazyWorkV1[4]; // plain MMIO skipped / committed, HLE charges lazy / committed (producer domain)
uint64_t PS2Runtime::gate3LazyWorkCountV1(unsigned index) { return g_gate3LazyWorkV1[index & 3u]; }
bool PS2Runtime::gate3LazyQuietV1(uint64_t cycle) const {
    const auto& lazy=m_gate3LazyV1; const auto& q=m_gate3QuietV1;
    return lazy.ctx && !lazy.verify && cycle<lazy.until &&
        rrv::guest_time::quiet_generation.load(std::memory_order_relaxed)==lazy.generation &&
        q.armed && q.ctx==lazy.ctx && q.context==rrv::guest_time::context_id && lazy.ctx->cop0_status==lazy.cop0 &&
        (!g_gate8AudioHooksV1.idle || g_gate8AudioHooksV1.idle(const_cast<PS2Runtime*>(this),cycle));
}
bool PS2Runtime::gate3PlainMmioQuietV1(uint32_t physical) const {
    static const bool enabled=[]{const char* v=std::getenv("RRV_GATE4_LAZY_MMIO");return !(v&&v[0]=='0');}();
    if(!enabled || !((physical-0x11000000u)<0x8000u || (physical-0x1000D000u)<0x100u || (physical-0x1000D400u)<0x100u))
        return false;
    // The full checkpoint this leaves out is the next instruction's, at the current cycle count.
    const bool quiet=gate3LazyQuietV1(m_gate3ModeledEeCyclesV1);
    ++g_gate3LazyWorkV1[quiet ? 0 : 1];
    return quiet;
}
// A named HLE charge of `cycles` for `ctx`: true = charged, the window still armed. The full path
// (gate3ChargeNamedHleV1) checkpoints, commits the charge and then, for a patch or library HLE,
// runs a full checkpoint at the new cycle (for a syscall the next instruction's is the full one).
// Inside the window the first is the quiet path and the others move the owner clock.
bool PS2Runtime::gate3LazyChargeV1(R5900Context* ctx,uint64_t cycles) {
    static const bool enabled=[]{const char* v=std::getenv("RRV_GATE4_LAZY_HLE");return !(v&&v[0]=='0');}();
    const uint64_t after=m_gate3ModeledEeCyclesV1+cycles;
    const bool quiet=enabled && ctx==m_gate3LazyV1.ctx && after>=m_gate3ModeledEeCyclesV1 && gate3LazyQuietV1(after);
    ++g_gate3LazyWorkV1[quiet ? 2 : 3];
    if(!quiet)return false;
    m_gate3ModeledEeCyclesV1=after;
    *m_gate3LazyV1.now=after; // where the commit would have left the owner clock
    return true;
}
bool PS2Runtime::gate3MmioReadV1(uint32_t address,unsigned width,uint64_t& value){
    if((address>>28)==7u)return false; // scratchpad, never I/O (no KSEG masking)
    if(gate3PlainMmioQuietV1(address&0x1fffffffU))return false;
    gate3HardwareV1();
    const auto physical=address&0x1fffffffU;
    if(physical>=0x12001080u && physical<0x12001088u){rrv::gate4::ownerBarrier(rrv::gate4::BarrierKind::SiglblidRead);gate4FenceV1();}
    if(physical>=0x12001000u && physical<0x12001008u){
        rrv::gate4::ownerBarrier(rrv::gate4::BarrierKind::CsrRead);
        gate4FenceV1();
        gate3PublishCsrV1();value=m_memory.gs().csr>>((physical-0x12001000u)*8);
        if(width<8)value&=(uint64_t(1)<<(width*8))-1;
        if(auto& d=gate3DiagV1();d.trace){++d.reads;gate3MixV1(d.readHash,(uint64_t(physical)<<8)|width);
            gate3MixV1(d.readHash,value);gate3MixV1(d.readHash,gate3TemporalV1().now());}
        return true;
    }
    const bool handled=gate3TemporalV1().Read(address,width,value);
    if(auto& d=gate3DiagV1();handled && d.trace){++d.reads;gate3MixV1(d.readHash,(uint64_t(physical)<<8)|width);
        gate3MixV1(d.readHash,value);gate3MixV1(d.readHash,gate3TemporalV1().now());}
    return handled;
}
bool PS2Runtime::gate3MmioWriteV1(uint32_t address,unsigned width,uint64_t value){
    if((address>>28)==7u)return false; // scratchpad, never I/O (no KSEG masking)
    if(gate3PlainMmioQuietV1(address&0x1fffffffU))return false;
    gate3HardwareV1();
    const auto physical=address&0x1fffffffU;
    const auto logWrite=[&]{auto& d=gate3DiagV1();if(!d.trace)return;++d.writes;
        const auto found=gate3WaitContexts.find(this);
        std::fprintf(d.trace,"W a=%x w=%u v=%llx cyc=%llu ctx=%d pc=%x\n",physical,width,(unsigned long long)value,
            (unsigned long long)gate3TemporalV1().now(),rrv::guest_time::context_id,found==gate3WaitContexts.end()?0u:found->second->pc);};
    if(physical>=0x12001080u && physical<0x12001088u)gate4GuestGsWriteV1(physical);
    if(physical>=0x12001000u && physical<0x12001008u){
        gate4GuestGsWriteV1(physical);
        logWrite();
        const uint64_t bits=value<<((physical-0x12001000u)*8);
        if(bits&(1u<<9)){
            gate3TemporalV1().ActionAt(rrv::guest_time::Action::CsrReset);
            auto& gs=m_memory.gs();gs.smode1=gs.smode2=gs.syncv=0;gs.imr=0x7f00;
        }
        if(bits&8)gate3TemporalV1().AcknowledgeVsint();
        // Existing bus/backend still owns SIGNAL/FINISH/RESET forwarding.
        return false;
    }
    if(physical==0x12000010u){
        // SINT is a direct control bit; timing route programming is a transaction.
        gate3TemporalV1().SetSint((value & (1ULL<<17))!=0);
    }
    const bool handled=gate3TemporalV1().Write(address,width,value);
    if(handled)logWrite();
    return handled;
}

bool PS2Runtime::gate3AdmissionSuppressedV1(uint32_t kind) {
    const auto found=m_gate3DeferredAdmissionsV1.find(rrv::guest_time::context_id);
    // Finish a runnable callback's same-cycle batch in admission order. Once
    // it parks, a later callback may be its wake source, even at this cycle.
    // Later deadlines are new batches and are never held behind this callback.
    return found!=m_gate3DeferredAdmissionsV1.end() && (found->second.first&kind)!=0 &&
        found->second.second==gate3TemporalV1().now() &&
        !(gate3Scheduler.status(rrv::guest_time::context_id)&4);
}

// Gate-4 lazy checkpoint (scripts/gate4_lazy_checkpoint_overlay.py).
// The quiet predicate of gate3QuietTemporalV1 plus the scheduler's quiet mark,
// without side effects: true iff gate3CheckpointAllV1 at `cycles` would only
// move the owner clock.
bool PS2Runtime::gate3LazyPredicateV1(R5900Context* ctx,int context,uint64_t cycles) {
    const auto& q=m_gate3QuietV1;
    if(!q.armed || q.trace || q.ctx!=ctx || q.context!=context || q.cop0!=ctx->cop0_status ||
       !m_gate3TimedWaitsV1.empty() || q.cdReadyBytes!=gate3CdReadyBytesV1())return false;
    const auto& gs=m_memory.gs(); auto& owner=*m_gate3TemporalV1; auto& intc=owner.intc();
    if(q.smode1!=gs.smode1 || q.syncv!=gs.syncv || q.csr!=gs.csr || q.imr!=gs.imr ||
       q.stat!=intc.ReadStat() || q.mask!=intc.ReadMask())return false;
    if(cycles<owner.now() || cycles>=owner.quiet_until())return false;
    return gate3Scheduler.quiet(context);
}
void PS2Runtime::gate3ArmLazyV1(R5900Context* ctx) {
    static const bool enabled=[]{const char* v=std::getenv("RRV_GATE4_LAZY_CHECKPOINT");return !(v&&v[0]=='0');}();
    static const bool verify=[]{const char* v=std::getenv("RRV_GATE4_LAZY_VERIFY");return v&&v[0]&&v[0]!='0';}();
    static const bool stats=[]{const char* v=std::getenv("RRV_GATE4_LAZY_STATS");return v&&v[0]&&v[0]!='0';}();
    auto& l=m_gate3LazyV1; l.ctx=nullptr; l.stats=stats;
    if(!enabled || !m_gate3TemporalV1 || m_gate3CheckpointV1)return;
    // Generation first: any change after this read invalidates the window.
    const uint64_t generation=rrv::guest_time::quiet_generation.load(std::memory_order_relaxed);
    if(!gate3LazyPredicateV1(ctx,rrv::guest_time::context_id,m_gate3ModeledEeCyclesV1))return;
    auto& owner=*m_gate3TemporalV1;
    l.until=owner.quiet_until(); l.now=owner.lazy_now(); l.cop0=ctx->cop0_status;
    l.generation=generation; l.verify=verify; ++l.arms; l.ctx=ctx;
}
void PS2Runtime::gate3LazyStatV1(R5900Context* ctx) {
    auto& l=m_gate3LazyV1; const uint64_t c=m_gate3ModeledEeCyclesV1;
    const int why=!l.ctx ? 0 : ctx!=l.ctx ? 1 : c>=l.until ? 2 : ctx->cop0_status!=l.cop0 ? 3 : 4;
    ++l.miss[why];
}
void PS2Runtime::gate3LazyVerifyV1(R5900Context* ctx,uint64_t cycles) {
    if(gate3LazyPredicateV1(ctx,rrv::guest_time::context_id,cycles))return;
    std::fprintf(stderr,"[gate4-lazy] VERIFY FAILED pc=0x%x context=%d cycles=%llu now=%llu until=%llu\n",
        ctx->pc,rrv::guest_time::context_id,(unsigned long long)cycles,
        (unsigned long long)gate3TemporalV1().now(),(unsigned long long)gate3TemporalV1().quiet_until());
    std::abort();
}
