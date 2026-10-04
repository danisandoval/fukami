// rrv_m2_initial_state.h -- bounded pre-execution guest-state fingerprint.
//
// This diagnostic is intentionally one-shot and opt-in.  It observes a
// defined subset of guest architectural state after boot setup and before the
// game thread begins; it never changes guest state, timing, or scheduling.
#ifndef RRV_M2_INITIAL_STATE_H
#define RRV_M2_INITIAL_STATE_H

#include <cstddef>
#include <cstdint>

class PS2Runtime;
struct R5900Context;

namespace rrv::m2initial
{
constexpr uint32_t kSchemaVersion = 1u;
constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

// Small generic surface for the asset-free serialization test.  `name` and
// bytes are input only; neither pointer identity nor address enters a hash.
struct ByteRegion final
{
    const char *name;
    const uint8_t *bytes;
    uint64_t sizeBytes;
};

uint64_t fnv1a64ForTesting(const uint8_t *bytes, uint64_t sizeBytes) noexcept;
uint64_t fingerprintRegionSequenceForTesting(const ByteRegion *regions,
                                             uint32_t count) noexcept;

// Enabled only when RRV_M2_INITIAL_STATE_FINGERPRINT names an output file.
// The runtime overlay calls this once after HLE boot setup/argv/reset and
// before PS2Runtime::run starts game execution or its VSync worker.
bool observe(PS2Runtime *runtime, R5900Context *context) noexcept;
} // namespace rrv::m2initial

#endif
