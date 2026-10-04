#include "Common.h"
#include "Pad.h"
#include "runtime/ps2_pad.h"

#include <array>
#include <chrono>

namespace ps2_stubs
{
    namespace
    {
        constexpr uint8_t kPadModeDigital = 0x41;
        constexpr uint8_t kPadModeDualShock = 0x73;
        constexpr uint8_t kPadModeDualShockPressure = 0x79;
        constexpr uint8_t kPadAnalogCenter = 0x80;
        constexpr int32_t kPadTypeDigital = 4;
        constexpr int32_t kPadTypeDualShock = 7;
        constexpr int32_t kPadStateDisconnected = 0;
        constexpr int32_t kPadStateStable = 6;
        constexpr size_t kPadPortCount = 2;
        constexpr size_t kPadSlotCount = 1;

        constexpr uint16_t kPadBtnSelect = 1u << 0;
        constexpr uint16_t kPadBtnL3 = 1u << 1;
        constexpr uint16_t kPadBtnR3 = 1u << 2;
        constexpr uint16_t kPadBtnStart = 1u << 3;
        constexpr uint16_t kPadBtnUp = 1u << 4;
        constexpr uint16_t kPadBtnRight = 1u << 5;
        constexpr uint16_t kPadBtnDown = 1u << 6;
        constexpr uint16_t kPadBtnLeft = 1u << 7;
        constexpr uint16_t kPadBtnL2 = 1u << 8;
        constexpr uint16_t kPadBtnR2 = 1u << 9;
        constexpr uint16_t kPadBtnL1 = 1u << 10;
        constexpr uint16_t kPadBtnR1 = 1u << 11;
        constexpr uint16_t kPadBtnTriangle = 1u << 12;
        constexpr uint16_t kPadBtnCircle = 1u << 13;
        constexpr uint16_t kPadBtnCross = 1u << 14;
        constexpr uint16_t kPadBtnSquare = 1u << 15;

        struct PadInputState
        {
            uint16_t buttons = 0xFFFF; // active-low
            uint8_t rx = kPadAnalogCenter;
            uint8_t ry = kPadAnalogCenter;
            uint8_t lx = kPadAnalogCenter;
            uint8_t ly = kPadAnalogCenter;
            uint8_t l2Pressure = 0;
            uint8_t r2Pressure = 0;
        };

        struct PadPortState
        {
            bool open = false;
            bool analogMode = true;
            bool pressureEnabled = false;
            uint16_t buttonMask = 0xFFFFu;
            uint32_t dmaAddr = 0u;
            uint32_t reqState = 0u;
            bool l2DigitalPressed = false;
            bool r2DigitalPressed = false;
            // scePadSetActAlign bytes 0/1: which direct-data byte drives the
            // small / large motor (PCSX2 PadDualshock2 VibrationMap; 0xff =
            // unmapped, the power-on state).
            uint8_t smallMotorAlign = 0xffu;
            uint8_t largeMotorAlign = 0xffu;
        };

        // PCSX2 PadDualshock2::Poll case 4 (d5f75c9e4): the small motor is
        // the LSB of its byte at full power, the large motor its byte / 255.
        // An align value other than 0 or 1 leaves that motor off.
        HostPadRumble dualShockRumble(uint8_t smallAlign, uint8_t largeAlign, const uint8_t *data)
        {
            const uint8_t small = smallAlign < 2u ? static_cast<uint8_t>(data[smallAlign] & 1u) : 0u;
            const uint8_t large = largeAlign < 2u ? data[largeAlign] : 0u;
            HostPadRumble rumble;
            rumble.lowFrequency = static_cast<float>(large) * (1.0f / 255.0f);
            rumble.highFrequency = small ? 1.0f : 0.0f;
            return rumble;
        }

        std::mutex g_padOverrideMutex;
        std::mutex g_padStateMutex;
        std::mutex g_padDiagMutex;
        bool g_padOverrideEnabled = false;
        PadInputState g_padOverrideState{};
        PadPortState g_padPorts[kPadPortCount]{};
        std::chrono::steady_clock::time_point g_lastPadConversionLog{};
        std::chrono::steady_clock::time_point g_lastActAlignLog{};
        std::chrono::steady_clock::time_point g_lastActDirectLog{};
        uint8_t axisToByte(float axis)
        {
            axis = std::clamp(axis, -1.0f, 1.0f);
            if (axis <= -1.0f)
                return 0;
            if (axis >= 1.0f)
                return 255;
            const float mapped = (axis + 1.0f) * 127.5f;
            return static_cast<uint8_t>(std::lround(mapped));
        }

        struct Stick
        {
            float x;
            float y;
        };

        Stick radialDeadzone(Stick value, float deadzone)
        {
            value.x = std::clamp(value.x, -1.0f, 1.0f);
            value.y = std::clamp(value.y, -1.0f, 1.0f);
            const float magnitude = std::sqrt(value.x * value.x + value.y * value.y);
            if (magnitude <= deadzone || magnitude <= 0.0f)
                return {};
            const float rescaled = std::min(1.0f, (magnitude - deadzone) / (1.0f - deadzone));
            const float scale = rescaled / magnitude;
            return {value.x * scale, value.y * scale};
        }

        bool triggerDigital(bool wasPressed, float trigger, bool digitalButton)
        {
            constexpr float kPressThreshold = 0.50f;
            constexpr float kReleaseThreshold = 0.40f;
            return digitalButton || (wasPressed ? trigger >= kReleaseThreshold : trigger >= kPressThreshold);
        }

        PadInputState convertHostState(const HostPadState &host, const HostPadCapabilities &capabilities,
                                       PadPortState &portState, float deadzone)
        {
            PadInputState state{};
            const float l2 = std::clamp(host.leftTrigger, 0.0f, 1.0f);
            const float r2 = std::clamp(host.rightTrigger, 0.0f, 1.0f);
            const bool l2DigitalFallback = !capabilities.analogTriggers && (host.pressedButtons & HostPadButtonL2) != 0;
            const bool r2DigitalFallback = !capabilities.analogTriggers && (host.pressedButtons & HostPadButtonR2) != 0;
            portState.l2DigitalPressed = triggerDigital(portState.l2DigitalPressed, l2, l2DigitalFallback);
            portState.r2DigitalPressed = triggerDigital(portState.r2DigitalPressed, r2, r2DigitalFallback);
            const uint32_t pressed = (host.pressedButtons & ~(static_cast<uint32_t>(HostPadButtonL2) |
                static_cast<uint32_t>(HostPadButtonR2))) |
                (portState.l2DigitalPressed ? HostPadButtonL2 : 0u) |
                (portState.r2DigitalPressed ? HostPadButtonR2 : 0u);
            state.buttons = static_cast<uint16_t>(~pressed);
            if (!portState.analogMode)
                return state;
            const Stick left = radialDeadzone({host.leftX, host.leftY}, deadzone);
            const Stick right = radialDeadzone({host.rightX, host.rightY}, deadzone);
            // Host Y is positive-up.  DualShock axis bytes are positive-down.
            state.lx = axisToByte(left.x);
            state.ly = axisToByte(-left.y);
            state.rx = axisToByte(right.x);
            state.ry = axisToByte(-right.y);
            state.l2Pressure = capabilities.analogTriggers ?
                static_cast<uint8_t>(std::lround(l2 * 255.0f)) : (l2DigitalFallback ? 0xFFu : 0u);
            state.r2Pressure = capabilities.analogTriggers ?
                static_cast<uint8_t>(std::lround(r2 * 255.0f)) : (r2DigitalFallback ? 0xFFu : 0u);
            return state;
        }

        void resetPadStateLocked()
        {
            for (PadPortState &portState : g_padPorts)
            {
                portState = PadPortState{};
            }
        }

        PadPortState *lookupPadPortStateLocked(int port, int slot)
        {
            if (port < 0 || port >= static_cast<int>(kPadPortCount))
            {
                return nullptr;
            }
            if (slot < 0 || slot >= static_cast<int>(kPadSlotCount))
            {
                return nullptr;
            }
            return &g_padPorts[port];
        }

        void initializePadPortLocked(PadPortState &portState, uint32_t dmaAddr)
        {
            portState.open = true;
            portState.analogMode = true;
            portState.pressureEnabled = false;
            portState.buttonMask = 0xFFFFu;
            portState.dmaAddr = dmaAddr;
            portState.reqState = 0u;
            portState.l2DigitalPressed = false;
            portState.r2DigitalPressed = false;
        }

        std::array<uint8_t, 6> readActuatorDiagnosticBytes(uint8_t *rdram, uint32_t address)
        {
            std::array<uint8_t, 6> bytes{};
            for (uint32_t i = 0; i < bytes.size() && address <= UINT32_MAX - i; ++i)
            {
                // Resolve one guest byte at a time.  A non-null pointer for
                // the first byte does not prove that six bytes remain in the
                // final mapped guest page.
                if (const uint8_t *byte = getMemPtr(rdram, address + i))
                    bytes[i] = *byte;
            }
            return bytes;
        }

        uint8_t pressureValue(const PadInputState &state, const PadPortState &portState, uint16_t mask)
        {
            if (!portState.pressureEnabled)
            {
                return 0u;
            }
            if ((portState.buttonMask & mask) == 0u)
            {
                return 0u;
            }
            if (mask == kPadBtnL2)
            {
                return state.l2Pressure ? state.l2Pressure : (((state.buttons & mask) == 0u) ? 0xFFu : 0u);
            }
            if (mask == kPadBtnR2)
            {
                return state.r2Pressure ? state.r2Pressure : (((state.buttons & mask) == 0u) ? 0xFFu : 0u);
            }
            return ((state.buttons & mask) == 0u) ? 0xFFu : 0u;
        }

        void fillPadStatus(uint8_t *data, const PadInputState &state, const PadPortState &portState)
        {
            std::memset(data, 0, 32);
            data[1] = !portState.analogMode ? kPadModeDigital :
                (portState.pressureEnabled ? kPadModeDualShockPressure : kPadModeDualShock);
            data[2] = static_cast<uint8_t>(state.buttons & 0xFFu);
            data[3] = static_cast<uint8_t>((state.buttons >> 8) & 0xFFu);
            data[4] = state.rx;
            data[5] = state.ry;
            data[6] = state.lx;
            data[7] = state.ly;
            data[8] = pressureValue(state, portState, kPadBtnRight);
            data[9] = pressureValue(state, portState, kPadBtnLeft);
            data[10] = pressureValue(state, portState, kPadBtnUp);
            data[11] = pressureValue(state, portState, kPadBtnDown);
            data[12] = pressureValue(state, portState, kPadBtnTriangle);
            data[13] = pressureValue(state, portState, kPadBtnCircle);
            data[14] = pressureValue(state, portState, kPadBtnCross);
            data[15] = pressureValue(state, portState, kPadBtnSquare);
            data[16] = pressureValue(state, portState, kPadBtnL1);
            data[17] = pressureValue(state, portState, kPadBtnR1);
            data[18] = pressureValue(state, portState, kPadBtnL2);
            data[19] = pressureValue(state, portState, kPadBtnR2);
        }

        bool readPadPortData(int port, int slot, PS2Runtime *runtime, uint8_t *outData)
        {
            if (!outData)
            {
                return false;
            }

            PadPortState portState;
            {
                std::lock_guard<std::mutex> lock(g_padStateMutex);
                const PadPortState *sharedPortState = lookupPadPortStateLocked(port, slot);
                if (!sharedPortState || !sharedPortState->open)
                {
                    return false;
                }
                portState = *sharedPortState;
            }

            const auto admitted = runtime->gate3PadSampleV1(port);
            portState.analogMode = portState.analogMode && admitted.capabilities.analogSticks;
            PadInputState state = convertHostState(
                admitted.state, admitted.capabilities, portState, admitted.deadzone);

            // Hysteresis is per emulated port.  Publish only these local
            // details; all host data remains outside the libpad lock.
            {
                std::lock_guard<std::mutex> lock(g_padStateMutex);
                if (PadPortState *shared = lookupPadPortStateLocked(port, slot))
                {
                    shared->l2DigitalPressed = portState.l2DigitalPressed;
                    shared->r2DigitalPressed = portState.r2DigitalPressed;
                }
            }

            fillPadStatus(outData, state, portState);

            return true;
        }
    }

    void PadSyncCallback(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        setReturnS32(ctx, 0);
    }

    void scePadEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        {
            std::lock_guard<std::mutex> lock(g_padStateMutex);
            resetPadStateLocked();
        }
        if (runtime)
            for (int port = 0; port < static_cast<int>(kPadPortCount); ++port)
                runtime->padBackend().setRumble(port, HostPadRumble{});
        setReturnS32(ctx, 1);
    }

    void scePadEnterPressMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                           static_cast<int>(getRegU32(ctx, 5)));
        if (!portState || !portState->open)
        {
            setReturnS32(ctx, 0);
            return;
        }

        portState->buttonMask = 0xFFFFu;
        portState->pressureEnabled = true;
        portState->reqState = 0u;
        if (runtime && runtime->padBackend().diagnosticsEnabled())
            std::printf("[pad] pressure=enabled port=%u slot=%u\n", getRegU32(ctx, 4), getRegU32(ctx, 5));
        setReturnS32(ctx, 1);
    }

    void scePadExitPressMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                           static_cast<int>(getRegU32(ctx, 5)));
        if (!portState || !portState->open)
        {
            setReturnS32(ctx, 0);
            return;
        }

        portState->buttonMask = 0u;
        portState->pressureEnabled = false;
        portState->reqState = 0u;
        if (runtime && runtime->padBackend().diagnosticsEnabled())
            std::printf("[pad] pressure=disabled port=%u slot=%u\n", getRegU32(ctx, 4), getRegU32(ctx, 5));
        setReturnS32(ctx, 1);
    }

    void scePadGetButtonMask(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        const PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                                 static_cast<int>(getRegU32(ctx, 5)));
        const uint16_t mask = portState ? portState->buttonMask : 0xFFFFu;
        setReturnS32(ctx, static_cast<int32_t>(mask));
    }

    void scePadGetDmaStr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        const PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                                 static_cast<int>(getRegU32(ctx, 5)));
        const uint32_t dmaAddr = portState ? portState->dmaAddr : getRegU32(ctx, 6);
        setReturnU32(ctx, dmaAddr);
    }

    void scePadGetFrameCount(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        static std::atomic<uint32_t> frameCount{0};
        setReturnU32(ctx, frameCount++);
    }

    void scePadGetModVersion(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        // Arbitrary non-zero module version.
        setReturnS32(ctx, 0x0200);
    }

    void scePadGetPortMax(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        setReturnS32(ctx, 2);
    }

    void scePadGetReqState(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        const PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                                 static_cast<int>(getRegU32(ctx, 5)));
        setReturnS32(ctx, static_cast<int32_t>(portState ? portState->reqState : 0u));
    }

    void scePadGetSlotMax(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        // Most games use one slot unless multitap is active.
        setReturnS32(ctx, 1);
    }

    void scePadGetState(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        frontendDiagInit();
        frontendDiagCountPad("scePadGetState");
        (void)rdram;
        (void)runtime;
        bool open = false;
        {
            std::lock_guard<std::mutex> lock(g_padStateMutex);
            const PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                                     static_cast<int>(getRegU32(ctx, 5)));
            open = portState && portState->open;
        }
        // The guest sees only the producer-admitted connection state.
        const bool connected = runtime->gate3PadSampleV1(static_cast<int>(getRegU32(ctx, 4))).state.connected;
        setReturnS32(ctx, (open && connected) ? kPadStateStable : kPadStateDisconnected);
    }

    void scePadInfoAct(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const auto admitted = runtime->gate3PadSampleV1(static_cast<int>(getRegU32(ctx, 4)));
        const int32_t act = static_cast<int32_t>(getRegU32(ctx, 6));
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        const PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                                 static_cast<int>(getRegU32(ctx, 5)));
        if (!portState || !portState->open)
        {
            setReturnS32(ctx, 0);
            return;
        }

        if (!admitted.state.connected || !admitted.capabilities.rumble)
        {
            setReturnS32(ctx, 0);
            return;
        }
        if (act < 0)
        {
            setReturnS32(ctx, 2); // small + large motors
            return;
        }
        setReturnS32(ctx, (act < 2) ? 1 : 0);
    }

    void scePadInfoComb(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        // No combined modes reported.
        setReturnS32(ctx, 0);
    }

    void scePadInfoMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;

        const auto admitted = runtime->gate3PadSampleV1(static_cast<int>(getRegU32(ctx, 4)));
        const int32_t infoMode = static_cast<int32_t>(getRegU32(ctx, 6)); // a2
        const int32_t index = static_cast<int32_t>(getRegU32(ctx, 7));    // a3
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        const PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                                 static_cast<int>(getRegU32(ctx, 5)));
        if (!portState || !portState->open)
        {
            setReturnS32(ctx, 0);
            return;
        }

        if (!admitted.state.connected)
        {
            setReturnS32(ctx, 0);
            return;
        }
        const int32_t currentId = (portState->analogMode && admitted.capabilities.analogSticks)
            ? kPadTypeDualShock : kPadTypeDigital;
        switch (infoMode)
        {
        case 1: // PAD_MODECURID
            setReturnS32(ctx, currentId);
            return;
        case 2: // PAD_MODECUREXID
            setReturnS32(ctx, currentId);
            return;
        case 3: // PAD_MODECUROFFS
            setReturnS32(ctx, 0);
            return;
        case 4: // PAD_MODETABLE
            if (index == -1)
            {
                setReturnS32(ctx, 1); // one available mode
            }
            else if (index == 0)
            {
                setReturnS32(ctx, currentId);
            }
            else
            {
                setReturnS32(ctx, 0);
            }
            return;
        default:
            setReturnS32(ctx, 0);
            return;
        }
    }

    void scePadInfoPressMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        const auto admitted = runtime->gate3PadSampleV1(static_cast<int>(getRegU32(ctx, 4)));
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        const PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                                 static_cast<int>(getRegU32(ctx, 5)));
        setReturnS32(ctx, (portState && portState->open && admitted.state.connected) ? 1 : 0);
    }

    void scePadInit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        frontendDiagInit();
        frontendDiagCountPad("scePadInit");
        (void)rdram;
        (void)runtime;
        {
            std::lock_guard<std::mutex> lock(g_padStateMutex);
            resetPadStateLocked();
        }
        setReturnS32(ctx, 1);
    }

    void scePadInit2(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        scePadInit(rdram, ctx, runtime);
    }

    void scePadPortClose(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                           static_cast<int>(getRegU32(ctx, 5)));
        if (!portState)
        {
            setReturnS32(ctx, 0);
            return;
        }

        portState->open = false;
        portState->pressureEnabled = false;
        portState->reqState = 0u;
        portState->l2DigitalPressed = false;
        portState->r2DigitalPressed = false;
        if (runtime)
            runtime->padBackend().setRumble(static_cast<int>(getRegU32(ctx, 4)), HostPadRumble{});
        setReturnS32(ctx, 1);
    }

    void scePadPortOpen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        frontendDiagInit();
        frontendDiagCountPad("scePadPortOpen");
        (void)runtime;
        const uint32_t dmaAddr = getRegU32(ctx, 6);
        uint8_t *dmaStr = getMemPtr(rdram, dmaAddr);
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                           static_cast<int>(getRegU32(ctx, 5)));
        if (!portState || (dmaAddr != 0u && !dmaStr))
        {
            setReturnS32(ctx, 0);
            return;
        }

        portState->open = true;
        portState->analogMode = true;
        portState->pressureEnabled = false;
        portState->buttonMask = 0xFFFFu;
        portState->dmaAddr = dmaAddr;
        portState->reqState = 0u;
        portState->l2DigitalPressed = false;
        portState->r2DigitalPressed = false;
        if (dmaStr)
        {
            std::memset(dmaStr, 0, 32);
        }
        setReturnS32(ctx, 1);
    }

    void scePadRead(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        frontendDiagInit();
        frontendDiagCountPad("scePadRead");
        const int port = static_cast<int>(getRegU32(ctx, 4));
        const int slot = static_cast<int>(getRegU32(ctx, 5));
        const uint32_t dataAddr = getRegU32(ctx, 6);
        uint8_t *data = getMemPtr(rdram, dataAddr);
        if (!data)
        {
            setReturnS32(ctx, 0);
            return;
        }

        if (!readPadPortData(port, slot, runtime, data))
        {
            setReturnS32(ctx, 0);
            return;
        }

        setReturnS32(ctx, 1);
    }

    void scePadReqIntToStr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        const uint32_t state = getRegU32(ctx, 4);
        const uint32_t strAddr = getRegU32(ctx, 5);
        char *buf = reinterpret_cast<char *>(getMemPtr(rdram, strAddr));
        if (!buf)
        {
            setReturnS32(ctx, -1);
            return;
        }

        const char *text = (state == 0) ? "COMPLETE" : "BUSY";
        std::strncpy(buf, text, 31);
        buf[31] = '\0';
        setReturnS32(ctx, 0);
    }

    void scePadSetActAlign(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (runtime && runtime->padBackend().diagnosticsEnabled())
        {
            std::lock_guard<std::mutex> diagLock(g_padDiagMutex);
            const auto now = std::chrono::steady_clock::now();
            if (now - g_lastActAlignLog >= std::chrono::seconds(1))
            {
                g_lastActAlignLog = now;
                const auto bytes = readActuatorDiagnosticBytes(rdram, getRegU32(ctx, 6));
                std::printf("[pad] scePadSetActAlign port=%u slot=%u bytes=%02x %02x %02x %02x %02x %02x\n",
                    getRegU32(ctx, 4), getRegU32(ctx, 5), bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
            }
        }
        {
            const auto align = readActuatorDiagnosticBytes(rdram, getRegU32(ctx, 6));
            std::lock_guard<std::mutex> lock(g_padStateMutex);
            if (PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                                   static_cast<int>(getRegU32(ctx, 5))))
            {
                portState->smallMotorAlign = align[0];
                portState->largeMotorAlign = align[1];
            }
        }
        setReturnS32(ctx, 1);
    }

    void scePadSetActDirect(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (runtime && runtime->padBackend().diagnosticsEnabled())
        {
            std::lock_guard<std::mutex> diagLock(g_padDiagMutex);
            const auto now = std::chrono::steady_clock::now();
            if (now - g_lastActDirectLog >= std::chrono::seconds(1))
            {
                g_lastActDirectLog = now;
                const auto bytes = readActuatorDiagnosticBytes(rdram, getRegU32(ctx, 6));
                std::printf("[pad] scePadSetActDirect port=%u slot=%u bytes=%02x %02x %02x %02x %02x %02x\n",
                    getRegU32(ctx, 4), getRegU32(ctx, 5), bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
            }
        }
        if (runtime)
        {
            const auto data = readActuatorDiagnosticBytes(rdram, getRegU32(ctx, 6));
            const int port = static_cast<int>(getRegU32(ctx, 4));
            bool open = false;
            uint8_t smallAlign = 0xffu, largeAlign = 0xffu;
            {
                std::lock_guard<std::mutex> lock(g_padStateMutex);
                if (const PadPortState *portState = lookupPadPortStateLocked(port, static_cast<int>(getRegU32(ctx, 5))))
                {
                    open = portState->open;
                    smallAlign = portState->smallMotorAlign;
                    largeAlign = portState->largeMotorAlign;
                }
            }
            if (open)
                runtime->padBackend().setRumble(port, dualShockRumble(smallAlign, largeAlign, data.data()));
        }
        setReturnS32(ctx, 1);
    }

    void scePadSetButtonInfo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                           static_cast<int>(getRegU32(ctx, 5)));
        if (portState && portState->open)
        {
            // SetButtonInfo selects the twelve pressure response bytes,
            // not the active-low digital button positions. RR5's
            // generated EnterPressMode calls this API with 0xfff.
            constexpr uint16_t pressureButtons[] = {
                kPadBtnRight, kPadBtnLeft, kPadBtnUp, kPadBtnDown,
                kPadBtnTriangle, kPadBtnCircle, kPadBtnCross, kPadBtnSquare,
                kPadBtnL1, kPadBtnR1, kPadBtnL2, kPadBtnR2};
            const uint32_t responseMask = getRegU32(ctx, 6) & 0x0FFFu;
            portState->buttonMask = 0u;
            for (unsigned index = 0; index < 12; ++index)
            {
                if ((responseMask & (1u << index)) != 0u)
                    portState->buttonMask |= pressureButtons[index];
            }
            portState->pressureEnabled = responseMask != 0u;
            portState->reqState = 0u;
            if (runtime && runtime->padBackend().diagnosticsEnabled())
                std::printf("[pad] pressure mask=%04x port=%u slot=%u\n", portState->buttonMask,
                    getRegU32(ctx, 4), getRegU32(ctx, 5));
        }
        setReturnS32(ctx, 1);
    }

    void scePadSetMainMode(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                           static_cast<int>(getRegU32(ctx, 5)));
        if (!portState || !portState->open)
        {
            setReturnS32(ctx, 0);
            return;
        }

        portState->analogMode = (getRegU32(ctx, 6) != 0u);
        portState->reqState = 0u;
        if (runtime && runtime->padBackend().diagnosticsEnabled())
            std::printf("[pad] mode=%s port=%u slot=%u\n", portState->analogMode ? "analog" : "digital",
                getRegU32(ctx, 4), getRegU32(ctx, 5));
        setReturnS32(ctx, 1);
    }

    void scePadSetReqState(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        std::lock_guard<std::mutex> lock(g_padStateMutex);
        PadPortState *portState = lookupPadPortStateLocked(static_cast<int>(getRegU32(ctx, 4)),
                                                           static_cast<int>(getRegU32(ctx, 5)));
        if (portState && portState->open)
        {
            portState->reqState = static_cast<uint32_t>(getRegU32(ctx, 6) ? 1u : 0u);
        }
        setReturnS32(ctx, 1);
    }

    void scePadSetVrefParam(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        setReturnS32(ctx, 1);
    }

    void scePadSetWarningLevel(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)rdram;
        (void)runtime;
        setReturnS32(ctx, 0);
    }

    void scePadStateIntToStr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        (void)runtime;
        const uint32_t state = getRegU32(ctx, 4);
        const uint32_t strAddr = getRegU32(ctx, 5);
        char *buf = reinterpret_cast<char *>(getMemPtr(rdram, strAddr));
        if (!buf)
        {
            setReturnS32(ctx, -1);
            return;
        }

        const char *text = "UNKNOWN";
        if (state == 6)
        {
            text = "STABLE";
        }
        else if (state == 1)
        {
            text = "FINDPAD";
        }
        else if (state == 0)
        {
            text = "DISCONNECTED";
        }

        std::strncpy(buf, text, 31);
        buf[31] = '\0';
        setReturnS32(ctx, 0);
    }

    void setPadOverrideState(uint16_t buttons, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry)
    {
        std::lock_guard<std::mutex> lock(g_padOverrideMutex);
        g_padOverrideEnabled = true;
        g_padOverrideState.buttons = buttons;
        g_padOverrideState.lx = lx;
        g_padOverrideState.ly = ly;
        g_padOverrideState.rx = rx;
        g_padOverrideState.ry = ry;
    }

    void clearPadOverrideState()
    {
        std::lock_guard<std::mutex> lock(g_padOverrideMutex);
        g_padOverrideEnabled = false;
        g_padOverrideState = PadInputState{};
    }
}
