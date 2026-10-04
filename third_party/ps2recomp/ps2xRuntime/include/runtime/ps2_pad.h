#ifndef PS2_PAD_H
#define PS2_PAD_H

#include <cstddef>
#include <cstdint>
#include <memory>

#include "runtime/host_pad.h"

class PSPadBackend
{
public:
    PSPadBackend();
    ~PSPadBackend();

    HostPadState snapshot(int port = 0);
    HostPadCapabilities capabilities(int port = 0) const;
    bool isConnected(int port = 0);
    // RRV_TEST_NEUTRAL_PAD is an explicit test-only deterministic input mode.
    // It is queried by libpad too, so it suppresses other test injection
    // helpers for a comparison run.
    bool neutralTestEnabled() const;
    float deadzone() const;
    bool diagnosticsEnabled() const;
    // DualShock 2 vibration (scripts/pad_rumble_overlay.py): forwarded to the
    // backend that supplies samples; ignored by the neutral test pad.
    void setRumble(int port, HostPadRumble rumble);

    // Product hosts set this before PS2Runtime is constructed. It prevents the
    // legacy raylib/GameController owners from being constructed for a direct
    // host; RRV_TEST_NEUTRAL_PAD still has higher priority.
    static void setExternalBackendForProcess(std::shared_ptr<HostPadBackend> backend);
    // Tests can install a fake backend without a window or physical controller.
    void setBackendForTesting(std::shared_ptr<HostPadBackend> backend);
    void clearBackendForTesting();

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

// Retains the existing temporary RRV_TEST_FORCE_START behavior while keeping
// packet construction in libpad.
bool ps2PadForceStartActive();

#endif
