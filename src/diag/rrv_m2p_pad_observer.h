#pragma once

#include <cstdint>

// Diagnostic-only host-to-libpad receipt.  The normal product compiles this
// header as no-ops; the M2P build enables the fixed-capacity in-memory rings.
// SDL values remain at the host edge as opaque numeric event identifiers.
namespace rrv::m2ppad
{
enum class SelectedSource : uint8_t
{
    None = 0,
    Keyboard = 1,
    Controller = 2,
    Fixture = 3,
};

#if defined(RRV_M2P_PAD_OBSERVER)
void initializeFromEnvironment();
// The SDL pad owner calls this only after PS2Runtime has joined, making ring
// export quiescent. Abnormal process exit deliberately emits no receipt.
void finalize();
void hostSample(uint64_t sampleId, uint64_t eventOrdinal, uint32_t eventType,
                uint32_t keyboardButtons, uint32_t controllerButtons,
                SelectedSource selected, uint32_t mergedButtons, bool connected);
void guestPacket(uint64_t sampleId, uint64_t pollOrdinal, uint16_t activeLowButtons,
                 uint8_t byte2, uint8_t byte3, bool overrideActive);
#else
inline void initializeFromEnvironment() {}
inline void finalize() {}
inline void hostSample(uint64_t, uint64_t, uint32_t, uint32_t, uint32_t,
                       SelectedSource, uint32_t, bool) {}
inline void guestPacket(uint64_t, uint64_t, uint16_t, uint8_t, uint8_t, bool) {}
#endif
} // namespace rrv::m2ppad
