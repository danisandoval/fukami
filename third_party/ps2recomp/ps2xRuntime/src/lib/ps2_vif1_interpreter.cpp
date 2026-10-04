// Based on Blackline Interactive implementation
#include "ps2_syscalls.h"
#include "runtime/ps2_memory.h"
#include "runtime/diag_counters.h"
#include "rrv_gate4_owner_timeline.h" // Gate-4 S0 owner timeline (RRV_GATE4_OWNER_TIMELINE)
#include "runtime/ps2_vu1.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "ps2_log.h"

// RRV_DMA_ORDER_DIAG ring (defined in ps2_memory.cpp) — see the comment there.
void rrvNoteDmaOrderEvent(const char *what, unsigned long long a, unsigned long long b);

// RRV_VIF_UNPACK_HW_QUIRK — write the components a V2/V3 UNPACK does not declare
// the way PS2 hardware does, instead of leaving the previous quadword contents.
//
// PCSX2 `Vif_Unpack.cpp` @ d5f75c9e4 states both rules in its own comments:
//   * UNPACK_V2: "The PS2 console actually writes v1v0v1v0 for all V2 unpacks --
//     the second v1v0 pair being officially 'indeterminate' but some games very
//     much depend on it."
//   * UNPACK_V3: "V3 and V4 unpacks both use the V4 unpack logic, even though
//     most of the OFFSET_W fields during V3 unpacking end up being overwritten
//     by the next unpack. This is confirmed real hardware behavior."
//
// docs/TESTING.md T-P7-VUIN measured this as the ONLY difference between our
// VU1 input and hardware's at the phase-7 sky microprogram: over all 16 KB of
// VU1 data memory, X/Y/Z agree byte for byte and 111 quadwords differ in W and
// nothing else. Unset/1 = hardware behaviour; 0 = the previous behaviour.
static bool vifUnpackHwQuirkEnabled()
{
    static const bool enabled = [] {
        const char *v = std::getenv("RRV_VIF_UNPACK_HW_QUIRK");
        return !(v && v[0] == '0');
    }();
    return enabled;
}

// DIAG-TEMP (T-FLY-TREES) — RRV_VU_MATRIX_TRACE logs every VIF1 UNPACK that
// lands on a watched VU1 data-memory quadword, with the guest source address of
// the bytes it unpacked, so a divergent VU1 constant can be walked back to the
// EE buffer that produced it. RRV_VU_MATRIX_TRACE_DEST is a comma-separated
// list of VU1 quadword indices (decimal or 0x-hex); it defaults to 0x391,0x392,
// the pair the phase-7 work watched.
static bool vuMatrixTraceWatch(uint32_t destVec)
{
    static const std::vector<uint32_t> watched = [] {
        std::vector<uint32_t> v;
        const char *s = std::getenv("RRV_VU_MATRIX_TRACE_DEST");
        if (!s || !s[0])
            return std::vector<uint32_t>{0x391u, 0x392u};
        while (*s)
        {
            char *end = nullptr;
            const unsigned long q = std::strtoul(s, &end, 0);
            if (end == s)
                break;
            v.push_back(static_cast<uint32_t>(q));
            s = (*end == ',') ? end + 1 : end;
        }
        return v;
    }();
    for (uint32_t q : watched)
        if (q == destVec)
            return true;
    return false;
}

enum VIFCmd : uint8_t
{
    VIF_NOP = 0x00,
    VIF_STCYCL = 0x01,
    VIF_OFFSET = 0x02,
    VIF_BASE = 0x03,
    VIF_ITOP = 0x04,
    VIF_STMOD = 0x05,
    VIF_MSKPATH3 = 0x06,
    VIF_MARK = 0x07,
    VIF_FLUSHE = 0x10,
    VIF_FLUSH = 0x11,
    VIF_FLUSHA = 0x13,
    VIF_MSCAL = 0x14,
    VIF_MSCALF = 0x15,
    VIF_MSCNT = 0x17,
    VIF_STMASK = 0x20,
    VIF_STROW = 0x30,
    VIF_STCOL = 0x31,
    VIF_MPG = 0x4A,
    VIF_DIRECT = 0x50,
    VIF_DIRECTHL = 0x51,
};

namespace
{
    std::atomic<uint32_t> s_debugVu1KickCount{0};
    std::atomic<uint32_t> s_debugVif1OpcodeCount{0};
    constexpr uint8_t kGifFmtImage = 2u;

    // Truncation census, default off (`RRV_VIF1_DIRECT_DIAG=1`). Prints the
    // first few and a running total, so the fix has a before/after number
    // instead of a screenshot.
    bool vif1DirectDiagOn()
    {
        static const bool s_on = [] {
            const char *v = std::getenv("RRV_VIF1_DIRECT_DIAG");
            return v && v[0] && v[0] != '0';
        }();
        return s_on;
    }

    // Provenance for a submitted PATH2 packet: which of the three sites emitted
    // it, how big it was, and whether its own GIFtag chain lands on its end.
    // KNOWN_ISSUES #18 named two packets by size only; this names their author.
    void rrvNoteVif1Path2(const char *site, const uint8_t *p, uint32_t bytes,
                          uint32_t immOrPending, uint32_t cmdWord = 0u,
                          uint32_t cmdPos = 0u, uint32_t xferBytes = 0u,
                          uint32_t codesBefore = 0u)
    {
        if (!vif1DirectDiagOn() || bytes < 65536u)
            return;
        // Walk the tag chain the same way the GS would.
        uint32_t off = 0u;
        int land = -2;
        if (p && bytes >= 16u && (bytes & 15u) == 0u)
        {
            land = -1;
            for (uint32_t guard = 0u; guard < (1u << 20); ++guard)
            {
                if (off == bytes) { land = 1; break; }
                if (off + 16u > bytes) { land = 0; break; }
                uint64_t tag = 0u;
                std::memcpy(&tag, p + off, sizeof(tag));
                const uint32_t nloop = static_cast<uint32_t>(tag & 0x7FFFu);
                const uint32_t flg = static_cast<uint32_t>((tag >> 58) & 0x3u);
                uint32_t nreg = static_cast<uint32_t>((tag >> 60) & 0xFu);
                if (nreg == 0u) nreg = 16u;
                uint64_t payload = (flg == 0u)   ? static_cast<uint64_t>(nloop) * nreg * 16ull
                                   : (flg == 1u) ? static_cast<uint64_t>(nloop) * nreg * 8ull +
                                                       (((static_cast<uint64_t>(nloop) * nreg) & 1ull) ? 8ull : 0ull)
                                                 : static_cast<uint64_t>(nloop) * 16ull;
                off += 16u;
                if (off + payload > bytes) { land = 0; break; }
                off += static_cast<uint32_t>(payload);
            }
        }
        std::fprintf(stderr,
                     "[vif1:path2] site=%s bytes=%u qw=%u imm/pending=%u land=%s "
                     "cmd=%08x num=%u cmdPos=%u xfer=%u tail=%d codesBefore=%u\n",
                     site, bytes, bytes / 16u, immOrPending,
                     land == 1 ? "exact" : land == 0 ? "SHORT" : "unwalkable",
                     cmdWord, (cmdWord >> 16) & 0xFFu, cmdPos, xferBytes,
                     (int)((int64_t)xferBytes - (int64_t)cmdPos - 4 - (int64_t)bytes),
                     codesBefore);
    }

    void rrvNoteVif1DirectTruncation(uint32_t declaredQw, uint32_t deliveredQw)
    {
        if (!vif1DirectDiagOn())
            return;
        static std::atomic<uint32_t> s_count{0};
        const uint32_t n = s_count.fetch_add(1u, std::memory_order_relaxed) + 1u;
        if (n <= 16u || (n % 256u) == 0u)
        {
            std::fprintf(stderr,
                         "[vif1:direct] truncated n=%u declared=%u qw (%u B) "
                         "delivered=%u qw (%u B) LOST=%u qw\n",
                         n, declaredQw, declaredQw * 16u, deliveredQw,
                         deliveredQw * 16u, declaredQw - deliveredQw);
        }
    }

    uint32_t gifImageQwcFromTag(const uint8_t *data, uint32_t sizeBytes)
    {
        if (!data || sizeBytes < 16u)
            return 0u;

        uint64_t tagLo = 0u;
        std::memcpy(&tagLo, data, sizeof(tagLo));
        const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
        if (flg != kGifFmtImage)
            return 0u;

        return static_cast<uint32_t>(tagLo & 0x7FFFu);
    }
}

void PS2Memory::processVIF1Data(uint32_t srcPhys, uint32_t sizeBytes)
{
    if (!m_rdram || !m_gsVRAM || sizeBytes == 0u)
        return;
    if (srcPhys >= PS2_RAM_SIZE)
        return;

    const uint64_t requestedEnd = static_cast<uint64_t>(srcPhys) + static_cast<uint64_t>(sizeBytes);
    if (requestedEnd > static_cast<uint64_t>(PS2_RAM_SIZE))
        sizeBytes = PS2_RAM_SIZE - srcPhys;

    processVIF1Data(m_rdram + srcPhys, sizeBytes);
}

// Gate-4 G4-8: VIF1 UNPACK fast path switch (tests compare both paths).
bool g_rrvVif1UnpackFastV1 = true;

namespace
{
// The fast path's loop (no mask, MODE 0, CL >= WL, a handled format), with the
// format as template constants: VL (0: 32 bits, 1: 16, 2: 8, 3: V4-5),
// COMPONENTS (1..4), ZEXT (the USN bit), QUIRK (RRV_VIF_UNPACK_HW_QUIRK: what
// hardware writes into the components a V2/V3 UNPACK does not declare) and
// CONTIG (CL == WL: vector i goes to address + i). It writes exactly what the
// generic loop in processVIF1Data() writes for the same command; the loop was
// one body that tested all of this for every vector (the Steam Deck profile,
// docs/evidence/GATE5_STEAMDECK_PROFILE_2026-10-02.md, has VIF UNPACK at 2.1 ms
// per field in the heavy race scenes). tests/gate4_vif_unpack_tests.cpp compares
// the two paths.
template <uint32_t VL, int COMPONENTS, bool ZEXT, bool QUIRK, bool CONTIG>
__attribute__((noinline)) void rrvVif1UnpackRun(uint8_t *vu1Data, const uint8_t *srcBase, const uint8_t *data,
                                                uint32_t sizeBytes, uint32_t writeVectorCount, uint32_t vuAddr,
                                                uint32_t cl, uint32_t wl)
{
    constexpr uint32_t kCompBytes = VL == 0u ? 4u : VL == 1u ? 2u : VL == 2u ? 1u : 2u;
    constexpr uint32_t kBytesPerVector = VL == 3u ? 2u : static_cast<uint32_t>(COMPONENTS) * kCompBytes;
    for (uint32_t i = 0; i < writeVectorCount; ++i)
    {
        const uint32_t destVec = CONTIG ? ((vuAddr + i) & 0x3FFu)
                                        : ((vuAddr + (i / wl) * cl + (i % wl)) & 0x3FFu);
        const uint32_t destOff = destVec * 16u;
        const uint8_t *s = srcBase + i * kBytesPerVector;
        uint32_t v[4];
        std::memcpy(v, vu1Data + destOff, sizeof(v));
        if constexpr (VL == 0u)
        {
            if constexpr (COMPONENTS == 1)
            {
                uint32_t x;
                std::memcpy(&x, s, 4);
                v[0] = v[1] = v[2] = v[3] = x;
            }
            else
                std::memcpy(v, s, static_cast<size_t>(COMPONENTS) * 4u);
        }
        else if constexpr (VL == 1u)
        {
            for (int c = 0; c < COMPONENTS; ++c)
            {
                uint16_t raw;
                std::memcpy(&raw, s + c * 2, 2);
                v[c] = ZEXT ? uint32_t(raw) : uint32_t(int32_t(int16_t(raw)));
            }
            if constexpr (COMPONENTS == 1)
                v[1] = v[2] = v[3] = v[0];
        }
        else if constexpr (VL == 2u)
        {
            for (int c = 0; c < COMPONENTS; ++c)
                v[c] = ZEXT ? uint32_t(s[c]) : uint32_t(int32_t(int8_t(s[c])));
            if constexpr (COMPONENTS == 1)
                v[1] = v[2] = v[3] = v[0];
        }
        else
        {
            uint16_t packed;
            std::memcpy(&packed, s, 2);
            v[0] = packed & 0x1Fu;
            v[1] = (packed >> 5) & 0x1Fu;
            v[2] = (packed >> 10) & 0x1Fu;
            v[3] = (packed >> 15) & 0x01u;
        }
        if constexpr (QUIRK && COMPONENTS == 2)
        {
            v[2] = v[0];
            v[3] = v[1];
        }
        else if constexpr (QUIRK && COMPONENTS == 3)
        {
            const uint8_t *wSrc = s + 3u * kCompBytes;
            if (static_cast<size_t>(wSrc - data) + kCompBytes <= sizeBytes)
            {
                if constexpr (VL == 0u)
                    std::memcpy(&v[3], wSrc, sizeof(uint32_t));
                else if constexpr (VL == 1u)
                {
                    uint16_t raw;
                    std::memcpy(&raw, wSrc, 2);
                    v[3] = ZEXT ? uint32_t(raw) : uint32_t(int32_t(int16_t(raw)));
                }
                else
                    v[3] = ZEXT ? uint32_t(wSrc[0]) : uint32_t(int32_t(int8_t(wSrc[0])));
            }
            else
                v[3] = 0u;
        }
        std::memcpy(vu1Data + destOff, v, sizeof(v));
    }
}

template <uint32_t VL, int COMPONENTS>
void rrvVif1UnpackSelect(uint8_t *vu1Data, const uint8_t *srcBase, const uint8_t *data, uint32_t sizeBytes,
                         uint32_t writeVectorCount, uint32_t vuAddr, uint32_t cl, uint32_t wl, bool zeroExtend,
                         bool quirk)
{
    const unsigned key = (zeroExtend ? 1u : 0u) | (quirk ? 2u : 0u) | (cl == wl ? 4u : 0u);
    switch (key)
    {
#define RRV_VIF1_UNPACK_CASE(K, Z, Q, C)                                                              \
    case K:                                                                                           \
        rrvVif1UnpackRun<VL, COMPONENTS, Z, Q, C>(vu1Data, srcBase, data, sizeBytes, writeVectorCount, \
                                                   vuAddr, cl, wl);                                    \
        break;
        RRV_VIF1_UNPACK_CASE(0u, false, false, false)
        RRV_VIF1_UNPACK_CASE(1u, true, false, false)
        RRV_VIF1_UNPACK_CASE(2u, false, true, false)
        RRV_VIF1_UNPACK_CASE(3u, true, true, false)
        RRV_VIF1_UNPACK_CASE(4u, false, false, true)
        RRV_VIF1_UNPACK_CASE(5u, true, false, true)
        RRV_VIF1_UNPACK_CASE(6u, false, true, true)
        RRV_VIF1_UNPACK_CASE(7u, true, true, true)
#undef RRV_VIF1_UNPACK_CASE
    default:
        break;
    }
}

// `vl` 0..3 and `components` 1..4 (the fast path takes VL 3 only as V4-5).
void rrvVif1UnpackDispatch(uint8_t *vu1Data, const uint8_t *srcBase, const uint8_t *data, uint32_t sizeBytes,
                           uint32_t writeVectorCount, uint32_t vuAddr, uint32_t cl, uint32_t wl, uint32_t vl,
                           int components, bool zeroExtend, bool quirk)
{
    switch ((vl << 3) | static_cast<uint32_t>(components))
    {
#define RRV_VIF1_UNPACK_FORMAT(VL, N)                                                                  \
    case ((VL) << 3) | (N):                                                                            \
        rrvVif1UnpackSelect<VL, N>(vu1Data, srcBase, data, sizeBytes, writeVectorCount, vuAddr, cl, wl, \
                                   zeroExtend, quirk);                                                 \
        break;
        RRV_VIF1_UNPACK_FORMAT(0u, 1)
        RRV_VIF1_UNPACK_FORMAT(0u, 2)
        RRV_VIF1_UNPACK_FORMAT(0u, 3)
        RRV_VIF1_UNPACK_FORMAT(0u, 4)
        RRV_VIF1_UNPACK_FORMAT(1u, 1)
        RRV_VIF1_UNPACK_FORMAT(1u, 2)
        RRV_VIF1_UNPACK_FORMAT(1u, 3)
        RRV_VIF1_UNPACK_FORMAT(1u, 4)
        RRV_VIF1_UNPACK_FORMAT(2u, 1)
        RRV_VIF1_UNPACK_FORMAT(2u, 2)
        RRV_VIF1_UNPACK_FORMAT(2u, 3)
        RRV_VIF1_UNPACK_FORMAT(2u, 4)
        RRV_VIF1_UNPACK_FORMAT(3u, 4)
#undef RRV_VIF1_UNPACK_FORMAT
    default:
        break;
    }
}
} // namespace

void PS2Memory::processVIF1Data(const uint8_t *data, uint32_t sizeBytes)
{
    rrv::gate4::OwnerScope gate4Owner(rrv::gate4::OwnerKind::Vif1Data, sizeBytes);
    if (!data || !m_gsVRAM || sizeBytes == 0u)
        return;



    auto recomputeVif1Tops = [&]()
    {
        const bool dbf = (vif1_regs.stat & (1u << 7)) != 0u;
        const uint32_t base = vif1_regs.base & 0x3FFu;
        const uint32_t ofst = vif1_regs.ofst & 0x3FFu;
        vif1_regs.tops = dbf ? ((base + ofst) & 0x3FFu) : base;
    };

    uint32_t pos = 0;


    // RRV_GIF_PATH3_INTERLEAVE release boundary. A VIFcode boundary is far too
    // fine: the chain's steady state is one batch per XGKICK
    // (UNPACK... -> FLUSH -> MSCAL -> XGKICK), and releasing between a batch's
    // UNPACKs splits a PATH1 run that hardware keeps atomic.
    //
    // The boundary that matches is a FLUSH-class stall that is the FIRST VIFcode
    // after a kick — the guest explicitly waiting for VU1 and the GIF PATH1
    // transfer to drain BEFORE it starts uploading the next batch. That is a
    // long window in which PATH1 is idle and stays idle, which is exactly when
    // the real GIF hands the bus to the lower-priority PATH3. The in-batch FLUSH
    // that every batch carries just before its MSCAL is NOT such a point: the
    // next kick follows immediately.
    //
    // Verified against the frozen hardware pair (docs/TESTING.md T-P7-BOUNDARY):
    // every hardware run boundary in the phase-7 head falls on one of our XGKICK
    // packet boundaries, and this rule fires on 3 of those 7 with ZERO false
    // positives — it never splits a run hardware keeps whole. Nothing here reads
    // GS state, a framebuffer, a context, or an analyzer-defined "draw run".
    bool stallFollowsKick = true;
    uint32_t vifCodesThisTransfer = 0u;

    while (pos + 4 <= sizeBytes)
    {
        if (m_vif1PendingPath2ImageQwc != 0u)
        {
            const uint32_t availableQw = (sizeBytes - pos) / 16u;
            if (availableQw == 0u)
            {
                break;
            }

            const uint32_t chunkQw = std::min<uint32_t>(m_vif1PendingPath2ImageQwc, availableQw);
            // Forward the continuation raw: the GS keeps per-path IMAGE
            // continuation state, so re-tagging it here would double-handle
            // (the GS would eat the synthetic GIFtag as pixel data).
            submitGifPacket(GifPathId::Path2,
                            data + pos,
                            chunkQw * 16u,
                            true,
                            m_vif1PendingPath2DirectHl);

            rrvNoteVif1Path2("image-cont", data + pos, chunkQw * 16u,
                             m_vif1PendingPath2ImageQwc);
            pos += chunkQw * 16u;
            m_vif1PendingPath2ImageQwc -= chunkQw;
            if (m_vif1PendingPath2ImageQwc == 0u)
            {
                m_vif1PendingPath2DirectHl = false;
            }
            continue;
        }

        const uint32_t cmdPos = pos;
        uint32_t cmd;
        memcpy(&cmd, data + pos, 4);
        pos += 4;
        ++vifCodesThisTransfer;

        uint8_t opcode = (cmd >> 24) & 0x7F;
        uint16_t imm = cmd & 0xFFFF;
        uint8_t num = (cmd >> 16) & 0xFF;

        const bool isStall =
            (opcode == VIF_FLUSH || opcode == VIF_FLUSHE || opcode == VIF_FLUSHA);
        if (m_path3PacedActive && isStall && stallFollowsKick)
        {
            m_path3PacedVif1Cursor = m_path3PacedVif1Base + cmdPos;
            pacePath3();
        }
        // Only a DATA upload into VU1 means the guest has started building the
        // next batch; after that, the batch's own FLUSH is immediately followed
        // by its MSCAL and the GIF gets no idle window. PATH2 traffic (DIRECT /
        // DIRECTHL) and the small state codes do not start a batch, and the
        // chain begins armed because no kick has happened yet — which is where
        // hardware drains the head of the PATH3 chain before the first XGKICK.
        if (opcode == VIF_MPG || opcode >= 0x60)
            stallFollowsKick = false;
        if (opcode == VIF_MSCAL || opcode == VIF_MSCALF || opcode == VIF_MSCNT)
            stallFollowsKick = true;
        const bool irq = (cmd & 0x80000000u) != 0u;
        const bool carDmaCommand =
            m_carDmaVifDiagActive && cmdPos >= m_carDmaVifDiagBegin && cmdPos < m_carDmaVifDiagEnd;
        if (carDmaCommand)
        {
            std::fprintf(stderr,
                         "[car-dma:%llu] VIF pos=%u interval=[%u,%u) opcode=%02x imm=%04x num=%u irq=%u\n",
                         static_cast<unsigned long long>(m_carDmaVifDiagChain), cmdPos,
                         m_carDmaVifDiagBegin, m_carDmaVifDiagEnd, opcode, imm,
                         static_cast<uint32_t>(num), irq ? 1u : 0u);
        }

        const uint32_t opcodeIndex = s_debugVif1OpcodeCount.fetch_add(1, std::memory_order_relaxed);
        // Surface VU1 microprogram traffic (MPG upload / MSCAL* run) during early
        // boot only; past the cap this floods stdout every frame and starves the
        // GS/EE threads, so keep it bounded like the generic opcode log below.
        if (opcodeIndex < 4096u &&
            (opcode == 0x4Au || opcode == 0x14u || opcode == 0x15u || opcode == 0x17u))
        {
            RUNTIME_LOG("[vif1:VU1] idx=" << opcodeIndex
                                          << " opcode=0x" << std::hex << static_cast<uint32_t>(opcode)
                                          << std::dec << std::endl);
        }
        if (opcodeIndex < 160u)
        {
            RUNTIME_LOG("[vif1:cmd] idx=" << opcodeIndex
                                          << " opcode=0x" << std::hex << static_cast<uint32_t>(opcode)
                                          << " imm=0x" << imm
                                          << std::dec
                                          << " num=" << static_cast<uint32_t>(num)
                                          << " irq=" << static_cast<uint32_t>(irq ? 1u : 0u)
                                          << std::endl);
        }

        // Track most-recent command for VIFn_CODE emulation.
        vif1_regs.code = cmd;
        vif1_regs.num = num;
        if (irq)
            vif1_regs.stat |= (1u << 11); // INT

        if (opcode == VIF_NOP)
        {
            continue;
        }
        else if (opcode == VIF_STCYCL)
        {
            vif1_regs.cycle = imm;
            continue;
        }
        else if (opcode == VIF_OFFSET)
        {
            // VIFcode OFFSET: OFST <- imm; DBF <- 0; TOPS <- BASE. It must NOT
            // modify BASE. The previous `base = oldTops` was non-spec and, once
            // DBF had been toggled by an MSCAL, corrupted BASE to the stale
            // BASE+OFST (e.g. 0 -> 448) on the next frame -- which pushed the
            // double-buffer (TOPS toggling 448<->896 instead of 0<->448) on top
            // of the static VU data at qword ~908-920, clobbering the per-batch
            // leading GIFtag template the geometry program reads from qword 919.
            vif1_regs.ofst = imm & 0x3FFu;
            vif1_regs.stat &= ~(1u << 7); // clear DBF
            recomputeVif1Tops();          // dbf=0 -> tops = base
            continue;
        }
        else if (opcode == VIF_BASE)
        {
            vif1_regs.base = imm & 0x3FFu;
            recomputeVif1Tops();
            continue;
        }
        else if (opcode == VIF_ITOP)
        {
            vif1_regs.itop = imm & 0x3FFu;
            continue;
        }
        else if (opcode == VIF_STMOD)
        {
            vif1_regs.mode = imm & 3u;
            continue;
        }
        else if (opcode == VIF_MSKPATH3)
        {
            // VIF command docs: MSKPATH3 uses IMMEDIATE bit 15.
            const bool wasMasked = m_path3Masked;
            m_path3Masked = (imm & 0x8000u) != 0u;
            rrvNoteDmaOrderEvent("mskpath3", m_path3Masked ? 1ull : 0ull,
                                 wasMasked ? 1ull : 0ull);
            if (wasMasked && !m_path3Masked)
                flushMaskedPath3Packets();
            continue;
        }
        else if (opcode == VIF_MARK)
        {
            vif1_regs.mark = imm;
            vif1_regs.stat |= (1u << 6); // MRK
            continue;
        }
        else if (opcode == VIF_FLUSHE || opcode == VIF_FLUSH || opcode == VIF_FLUSHA)
        {
            continue;
        }
        else if (opcode == VIF_MSCAL || opcode == VIF_MSCALF)
        {
            // TOP = TOPS snapshot BEFORE the DBF toggle: the VU reads (via XTOP)
            // the buffer that UNPACK just filled, while the toggle points the
            // next UNPACK at the other buffer.
            const uint32_t vuTop = vif1_regs.tops & 0x3FFu;
            vif1_regs.top = vuTop;
            vif1_regs.itops = vif1_regs.itop & 0x3FFu;
            vif1_regs.stat ^= (1u << 7); // toggle DBF
            recomputeVif1Tops();
            uint32_t startPC = (uint32_t)imm * 8u;
            const uint32_t kickIndex = s_debugVu1KickCount.fetch_add(1, std::memory_order_relaxed);
            if (kickIndex < 48u)
            {
                RUNTIME_LOG("[vif1:mscal] idx=" << kickIndex
                                                << " opcode=0x" << std::hex << static_cast<uint32_t>(opcode)
                                                << " imm=0x" << imm
                                                << " startPc=0x" << startPC
                                                << " itop=0x" << vif1_regs.itop
                                                << std::dec << std::endl);
            }
            ps2_diag::CarDmaVu1Result carVu1Result;
            if (carDmaCommand)
                ps2_diag::beginCarDmaVu1Scope(m_carDmaVifDiagChain, cmdPos);
            if (m_vu1MscalCallback)
                m_vu1MscalCallback(startPC, vif1_regs.itop, vuTop);
            if (carDmaCommand)
            {
                carVu1Result = ps2_diag::endCarDmaVu1Scope();
                std::fprintf(stderr,
                             "[car-dma:%llu] %s pos=%u startPC=%08x XGKICK attempted=%u submitted=%u "
                             "invalid=%u overflow=%u empty=%u\n",
                             static_cast<unsigned long long>(m_carDmaVifDiagChain),
                             opcode == VIF_MSCALF ? "MSCALF" : "MSCAL", cmdPos, startPC,
                             carVu1Result.attempted, carVu1Result.submitted,
                             carVu1Result.skippedInvalid, carVu1Result.skippedOverflow,
                             carVu1Result.skippedEmpty);
            }
            continue;
        }
        else if (opcode == VIF_MSCNT)
        {
            // MSCNT snapshots the buffer just filled into TOP before toggling
            // DBF/TOPS for the next UNPACK, just like MSCAL. A resumed VU1
            // program observes this snapshot through XTOP.
            const uint32_t vuTop = vif1_regs.tops & 0x3FFu;
            vif1_regs.top = vuTop;
            vif1_regs.itops = vif1_regs.itop & 0x3FFu;
            vif1_regs.stat ^= (1u << 7); // toggle DBF
            recomputeVif1Tops();
            const uint32_t kickIndex = s_debugVu1KickCount.fetch_add(1, std::memory_order_relaxed);
            if (kickIndex < 48u)
            {
                RUNTIME_LOG("[vif1:mscnt] idx=" << kickIndex
                                                << " itop=0x" << std::hex << vif1_regs.itop
                                                << " pc=resume"
                                                << std::dec << std::endl);
            }
            ps2_diag::CarDmaVu1Result carVu1Result;
            if (carDmaCommand)
                ps2_diag::beginCarDmaVu1Scope(m_carDmaVifDiagChain, cmdPos);
            if (m_vu1MscntCallback)
                m_vu1MscntCallback(vif1_regs.itop, vuTop);
            if (carDmaCommand)
            {
                carVu1Result = ps2_diag::endCarDmaVu1Scope();
                std::fprintf(stderr,
                             "[car-dma:%llu] MSCNT pos=%u XGKICK attempted=%u submitted=%u "
                             "invalid=%u overflow=%u empty=%u\n",
                             static_cast<unsigned long long>(m_carDmaVifDiagChain), cmdPos,
                             carVu1Result.attempted, carVu1Result.submitted,
                             carVu1Result.skippedInvalid, carVu1Result.skippedOverflow,
                             carVu1Result.skippedEmpty);
            }
            continue;
        }
        else if (opcode == VIF_STMASK)
        {
            if (pos + 4 > sizeBytes)
                break;
            uint32_t maskValue = 0;
            std::memcpy(&maskValue, data + pos, sizeof(maskValue));
            vif1_regs.mask = maskValue;
            pos += 4;
            continue;
        }
        else if (opcode == VIF_STROW)
        {
            if (pos + 16 > sizeBytes)
                break;
            std::memcpy(vif1_regs.row, data + pos, 16);
            pos += 16;
            continue;
        }
        else if (opcode == VIF_STCOL)
        {
            if (pos + 16 > sizeBytes)
                break;
            std::memcpy(vif1_regs.col, data + pos, 16);
            pos += 16;
            continue;
        }
        else if (opcode == VIF_MPG)
        {
            uint32_t destAddr = (uint32_t)imm * 8u;
            // VIF MPG semantics: NUM==0 means 256 instructions (2048 bytes).
            // MPG payload is instruction-packed and should not be QW-aligned.
            const uint32_t instructionCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);
            const uint32_t mpgBytes = instructionCount * 8u;
            if (m_vu1Code && destAddr < PS2_VU1_CODE_SIZE && mpgBytes > 0)
            {
                uint32_t copyBytes = mpgBytes;
                if (destAddr + copyBytes > PS2_VU1_CODE_SIZE)
                    copyBytes = PS2_VU1_CODE_SIZE - destAddr;
                if (pos + copyBytes <= sizeBytes)
                {
                    if (std::memcmp(m_vu1Code + destAddr, data + pos, copyBytes) != 0)
                    {
                        std::memcpy(m_vu1Code + destAddr, data + pos, copyBytes);
                        rrvVuCodeChanged(1);
                    }
#if defined(_DEBUG)
                    // RRV_RUNTIME_LOG: VU1 microcode (MPG) upload count, reported via
                    // [perf:vu1] mpg-uploads/s in ps2_runtime.cpp (VU1-JIT cache sizing).
                    ps2_diag::g_vu1MpgUploads.fetch_add(1, std::memory_order_relaxed);
#endif
                }
            }
            pos += mpgBytes;
            if (pos > sizeBytes)
                break;
            continue;
        }
        else if (opcode == VIF_DIRECT || opcode == VIF_DIRECTHL)
        {
            uint32_t qwCount = imm;
            if (qwCount == 0)
                qwCount = 65536; // IMM==0 means 65536 qwords (PCSX2 Vif_Codes.cpp)
            const uint32_t declaredQw = qwCount;
            const uint32_t availableQw = (sizeBytes - pos) / 16u;
            const bool truncated = qwCount > availableQw;
            if (qwCount > availableQw)
                qwCount = availableQw;

            if (qwCount > 0)
            {
                const bool directHl = (opcode == VIF_DIRECTHL);
                submitGifPacket(GifPathId::Path2, data + pos, qwCount * 16, true, directHl);
                rrvNoteVif1Path2(directHl ? "directhl" : "direct", data + pos,
                                 qwCount * 16u, imm, cmd, cmdPos, sizeBytes,
                                 vifCodesThisTransfer);

                // KNOWN_ISSUES #18 census. A DIRECT whose declared count outran
                // its own DMA transfer would lose the remainder here (hardware
                // keeps the VIFcode in flight and finishes it out of the next
                // transfer). Measured over a full cold attract: this NEVER
                // happens in RR5 — every DIRECT/DIRECTHL is delivered whole. The
                // carry is therefore deliberately NOT implemented; implement it
                // only against a case that actually fires.
                if (truncated)
                    rrvNoteVif1DirectTruncation(declaredQw, qwCount);

                const uint32_t imageQw = gifImageQwcFromTag(data + pos, qwCount * 16u);
                if (imageQw != 0u)
                {
                    const uint32_t inlineImageQw = (qwCount > 0u) ? (qwCount - 1u) : 0u;
                    if (imageQw > inlineImageQw)
                    {
                        m_vif1PendingPath2ImageQwc = imageQw - inlineImageQw;
                        m_vif1PendingPath2DirectHl = directHl;
                    }
                }
            }

            pos += qwCount * 16;
            if (truncated)
            {
                pos = sizeBytes;
                break;
            }
            continue;
        }
        else if ((opcode & 0x60) == 0x60)
        {
            uint8_t vn = (opcode >> 2) & 0x3;
            uint8_t vl = opcode & 0x3;
            const bool maskEnable = (opcode & 0x10u) != 0u;
            int components = vn + 1;
            int bitsPerComponent = 32;
            switch (vl)
            {
            case 0:
                bitsPerComponent = 32;
                break;
            case 1:
                bitsPerComponent = 16;
                break;
            case 2:
                bitsPerComponent = 8;
                break;
            case 3:
                bitsPerComponent = (vn == 3) ? 4 : 16;
                break;
            default:
                break;
            }
            int bitsPerVector = (vl == 3 && vn == 3) ? 16 : (components * bitsPerComponent);
            uint32_t bytesPerVector = (bitsPerVector + 7) / 8;
            // UNPACK semantics: NUM is 8-bit and NUM==0 means 256 vectors (writes).
            const uint32_t writeVectorCount = (num == 0u) ? 256u : static_cast<uint32_t>(num);

            // STCYCL controls write cycles for UNPACK.
            uint32_t cl = vif1_regs.cycle & 0xFFu;
            uint32_t wl = (vif1_regs.cycle >> 8) & 0xFFu;
            if (cl == 0u)
                cl = 1u;
            if (wl == 0u)
                wl = 1u;

            uint32_t sourceVectorCount = writeVectorCount;
            if (cl < wl)
            {
                const uint32_t fullBlocks = writeVectorCount / wl;
                uint32_t remainder = writeVectorCount % wl;
                if (remainder > cl)
                    remainder = cl;
                sourceVectorCount = fullBlocks * cl + remainder;
            }

            uint32_t totalBytes = sourceVectorCount * bytesPerVector;
            totalBytes = (totalBytes + 3) & ~3u;

            uint32_t vuAddr = (uint32_t)imm & 0x3FFu;
            if ((imm & 0x8000u) != 0u)
                vuAddr = (vuAddr + (vif1_regs.tops & 0x3FFu)) & 0x3FFu;

            const bool zeroExtend = (imm & 0x4000u) != 0u;
            if (m_vu1Data && totalBytes > 0 && pos + totalBytes <= sizeBytes)
            {
                const uint8_t *srcBase = data + pos;
                static const bool gate4MatrixTrace = [] {
                    const char *v = std::getenv("RRV_VU_MATRIX_TRACE");
                    return v && v[0] && v[0] != '0';
                }();
                // Gate-4 G4-8 fast path: no mask, MODE 0, CL >= WL, handled
                // format. Writes exactly the lanes the generic loop below
                // would; any other case falls through to it unchanged.
                if (g_rrvVif1UnpackFastV1 && !maskEnable && (vif1_regs.mode & 3u) == 0u && cl >= wl &&
                    (vl != 3u || vn == 3u) && !gate4MatrixTrace)
                {
                    const bool quirk = vifUnpackHwQuirkEnabled() && vl != 3u;
                    // One instance of the loop per format (rrvVif1UnpackRun above):
                    // the per-vector tests on VL, the component count, the sign
                    // extension, the quirk and CL == WL are decided here, once.
                    rrvVif1UnpackDispatch(m_vu1Data, srcBase, data, sizeBytes, writeVectorCount, vuAddr, cl, wl,
                                          static_cast<uint32_t>(vl), components, zeroExtend, quirk);
                }
                else
                {
                uint32_t srcIndex = 0u;
                for (uint32_t writeIndex = 0; writeIndex < writeVectorCount; ++writeIndex)
                {
                    const uint32_t cyclePos = writeIndex % wl;
                    const bool sourceAvailable = (cl >= wl) || (cyclePos < cl);

                    uint32_t destVec = 0;
                    if (cl >= wl)
                    {
                        destVec = (vuAddr + (writeIndex / wl) * cl + cyclePos) & 0x3FFu;
                    }
                    else
                    {
                        destVec = (vuAddr + writeIndex) & 0x3FFu;
                    }

                    uint32_t destOff = destVec * 16u;
                    if (destOff + 16u > PS2_VU1_DATA_SIZE)
                    {
                        if (sourceAvailable && srcIndex < sourceVectorCount)
                            ++srcIndex;
                        continue;
                    }

                    uint32_t lanes[4] = {0u, 0u, 0u, 0u};
                    std::memcpy(lanes, m_vu1Data + destOff, sizeof(lanes));
                    uint32_t decompressed[4] = {lanes[0], lanes[1], lanes[2], lanes[3]};
                    bool decoded = false;

                    const uint8_t *srcVec = nullptr;
                    if (sourceAvailable && srcIndex < sourceVectorCount)
                    {
                        srcVec = srcBase + srcIndex * bytesPerVector;
                        ++srcIndex;
                        decoded = true;
                    }

                    auto extend16 = [&](uint16_t raw) -> uint32_t
                    {
                        if (zeroExtend)
                            return static_cast<uint32_t>(raw);
                        return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(raw)));
                    };

                    auto extend8 = [&](uint8_t raw) -> uint32_t
                    {
                        if (zeroExtend)
                            return static_cast<uint32_t>(raw);
                        return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(raw)));
                    };

                    bool handledFormat = true;
                    if (!decoded)
                    {
                        handledFormat = false;
                    }
                    else if (vl == 0u)
                    {
                        if (components == 1)
                        {
                            uint32_t scalar = 0;
                            std::memcpy(&scalar, srcVec, sizeof(scalar));
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                uint32_t scalar = 0;
                                std::memcpy(&scalar, srcVec + c * 4u, sizeof(scalar));
                                decompressed[c] = scalar;
                            }
                        }
                    }
                    else if (vl == 1u)
                    {
                        if (components == 1)
                        {
                            uint16_t raw = 0;
                            std::memcpy(&raw, srcVec, sizeof(raw));
                            const uint32_t scalar = extend16(raw);
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                uint16_t raw = 0;
                                std::memcpy(&raw, srcVec + c * 2u, sizeof(raw));
                                decompressed[c] = extend16(raw);
                            }
                        }
                    }
                    else if (vl == 2u)
                    {
                        if (components == 1)
                        {
                            const uint32_t scalar = extend8(srcVec[0]);
                            decompressed[0] = scalar;
                            decompressed[1] = scalar;
                            decompressed[2] = scalar;
                            decompressed[3] = scalar;
                        }
                        else
                        {
                            const uint32_t limit = (components > 4) ? 4u : static_cast<uint32_t>(components);
                            for (uint32_t c = 0; c < limit; ++c)
                            {
                                decompressed[c] = extend8(srcVec[c]);
                            }
                        }
                    }
                    else if (vl == 3u && vn == 3u)
                    {
                        // V4-5: packed color-like format in a single 16-bit value.
                        uint16_t packed = 0;
                        std::memcpy(&packed, srcVec, sizeof(packed));
                        decompressed[0] = packed & 0x1Fu;
                        decompressed[1] = (packed >> 5) & 0x1Fu;
                        decompressed[2] = (packed >> 10) & 0x1Fu;
                        decompressed[3] = (packed >> 15) & 0x01u;
                    }
                    else
                    {
                        handledFormat = false;
                    }

                    // PS2 hardware fills the components a V2/V3 UNPACK does not
                    // declare — see vifUnpackHwQuirkEnabled() above. V2 repeats
                    // the pair (v1v0v1v0); V3 runs V4 logic, so W is the next
                    // word of the source stream. Reading past the element is
                    // what hardware does; clamp to the packet we were handed.
                    if (handledFormat && decoded && vifUnpackHwQuirkEnabled() && vl != 3u)
                    {
                        if (components == 2)
                        {
                            decompressed[2] = decompressed[0];
                            decompressed[3] = decompressed[1];
                        }
                        else if (components == 3)
                        {
                            const uint32_t compBytes = bytesPerVector / 3u;
                            const uint8_t *wSrc = srcVec + 3u * compBytes;
                            const size_t offset = static_cast<size_t>(wSrc - data);
                            if (offset + compBytes <= sizeBytes)
                            {
                                if (vl == 0u)
                                    std::memcpy(&decompressed[3], wSrc, sizeof(uint32_t));
                                else if (vl == 1u)
                                {
                                    uint16_t raw = 0;
                                    std::memcpy(&raw, wSrc, sizeof(raw));
                                    decompressed[3] = extend16(raw);
                                }
                                else
                                    decompressed[3] = extend8(wSrc[0]);
                            }
                            else
                            {
                                decompressed[3] = 0u;
                            }
                        }
                    }

                    // Unknown compressed format fallback: preserve legacy raw-copy behavior.
                    if (!handledFormat && decoded && !maskEnable && (vif1_regs.mode == 0u || vif1_regs.mode == 3u))
                    {
                        uint32_t copyBytes = (bytesPerVector < 16u) ? bytesPerVector : 16u;
                        std::memcpy(m_vu1Data + destOff, srcVec, copyBytes);
                        static const bool matrixTrace = [] {
                            const char *v = std::getenv("RRV_VU_MATRIX_TRACE");
                            return v && v[0] && v[0] != '0';
                        }();
                        if (matrixTrace && vuMatrixTraceWatch(destVec))
                        {
                            const auto *dst = reinterpret_cast<const uint32_t *>(m_vu1Data + destOff);
                            const uint32_t phase = m_rdram ? *reinterpret_cast<const uint32_t *>(m_rdram + 0x334E94u) : 0xffffffffu;
                            std::fprintf(stderr,
                                         "[vif-matrix-write] phase=%u dest=%03x opcode=%02x src=%08x/%08x/%08x/%08x dst=%08x/%08x/%08x/%08x\n",
                                         phase, destVec, opcode,
                                         decoded && bytesPerVector >= 4u ? *reinterpret_cast<const uint32_t *>(srcVec) : 0u,
                                         decoded && bytesPerVector >= 8u ? *reinterpret_cast<const uint32_t *>(srcVec + 4u) : 0u,
                                         decoded && bytesPerVector >= 12u ? *reinterpret_cast<const uint32_t *>(srcVec + 8u) : 0u,
                                         decoded && bytesPerVector >= 16u ? *reinterpret_cast<const uint32_t *>(srcVec + 12u) : 0u,
                                         dst[0], dst[1], dst[2], dst[3]);
                        }
                        continue;
                    }

                    const bool canAdd = (vl != 3u || vn != 3u);
                    const uint32_t mode = vif1_regs.mode & 3u;
                    const uint32_t colIdx = (cyclePos > 3u) ? 3u : cyclePos;
                    const uint32_t maskCycle = (cyclePos > 3u) ? 3u : cyclePos;

                    for (uint32_t field = 0u; field < 4u; ++field)
                    {
                        uint32_t maskSpec = 0u;
                        if (maskEnable)
                        {
                            const uint32_t shift = ((maskCycle * 4u) + field) * 2u;
                            maskSpec = (vif1_regs.mask >> shift) & 0x3u;
                        }

                        // In fill-write cycles with suspended source reads, treat raw-data selections as row-fill.
                        if (!decoded && maskSpec == 0u)
                            maskSpec = 1u;

                        uint32_t writeVal = lanes[field];
                        if (maskSpec == 0u)
                        {
                            if (handledFormat)
                            {
                                writeVal = decompressed[field];
                                if (canAdd && (mode == 1u || mode == 2u))
                                {
                                    writeVal = writeVal + vif1_regs.row[field];
                                    if (mode == 2u)
                                        vif1_regs.row[field] = writeVal;
                                }
                            }
                        }
                        else if (maskSpec == 1u)
                        {
                            writeVal = vif1_regs.row[field];
                        }
                        else if (maskSpec == 2u)
                        {
                            writeVal = vif1_regs.col[colIdx];
                        }
                        else
                        {
                            continue; // write-protect
                        }

                        lanes[field] = writeVal;
                    }

                    std::memcpy(m_vu1Data + destOff, lanes, sizeof(lanes));
                    static const bool matrixTrace = [] {
                        const char *v = std::getenv("RRV_VU_MATRIX_TRACE");
                        return v && v[0] && v[0] != '0';
                    }();
                    if (matrixTrace && vuMatrixTraceWatch(destVec))
                    {
                        uint32_t raw[4] = {0u, 0u, 0u, 0u};
                        if (decoded)
                            std::memcpy(raw, srcVec, std::min<uint32_t>(bytesPerVector, sizeof(raw)));
                        const uint32_t phase = m_rdram ? *reinterpret_cast<const uint32_t *>(m_rdram + 0x334E94u) : 0xffffffffu;
                        const size_t packetOffset = (m_vif1TraceDataBase && srcVec >= m_vif1TraceDataBase)
                            ? static_cast<size_t>(srcVec - m_vif1TraceDataBase) : 0u;
                        uint32_t guestSource = 0xffffffffu;
                        size_t blockOffset = 0u;
                        if (m_vif1TraceSourceBlocks)
                        {
                            for (const auto &block : *m_vif1TraceSourceBlocks)
                            {
                                if (packetOffset >= block.offset && packetOffset < block.offset + block.bytes)
                                {
                                    guestSource = block.source + static_cast<uint32_t>(packetOffset - block.offset);
                                    blockOffset = block.offset;
                                    break;
                                }
                            }
                        }
                        std::fprintf(stderr,
                                     "[vif-matrix-write] phase=%u chain=%llu packet-off=%08zx block-off=%08zx guest-src=%08x dest=%03x opcode=%02x src=%08x/%08x/%08x/%08x dst=%08x/%08x/%08x/%08x\n",
                                     phase, static_cast<unsigned long long>(m_vif1TraceActiveChainId), packetOffset, blockOffset, guestSource, destVec, opcode,
                                     raw[0], raw[1], raw[2], raw[3], lanes[0], lanes[1], lanes[2], lanes[3]);
                    }
                }
                } // Gate-4 G4-8: end of the generic UNPACK loop
            }
            pos += totalBytes;

            if (pos > sizeBytes)
                break;
            continue;
        }
        else
        {
            continue;
        }
    }
}
