// rrv::host::CursorIdle (src/host/rrv_cursor_idle.h): the pointer is hidden only in full screen, with the menu
// closed, after the mouse has been still for kIdle; activity, the menu and leaving full screen show it.
#include "rrv_cursor_idle.h"

#include <iostream>
#include <string>

namespace {

int failures = 0;

void expect(bool ok, const std::string &what)
{
    if (!ok)
    {
        ++failures;
        std::cerr << "FAIL: " << what << "\n";
    }
}

} // namespace

int main()
{
    using rrv::host::CursorIdle;
    using namespace std::chrono_literals;
    const CursorIdle::Clock::time_point t0 = CursorIdle::Clock::time_point{} + 1h;

    CursorIdle untouched;
    expect(untouched.hidden(true, false, t0), "full screen with a mouse never touched: hidden at once");
    expect(!untouched.hidden(false, false, t0), "a window that is not full screen keeps the pointer");
    expect(!untouched.hidden(true, true, t0), "the open menu keeps the pointer");

    CursorIdle cursor;
    cursor.activity(t0);
    expect(!cursor.hidden(true, false, t0), "shown at the moment of activity");
    expect(!cursor.hidden(true, false, t0 + CursorIdle::kIdle - 1ms), "still shown just before the idle time");
    expect(cursor.hidden(true, false, t0 + CursorIdle::kIdle), "hidden once the mouse has been still for the idle time");
    expect(!cursor.hidden(false, false, t0 + 1h), "never hidden outside full screen, however long the mouse is still");
    expect(!cursor.hidden(true, true, t0 + 1h), "never hidden while the menu is open");
    expect(cursor.hidden(true, false, t0 + 1h), "hidden again when the menu closes and the mouse is still");

    cursor.activity(t0 + 1h);
    expect(!cursor.hidden(true, false, t0 + 1h + 1ms), "motion shows it again at once");
    expect(cursor.hidden(true, false, t0 + 1h + CursorIdle::kIdle), "and it hides again after the idle time");

    if (failures != 0)
    {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "cursor idle tests passed\n";
    return 0;
}
