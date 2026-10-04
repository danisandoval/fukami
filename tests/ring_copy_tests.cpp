// rrv::ringCopy (runtime/rrv_ring_copy.h) against the byte loop it replaces in the scratchpad DMA
// channels (PS2Memory::writeIORegister, ch8 fromSPR / ch9 toSPR): the same bytes in both buffers and the
// same final offsets, for random offsets and lengths, including transfers that wrap either buffer, end
// exactly on a wrap, are empty, or are longer than a buffer.

#include "runtime/rrv_ring_copy.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
uint64_t g_rng = 0x243F6A8885A308D3ull;
uint64_t rnd()
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

// The loop as it stood in ps2_memory.cpp.
void byteLoop(uint8_t *dst, uint32_t &dstOff, uint32_t dstMask, const uint8_t *src, uint32_t &srcOff,
              uint32_t srcMask, uint64_t bytes)
{
    for (uint64_t i = 0; i < bytes; ++i)
    {
        dst[dstOff] = src[srcOff];
        dstOff = (dstOff + 1u) & dstMask;
        srcOff = (srcOff + 1u) & srcMask;
    }
}
} // namespace

int main()
{
    // Scratchpad-sized and a small "RAM" so that wraps of both buffers are common.
    const uint32_t sizes[][2] = {{16384u, 65536u}, {65536u, 16384u}, {64u, 256u}, {256u, 64u}, {16u, 16u}};
    int failures = 0;
    uint64_t cases = 0, wraps = 0, longer = 0;
    for (const auto &size : sizes)
    {
        const uint32_t dstSize = size[0], srcSize = size[1];
        std::vector<uint8_t> src(srcSize), dstA(dstSize), dstB(dstSize);
        for (int iter = 0; iter < 4000; ++iter)
        {
            for (uint8_t &b : src)
                b = static_cast<uint8_t>(rnd());
            for (uint32_t i = 0; i < dstSize; ++i)
                dstA[i] = dstB[i] = static_cast<uint8_t>(rnd());
            uint32_t dstOff = static_cast<uint32_t>(rnd()) & (dstSize - 1u);
            uint32_t srcOff = static_cast<uint32_t>(rnd()) & (srcSize - 1u);
            uint64_t bytes;
            switch (rnd() % 6)
            {
            case 0: bytes = 0; break;
            case 1: bytes = dstSize - dstOff; break;                       // ends exactly on the wrap
            case 2: bytes = srcSize - srcOff; break;
            case 3: bytes = (rnd() % 4u) * dstSize + rnd() % (2u * srcSize); break; // longer than a buffer
            default: bytes = (1u + rnd() % 64u) * 16u; break;             // a DMA burst: whole quadwords
            }
            if (rnd() % 8 == 0) // start right before a wrap
            {
                dstOff = (dstSize - 1u - static_cast<uint32_t>(rnd() % 8u)) & (dstSize - 1u);
                srcOff = (srcSize - 1u - static_cast<uint32_t>(rnd() % 8u)) & (srcSize - 1u);
            }
            uint32_t dA = dstOff, sA = srcOff, dB = dstOff, sB = srcOff;
            byteLoop(dstA.data(), dA, dstSize - 1u, src.data(), sA, srcSize - 1u, bytes);
            rrv::ringCopy(dstB.data(), dB, dstSize - 1u, src.data(), sB, srcSize - 1u, bytes);
            ++cases;
            wraps += (dstOff + bytes > dstSize || srcOff + bytes > srcSize) ? 1u : 0u;
            longer += bytes > dstSize ? 1u : 0u;
            if (dstA != dstB || dA != dB || sA != sB)
            {
                if (failures < 10)
                    std::fprintf(stderr, "FAIL: dst %u src %u dstOff %u srcOff %u bytes %llu (offsets %u/%u vs %u/%u)\n",
                                 dstSize, srcSize, dstOff, srcOff, (unsigned long long)bytes, dA, sA, dB, sB);
                ++failures;
            }
        }
    }
    std::printf("ring copy: %llu cases (%llu wrap a buffer, %llu longer than the destination), %d failures\n",
                (unsigned long long)cases, (unsigned long long)wraps, (unsigned long long)longer, failures);

    return failures == 0 ? 0 : 1;
}
