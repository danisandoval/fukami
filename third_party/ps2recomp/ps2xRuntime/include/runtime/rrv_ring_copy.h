#ifndef RRV_RING_COPY_H
#define RRV_RING_COPY_H

// rrv_ring_copy.h — block copy between two power-of-two ring buffers.
//
// The scratchpad DMA channels (ch8 fromSPR, ch9 toSPR; ps2_memory.cpp) copy
// between main RAM and the 16 KB scratchpad, and both addresses wrap at their
// buffer's size. They did it one byte at a time, masking both offsets for
// every byte: about 0.8 ms per VBlank start in the race on a Steam Deck (80%
// of PS2Memory::writeIORegister, profile 2026-10-02). RR5's display-list
// builder stages every GIF list through the scratchpad this way.
//
// ringCopy() moves the same bytes in the same order, in runs that end where
// either buffer wraps. The two buffers must not overlap (RAM and scratchpad are
// separate allocations); then the result equals the byte loop's for every
// length, including a transfer that wraps or is longer than a buffer (later
// bytes overwrite earlier ones, as in the loop). tests/ring_copy_tests.cpp
// compares the two.

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace rrv
{

// `dstMask` / `srcMask` are size - 1 of each buffer. The offsets are advanced
// past the last byte, wrapped, exactly as the byte loop left them.
inline void ringCopy(uint8_t *dst, uint32_t &dstOff, uint32_t dstMask,
                     const uint8_t *src, uint32_t &srcOff, uint32_t srcMask, uint64_t bytes)
{
    while (bytes != 0u)
    {
        uint64_t run = bytes;
        run = std::min<uint64_t>(run, static_cast<uint64_t>(dstMask) + 1u - dstOff);
        run = std::min<uint64_t>(run, static_cast<uint64_t>(srcMask) + 1u - srcOff);
        std::memcpy(dst + dstOff, src + srcOff, static_cast<size_t>(run));
        dstOff = static_cast<uint32_t>(dstOff + run) & dstMask;
        srcOff = static_cast<uint32_t>(srcOff + run) & srcMask;
        bytes -= run;
    }
}

} // namespace rrv

#endif // RRV_RING_COPY_H
