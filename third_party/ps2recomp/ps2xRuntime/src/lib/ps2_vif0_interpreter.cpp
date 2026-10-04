#include "runtime/ps2_memory.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace
{
std::atomic<uint32_t> s_vif0CommandCount{0u};
}

void PS2Memory::processVIF0Data(uint32_t srcPhys, uint32_t sizeBytes)
{
    if (!m_rdram || srcPhys >= PS2_RAM_SIZE || sizeBytes == 0u)
        return;
    sizeBytes = std::min<uint32_t>(sizeBytes, PS2_RAM_SIZE - srcPhys);
    processVIF0Data(m_rdram + srcPhys, sizeBytes);
}

void PS2Memory::processVIF0Data(const uint8_t *data, uint32_t sizeBytes)
{
    if (!data || !m_vu0Code || !m_vu0Data || sizeBytes == 0u)
        return;

    uint32_t pos = 0u;
    while (pos + 4u <= sizeBytes)
    {
        uint32_t cmd = 0u;
        std::memcpy(&cmd, data + pos, sizeof(cmd));
        pos += 4u;

        const uint8_t opcode = static_cast<uint8_t>((cmd >> 24) & 0x7fu);
        const uint8_t num = static_cast<uint8_t>((cmd >> 16) & 0xffu);
        const uint16_t imm = static_cast<uint16_t>(cmd & 0xffffu);
        const uint32_t commandIndex = s_vif0CommandCount.fetch_add(1u, std::memory_order_relaxed);
        if (commandIndex < 96u)
        {
            std::fprintf(stderr, "[vif0:cmd] idx=%u opcode=%02x imm=%04x num=%u\n",
                         commandIndex, opcode, imm, static_cast<uint32_t>(num));
        }

        vif0_regs.code = cmd;
        vif0_regs.num = num;
        if ((cmd & 0x80000000u) != 0u)
            vif0_regs.stat |= (1u << 11);

        if (opcode == 0x00u || opcode == 0x10u || opcode == 0x11u || opcode == 0x13u)
            continue;
        if (opcode == 0x01u)
        {
            vif0_regs.cycle = imm;
            continue;
        }
        if (opcode == 0x04u)
        {
            vif0_regs.itop = imm & 0xffu;
            continue;
        }
        if (opcode == 0x05u)
        {
            vif0_regs.mode = imm & 3u;
            continue;
        }
        if (opcode == 0x07u)
        {
            vif0_regs.mark = imm;
            vif0_regs.stat |= (1u << 6);
            continue;
        }
        if (opcode == 0x20u)
        {
            if (pos + 4u > sizeBytes)
                break;
            std::memcpy(&vif0_regs.mask, data + pos, 4u);
            pos += 4u;
            continue;
        }
        if (opcode == 0x30u || opcode == 0x31u)
        {
            if (pos + 16u > sizeBytes)
                break;
            std::memcpy(opcode == 0x30u ? vif0_regs.row : vif0_regs.col,
                        data + pos, 16u);
            pos += 16u;
            continue;
        }
        if (opcode == 0x4au)
        {
            const uint32_t instructionCount = num == 0u ? 256u : static_cast<uint32_t>(num);
            const uint32_t payloadBytes = instructionCount * 8u;
            const uint32_t dest = (static_cast<uint32_t>(imm) & 0x1ffu) * 8u;
            if (pos + payloadBytes > sizeBytes)
                break;
            if (dest < PS2_VU0_CODE_SIZE)
            {
                const uint32_t copyBytes = std::min<uint32_t>(payloadBytes,
                                                              PS2_VU0_CODE_SIZE - dest);
                if (std::memcmp(m_vu0Code + dest, data + pos, copyBytes) != 0)
                {
                    std::memcpy(m_vu0Code + dest, data + pos, copyBytes);
                    rrvVuCodeChanged(0);
                }
                // MPG uploads recur every frame in RRV.  Keep the diagnostic
                // bounded or stderr contention makes visual validation much
                // slower than the renderer itself.
                if (commandIndex < 96u)
                {
                    std::fprintf(stderr,
                                 "[vif0:mpg] dest=%04x instructions=%u bytes=%u\n",
                                 dest, instructionCount, copyBytes);
                }
            }
            pos += payloadBytes;
            continue;
        }

        // VIF0 UNPACK feeds the VU0 macro-mode geometry helpers used by RRV's
        // character renderer.  The EE builds a short VIF0 chain, UNPACKs model
        // data into VU0 data memory, then invokes VCALLMS directly.  Merely
        // consuming the payload (the old behavior) left VU0 memory stale and
        // caused every generated scratch packet to be empty.
        if ((opcode & 0x60u) == 0x60u)
        {
            const uint8_t vn = (opcode >> 2) & 3u;
            const uint8_t vl = opcode & 3u;
            const bool maskEnable = (opcode & 0x10u) != 0u;
            const uint32_t components = static_cast<uint32_t>(vn) + 1u;
            uint32_t bitsPerComponent = 32u;
            if (vl == 1u)
                bitsPerComponent = 16u;
            else if (vl == 2u)
                bitsPerComponent = 8u;
            else if (vl == 3u)
                bitsPerComponent = (vn == 3u) ? 4u : 16u;

            const uint32_t bitsPerVector =
                (vl == 3u && vn == 3u) ? 16u : components * bitsPerComponent;
            const uint32_t bytesPerVector = (bitsPerVector + 7u) / 8u;
            const uint32_t writeVectorCount =
                num == 0u ? 256u : static_cast<uint32_t>(num);

            const uint32_t rawCl = vif0_regs.cycle & 0xffu;
            const uint32_t rawWl = (vif0_regs.cycle >> 8u) & 0xffu;
            const uint32_t cl = rawCl == 0u ? 256u : rawCl;
            const uint32_t wl = rawWl == 0u ? 256u : rawWl;

            uint32_t sourceVectorCount = writeVectorCount;
            if (wl > cl)
            {
                const uint32_t fullBlocks = writeVectorCount / wl;
                uint32_t remainder = writeVectorCount % wl;
                if (remainder > cl)
                    remainder = cl;
                sourceVectorCount = fullBlocks * cl + remainder;
            }

            uint32_t payloadBytes = sourceVectorCount * bytesPerVector;
            payloadBytes = (payloadBytes + 3u) & ~3u;
            if (pos + payloadBytes > sizeBytes)
                break;

            // VU0 data memory contains 256 quadwords; VIF0 UNPACK's destination
            // address is correspondingly eight bits wide.  Bit 14 selects
            // unsigned extension for 8/16-bit formats.
            const uint32_t vuAddr = static_cast<uint32_t>(imm) & 0xffu;
            const bool zeroExtend = (imm & 0x4000u) != 0u;
            const uint8_t *srcBase = data + pos;
            uint32_t srcIndex = 0u;

            for (uint32_t writeIndex = 0u; writeIndex < writeVectorCount; ++writeIndex)
            {
                const uint32_t cyclePos = writeIndex % wl;
                const bool sourceAvailable = (cl >= wl) || (cyclePos < cl);
                const uint32_t destVec = cl >= wl
                    ? (vuAddr + (writeIndex / wl) * cl + cyclePos) & 0xffu
                    : (vuAddr + writeIndex) & 0xffu;
                const uint32_t destOff = destVec * 16u;

                uint32_t lanes[4] = {};
                std::memcpy(lanes, m_vu0Data + destOff, sizeof(lanes));
                uint32_t decompressed[4] = {lanes[0], lanes[1], lanes[2], lanes[3]};
                const uint8_t *srcVec = nullptr;
                bool decoded = false;
                if (sourceAvailable && srcIndex < sourceVectorCount)
                {
                    srcVec = srcBase + srcIndex * bytesPerVector;
                    ++srcIndex;
                    decoded = true;
                }

                auto extend16 = [&](uint16_t raw) -> uint32_t
                {
                    return zeroExtend
                        ? static_cast<uint32_t>(raw)
                        : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(raw)));
                };
                auto extend8 = [&](uint8_t raw) -> uint32_t
                {
                    return zeroExtend
                        ? static_cast<uint32_t>(raw)
                        : static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(raw)));
                };

                bool handledFormat = decoded;
                if (decoded && vl == 0u)
                {
                    if (components == 1u)
                    {
                        uint32_t scalar = 0u;
                        std::memcpy(&scalar, srcVec, sizeof(scalar));
                        for (uint32_t field = 0u; field < 4u; ++field)
                            decompressed[field] = scalar;
                    }
                    else
                    {
                        for (uint32_t field = 0u; field < components && field < 4u; ++field)
                            std::memcpy(&decompressed[field], srcVec + field * 4u, 4u);
                    }
                }
                else if (decoded && vl == 1u)
                {
                    if (components == 1u)
                    {
                        uint16_t raw = 0u;
                        std::memcpy(&raw, srcVec, sizeof(raw));
                        const uint32_t scalar = extend16(raw);
                        for (uint32_t field = 0u; field < 4u; ++field)
                            decompressed[field] = scalar;
                    }
                    else
                    {
                        for (uint32_t field = 0u; field < components && field < 4u; ++field)
                        {
                            uint16_t raw = 0u;
                            std::memcpy(&raw, srcVec + field * 2u, sizeof(raw));
                            decompressed[field] = extend16(raw);
                        }
                    }
                }
                else if (decoded && vl == 2u)
                {
                    if (components == 1u)
                    {
                        const uint32_t scalar = extend8(srcVec[0]);
                        for (uint32_t field = 0u; field < 4u; ++field)
                            decompressed[field] = scalar;
                    }
                    else
                    {
                        for (uint32_t field = 0u; field < components && field < 4u; ++field)
                            decompressed[field] = extend8(srcVec[field]);
                    }
                }
                else if (decoded && vl == 3u && vn == 3u)
                {
                    uint16_t packed = 0u;
                    std::memcpy(&packed, srcVec, sizeof(packed));
                    decompressed[0] = (packed & 0x001fu) << 3u;
                    decompressed[1] = (packed & 0x03e0u) >> 2u;
                    decompressed[2] = (packed & 0x7c00u) >> 7u;
                    decompressed[3] = (packed & 0x8000u) >> 8u;
                }
                else if (decoded)
                {
                    handledFormat = false;
                }

                if (!handledFormat && decoded && !maskEnable &&
                    (vif0_regs.mode == 0u || vif0_regs.mode == 3u))
                {
                    const uint32_t copyBytes = std::min<uint32_t>(bytesPerVector, 16u);
                    std::memcpy(m_vu0Data + destOff, srcVec, copyBytes);
                    continue;
                }

                // Hardware duplicates V2 as XYXY. V3 uses the V4 write path;
                // its indeterminate W lane is the following packed component
                // (or source padding at the end of the payload).
                if (handledFormat && decoded && vn == 1u)
                {
                    decompressed[2] = decompressed[0];
                    decompressed[3] = decompressed[1];
                }
                else if (handledFormat && decoded && vn == 2u)
                {
                    const uint32_t componentBytes = bitsPerComponent / 8u;
                    const uint8_t *wSource = srcVec + 3u * componentBytes;
                    const uint8_t *payloadEnd = srcBase + payloadBytes;
                    if (componentBytes != 0u && wSource + componentBytes <= payloadEnd)
                    {
                        if (vl == 0u)
                            std::memcpy(&decompressed[3], wSource, 4u);
                        else if (vl == 1u)
                        {
                            uint16_t raw = 0u;
                            std::memcpy(&raw, wSource, 2u);
                            decompressed[3] = extend16(raw);
                        }
                        else if (vl == 2u)
                            decompressed[3] = extend8(*wSource);
                    }
                }

                const bool canAdd = !(vl == 3u && vn == 3u);
                const uint32_t mode = vif0_regs.mode & 3u;
                const uint32_t colIdx = std::min<uint32_t>(cyclePos, 3u);
                const uint32_t maskCycle = std::min<uint32_t>(cyclePos, 3u);
                for (uint32_t field = 0u; field < 4u; ++field)
                {
                    uint32_t maskSpec = 0u;
                    if (maskEnable)
                    {
                        const uint32_t shift = ((maskCycle * 4u) + field) * 2u;
                        maskSpec = (vif0_regs.mask >> shift) & 3u;
                    }
                    if (!decoded && maskSpec == 0u)
                        maskSpec = 1u;

                    uint32_t writeValue = lanes[field];
                    if (maskSpec == 0u)
                    {
                        if (handledFormat)
                        {
                            writeValue = decompressed[field];
                            if (canAdd && (mode == 1u || mode == 2u))
                            {
                                writeValue += vif0_regs.row[field];
                                if (mode == 2u)
                                    vif0_regs.row[field] = writeValue;
                            }
                            else if (canAdd && mode == 3u)
                            {
                                vif0_regs.row[field] = writeValue;
                            }
                        }
                    }
                    else if (maskSpec == 1u)
                    {
                        writeValue = vif0_regs.row[field];
                    }
                    else if (maskSpec == 2u)
                    {
                        writeValue = vif0_regs.col[colIdx];
                    }
                    else
                    {
                        continue;
                    }
                    lanes[field] = writeValue;
                }
                std::memcpy(m_vu0Data + destOff, lanes, sizeof(lanes));
            }

            pos += payloadBytes;
            continue;
        }

        static std::atomic<uint32_t> unsupportedCount{0u};
        const uint32_t unsupportedIndex =
            unsupportedCount.fetch_add(1u, std::memory_order_relaxed);
        static std::atomic<bool> dumpedFirstUnsupported{false};
        if (!dumpedFirstUnsupported.exchange(true, std::memory_order_relaxed))
        {
            if (FILE *dump = std::fopen("/private/tmp/vif0_first_bad_stream.bin", "wb"))
            {
                std::fwrite(data, 1u, sizeBytes, dump);
                std::fclose(dump);
            }
            std::fprintf(stderr,
                         "[vif0] dumped first unsupported stream size=%u "
                         "offset=%u opcode=%02x cycle=%04x\n",
                         sizeBytes, pos - 4u, opcode, vif0_regs.cycle & 0xffffu);
        }
        if (unsupportedIndex < 96u)
        {
            std::fprintf(stderr, "[vif0] unsupported opcode=%02x at stream offset=%u\n",
                         opcode, pos - 4u);
        }
    }
}
