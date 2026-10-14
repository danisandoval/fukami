// rrv::host::padslots (src/host/rrv_pad_slots.h): which controller is PS2 port 0 and which is port 1. The first
// controller is player 1, the second player 2, a third is ignored, slots never shift, and one controller never
// holds two slots.
#include "rrv_pad_slots.h"

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
    using namespace rrv::host::padslots;

    SlotIds ids = emptySlots();
    expect(anyEmpty(ids), "a fresh table has empty slots");
    expect(slotOf(ids, 0) == -1, "nothing is in a slot at the start");

    expect(assign(ids, 7) == 0, "the first controller takes port 0");
    expect(assign(ids, 9) == 1, "the second controller takes port 1");
    expect(!anyEmpty(ids), "two controllers fill both slots");
    expect(assign(ids, 11) == -1, "a third controller is ignored");
    expect(slotOf(ids, 7) == 0 && slotOf(ids, 9) == 1 && slotOf(ids, 11) == -1,
           "a third controller changes neither slot");

    expect(assign(ids, 7) == -1 && assign(ids, 9) == -1, "an id already in a slot is never assigned again");
    expect(ids[0] == 7 && ids[1] == 9, "a refused assign leaves the table unchanged");

    expect(release(ids, 7) == 0, "removing the port-0 controller frees port 0");
    expect(slotOf(ids, 9) == 1 && ids[1] == 9, "port 1 keeps its pad when port 0 is removed (no shift)");
    expect(anyEmpty(ids), "a freed slot is empty");
    expect(release(ids, 7) == -1, "removing an id that holds no slot does nothing");
    expect(assign(ids, 11) == 0, "the next controller fills the freed slot 0, not 1");
    expect(ids[0] == 11 && ids[1] == 9, "slot 0 holds the new controller and slot 1 the same one as before");
    expect(assign(ids, 9) == -1, "the port-1 controller cannot also take slot 0");

    expect(release(ids, 9) == 1, "removing the port-1 controller frees port 1");
    expect(ids[0] == 11, "port 0 keeps its pad when port 1 is removed");
    expect(assign(ids, 3) == 1, "a new controller takes the empty port 1");

    SlotIds zero = emptySlots();
    expect(assign(zero, 0) == 0, "SDL instance id 0 is a valid id");
    expect(slotOf(zero, 0) == 0 && assign(zero, 0) == -1, "id 0 never holds two slots");
    expect(assign(zero, -1) == -1 && release(zero, -1) == -1, "an invalid id never takes or frees a slot");
    expect(assign(zero, 1) == 1, "the empty marker is not confused with an id");

    // promote(): a lone slot-1 controller becomes port 0.
    SlotIds lone = emptySlots();
    expect(!promote(lone) && lone[0] == kEmpty && lone[1] == kEmpty, "promote on an empty table does nothing");
    assign(lone, 4);
    expect(!promote(lone) && lone[0] == 4 && lone[1] == kEmpty, "promote with only slot 0 held does nothing");
    assign(lone, 5);
    expect(!promote(lone) && lone[0] == 4 && lone[1] == 5, "promote with both slots held does nothing");
    release(lone, 4);
    expect(promote(lone), "promote moves a lone slot-1 controller");
    expect(lone[0] == 5 && lone[1] == kEmpty, "the promoted controller holds slot 0 and slot 1 is empty");
    expect(slotOf(lone, 5) == 0 && anyEmpty(lone), "the promoted id is found in slot 0 only");
    expect(!promote(lone), "a second promote does nothing");
    expect(assign(lone, 6) == 1, "the next controller after a promotion takes the empty port 1");
    SlotIds zeroId = emptySlots();
    assign(zeroId, 3);
    assign(zeroId, 0);
    release(zeroId, 3);
    expect(promote(zeroId) && zeroId[0] == 0 && zeroId[1] == kEmpty, "id 0 is promoted like any other id");

    if (failures == 0)
        std::cout << "pad slot tests passed\n";
    return failures == 0 ? 0 : 1;
}
