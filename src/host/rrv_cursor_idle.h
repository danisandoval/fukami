#ifndef RRV_CURSOR_IDLE_H
#define RRV_CURSOR_IDLE_H

// rrv_cursor_idle.h — when the product window hides the mouse pointer.
//
// In full screen the pointer sat on the picture for the whole session (owner
// report, 2026-10-04). It is now hidden while the window is full screen, the
// in-game menu is closed and the mouse has been still for kIdle; any mouse
// motion, button or wheel shows it again at once, as does opening the menu
// (the mouse drives it) or leaving full screen. A window that is not full
// screen keeps the pointer: it is a desktop window like any other.
//
// The policy is separate from SDL so it can be tested without a window
// (tests/cursor_idle_tests.cpp); SdlPresentation::pumpEvents feeds it and
// calls SDL_ShowCursor on a change. Host presentation only: nothing here
// reaches the guest.

#include <chrono>

namespace rrv::host
{
class CursorIdle
{
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::milliseconds kIdle{2000};

    // The mouse moved, a button changed or the wheel turned.
    void activity(Clock::time_point now)
    {
        m_lastActivity = now;
        m_seenActivity = true;
    }

    // Whether the pointer should be hidden now. A session that starts full
    // screen with the mouse never touched hides it from the first call.
    bool hidden(bool fullscreen, bool menuOpen, Clock::time_point now) const
    {
        if (!fullscreen || menuOpen)
            return false;
        return !m_seenActivity || now - m_lastActivity >= kIdle;
    }

private:
    Clock::time_point m_lastActivity{};
    bool m_seenActivity = false;
};
} // namespace rrv::host

#endif // RRV_CURSOR_IDLE_H
