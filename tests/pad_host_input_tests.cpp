// Asset-free conformance gate for the portable HostPadBackend -> libpad path.
// It deliberately exercises scePad* calls and the guest's 32-byte packet,
// rather than duplicating the packet conversion logic in a unit-test helper.

#include "ps2_runtime.h"
#include "runtime/host_pad.h"
#include "Stubs/Pad.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
constexpr uint32_t kDmaAddress = 0x1000u;
constexpr uint32_t kPacketAddress = 0x1100u;
constexpr int32_t kPadStateDisconnected = 0;
constexpr int32_t kPadStateStable = 6;
constexpr uint8_t kPadModeDigital = 0x41u;
constexpr uint8_t kPadModeDualShock = 0x73u;
constexpr uint8_t kPadModeDualShockPressure = 0x79u;
#if defined(RRV_PAD_PRESSURE_FIX) && RRV_PAD_PRESSURE_FIX
constexpr unsigned kL2PressureByte = 18;
constexpr uint32_t kFullPressureMask = 0x0fffu;
constexpr uint32_t kCrossPressureBit = 1u << 6;
#else
// The frozen diagnostic/oracle graph uses the original producer without the
// product pressure overlay. Preserve its historical packet expectations.
constexpr unsigned kL2PressureByte = 17;
constexpr uint32_t kFullPressureMask = 0xffffu;
constexpr uint32_t kCrossPressureBit = HostPadButtonCross;
#endif

int g_failures = 0;

class ScopedEnvironment final
{
public:
    explicit ScopedEnvironment(const char *name) : m_name(name)
    {
        if (const char *value = std::getenv(name))
        {
            m_hadValue = true;
            m_value = value;
        }
    }

    ~ScopedEnvironment()
    {
        if (m_hadValue)
            setenv(m_name, m_value.c_str(), 1);
        else
            unsetenv(m_name);
    }

    void set(const char *value) { setenv(m_name, value, 1); }

private:
    const char *m_name;
    bool m_hadValue = false;
    std::string m_value;
};

void check(bool condition, const char *what)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

template <typename T>
void checkEqual(T actual, T expected, const char *what)
{
    if (actual != expected)
    {
        std::fprintf(stderr, "FAIL: %s (got %lld, expected %lld)\n", what,
                     static_cast<long long>(actual), static_cast<long long>(expected));
        ++g_failures;
    }
}

void setGpr(R5900Context &ctx, int reg, uint32_t value)
{
    ctx.r[reg] = _mm_cvtsi64_si128(static_cast<int64_t>(static_cast<int32_t>(value)));
}

uint32_t returnValue(const R5900Context &ctx)
{
    return static_cast<uint32_t>(_mm_extract_epi64(ctx.r[2], 0));
}

using PadCall = void (*)(uint8_t *, R5900Context *, PS2Runtime *);

uint32_t call(PadCall fn, uint8_t *ram, PS2Runtime &runtime, uint32_t a0 = 0,
              uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0)
{
    R5900Context ctx{};
    setGpr(ctx, 4, a0);
    setGpr(ctx, 5, a1);
    setGpr(ctx, 6, a2);
    setGpr(ctx, 7, a3);
    fn(ram, &ctx, &runtime);
    return returnValue(ctx);
}

class FakeHostPadBackend final : public HostPadBackend
{
public:
    HostPadState state{};
    HostPadCapabilities caps{true, true, false};

    HostPadState snapshot(unsigned port) override
    {
        return port == 0u ? state : HostPadState{};
    }

    HostPadCapabilities capabilities(unsigned port) const override
    {
        return port == 0u ? caps : HostPadCapabilities{};
    }
};

class PadHarness
{
public:
    PadHarness()
        : ram(PS2_RAM_SIZE), backend(std::make_shared<FakeHostPadBackend>())
    {
        // Construct the runtime only after setting this option: the backend
        // parses it during construction.  0.20 is deliberately a test value,
        // not a suggested device default.
        setenv("RRV_PAD_DEADZONE", "0.20", 1);
        m_runtime = std::make_unique<PS2Runtime>();
        runtime().padBackend().setBackendForTesting(backend);
        checkEqual(call(ps2_stubs::scePadInit, ram.data(), runtime()), 1u,
                   "scePadInit succeeds");
        checkEqual(call(ps2_stubs::scePadPortOpen, ram.data(), runtime(), 0, 0, kDmaAddress),
                   1u, "scePadPortOpen succeeds");
    }

    ~PadHarness()
    {
        runtime().padBackend().clearBackendForTesting();
        m_runtime.reset();
        unsetenv("RRV_PAD_DEADZONE");
    }

    std::array<uint8_t, 32> read()
    {
        std::memset(ram.data() + kPacketAddress, 0xa5, 32);
        checkEqual(call(ps2_stubs::scePadRead, ram.data(), runtime(), 0, 0, kPacketAddress),
                   1u, "scePadRead succeeds for an open port");
        std::array<uint8_t, 32> packet{};
        std::memcpy(packet.data(), ram.data() + kPacketAddress, packet.size());
        return packet;
    }

    int32_t state()
    {
        return static_cast<int32_t>(call(ps2_stubs::scePadGetState, ram.data(), runtime()));
    }

    PS2Runtime &runtime() { return *m_runtime; }

    std::vector<uint8_t> ram;
    std::shared_ptr<FakeHostPadBackend> backend;

private:
    std::unique_ptr<PS2Runtime> m_runtime;
};

void checkNeutralPacket(const std::array<uint8_t, 32> &packet, const char *what)
{
    checkEqual(packet[1], kPadModeDualShock, what);
    checkEqual(packet[2], uint8_t{0xff}, "neutral packet low active-low buttons");
    checkEqual(packet[3], uint8_t{0xff}, "neutral packet high active-low buttons");
    for (unsigned index = 4; index <= 7; ++index)
        checkEqual(packet[index], uint8_t{128}, "neutral packet axis center");
    for (unsigned index = 8; index < packet.size(); ++index)
        checkEqual(packet[index], uint8_t{0}, "neutral packet clears unused/pressure bytes");
}

void testConnectionAndReconnect(PadHarness &harness)
{
    // An emulated port can be open yet physically disconnected.  It remains
    // readable (neutral data) but is not STABLE; closing it makes read fail.
    checkEqual(harness.state(), kPadStateDisconnected,
               "open port with no controller is disconnected");
    checkNeutralPacket(harness.read(), "open disconnected packet remains analog");

    harness.backend->state.connected = true;
    harness.backend->state.deviceId = 0x1234u;
    checkEqual(harness.state(), kPadStateStable, "connected port is stable");

    harness.backend->state.pressedButtons = HostPadButtonStart;
    auto packet = harness.read();
    check((packet[2] & (1u << 3)) == 0u, "connected fake reaches scePadRead");

    harness.backend->state = {};
    checkEqual(harness.state(), kPadStateDisconnected, "disconnect changes state without restart");
    checkNeutralPacket(harness.read(), "disconnect returns neutral packet");

    harness.backend->state.connected = true;
    harness.backend->state.deviceId = 0x5678u;
    harness.backend->state.pressedButtons = HostPadButtonCross;
    checkEqual(harness.state(), kPadStateStable, "reconnect returns to stable");
    packet = harness.read();
    check((packet[3] & (1u << 6)) == 0u, "reconnected fake reaches guest packet");
}

void testButtonsAxesAndDeadzone(PadHarness &harness)
{
    auto &state = harness.backend->state;
    state = {};
    state.connected = true;
    // A generic controller that only exposes digital shoulder triggers must
    // retain the active-high L2/R2 button bits through the shared seam.
    harness.backend->caps.analogTriggers = false;
    state.pressedButtons = 0xffffu;
    const auto allPressed = harness.read();
    checkEqual(allPressed[0], uint8_t{0}, "packet byte zero is deterministic");
    checkEqual(allPressed[1], kPadModeDualShock, "analog mode marker");
    checkEqual(allPressed[2], uint8_t{0}, "all host low button bits become active-low zero");
    checkEqual(allPressed[3], uint8_t{0}, "all host high button bits become active-low zero");
    for (unsigned index = 20; index < allPressed.size(); ++index)
        checkEqual(allPressed[index], uint8_t{0}, "scePadRead writes the entire 32-byte packet");

    state.pressedButtons = 0;
    state.leftX = state.leftY = state.rightX = state.rightY = 0.0f;
    auto packet = harness.read();
    for (unsigned index = 4; index <= 7; ++index)
        checkEqual(packet[index], uint8_t{128}, "stick center converts exactly to 128");

    state.rightX = -1.0f;
    state.leftX = 1.0f;
    state.rightY = state.leftY = 0.0f;
    packet = harness.read();
    checkEqual(packet[4], uint8_t{0}, "right stick full left reaches byte zero");
    checkEqual(packet[6], uint8_t{255}, "left stick full right reaches byte 255");

    state.rightX = state.leftX = 0.0f;
    state.rightY = 1.0f; // host positive-up maps to DualShock positive-down byte 0
    state.leftY = -1.0f; // host down maps to byte 255
    packet = harness.read();
    checkEqual(packet[5], uint8_t{0}, "positive-up right stick reaches byte zero");
    checkEqual(packet[7], uint8_t{255}, "negative-up left stick reaches byte 255");

    state.leftX = 0.10f;
    state.leftY = 0.10f;
    state.rightX = state.rightY = 0.0f;
    packet = harness.read();
    checkEqual(packet[6], uint8_t{128}, "radial deadzone removes left-stick X drift");
    checkEqual(packet[7], uint8_t{128}, "radial deadzone removes left-stick Y drift");

    // A diagonal vector can be outside the radial deadzone even when each
    // component is below it.  Both components must survive symmetrically;
    // per-axis deadzone handling would incorrectly keep this at center.
    state.leftX = 0.15f;
    state.leftY = 0.15f;
    packet = harness.read();
    check(packet[6] != uint8_t{128}, "radial deadzone preserves diagonal X input");
    check(packet[7] != uint8_t{128}, "radial deadzone preserves diagonal Y input");
    check(std::abs(std::abs(static_cast<int>(packet[6]) - 128) -
                   std::abs(static_cast<int>(packet[7]) - 128)) <= 1,
          "radial deadzone preserves diagonal symmetry");

    state.leftX = 0.60f;
    state.leftY = 0.0f;
    packet = harness.read();
    // (0.60 - 0.20) / (1.00 - 0.20) = 0.50, then 0.50 maps to 191.25 -> 191.
    checkEqual(packet[6], uint8_t{191}, "radial deadzone rescales remaining stick travel");

    state.leftX = 1.0f;
    packet = harness.read();
    checkEqual(packet[6], uint8_t{255}, "deadzone rescale retains full endpoint");
}

void testDirectionalPressReleasePackets(PadHarness &harness)
{
    auto &state = harness.backend->state;
    state = {};
    state.connected = true;

    const auto direction = [&](uint32_t button, uint8_t bit, const char *name) {
        state.pressedButtons = 0u;
        check((harness.read()[2] & (1u << bit)) != 0u, "neutral direction is released");
        state.pressedButtons = button;
        auto packet = harness.read();
        check((packet[2] & (1u << bit)) == 0u, name);
        packet = harness.read();
        check((packet[2] & (1u << bit)) == 0u, "held direction remains pressed");
        state.pressedButtons = 0u;
        packet = harness.read();
        check((packet[2] & (1u << bit)) != 0u, "released direction reaches next guest packet");
    };

    // Required M2P packet sequence: neutral, Down press/hold/release,
    // neutral, then Up/Left/Right press-release.  This is intentionally
    // packet-level, rather than inferring state from menu motion.
    direction(HostPadButtonDown, 6u, "Down press reaches active-low guest packet");
    direction(HostPadButtonUp, 4u, "Up press reaches active-low guest packet");
    direction(HostPadButtonLeft, 7u, "Left press reaches active-low guest packet");
    direction(HostPadButtonRight, 5u, "Right press reaches active-low guest packet");

    state.pressedButtons = HostPadButtonUp | HostPadButtonLeft;
    auto packet = harness.read();
    check((packet[2] & (1u << 4)) == 0u && (packet[2] & (1u << 7)) == 0u,
          "simultaneous directions retain both active-low bits");
    state.pressedButtons = HostPadButtonUp | HostPadButtonDown;
    packet = harness.read();
    check((packet[2] & (1u << 4)) == 0u && (packet[2] & (1u << 6)) == 0u,
          "opposite directions preserve source state without hidden cancellation");
    state.pressedButtons = 0u;
    checkNeutralPacket(harness.read(), "all released directions return to neutral packet");
}

void testModesPressureAndHysteresis(PadHarness &harness)
{
    auto &state = harness.backend->state;
    state = {};
    state.connected = true;
    harness.backend->caps.analogTriggers = true;
    state.leftX = 1.0f;
    state.rightX = -1.0f;
    state.leftTrigger = 0.25f;
    state.rightTrigger = 0.75f;

    checkEqual(call(ps2_stubs::scePadSetMainMode, harness.ram.data(), harness.runtime(), 0, 0, 0),
               1u, "scePadSetMainMode digital succeeds");
    auto packet = harness.read();
    checkEqual(packet[1], kPadModeDigital, "digital mode packet marker");
    for (unsigned index = 4; index <= 7; ++index)
        checkEqual(packet[index], uint8_t{128}, "digital mode omits analog axis payload");
    checkEqual(packet[kL2PressureByte], uint8_t{0}, "digital mode omits trigger pressure payload");
    checkEqual(packet[19], uint8_t{0}, "digital mode omits trigger pressure payload");

    checkEqual(call(ps2_stubs::scePadSetMainMode, harness.ram.data(), harness.runtime(), 0, 0, 1),
               1u, "scePadSetMainMode analog succeeds");
    checkEqual(call(ps2_stubs::scePadEnterPressMode, harness.ram.data(), harness.runtime()), 1u,
               "scePadEnterPressMode succeeds");
    packet = harness.read();
    checkEqual(packet[1], kPadModeDualShockPressure, "pressure-enabled analog packet marker");
    checkEqual(packet[4], uint8_t{0}, "analog mode retains right-stick payload");
    checkEqual(packet[6], uint8_t{255}, "analog mode retains left-stick payload");
    checkEqual(packet[kL2PressureByte], uint8_t{64}, "left trigger emits analog pressure");
    checkEqual(packet[19], uint8_t{191}, "right trigger emits analog pressure");

    // L2 digital state has hysteresis: it presses at 0.50, remains held above
    // the 0.40 release threshold, and releases below it.  Pressure remains
    // analog throughout, independent of the digital bit.
    state.leftTrigger = 0.49f;
    packet = harness.read();
    check((packet[3] & (1u << 0)) != 0u, "L2 stays released below press threshold");
    state.leftTrigger = 0.50f;
    packet = harness.read();
    check((packet[3] & (1u << 0)) == 0u, "L2 presses at 0.50 threshold");
    state.leftTrigger = 0.40f;
    packet = harness.read();
    check((packet[3] & (1u << 0)) == 0u, "L2 remains held at release threshold");
    state.leftTrigger = 0.39f;
    packet = harness.read();
    check((packet[3] & (1u << 0)) != 0u, "L2 releases below 0.40 threshold");

    // Digital face buttons remain full pressure when pressure mode is active.
    state.leftTrigger = 0.0f;
    state.rightTrigger = 0.0f;
    harness.backend->caps.analogTriggers = false;
    state.pressedButtons = HostPadButtonCross | HostPadButtonL2;
    checkEqual(call(ps2_stubs::scePadSetButtonInfo, harness.ram.data(), harness.runtime(),
                    0, 0, kFullPressureMask),
               1u, "full pressure mask succeeds");
    packet = harness.read();
    checkEqual(packet[14], uint8_t{255}, "pressed Cross retains full digital pressure");
    checkEqual(packet[kL2PressureByte], uint8_t{255}, "digital L2 retains full pressure compatibility");

    checkEqual(call(ps2_stubs::scePadSetButtonInfo, harness.ram.data(), harness.runtime(),
                    0, 0, kFullPressureMask & ~kCrossPressureBit),
               1u, "pressure mask update succeeds");
    packet = harness.read();
    checkEqual(packet[14], uint8_t{0}, "pressure mask suppresses disabled Cross pressure");
    checkEqual(packet[kL2PressureByte], uint8_t{255}, "pressure mask preserves enabled L2 pressure");
}

#if defined(RRV_PAD_PRESSURE_FIX) && RRV_PAD_PRESSURE_FIX
void testSetButtonInfoNegotiation()
{
    PadHarness harness;
    auto &state = harness.backend->state;
    state.connected = true;
    harness.backend->caps = {false, false, false}; // keyboard-style digital source
    checkEqual(harness.read()[1], kPadModeDualShock, "fresh port starts in analog mode");

    // RR5 uses its generated EnterPressMode wrapper, which calls this lower
    // API directly. Calling the HLE EnterPressMode first hid PAD-001.
    checkEqual(call(ps2_stubs::scePadSetButtonInfo, harness.ram.data(), harness.runtime(),
                    0, 0, 0x0fffu), 1u, "RR5 pressure request succeeds from fresh analog mode");
    checkEqual(harness.read()[1], kPadModeDualShockPressure,
               "SetButtonInfo alone negotiates the 0x79 packet RR5 expects");

    constexpr uint32_t pressureButtons[] = {
        HostPadButtonRight, HostPadButtonLeft, HostPadButtonUp, HostPadButtonDown,
        HostPadButtonTriangle, HostPadButtonCircle, HostPadButtonCross, HostPadButtonSquare,
        HostPadButtonL1, HostPadButtonR1, HostPadButtonL2, HostPadButtonR2};
    for (unsigned pressed = 0; pressed < 12; ++pressed)
    {
        state.pressedButtons = pressureButtons[pressed];
        const auto packet = harness.read();
        for (unsigned index = 0; index < 12; ++index)
            checkEqual(packet[8 + index], static_cast<uint8_t>(index == pressed ? 255 : 0),
                       "full response mask emits each pressure button in PS2 packet order");
    }

    // Read the same snapshot repeatedly, as multiple guest reads can occur
    // between host updates. These must remain levels: the guest owns edges
    // and held-repeat timing. Check both keyboard-style and gamepad-style
    // capabilities through the same public host seam.
    for (const bool gamepad : {false, true})
    {
        harness.backend->caps = {gamepad, gamepad, false};
        for (const uint32_t button : {HostPadButtonDown, HostPadButtonUp, HostPadButtonLeft,
                                     HostPadButtonRight, HostPadButtonStart, HostPadButtonCross})
        {
            uint16_t previous = 0;
            unsigned pressEdges = 0;
            unsigned releaseEdges = 0;
            // A one-poll tap followed by a sixty-poll hold is two presses.
            for (unsigned poll = 0; poll < 70; ++poll)
            {
                const bool held = poll == 1 || (poll >= 5 && poll <= 64);
                state.pressedButtons = held ? button : 0u;
                const auto packet = harness.read();
                checkEqual(packet[1], kPadModeDualShockPressure,
                           "pressure mode remains stable across press hold and release");
                const uint16_t current = static_cast<uint16_t>(
                    ~(static_cast<uint16_t>(packet[2]) | (static_cast<uint16_t>(packet[3]) << 8)));
                checkEqual(current, static_cast<uint16_t>(held ? button : 0u),
                           "keyboard and gamepad snapshots preserve button levels across repeated reads");
                pressEdges += (current & ~previous & button) != 0u;
                releaseEdges += (previous & ~current & button) != 0u;
                previous = current;
            }
            checkEqual(pressEdges, 2u, "tap and hold each produce one guest-derived press edge");
            checkEqual(releaseEdges, 2u, "tap and hold each produce one guest-derived release edge");
        }
    }

    state.pressedButtons = HostPadButtonDown;
    const auto enabled = harness.read();
    checkEqual(call(ps2_stubs::scePadSetButtonInfo, harness.ram.data(), harness.runtime(),
                    0, 0, 0u), 1u, "zero response mask exits pressure mode");
    const auto disabled = harness.read();
    checkEqual(disabled[1], kPadModeDualShock, "zero response mask restores 0x73 analog packet");
    checkEqual(disabled[2], enabled[2], "exiting pressure mode preserves held digital buttons");
    for (unsigned index = 8; index < 20; ++index)
        checkEqual(disabled[index], uint8_t{0}, "zero response mask clears every pressure byte");

    checkEqual(call(ps2_stubs::scePadEnterPressMode, harness.ram.data(), harness.runtime()),
               1u, "direct EnterPressMode succeeds after zero response mask");
    check(harness.read() == enabled, "direct EnterPressMode matches SetButtonInfo full mask");
    checkEqual(call(ps2_stubs::scePadExitPressMode, harness.ram.data(), harness.runtime()),
               1u, "direct ExitPressMode succeeds");
    check(harness.read() == disabled, "direct ExitPressMode matches SetButtonInfo zero mask");
}
#endif

void testClosedPort(PadHarness &harness)
{
    checkEqual(call(ps2_stubs::scePadPortClose, harness.ram.data(), harness.runtime()), 1u,
               "scePadPortClose succeeds");
    checkEqual(harness.state(), kPadStateDisconnected, "closed port is disconnected");
    checkEqual(call(ps2_stubs::scePadRead, harness.ram.data(), harness.runtime(), 0, 0, kPacketAddress),
               0u, "scePadRead fails for closed port");
}

void testNeutralPadMode()
{
    ScopedEnvironment neutralMode("RRV_TEST_NEUTRAL_PAD");
    ScopedEnvironment forceStart("RRV_TEST_FORCE_START");
    neutralMode.set("1");
    forceStart.set("1");
    {
        PadHarness harness;
        harness.backend->state.connected = true;
        harness.backend->state.pressedButtons = 0xffffu;
        harness.backend->state.leftX = harness.backend->state.leftY = 1.0f;
        harness.backend->state.rightX = harness.backend->state.rightY = -1.0f;
        harness.backend->state.leftTrigger = harness.backend->state.rightTrigger = 1.0f;

        const HostPadState snapshot = harness.runtime().padBackend().snapshot(0);
        const HostPadCapabilities capabilities = harness.runtime().padBackend().capabilities(0);
        check(snapshot.connected, "neutral test pad reports a connected logical pad");
        checkEqual(snapshot.deviceId, UINT64_C(0x5252564e45555452), "neutral test pad identity is fixed");
        checkEqual(snapshot.pressedButtons, 0u, "neutral test pad suppresses host buttons");
        checkEqual(snapshot.leftX, 0.0f, "neutral test pad suppresses host left-stick X");
        checkEqual(snapshot.rightY, 0.0f, "neutral test pad suppresses host right-stick Y");
        check(capabilities.analogSticks && capabilities.analogTriggers,
              "neutral test pad has deterministic analog capabilities");
        checkEqual(harness.state(), kPadStateStable, "neutral test pad keeps the guest port stable");

        ps2_stubs::setPadOverrideState(0u, 255u, 255u, 0u, 0u);
        const auto packet = harness.read();
        checkNeutralPacket(packet, "neutral mode suppresses explicit packet override");
        check((packet[2] & (1u << 3)) != 0u,
              "neutral mode suppresses RRV_TEST_FORCE_START");
        ps2_stubs::clearPadOverrideState();
    }
}

void testNeutralPadExplicitlyDisabled()
{
    ScopedEnvironment neutralMode("RRV_TEST_NEUTRAL_PAD");
    neutralMode.set("0");
    PadHarness harness;
    harness.backend->state.connected = true;
    harness.backend->state.deviceId = 0x4321u;
    harness.backend->state.pressedButtons = HostPadButtonCross;
    const HostPadState snapshot = harness.runtime().padBackend().snapshot(0);
    const auto packet = harness.read();
    check(!harness.runtime().padBackend().neutralTestEnabled(), "zero disables neutral-pad mode");
    checkEqual(snapshot.deviceId, UINT64_C(0x4321), "zero retains hostile fake device identity");
    check((packet[3] & (1u << 6)) == 0u, "zero retains hostile fake button input");
}

void testInvalidNeutralPadMode()
{
    ScopedEnvironment neutralMode("RRV_TEST_NEUTRAL_PAD");
    neutralMode.set("invalid");
    bool rejected = false;
    try
    {
        auto runtime = std::make_unique<PS2Runtime>();
        (void)runtime;
    }
    catch (const std::invalid_argument &)
    {
        rejected = true;
    }
    check(rejected, "invalid neutral-pad opt-in fails during runtime startup");
}

} // namespace

int main()
{
    testNeutralPadMode();
#if defined(RRV_PAD_PRESSURE_FIX) && RRV_PAD_PRESSURE_FIX
    testSetButtonInfoNegotiation();
#endif
    PadHarness harness;
    testConnectionAndReconnect(harness);
    testButtonsAxesAndDeadzone(harness);
    testDirectionalPressReleasePackets(harness);
    testModesPressureAndHysteresis(harness);
    testClosedPort(harness);
    testNeutralPadExplicitlyDisabled();
    testInvalidNeutralPadMode();
    return g_failures == 0 ? 0 : 1;
}
