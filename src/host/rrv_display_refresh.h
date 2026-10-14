#ifndef RRV_DISPLAY_REFRESH_H
#define RRV_DISPLAY_REFRESH_H

// rrv_display_refresh.h — which refresh rate the display gets while the product window is full screen.
//
// The game makes 59.94 pictures a second. A display whose rate is a multiple of 60 can show each of them for the
// same number of refreshes; any other rate cannot (165 Hz: 3-3-3-2, a 6 ms step every fourth picture; owner report,
// 2026-10-12, "frame pacing is not right" on a 165 Hz laptop panel and a 240 Hz monitor). So in full screen the
// display is switched to a multiple of 60 it supports at its current resolution, and switched back when the window
// leaves full screen or closes. `[display] refresh_rate`:
//   auto            a display already at a multiple of 60 stays; another goes to the highest multiple of 60 it
//                   supports below its rate (165 -> 120, 144 -> 120, 75 -> 60), or the lowest when there is none
//   60|120|180|240  that rate when the display supports it, else as auto
//   off             the display is left alone
// It is also left alone with `[display] vrr = on`: a variable-refresh display shows each picture at its own time
// whatever its rate (measured on a FreeSync laptop panel: switching it to 120 Hz made those times coarser).
// Windows only (ChangeDisplaySettingsEx with CDS_FULLSCREEN: a temporary mode that Windows also undoes when the
// process ends); macOS and Linux keep the desktop's rate.
//
// The choice is separate from the Win32 calls so it can be tested without a display
// (tests/display_refresh_tests.cpp); SdlPresentation applies it. Host presentation only: nothing here reaches the
// guest.

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace rrv::host::displayrefresh
{
inline constexpr std::array<int, 4> kRates{60, 120, 180, 240};

// The rate to switch the display to (a value of `supported`), or 0 to leave it as it is.
// `setting` is the value of `[display] refresh_rate`; `supported` the rates the display offers at its current
// resolution; `current` the rate it has now. Windows lists a 59.94 Hz mode as 59 and a 119.88 Hz one as 119: they
// count as the 60 and the 120, after the exact ones.
inline int choose(std::string_view setting, const std::vector<int> &supported, int current)
{
    if (setting == "off")
        return 0;
    const auto offered = [&](int rate) {
        for (const int candidate : {rate, rate - 1})
            if (std::find(supported.begin(), supported.end(), candidate) != supported.end())
                return candidate;
        return 0;
    };
    int wanted = 0;
    for (const int rate : kRates)
        if (setting == std::to_string(rate))
            wanted = offered(rate);
    if (!wanted)
    {
        // auto, or a rate this display does not have.
        for (const int rate : kRates)
            if (current == rate || current == rate - 1)
                return 0; // already a multiple of 60
        for (const int rate : kRates)
            if (const int mode = offered(rate); mode && (rate < current || !wanted))
                wanted = mode;
    }
    return wanted == current ? 0 : wanted;
}
} // namespace rrv::host::displayrefresh

#endif // RRV_DISPLAY_REFRESH_H
