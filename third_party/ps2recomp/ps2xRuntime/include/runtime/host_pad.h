#pragma once

#include <cstdint>
#include <memory>

// This is deliberately a host-facing representation.  Its button bits are
// active-high and its axes use a conventional positive-up Y direction.  PS2
// packet details must not leak across this boundary: Windows and Linux
// backends will use this interface too.
enum HostPadButton : uint32_t
{
    HostPadButtonSelect   = 1u << 0,
    HostPadButtonL3       = 1u << 1,
    HostPadButtonR3       = 1u << 2,
    HostPadButtonStart    = 1u << 3,
    HostPadButtonUp       = 1u << 4,
    HostPadButtonRight    = 1u << 5,
    HostPadButtonDown     = 1u << 6,
    HostPadButtonLeft     = 1u << 7,
    HostPadButtonL2       = 1u << 8,
    HostPadButtonR2       = 1u << 9,
    HostPadButtonL1       = 1u << 10,
    HostPadButtonR1       = 1u << 11,
    HostPadButtonTriangle = 1u << 12,
    HostPadButtonCircle   = 1u << 13,
    HostPadButtonCross    = 1u << 14,
    HostPadButtonSquare   = 1u << 15,
};

struct HostPadState
{
    bool connected = false;
    uint64_t deviceId = 0;
    uint32_t pressedButtons = 0; // active-high HostPadButton bits
    float leftX = 0.0f;
    float leftY = 0.0f;          // positive is up
    float rightX = 0.0f;
    float rightY = 0.0f;         // positive is up
    float leftTrigger = 0.0f;
    float rightTrigger = 0.0f;
};

struct HostPadCapabilities
{
    bool analogSticks = false;
    bool analogTriggers = false;
    bool rumble = false; // I4 intentionally deferred.
};

struct HostPadRumble
{
    float lowFrequency = 0.0f;
    float highFrequency = 0.0f;
};

class HostPadBackend
{
public:
    virtual ~HostPadBackend() = default;
    virtual HostPadState snapshot(unsigned port) = 0;
    virtual HostPadCapabilities capabilities(unsigned port) const = 0;

    // Kept as a no-op seam until the separately-scoped I4 rumble work.
    virtual void setRumble(unsigned, HostPadRumble) {}
    virtual void stopRumble(unsigned) {}
};

// Apple implementation is supplied by an Objective-C++ TU only on Apple
// desktop builds.  Non-Apple backends never need to include Apple headers.
#if defined(__APPLE__) && !defined(PLATFORM_VITA)
std::shared_ptr<HostPadBackend> createGameControllerHostPadBackend();
#endif
