// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-FileCopyrightText: 2026 RRV-Recomp contributors
// SPDX-License-Identifier: GPL-3.0+
//
// rrv_pcsx2_spu2 glue: replaces PCSX2's pcsx2/SPU2/spu2.cpp (2.8.2,
// fd9d310ccbb6b8b62c976da8886a3c8fd3a10ff3) and the IOP-side hooks the SPU2
// core calls (IopDma.cpp spu2DMA4Irq/spu2DMA7Irq, IopIrq.cpp spu2Irq).
//
// Adapted from upstream spu2.cpp (see PROVENANCE.md): StereoOut32::Empty,
// lClocks, SPU2read(), SPU2write(), SPU2{read,write}DMA{4,7}Mem(),
// SPU2interruptDMA{4,7}(), SPU2::InternalReset(), DCFilter() and spu2Output().
// The AudioStream backend is replaced by a lock-free SPSC ring; the IOP
// CPU/DMA controller is replaced by timestamped commands.

#include "rrv_pcsx2_spu2.h"

#include "SPU2/defs.h"
#include "SPU2/regs.h"
#include "SPU2/spu2.h"
#include "IopDma.h"
#include "IopHw.h"
#include "IopMem.h"
#include "IopCounters.h"
#include "R3000A.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <time.h>
#endif

// ---------------------------------------------------------------------------
// Globals the PCSX2 SPU2 core expects from the rest of the emulator.
// ---------------------------------------------------------------------------

const StereoOut32 StereoOut32::Empty(0, 0); // upstream spu2.cpp
u64 lClocks = 0;                            // upstream spu2.cpp

psxRegisters psxRegs;
s32 psxNextDeltaCounter;
u64 psxNextStartCounter;
psxCounter psxCounters[NUM_COUNTERS];
u8 rrv_spu2_iopHw[0x10000];
u8 rrv_spu2_iopRam[RRV_SPU2_IOP_RAM_SIZE];

namespace
{
constexpr u32 kTickInterval = 768; // IOP cycles per 48 kHz frame (spu2sys.cpp TickInterval)
constexpr u64 kMaxStep = u64{kTickInterval} * 1024; // stays far below TimeUpdate's sanity clamp (768*4800)
constexpr u32 kChcrBusy = 0x01000000;
constexpr u32 kChcrToSpu = 0x01000201;   // psxDmaGeneric: IOP -> SPU2
constexpr u32 kChcrFromSpu = 0x01000200; // psxDmaGeneric: SPU2 -> IOP
constexpr u32 kStagingBytesPerCore = RRV_SPU2_IOP_RAM_SIZE / 2;
constexpr u32 kMaxDmaHalfwords = kStagingBytesPerCore / 2;

struct rrv_core_state;
rrv_core_state* g_state = nullptr; // core-thread view of the live instance
std::atomic<bool> g_instance_live{false};

// Core-thread-only state (touched by whichever thread runs the core).
struct rrv_core_state
{
	std::vector<rrv_spu2_event> events;
	bool in_mix = false;
	bool suppress_sinks = false;
	u32 irq_bits_seen = 0;
	float dc_in[2] = {0, 0};
	float dc_out[2] = {0, 0};
	u64 out_hash = 0xcbf29ce484222325ull;
};

void push_event(u64 cycle, u32 kind, u32 data)
{
	if (!g_state || g_state->suppress_sinks)
		return;
	g_state->events.push_back(rrv_spu2_event{cycle, kind, data});
}

void rrv_mix()
{
	g_state->in_mix = true;
	isa_native::spu2Mix();
	g_state->in_mix = false;
}
} // namespace

// ---------------------------------------------------------------------------
// IOP interrupt sinks (upstream: IopIrq.cpp spu2Irq, IopDma.cpp spu2DMA4Irq/7).
// ---------------------------------------------------------------------------

// Upstream spu2.cpp SPU2interruptDMA4/7 (unchanged logic).
void SPU2interruptDMA4()
{
	if (Cores[0].DmaMode)
		Cores[0].Regs.STATX |= 0x80;
	Cores[0].Regs.STATX &= ~0x400;
	Cores[0].TSA = Cores[0].ActiveTSA;
}

void SPU2interruptDMA7()
{
	if (Cores[1].DmaMode)
		Cores[1].Regs.STATX |= 0x80;
	Cores[1].Regs.STATX &= ~0x400;
	Cores[1].TSA = Cores[1].ActiveTSA;
}

void spu2Irq()
{
	// SetIrqCall()/CheckDMAProgress() set Spdif.Info bit (4 << core) right
	// before calling us; report the bits that were not already reported.
	const u32 bits = (Spdif.Info >> 2) & 3;
	u32 fresh = bits & ~(g_state ? g_state->irq_bits_seen : 0);
	if (!fresh)
		fresh = bits;
	if (g_state)
		g_state->irq_bits_seen = bits;
	// Inside a mix tick the IRQ belongs to that tick's end cycle (lClocks);
	// otherwise to the current core time.
	const u64 cyc = (g_state && g_state->in_mix) ? lClocks : psxRegs.cycle;
	push_event(cyc, RRV_SPU2_EVENT_SPU_IRQ, fresh);
}

void spu2DMA4Irq()
{
	SPU2interruptDMA4();
	if (HW_DMA4_CHCR & kChcrBusy)
	{
		HW_DMA4_CHCR &= ~kChcrBusy;
		push_event(psxRegs.cycle, RRV_SPU2_EVENT_DMA4_DONE, 0);
	}
}

void spu2DMA7Irq()
{
	SPU2interruptDMA7();
	if (HW_DMA7_CHCR & kChcrBusy)
	{
		HW_DMA7_CHCR &= ~kChcrBusy;
		push_event(psxRegs.cycle, RRV_SPU2_EVENT_DMA7_DONE, 0);
	}
}

// ---------------------------------------------------------------------------
// Upstream spu2.cpp entry points (adapted: no logging, no AudioStream).
// ---------------------------------------------------------------------------

void SPU2readDMA4Mem(u16* pMem, u32 size)
{
	TimeUpdate(psxRegs.cycle);
	Cores[0].DoDMAread(pMem, size);
}

void SPU2writeDMA4Mem(u16* pMem, u32 size)
{
	TimeUpdate(psxRegs.cycle);
	Cores[0].DoDMAwrite(pMem, size);
}

void SPU2readDMA7Mem(u16* pMem, u32 size)
{
	TimeUpdate(psxRegs.cycle);
	Cores[1].DoDMAread(pMem, size);
}

void SPU2writeDMA7Mem(u16* pMem, u32 size)
{
	TimeUpdate(psxRegs.cycle);
	Cores[1].DoDMAwrite(pMem, size);
}

// Upstream spu2.cpp: true only after SPU2::Reset(true) (PS1 mode), which RRV never does.
bool SPU2::IsRunningPSXMode()
{
	return false;
}

void SPU2async()
{
	TimeUpdate(psxRegs.cycle);
}

u16 SPU2read(u32 rmem)
{
	u16 ret = 0xDEAD;
	u32 core = 0;
	const u32 mem = rmem & 0xFFFF;
	u32 omem = mem;

	if (mem & 0x400)
	{
		omem ^= 0x400;
		core = 1;
	}

	// Upstream compares the 16-bit offset with a full address, so this branch
	// never fires in PCSX2 2.8.2 either; kept verbatim for equivalence.
	if (omem == 0x1f9001AC)
	{
		Cores[core].ActiveTSA = Cores[core].TSA;
		for (int i = 0; i < 2; i++)
		{
			if (Cores[i].IRQEnable && (Cores[i].IRQA == Cores[core].ActiveTSA))
				SetIrqCall(i);
		}
		ret = Cores[core].DmaRead();
	}
	else
	{
		TimeUpdate(psxRegs.cycle);

		if (rmem >> 16 == 0x1f80)
			ret = Cores[0].ReadRegPS1(rmem);
		else if (mem >= 0x800)
			ret = spu2Ru16(mem);
		else
			ret = *(regtable[(mem >> 1)]);
	}

	return ret;
}

void SPU2write(u32 rmem, u16 value)
{
	TimeUpdate(psxRegs.cycle);

	if (rmem >> 16 == 0x1f80)
		Cores[0].WriteRegPS1(rmem, value);
	else
		SPU2_FastWrite(rmem, value);
}

// Upstream spu2.cpp DCFilter() + spu2Output(), writing to our ring instead of
// AudioStream::WriteChunk. Declared in SPU2/defs.h; called once per frame.
namespace rrv_out
{
void push_frame(float l, float r, s16 raw_l, s16 raw_r);
}

void spu2Output(StereoOut32 out)
{
	rrv_core_state& st = *g_state;
	const s32 cl = clamp_mix(out.Left);
	const s32 cr = clamp_mix(out.Right);

	float conv[2];
	conv[0] = static_cast<float>(cl) / INT16_MAX;
	conv[1] = static_cast<float>(cr) / INT16_MAX;

	// DC blocking high-pass filter (upstream DCFilter()).
	float o0 = (conv[0] - st.dc_in[0] + ((0.995f * st.dc_out[0])));
	float o1 = (conv[1] - st.dc_in[1] + ((0.995f * st.dc_out[1])));
	st.dc_in[0] = conv[0];
	st.dc_in[1] = conv[1];
	st.dc_out[0] = o0;
	st.dc_out[1] = o1;

	rrv_out::push_frame(o0, o1, static_cast<s16>(cl), static_cast<s16>(cr));
}

// ---------------------------------------------------------------------------
// Instance
// ---------------------------------------------------------------------------

namespace
{
enum class Op : u8
{
	Write16,
	Read16,
	DmaWrite,
	DmaRead,
	Advance,
	Sync,
	Hash,
};

struct Barrier
{
	bool done = false;
	u16 r16 = 0;
	u64 r64 = 0;
	u16* out = nullptr;
};

struct Cmd
{
	Op op;
	int core;
	u64 t;
	u32 a;
	u32 b;
	std::vector<u16> payload;
	Barrier* barrier;
};

u64 hash_bytes(u64 h, const void* p, size_t n)
{
	const u8* b = static_cast<const u8*>(p);
	size_t i = 0;
	for (; i + 8 <= n; i += 8)
	{
		u64 w;
		std::memcpy(&w, b + i, 8);
		h ^= w;
		h *= 0x100000001b3ull;
		h ^= h >> 29;
	}
	for (; i < n; i++)
	{
		h ^= b[i];
		h *= 0x100000001b3ull;
	}
	return h;
}

template <typename T>
u64 hv(u64 h, const T& v)
{
	return hash_bytes(h, &v, sizeof(v));
}

u64 hash_volume(u64 h, const V_VolumeSlide& v)
{
	h = hv(h, v.Reg_VOL);
	h = hv(h, v.Counter);
	return hv(h, v.Value);
}

u64 guest_state_hash()
{
	u64 h = 0xcbf29ce484222325ull;
	h = hash_bytes(h, spu2regs, sizeof(spu2regs));
	h = hash_bytes(h, _spu2mem, sizeof(_spu2mem));
	h = hv(h, psxRegs.cycle);
	h = hv(h, lClocks);
	h = hv(h, Cycles);
	h = hv(h, OutPos);
	h = hv(h, InputPos);
	h = hv(h, PlayMode);
	h = hash_bytes(h, &Spdif, sizeof(Spdif));
	for (int c = 0; c < 2; c++)
	{
		const V_Core& k = Cores[c];
		h = hash_bytes(h, k.VoiceGates, sizeof(k.VoiceGates));
		h = hash_bytes(h, &k.DryGate, sizeof(k.DryGate));
		h = hash_bytes(h, &k.WetGate, sizeof(k.WetGate));
		h = hash_volume(h, k.MasterVol.Left);
		h = hash_volume(h, k.MasterVol.Right);
		h = hv(h, k.ExtVol.Left); h = hv(h, k.ExtVol.Right);
		h = hv(h, k.InpVol.Left); h = hv(h, k.InpVol.Right);
		h = hv(h, k.FxVol.Left); h = hv(h, k.FxVol.Right);
		for (const V_Voice& v : k.Voices)
		{
			h = hash_volume(h, v.Volume.Left);
			h = hash_volume(h, v.Volume.Right);
			h = hv(h, v.ADSR.reg32);
			h = hv(h, v.ADSR.Counter);
			h = hv(h, v.ADSR.Value);
			h = hv(h, v.ADSR.Phase);
			h = hv(h, v.Pitch);
			h = hv(h, v.LoopStartA);
			h = hv(h, v.StartA);
			h = hv(h, v.NextA);
			h = hv(h, v.Prev1);
			h = hv(h, v.Prev2);
			h = hv(h, v.Modulated);
			h = hv(h, v.Noise);
			h = hv(h, v.LoopMode);
			h = hv(h, v.LoopFlags);
			h = hv(h, v.SP);
			h = hv(h, v.OutX);
			h = hash_bytes(h, v.DecodeFifo, sizeof(v.DecodeFifo));
			h = hv(h, v.DecPosWrite);
			h = hv(h, v.DecPosRead);
		}
		h = hv(h, k.IRQA); h = hv(h, k.TSA); h = hv(h, k.ActiveTSA);
		h = hv(h, k.IRQEnable); h = hv(h, k.FxEnable); h = hv(h, k.Mute); h = hv(h, k.AdmaInProgress);
		h = hv(h, k.DMABits); h = hv(h, k.NoiseClk); h = hv(h, k.NoiseCnt); h = hv(h, k.NoiseOut);
		h = hv(h, k.AutoDMACtrl); h = hv(h, k.DMAICounter); h = hv(h, k.LastClock);
		h = hv(h, k.InputDataLeft); h = hv(h, k.InputDataTransferred);
		h = hv(h, k.InputPosWrite); h = hv(h, k.InputDataProgress);
		h = hash_bytes(h, &k.Revb, sizeof(k.Revb));
		h = hash_bytes(h, k.RevbDownBuf, sizeof(k.RevbDownBuf));
		h = hash_bytes(h, k.RevbUpBuf, sizeof(k.RevbUpBuf));
		h = hv(h, k.RevbSampleBufPos); h = hv(h, k.EffectsStartA); h = hv(h, k.EffectsEndA);
		h = hash_bytes(h, &k.Regs, sizeof(k.Regs));
		h = hv(h, k.LastEffect.Left); h = hv(h, k.LastEffect.Right);
		h = hv(h, k.CoreEnabled); h = hv(h, k.AttrBit0); h = hv(h, k.DmaMode);
		h = hv(h, k.ReadSize); h = hv(h, k.IsDMARead);
		h = hv(h, k.KeyOn); h = hv(h, k.KeyOff);
	}
	h = hv(h, HW_DMA4_CHCR);
	h = hv(h, HW_DMA7_CHCR);
	return h;
}

u32 next_pow2(u32 v)
{
	u32 p = 1;
	while (p < v && p < 0x80000000u)
		p <<= 1;
	return p;
}

#if !defined(_WIN32)
u64 thread_cpu_ns()
{
	timespec ts;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
	return u64(ts.tv_sec) * 1000000000ull + u64(ts.tv_nsec);
}
#else
u64 thread_cpu_ns() { return 0; }
#endif
} // namespace

struct rrv_spu2
{
	rrv_spu2_config cfg{};

	// ---- caller side
	u64 last_t = 0;

	// ---- core side (worker thread in threaded mode, caller in inline mode)
	rrv_core_state core;

	// ---- queue (threaded mode)
	std::mutex m;
	std::condition_variable cv_work;
	std::condition_variable cv_done;
	std::deque<Cmd> q;
	bool stop = false;
	bool worker_sleeping = false;
	std::vector<rrv_spu2_event> ready_events; // guarded by m in threaded mode
	std::thread worker;

	// ---- output ring (SPSC: core produces, audio thread consumes)
	std::vector<float> ring;
	u64 ring_mask = 0;
	alignas(64) std::atomic<u64> ring_head{0}; // frames written
	alignas(64) std::atomic<u64> ring_tail{0}; // frames read

	// ---- stats
	std::atomic<u64> frames_mixed{0};
	std::atomic<u64> underruns{0};
	std::atomic<u64> overruns{0};
	std::atomic<u64> clamp_violations{0};
	std::atomic<u64> queue_high_water{0};
	std::atomic<u64> commands{0};
	std::atomic<u64> dma_overlaps{0};
	std::atomic<u64> dma_truncated{0};
	std::atomic<u64> output_hash{0};
	std::atomic<u64> worker_cpu_ns{0};
};

namespace
{
rrv_spu2* g_inst = nullptr;
}

void rrv_out::push_frame(float l, float r, s16 raw_l, s16 raw_r)
{
	rrv_spu2* s = g_inst;
	rrv_core_state& st = s->core;
	u32 raw = (u32(u16(raw_l)) << 16) | u16(raw_r);
	st.out_hash = (st.out_hash ^ raw) * 0x100000001b3ull;
	s->output_hash.store(st.out_hash, std::memory_order_relaxed);
	s->frames_mixed.fetch_add(1, std::memory_order_relaxed);

	const u64 head = s->ring_head.load(std::memory_order_relaxed);
	const u64 tail = s->ring_tail.load(std::memory_order_acquire);
	if (head - tail > s->ring_mask) // full: drop, never block the core
	{
		s->overruns.fetch_add(1, std::memory_order_relaxed);
		return;
	}
	const u64 i = (head & s->ring_mask) * 2;
	s->ring[i] = l;
	s->ring[i + 1] = r;
	s->ring_head.store(head + 1, std::memory_order_release);
}

namespace
{
// Adapted from upstream spu2.cpp SPU2::InternalReset(false) + SPU2::Open(),
// plus a full power-on clear so a second instance in the same process starts
// from exactly the state the first one did.
void core_power_on(rrv_core_state& st)
{
	// spu2sys.cpp keeps `static bool has_to_call_irq_dma[2]`, which no reset
	// clears. It is cleared when a DMA countdown expires, so run one 1-cycle
	// countdown on core 0 with IRQs disabled and sinks suppressed.
	st.suppress_sinks = true;
	Cores[0].IRQEnable = false;
	Cores[1].IRQEnable = false;
	Cores[0].AutoDMACtrl = 0;
	Cores[0].ReadSize = 0;
	Cores[0].DMAICounter = 1;
	Cores[0].LastClock = 0;
	lClocks = 0;
	psxRegs.cycle = 1;
	TimeUpdate(psxRegs.cycle);
	st.suppress_sinks = false;

	std::memset(static_cast<void*>(Cores), 0, sizeof(Cores));
	std::memset(static_cast<void*>(DebugCores), 0, sizeof(DebugCores));
	std::memset(&Spdif, 0, sizeof(Spdif));
	std::memset(static_cast<void*>(pcm_cache_data), 0, sizeof(PcmCacheEntry) * pcm_BlockCount);
	std::memset(rrv_spu2_iopHw, 0, sizeof(rrv_spu2_iopHw));
	std::memset(rrv_spu2_iopRam, 0, sizeof(rrv_spu2_iopRam));
	std::memset(psxCounters, 0, sizeof(psxCounters));
	psxNextDeltaCounter = 0;
	psxNextStartCounter = 0;
	OutPos = 0;
	InputPos = 0;
	Cycles = 0;
	PlayMode = 0;
	psxRegs.cycle = 0;
	lClocks = psxRegs.cycle; // SPU2::Open()

	// SPU2::InternalReset(false)
	spu2Mix = rrv_mix; // wraps isa_native::spu2Mix to tag IRQs raised inside a tick
	ReverbDownsample = MULTI_ISA_SELECT(ReverbDownsample);
	ReverbUpsample = MULTI_ISA_SELECT(ReverbUpsample);
	std::memset(spu2regs, 0, 0x010000);
	std::memset(_spu2mem, 0, 0x200000);
	std::memset(_spu2mem + 0x2800, 7, 0x10); // from BIOS reversal. Locks the voices so they don't run free.
	std::memset(_spu2mem + 0xe870, 7, 0x10); // Loop which gets left over by the BIOS, Megaman X7 relies on it being there.
	st.dc_in[0] = st.dc_in[1] = 0;
	st.dc_out[0] = st.dc_out[1] = 0;
	Spdif.Info = 0;
	Cores[0].Init(0);
	Cores[1].Init(1);

	st.events.clear();
	st.irq_bits_seen = 0;
	st.out_hash = 0xcbf29ce484222325ull;
}

// Run the core to guest cycle t. Steps stop at each pending DMA countdown
// deadline, which is what PCSX2's IOP counter 6 (CounterUpdate) schedules, so
// DMA completion events land on their exact guest cycle regardless of how the
// caller batches its calls.
void run_to(u64 t)
{
	while (psxRegs.cycle < t)
	{
		u64 next = std::min<u64>(t, psxRegs.cycle + kMaxStep);
		for (int c = 0; c < 2; c++)
		{
			if (Cores[c].DMAICounter > 0)
			{
				const u64 dl = Cores[c].LastClock + static_cast<u64>(Cores[c].DMAICounter);
				if (dl > psxRegs.cycle && dl < next)
					next = dl;
			}
		}
		psxRegs.cycle = next;
		TimeUpdate(psxRegs.cycle);
	}
}

void write_tsa(int core, u32 addr)
{
	const u32 base = 0x1F900000u + (core ? 0x400u : 0u);
	SPU2write(base + REG_A_TSA, static_cast<u16>((addr >> 16) & 0xF));
	SPU2write(base + REG_A_TSA + 2, static_cast<u16>(addr & 0xFFFF));
}

u32 norm_reg(u32 a)
{
	return (a < 0x10000u) ? (a | 0x1F900000u) : a;
}

void execute(rrv_spu2* s, Cmd& c)
{
	s->commands.fetch_add(1, std::memory_order_relaxed);
	run_to(c.t);
	switch (c.op)
	{
		case Op::Write16:
			SPU2write(c.a, static_cast<u16>(c.b));
			break;
		case Op::Read16:
			c.barrier->r16 = SPU2read(c.a);
			break;
		case Op::DmaWrite:
		case Op::DmaRead:
		{
			const int core = c.core ? 1 : 0;
			u32& chcr = core ? HW_DMA7_CHCR : HW_DMA4_CHCR;
			if (chcr & kChcrBusy)
				s->dma_overlaps.fetch_add(1, std::memory_order_relaxed);
			if (c.a != RRV_SPU2_KEEP_TSA)
				write_tsa(core, c.a);
			const u32 staging = core ? kStagingBytesPerCore : 0;
			u16* mem = reinterpret_cast<u16*>(iopPhysMem(staging));
			(core ? HW_DMA7_MADR : HW_DMA4_MADR) = staging;
			(core ? HW_DMA7_BCR : HW_DMA4_BCR) = (c.b + 1) / 2 | (1u << 16);
			if (c.op == Op::DmaWrite)
			{
				std::memcpy(mem, c.payload.data(), c.payload.size() * 2);
				chcr = kChcrToSpu;
				if (core)
					SPU2writeDMA7Mem(mem, c.b);
				else
					SPU2writeDMA4Mem(mem, c.b);
			}
			else
			{
				u32 a = Cores[core].TSA & 0xFFFFF;
				for (u32 i = 0; i < c.b; i++)
					c.barrier->out[i] = static_cast<u16>(_spu2mem[(a + i) & 0xFFFFF]);
				chcr = kChcrFromSpu;
				if (core)
					SPU2readDMA7Mem(mem, c.b);
				else
					SPU2readDMA4Mem(mem, c.b);
			}
			break;
		}
		case Op::Advance:
		case Op::Sync:
			break;
		case Op::Hash:
			c.barrier->r64 = guest_state_hash();
			break;
	}
	// Refresh the "already reported" IRQ bits after any guest-visible change
	// (a register write may have cleared them).
	s->core.irq_bits_seen = (Spdif.Info >> 2) & 3;
}

void worker_main(rrv_spu2* s)
{
	std::deque<Cmd> batch;
	std::unique_lock<std::mutex> lk(s->m);
	for (;;)
	{
		while (s->q.empty() && !s->stop)
		{
			s->worker_sleeping = true;
			s->cv_work.wait(lk);
			s->worker_sleeping = false;
		}
		if (s->q.empty() && s->stop)
			break;
		batch.swap(s->q);
		lk.unlock();

		bool any_barrier = false;
		for (Cmd& c : batch)
		{
			execute(s, c);
			if (c.barrier)
			{
				lk.lock();
				for (const rrv_spu2_event& e : s->core.events)
					s->ready_events.push_back(e);
				s->core.events.clear();
				c.barrier->done = true;
				lk.unlock();
				any_barrier = true;
			}
		}
		batch.clear();
		s->worker_cpu_ns.store(thread_cpu_ns(), std::memory_order_relaxed);
		if (any_barrier)
			s->cv_done.notify_all();
		lk.lock();
	}
}

u64 stamp(rrv_spu2* s, u64 t)
{
	if (t < s->last_t)
	{
		s->clamp_violations.fetch_add(1, std::memory_order_relaxed);
		return s->last_t;
	}
	s->last_t = t;
	return t;
}

// Submit a command; in inline mode run it now. Waits for barriers.
void submit(rrv_spu2* s, Cmd&& c)
{
	if (!s->cfg.threaded)
	{
		execute(s, c);
		if (c.barrier)
		{
			for (const rrv_spu2_event& e : s->core.events)
				s->ready_events.push_back(e);
			s->core.events.clear();
			c.barrier->done = true;
		}
		return;
	}
	Barrier* b = c.barrier;
	std::unique_lock<std::mutex> lk(s->m);
	s->q.push_back(std::move(c));
	const u64 depth = s->q.size();
	if (depth > s->queue_high_water.load(std::memory_order_relaxed))
		s->queue_high_water.store(depth, std::memory_order_relaxed);
	const bool wake = s->worker_sleeping;
	if (!b)
	{
		lk.unlock();
		if (wake)
			s->cv_work.notify_one();
		return;
	}
	if (wake)
		s->cv_work.notify_one();
	s->cv_done.wait(lk, [b] { return b->done; });
}
} // namespace

// ---------------------------------------------------------------------------
// C ABI
// ---------------------------------------------------------------------------

extern "C" {

rrv_spu2* rrv_spu2_create(const rrv_spu2_config* cfg)
{
	bool expected = false;
	if (!g_instance_live.compare_exchange_strong(expected, true))
		return nullptr;

	rrv_spu2* s = new rrv_spu2();
	if (cfg)
		s->cfg = *cfg;
	const u32 frames = next_pow2(s->cfg.output_ring_frames ? s->cfg.output_ring_frames : 8192);
	s->ring.assign(size_t(frames) * 2, 0.0f);
	s->ring_mask = frames - 1;

	g_inst = s;
	g_state = &s->core;
	core_power_on(s->core);

	if (s->cfg.threaded)
		s->worker = std::thread(worker_main, s);
	return s;
}

void rrv_spu2_destroy(rrv_spu2* s)
{
	if (!s)
		return;
	if (s->worker.joinable())
	{
		{
			std::lock_guard<std::mutex> lk(s->m);
			s->stop = true;
		}
		s->cv_work.notify_one();
		s->worker.join();
	}
	g_state = nullptr;
	g_inst = nullptr;
	delete s;
	g_instance_live.store(false);
}

void rrv_spu2_write16(rrv_spu2* s, uint64_t t, uint32_t reg_addr, uint16_t v)
{
	submit(s, Cmd{Op::Write16, 0, stamp(s, t), norm_reg(reg_addr), v, {}, nullptr});
}

uint16_t rrv_spu2_read16(rrv_spu2* s, uint64_t t, uint32_t reg_addr)
{
	Barrier b;
	submit(s, Cmd{Op::Read16, 0, stamp(s, t), norm_reg(reg_addr), 0, {}, &b});
	return b.r16;
}

void rrv_spu2_dma_write(rrv_spu2* s, uint64_t t, int core, uint32_t spu_addr16, const uint16_t* data, uint32_t count16)
{
	if (count16 > kMaxDmaHalfwords)
	{
		s->dma_truncated.fetch_add(1, std::memory_order_relaxed);
		count16 = kMaxDmaHalfwords;
	}
	Cmd c{Op::DmaWrite, core, stamp(s, t), spu_addr16, count16, {}, nullptr};
	c.payload.assign(data, data + count16);
	submit(s, std::move(c));
}

void rrv_spu2_dma_read(rrv_spu2* s, uint64_t t, int core, uint32_t spu_addr16, uint16_t* out, uint32_t count16)
{
	if (count16 > kMaxDmaHalfwords)
	{
		s->dma_truncated.fetch_add(1, std::memory_order_relaxed);
		count16 = kMaxDmaHalfwords;
	}
	Barrier b;
	b.out = out;
	submit(s, Cmd{Op::DmaRead, core, stamp(s, t), spu_addr16, count16, {}, &b});
}

void rrv_spu2_advance(rrv_spu2* s, uint64_t t)
{
	submit(s, Cmd{Op::Advance, 0, stamp(s, t), 0, 0, {}, nullptr});
}

size_t rrv_spu2_sync(rrv_spu2* s, uint64_t t, rrv_spu2_event* out, size_t max)
{
	Barrier b;
	submit(s, Cmd{Op::Sync, 0, stamp(s, t), 0, 0, {}, &b});
	std::unique_lock<std::mutex> lk(s->m, std::defer_lock);
	if (s->cfg.threaded)
		lk.lock();
	const size_t n = std::min(max, s->ready_events.size());
	for (size_t i = 0; i < n; i++)
		out[i] = s->ready_events[i];
	s->ready_events.erase(s->ready_events.begin(), s->ready_events.begin() + static_cast<std::ptrdiff_t>(n));
	return n;
}

size_t rrv_spu2_pull_output(rrv_spu2* s, float* dst, size_t frames)
{
	const u64 tail = s->ring_tail.load(std::memory_order_relaxed);
	const u64 head = s->ring_head.load(std::memory_order_acquire);
	const size_t avail = static_cast<size_t>(std::min<u64>(head - tail, frames));
	for (size_t i = 0; i < avail; i++)
	{
		const u64 j = ((tail + i) & s->ring_mask) * 2;
		dst[2 * i] = s->ring[j];
		dst[2 * i + 1] = s->ring[j + 1];
	}
	s->ring_tail.store(tail + avail, std::memory_order_release);
	if (avail < frames)
	{
		std::memset(dst + 2 * avail, 0, (frames - avail) * 2 * sizeof(float));
		s->underruns.fetch_add(frames - avail, std::memory_order_relaxed);
	}
	return avail;
}

void rrv_spu2_get_stats(rrv_spu2* s, rrv_spu2_stats* o)
{
	o->frames_mixed = s->frames_mixed.load(std::memory_order_relaxed);
	o->underrun_frames = s->underruns.load(std::memory_order_relaxed);
	o->overrun_frames = s->overruns.load(std::memory_order_relaxed);
	o->clamp_violations = s->clamp_violations.load(std::memory_order_relaxed);
	o->queue_high_water = s->queue_high_water.load(std::memory_order_relaxed);
	o->commands = s->commands.load(std::memory_order_relaxed);
	o->dma_overlaps = s->dma_overlaps.load(std::memory_order_relaxed);
	o->dma_truncated = s->dma_truncated.load(std::memory_order_relaxed);
	o->output_hash = s->output_hash.load(std::memory_order_relaxed);
	o->worker_cpu_ns = s->worker_cpu_ns.load(std::memory_order_relaxed);
}

uint64_t rrv_spu2_state_hash(rrv_spu2* s, uint64_t t)
{
	Barrier b;
	submit(s, Cmd{Op::Hash, 0, stamp(s, t), 0, 0, {}, &b});
	return b.r64;
}

} // extern "C"
