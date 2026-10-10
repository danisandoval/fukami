#include "rrv_sdl_presentation.h"
#include "rrv_cursor_idle.h"

#if defined(RRV_M2P_PAD_OBSERVER)
#include "rrv_m2p_pad_observer.h"
#endif
#include "runtime/host_pad.h"
#include "fukami_app.h"
#include "fukami_menu.h"
// Plain C ABI header: key codes and rrv_pcsx2_gs_bridge_set_overlay's type.
#include "../../tools/pcsx2-gs-bridge/rrv_pcsx2_gs_bridge.h"

#include <SDL.h>
#if defined(__APPLE__)
#include <SDL_metal.h>
#endif
#include <SDL_syswm.h>
#if defined(__linux__)
// SDL_syswm.h pulls in Xlib, whose macros collide with RRV names
// (NativeSurfaceKind::None, ...). Only the handle types are needed here.
#undef None
#undef Status
#undef Bool
#undef True
#undef False
#endif

#include <dlfcn.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <link.h> // dl_iterate_phdr (Linux, Gate 5)
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
bool diagnosticsEnabled()
{
    const char *value = std::getenv("RRV_PAD_DIAG");
    return value && value[0] && std::strcmp(value, "0") != 0;
}

std::string lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

struct M2PInputFixture
{
    uint32_t pressedButtons = 0u;
    std::chrono::milliseconds startDelay{};
    std::chrono::milliseconds holdDuration{};
};

std::optional<M2PInputFixture> m2pInputFixtureFromEnvironment()
{
    const char *value = std::getenv("RRV_M2P_INPUT_FIXTURE");
    if (!value || !value[0])
        return std::nullopt;

    char button[16] = {};
    unsigned long long delay = 0;
    unsigned long long hold = 0;
    char trailing = '\0';
    if (std::sscanf(value, "%15[^:]:%llu:%llu%c", button, &delay, &hold, &trailing) != 3 ||
        delay > 600000u || hold == 0u || hold > 10000u)
    {
        std::fprintf(stderr,
                     "[m2p-input-fixture] invalid RRV_M2P_INPUT_FIXTURE=%s; "
                     "expected <start|return|up|down|left|right>:"
                     "<delay-ms 0..600000>:<hold-ms 1..10000>\n",
                     value);
        throw std::invalid_argument("invalid RRV_M2P_INPUT_FIXTURE");
    }
    uint32_t pressedButtons = 0u;
    if (std::strcmp(button, "start") == 0)
        pressedButtons = HostPadButtonStart;
    else if (std::strcmp(button, "return") == 0)
        pressedButtons = HostPadButtonStart;
    else if (std::strcmp(button, "up") == 0)
        pressedButtons = HostPadButtonUp;
    else if (std::strcmp(button, "down") == 0)
        pressedButtons = HostPadButtonDown;
    else if (std::strcmp(button, "left") == 0)
        pressedButtons = HostPadButtonLeft;
    else if (std::strcmp(button, "right") == 0)
        pressedButtons = HostPadButtonRight;
    else
    {
        std::fprintf(stderr, "[m2p-input-fixture] unsupported button %s\n", button);
        throw std::invalid_argument("invalid RRV_M2P_INPUT_FIXTURE button");
    }
    return M2PInputFixture{pressedButtons, std::chrono::milliseconds(delay),
                           std::chrono::milliseconds(hold)};
}

#if defined(RRV_M2P_PAD_OBSERVER)
bool isPadKeyboardScancode(SDL_Scancode scancode)
{
    switch (scancode)
    {
    case SDL_SCANCODE_UP: case SDL_SCANCODE_W:
    case SDL_SCANCODE_DOWN: case SDL_SCANCODE_S:
    case SDL_SCANCODE_LEFT: case SDL_SCANCODE_A:
    case SDL_SCANCODE_RIGHT: case SDL_SCANCODE_D:
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_SPACE:
    // Esc/F1 open the in-game menu (the pad then reads neutral); Circle moved
    // to Backspace/C (Esc too while no menu is available).
    case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_F1:
    case SDL_SCANCODE_BACKSPACE: case SDL_SCANCODE_C:
    case SDL_SCANCODE_KP_0: case SDL_SCANCODE_Z:
    case SDL_SCANCODE_KP_1: case SDL_SCANCODE_X:
    case SDL_SCANCODE_Q: case SDL_SCANCODE_E:
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: case SDL_SCANCODE_TAB:
        return true;
    default:
        return false;
    }
}

bool observerEvent(const SDL_Event &event, bool hasSelectedController,
                   uint64_t selectedControllerId, uint32_t *encoded)
{
    constexpr uint32_t kTypeMask = 0x0000ffffu;
    constexpr uint32_t kKeyRepeat = 0x80000000u;
    switch (event.type)
    {
    case SDL_KEYDOWN:
    case SDL_KEYUP:
        if (!isPadKeyboardScancode(event.key.keysym.scancode))
            return false;
        // A repeat is a real SDL transition receipt even when the state poll
        // remains held. Keep it distinct from the initial key-down.
        *encoded = (static_cast<uint32_t>(event.type) & kTypeMask) |
                   (event.key.repeat != 0 ? kKeyRepeat : 0u);
        return true;
    case SDL_CONTROLLERDEVICEADDED:
        // This is low-rate and can alter which physical device becomes the
        // selected owner on the following refresh.
        *encoded = static_cast<uint32_t>(event.type) & kTypeMask;
        return true;
    case SDL_CONTROLLERDEVICEREMOVED:
    case SDL_CONTROLLERDEVICEREMAPPED:
        if (!hasSelectedController ||
            static_cast<uint64_t>(event.cdevice.which) != selectedControllerId)
            return false;
        *encoded = static_cast<uint32_t>(event.type) & kTypeMask;
        return true;
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP:
        if (!hasSelectedController ||
            static_cast<uint64_t>(event.cbutton.which) != selectedControllerId)
            return false;
        *encoded = static_cast<uint32_t>(event.type) & kTypeMask;
        return true;
    case SDL_QUIT:
        *encoded = static_cast<uint32_t>(event.type) & kTypeMask;
        return true;
    case SDL_WINDOWEVENT:
        switch (event.window.event)
        {
        case SDL_WINDOWEVENT_FOCUS_GAINED:
        case SDL_WINDOWEVENT_FOCUS_LOST:
        case SDL_WINDOWEVENT_HIDDEN:
        case SDL_WINDOWEVENT_SHOWN:
        case SDL_WINDOWEVENT_MINIMIZED:
        case SDL_WINDOWEVENT_RESTORED:
            // Low bits identify SDL_WINDOWEVENT; high bits retain its exact
            // lifecycle subtype without moving SDL definitions across host ABI.
            *encoded = (static_cast<uint32_t>(event.type) & kTypeMask) |
                       (static_cast<uint32_t>(event.window.event) << 16u);
            return true;
        default:
            return false;
        }
    default:
        return false;
    }
}
#endif

class SdlHostPadBackend final : public HostPadBackend
{
public:
    explicit SdlHostPadBackend(SDL_Window *window,
                               std::shared_ptr<const fukami::menu::Menu> menu = nullptr) :
        m_window(window),
        m_menu(std::move(menu)),
        m_inputFixture(m2pInputFixtureFromEnvironment()),
        m_inputFixtureStart(std::chrono::steady_clock::now())
    {
#if defined(RRV_M2P_PAD_OBSERVER)
        rrv::m2ppad::initializeFromEnvironment();
#endif
        const char *filter = std::getenv("RRV_PAD_DEVICE");
        if (filter)
            m_deviceFilter = lower(filter);
        // Launcher options `analog` / `rumble` (owner decision 2026-09-29),
        // both default on. RRV_PAD_ANALOG=0 reports a digital-only pad: the
        // guest then sees a digital controller (libpad mode 0x41, sticks and
        // trigger pressure ignored). RRV_PAD_RUMBLE=0 reports no motors, so
        // scePadInfoAct offers none and no vibration reaches the controller.
        const auto envOn = [](const char *name) {
            const char *value = std::getenv(name);
            return !(value && value[0] == '0' && value[1] == '\0');
        };
        m_analog = envOn("RRV_PAD_ANALOG");
        m_rumble = envOn("RRV_PAD_RUMBLE");
        std::fprintf(stderr, "[sdl-pad] analog=%d rumble=%d\n", m_analog ? 1 : 0, m_rumble ? 1 : 0);
        if (m_inputFixture)
            std::fprintf(stderr,
                         "[m2p-input-fixture] test-only pad fixture enabled: "
                         "mask=0x%04x neutral=%lldms press=%lldms then neutral\n",
                         m_inputFixture->pressedButtons,
                         static_cast<long long>(m_inputFixture->startDelay.count()),
                         static_cast<long long>(m_inputFixture->holdDuration.count()));
    }

    ~SdlHostPadBackend() override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        closeControllerLocked();
#if defined(RRV_M2P_PAD_OBSERVER)
        // SdlPresentation outlives the scoped PS2Runtime in rrv-product, so
        // no guest reader can still be writing the diagnostic rings here.
        rrv::m2ppad::finalize();
#endif
    }

    void handleEvent(const SDL_Event &event)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
#if defined(RRV_M2P_PAD_OBSERVER)
        uint32_t diagnosticEvent = 0u;
        if (observerEvent(event, m_controller != nullptr, m_controllerId, &diagnosticEvent))
        {
            ++m_eventOrdinal;
            m_lastEventType = diagnosticEvent;
        }
#endif
        switch (event.type)
        {
        case SDL_CONTROLLERDEVICEADDED:
            maybeOpenControllerLocked(event.cdevice.which);
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            if (m_controller && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(m_controller)) ==
                                    event.cdevice.which)
            {
                closeControllerLocked();
                m_emitDisconnectNeutral = true;
#if defined(RRV_M2P_PAD_OBSERVER)
                // Do not mutate m_cachedState. The one-shot neutral response
                // needs its own receipt while a later pre-refresh snapshot
                // must still expose the accurately labelled old cache.
                m_disconnectNeutralSampleId = ++m_sampleId;
                rrv::m2ppad::hostSample(m_disconnectNeutralSampleId, m_eventOrdinal,
                                         m_lastEventType, 0u, 0u,
                                         rrv::m2ppad::SelectedSource::None, 0u, false);
#endif
            }
            break;
        default:
            break;
        }
    }

    void refresh()
    {
        // SDL's input queries have the same ownership as SDL_PollEvent: they
        // execute only on the product main thread. The guest pad thread sees
        // a mutex-protected cache below and never calls into SDL.
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_inputFixture)
        {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - m_inputFixtureStart);
            HostPadState fixture{};
            fixture.connected = true;
            fixture.deviceId = 0x6d32706669787472ULL; // "m2pfixtr"
            if (elapsed >= m_inputFixture->startDelay &&
                elapsed < m_inputFixture->startDelay + m_inputFixture->holdDuration)
                fixture.pressedButtons = m_inputFixture->pressedButtons;
#if defined(RRV_M2P_PAD_OBSERVER)
            publishSampleLocked(fixture, {true, false, false}, 0u, 0u,
                                rrv::m2ppad::SelectedSource::Fixture);
#else
            m_cachedState = fixture;
            m_cachedCapabilities = {true, false, false};
#endif
            return;
        }
        if (!m_controller)
        {
            const int count = SDL_NumJoysticks();
            for (int index = 0; index < count && !m_controller; ++index)
                maybeOpenControllerLocked(index);
        }
        applyRumbleLocked();

        const Uint8 *keys = SDL_GetKeyboardState(nullptr);
        m_keyboardButtons = 0u;
        if (!keys || !m_window)
        {
#if defined(RRV_M2P_PAD_OBSERVER)
            const HostPadState controller = m_controller ? controllerStateLocked() : HostPadState{};
            publishSampleLocked(menuGateLocked(controller), m_controller ? controllerCapabilitiesLocked()
                                                          : HostPadCapabilities{}, 0u,
                                controller.pressedButtons,
                                m_controller ? rrv::m2ppad::SelectedSource::Controller
                                             : rrv::m2ppad::SelectedSource::None);
#else
            m_cachedState = menuGateLocked(m_controller ? controllerStateLocked() : HostPadState{});
            m_cachedCapabilities = m_controller ? controllerCapabilitiesLocked()
                                             : HostPadCapabilities{};
#endif
            return;
        }
        const auto held = [&](SDL_Scancode code) { return keys[code] != 0; };
        const auto set = [&](HostPadButton button, SDL_Scancode code) {
            if (held(code))
                m_keyboardButtons |= static_cast<uint32_t>(button);
        };
        if (held(SDL_SCANCODE_UP) || held(SDL_SCANCODE_W)) m_keyboardButtons |= HostPadButtonUp;
        if (held(SDL_SCANCODE_DOWN) || held(SDL_SCANCODE_S)) m_keyboardButtons |= HostPadButtonDown;
        if (held(SDL_SCANCODE_LEFT) || held(SDL_SCANCODE_A)) m_keyboardButtons |= HostPadButtonLeft;
        if (held(SDL_SCANCODE_RIGHT) || held(SDL_SCANCODE_D)) m_keyboardButtons |= HostPadButtonRight;
        set(HostPadButtonCross, SDL_SCANCODE_SPACE);
        // Esc and F1 open the in-game menu; Circle is Backspace or C (and Esc
        // only while no menu is available, e.g. a bridge without the overlay).
        if (held(SDL_SCANCODE_BACKSPACE) || held(SDL_SCANCODE_C) ||
            (held(SDL_SCANCODE_ESCAPE) && !(m_menu && m_menu->available())))
            m_keyboardButtons |= HostPadButtonCircle;
        if (held(SDL_SCANCODE_KP_0) || held(SDL_SCANCODE_Z)) m_keyboardButtons |= HostPadButtonSquare;
        if (held(SDL_SCANCODE_KP_1) || held(SDL_SCANCODE_X)) m_keyboardButtons |= HostPadButtonTriangle;
        set(HostPadButtonL1, SDL_SCANCODE_Q);
        set(HostPadButtonR1, SDL_SCANCODE_E);
        set(HostPadButtonL2, SDL_SCANCODE_LSHIFT);
        set(HostPadButtonR2, SDL_SCANCODE_RSHIFT);
        set(HostPadButtonStart, SDL_SCANCODE_RETURN);
        set(HostPadButtonSelect, SDL_SCANCODE_TAB);

        if (m_controller)
        {
#if defined(RRV_M2P_PAD_OBSERVER)
            const HostPadState controller = controllerStateLocked();
            publishSampleLocked(menuGateLocked(controller), controllerCapabilitiesLocked(), m_keyboardButtons,
                                controller.pressedButtons,
                                rrv::m2ppad::SelectedSource::Controller);
#else
            m_cachedState = menuGateLocked(controllerStateLocked());
            m_cachedCapabilities = controllerCapabilitiesLocked();
#endif
            return;
        }
        HostPadState keyboard{};
        keyboard.connected = true;
        keyboard.deviceId = 0x6b6579626f617264ULL; // "keyboard"
        keyboard.pressedButtons = m_keyboardButtons;
        keyboard.leftTrigger = (m_keyboardButtons & HostPadButtonL2) ? 1.0f : 0.0f;
        keyboard.rightTrigger = (m_keyboardButtons & HostPadButtonR2) ? 1.0f : 0.0f;
#if defined(RRV_M2P_PAD_OBSERVER)
        publishSampleLocked(menuGateLocked(keyboard), {m_analog, false, false}, m_keyboardButtons, 0u,
                            rrv::m2ppad::SelectedSource::Keyboard);
#else
        m_cachedState = menuGateLocked(keyboard);
        m_cachedCapabilities = {m_analog, false, false};
#endif
    }

    HostPadState snapshot(unsigned port) override
    {
        if (port != 0u)
            return {};
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_emitDisconnectNeutral)
        {
            m_emitDisconnectNeutral = false;
            HostPadState neutral{};
#if defined(RRV_M2P_PAD_OBSERVER)
            neutral.diagnosticSampleId = m_disconnectNeutralSampleId;
            m_disconnectNeutralSampleId = 0u;
#endif
            return neutral;
        }
        return m_cachedState;
    }

    HostPadCapabilities capabilities(unsigned port) const override
    {
        if (port != 0u)
            return {};
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_cachedCapabilities;
    }

    // Called from the guest pad thread (libpad scePadSetActDirect via
    // scripts/pad_rumble_overlay.py). SDL is main-thread only here, so the
    // request is cached and refresh() applies it.
    void setRumble(unsigned port, HostPadRumble rumble) override
    {
        if (port != 0u || !m_rumble)
            return;
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto level = [](float value) {
            return static_cast<Uint16>(std::lround(std::clamp(value, 0.0f, 1.0f) * 65535.0f));
        };
        const Uint16 low = level(rumble.lowFrequency);
        const Uint16 high = level(rumble.highFrequency);
        if (low != m_rumbleLow || high != m_rumbleHigh)
        {
            logRumble("guest", low, high);
            m_rumbleLow = low;
            m_rumbleHigh = high;
            m_rumbleDirty = true;
        }
    }

    void stopRumble(unsigned port) override { setRumble(port, HostPadRumble{}); }

private:
#if defined(RRV_M2P_PAD_OBSERVER)
    void publishSampleLocked(HostPadState state, HostPadCapabilities capabilities,
                             uint32_t keyboardButtons, uint32_t controllerButtons,
                             rrv::m2ppad::SelectedSource selected)
    {
        const bool transition = !m_hasRecordedHostState ||
            m_lastRecordedEventOrdinal != m_eventOrdinal ||
            m_lastRecordedEventType != m_lastEventType ||
            m_lastRecordedKeyboardButtons != keyboardButtons ||
            m_lastRecordedControllerButtons != controllerButtons ||
            m_lastRecordedSelected != selected ||
            m_lastRecordedMergedButtons != state.pressedButtons ||
            m_lastRecordedConnected != state.connected;
        if (transition)
        {
            ++m_sampleId;
            m_hasRecordedHostState = true;
            m_lastRecordedEventOrdinal = m_eventOrdinal;
            m_lastRecordedEventType = m_lastEventType;
            m_lastRecordedKeyboardButtons = keyboardButtons;
            m_lastRecordedControllerButtons = controllerButtons;
            m_lastRecordedSelected = selected;
            m_lastRecordedMergedButtons = state.pressedButtons;
            m_lastRecordedConnected = state.connected;
            rrv::m2ppad::hostSample(m_sampleId, m_eventOrdinal, m_lastEventType,
                                     keyboardButtons, controllerButtons, selected,
                                     state.pressedButtons, state.connected);
        }
        // Presentation polls continue to refresh axes and capabilities on
        // every iteration. Only the receipt ID is transition-driven.
        state.diagnosticSampleId = m_sampleId;
        m_cachedState = state;
        m_cachedCapabilities = capabilities;
    }
#endif

    bool matchesDeviceFilterLocked(int deviceIndex) const
    {
        if (m_deviceFilter.empty())
            return true;
        const char *name = SDL_GameControllerNameForIndex(deviceIndex);
        return name && lower(name).find(m_deviceFilter) != std::string::npos;
    }

    void maybeOpenControllerLocked(int deviceIndex)
    {
        if (m_controller || !SDL_IsGameController(deviceIndex) ||
            !matchesDeviceFilterLocked(deviceIndex))
            return;
        m_controller = SDL_GameControllerOpen(deviceIndex);
        if (!m_controller)
        {
            std::fprintf(stderr, "[sdl-pad] cannot open controller %d: %s\n", deviceIndex,
                         SDL_GetError());
            return;
        }
        m_controllerId = static_cast<uint64_t>(
            SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(m_controller)));
        {
            const char *name = SDL_GameControllerName(m_controller);
            std::fprintf(stderr, "[sdl-pad] controller=%s analog=%d rumble=%d\n", name ? name : "<unnamed>",
                         m_analog ? 1 : 0, controllerCapabilitiesLocked().rumble ? 1 : 0);
        }
        if (diagnosticsEnabled())
        {
            const char *name = SDL_GameControllerName(m_controller);
            std::fprintf(stderr, "[sdl-pad] selected id=%llu name=%s\n",
                         static_cast<unsigned long long>(m_controllerId),
                         name ? name : "<unnamed>");
        }
    }

    HostPadCapabilities controllerCapabilitiesLocked() const
    {
        return {m_analog, m_analog,
                m_rumble && m_controller && SDL_GameControllerHasRumble(m_controller) == SDL_TRUE};
    }

    // The guest holds a motor level until it writes a new one; SDL rumble
    // expires, so a running motor is re-armed before its duration ends. Like
    // PCSX2 (SDLInputSource::SendRumbleUpdate): the longest SDL duration and a
    // new call only on a change or near expiry. Re-sending the same level every
    // 500 ms restarted the effect on the Steam Deck, so one event pulsed.
    void applyRumbleLocked()
    {
        if (!m_controller || !m_rumble)
            return;
        // Motors stop while the in-game menu is open and resume on close.
        if (m_menu && m_menu->isOpen())
        {
            if (!m_rumbleMuted)
                SDL_GameControllerRumble(m_controller, 0u, 0u, 0u);
            m_rumbleMuted = true;
            return;
        }
        if (m_rumbleMuted)
        {
            m_rumbleMuted = false;
            m_rumbleDirty = true;
        }
        const auto now = std::chrono::steady_clock::now();
        const bool running = m_rumbleLow != 0u || m_rumbleHigh != 0u;
        constexpr Uint32 kRumbleDurationMs = 65535u; // SDL_MAX_RUMBLE_DURATION_MS
        if (!m_rumbleDirty && !(running && now - m_rumbleArmed >= std::chrono::milliseconds(60000)))
            return;
        logRumble(m_rumbleDirty ? "sdl" : "sdl-rearm", m_rumbleLow, m_rumbleHigh);
        SDL_GameControllerRumble(m_controller, m_rumbleLow, m_rumbleHigh, running ? kRumbleDurationMs : 0u);
        m_rumbleArmed = now;
        m_rumbleDirty = false;
    }

    // Rumble changes are rare, so each one is logged (capped): the steady-clock
    // nanoseconds match start-clock.txt, which maps a rumble to its start.
    static void logRumble(const char *what, Uint16 low, Uint16 high)
    {
        static unsigned logged = 0;
        if (logged >= 4000u)
            return;
        ++logged;
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        std::fprintf(stderr, "[rumble] %s ns=%lld large=%u small=%u\n", what, static_cast<long long>(ns),
                     static_cast<unsigned>(low), static_cast<unsigned>(high));
    }

    void closeControllerLocked()
    {
        if (m_controller && (m_rumbleLow != 0u || m_rumbleHigh != 0u))
            SDL_GameControllerRumble(m_controller, 0u, 0u, 0u);
        m_rumbleDirty = m_rumbleLow != 0u || m_rumbleHigh != 0u; // re-arm on a new controller
        if (m_controller)
            SDL_GameControllerClose(m_controller);
        m_controller = nullptr;
        m_controllerId = 0u;
    }

    HostPadState controllerStateLocked() const
    {
        HostPadState state{};
        state.connected = true;
        state.deviceId = m_controllerId;
        const auto pressed = [&](HostPadButton host, SDL_GameControllerButton button) {
            if (SDL_GameControllerGetButton(m_controller, button) != 0)
                state.pressedButtons |= static_cast<uint32_t>(host);
        };
        pressed(HostPadButtonUp, SDL_CONTROLLER_BUTTON_DPAD_UP);
        pressed(HostPadButtonDown, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
        pressed(HostPadButtonLeft, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
        pressed(HostPadButtonRight, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
        pressed(HostPadButtonCross, SDL_CONTROLLER_BUTTON_A);
        pressed(HostPadButtonCircle, SDL_CONTROLLER_BUTTON_B);
        pressed(HostPadButtonSquare, SDL_CONTROLLER_BUTTON_X);
        pressed(HostPadButtonTriangle, SDL_CONTROLLER_BUTTON_Y);
        pressed(HostPadButtonL1, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
        pressed(HostPadButtonR1, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
        pressed(HostPadButtonSelect, SDL_CONTROLLER_BUTTON_BACK);
        pressed(HostPadButtonStart, SDL_CONTROLLER_BUTTON_START);
        pressed(HostPadButtonL3, SDL_CONTROLLER_BUTTON_LEFTSTICK);
        pressed(HostPadButtonR3, SDL_CONTROLLER_BUTTON_RIGHTSTICK);
        const auto axis = [&](SDL_GameControllerAxis value) {
            return std::clamp(static_cast<float>(SDL_GameControllerGetAxis(m_controller, value)) / 32767.0f,
                              -1.0f, 1.0f);
        };
        state.leftX = axis(SDL_CONTROLLER_AXIS_LEFTX);
        state.leftY = -axis(SDL_CONTROLLER_AXIS_LEFTY);
        state.rightX = axis(SDL_CONTROLLER_AXIS_RIGHTX);
        state.rightY = -axis(SDL_CONTROLLER_AXIS_RIGHTY);
        state.leftTrigger = std::max(0.0f, axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT));
        state.rightTrigger = std::max(0.0f, axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT));
        if (state.leftTrigger > 0.0f) state.pressedButtons |= HostPadButtonL2;
        if (state.rightTrigger > 0.0f) state.pressedButtons |= HostPadButtonR2;
        return state;
    }

    // In-game menu gate. While the menu is open the game reads a neutral pad
    // (same device, nothing pressed, sticks centred). After it closes, inputs
    // still held from the menu (Esc, A/Cross, a pushed stick) stay hidden
    // until released, so closing the menu never presses a game button. With
    // the menu never opened this returns `state` unchanged.
    HostPadState menuGateLocked(HostPadState state)
    {
        if (m_menu && m_menu->isOpen())
        {
            m_menuHeld = true;
            HostPadState neutral{};
            neutral.connected = state.connected;
            neutral.deviceId = state.deviceId;
            return neutral;
        }
        if (m_menuHeld)
        {
            m_menuHeld = false;
            m_menuSuppressedButtons = state.pressedButtons;
            m_menuSuppressAxes = true;
        }
        if (m_menuSuppressedButtons != 0u)
        {
            m_menuSuppressedButtons &= state.pressedButtons;
            state.pressedButtons &= ~m_menuSuppressedButtons;
        }
        if (m_menuSuppressAxes)
        {
            constexpr float kRest = 0.25f;
            const bool atRest = std::fabs(state.leftX) < kRest && std::fabs(state.leftY) < kRest &&
                                std::fabs(state.rightX) < kRest && std::fabs(state.rightY) < kRest &&
                                state.leftTrigger < kRest && state.rightTrigger < kRest;
            if (atRest)
                m_menuSuppressAxes = false;
            else
            {
                state.leftX = state.leftY = state.rightX = state.rightY = 0.0f;
                state.leftTrigger = state.rightTrigger = 0.0f;
            }
        }
        return state;
    }

    SDL_Window *m_window = nullptr; // owned by SdlPresentation
    const std::shared_ptr<const fukami::menu::Menu> m_menu;
    bool m_menuHeld = false;
    uint32_t m_menuSuppressedButtons = 0u;
    bool m_menuSuppressAxes = false;
    bool m_rumbleMuted = false;
    mutable std::mutex m_mutex;
    SDL_GameController *m_controller = nullptr;
    uint64_t m_controllerId = 0u;
    uint32_t m_keyboardButtons = 0u;
#if defined(RRV_M2P_PAD_OBSERVER)
    uint64_t m_sampleId = 0u;
    uint64_t m_eventOrdinal = 0u;
    uint32_t m_lastEventType = 0u;
    uint64_t m_disconnectNeutralSampleId = 0u;
    bool m_hasRecordedHostState = false;
    uint64_t m_lastRecordedEventOrdinal = 0u;
    uint32_t m_lastRecordedEventType = 0u;
    uint32_t m_lastRecordedKeyboardButtons = 0u;
    uint32_t m_lastRecordedControllerButtons = 0u;
    rrv::m2ppad::SelectedSource m_lastRecordedSelected = rrv::m2ppad::SelectedSource::None;
    uint32_t m_lastRecordedMergedButtons = 0u;
    bool m_lastRecordedConnected = false;
#endif
    HostPadState m_cachedState{};
    HostPadCapabilities m_cachedCapabilities{};
    bool m_analog = true;
    bool m_rumble = true;
    Uint16 m_rumbleLow = 0u;  // large motor
    Uint16 m_rumbleHigh = 0u; // small motor
    bool m_rumbleDirty = false;
    std::chrono::steady_clock::time_point m_rumbleArmed{};
    bool m_emitDisconnectNeutral = false;
    std::string m_deviceFilter;
    const std::optional<M2PInputFixture> m_inputFixture;
    const std::chrono::steady_clock::time_point m_inputFixtureStart;
};
} // namespace

namespace rrv::host
{
class SdlPresentation::Impl
{
public:
    SDL_Window *window = nullptr;
#if defined(__APPLE__)
    SDL_MetalView metalView = nullptr;
    void *metalLayer = nullptr;
#else
    // Linux (Gate 5): the X11 Display*/Window or Wayland wl_display*/wl_surface*
    // SDL owns for the window's lifetime (SDL_GetWindowWMInfo).
    rrv::gsbackend::NativeSurfaceKind surfaceKind = rrv::gsbackend::NativeSurfaceKind::None;
    void *nativeView = nullptr;
    void *nativeLayer = nullptr;
#endif
    std::shared_ptr<SdlHostPadBackend> pad;
    bool closeRequested = false;
    // Fukami in-game menu (see src/app/fukami_menu.h) and its bridge overlay.
    std::shared_ptr<fukami::menu::Menu> menu;
    void *bridgeImage = nullptr; // our own RTLD_NOLOAD reference to the bridge
    void (*setOverlay)(RrvPcsx2GsBridgeOverlayFrame, void *) = nullptr;
    bool overlayUnavailable = false;
    unsigned overlayProbe = 0u;
    bool stickLeft = false, stickRight = false, stickUp = false, stickDown = false;
    bool lastFullscreen = false;
    bool restartAfterClose = false;
    // The pointer is hidden in full screen while the mouse is still (rrv_cursor_idle.h).
    rrv::host::CursorIdle cursorIdle;
    bool cursorHidden = false;

    // Main thread (pumpEvents).
    void routeMenuEvent(const SDL_Event &event);
    void serviceMenu();
    void toggleMenu();
    void postStick(bool &held, bool now, uint32_t key);
    bool testLifecycle = false;
    unsigned testLifecycleStep = 0u;
    std::chrono::steady_clock::time_point testLifecycleStart{};
    bool testLifecycleFailed = false;
    std::string testLifecycleFailure;
    mutable uint32_t lastSurfaceWidth = 0u;
    mutable uint32_t lastSurfaceHeight = 0u;
    mutable unsigned observedSurfaceChanges = 0u;
};

namespace
{
// Gate 7 aspect/UI: the window the product opens. RRV_WINDOW_SIZE=WxH (logical
// points) wins; otherwise the window takes the shape of RRV_PCSX2_GS_ASPECT
// (16:9, 16:10 or 21:9 when set to one of those, else 4:3) at 80% of the
// usable display height, rounded down
// to a multiple of 8 and never below 640x480. The picture itself is fitted by
// PCSX2 (see the bridge), so any size is valid; this only picks a good start.
void initialWindowSize(int &width, int &height)
{
    if (const char *value = std::getenv("RRV_WINDOW_SIZE"); value && value[0] != '\0')
    {
        int w = 0, h = 0;
        char tail = 0;
        if (std::sscanf(value, "%dx%d%c", &w, &h, &tail) == 2 && w >= 320 && h >= 224 && w <= 16384 &&
            h <= 16384)
        {
            width = w;
            height = h;
            return;
        }
        std::fprintf(stderr, "[sdl] ignoring invalid RRV_WINDOW_SIZE=%s (want WxH)\n", value);
    }
    const char *aspect = std::getenv("RRV_PCSX2_GS_ASPECT");
    int rw = 4, rh = 3;
    if (aspect && (std::strcmp(aspect, "16:9") == 0 || std::strcmp(aspect, "16:10") == 0 ||
                   std::strcmp(aspect, "21:9") == 0))
        std::sscanf(aspect, "%d:%d", &rw, &rh);
    SDL_Rect usable{};
    int h = 960;
    if (SDL_GetDisplayUsableBounds(0, &usable) == 0 && usable.h > 0)
        h = std::max(480, (usable.h * 4 / 5) / 8 * 8);
    int w = h * rw / rh;
    if (usable.w > 0 && w > usable.w)
    {
        w = std::max(640, usable.w / 8 * 8);
        h = std::max(480, (w * rh / rw) / 8 * 8);
    }
    width = w;
    height = h;
}

// The window's drawable size in pixels: the Metal layer's on macOS, the
// window's pixel size on Linux (X11/Wayland; SDL 2.26+).
void drawableSize(SDL_Window *window, int *width, int *height)
{
#if defined(__APPLE__)
    SDL_Metal_GetDrawableSize(window, width, height);
#else
    SDL_GetWindowSizeInPixels(window, width, height);
#endif
}

// ---- macOS full screen ------------------------------------------------------
// SDL 2.32 puts an SDL_WINDOW_FULLSCREEN_DESKTOP window in its own full-screen
// Space: the same AppKit state the green window button and SDL's default
// Window > Toggle Full Screen item (Cmd+Ctrl+F) enter. But SDL does not report
// a Space the user entered that way in SDL_GetWindowFlags(), and while its own
// flag is set it disables that menu item (SDL_cocoawindow.m validateMenuItem).
// So the host asks the window itself whether it is full screen (its style
// mask), and enters full screen exactly as the green button does
// (toggleFullScreen:), leaving SDL's flag clear so the button, the menu item
// and Cmd+Ctrl+F keep working. A full screen SDL entered itself (startup with
// RRV_WINDOW_FULLSCREEN=1) is left through SDL_SetWindowFullscreen(0).
// The two AppKit messages go through the Objective-C runtime so this SDL-only
// host includes no AppKit headers (scripts/check_product_direct.py).
using ObjcSelRegister = void *(*)(const char *);
using ObjcStyleMask = unsigned long (*)(void *, void *);
using ObjcToggle = void (*)(void *, void *, void *);
constexpr unsigned long kAppKitStyleMaskFullScreen = 1ul << 14;

struct ObjcFullscreen
{
    void *msgSend = nullptr;
    void *styleMask = nullptr;
    void *toggleFullScreen = nullptr;
};

const ObjcFullscreen &objcFullscreen()
{
    static const ObjcFullscreen runtime = [] {
        ObjcFullscreen value;
        // libobjc is always loaded under SDL's Cocoa video backend.
        value.msgSend = dlsym(RTLD_DEFAULT, "objc_msgSend");
        const auto registerSel = reinterpret_cast<ObjcSelRegister>(dlsym(RTLD_DEFAULT, "sel_registerName"));
        if (value.msgSend && registerSel)
        {
            value.styleMask = registerSel("styleMask");
            value.toggleFullScreen = registerSel("toggleFullScreen:");
        }
        return value;
    }();
    return runtime;
}

void *appKitWindow(SDL_Window *window)
{
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
#if defined(__APPLE__)
    if (!window || SDL_GetWindowWMInfo(window, &info) != SDL_TRUE || info.subsystem != SDL_SYSWM_COCOA)
        return nullptr;
    return static_cast<void *>(info.info.cocoa.window);
#else
    (void)window;
    (void)info;
    return nullptr; // no AppKit window off macOS; SDL's own fullscreen is used
#endif
}

bool windowFullscreen(SDL_Window *window)
{
    if (!window)
        return false;
    if ((SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0u)
        return true;
    const ObjcFullscreen &objc = objcFullscreen();
    void *native = appKitWindow(window);
    if (!native || !objc.styleMask)
        return false;
    return (reinterpret_cast<ObjcStyleMask>(objc.msgSend)(native, objc.styleMask) & kAppKitStyleMaskFullScreen) != 0u;
}

// Main thread only (SDL and AppKit window calls).
void setWindowFullscreen(SDL_Window *window, bool fullscreen)
{
    if (!window || windowFullscreen(window) == fullscreen)
        return;
    if (!fullscreen && (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0u)
    {
        if (SDL_SetWindowFullscreen(window, 0u) != 0)
            std::fprintf(stderr, "[sdl] fullscreen change failed: %s\n", SDL_GetError());
        return;
    }
    const ObjcFullscreen &objc = objcFullscreen();
    if (void *native = appKitWindow(window); native && objc.toggleFullScreen)
    {
        reinterpret_cast<ObjcToggle>(objc.msgSend)(native, objc.toggleFullScreen, nullptr);
        return;
    }
    if (SDL_SetWindowFullscreen(window, fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0u) != 0)
        std::fprintf(stderr, "[sdl] fullscreen change failed: %s\n", SDL_GetError());
}

// ---- in-game menu -----------------------------------------------------------
// The bridge image the GS backend loaded (and verified through
// rrv::resource_package) exports the optional overlay entry point. RTLD_NOLOAD
// never loads anything: before the backend has loaded the bridge this finds
// nothing and the host tries again later. The returned handle is our own
// reference, which keeps the image mapped until destroy() cleared the overlay.
// Returns 1 (found), 0 (bridge not loaded yet) or -1 (bridge without overlay).
#if defined(__APPLE__)
constexpr std::string_view kBridgeImageSuffix = "/librrv-pcsx2-gs-bridge.dylib";
#else
constexpr std::string_view kBridgeImageSuffix = "/librrv-pcsx2-gs-bridge.so";
#endif

// Names of the images loaded in this process (dyld on macOS, the dynamic
// linker's list via dl_iterate_phdr on Linux).
std::vector<std::string> loadedImageNames()
{
    std::vector<std::string> names;
#if defined(__APPLE__)
    for (uint32_t index = 0, count = _dyld_image_count(); index < count; ++index)
        if (const char *name = _dyld_get_image_name(index))
            names.emplace_back(name);
#else
    dl_iterate_phdr(
        [](struct dl_phdr_info *info, size_t, void *data) {
            if (info->dlpi_name && info->dlpi_name[0] != '\0')
                static_cast<std::vector<std::string> *>(data)->emplace_back(info->dlpi_name);
            return 0;
        },
        &names);
#endif
    return names;
}

int findBridgeOverlay(void **image, void **symbol)
{
    for (const std::string &loaded : loadedImageNames())
    {
        const char *name = loaded.c_str();
        const std::string_view path = loaded;
        if (path.size() < kBridgeImageSuffix.size() ||
            path.substr(path.size() - kBridgeImageSuffix.size()) != kBridgeImageSuffix)
            continue;
        void *handle = dlopen(name, RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
        if (!handle)
            continue;
        void *entry = dlsym(handle, "rrv_pcsx2_gs_bridge_set_overlay");
        Dl_info info{};
        char expected[PATH_MAX] = {};
        char observed[PATH_MAX] = {};
        if (!entry || dladdr(entry, &info) == 0 || !info.dli_fname || !realpath(name, expected) ||
            !realpath(info.dli_fname, observed) || std::strcmp(expected, observed) != 0)
        {
            dlclose(handle);
            return -1;
        }
        *image = handle;
        *symbol = entry;
        return 1;
    }
    return 0;
}

uint32_t menuKeyForScancode(SDL_Scancode scancode)
{
    switch (scancode)
    {
    case SDL_SCANCODE_TAB: return RRV_PCSX2_GS_UI_KEY_TAB;
    case SDL_SCANCODE_LEFT: case SDL_SCANCODE_A: return RRV_PCSX2_GS_UI_KEY_LEFT;
    case SDL_SCANCODE_RIGHT: case SDL_SCANCODE_D: return RRV_PCSX2_GS_UI_KEY_RIGHT;
    case SDL_SCANCODE_UP: case SDL_SCANCODE_W: return RRV_PCSX2_GS_UI_KEY_UP;
    case SDL_SCANCODE_DOWN: case SDL_SCANCODE_S: return RRV_PCSX2_GS_UI_KEY_DOWN;
    case SDL_SCANCODE_PAGEUP: return RRV_PCSX2_GS_UI_KEY_PAGE_UP;
    case SDL_SCANCODE_PAGEDOWN: return RRV_PCSX2_GS_UI_KEY_PAGE_DOWN;
    case SDL_SCANCODE_HOME: return RRV_PCSX2_GS_UI_KEY_HOME;
    case SDL_SCANCODE_END: return RRV_PCSX2_GS_UI_KEY_END;
    case SDL_SCANCODE_SPACE: return RRV_PCSX2_GS_UI_KEY_SPACE;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return RRV_PCSX2_GS_UI_KEY_ENTER;
    case SDL_SCANCODE_ESCAPE: case SDL_SCANCODE_BACKSPACE: return RRV_PCSX2_GS_UI_KEY_ESCAPE;
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return RRV_PCSX2_GS_UI_KEY_SHIFT;
    default: return 0u;
    }
}

uint32_t menuKeyForButton(Uint8 button)
{
    switch (button)
    {
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return RRV_PCSX2_GS_UI_KEY_GAMEPAD_DPAD_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return RRV_PCSX2_GS_UI_KEY_GAMEPAD_DPAD_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return RRV_PCSX2_GS_UI_KEY_GAMEPAD_DPAD_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return RRV_PCSX2_GS_UI_KEY_GAMEPAD_DPAD_RIGHT;
    case SDL_CONTROLLER_BUTTON_A: return RRV_PCSX2_GS_UI_KEY_GAMEPAD_FACE_DOWN;
    case SDL_CONTROLLER_BUTTON_B: return RRV_PCSX2_GS_UI_KEY_GAMEPAD_FACE_RIGHT;
    case SDL_CONTROLLER_BUTTON_X: return RRV_PCSX2_GS_UI_KEY_GAMEPAD_FACE_LEFT;
    case SDL_CONTROLLER_BUTTON_Y: return RRV_PCSX2_GS_UI_KEY_GAMEPAD_FACE_UP;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return RRV_PCSX2_GS_UI_KEY_GAMEPAD_L1;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return RRV_PCSX2_GS_UI_KEY_GAMEPAD_R1;
    default: return 0u;
    }
}

// Window points -> drawable pixels (ImGui's display is the drawable).
float pixelsPerPoint(SDL_Window *window)
{
    int points = 0, pixels = 0, unused = 0;
    SDL_GetWindowSize(window, &points, &unused);
    drawableSize(window, &pixels, &unused);
    return points > 0 && pixels > 0 ? static_cast<float>(pixels) / static_cast<float>(points) : 1.0f;
}

fukami::menu::Input menuKey(uint32_t key, bool down)
{
    fukami::menu::Input input;
    input.type = fukami::menu::Input::Type::key;
    input.code = key;
    input.down = down;
    return input;
}
} // namespace

void SdlPresentation::Impl::toggleMenu()
{
    if (menu->isOpen())
    {
        menu->close();
        return;
    }
    menu->open();
    if (!menu->isOpen())
        return; // no overlay yet
    stickLeft = stickRight = stickUp = stickDown = false;
    int x = 0, y = 0;
    SDL_GetMouseState(&x, &y);
    fukami::menu::Input position;
    position.type = fukami::menu::Input::Type::mousePosition;
    const float scale = pixelsPerPoint(window);
    position.x = static_cast<float>(x) * scale;
    position.y = static_cast<float>(y) * scale;
    menu->post(position);
}

void SdlPresentation::Impl::postStick(bool &held, bool now, uint32_t key)
{
    if (held == now)
        return;
    held = now;
    menu->post(menuKey(key, now));
}

// Esc and F1 toggle the menu, as does a controller's guide/Home button. While
// it is open, keyboard, mouse and controller input go to the menu (the game's
// pad reads neutral; see SdlHostPadBackend::menuGateLocked). Esc and B/Circle
// are forwarded too: the menu closes a combo with them, else itself.
void SdlPresentation::Impl::routeMenuEvent(const SDL_Event &event)
{
    using Input = fukami::menu::Input;
    switch (event.type)
    {
    case SDL_KEYDOWN:
    case SDL_KEYUP:
    {
        const SDL_Scancode scancode = event.key.keysym.scancode;
        const bool down = event.type == SDL_KEYDOWN;
        if (event.key.repeat != 0)
            return;
        if (down && (scancode == SDL_SCANCODE_F1 || (scancode == SDL_SCANCODE_ESCAPE && !menu->isOpen())))
        {
            toggleMenu();
            return;
        }
        if (const uint32_t key = menuKeyForScancode(scancode); key != 0u && menu->isOpen())
            menu->post(menuKey(key, down));
        return;
    }
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP:
    {
        const bool down = event.type == SDL_CONTROLLERBUTTONDOWN;
        if (event.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE)
        {
            if (down)
                toggleMenu();
            return;
        }
        if (const uint32_t key = menuKeyForButton(event.cbutton.button); key != 0u && menu->isOpen())
            menu->post(menuKey(key, down));
        return;
    }
    case SDL_CONTROLLERAXISMOTION:
    {
        if (!menu->isOpen())
            return;
        // The left stick navigates like the d-pad (as PCSX2's ImGuiManager
        // maps it), with hysteresis so a resting stick never repeats.
        const float value = static_cast<float>(event.caxis.value) / 32767.0f;
        const auto held = [](bool was, float amount) { return was ? amount > 0.3f : amount > 0.5f; };
        if (event.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX)
        {
            postStick(stickLeft, held(stickLeft, -value), RRV_PCSX2_GS_UI_KEY_GAMEPAD_DPAD_LEFT);
            postStick(stickRight, held(stickRight, value), RRV_PCSX2_GS_UI_KEY_GAMEPAD_DPAD_RIGHT);
        }
        else if (event.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY)
        {
            postStick(stickUp, held(stickUp, -value), RRV_PCSX2_GS_UI_KEY_GAMEPAD_DPAD_UP);
            postStick(stickDown, held(stickDown, value), RRV_PCSX2_GS_UI_KEY_GAMEPAD_DPAD_DOWN);
        }
        return;
    }
    case SDL_MOUSEMOTION:
    {
        if (!menu->isOpen())
            return;
        Input input;
        input.type = Input::Type::mousePosition;
        const float scale = pixelsPerPoint(window);
        input.x = static_cast<float>(event.motion.x) * scale;
        input.y = static_cast<float>(event.motion.y) * scale;
        menu->post(input);
        return;
    }
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
    {
        if (!menu->isOpen())
            return;
        Input input;
        input.type = Input::Type::mouseButton;
        input.down = event.type == SDL_MOUSEBUTTONDOWN;
        switch (event.button.button)
        {
        case SDL_BUTTON_LEFT: input.code = 0u; break;
        case SDL_BUTTON_RIGHT: input.code = 1u; break;
        case SDL_BUTTON_MIDDLE: input.code = 2u; break;
        default: return;
        }
        menu->post(input);
        return;
    }
    case SDL_MOUSEWHEEL:
    {
        if (!menu->isOpen())
            return;
        Input input;
        input.type = Input::Type::mouseWheel;
        const float flip = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
        input.x = event.wheel.preciseX * flip;
        input.y = event.wheel.preciseY * flip;
        menu->post(input);
        return;
    }
    default:
        return;
    }
}

void SdlPresentation::Impl::serviceMenu()
{
    // Bind the overlay once the GS backend has loaded the bridge.
    if (!setOverlay && !overlayUnavailable && (overlayProbe++ % 30u) == 0u)
    {
        void *image = nullptr;
        void *entry = nullptr;
        const int found = findBridgeOverlay(&image, &entry);
        if (found > 0)
        {
            bridgeImage = image;
            setOverlay = reinterpret_cast<void (*)(RrvPcsx2GsBridgeOverlayFrame, void *)>(entry);
            setOverlay(&fukami::menu::Menu::frameCallback, menu.get());
            menu->setAvailable(true);
            std::fprintf(stderr, "[fukami-menu] in-game menu ready (Esc or F1; settings %s)\n",
                         menu->iniPath().empty() ? "not saved" : "saved to the launch's settings file");
        }
        else if (found < 0)
        {
            overlayUnavailable = true;
            std::fprintf(stderr, "[fukami-menu] bridge has no overlay support; in-game menu unavailable\n");
        }
    }
    // Requests made in the menu (GS owner thread) are carried out here.
    if (const int fullscreen = menu->takeFullscreenRequest(); fullscreen >= 0)
        setWindowFullscreen(window, fullscreen == 1);
    if (menu->takeQuitRequest())
    {
        std::fprintf(stderr, "[fukami-menu] quit\n");
        closeRequested = true;
    }
    if (menu->takeRestartRequest() && fukami::app::canRestart())
    {
        std::fprintf(stderr, "[fukami-menu] restart requested\n");
        restartAfterClose = true;
        closeRequested = true;
    }
}

SdlPresentation::SdlPresentation() = default;

SdlPresentation::~SdlPresentation()
{
    destroy();
}

bool SdlPresentation::create(const char *title, std::string *error)
{
    destroy();
    // SDL leaves rumble off for PS4/PS5 pads over Bluetooth (it switches
    // them to the enhanced report mode). Turn it on unless RRV_PAD_RUMBLE=0,
    // so a DualShock 4 / DualSense vibrates like the DualShock 2 it stands in
    // for. Must be set before the controllers open.
    if (const char *rumble = std::getenv("RRV_PAD_RUMBLE"); !(rumble && rumble[0] == '0' && rumble[1] == '\0'))
    {
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
    }
    // Full screen is a macOS full-screen Space (SDL's default, stated here
    // because the green button and Cmd+Ctrl+F rely on it; see setWindowFullscreen).
    SDL_SetHint(SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES, "1");
#if defined(__linux__)
    // Gate 5: X11 first (Steam Deck Game Mode's gamescope runs SDL on
    // XWayland), Wayland second. An explicit SDL_VIDEODRIVER still wins.
    if (const char *driver = std::getenv("SDL_VIDEODRIVER"); !driver || driver[0] == '\0')
        SDL_SetHint(SDL_HINT_VIDEODRIVER, "x11,wayland");
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0)
    {
        if (error) *error = std::string("SDL initialization failed: ") + SDL_GetError();
        return false;
    }
    auto impl = std::make_unique<Impl>();
    int windowWidth = 640;
    int windowHeight = 480;
    initialWindowSize(windowWidth, windowHeight);
    const char *fullscreenEnv = std::getenv("RRV_WINDOW_FULLSCREEN");
    const bool startFullscreen = fullscreenEnv && std::strcmp(fullscreenEnv, "1") == 0;
#if defined(__APPLE__)
    constexpr Uint32 kApiWindowFlag = SDL_WINDOW_METAL;
    constexpr const char *kApiName = "Metal";
#else
    // Linux: the bridge's Vulkan device makes its own VkSurfaceKHR from the
    // native handles, so the window needs no SDL graphics API flag.
    constexpr Uint32 kApiWindowFlag = 0u;
    constexpr const char *kApiName = "Vulkan";
#endif
    impl->window = SDL_CreateWindow(title ? title : "Fukami", SDL_WINDOWPOS_CENTERED,
                                    SDL_WINDOWPOS_CENTERED, windowWidth, windowHeight,
                                    kApiWindowFlag | SDL_WINDOW_RESIZABLE |
                                        SDL_WINDOW_ALLOW_HIGHDPI |
                                        (startFullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0u));
    if (!impl->window)
    {
        if (error) *error = std::string("SDL ") + kApiName + " window creation failed: " + SDL_GetError();
        SDL_Quit();
        return false;
    }
#if defined(__APPLE__)
    impl->metalView = SDL_Metal_CreateView(impl->window);
    if (!impl->metalView)
    {
        if (error) *error = std::string("SDL Metal view creation failed: ") + SDL_GetError();
        SDL_DestroyWindow(impl->window);
        SDL_Quit();
        return false;
    }
    impl->metalLayer = SDL_Metal_GetLayer(impl->metalView);
    if (!impl->metalLayer)
    {
        if (error) *error = std::string("SDL Metal layer acquisition failed: ") + SDL_GetError();
        SDL_Metal_DestroyView(impl->metalView);
        SDL_DestroyWindow(impl->window);
        SDL_Quit();
        return false;
    }
#else
    {
        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        if (SDL_GetWindowWMInfo(impl->window, &info) != SDL_TRUE)
        {
            if (error) *error = std::string("SDL window handle query failed: ") + SDL_GetError();
            SDL_DestroyWindow(impl->window);
            SDL_Quit();
            return false;
        }
        switch (info.subsystem)
        {
#if defined(SDL_VIDEO_DRIVER_X11)
        case SDL_SYSWM_X11:
            impl->surfaceKind = rrv::gsbackend::NativeSurfaceKind::LinuxX11;
            impl->nativeView = static_cast<void *>(info.info.x11.display);
            impl->nativeLayer = reinterpret_cast<void *>(static_cast<uintptr_t>(info.info.x11.window));
            break;
#endif
#if defined(SDL_VIDEO_DRIVER_WAYLAND)
        case SDL_SYSWM_WAYLAND:
            impl->surfaceKind = rrv::gsbackend::NativeSurfaceKind::LinuxWayland;
            impl->nativeView = static_cast<void *>(info.info.wl.display);
            impl->nativeLayer = static_cast<void *>(info.info.wl.surface);
            break;
#endif
        default:
            break;
        }
        if (impl->surfaceKind == rrv::gsbackend::NativeSurfaceKind::None || !impl->nativeView ||
            !impl->nativeLayer)
        {
            if (error)
                *error = "SDL video driver " + std::string(SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?") +
                         " gives no X11 or Wayland window handle";
            SDL_DestroyWindow(impl->window);
            SDL_Quit();
            return false;
        }
        std::fprintf(stderr, "[sdl] video driver %s, %s surface\n", SDL_GetCurrentVideoDriver(),
                     impl->surfaceKind == rrv::gsbackend::NativeSurfaceKind::LinuxX11 ? "X11" : "Wayland");
    }
#endif
    // The in-game menu reads/writes the launch's settings file (empty: none).
    // It becomes visible once the bridge accepts its overlay (pumpEvents).
    // In the app, a setting the file does not have shows the app's bundled default (as the launcher used it).
    impl->menu = std::make_shared<fukami::menu::Menu>(fukami::app::iniPath(), fukami::app::defaultIniPath());
    impl->lastFullscreen = windowFullscreen(impl->window);
    impl->menu->fullscreenChanged(impl->lastFullscreen, false);
    impl->pad = std::make_shared<SdlHostPadBackend>(impl->window, impl->menu);
    impl->pad->refresh();
    if (const char *test = std::getenv("RRV_TEST_SDL_LIFECYCLE");
        test && std::strcmp(test, "1") == 0)
    {
        impl->testLifecycle = true;
        impl->testLifecycleStart = std::chrono::steady_clock::now();
        std::fprintf(stderr,
                     "[m1-lifecycle] test-only SDL lifecycle sequence enabled; normal execution unchanged\n");
    }
    std::fprintf(stderr, "[sdl] window %dx%d%s (F11 or Cmd+Ctrl+F toggles fullscreen)\n", windowWidth,
                 windowHeight, startFullscreen ? " fullscreen" : "");
    m_impl = std::move(impl);
    return true;
}

void SdlPresentation::destroy()
{
    if (!m_impl)
        return;
    // Clear the overlay first: this waits for an in-flight menu frame on the
    // GS owner, then our image reference may go.
    if (m_impl->setOverlay)
        m_impl->setOverlay(nullptr, nullptr);
    if (m_impl->bridgeImage)
        dlclose(m_impl->bridgeImage);
    m_impl->pad.reset();
    m_impl->menu.reset();
#if defined(__APPLE__)
    if (m_impl->metalView)
        SDL_Metal_DestroyView(m_impl->metalView);
#endif
    if (m_impl->window)
        SDL_DestroyWindow(m_impl->window);
    SDL_Quit();
    const bool restart = m_impl->restartAfterClose;
    m_impl.reset();
    if (restart)
    {
        // The menu's Restart: the product has stopped exactly as for a window
        // close (runtime and GS torn down first); start it again so the
        // settings file is read anew. restart() returns only on failure.
        std::fprintf(stderr, "[fukami-menu] restarting to apply settings\n");
        std::fflush(nullptr);
        fukami::app::restart();
        std::fprintf(stderr, "[fukami-menu] restart failed; relaunch the app\n");
    }
}

void SdlPresentation::pumpEvents()
{
    if (!m_impl)
        return;
    SDL_Event event{};
    while (SDL_PollEvent(&event) != 0)
    {
        if (event.type == SDL_QUIT ||
            (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE))
        {
            m_impl->closeRequested = true;
        }
        else if (m_impl->testLifecycle && event.type == SDL_WINDOWEVENT)
        {
            std::fprintf(stderr, "[m1-lifecycle] SDL window event=%u data=%d,%d\n",
                         event.window.event, event.window.data1, event.window.data2);
        }
        else if (event.type == SDL_KEYDOWN && event.key.keysym.scancode == SDL_SCANCODE_F11 &&
                 event.key.repeat == 0)
        {
            setWindowFullscreen(m_impl->window, !windowFullscreen(m_impl->window));
        }
        else if (event.type == SDL_KEYDOWN && event.key.keysym.scancode == SDL_SCANCODE_F &&
                 (event.key.keysym.mod & KMOD_CTRL) != 0 && (event.key.keysym.mod & KMOD_GUI) != 0 &&
                 event.key.repeat == 0)
        {
            // Cmd+Ctrl+F is SDL's Window > Toggle Full Screen key: AppKit
            // toggles the Space itself. SDL disables that item only while its
            // own full-screen flag is set (startup full screen); leave it here.
            if ((SDL_GetWindowFlags(m_impl->window) & SDL_WINDOW_FULLSCREEN) != 0u)
                setWindowFullscreen(m_impl->window, false);
        }
        else if (m_impl->menu)
        {
            m_impl->routeMenuEvent(event);
        }
        if (event.type == SDL_MOUSEMOTION || event.type == SDL_MOUSEBUTTONDOWN ||
            event.type == SDL_MOUSEBUTTONUP || event.type == SDL_MOUSEWHEEL)
            m_impl->cursorIdle.activity(std::chrono::steady_clock::now());
        m_impl->pad->handleEvent(event);
    }
    if (m_impl->menu)
        m_impl->serviceMenu();
    const bool fullscreen = windowFullscreen(m_impl->window);
    if (fullscreen != m_impl->lastFullscreen)
    {
        // Any route: F11, Cmd+Ctrl+F, the green button, the menu bar, the menu.
        m_impl->lastFullscreen = fullscreen;
        std::fprintf(stderr, "[sdl] fullscreen=%d\n", fullscreen ? 1 : 0);
        if (m_impl->menu)
            m_impl->menu->fullscreenChanged(fullscreen, !m_impl->testLifecycle);
    }
    const bool hideCursor = m_impl->cursorIdle.hidden(fullscreen, m_impl->menu && m_impl->menu->isOpen(),
                                                      std::chrono::steady_clock::now());
    if (hideCursor != m_impl->cursorHidden)
    {
        m_impl->cursorHidden = hideCursor;
        std::fprintf(stderr, "[sdl] pointer %s\n", hideCursor ? "hidden (full screen, mouse still)" : "shown");
        if (SDL_ShowCursor(hideCursor ? SDL_DISABLE : SDL_ENABLE) < 0)
            std::fprintf(stderr, "[sdl] cursor %s failed: %s\n", hideCursor ? "hide" : "show", SDL_GetError());
    }
    if (m_impl->testLifecycle)
    {
        using namespace std::chrono;
        const auto elapsed = duration_cast<milliseconds>(steady_clock::now() -
                                                          m_impl->testLifecycleStart);
        const auto setFullscreen = [&](Uint32 flags, const char *label) {
            if (SDL_SetWindowFullscreen(m_impl->window, flags) != 0)
            {
                std::fprintf(stderr, "[m1-lifecycle] %s failed: %s\n", label, SDL_GetError());
                m_impl->testLifecycleFailed = true;
                m_impl->testLifecycleFailure = std::string(label) + " failed: " + SDL_GetError();
            }
            else
                std::fprintf(stderr, "[m1-lifecycle] %s requested on SDL_Window\n", label);
        };
        const auto failState = [&](const char *message) {
            m_impl->testLifecycleFailed = true;
            if (m_impl->testLifecycleFailure.empty()) m_impl->testLifecycleFailure = message;
            std::fprintf(stderr, "[m1-lifecycle] state verification failed: %s\n", message);
        };
        switch (m_impl->testLifecycleStep)
        {
        case 0u:
            if (elapsed >= 250ms)
            {
                SDL_SetWindowSize(m_impl->window, 800, 600);
                std::fprintf(stderr, "[m1-lifecycle] resize 800x600 requested on SDL_Window\n");
                ++m_impl->testLifecycleStep;
            }
            break;
        case 1u:
            if (elapsed >= 500ms)
            {
                int width = 0, height = 0;
                SDL_GetWindowSize(m_impl->window, &width, &height);
                if (width != 800 || height != 600) failState("800x600 resize was not observed");
                setFullscreen(SDL_WINDOW_FULLSCREEN_DESKTOP, "fullscreen enter");
                ++m_impl->testLifecycleStep;
            }
            break;
        case 2u:
            if (elapsed >= 1000ms)
            {
                if ((SDL_GetWindowFlags(m_impl->window) & SDL_WINDOW_FULLSCREEN_DESKTOP) == 0u)
                    failState("fullscreen entry was not observed");
                setFullscreen(0u, "fullscreen exit");
                ++m_impl->testLifecycleStep;
            }
            break;
        case 3u:
            if (elapsed >= 2500ms)
            {
                if ((SDL_GetWindowFlags(m_impl->window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0u)
                    failState("fullscreen exit was not observed");
                SDL_MinimizeWindow(m_impl->window);
                std::fprintf(stderr, "[m1-lifecycle] minimize requested on SDL_Window\n");
                ++m_impl->testLifecycleStep;
            }
            break;
        case 4u:
            if (elapsed >= 4000ms)
            {
                if ((SDL_GetWindowFlags(m_impl->window) & SDL_WINDOW_MINIMIZED) == 0u)
                    failState("minimize was not observed");
                SDL_RestoreWindow(m_impl->window);
                SDL_RaiseWindow(m_impl->window);
                std::fprintf(stderr, "[m1-lifecycle] restore/foreground requested on SDL_Window\n");
                ++m_impl->testLifecycleStep;
            }
            break;
        case 5u:
            if (elapsed >= 4750ms)
            {
                if ((SDL_GetWindowFlags(m_impl->window) & SDL_WINDOW_MINIMIZED) != 0u)
                    failState("restore was not observed");
                SDL_HideWindow(m_impl->window);
                std::fprintf(stderr, "[m1-lifecycle] hide/occlusion requested on SDL_Window\n");
                ++m_impl->testLifecycleStep;
            }
            break;
        case 6u:
            if (elapsed >= 5250ms)
            {
                if ((SDL_GetWindowFlags(m_impl->window) & SDL_WINDOW_HIDDEN) == 0u)
                    failState("hide/occlusion was not observed");
                SDL_ShowWindow(m_impl->window);
                SDL_RaiseWindow(m_impl->window);
                std::fprintf(stderr, "[m1-lifecycle] show/foreground requested on SDL_Window\n");
                ++m_impl->testLifecycleStep;
            }
            break;
        case 7u:
            if (elapsed >= 5750ms)
            {
                if ((SDL_GetWindowFlags(m_impl->window) & SDL_WINDOW_HIDDEN) != 0u)
                    failState("show/foreground was not observed");
                SDL_SetWindowSize(m_impl->window, 640, 448);
                std::fprintf(stderr, "[m1-lifecycle] resize 640x448 requested on SDL_Window\n");
                ++m_impl->testLifecycleStep;
            }
            break;
        case 8u:
            if (elapsed >= 6250ms)
            {
                int logicalWidth = 0, logicalHeight = 0, pixelWidth = 0, pixelHeight = 0;
                SDL_GetWindowSize(m_impl->window, &logicalWidth, &logicalHeight);
                drawableSize(m_impl->window, &pixelWidth, &pixelHeight);
                const float scale = logicalWidth > 0 ? static_cast<float>(pixelWidth) / logicalWidth : 0.0f;
                if (logicalWidth != 640 || logicalHeight != 448 || pixelWidth <= 0 ||
                    pixelHeight <= 0 || scale <= 0.0f)
                    failState("final drawable size/backing scale was not observed");
                if (m_impl->observedSurfaceChanges < 3u)
                    failState("runtime did not observe at least three direct-surface changes");
                std::fprintf(stderr,
                             "[m1-lifecycle] sequence complete logical=%dx%d pixels=%dx%d "
                             "backing-scale=%.3f surface-changes=%u result=%s\n",
                             logicalWidth, logicalHeight, pixelWidth, pixelHeight, scale,
                             m_impl->observedSurfaceChanges,
                             m_impl->testLifecycleFailed ? "FAIL" : "PASS");
                ++m_impl->testLifecycleStep;
            }
            break;
        default:
            break;
        }
    }
    m_impl->pad->refresh();
}

bool SdlPresentation::shouldClose() const
{
    return !m_impl || m_impl->closeRequested;
}

bool SdlPresentation::currentSurface(rrv::gsbackend::NativeSurface *surface) const
{
#if defined(__APPLE__)
    if (!m_impl || !surface || !m_impl->window || !m_impl->metalView || !m_impl->metalLayer)
        return false;
#else
    if (!m_impl || !surface || !m_impl->window || !m_impl->nativeView || !m_impl->nativeLayer)
        return false;
#endif
    int width = 0;
    int height = 0;
    drawableSize(m_impl->window, &width, &height);
    if (width <= 0 || height <= 0)
        return false;
    int logicalWidth = 0;
    int logicalHeight = 0;
    SDL_GetWindowSize(m_impl->window, &logicalWidth, &logicalHeight);
    *surface = {};
#if defined(__APPLE__)
    surface->kind = rrv::gsbackend::NativeSurfaceKind::MacosViewMetalLayer;
    surface->nativeView = m_impl->metalView;
    surface->nativeLayer = m_impl->metalLayer;
#else
    surface->kind = m_impl->surfaceKind;
    surface->nativeView = m_impl->nativeView;
    surface->nativeLayer = m_impl->nativeLayer;
#endif
    surface->widthPixels = static_cast<uint32_t>(width);
    surface->heightPixels = static_cast<uint32_t>(height);
    surface->backingScale = logicalWidth > 0 ? static_cast<float>(width) / logicalWidth : 1.0f;
    surface->mainThreadPrepared = true;
    if (m_impl->testLifecycle)
    {
        if (m_impl->lastSurfaceWidth != 0u &&
            (m_impl->lastSurfaceWidth != surface->widthPixels ||
             m_impl->lastSurfaceHeight != surface->heightPixels))
            ++m_impl->observedSurfaceChanges;
        m_impl->lastSurfaceWidth = surface->widthPixels;
        m_impl->lastSurfaceHeight = surface->heightPixels;
    }
    return true;
}

std::shared_ptr<HostPadBackend> SdlPresentation::padBackend() const
{
    return m_impl ? m_impl->pad : nullptr;
}

bool SdlPresentation::lifecycleTestEnabled() const
{
    return m_impl && m_impl->testLifecycle;
}

bool SdlPresentation::lifecycleTestComplete(std::string *error) const
{
    if (!lifecycleTestEnabled()) return true;
    if (m_impl->testLifecycleStep <= 8u)
    {
        if (error) *error = "test-only SDL lifecycle sequence did not complete";
        return false;
    }
    if (m_impl->testLifecycleFailed)
    {
        if (error) *error = m_impl->testLifecycleFailure;
        return false;
    }
    return true;
}

void SdlPresentation::pumpCallback(void *context)
{
    static_cast<SdlPresentation *>(context)->pumpEvents();
}

bool SdlPresentation::closeCallback(void *context)
{
    return static_cast<SdlPresentation *>(context)->shouldClose();
}

bool SdlPresentation::surfaceCallback(void *context, rrv::gsbackend::NativeSurface *surface)
{
    return static_cast<SdlPresentation *>(context)->currentSurface(surface);
}
} // namespace rrv::host
