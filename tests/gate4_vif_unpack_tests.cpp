// Gate-4 G4-8: the VIF1 UNPACK fast path must write exactly what the generic
// loop writes. Random UNPACK streams (every VN/VL, masks, MODE, CL/WL, TOPS
// flag, signed/unsigned) run through both paths on identical starting state;
// VU1 data memory and the VIF row/col registers must match byte for byte.
#include "ps2_runtime.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

extern bool g_rrvVif1UnpackFastV1;

namespace {
struct Result { std::vector<uint8_t> data; VIFRegisters regs; };

Result run(bool fast, const std::vector<uint8_t>& vuInit, const VIFRegisters& regs,
           const std::vector<uint8_t>& stream) {
    PS2Memory memory;
    if (!memory.initialize()) throw std::runtime_error("PS2Memory initialize failed");
    std::memcpy(memory.getVU1Data(), vuInit.data(), vuInit.size());
    memory.vif1_regs = regs;
    g_rrvVif1UnpackFastV1 = fast;
    memory.processVIF1Data(stream.data(), static_cast<uint32_t>(stream.size()));
    return {std::vector<uint8_t>(memory.getVU1Data(), memory.getVU1Data() + PS2_VU1_DATA_SIZE), memory.vif1_regs};
}
} // namespace

int main() {
    std::mt19937_64 rng(0x6a4e5d0c0ffeeull);
    int cases = 0, fastEligible = 0;
    for (int iter = 0; iter < 1500; ++iter) {
        std::vector<uint8_t> vuInit(PS2_VU1_DATA_SIZE);
        for (auto& b : vuInit) b = static_cast<uint8_t>(rng());
        VIFRegisters regs{};
        regs.tops = static_cast<uint32_t>(rng() & 0x3FFu);
        for (int i = 0; i < 4; ++i) { regs.row[i] = static_cast<uint32_t>(rng()); regs.col[i] = static_cast<uint32_t>(rng()); }
        std::vector<uint8_t> stream;
        auto put32 = [&](uint32_t w) { for (int i = 0; i < 4; ++i) stream.push_back(static_cast<uint8_t>(w >> (8 * i))); };
        const int commands = 1 + static_cast<int>(rng() % 4);
        for (int c = 0; c < commands; ++c) {
            const bool plain = (rng() % 3) != 0; // bias towards fast-path eligible streams
            uint32_t cl = 1 + rng() % 4, wl = 1 + rng() % 4;
            if (plain && cl < wl) std::swap(cl, wl);
            put32((0x01u << 24) | (wl << 8) | cl);                          // STCYCL
            put32((0x05u << 24) | (plain ? 0u : static_cast<uint32_t>(rng() % 4))); // STMOD
            if (!plain) { put32(0x20u << 24); put32(static_cast<uint32_t>(rng())); } // STMASK
            const uint32_t vn = rng() % 4, vl = rng() % 4;
            const bool mask = !plain && (rng() & 1);
            const uint32_t num = 1 + rng() % 40;
            const uint32_t imm = static_cast<uint32_t>(rng() & 0xC3FFu);    // addr, USN, FLG
            put32(((0x60u | (mask ? 0x10u : 0u) | (vn << 2) | vl) << 24) | (num << 16) | imm);
            // Source size the generic path computes, rounded to words, plus slack so
            // V3 W reads past the element stay inside the packet sometimes.
            const uint32_t bits = (vl == 3 && vn == 3) ? 16 : (vn + 1) * (vl == 0 ? 32 : vl == 1 ? 16 : vl == 2 ? 8 : 16);
            uint32_t src = num;
            if (cl < wl) src = (num / wl) * cl + std::min(num % wl, cl);
            const uint32_t bytes = ((src * ((bits + 7) / 8)) + 3) & ~3u;
            for (uint32_t i = 0; i < bytes; ++i) stream.push_back(static_cast<uint8_t>(rng()));
            if (plain && vl != 3 && cl >= wl) ++fastEligible;
        }
        if (rng() & 1) for (int i = 0; i < 4; ++i) stream.push_back(static_cast<uint8_t>(rng())); // trailing bytes
        const Result generic = run(false, vuInit, regs, stream);
        const Result fast = run(true, vuInit, regs, stream);
        if (generic.data != fast.data || std::memcmp(&generic.regs, &fast.regs, sizeof(VIFRegisters)) != 0) {
            std::printf("FAIL vif-unpack-fast-equals-generic iter=%d\n", iter);
            return 1;
        }
        ++cases;
    }
    g_rrvVif1UnpackFastV1 = true;
    std::printf("PASS vif-unpack-fast-equals-generic cases=%d fast-eligible-unpacks=%d\n", cases, fastEligible);
    return 0;
}
