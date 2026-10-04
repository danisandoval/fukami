// SPDX-License-Identifier: GPL-3.0+
// Self-test for rrv_pcsx2_spu2: tone, inline-vs-threaded determinism,
// threading cost, and a CPU-cost benchmark.
//
//   spu2_selftest tone | determinism | threading | bench | all
//
// Exit code 0 = pass.

#include "rrv_pcsx2_spu2.h"

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <time.h>
#endif

namespace
{
constexpr uint64_t kIopHz = 36864000; // 768 cycles per 48 kHz frame
constexpr uint64_t kCyclesPerFrame = 768;
constexpr uint32_t R = 0x1F900000u;

// Register helpers (core c, voice v); offsets from PCSX2 SPU2/regs.h.
uint32_t vp(int c, int v, uint32_t p) { return R + (c ? 0x400 : 0) + v * 16 + p; }
uint32_t va(int c, int v, uint32_t p) { return R + (c ? 0x400 : 0) + 0x1C0 + v * 12 + p; }
uint32_t cr(int c, uint32_t off) { return R + (c ? 0x400 : 0) + off; }
uint32_t cx(int c, uint32_t off) { return R + off + (c ? 0x28 : 0); } // 0x760.. "different" area

constexpr uint32_t VOLL = 0x0, VOLR = 0x2, PITCH = 0x4, ADSR1 = 0x6, ADSR2 = 0x8, ENVX = 0xA;
constexpr uint32_t SSA = 0x0, NAX = 0x8;
constexpr uint32_t MMIX = 0x198, ATTR = 0x19A, IRQA = 0x19C, KON = 0x1A0, KOFF = 0x1A4, ENDX = 0x340, STATX = 0x344;
constexpr uint32_t MVOLL = 0x760, MVOLR = 0x762, EVOLL = 0x764, EVOLR = 0x766;

// PS-ADPCM, filter 0: each 16-byte block = header (shift | filter<<4, flags)
// + 28 4-bit samples. With shift 0 a nibble n decodes to n << 12.
std::vector<uint16_t> encode_sine_loop(int period_samples, int blocks)
{
	std::vector<uint16_t> out;
	int n = 0;
	for (int b = 0; b < blocks; b++)
	{
		uint8_t bytes[16] = {};
		bytes[0] = 0x00; // shift 0, filter 0
		uint8_t flags = 0x2;                  // LOOP
		if (b == 0) flags |= 0x4;             // LOOP_START
		if (b == blocks - 1) flags |= 0x1;    // LOOP_END
		bytes[1] = flags;
		for (int i = 0; i < 28; i++, n++)
		{
			const double s = std::sin(2.0 * M_PI * double(n) / double(period_samples));
			int q = int(std::lround(s * 7.0));
			if (q < -8) q = -8;
			if (q > 7) q = 7;
			const uint8_t nib = uint8_t(q) & 0xF;
			if (i & 1)
				bytes[2 + i / 2] |= uint8_t(nib << 4);
			else
				bytes[2 + i / 2] |= nib;
		}
		for (int i = 0; i < 8; i++)
			out.push_back(uint16_t(bytes[2 * i] | (bytes[2 * i + 1] << 8)));
	}
	return out;
}

void key_voice(rrv_spu2* s, uint64_t t, int c, int v, uint32_t ssa, uint16_t pitch)
{
	rrv_spu2_write16(s, t, vp(c, v, VOLL), 0x3FFF);
	rrv_spu2_write16(s, t, vp(c, v, VOLR), 0x3FFF);
	rrv_spu2_write16(s, t, vp(c, v, PITCH), pitch);
	rrv_spu2_write16(s, t, vp(c, v, ADSR1), 0x000F);
	rrv_spu2_write16(s, t, vp(c, v, ADSR2), 0x1FC0);
	rrv_spu2_write16(s, t, va(c, v, SSA), uint16_t((ssa >> 16) & 0xF));
	rrv_spu2_write16(s, t, va(c, v, SSA + 2), uint16_t(ssa & 0xFFFF));
}

double dominant_hz(const std::vector<float>& lr, size_t first, size_t count, double lo, double hi, double step)
{
	double best_f = 0, best_p = -1;
	for (double f = lo; f <= hi; f += step)
	{
		const double w = 2.0 * M_PI * f / 48000.0;
		const double coeff = 2.0 * std::cos(w);
		double s1 = 0, s2 = 0;
		for (size_t i = 0; i < count; i++)
		{
			const double x = lr[2 * (first + i)];
			const double s0 = x + coeff * s1 - s2;
			s2 = s1;
			s1 = s0;
		}
		const double p = s1 * s1 + s2 * s2 - coeff * s1 * s2;
		if (p > best_p)
		{
			best_p = p;
			best_f = f;
		}
	}
	return best_f;
}

uint64_t now_ns()
{
	return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint64_t thread_cpu_ns()
{
#if !defined(_WIN32)
	timespec ts;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
	return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
#else
	return now_ns();
#endif
}

// ---------------------------------------------------------------------------
// 1. Tone
// ---------------------------------------------------------------------------
bool test_tone()
{
	rrv_spu2_config cfg{0, 65536};
	rrv_spu2* s = rrv_spu2_create(&cfg);
	if (!s) { std::printf("tone: create failed\n"); return false; }

	const auto adpcm = encode_sine_loop(48, 12); // 1000 Hz at pitch 0x1000; 336-sample loop
	uint64_t t = 1000;
	rrv_spu2_dma_write(s, t, 0, 0x5000, adpcm.data(), uint32_t(adpcm.size()));
	rrv_spu2_event ev[16];
	size_t n = rrv_spu2_sync(s, t + 10000, ev, 16);
	bool dma_ok = (n == 1 && ev[0].kind == RRV_SPU2_EVENT_DMA4_DONE);
	std::printf("tone: DMA4 upload of %zu halfwords -> %zu event(s)", adpcm.size(), n);
	if (n) std::printf(", first kind=%u at cycle %" PRIu64 " (submitted at %" PRIu64 ", +%" PRIu64 ")", ev[0].kind, ev[0].iop_cycle, t, ev[0].iop_cycle - t);
	std::printf("\n");

	t += 20000;
	key_voice(s, t, 1, 0, 0x5000, 0x1000);
	rrv_spu2_write16(s, t, cx(1, MVOLL), 0x3FFF);
	rrv_spu2_write16(s, t, cx(1, MVOLR), 0x3FFF);
	rrv_spu2_write16(s, t, cr(1, KON), 0x0001);
	const uint64_t t_on = t;
	t += kIopHz / 2;
	rrv_spu2_advance(s, t);
	rrv_spu2_sync(s, t, ev, 16);

	std::vector<float> out(65536 * 2);
	const size_t got = rrv_spu2_pull_output(s, out.data(), 65536);
	rrv_spu2_stats st;
	rrv_spu2_get_stats(s, &st);

	// Analyse the samples after key-on (skip 1000 frames of attack/filter settle).
	const size_t on_frame = size_t(t_on / kCyclesPerFrame);
	const size_t first = on_frame + 1000;
	const size_t count = got > first ? got - first : 0;
	double rms = 0;
	for (size_t i = 0; i < count; i++)
		rms += double(out[2 * (first + i)]) * out[2 * (first + i)];
	rms = count ? std::sqrt(rms / double(count)) : 0;
	const double coarse = dominant_hz(out, first, count, 50, 6000, 10);
	const double fine = dominant_hz(out, first, count, coarse - 10, coarse + 10, 0.25);
	const double err = std::fabs(fine - 1000.0) / 1000.0;
	std::printf("tone: frames mixed=%" PRIu64 " pulled=%zu analysed=%zu rms=%.4f dominant=%.2f Hz (expected 1000, err %.3f%%)\n",
		st.frames_mixed, got, count, rms, fine, err * 100.0);
	rrv_spu2_destroy(s);

	const bool ok = dma_ok && count > 20000 && rms > 0.05 && err < 0.02;
	std::printf("tone: %s\n", ok ? "PASS" : "FAIL");
	return ok;
}

// ---------------------------------------------------------------------------
// 2. Determinism (inline vs threaded)
// ---------------------------------------------------------------------------
struct RunResult
{
	std::vector<uint32_t> reads;
	std::vector<rrv_spu2_event> events;
	std::vector<uint64_t> hashes;
	std::vector<float> output;
	rrv_spu2_stats stats{};
	uint64_t caller_async_ns = 0; // wall time inside write16/advance/dma_write
	uint64_t async_calls = 0;
};

RunResult run_script(int threaded)
{
	RunResult r;
	rrv_spu2_config cfg{threaded, 1 << 16};
	rrv_spu2* s = rrv_spu2_create(&cfg);
	if (!s) { std::printf("determinism: create failed\n"); std::exit(2); }

	auto W = [&](uint64_t t, uint32_t a, uint16_t v) {
		const uint64_t t0 = now_ns();
		rrv_spu2_write16(s, t, a, v);
		r.caller_async_ns += now_ns() - t0;
		r.async_calls++;
	};
	auto A = [&](uint64_t t) {
		const uint64_t t0 = now_ns();
		rrv_spu2_advance(s, t);
		r.caller_async_ns += now_ns() - t0;
		r.async_calls++;
	};
	auto D = [&](uint64_t t, int core, uint32_t addr, const std::vector<uint16_t>& d) {
		const uint64_t t0 = now_ns();
		rrv_spu2_dma_write(s, t, core, addr, d.data(), uint32_t(d.size()));
		r.caller_async_ns += now_ns() - t0;
		r.async_calls++;
	};
	auto Rd = [&](uint64_t t, uint32_t a) { r.reads.push_back(rrv_spu2_read16(s, t, a)); };
	auto S = [&](uint64_t t) {
		rrv_spu2_event ev[8];
		size_t n;
		size_t total = 0;
		// Small max on purpose: exercises leaving events queued between calls.
		while ((n = rrv_spu2_sync(s, t, ev, 8)) > 0)
		{
			r.events.insert(r.events.end(), ev, ev + n);
			total += n;
		}
		return total;
	};

	const auto toneA = encode_sine_loop(48, 12); // 1000 Hz
	const auto toneB = encode_sine_loop(56, 2);  // ~857 Hz
	std::vector<uint16_t> big(0x3000);
	for (size_t i = 0; i < big.size(); i++)
		big[i] = uint16_t(i * 2654435761u >> 16);

	uint64_t t = 100;
	D(t, 0, 0x5000, toneA);
	D(t + 50, 1, 0x6000, toneB);        // core 1 DMA while core 0's is in flight
	D(t + 3000, 0, 0x20000, big);       // multi-step plain DMA (partial copies + countdowns)
	S(t + 3010);                        // only the tone DMAs are due yet
	t = 200000;
	S(t);
	r.hashes.push_back(rrv_spu2_state_hash(s, t));

	// Mixer routing: core0 -> core1 external input (dry), master volumes, reverb on core1.
	W(t, cr(1, MMIX), 0x0FFC);
	W(t, cx(0, MVOLL), 0x3FFF); W(t, cx(0, MVOLR), 0x3FFF);
	W(t, cx(1, MVOLL), 0x3FFF); W(t, cx(1, MVOLR), 0x3FFF);
	W(t, cx(1, EVOLL), 0x2000); W(t, cx(1, EVOLR), 0x2000);
	key_voice(s, t, 1, 0, 0x5000, 0x1000);
	key_voice(s, t, 0, 1, 0x6000, 0x0C00);
	// IRQ on core 1 when voice 0 reads 0x5000 + 0x28 (inside the loop).
	W(t, cr(1, IRQA), 0x0000);
	W(t, cr(1, IRQA + 2), 0x5028);
	W(t, cr(1, ATTR), 0x80C0); // IRQ enable + FX enable (+ bit 15)
	W(t + 7, cr(1, KON), 0x0001);
	W(t + 9000, cr(0, KON), 0x0002);
	Rd(t + 9000, vp(1, 0, ENVX));
	t += 10000;

	// Irregular advance pattern with reads and IRQ re-arming.
	uint64_t step = 1234;
	int rearms = 0;
	for (int i = 0; i < 400; i++)
	{
		t += step;
		step = (step * 7 + 4099) % 180000 + 100;
		A(t);
		if (i % 9 == 0)
		{
			Rd(t, vp(1, 0, ENVX));
			Rd(t, va(1, 0, NAX + 2));
			Rd(t, cr(1, ENDX));
			Rd(t, cr(0, STATX));
		}
		if (i % 13 == 0)
		{
			if (S(t) && rearms < 20)
			{
				// Re-arm: IRQ enable off/on clears Spdif.Info bit for the core.
				W(t, cr(1, ATTR), 0x8080);
				W(t, cr(1, ATTR), 0x80C0);
				rearms++;
			}
		}
		if (i == 120)
			W(t, cr(1, KOFF), 0x0001);
		if (i == 160)
			W(t, cr(1, KON), 0x0001);
		if (i == 200)
		{
			std::vector<uint16_t> back(64);
			rrv_spu2_dma_read(s, t, 0, 0x20010, back.data(), uint32_t(back.size()));
			for (uint16_t v : back)
				r.reads.push_back(v);
		}
		if (i == 250)
			A(t - 5000); // deliberate backwards timestamp: clamped + counted
		if (i % 50 == 0)
			r.hashes.push_back(rrv_spu2_state_hash(s, t));
	}
	S(t);
	r.hashes.push_back(rrv_spu2_state_hash(s, t));

	r.output.resize(size_t(1 << 16) * 2);
	const size_t got = rrv_spu2_pull_output(s, r.output.data(), 1 << 16);
	r.output.resize(got * 2);
	rrv_spu2_get_stats(s, &r.stats);
	rrv_spu2_destroy(s);
	return r;
}

bool test_determinism(RunResult* inline_out = nullptr, RunResult* threaded_out = nullptr)
{
	RunResult a = run_script(0);
	RunResult b = run_script(1);

	bool ok = true;
	auto check = [&](bool c, const char* what) {
		std::printf("determinism: %-28s %s\n", what, c ? "identical" : "DIFFERENT");
		ok = ok && c;
	};
	check(a.reads == b.reads, "register/DMA reads");
	bool ev_same = a.events.size() == b.events.size();
	for (size_t i = 0; ev_same && i < a.events.size(); i++)
		ev_same = a.events[i].iop_cycle == b.events[i].iop_cycle && a.events[i].kind == b.events[i].kind && a.events[i].data == b.events[i].data;
	check(ev_same, "events (cycle, kind, data)");
	check(a.hashes == b.hashes, "state hashes");
	check(a.output.size() == b.output.size() &&
			  std::memcmp(a.output.data(), b.output.data(), a.output.size() * sizeof(float)) == 0,
		"mixed output samples");
	check(a.stats.output_hash == b.stats.output_hash, "s16 output hash");
	check(a.stats.frames_mixed == b.stats.frames_mixed, "frames mixed");
	check(a.stats.clamp_violations == 1 && b.stats.clamp_violations == 1, "clamp violations (==1)");
	std::printf("determinism: clamp violations inline=%" PRIu64 " threaded=%" PRIu64 "\n", a.stats.clamp_violations, b.stats.clamp_violations);

	size_t irqs = 0, dma4 = 0, dma7 = 0;
	for (const auto& e : a.events)
		(e.kind == 1 ? irqs : e.kind == 4 ? dma4 : dma7)++;
	double peak = 0;
	for (float f : a.output)
		peak = std::max(peak, double(std::fabs(f)));
	std::printf("determinism: %zu reads, %zu events (%zu SPU IRQ, %zu DMA4, %zu DMA7), %zu hashes, %zu frames, peak %.3f, final hash %016" PRIx64 "\n",
		a.reads.size(), a.events.size(), irqs, dma4, dma7, a.hashes.size(), a.output.size() / 2, peak,
		a.hashes.empty() ? 0 : a.hashes.back());
	if (!a.events.empty())
	{
		std::printf("determinism: first events:");
		for (size_t i = 0; i < std::min<size_t>(a.events.size(), 6); i++)
			std::printf(" [%u@%" PRIu64 " d=%u]", a.events[i].kind, a.events[i].iop_cycle, a.events[i].data);
		std::printf("\n");
	}
	ok = ok && irqs >= 2 && dma4 >= 2 && dma7 >= 1 && peak > 0.05;
	std::printf("determinism: %s\n", ok ? "PASS" : "FAIL");
	if (inline_out) *inline_out = std::move(a);
	if (threaded_out) *threaded_out = std::move(b);
	return ok;
}

// ---------------------------------------------------------------------------
// 3. Threading: caller-side cost + concurrent consumer (for TSan)
// ---------------------------------------------------------------------------
bool test_threading()
{
	RunResult a, b;
	if (!test_determinism(&a, &b))
		return false;
	std::printf("threading: caller time in async calls: inline %.1f us total (%.3f us/call), threaded %.1f us total (%.3f us/call), %" PRIu64 " calls, queue high-water %" PRIu64 "\n",
		a.caller_async_ns / 1e3, double(a.caller_async_ns) / 1e3 / double(a.async_calls),
		b.caller_async_ns / 1e3, double(b.caller_async_ns) / 1e3 / double(b.async_calls), b.async_calls,
		b.stats.queue_high_water);

	// Concurrent consumer thread pulling while the worker mixes.
	rrv_spu2_config cfg{1, 4096};
	rrv_spu2* s = rrv_spu2_create(&cfg);
	const auto tone = encode_sine_loop(48, 12);
	rrv_spu2_dma_write(s, 10, 0, 0x5000, tone.data(), uint32_t(tone.size()));
	key_voice(s, 20000, 1, 0, 0x5000, 0x1000);
	rrv_spu2_write16(s, 20000, cx(1, MVOLL), 0x3FFF);
	rrv_spu2_write16(s, 20000, cx(1, MVOLR), 0x3FFF);
	rrv_spu2_write16(s, 20000, cr(1, KON), 1);
	std::atomic<bool> done{false};
	std::atomic<uint64_t> pulled{0};
	std::thread consumer([&] {
		std::vector<float> buf(256 * 2);
		while (!done.load())
		{
			pulled += rrv_spu2_pull_output(s, buf.data(), 256);
			std::this_thread::sleep_for(std::chrono::microseconds(500));
		}
	});
	uint64_t t = 20000;
	for (int i = 0; i < 2000; i++)
	{
		t += 36864; // 1 ms of guest time
		rrv_spu2_advance(s, t);
		if (i % 100 == 0)
			rrv_spu2_state_hash(s, t);
	}
	rrv_spu2_event ev[4];
	rrv_spu2_sync(s, t, ev, 4);
	done = true;
	consumer.join();
	rrv_spu2_stats st;
	rrv_spu2_get_stats(s, &st);
	rrv_spu2_destroy(s);
	std::printf("threading: concurrent consumer pulled %" PRIu64 " of %" PRIu64 " frames (overrun %" PRIu64 ", underrun %" PRIu64 ")\n",
		pulled.load(), st.frames_mixed, st.overrun_frames, st.underrun_frames);
	const bool ok = pulled.load() > 0 && pulled.load() + st.overrun_frames <= st.frames_mixed;
	std::printf("threading: %s\n", ok ? "PASS" : "FAIL");
	return ok;
}

// ---------------------------------------------------------------------------
// 4. Benchmark: CPU per second of audio, inline vs worker.
// ---------------------------------------------------------------------------
void bench_mode(int threaded, double seconds)
{
	rrv_spu2_config cfg{threaded, 1 << 14};
	rrv_spu2* s = rrv_spu2_create(&cfg);
	const auto tone = encode_sine_loop(48, 12);
	rrv_spu2_dma_write(s, 10, 0, 0x5000, tone.data(), uint32_t(tone.size()));
	uint64_t t = 20000;
	// 16 voices per core, reverb enabled on core 1.
	for (int c = 0; c < 2; c++)
		for (int v = 0; v < 16; v++)
			key_voice(s, t, c, v, 0x5000, uint16_t(0x0800 + v * 0x80));
	rrv_spu2_write16(s, t, cr(1, MMIX), 0x0FFC);
	rrv_spu2_write16(s, t, cx(1, MVOLL), 0x1000);
	rrv_spu2_write16(s, t, cx(1, MVOLR), 0x1000);
	rrv_spu2_write16(s, t, cr(1, ATTR), 0x8080);
	rrv_spu2_write16(s, t, cr(0, KON), 0xFFFF);
	rrv_spu2_write16(s, t, cr(1, KON), 0xFFFF);
	rrv_spu2_event ev[8];
	rrv_spu2_sync(s, t, ev, 8);

	std::vector<float> buf(2048 * 2);
	const uint64_t c0 = thread_cpu_ns();
	const uint64_t w0 = now_ns();
	const int ms = int(seconds * 1000);
	for (int i = 0; i < ms; i++)
	{
		t += 36864;
		rrv_spu2_write16(s, t, vp(1, 0, PITCH), uint16_t(0x1000 + (i & 15))); // a register write per ms
		rrv_spu2_advance(s, t);
		if (i % 16 == 15)
			rrv_spu2_pull_output(s, buf.data(), 768);
	}
	rrv_spu2_sync(s, t, ev, 8);
	const uint64_t caller_cpu = thread_cpu_ns() - c0;
	const uint64_t wall = now_ns() - w0;
	rrv_spu2_stats st;
	rrv_spu2_get_stats(s, &st);
	rrv_spu2_destroy(s);
	const double audio_s = double(ms) / 1000.0;
	std::printf("bench: %-8s %.0f s audio (32 voices keyed, reverb on): caller CPU %.2f ms/s-audio, worker CPU %.2f ms/s-audio, wall %.2f ms/s-audio, frames %" PRIu64 "\n",
		threaded ? "threaded" : "inline", audio_s, caller_cpu / 1e6 / audio_s,
		st.worker_cpu_ns / 1e6 / audio_s, wall / 1e6 / audio_s, st.frames_mixed);
}
} // namespace

int main(int argc, char** argv)
{
	const std::string what = argc > 1 ? argv[1] : "all";
	bool ok = true;
	if (what == "tone" || what == "all")
		ok = test_tone() && ok;
	if (what == "determinism" || what == "all")
		ok = test_determinism() && ok;
	if (what == "threading" || what == "all")
		ok = test_threading() && ok;
	if (what == "bench" || what == "all")
	{
		const double secs = argc > 2 ? std::atof(argv[2]) : 10.0;
		bench_mode(0, secs);
		bench_mode(1, secs);
	}
	std::printf("%s\n", ok ? "ALL PASS" : "FAILURES");
	return ok ? 0 : 1;
}
