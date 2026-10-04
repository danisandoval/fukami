#include "rrv_sdl_audio.h"

#include <SDL.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>
#include <time.h>
#include <vector>

namespace rrv::audio
{
struct Sound
{
    std::vector<float> stereo;
    size_t cursor = 0;
    float gain = 0;
    bool active = false;
};

namespace
{
struct Device
{
    SDL_AudioDeviceID id = 0;
    SDL_AudioSpec spec{};
    bool ownsSubsystem = false;
    std::atomic<bool> initialized{false};
    std::mutex mutex;
    std::vector<SoundHandle> sounds;
    std::atomic<StreamPull> streamPull{nullptr};
    std::atomic<void*> streamUser{nullptr};
    // Host-stall watchdog (Gate-8 L3), see watchdogLoop().
    std::thread watchdog;
    std::atomic<bool> watchdogStop{false};
    uint64_t restarts = 0;
};
Device& device()
{
    static Device state;
    return state;
}

void mix(float* output, size_t samples)
{
    auto& state = device();
    std::fill_n(output, samples, 0.0f);
    std::lock_guard<std::mutex> lock(state.mutex);
    if (const StreamPull pull = state.streamPull.load(std::memory_order_acquire))
        pull(state.streamUser.load(std::memory_order_acquire), output, samples / 2);
    for (const auto& sound : state.sounds)
    {
        if (!sound->active)
            continue;
        const size_t count = std::min(samples, sound->stereo.size() - sound->cursor);
        for (size_t index = 0; index < count; ++index)
            output[index] += sound->stereo[sound->cursor + index] * sound->gain;
        sound->cursor += count;
        if (sound->cursor == sound->stereo.size())
            sound->active = false;
    }
    std::erase_if(state.sounds, [](const auto& sound) { return !sound->active; });
}

void callback(void*, Uint8* stream, int length)
{
    mix(reinterpret_cast<float*>(stream), static_cast<size_t>(length) / sizeof(float));
}

SDL_AudioDeviceID openDevice(SDL_AudioSpec* obtained)
{
    SDL_AudioSpec wanted{};
    wanted.freq = 48000;
    wanted.format = AUDIO_F32SYS;
    wanted.channels = 2;
    wanted.samples = 1024;
    wanted.callback = callback;
    // Gate 8: the device runs at the SPU2's 48 kHz; SDL converts to the
    // hardware rate, so the guest's audio rate never depends on the host.
    return SDL_OpenAudioDevice(nullptr, 0, &wanted, obtained, 0);
}

// CLOCK_MONOTONIC keeps counting through system sleep on macOS
// (mach_continuous_time), unlike steady_clock.
uint64_t continuousNs()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

// Host-stall watchdog (Gate-8 L3, 2026-09-29). When the whole process is
// frozen (SIGSTOP/SIGCONT, system sleep, App Nap), SDL's CoreAudio queue
// replays the missed time on resume: it asks for audio at ~3x real time until
// it has caught up (measured: 60.5 s of padding after a 60 s stop, ~20 s of
// broken sound). This thread wakes every 100 ms; a wake more than 500 ms late
// means the host stalled, and the device is closed and reopened before the
// catch-up starts, so playback resumes at real time. Host-only: the guest's
// audio production is untouched. RRV_AUDIO_STALL_RESTART=0 disables it.
void watchdogLoop()
{
    auto& state = device();
    uint64_t last = continuousNs();
    uint64_t lastRetry = 0;
    while (!state.watchdogStop.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const uint64_t now = continuousNs();
        const uint64_t gap = now - last;
        last = now;
        if (state.watchdogStop.load())
            break;
        // Right after a system wake the output device can refuse to open
        // (observed on the owner's Mac 2026-09-29: two of four reopens
        // failed during sleep/wake). Keep retrying once a second until it
        // opens, so audio never stays dead after a failed reopen.
        if (!state.id && gap < 500000000ull)
        {
            if (now - lastRetry < 1000000000ull)
                continue;
            lastRetry = now;
            SDL_AudioSpec obtained{};
            state.id = openDevice(&obtained);
            if (state.id)
            {
                SDL_PauseAudioDevice(state.id, 0);
                std::fprintf(stderr, "[audio] playback device reopened on retry\n");
            }
            continue;
        }
        // Gate-8 L4: an output device that disappears (unplugged, Bluetooth
        // off) leaves SDL's device stopped; reopen on the current default.
        if (state.id && gap < 500000000ull && SDL_GetAudioDeviceStatus(state.id) != SDL_AUDIO_PLAYING)
        {
            SDL_CloseAudioDevice(state.id);
            SDL_AudioSpec obtained{};
            state.id = openDevice(&obtained);
            if (state.id)
                SDL_PauseAudioDevice(state.id, 0);
            ++state.restarts;
            std::fprintf(stderr, "[audio] playback device stopped (output lost): %s\n",
                         state.id ? "reopened on the default output" : "reopen FAILED, retrying");
            lastRetry = now;
            continue;
        }
        if (gap < 500000000ull)
            continue;
        // Close joins the callback; never hold the mixer mutex here.
        if (state.id)
            SDL_CloseAudioDevice(state.id);
        SDL_AudioSpec obtained{};
        state.id = openDevice(&obtained);
        if (state.id)
            SDL_PauseAudioDevice(state.id, 0);
        ++state.restarts;
        std::fprintf(stderr, "[audio] host stall of %llu ms: playback device %s\n",
                     (unsigned long long)(gap / 1000000ull), state.id ? "restarted" : "reopen FAILED");
        last = lastRetry = continuousNs();
    }
}
}

void initialize()
{
    auto& state = device();
    if (state.initialized.load())
        return;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
    {
        std::fprintf(stderr, "[audio] SDL audio initialization failed: %s\n", SDL_GetError());
        return;
    }
    state.ownsSubsystem = true;
    state.id = openDevice(&state.spec);
    if (!state.id)
    {
        std::fprintf(stderr, "[audio] SDL playback device open failed: %s\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        state.ownsSubsystem = false;
        return;
    }
    SDL_PauseAudioDevice(state.id, 0);
    if (SDL_GetAudioDeviceStatus(state.id) != SDL_AUDIO_PLAYING)
    {
        SDL_CloseAudioDevice(state.id);
        state.id = 0;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        state.ownsSubsystem = false;
        std::fprintf(stderr, "[audio] SDL playback device did not start\n");
        return;
    }
    state.initialized.store(true);
    std::fprintf(stderr, "[audio] SDL default playback initialized; float32 stereo rate=%d\n", state.spec.freq);
#if !defined(RRV_SDL_AUDIO_TESTING)
    if (const char* v = std::getenv("RRV_AUDIO_STALL_RESTART"); !(v && v[0] == '0' && v[1] == '\0'))
    {
        state.watchdogStop.store(false);
        state.watchdog = std::thread(watchdogLoop);
    }
#endif
}

bool ready() { return device().initialized.load(); }

void setStreamSource(StreamPull pull, void* user)
{
    auto& state = device();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.streamUser.store(user, std::memory_order_release);
    state.streamPull.store(pull, std::memory_order_release);
}

void shutdown()
{
    auto& state = device();
    state.initialized.store(false);
    if (state.watchdog.joinable())
    {
        state.watchdogStop.store(true);
        state.watchdog.join();
        if (state.restarts)
            std::fprintf(stderr, "[audio] playback device restarts after host stalls: %llu\n",
                         (unsigned long long)state.restarts);
    }
    // Close joins the callback; never wait for it while holding its mutex.
    if (state.id)
        SDL_CloseAudioDevice(state.id);
    state.id = 0;
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        for (auto& sound : state.sounds)
            sound->active = false;
        state.sounds.clear();
    }
    if (state.ownsSubsystem)
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    state.ownsSubsystem = false;
}

SoundHandle createMonoSound(const int16_t* pcm, size_t frames, uint32_t sampleRate,
                            float pitch, float volume)
{
    if (!ready() || !pcm || !frames || !sampleRate ||
        frames > static_cast<size_t>(std::numeric_limits<int>::max()) / sizeof(int16_t))
        return {};
    const int outputRate = device().spec.freq;
    // The old sound API ignores nonpositive pitch, retaining 1.0. Pitch is
    // rate conversion, so duration changes with pitch; samples never loop.
    if (!(pitch > 0.0f) || !std::isfinite(pitch))
        pitch = 1.0f;
    const double pitchedRate = static_cast<double>(sampleRate) * pitch;
    if (pitchedRate < 1 || pitchedRate > std::numeric_limits<int>::max())
        return {};
    SDL_AudioCVT conversion{};
    if (SDL_BuildAudioCVT(&conversion, AUDIO_S16SYS, 1, static_cast<int>(pitchedRate),
                          AUDIO_F32SYS, 2, outputRate) < 0)
        return {};
    conversion.len = static_cast<int>(frames * sizeof(int16_t));
    if (conversion.len_mult <= 0 || conversion.len > std::numeric_limits<int>::max() / conversion.len_mult)
        return {};
    std::vector<Uint8> buffer(static_cast<size_t>(conversion.len) * conversion.len_mult);
    conversion.buf = buffer.data();
    std::memcpy(conversion.buf, pcm, conversion.len);
    if (SDL_ConvertAudio(&conversion) != 0 || conversion.len_cvt <= 0)
        return {};
    auto sound = std::make_shared<Sound>();
    sound->stereo.resize(static_cast<size_t>(conversion.len_cvt) / sizeof(float));
    std::memcpy(sound->stereo.data(), conversion.buf, conversion.len_cvt);
    // Preserve the producer host's default centered pan law (0.6875/channel).
    sound->gain = volume * 0.6875f;
    return sound;
}

void play(const SoundHandle& sound)
{
    if (!sound)
        return;
    auto& state = device();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.initialized.load())
        return;
    if (!sound->active)
        state.sounds.push_back(sound);
    sound->cursor = 0;
    sound->active = true;
}

bool playing(const SoundHandle& sound)
{
    std::lock_guard<std::mutex> lock(device().mutex);
    return sound && sound->active;
}

void stop(const SoundHandle& sound)
{
    auto& state = device();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (sound)
        sound->active = false;
    std::erase(state.sounds, sound);
}

#if defined(RRV_SDL_AUDIO_TESTING)
void pauseForTesting() { SDL_PauseAudioDevice(device().id, 1); }
void mixForTesting(float* stereo, size_t frames) { mix(stereo, frames * 2); }
int sampleRateForTesting() { return device().spec.freq; }
#endif
}
