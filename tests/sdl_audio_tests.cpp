#include "rrv_sdl_audio.h"
#include <SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#define CHECK(condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); std::exit(1); } } while (false)

int main(int argc, char** argv)
{
    using namespace rrv::audio;
    CHECK(!ready());
    shutdown();
    if (argc > 1 && std::strcmp(argv[1], "--failure") == 0)
    {
        initialize();
        CHECK(!ready());
        const int16_t pcm = 42;
        CHECK(!createMonoSound(&pcm, 1, 48000, 1, 1));
        shutdown();
        CHECK((SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) == 0);
        std::puts("SDL unavailable device stays not ready, cleanup succeeds");
        return 0;
    }
    // Audio owns exactly its own SDL subsystem reference; the existing host's
    // event/window ownership and an independent audio owner survive shutdown.
    CHECK(SDL_InitSubSystem(SDL_INIT_EVENTS | SDL_INIT_AUDIO) == 0);
    initialize();
    CHECK(ready());
    pauseForTesting();
    initialize(); // no duplicate SDL reference/device
    const int rate = sampleRateForTesting();
    CHECK(rate > 0);
    std::vector<float> out(128, 123.0f);
    mixForTesting(out.data(), 64);
    CHECK(std::all_of(out.begin(), out.end(), [](float x) { return x == 0; }));

    const std::vector<int16_t> pcm(64, 16384);
    auto first = createMonoSound(pcm.data(), pcm.size(), rate, 1, 1);
    CHECK(first && !playing(first));
    play(first);
    CHECK(playing(first));
    mixForTesting(out.data(), 16);
    for (size_t i = 0; i < 32; ++i)
        CHECK(std::abs(out[i] - 0.34375f) < 0.00001f);
    auto second = createMonoSound(pcm.data(), pcm.size(), rate, 1, 0.5f);
    play(second);
    mixForTesting(out.data(), 16);
    for (size_t i = 0; i < 32; ++i)
        CHECK(std::abs(out[i] - 0.515625f) < 0.00001f);
    stop(first);
    CHECK(!playing(first) && playing(second));
    mixForTesting(out.data(), 64);
    CHECK(!playing(second));
    for (size_t i = 0; i < 96; ++i)
        CHECK(std::abs(out[i] - 0.171875f) < 0.00001f);
    for (size_t i = 96; i < 128; ++i)
        CHECK(out[i] == 0);
    mixForTesting(out.data(), 64);
    CHECK(std::all_of(out.begin(), out.end(), [](float x) { return x == 0; }));

    // Rate conversion changes duration and ends naturally; no implicit loops.
    const std::vector<int16_t> longPcm(4096, 16384);
    auto duration = [&](float pitch) {
        auto sound = createMonoSound(longPcm.data(), longPcm.size(), rate, pitch, 1);
        CHECK(sound);
        play(sound);
        size_t frames = 0;
        while (playing(sound) && frames < 20000)
        {
            mixForTesting(out.data(), 1);
            ++frames;
        }
        CHECK(!playing(sound));
        return frames;
    };
    CHECK(duration(1) == 4096);
    CHECK(duration(2) == 2048);
    CHECK(duration(0.5f) == 8192);
    CHECK(duration(0) == 4096);
    CHECK(!createMonoSound(nullptr, 64, rate, 1, 1));
    CHECK(!createMonoSound(pcm.data(), 0, rate, 1, 1));
    CHECK(!createMonoSound(pcm.data(), 64, 0, 1, 1));

    play(first);
    shutdown();
    CHECK(!ready() && !playing(first));
    stop(first); // retained handles remain safe after device closure
    CHECK((SDL_WasInit(SDL_INIT_EVENTS) & SDL_INIT_EVENTS) != 0);
    CHECK((SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) != 0);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    CHECK((SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) == 0);
    initialize();
    CHECK(ready());
    shutdown();
    CHECK((SDL_WasInit(SDL_INIT_EVENTS) & SDL_INIT_EVENTS) != 0);
    SDL_QuitSubSystem(SDL_INIT_EVENTS);
    std::puts("SDL audio lifecycle, silence, mixing, volume, pitch and completion passed");
}
