#ifndef RRV_PAD_SLOTS_H
#define RRV_PAD_SLOTS_H

// rrv_pad_slots.h — which physical controller is PS2 port 0 and which is port 1.
//
// Two stable slots (owner decision 2026-10-11, public issue danisandoval/fukami#2:
// "a second controller also can't be detected"). A newly opened controller takes
// the lowest empty slot, so the first is player 1 (port 0) and the second player 2
// (port 1); a third is ignored. When the controller in slot 0 is removed and a third
// controller is waiting (connected, usable, not in a slot), slot 1 keeps its pad and
// the waiting controller fills slot 0. When NO other usable controller is waiting,
// which includes the plain two-controller case, the slot-1 controller is promoted to
// slot 0 (promote(); owner, 2026-10-11), so a lone pad is always player 1 (one
// physical pad seen twice, e.g. over Bluetooth and USB, would otherwise stay player 2
// with the keyboard on port 0). The honest consequence: with exactly two controllers,
// removing player 1's promotes player 2's to player 1 (a dropout of P1 in a
// two-player race swaps the pads), and a pad that comes back becomes player 2. Slots
// only stay put while a third controller is waiting. One controller (an SDL instance
// id) never holds two slots.
//
// The rule is separate from SDL so it can be tested without a device
// (tests/pad_slots_tests.cpp); SdlHostPadBackend (rrv_sdl_presentation.cpp) keeps
// the ids in step with the controllers it has open. Host input only: nothing here
// reaches the guest except which port a pad answers on.

#include <array>
#include <cstdint>

namespace rrv::host::padslots
{
constexpr unsigned kPorts = 2;
constexpr std::int64_t kEmpty = -1; // SDL instance ids are never negative (0 is valid)

using SlotIds = std::array<std::int64_t, kPorts>;

constexpr SlotIds emptySlots() { return {kEmpty, kEmpty}; }

// The slot holding `id`, or -1.
constexpr int slotOf(const SlotIds &ids, std::int64_t id)
{
    for (unsigned slot = 0; slot < kPorts; ++slot)
        if (ids[slot] == id)
            return static_cast<int>(slot);
    return -1;
}

constexpr bool anyEmpty(const SlotIds &ids) { return slotOf(ids, kEmpty) >= 0; }

// Gives `id` the lowest empty slot and returns it; -1 (ids unchanged) when `id`
// already holds a slot, is not a valid id, or both slots are taken.
constexpr int assign(SlotIds &ids, std::int64_t id)
{
    if (id < 0 || slotOf(ids, id) >= 0)
        return -1;
    const int slot = slotOf(ids, kEmpty);
    if (slot >= 0)
        ids[static_cast<unsigned>(slot)] = id;
    return slot;
}

// Frees the slot holding `id` and returns it; -1 when `id` holds none. The other
// slot is left as it is.
constexpr int release(SlotIds &ids, std::int64_t id)
{
    if (id < 0)
        return -1;
    const int slot = slotOf(ids, id);
    if (slot >= 0)
        ids[static_cast<unsigned>(slot)] = kEmpty;
    return slot;
}

// Moves the slot-1 controller to slot 0 when slot 0 is empty and slot 1 is not,
// and returns true; otherwise nothing changes (false). The caller decides WHEN to
// promote: only after a slot-0 removal with no other unassigned controller left.
constexpr bool promote(SlotIds &ids)
{
    if (ids[0] != kEmpty || ids[1] == kEmpty)
        return false;
    ids[0] = ids[1];
    ids[1] = kEmpty;
    return true;
}
} // namespace rrv::host::padslots

#endif // RRV_PAD_SLOTS_H
