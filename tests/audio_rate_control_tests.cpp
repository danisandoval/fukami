// rrv::audio::RateControl and StereoResampler (src/audio/rrv_audio_rate_control.h): the host jitter buffer's output
// follows its fill, so a guest that makes slightly more or less audio than the sound card takes neither drops
// frames nor runs the buffer dry.
#include "rrv_audio_rate_control.h"

#include <cmath>
#include <deque>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool ok, const std::string &what)
{
    if (!ok)
    {
        ++failures;
        std::cerr << "FAIL: " << what << "\n";
    }
}

constexpr double kPi = 3.14159265358979323846;

// A stereo sine: left at `hz`, right its negative, sample `n` of a 48 kHz stream.
void sine(double hz, double n, float *frame)
{
    const float v = static_cast<float>(std::sin(2.0 * kPi * hz * n / 48000.0));
    frame[0] = v;
    frame[1] = -v;
}

// The jitter buffer of rspu2_glue.cpp with rate control, fed `produced` frames a second in fields and emptied 1024
// frames at a time at 48000 a second, for `seconds`. Returns false if it ever ran dry or passed `limit` frames.
bool settles(double produced, double seconds, size_t target, size_t limit, double &ratio, double &fill)
{
    rrv::audio::RateControl rate;
    rrv::audio::StereoResampler resampler;
    std::deque<float> fifo; // interleaved
    std::vector<float> in, out(1024 * 2);
    const double field = 1001.0 / 60000.0 * 48000.0 / produced; // seconds between deliveries of 800.8 frames
    double nextField = 0.0, owed = 0.0, time = 0.0;
    bool primed = false, ok = true;
    double sample = 0.0;
    for (; time < seconds; time += 1024.0 / 48000.0)
    {
        for (; nextField <= time; nextField += field)
        {
            owed += 800.8;
            for (; owed >= 1.0; owed -= 1.0, sample += 1.0)
            {
                float frame[2];
                sine(440.0, sample, frame);
                fifo.push_back(frame[0]);
                fifo.push_back(frame[1]);
            }
        }
        if (!primed && fifo.size() / 2 < target)
            continue;
        primed = true;
        const double r = rate.update(fifo.size() / 2, target);
        in.assign(fifo.begin(), fifo.end());
        size_t used = 0;
        const size_t written = resampler.process(in.data(), in.size() / 2, out.data(), 1024, r, used);
        fifo.erase(fifo.begin(), fifo.begin() + static_cast<std::ptrdiff_t>(used * 2));
        ok = ok && written == 1024 && fifo.size() / 2 <= limit;
    }
    ratio = rate.ratio;
    fill = rate.smoothed;
    return ok;
}

} // namespace

int main()
{
    using rrv::audio::RateControl;
    using rrv::audio::StereoResampler;

    // The resampler at ratio 1: the input, one frame late, whatever the sizes of the pieces it arrives in.
    {
        std::vector<float> in(4000 * 2), out(4000 * 2, 9.0f);
        for (size_t i = 0; i < 4000; ++i)
            sine(1000.0, static_cast<double>(i), &in[i * 2]);
        StereoResampler resampler;
        const size_t pieces[] = {1, 7, 512, 3, 1024, 2, 999, 1452};
        size_t taken = 0, written = 0;
        for (const size_t piece : pieces)
        {
            size_t offset = 0;
            while (offset < piece)
            {
                size_t used = 0;
                written += resampler.process(&in[(taken + offset) * 2], piece - offset, &out[written * 2], 4000 - written, 1.0, used);
                if (!used)
                    break;
                offset += used;
            }
            taken += piece;
        }
        expect(written == 3999 || written == 4000, "ratio 1 writes a frame per frame taken (wrote " + std::to_string(written) + ")");
        bool same = out[0] == 0.0f && out[1] == 0.0f;
        for (size_t i = 1; i < written; ++i)
            same = same && out[i * 2] == in[(i - 1) * 2] && out[i * 2 + 1] == in[(i - 1) * 2 + 1];
        expect(same, "ratio 1 returns the input unchanged, one frame late");
    }

    // At the extreme ratios: the frames taken per frame written, and a 1 kHz tone that stays a tone.
    for (const double ratio : {1.005, 0.995, 1.001})
    {
        const size_t frames = 48000;
        std::vector<float> in((frames + 600) * 2), out(frames * 2);
        for (size_t i = 0; i < frames + 600; ++i)
            sine(1000.0, static_cast<double>(i), &in[i * 2]);
        StereoResampler resampler;
        size_t used = 0;
        const size_t written = resampler.process(in.data(), frames + 600, out.data(), frames, ratio, used);
        expect(written == frames, "enough input for a second of output at ratio " + std::to_string(ratio));
        expect(std::fabs(static_cast<double>(used) / static_cast<double>(written) - ratio) < 1e-4,
            "frames taken per frame written is the ratio " + std::to_string(ratio));
        // Output n lies at input position (n - 1) * ratio: the first output is the frame before the first input.
        double worst = 0.0;
        for (size_t n = 2; n < written; ++n)
        {
            float ideal[2];
            sine(1000.0, (static_cast<double>(n) - 1.0) * ratio, ideal);
            worst = std::max(worst, std::fabs(static_cast<double>(out[n * 2]) - ideal[0]));
            worst = std::max(worst, std::fabs(static_cast<double>(out[n * 2 + 1]) - ideal[1]));
        }
        expect(worst < 2e-3, "a 1 kHz tone resampled at " + std::to_string(ratio) + " is within 0.002 of the ideal (worst " +
            std::to_string(worst) + ")");
    }

    // The ratio from the fill.
    {
        RateControl rate;
        expect(rate.update(2048, 2048) == 1.0, "at the target the ratio is 1");
        RateControl full;
        expect(std::fabs(full.update(9600, 2048) - 1.005) < 1e-12, "far above the target: 1.005, no more");
        RateControl half;
        expect(std::fabs(half.update(3072, 2048) - 1.005) < 1e-12, "half the target above it: already 1.005");
        RateControl empty;
        expect(std::fabs(empty.update(0, 2048) - 0.995) < 1e-12, "empty: 0.995, no less");
        RateControl steps;
        steps.update(2048, 2048);
        const double after = steps.update(2848, 2048); // one field's delivery more
        expect(after > 1.0 && after < 1.0001, "one field's audio arriving moves the ratio by less than 0.01 %");
        steps.reset();
        expect(steps.update(2048, 2048) == 1.0, "after a reset the fill before it is forgotten");
    }

    // The whole loop. A display lock at 60.00 Hz makes 0.1 % more audio than the card takes, and the lock is allowed
    // up to 0.2 %; a sound card is off by far less. None may run dry or grow past the buffer's 9600 frames, and the
    // ratio must settle on the rate of the audio made.
    for (const double offset : {0.001, -0.001, 0.002, -0.002, 0.004, 0.0})
    {
        double ratio = 0.0, fill = 0.0;
        const bool ok = settles(48000.0 * (1.0 + offset), 120.0, 2048, 9600, ratio, fill);
        const std::string name = "audio made " + std::to_string(offset * 100.0) + " % off the card's rate";
        expect(ok, name + ": never dry, never past 9600 frames in 120 s");
        expect(std::fabs(ratio - (1.0 + offset)) < 2e-4, name + ": the ratio settles on it (" + std::to_string(ratio) + ")");
        expect(fill > 600.0 && fill < 4096.0, name + ": the fill settles near the target (" + std::to_string(fill) + ")");
    }

    if (failures)
    {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "audio rate control: ok\n";
    return 0;
}
