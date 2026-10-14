// rrv::host::displayrefresh::choose (src/host/rrv_display_refresh.h): the refresh rate a full-screen window asks
// the display for: a multiple of 60 the display offers, by the `[display] refresh_rate` setting.
#include "rrv_display_refresh.h"

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
    using rrv::host::displayrefresh::choose;
    const std::vector<int> laptop{48, 60, 75, 100, 120, 165};
    const std::vector<int> monitor{23, 24, 25, 29, 30, 50, 59, 60, 97, 100, 119, 120, 240};
    const std::vector<int> office{59, 60, 75};
    const std::vector<int> ntsc{59};
    const std::vector<int> odd{50, 75, 144};

    expect(choose("auto", laptop, 165) == 120, "a 165 Hz panel goes to 120 Hz, the highest multiple of 60 below its rate");
    expect(choose("auto", laptop, 60) == 0, "a display already at 60 Hz stays");
    expect(choose("auto", laptop, 120) == 0, "a display already at 120 Hz stays");
    expect(choose("auto", laptop, 100) == 60, "from 100 Hz: 60, not the 120 above it");
    expect(choose("auto", laptop, 48) == 60, "from 48 Hz there is no multiple below: the lowest, 60");
    expect(choose("auto", monitor, 240) == 0, "a 240 Hz monitor at 240 Hz stays");
    expect(choose("auto", monitor, 120) == 0, "a 240 Hz monitor at 120 Hz stays");
    expect(choose("auto", monitor, 119) == 0, "119 (119.88 Hz) is the 120: stays");
    expect(choose("auto", monitor, 100) == 60, "a 240 Hz monitor at 100 Hz goes to 60");
    expect(choose("auto", office, 75) == 60, "a 75 Hz display goes to 60");
    expect(choose("auto", odd, 144) == 0, "a display with no multiple of 60 is left alone");

    expect(choose("60", laptop, 165) == 60, "an explicit 60");
    expect(choose("120", monitor, 240) == 120, "an explicit 120 on a display that could do 240");
    expect(choose("240", laptop, 165) == 120, "a rate the display does not have falls back to auto");
    expect(choose("180", monitor, 240) == 0, "no 180 on this monitor: as auto, and 240 Hz stays");
    expect(choose("60", office, 60) == 0, "an explicit rate that is already set changes nothing");

    expect(choose("auto", ntsc, 59) == 0, "a 59.94 Hz mode listed as 59 is the 60: nothing to change");
    expect(choose("60", ntsc, 75) == 59, "and it is what 60 selects when there is no exact 60");
    expect(choose("60", office, 75) == 60, "the exact 60 is preferred to the 59");

    expect(choose("off", laptop, 165) == 0, "off never changes the display");
    expect(choose("off", office, 75) == 0, "off never changes the display (75 Hz)");
    expect(choose("auto", {}, 60) == 0, "no modes known: nothing to change");

    if (failures != 0)
    {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "display refresh tests passed\n";
    return 0;
}
