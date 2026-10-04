#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace rrv::audio
{
// The product's existing mono PCM sound boundary. Guest decoding, sample
// selection and voice policy remain in PS2AudioBackend.
struct Sound;
using SoundHandle = std::shared_ptr<Sound>;

void initialize();
bool ready();
void shutdown();
SoundHandle createMonoSound(const int16_t* pcm, size_t frames, uint32_t sampleRate,
                            float pitch, float volume);
void play(const SoundHandle& sound);
bool playing(const SoundHandle& sound);
void stop(const SoundHandle& sound);

// Gate 8: a 48 kHz interleaved-stereo float source pulled from the SDL
// callback thread (the SPU2 output ring). The pull must never block; it
// returns the frames of real audio it wrote and leaves the rest as silence.
using StreamPull = size_t (*)(void* user, float* stereo, size_t frames);
void setStreamSource(StreamPull pull, void* user);

#if defined(RRV_SDL_AUDIO_TESTING)
// Exercise the actual callback mixer deterministically with the dummy device
// paused; production builds expose no capture or alternate device controls.
void pauseForTesting();
void mixForTesting(float* stereo, size_t frames);
int sampleRateForTesting();
#endif
}
