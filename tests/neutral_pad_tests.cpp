// Test-only contract for RRV_TEST_NEUTRAL_PAD.  This deliberately constructs
// only the public libpad backend so it remains asset-free and independent of
// GS/window initialization.

#include "runtime/host_pad.h"
#include "runtime/ps2_pad.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>

namespace
{
int failures = 0;

void check(bool condition, const char *message)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

class ActiveHostPad final : public HostPadBackend
{
public:
    HostPadState snapshot(unsigned port) override
    {
        if (port != 0u)
            return {};
        HostPadState state{};
        state.connected = true;
        state.deviceId = 0x1234u;
        state.pressedButtons = 0xffffu;
        state.leftX = state.leftY = state.rightX = state.rightY = 1.0f;
        state.leftTrigger = state.rightTrigger = 1.0f;
        return state;
    }

    HostPadCapabilities capabilities(unsigned port) const override
    {
        return port == 0u ? HostPadCapabilities{false, false, true} : HostPadCapabilities{};
    }
};

void testNeutralMode()
{
    setenv("RRV_TEST_NEUTRAL_PAD", "1", 1);
    {
        PSPadBackend backend;
        backend.setBackendForTesting(std::make_shared<ActiveHostPad>());
        const HostPadState state = backend.snapshot(0);
        const HostPadCapabilities caps = backend.capabilities(0);
        check(backend.neutralTestEnabled(), "neutral pad mode reports enabled");
        check(state.connected, "neutral pad mode keeps port zero connected");
        check(state.deviceId == 0x5252564e45555452ULL, "neutral pad identity is fixed");
        check(state.pressedButtons == 0u, "neutral pad suppresses host buttons");
        check(state.leftX == 0.0f && state.leftY == 0.0f &&
              state.rightX == 0.0f && state.rightY == 0.0f,
              "neutral pad suppresses host sticks");
        check(state.leftTrigger == 0.0f && state.rightTrigger == 0.0f,
              "neutral pad suppresses host triggers");
        check(caps.analogSticks && caps.analogTriggers && !caps.rumble,
              "neutral pad capabilities are fixed");
        check(!backend.snapshot(1).connected, "only port zero is connected");
    }
    unsetenv("RRV_TEST_NEUTRAL_PAD");
}

void testDisabledAndInvalidValues()
{
    setenv("RRV_TEST_NEUTRAL_PAD", "0", 1);
    {
        PSPadBackend backend;
        check(!backend.neutralTestEnabled(), "neutral pad mode is disabled by zero");
    }
    unsetenv("RRV_TEST_NEUTRAL_PAD");

    setenv("RRV_TEST_NEUTRAL_PAD", "invalid", 1);
    bool rejected = false;
    try
    {
        PSPadBackend backend;
    }
    catch (const std::invalid_argument &)
    {
        rejected = true;
    }
    check(rejected, "invalid neutral-pad value fails during backend startup");
    unsetenv("RRV_TEST_NEUTRAL_PAD");
}
} // namespace

int main()
{
    testNeutralMode();
    testDisabledAndInvalidValues();
    return failures == 0 ? 0 : 1;
}
