// RRV product SDL host overlay
#include "runtime/ps2_pad.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>

namespace
{
bool envEnabled(const char *name)
{
    const char *value = std::getenv(name);
    return value && value[0] && std::strcmp(value, "0") != 0;
}

bool testNeutralPadEnabled()
{
    const char *value = std::getenv("RRV_TEST_NEUTRAL_PAD");
    if (!value || !value[0] || !std::strcmp(value, "0"))
        return false;
    if (!std::strcmp(value, "1"))
        return true;
    std::fprintf(stderr, "[pad] invalid RRV_TEST_NEUTRAL_PAD=%s; expected 0 or 1\n", value);
    throw std::invalid_argument("invalid RRV_TEST_NEUTRAL_PAD (expected 0 or 1)");
}

std::mutex g_externalBackendMutex;
std::shared_ptr<HostPadBackend> g_externalBackend;

std::shared_ptr<HostPadBackend> externalBackendForProcess()
{
    std::lock_guard<std::mutex> lock(g_externalBackendMutex);
    return g_externalBackend;
}

HostPadState neutralTestPadState(unsigned port)
{
    if (port != 0u)
        return {};
    HostPadState state{};
    state.connected = true;
    // Fixed logical-device identity, never derived from physical host input.
    state.deviceId = 0x5252564e45555452ULL; // "RRVNEUTR"
    return state;
}

constexpr HostPadCapabilities kNeutralTestPadCapabilities{true, true, false};

float envDeadzone()
{
    const char *value = std::getenv("RRV_PAD_DEADZONE");
    if (!value || !value[0])
        return 0.0f; // Do not choose an unmeasured device-specific default.
    char *end = nullptr;
    const float parsed = std::strtof(value, &end);
    return (end != value && std::isfinite(parsed)) ? std::clamp(parsed, 0.0f, 0.95f) : 0.0f;
}

enum class BackendMode { Auto, GameController, Raylib, Keyboard };
enum class AutoPhysicalBackend { None, GameController, Raylib };
BackendMode backendMode()
{
    const char *value = std::getenv("RRV_PAD_BACKEND");
    if (!value || !value[0] || !std::strcmp(value, "auto")) return BackendMode::Auto;
    if (!std::strcmp(value, "gamecontroller")) return BackendMode::GameController;
    if (!std::strcmp(value, "raylib")) return BackendMode::Raylib;
    if (!std::strcmp(value, "keyboard")) return BackendMode::Keyboard;
    std::fprintf(stderr, "[pad] unknown RRV_PAD_BACKEND=%s; using auto\n", value);
    return BackendMode::Auto;
}

const char *backendName(BackendMode mode)
{
    switch (mode) {
    case BackendMode::GameController: return "gamecontroller";
    case BackendMode::Raylib: return "raylib";
    case BackendMode::Keyboard: return "keyboard";
    default: return "auto";
    }
}

std::string describeButtons(uint32_t buttons)
{
    struct NamedButton { HostPadButton bit; const char *name; };
    static constexpr NamedButton kNames[] = {
        {HostPadButtonCross, "Cross"}, {HostPadButtonCircle, "Circle"},
        {HostPadButtonSquare, "Square"}, {HostPadButtonTriangle, "Triangle"},
        {HostPadButtonUp, "Up"}, {HostPadButtonDown, "Down"},
        {HostPadButtonLeft, "Left"}, {HostPadButtonRight, "Right"},
        {HostPadButtonL1, "L1"}, {HostPadButtonR1, "R1"},
        {HostPadButtonL2, "L2"}, {HostPadButtonR2, "R2"},
        {HostPadButtonStart, "Start"}, {HostPadButtonSelect, "Select"},
        {HostPadButtonL3, "L3"}, {HostPadButtonR3, "R3"},
    };
    std::string result;
    for (const NamedButton &named : kNames)
    {
        if ((buttons & static_cast<uint32_t>(named.bit)) == 0)
            continue;
        if (!result.empty()) result += ',';
        result += named.name;
    }
    return result.empty() ? "-" : result;
}
} // namespace

class PSPadBackend::Impl
{
public:
    Impl() : mode(backendMode()), neutralTest(testNeutralPadEnabled()), deadzoneValue(envDeadzone()), diagnostics(envEnabled("RRV_PAD_DIAG")), external(externalBackendForProcess())
    {
        if (neutralTest)
            std::fprintf(stderr, "[pad] RRV_TEST_NEUTRAL_PAD=1: deterministic connected neutral pad enabled; host input disabled\n");
        // SDL owns product input; absent injection remains disconnected.
        // Preserve the existing late-injection seam used by runtime tests.
        if (diagnostics)
            std::fprintf(stderr, "[pad] backend=%s deadzone=%.3f device=%s\n", backendName(mode), deadzoneValue,
                std::getenv("RRV_PAD_DEVICE") ? std::getenv("RRV_PAD_DEVICE") : "<auto>");
    }

    std::shared_ptr<HostPadBackend> configuredBackend() const
    {
        if (testing) return testing;
        if (mode == BackendMode::GameController) return gameController;
        if (mode == BackendMode::Raylib) return raylib;
        if (mode == BackendMode::Keyboard) return keyboard;
        return {};
    }

    std::shared_ptr<HostPadBackend> physicalBackend(AutoPhysicalBackend backend) const
    {
        switch (backend) {
        case AutoPhysicalBackend::GameController: return gameController;
        case AutoPhysicalBackend::Raylib: return raylib;
        default: return {};
        }
    }

    std::shared_ptr<HostPadBackend> selected() const
    {
        if (std::shared_ptr<HostPadBackend> configured = configuredBackend())
            return configured;
        // Auto owns the first physical backend it can use.  It remains owned
        // until that backend reports a disconnect; in particular, a delayed
        // GameController notification cannot steal a still-connected raylib
        // device.  Keyboard is a transient fallback and is never latched.
        if (autoPhysicalBackend == AutoPhysicalBackend::GameController)
            return gameController;
        if (autoPhysicalBackend == AutoPhysicalBackend::Raylib)
            return raylib;
        return keyboard;
    }

    std::shared_ptr<HostPadBackend> claimAutoPhysical(AutoPhysicalBackend candidate)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (autoPhysicalBackend == AutoPhysicalBackend::None)
            autoPhysicalBackend = candidate;
        return physicalBackend(autoPhysicalBackend);
    }

    void releaseAutoPhysical(const HostPadBackend *backend)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (physicalBackend(autoPhysicalBackend).get() == backend)
            autoPhysicalBackend = AutoPhysicalBackend::None;
    }

    HostPadState getSnapshot(unsigned port)
    {
        if (neutralTest)
            return neutralTestPadState(port);
        if (external)
            return external->snapshot(port);
        std::shared_ptr<HostPadBackend> backend;
        {
            std::lock_guard<std::mutex> lock(mutex);
            backend = selected();
        }
        HostPadState state = backend ? backend->snapshot(port) : HostPadState{};

        bool emittedDisconnectNeutral = false;
        if (!testing && mode == BackendMode::Auto &&
            (backend.get() == gameController.get() || backend.get() == raylib.get()))
        {
            if (!state.connected)
            {
                // Return one neutral disconnect sample before choosing again.
                // The next poll can then fall back to keyboard or latch a
                // compatible replacement controller without restarting.
                state = {};
                releaseAutoPhysical(backend.get());
                emittedDisconnectNeutral = true;
            }
        }

        if (!testing && mode == BackendMode::Auto && !emittedDisconnectNeutral && backend == keyboard)
        {
            // No physical backend is currently owned.  Probe in priority
            // order and claim exactly the backend that supplied the connected
            // sample.  Once claimed, selected() will not probe competitors
            // until this backend later disconnects.
            HostPadState gameControllerState = gameController ? gameController->snapshot(port) : HostPadState{};
            if (gameControllerState.connected)
            {
                backend = claimAutoPhysical(AutoPhysicalBackend::GameController);
                state = backend.get() == gameController.get() ? gameControllerState : backend->snapshot(port);
            }
            else
            {
                HostPadState raylibState = raylib ? raylib->snapshot(port) : HostPadState{};
                if (raylibState.connected)
                {
                    backend = claimAutoPhysical(AutoPhysicalBackend::Raylib);
                    state = backend.get() == raylib.get() ? raylibState : backend->snapshot(port);
                }
                else
                {
                    backend = keyboard;
                    state = backend ? backend->snapshot(port) : HostPadState{};
                }
            }
        }
        if (diagnostics) log(port, state, backend.get());
        return state;
    }

    HostPadCapabilities getCapabilities(unsigned port) const
    {
        if (neutralTest)
            return port == 0u ? kNeutralTestPadCapabilities : HostPadCapabilities{};
        if (external)
            return external->capabilities(port);
        std::shared_ptr<HostPadBackend> backend;
        { std::lock_guard<std::mutex> lock(mutex); backend = selected(); }
        return backend ? backend->capabilities(port) : HostPadCapabilities{};
    }

    void log(unsigned port, const HostPadState &state, const HostPadBackend *backend)
    {
        if (port >= diagnosticPorts.size())
            return;
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(diagMutex);
        DiagnosticPort &previous = diagnosticPorts[port];
        const bool buttonsChanged = state.pressedButtons != previous.buttons;
        if (backend == previous.backend && state.connected == previous.connected && !buttonsChanged &&
            now - previous.lastLog < std::chrono::seconds(2)) return;
        previous.backend = backend;
        previous.connected = state.connected;
        previous.buttons = state.pressedButtons;
        previous.lastLog = now;
        const char *name = backend == testing.get() ? "test" : backend == gameController.get() ? "gamecontroller" :
            backend == raylib.get() ? "raylib" : backend == keyboard.get() ? "keyboard" : "none";
        const std::string buttonNames = describeButtons(state.pressedButtons);
        std::fprintf(stderr, "[pad] port=%u backend=%s connected=%d id=%llu buttons=%04x [%s] raw L=(%.3f,%.3f) R=(%.3f,%.3f) T=(%.3f,%.3f)\n",
            port, name, state.connected ? 1 : 0, static_cast<unsigned long long>(state.deviceId), state.pressedButtons, buttonNames.c_str(),
            state.leftX, state.leftY, state.rightX, state.rightY, state.leftTrigger, state.rightTrigger);
    }

    struct DiagnosticPort
    {
        const HostPadBackend *backend = nullptr;
        bool connected = false;
        uint32_t buttons = UINT32_MAX;
        std::chrono::steady_clock::time_point lastLog{};
    };

    BackendMode mode;
    const bool neutralTest;
    float deadzoneValue;
    bool diagnostics;
    mutable std::mutex mutex;
    std::shared_ptr<HostPadBackend> testing, gameController, raylib, keyboard, external;
    mutable AutoPhysicalBackend autoPhysicalBackend = AutoPhysicalBackend::None;
    std::mutex diagMutex;
    std::array<DiagnosticPort, 2> diagnosticPorts{};
};

PSPadBackend::PSPadBackend() : m_impl(std::make_unique<Impl>()) {}
PSPadBackend::~PSPadBackend() = default;
void PSPadBackend::setExternalBackendForProcess(std::shared_ptr<HostPadBackend> backend) { std::lock_guard<std::mutex> lock(g_externalBackendMutex); g_externalBackend = std::move(backend); }
HostPadState PSPadBackend::snapshot(int port) { return m_impl->getSnapshot(port < 0 ? 0u : static_cast<unsigned>(port)); }
HostPadCapabilities PSPadBackend::capabilities(int port) const { return m_impl->getCapabilities(port < 0 ? 0u : static_cast<unsigned>(port)); }
bool PSPadBackend::isConnected(int port) { return snapshot(port).connected; }
bool PSPadBackend::neutralTestEnabled() const { return m_impl->neutralTest; }
float PSPadBackend::deadzone() const { return m_impl->deadzoneValue; }
bool PSPadBackend::diagnosticsEnabled() const { return m_impl->diagnostics; }
void PSPadBackend::setRumble(int port, HostPadRumble rumble)
{
    if (m_impl->neutralTest || port < 0)
        return;
    std::shared_ptr<HostPadBackend> backend = m_impl->external;
    if (!backend)
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        backend = m_impl->selected();
    }
    if (!backend)
        return;
    if (rumble.lowFrequency <= 0.0f && rumble.highFrequency <= 0.0f)
        backend->stopRumble(static_cast<unsigned>(port));
    else
        backend->setRumble(static_cast<unsigned>(port), rumble);
}
void PSPadBackend::setBackendForTesting(std::shared_ptr<HostPadBackend> backend) { std::lock_guard<std::mutex> lock(m_impl->mutex); m_impl->testing = std::move(backend); }
void PSPadBackend::clearBackendForTesting() { std::lock_guard<std::mutex> lock(m_impl->mutex); m_impl->testing.reset(); }

bool ps2PadForceStartActive()
{
    // Preserve the temporary phase-39 test override, but apply it where PS2
    // active-low packets are made rather than in a host backend.
    static int remaining = [] { const char *v = std::getenv("RRV_TEST_FORCE_START"); return (v && v[0]) ? std::atoi(v) : 0; }();
    if (remaining <= 0) return false;
    --remaining;
    return true;
}
