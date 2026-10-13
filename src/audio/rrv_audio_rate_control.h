#ifndef RRV_AUDIO_RATE_CONTROL_H
#define RRV_AUDIO_RATE_CONTROL_H

// rrv_audio_rate_control.h — dynamic rate control for the host audio jitter buffer.
//
// The guest makes exactly 48000 audio frames per guest second and the sound card takes 48000 per second of its own
// clock. The two clocks never agree: the card's crystal is off by some parts in 10,000, and with
// `[display] sync_to_display` the guest runs at the display's rate (60.00 instead of 59.94 starts a second: 0.1 %
// more audio than the card takes). Without rate control the jitter buffer absorbs the difference by dropping its
// oldest frames when it is full and by playing silence when it runs dry (rspu2_glue.cpp, Jitter).
//
// Rate control resamples the buffer's output by a ratio within 0.5 % of 1 that follows the buffer's fill: fuller
// than its target, the output takes slightly more than one buffered frame per frame played, and the reverse. It is
// the scheme of RetroArch (Themaister, "Dynamic Rate Control for Retro Game Emulators") and bsnes (near): 0.5 % is 8.6
// cents of pitch at most; the 0.1 % of a display lock is 1.7 cents.
//
// The fill is smoothed over about a second before it sets the ratio: the guest delivers a field's audio (about 800
// frames) at once and the card asks for 1024 at a time, so the fill at any one callback moves by a third of the
// target, and following that would be an audible wobble. The loop that results (the fill integrates the rate error;
// the ratio is proportional to the smoothed fill) settles in about 4 s, slower than the smoothing: no overshoot.
// The ratio reaches its limit when the fill is half the target away from it. What the buffer can hold is less than
// that 0.5 %: too little audio lowers the fill, and it must still cover a callback. The test holds 0.2 % either
// way for two minutes; a display lock therefore has to stay within 0.2 % of the game's rate (60.00 Hz is 0.1 %).
// The resampler is a 4-point cubic (Hermite): at ratio 1 it returns its input unchanged, one frame late.
//
// Host only: it runs on the SDL callback thread on audio the guest has already produced. Off unless
// RRV_AUDIO_RATE_CONTROL=1. tests/audio_rate_control_tests.cpp.

#include <algorithm>
#include <cstddef>

namespace rrv::audio
{

struct RateControl
{
    static constexpr double kMaxDelta = 0.005; // the ratio stays within 1 +- this
    static constexpr double kSmoothing = 0.02; // share of a new fill sample per callback (about 47 callbacks a second)

    // The ratio (buffered frames taken per frame played) for a callback that finds `fill` frames buffered and
    // should find `target`.
    double update(size_t fill, size_t target)
    {
        const double sample = static_cast<double>(fill);
        smoothed = have ? smoothed + kSmoothing * (sample - smoothed) : sample;
        have = true;
        const double error = target ? (smoothed - static_cast<double>(target)) / (0.5 * static_cast<double>(target)) : 0.0;
        ratio = 1.0 + kMaxDelta * std::clamp(error, -1.0, 1.0);
        lowest = std::min(lowest, ratio);
        highest = std::max(highest, ratio);
        return ratio;
    }

    // After a gap (the buffer ran dry and primes again, a fast-forward): the fill before it says nothing now.
    void reset()
    {
        have = false;
        ratio = 1.0;
    }

    double smoothed = 0.0, ratio = 1.0, lowest = 1.0, highest = 1.0;
    bool have = false;
};

// Interleaved stereo float. Keeps its place between calls, so the input may arrive in pieces of any length.
struct StereoResampler
{
    // Writes up to `outFrames` frames to `out` from `in`, advancing the input by `ratio` frames per frame written.
    // Returns the frames written and sets `consumed` to the input frames taken; it stops early when the input runs out.
    size_t process(const float* in, size_t inFrames, float* out, size_t outFrames, double ratio, size_t& consumed)
    {
        size_t taken = 0, written = 0;
        while (written < outFrames)
        {
            // The output lies `phase` of the way from history[1] to history[2]; the cubic also needs the frame after.
            while (phase >= 1.0 && taken < inFrames)
            {
                push(in + taken * 2);
                ++taken;
                phase -= 1.0;
            }
            if (phase >= 1.0 || taken >= inFrames)
                break;
            const float* next = in + taken * 2;
            const float t = static_cast<float>(phase);
            for (int channel = 0; channel < 2; ++channel)
            {
                const float y0 = history[0][channel], y1 = history[1][channel], y2 = history[2][channel], y3 = next[channel];
                const float c1 = 0.5f * (y2 - y0);
                const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
                const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
                out[written * 2 + channel] = ((c3 * t + c2) * t + c1) * t + y1;
            }
            ++written;
            phase += ratio;
        }
        consumed = taken;
        return written;
    }

    void reset()
    {
        *this = StereoResampler{};
    }

    float history[3][2] = {};
    double phase = 1.0; // the first output is the first input frame, once the history has passed it

private:
    void push(const float* frame)
    {
        for (int channel = 0; channel < 2; ++channel)
        {
            history[0][channel] = history[1][channel];
            history[1][channel] = history[2][channel];
            history[2][channel] = frame[channel];
        }
    }
};

} // namespace rrv::audio

#endif
