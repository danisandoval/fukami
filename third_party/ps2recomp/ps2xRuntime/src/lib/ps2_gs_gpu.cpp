#include "runtime/ps2_gs_gpu.h"
#include "runtime/ps2_gs_common.h"
#include "runtime/ps2_gs_psmct16.h"
#include "ps2_log.h"
#include "ps2_syscalls.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_gs_memory.h"
// NOTE: ps2_gs_psm{ct32,t4,t8}.h are reference-only oracles (see their
// header banners) and are deliberately NOT included here -- this file
// addresses VRAM through GSMem (runtime/ps2_gs_memory.h) only.
#include "runtime/diag_counters.h" // RRV_RUNTIME_LOG: tile-binning feasibility stats
#if !defined(PS2X_RRV_FIELD_ONLY)
#include "rrv_ir_hooks.h"          // milestone B1: write-only Renderer-IR capture (RRV_IR_CAPTURE)
#endif
#include "rrv_gs_record_hooks.h"   // milestone B4: write-only GS-stream recorder (RRV_GS_RECORD)
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace
{
    // B-1 (RRV_CAR_GIF_DIAG): what actually reaches GIF/GS while the demo car
    // slot is live. Aggregates every GIFtag decoded during a car-live packet,
    // keyed by (a) the PATH + originating VU1 XGKICK micro-PC (PATH1 only; 0 for
    // PATH2/3) and (b) the PATH + PRIM material register. Answers whether the car
    // uses XGKICK PCs / material sets outside the environment-only set, or a
    // data-driven PATH3 GIF builder, or is simply not submitted. Dumped every 2s
    // to stderr as [cargif] lines. Zero cost unless RRV_CAR_GIF_DIAG is set.
    // Key layout: (phase<<40)|(path<<36)|value36 . phase = demo phase word
    // 0x334E94, path = GifPathId, value = XGKICK micro-PC or PRIM register.
    static inline uint64_t carGifKey(uint32_t phase, uint32_t path, uint32_t value)
    {
        return (static_cast<uint64_t>(phase & 0xFFu) << 40) |
               (static_cast<uint64_t>(path & 0xFu) << 36) |
               (static_cast<uint64_t>(value) & 0xFFFFFFFFFull);
    }
    struct CarGifBBox
    {
        float minx = 0.f, miny = 0.f, maxx = 0.f, maxy = 0.f;
        uint64_t verts = 0, drawing = 0, nonfinite = 0, offscreen = 0, extreme = 0;
        bool init = false;
    };
    struct CarGifDiag
    {
        bool enabled = false;
        std::mutex mtx;
        std::unordered_map<uint64_t, uint64_t> tagsByPhasePathPc;   // -> giftags
        std::unordered_map<uint64_t, uint64_t> vertsByPhasePathPc;  // -> sum(nloop)
        std::unordered_map<uint64_t, uint64_t> tagsByPhasePathPrim; // -> giftags
        std::unordered_map<uint64_t, CarGifBBox> bboxByPhasePathPc;  // screen-space vertex bbox
        uint64_t carLiveTags = 0;
        uint64_t carLivePackets = 0;
        std::chrono::steady_clock::time_point last;
    };
    CarGifDiag g_carGifDiag;

    void carGifDiagInit()
    {
        static std::once_flag once;
        std::call_once(once, [] {
            const char *v = std::getenv("RRV_CAR_GIF_DIAG");
            g_carGifDiag.enabled = (v && v[0] && v[0] != '0');
            g_carGifDiag.last = std::chrono::steady_clock::now();
        });
    }

    void carGifDiagDumpLocked()
    {
        auto histLine = [](const char *label, std::unordered_map<uint64_t, uint64_t> &m) {
            std::vector<std::pair<uint64_t, uint64_t>> v(m.begin(), m.end());
            std::sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.first < b.first; });
            std::fprintf(stderr, "[cargif] %s:", label);
            for (auto &p : v)
                std::fprintf(stderr, " ph%02x/p%u/%llx=%llu",
                             static_cast<uint32_t>((p.first >> 40) & 0xFFu),
                             static_cast<uint32_t>((p.first >> 36) & 0xFu),
                             static_cast<unsigned long long>(p.first & 0xFFFFFFFFFull),
                             static_cast<unsigned long long>(p.second));
            std::fprintf(stderr, "\n");
        };
        std::fprintf(stderr, "[cargif] ---- carLivePackets=%llu carLiveTags=%llu (key=phase/path/value) ----\n",
                     static_cast<unsigned long long>(g_carGifDiag.carLivePackets),
                     static_cast<unsigned long long>(g_carGifDiag.carLiveTags));
        histLine("tags  by phase/path/XGKICKpc", g_carGifDiag.tagsByPhasePathPc);
        histLine("verts by phase/path/XGKICKpc", g_carGifDiag.vertsByPhasePathPc);
        histLine("tags  by phase/path/PRIM    ", g_carGifDiag.tagsByPhasePathPrim);
        {
            std::vector<std::pair<uint64_t, CarGifBBox>> v(g_carGifDiag.bboxByPhasePathPc.begin(),
                                                           g_carGifDiag.bboxByPhasePathPc.end());
            std::sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.first < b.first; });
            for (auto &p : v)
                std::fprintf(stderr,
                             "[cargif] bbox ph%02x/p%u/%llx verts=%llu draw=%llu bbox=[%.0f,%.0f..%.0f,%.0f] "
                             "offscreen=%llu extreme=%llu nonfinite=%llu\n",
                             static_cast<uint32_t>((p.first >> 40) & 0xFFu),
                             static_cast<uint32_t>((p.first >> 36) & 0xFu),
                             static_cast<unsigned long long>(p.first & 0xFFFFFFFFFull),
                             static_cast<unsigned long long>(p.second.verts),
                             static_cast<unsigned long long>(p.second.drawing),
                             p.second.minx, p.second.miny, p.second.maxx, p.second.maxy,
                             static_cast<unsigned long long>(p.second.offscreen),
                             static_cast<unsigned long long>(p.second.extreme),
                             static_cast<unsigned long long>(p.second.nonfinite));
        }
    }

    static constexpr uint32_t kDefaultDisplayWidth = 640u;
    static constexpr uint32_t kDefaultDisplayHeight = 448u;
    static constexpr uint32_t kHostFrameWidth = 640u;
    static constexpr uint32_t kHostFrameHeight = 512u;

    uint16_t encodeFramePixelPSMCT16(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    {
        return static_cast<uint16_t>(((r >> 3) & 0x1Fu) |
                                     (((g >> 3) & 0x1Fu) << 5) |
                                     (((b >> 3) & 0x1Fu) << 10) |
                                     ((a >= 0x40u) ? 0x8000u : 0u));
    }

    uint32_t addrPSMCT16Family(uint32_t basePtr, uint32_t width, uint8_t psm, uint32_t x, uint32_t y)
    {
        switch (psm)
        {
        case GS_PSM_CT16:
            return GSPSMCT16::addrPSMCT16(basePtr, width, x, y);
        case GS_PSM_CT16S:
            return GSPSMCT16::addrPSMCT16S(basePtr, width, x, y);
        case GS_PSM_Z16:
            return GSPSMCT16::addrPSMZ16(basePtr, width, x, y);
        case GS_PSM_Z16S:
            return GSPSMCT16::addrPSMZ16S(basePtr, width, x, y);
        default:
            return 0u;
        }
    }

    static inline uint64_t loadLE64(const uint8_t *p)
    {
        uint64_t v;
        std::memcpy(&v, p, 8);
        return v;
    }

    // `smode264` is the SMODE2 value that is live for this DISPLAY register.
    // DH counts *output scanlines*, not framebuffer lines: in interlaced FRAME
    // mode (INT=1, FFMD=1) each buffer line is shown on both fields, so the
    // buffer the CRTC reads is (DH+1)/2 lines tall. Ignoring that reads twice
    // the buffer's height — measured on `local/gt/gt_A2_girl.gsr` (PCSX2 ground
    // truth: INT=1 FFMD=1 DW=2559 MAGH=3 DH=447, i.e. 640x224 at fbp 0 and 70,
    // exactly 70 pages = 224 rows apart, SCISSOR y[0..223]): a 448-row read
    // returned the frame stacked on top of the *other* double-buffer half.
    // Callers that pass 0 (no SMODE2 context) keep the raw DH+1 behaviour.
    void decodeDisplaySize(uint64_t display64, uint32_t &outWidth, uint32_t &outHeight,
                           uint64_t smode264)
    {
        const uint32_t dx = static_cast<uint32_t>((display64 >> 0) & 0x0FFFu);
        const uint32_t dy = static_cast<uint32_t>((display64 >> 12) & 0x07FFu);
        const uint32_t dw = static_cast<uint32_t>((display64 >> 32) & 0x0FFFu);
        const uint32_t dh = static_cast<uint32_t>((display64 >> 44) & 0x07FFu);
        const uint32_t magh = static_cast<uint32_t>((display64 >> 23) & 0x0Fu);

        outWidth = (dw + 1u) / (magh + 1u);
        outHeight = dh + 1u;

        // The "is DISPLAY programmed at all" guard runs on the RAW register value,
        // exactly as before, so this change can never newly trip it; the FFMD
        // halving is a pure post-transform on an already-plausible height.
        if (outWidth < 64u || outHeight < 64u)
        {
            outWidth = kDefaultDisplayWidth;
            outHeight = kDefaultDisplayHeight;
        }
        else if ((smode264 & 0x1ull) != 0ull && ((smode264 >> 1) & 0x1ull) != 0ull)
        {
            outHeight /= 2u;
        }

        outWidth = std::min<uint32_t>(outWidth, kHostFrameWidth);
        outHeight = std::min<uint32_t>(outHeight, kHostFrameHeight);
    }

    GSFrameReg decodeDisplayFrame(uint64_t dispfb64)
    {
        GSFrameReg frame{};
        frame.fbp = static_cast<uint32_t>(dispfb64 & 0x1FFu);
        frame.fbw = static_cast<uint32_t>((dispfb64 >> 9) & 0x3Fu);
        frame.psm = static_cast<uint8_t>((dispfb64 >> 15) & 0x1Fu);
        return frame;
    }

    struct GSDisplayReadOrigin
    {
        uint32_t x = 0u;
        uint32_t y = 0u;
    };

    GSDisplayReadOrigin decodeDisplayReadOrigin(uint64_t dispfb64)
    {
        GSDisplayReadOrigin origin{};
        origin.x = static_cast<uint32_t>((dispfb64 >> 32) & 0x7FFu);
        origin.y = static_cast<uint32_t>((dispfb64 >> 43) & 0x7FFu);
        return origin;
    }

    bool hasDisplaySetup(uint64_t display64, const GSFrameReg &frame)
    {
        const uint32_t dw = static_cast<uint32_t>((display64 >> 32) & 0x0FFFu);
        const uint32_t dh = static_cast<uint32_t>((display64 >> 44) & 0x07FFu);
        const uint32_t magh = static_cast<uint32_t>((display64 >> 23) & 0x0Fu);
        return frame.fbw != 0u || dw != 0u || dh != 0u || magh != 0u;
    }

    struct GSPmodeState
    {
        bool enableCrt1 = false;
        bool enableCrt2 = false;
        bool mmod = false;
        bool amod = false;
        bool slbg = false;
        uint8_t alp = 0u;
    };

    GSPmodeState decodePmode(uint64_t pmode64)
    {
        GSPmodeState pmode{};
        pmode.enableCrt1 = (pmode64 & 0x1ull) != 0ull;
        pmode.enableCrt2 = (pmode64 & 0x2ull) != 0ull;
        pmode.mmod = ((pmode64 >> 5) & 0x1ull) != 0ull;
        pmode.amod = ((pmode64 >> 6) & 0x1ull) != 0ull;
        pmode.slbg = ((pmode64 >> 7) & 0x1ull) != 0ull;
        pmode.alp = static_cast<uint8_t>((pmode64 >> 8) & 0xFFu);
        return pmode;
    }

    struct GSSmode2State
    {
        bool interlaced = false;
        bool frameMode = true;
    };

    GSSmode2State decodeSMode2(uint64_t smode264)
    {
        GSSmode2State smode2{};
        smode2.interlaced = (smode264 & 0x1ull) != 0ull;
        smode2.frameMode = ((smode264 >> 1) & 0x1ull) != 0ull;
        return smode2;
    }

    void applyFieldPresentation(std::vector<uint8_t> &pixels, uint32_t width, uint32_t height, bool oddField)
    {
        if (pixels.empty() || width == 0u || height < 2u)
        {
            return;
        }

        const std::vector<uint8_t> source = pixels;
        for (uint32_t y = 0; y < height; ++y)
        {
            uint32_t sourceY = ((y >> 1u) << 1u) + (oddField ? 1u : 0u);
            if (sourceY >= height)
            {
                sourceY = height - 1u;
            }

            const uint8_t *srcRow = source.data() + (sourceY * kHostFrameWidth * 4u);
            uint8_t *dstRow = pixels.data() + (y * kHostFrameWidth * 4u);
            std::memcpy(dstRow, srcRow, width * 4u);
        }
    }

    void normalizePresentationAlpha(std::vector<uint8_t> &pixels, uint32_t width, uint32_t height)
    {
        if (pixels.empty() || width == 0u || height == 0u)
        {
            return;
        }

        for (uint32_t y = 0; y < height; ++y)
        {
            uint8_t *row = pixels.data() + (y * kHostFrameWidth * 4u);
            for (uint32_t x = 0; x < width; ++x)
            {
                row[x * 4u + 3u] = 255u;
            }
        }
    }

    uint8_t blendPresentationChannel(uint8_t src, uint8_t dst, uint32_t factor)
    {
        const int delta = static_cast<int>(src) - static_cast<int>(dst);
        return GSInternal::clampU8(static_cast<int>(dst) + ((delta * static_cast<int>(factor)) / 255));
    }

    uint32_t countNonBlackPixels(const std::vector<uint8_t> &pixels, uint32_t width, uint32_t height)
    {
        uint32_t count = 0u;
        for (uint32_t y = 0; y < height; ++y)
        {
            const uint8_t *row = pixels.data() + (y * kHostFrameWidth * 4u);
            for (uint32_t x = 0; x < width; ++x)
            {
                const uint8_t r = row[x * 4u + 0u];
                const uint8_t g = row[x * 4u + 1u];
                const uint8_t b = row[x * 4u + 2u];
                if (r != 0u || g != 0u || b != 0u)
                {
                    ++count;
                }
            }
        }
        return count;
    }

    bool clearFramebufferRect(GS* gs, const GSContext &ctx, uint32_t rgba)
    {
        if (ctx.frame.fbw == 0u)
        {
            return false;
        }

        const uint32_t stride = GSInternal::fbStride(ctx.frame.fbw, ctx.frame.psm);
        if (stride == 0u)
        {
            return false;
        }

        const u32 x0 = static_cast<u32>(std::max<int>(0, ctx.scissor.x0));
        const u32 x1 = static_cast<u32>(std::max<int>(x0, ctx.scissor.x1));
        const u32 y0 = static_cast<u32>(std::max<int>(0, ctx.scissor.y0));
        const u32 y1 = static_cast<u32>(std::max<int>(y0, ctx.scissor.y1));

        uint8_t r = static_cast<uint8_t>(rgba & 0xFFu);
        uint8_t g = static_cast<uint8_t>((rgba >> 8) & 0xFFu);
        uint8_t b = static_cast<uint8_t>((rgba >> 16) & 0xFFu);
        uint8_t a = static_cast<uint8_t>((rgba >> 24) & 0xFFu);

        u32 fbp = GSInternal::framePageBaseToBlock(ctx.frame.fbp);
        u32 fbw = std::max<u32>(ctx.frame.fbw, 1u);
        u32 fpsm = ctx.frame.psm;

        if ((ctx.fba & 0x1ull) != 0ull && ctx.frame.psm != GS_PSM_CT24)
        {
            a = static_cast<uint8_t>(a | 0x80u);
        }

        if (ctx.frame.psm == GS_PSM_CT32 || ctx.frame.psm == GS_PSM_CT24)
        {
            const uint32_t srcPixel =
                static_cast<uint32_t>(r) |
                (static_cast<uint32_t>(g) << 8) |
                (static_cast<uint32_t>(b) << 16) |
                (static_cast<uint32_t>(a) << 24);

            for (int y = y0; y <= y1; ++y)
            {
                for (int x = x0; x <= x1; ++x)
                {
                    uint32_t pixel = srcPixel;
                    if (ctx.frame.fbmsk != 0u)
                    {
                        const u32 c = gs->ReadVram(fpsm, fbp, fbw, x, y);
                        pixel = (pixel & ~ctx.frame.fbmsk) | (c & ctx.frame.fbmsk);
                    }
                    gs->WriteVram(fpsm, fbp, fbw, x, y, pixel);
                }
            }
            return true;
        }

        if (ctx.frame.psm == GS_PSM_CT16 || ctx.frame.psm == GS_PSM_CT16S)
        {
            const uint16_t srcPixel = encodeFramePixelPSMCT16(r, g, b, a);
            const uint16_t mask = static_cast<uint16_t>(ctx.frame.fbmsk & 0xFFFFu);
            const uint32_t widthBlocks = (ctx.frame.fbw != 0u) ? ctx.frame.fbw : 1u;
            const uint32_t basePtr = GSInternal::framePageBaseToBlock(ctx.frame.fbp);

            for (int y = y0; y <= y1; ++y)
            {
                for (int x = x0; x <= x1; ++x)
                {
                    uint16_t pixel = srcPixel;
                    if (mask != 0u)
                    {
                        const u16 c = gs->ReadVram(fpsm, fbp, fbw, x, y);
                        pixel = static_cast<uint16_t>((pixel & ~mask) | (c & mask));
                    }
                    gs->WriteVram(fpsm, fbp, fbw, x, y, pixel);
                }
            }
            return true;
        }

        return false;
    }

    std::atomic<uint32_t> s_debugGifPacketCount{0};
    std::atomic<uint32_t> s_debugGsRegisterCount{0};
    std::atomic<uint32_t> s_debugGsPackedVertexCount{0};
    std::atomic<uint32_t> s_debugGsVertexKickCount{0};
    std::atomic<uint32_t> s_debugCopyRegCount{0};
    std::atomic<uint32_t> s_debugTexaWriteCount{0};
    std::atomic<uint32_t> s_debugCvFontUploadCount{0};
    std::atomic<uint32_t> s_debugLocalCopyCount{0};
}

using namespace GSInternal;

GS::GS()
{
    using namespace GSMem;

    InitLookupTables();

    for (usz i = 0; i < 0x40; ++i)
    {
        switch (i)
        {
        case GS_PSM_CT32:
            m_read_vram_funcs[i] = ReadCT32;
            m_write_vram_funcs[i] = WriteCT32;
            break;
        case GS_PSM_CT24:
            m_read_vram_funcs[i] = ReadCT24;
            m_write_vram_funcs[i] = WriteCT24;
            break;
        case GS_PSM_CT16:
            m_read_vram_funcs[i] = ReadCT16;
            m_write_vram_funcs[i] = WriteCT16;
            break;
        case GS_PSM_CT16S:
            m_read_vram_funcs[i] = ReadCT16S;
            m_write_vram_funcs[i] = WriteCT16S;
            break;
        case GS_PSM_T8:
            m_read_vram_funcs[i] = ReadP8;
            m_write_vram_funcs[i] = WriteP8;
            break;
        case GS_PSM_T8H:
            m_read_vram_funcs[i] = ReadP8H;
            m_write_vram_funcs[i] = WriteP8H;
            break;
        case GS_PSM_T4:
            m_read_vram_funcs[i] = ReadP4;
            m_write_vram_funcs[i] = WriteP4;
            break;
        case GS_PSM_T4HH:
            m_read_vram_funcs[i] = ReadP4HH;
            m_write_vram_funcs[i] = WriteP4HH;
            break;
        case GS_PSM_T4HL:
            m_read_vram_funcs[i] = ReadP4HL;
            m_write_vram_funcs[i] = WriteP4HL;
            break;
        case GS_PSM_Z32:
            m_read_vram_funcs[i] = ReadZ32;
            m_write_vram_funcs[i] = WriteZ32;
            break;
        case GS_PSM_Z24:
            m_read_vram_funcs[i] = ReadZ24;
            m_write_vram_funcs[i] = WriteZ24;
            break;
        case GS_PSM_Z16:
            m_read_vram_funcs[i] = ReadZ16;
            m_write_vram_funcs[i] = WriteZ16;
            break;
        case GS_PSM_Z16S:
            m_read_vram_funcs[i] = ReadZ16S;
            m_write_vram_funcs[i] = WriteZ16S;
            break;
        default:
            m_read_vram_funcs[i] = ReadNull;
            m_write_vram_funcs[i] = WriteNull;
            break;
        }
    }

    reset();
}

void GS::init(uint8_t *vram, uint32_t vramSize, GSRegisters *privRegs)
{
    m_vram = vram;
    m_vramSize = vramSize;
    m_privRegs = privRegs;
    // Resident-VRAM payload source (resident-VRAM-as-second-payload-source
    // task, 2026-08-03): one-shot bind so src/ir/rrv_ir_resident_extractor.cpp
    // can read live GS VRAM at draw time for texture/CLUT identities the
    // upload-driven texture-payload sidecar never observes an IrDiskUpload
    // for (content resident since this GS's initial snapshot, or moved there
    // by a local->local transfer). Single line, no other call site touched.
#if !defined(PS2X_RRV_FIELD_ONLY)
    rrv::ir::hookBindVram(m_vram, m_vramSize);
#endif
    reset();
}

void GS::reset()
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    // GSContext (via GSTex0Reg::clutCache, RRV_GS_CLUT_CACHE) now holds a
    // non-trivial shared_ptr member, so a raw memset would skip its destructor
    // and leak/UB rather than releasing the referenced buffer. Reassign
    // default-constructed contexts instead; behaviourally identical to the
    // old zero-fill for every POD field.
    for (auto &c : m_ctx)
        c = GSContext{};
    m_clutCbp0 = 0xFFFFFFFFu;
    m_clutCbp1 = 0xFFFFFFFFu;
    m_prim = {};
    m_curR = 0x80;
    m_curG = 0x80;
    m_curB = 0x80;
    m_curA = 0x80;
    m_curQ = 1.0f;
    m_curS = 0.0f;
    m_curT = 0.0f;
    m_curU = 0;
    m_curV = 0;
    m_curFog = 0;
    m_prmodecont = true;
    m_pabe = false;
    m_texa = {0u, false, 0u};
    m_texclut = {0u, 0u, 0u};
    m_fogcol = 0u;
    m_bitbltbuf = {};
    m_trxpos = {};
    m_trxreg = {};
    m_trxdir = 3;
    std::memset(m_pathPendingImageBytes, 0, sizeof(m_pathPendingImageBytes));
    m_vtxCount = 0;
    m_vtxIndex = 0;
    m_localToHostBuffer.clear();
    m_localToHostReadPos = 0;
    m_preferredDisplaySourceFrame = {};
    m_preferredDisplayDestFbp = 0;
    m_hasPreferredDisplaySource = false;
    m_hostPresentationFrame.clear();
    m_hostPresentationWidth = 0u;
    m_hostPresentationHeight = 0u;
    m_hostPresentationDisplayFbp = 0u;
    m_hostPresentationSourceFbp = 0u;
    m_hostPresentationSourceFrame = {};
    m_hostPresentationSourceValid = false;
    m_hostPresentationUsedPreferred = false;
    m_hasHostPresentationFrame = false;
    m_hasCompletedFrameClearPresentation = false;
    m_frameClearReuseLatchCount = 0u;
    m_frameClearCaptureNonBlack = 0u;
    m_frameClearCapturePmode = 0u;
    m_frameClearCaptureSmode2 = 0u;
    m_frameClearCaptureDisplay1 = 0u;
    m_frameClearCaptureDisplay2 = 0u;
    m_processingGifPacket = false;
    m_currentGifPacketPath = GifPathId::Path1;

    for (int i = 0; i < 2; ++i)
    {
        m_ctx[i].frame.fbw = 10;
        m_ctx[i].scissor = {0, 639, 0, 447};
        m_ctx[i].xyoffset = {0, 0};
    }
}

// GS::activeContext() is now defined inline in the header (hot per-pixel path).

void GS::snapshotVRAM()
{
    std::lock_guard<std::recursive_mutex> stateLock(m_stateMutex);
    if (!m_vram || m_vramSize == 0)
        return;
    std::lock_guard<std::mutex> lock(m_snapshotMutex);
    m_displaySnapshot.resize(m_vramSize);
    std::memcpy(m_displaySnapshot.data(), m_vram, m_vramSize);
}

const uint8_t *GS::lockDisplaySnapshot(uint32_t &outSize)
{
    m_snapshotMutex.lock();
    if (m_displaySnapshot.empty())
    {
        outSize = 0;
        return nullptr;
    }

    outSize = static_cast<uint32_t>(m_displaySnapshot.size());
    return m_displaySnapshot.data();
}

bool GS::getPreferredDisplaySource(GSFrameReg &outSource, uint32_t &outDestFbp) const
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (!m_hasPreferredDisplaySource)
    {
        outSource = {};
        outDestFbp = 0u;
        return false;
    }

    outSource = m_preferredDisplaySourceFrame;
    outDestFbp = m_preferredDisplayDestFbp;
    return true;
}

void GS::unlockDisplaySnapshot()
{
    m_snapshotMutex.unlock();
}

uint32_t GS::getLastDisplayBaseBytes() const
{
    return m_lastDisplayBaseBytes;
}

void GS::refreshDisplaySnapshot()
{
    snapshotVRAM();
}

namespace
{
    // RRV_PRESENT_CONVERT_AUDIT (default OFF) — bounded architecture audit of
    // GS::copyFrameToHostRgbaUnlocked() call volume, opened at
    // docs/HANDOFF_RENDERER_THROUGHPUT.md §4b.1 ("~78-81 calls per present, not
    // one"). Purely observational: every value it computes is read-only (hashes
    // of already-produced data) and nothing it does is ever read back by the
    // renderer, so enabling it cannot change presented output, only stderr
    // volume. Overhead when disabled is one relaxed-ish bool check per call
    // site (a function-local static after first use) plus a virtually-free
    // branch — no allocation, no hashing, no VRAM read.
    //
    // Emits three line kinds to stderr:
    //   [audit:call]    one per copyFrameToHostRgbaUnlocked call: site tag,
    //                   present-epoch index, guest VSync tick, source fbp/w/h,
    //                   a hash of the source VRAM footprint, a hash of the
    //                   produced RGBA output, and whether the output hash
    //                   repeats the immediately-preceding call's output.
    //   [audit:frame]   printed when the guest VSync tick advances: how many
    //                   conversions happened during the tick that just ended.
    //   [audit:present] printed once per latchHostPresentationFrame() entry
    //                   (i.e. once per host present): total conversions since
    //                   the previous present, broken down by call site, plus
    //                   the longest run of consecutive duplicate outputs and
    //                   the cumulative duplicate count for the whole process.
    //
    // Call-site tags (see the 5 copyFrameToHostRgbaUnlocked() call sites in
    // this file): "frameclear_target" and "frameclear_newest_fb" are inside
    // captureFrameClearPresentationUnlocked (driven by GS_REG_FRAME_2 writes
    // during PATH3 processing); "latch_preferred", "latch_main" and
    // "latch_candidate" are inside latchHostPresentationFrame's fallback
    // display-source resolution (only exercised when the frame-clear fast
    // path is unavailable — signature mismatch, pinned-black, or the 12-latch
    // reuse cap).
    //
    // Source-footprint hash caveat, stated honestly: PS2 GS render targets are
    // block-swizzled, so the exact byte *order* GSMem::ReadCT32/CT24/CT16/
    // CT16S touches is not a contiguous range. The hash below covers the same
    // bounding rectangle [fbp*8192, fbp*8192 + fbw*64*bytesPerPixel*height)
    // that copyFrameToHostRgbaUnlocked's own non-swizzled fallback branch
    // already uses as its linear addressing model — a conservative superset
    // of the true swizzled footprint, not the exact byte order. Good enough to
    // answer "did the source region change since the last call," not offered
    // as a bit-exact swizzle trace.
    struct PresentConvertAudit
    {
        bool enabled = false;
        uint64_t presentIndex = 0;
        uint64_t lastTick = ~0ull;
        bool haveLastTick = false;
        uint64_t tickEpochCalls = 0;
        uint64_t presentEpochCalls = 0;
        uint64_t presentEpochBySite[5] = {0, 0, 0, 0, 0};
        uint64_t lastOutHash = 0;
        bool haveLastOutHash = false;
        uint64_t dupRun = 0;
        uint64_t dupRunMax = 0;
        uint64_t dupTotal = 0;
        uint64_t totalCalls = 0;

        static PresentConvertAudit &instance()
        {
            static PresentConvertAudit s = [] {
                PresentConvertAudit init;
                const char *v = std::getenv("RRV_PRESENT_CONVERT_AUDIT");
                init.enabled = v && v[0] && v[0] != '0';
                return init;
            }();
            return s;
        }
    };

    constexpr const char *kAuditSiteNames[5] = {
        "frameclear_target",
        "frameclear_newest_fb",
        "latch_preferred",
        "latch_main",
        "latch_candidate",
    };

    inline uint64_t auditFnv1a(const void *data, size_t len)
    {
        const uint8_t *p = static_cast<const uint8_t *>(data);
        uint64_t h = 1469598103934665603ull;
        for (size_t i = 0; i < len; ++i)
        {
            h ^= p[i];
            h *= 1099511628211ull;
        }
        return h;
    }

    // Called right after every successful copyFrameToHostRgbaUnlocked(). Takes
    // vram/vramSize explicitly rather than being a GS member so no header
    // change is needed for an audit-only hatch.
    void auditPresentConvertCall(int siteId, const uint8_t *vram, uint32_t vramSize,
                                 const GSFrameReg &frame, uint32_t width, uint32_t height,
                                 const std::vector<uint8_t> &outPixels)
    {
        PresentConvertAudit &a = PresentConvertAudit::instance();
        if (!a.enabled)
            return;

        const uint64_t tick = ps2_syscalls::GetCurrentVSyncTick();
        if (!a.haveLastTick || tick != a.lastTick)
        {
            if (a.haveLastTick && a.tickEpochCalls > 0)
            {
                std::fprintf(stderr, "[audit:frame] tick=%llu calls=%llu\n",
                             static_cast<unsigned long long>(a.lastTick),
                             static_cast<unsigned long long>(a.tickEpochCalls));
            }
            a.lastTick = tick;
            a.haveLastTick = true;
            a.tickEpochCalls = 0;
        }
        ++a.tickEpochCalls;
        ++a.presentEpochCalls;
        ++a.totalCalls;
        const int site = (siteId >= 0 && siteId < 5) ? siteId : 0;
        ++a.presentEpochBySite[site];

        const uint32_t baseBytes = frame.fbp * 8192u;
        const uint32_t fbwBlocks = frame.fbw ? frame.fbw : (kHostFrameWidth / 64u);
        const uint32_t bytesPerPixel =
            (frame.psm == GS_PSM_CT16 || frame.psm == GS_PSM_CT16S) ? 2u : 4u;
        const uint32_t strideBytes = fbwBlocks * 64u * bytesPerPixel;
        uint64_t srcHash = 0;
        if (vram && vramSize)
        {
            const uint64_t want = static_cast<uint64_t>(strideBytes) * height;
            const uint64_t avail = (baseBytes < vramSize) ? (vramSize - baseBytes) : 0ull;
            const uint64_t span = std::min(want, avail);
            srcHash = auditFnv1a(vram + baseBytes, static_cast<size_t>(span));
        }
        const uint64_t outHash = auditFnv1a(outPixels.data(), outPixels.size());
        const bool dup = a.haveLastOutHash && outHash == a.lastOutHash;
        if (dup)
        {
            ++a.dupRun;
            ++a.dupTotal;
        }
        else
        {
            a.dupRun = 1;
        }
        a.dupRunMax = std::max(a.dupRunMax, a.dupRun);
        a.lastOutHash = outHash;
        a.haveLastOutHash = true;

        std::fprintf(stderr,
                     "[audit:call] site=%s present=%llu tick=%llu fbp=%u w=%u h=%u "
                     "srcHash=%016llx outHash=%016llx dupPrev=%d\n",
                     kAuditSiteNames[site], static_cast<unsigned long long>(a.presentIndex),
                     static_cast<unsigned long long>(tick), frame.fbp, width, height,
                     static_cast<unsigned long long>(srcHash),
                     static_cast<unsigned long long>(outHash), dup ? 1 : 0);
    }

    // Called at the top of latchHostPresentationFrame(), before any state is
    // read, so it closes out the previous present's epoch (the conversions
    // that happened while building the frame this present is about to show).
    void auditPresentEpochFlush()
    {
        PresentConvertAudit &a = PresentConvertAudit::instance();
        if (!a.enabled)
            return;
        std::fprintf(stderr,
                     "[audit:present] present=%llu calls=%llu frameclear_target=%llu "
                     "frameclear_newest_fb=%llu latch_preferred=%llu latch_main=%llu "
                     "latch_candidate=%llu dupRunMax=%llu dupTotalCumulative=%llu\n",
                     static_cast<unsigned long long>(a.presentIndex),
                     static_cast<unsigned long long>(a.presentEpochCalls),
                     static_cast<unsigned long long>(a.presentEpochBySite[0]),
                     static_cast<unsigned long long>(a.presentEpochBySite[1]),
                     static_cast<unsigned long long>(a.presentEpochBySite[2]),
                     static_cast<unsigned long long>(a.presentEpochBySite[3]),
                     static_cast<unsigned long long>(a.presentEpochBySite[4]),
                     static_cast<unsigned long long>(a.dupRunMax),
                     static_cast<unsigned long long>(a.dupTotal));
        ++a.presentIndex;
        a.presentEpochCalls = 0;
        for (uint64_t &c : a.presentEpochBySite)
            c = 0;
        a.dupRunMax = 0;
    }
} // namespace

bool GS::copyFrameToHostRgbaUnlocked(const GSFrameReg &frame,
                                     uint32_t width,
                                     uint32_t height,
                                     std::vector<uint8_t> &outPixels,
                                     bool preserveAlpha,
                                     bool useLocalMemoryLayout,
                                     bool frameBaseIsPages,
                                     uint32_t sourceOriginX,
                                     uint32_t sourceOriginY) const
{
    if (!m_vram || m_vramSize == 0u)
    {
        return false;
    }

    outPixels.assign(kHostFrameWidth * kHostFrameHeight * 4u, 0u);

    const uint32_t baseBytes = frameBaseIsPages ? (frame.fbp * 8192u) : (frame.fbp * 256u);
    const uint32_t basePtr = frameBaseIsPages ? GSInternal::framePageBaseToBlock(frame.fbp) : frame.fbp;
    const uint32_t fbwBlocks = frame.fbw ? frame.fbw : (kHostFrameWidth / 64u);
    const uint32_t bytesPerPixel = (frame.psm == GS_PSM_CT16 || frame.psm == GS_PSM_CT16S) ? 2u : 4u;
    const uint32_t strideBytes = fbwBlocks * 64u * bytesPerPixel;

    if (frame.psm == GS_PSM_CT32 || frame.psm == GS_PSM_CT24)
    {
        const uint32_t srcPixelBytes = (frame.psm == GS_PSM_CT24) ? 3u : 4u;
        if (useLocalMemoryLayout)
        {
            // RRV_FAST_FRAME_CONVERT (default OFF): this branch already knows
            // frame.psm is CT32 or CT24, but the stock loop still calls the
            // generic ReadVram() dispatcher, which re-derives that from
            // frame.psm on every pixel via a std::array<std::function<...>>
            // indirection (GS::ReadVram -> m_read_vram_funcs[psm & 0x3F]).
            // Measured (Phase 1, `sample` on RRV_TILE_RASTER_BYPASS=1 replay,
            // gt_A2_girl/gt_A3_demorace, 3+ runs): that std::function thunk
            // alone is ~14-18% of this present-conversion function's total
            // samples -- on top of GSMem::ReadCT32/CT24's own address-calc +
            // memcpy body, counted separately. Calling the concrete free
            // function directly removes exactly that dispatch tax and
            // nothing else: same GSMem swizzle math, same byte output. The
            // isCt24 branch and preserveAlpha/format decisions are hoisted
            // out of the pixel loop into which loop body runs, instead of
            // being re-evaluated per pixel.
            //
            // Generic fallback: any PSM this function doesn't specialize
            // still falls through the untouched ReadVram()-dispatch loop
            // below, unconditionally (this function only ever handles CT32/
            // CT24/CT16/CT16S to begin with -- see the return false at the
            // end for everything else).
            //
            // TEST COVERAGE, stated honestly: every Ridge Racer V reference
            // stream in local/gt/ presents CT32, so only the CT32 fast path is
            // exercised by the byte-identical gates. CT24, CT16 and CT16S are
            // *mechanically* mapped -- GS::init() builds m_read_vram_funcs as a
            // 1:1 PSM->function table (CT24->ReadCT24, CT16->ReadCT16,
            // CT16S->ReadCT16S), so calling the concrete function directly is
            // the same call the dispatcher would have made -- but that
            // equivalence is established by construction, not by measurement.
            // Anything that changes that table must revisit these three paths.
            static const bool s_fastConvert = [] {
                const char *v = std::getenv("RRV_FAST_FRAME_CONVERT");
                return v && v[0] && v[0] != '0';
            }();
            const bool isCt24 = (frame.psm == GS_PSM_CT24);
            const bool wantAlpha = preserveAlpha && !isCt24;

            auto storePixel = [&](uint8_t *dstRow, uint32_t x, u32 c) {
                dstRow[x * 4u + 0u] = static_cast<uint8_t>(c & 0xFFu);
                dstRow[x * 4u + 1u] = static_cast<uint8_t>((c >> 8) & 0xFFu);
                dstRow[x * 4u + 2u] = static_cast<uint8_t>((c >> 16) & 0xFFu);
                dstRow[x * 4u + 3u] = wantAlpha ? static_cast<uint8_t>((c >> 24) & 0xFFu) : 0xFFu;
            };

            if (s_fastConvert && isCt24)
            {
                for (uint32_t y = 0; y < height; ++y)
                {
                    uint8_t *dstRow = outPixels.data() + (y * kHostFrameWidth * 4u);
                    const uint32_t srcY = sourceOriginY + y;
                    for (uint32_t x = 0; x < width; ++x)
                    {
                        storePixel(dstRow, x, GSMem::ReadCT24(m_vram, basePtr, fbwBlocks, sourceOriginX + x, srcY));
                    }
                }
                return true;
            }
            if (s_fastConvert)
            {
                for (uint32_t y = 0; y < height; ++y)
                {
                    uint8_t *dstRow = outPixels.data() + (y * kHostFrameWidth * 4u);
                    const uint32_t srcY = sourceOriginY + y;
                    for (uint32_t x = 0; x < width; ++x)
                    {
                        storePixel(dstRow, x, GSMem::ReadCT32(m_vram, basePtr, fbwBlocks, sourceOriginX + x, srcY));
                    }
                }
                return true;
            }

            for (uint32_t y = 0; y < height; ++y)
            {
                uint8_t *dstRow = outPixels.data() + (y * kHostFrameWidth * 4u);
                const uint32_t srcY = sourceOriginY + y;
                for (uint32_t x = 0; x < width; ++x)
                {
                    const uint32_t srcX = sourceOriginX + x;
                    storePixel(dstRow, x, ReadVram(frame.psm, basePtr, fbwBlocks, srcX, srcY));
                }
            }
            return true;
        }

        for (uint32_t y = 0; y < height; ++y)
        {
            const uint32_t dstOff = y * kHostFrameWidth * 4u;
            uint8_t *dstRow = outPixels.data() + dstOff;
            for (uint32_t x = 0; x < width; ++x)
            {
                const uint32_t srcX = sourceOriginX + x;
                const uint32_t srcY = sourceOriginY + y;
                const uint32_t srcOff = baseBytes + (srcY * strideBytes) + (srcX * srcPixelBytes);
                if (srcOff + srcPixelBytes > m_vramSize)
                {
                    return false;
                }

                dstRow[x * 4u + 0u] = m_vram[srcOff + 0u];
                dstRow[x * 4u + 1u] = m_vram[srcOff + 1u];
                dstRow[x * 4u + 2u] = m_vram[srcOff + 2u];
                dstRow[x * 4u + 3u] =
                    (preserveAlpha && frame.psm != GS_PSM_CT24) ? m_vram[srcOff + 3u] : 255u;
            }
        }
        return true;
    }

    if (frame.psm == GS_PSM_CT16 || frame.psm == GS_PSM_CT16S)
    {
        if (useLocalMemoryLayout)
        {
            // RRV_FAST_FRAME_CONVERT (default OFF): same rationale as the
            // CT32/CT24 branch above -- call the concrete GSMem::ReadCT16 /
            // ReadCT16S directly instead of through the per-pixel ReadVram()
            // std::function dispatch. Generic ReadVram() fallback unchanged.
            static const bool s_fastConvert = [] {
                const char *v = std::getenv("RRV_FAST_FRAME_CONVERT");
                return v && v[0] && v[0] != '0';
            }();
            const bool isCt16S = (frame.psm == GS_PSM_CT16S);

            auto storePixel = [&](uint8_t *dst, uint32_t x, u16 c) {
                const uint32_t r = c & 31u;
                const uint32_t g = (c >> 5) & 31u;
                const uint32_t b = (c >> 10) & 31u;
                dst[x * 4u + 0u] = static_cast<uint8_t>((r << 3) | (r >> 2));
                dst[x * 4u + 1u] = static_cast<uint8_t>((g << 3) | (g >> 2));
                dst[x * 4u + 2u] = static_cast<uint8_t>((b << 3) | (b >> 2));
                dst[x * 4u + 3u] = preserveAlpha ? ((c & 0x8000u) ? 0x80u : 0x00u) : 255u;
            };

            if (s_fastConvert && isCt16S)
            {
                for (uint32_t y = 0; y < height; ++y)
                {
                    uint8_t *dst = outPixels.data() + (y * kHostFrameWidth * 4u);
                    const uint32_t srcY = sourceOriginY + y;
                    for (uint32_t x = 0; x < width; ++x)
                    {
                        storePixel(dst, x, static_cast<u16>(GSMem::ReadCT16S(m_vram, basePtr, fbwBlocks, sourceOriginX + x, srcY)));
                    }
                }
                return true;
            }
            if (s_fastConvert)
            {
                for (uint32_t y = 0; y < height; ++y)
                {
                    uint8_t *dst = outPixels.data() + (y * kHostFrameWidth * 4u);
                    const uint32_t srcY = sourceOriginY + y;
                    for (uint32_t x = 0; x < width; ++x)
                    {
                        storePixel(dst, x, static_cast<u16>(GSMem::ReadCT16(m_vram, basePtr, fbwBlocks, sourceOriginX + x, srcY)));
                    }
                }
                return true;
            }

            for (uint32_t y = 0; y < height; ++y)
            {
                const uint32_t dstOff = y * kHostFrameWidth * 4u;
                uint8_t *dst = outPixels.data() + dstOff;
                const uint32_t srcY = sourceOriginY + y;
                for (uint32_t x = 0; x < width; ++x)
                {
                    const uint32_t srcX = sourceOriginX + x;
                    storePixel(dst, x, ReadVram(frame.psm, basePtr, fbwBlocks, srcX, srcY));
                }
            }
            return true;
        }

        for (uint32_t y = 0; y < height; ++y)
        {
            const uint32_t dstOff = y * kHostFrameWidth * 4u;
            uint8_t *dst = outPixels.data() + dstOff;
            for (uint32_t x = 0; x < width; ++x)
            {
                const uint32_t srcX = sourceOriginX + x;
                const uint32_t srcY = sourceOriginY + y;
                const uint32_t srcOff = baseBytes + (srcY * strideBytes) + (srcX * 2u);
                if (srcOff + sizeof(uint16_t) > m_vramSize)
                {
                    return false;
                }

                uint16_t pixel = 0u;
                std::memcpy(&pixel, m_vram + srcOff, sizeof(pixel));
                const uint32_t r = pixel & 31u;
                const uint32_t g = (pixel >> 5) & 31u;
                const uint32_t b = (pixel >> 10) & 31u;
                dst[x * 4u + 0u] = static_cast<uint8_t>((r << 3) | (r >> 2));
                dst[x * 4u + 1u] = static_cast<uint8_t>((g << 3) | (g >> 2));
                dst[x * 4u + 2u] = static_cast<uint8_t>((b << 3) | (b >> 2));
                dst[x * 4u + 3u] = preserveAlpha ? ((pixel & 0x8000u) ? 0x80u : 0x00u) : 255u;
            }
        }
        return true;
    }

    return false;
}

// Milestone C0 fix (was: B4 finding "1-frame present lag"). Tile-binning
// only flushed its deferred command buffer from two places: (1) the next
// frame's first processGIFPacket call detecting a VSync-tick change, and
// (2) VRAM read-back/upload/transfer paths. latchHostPresentationFrame (the
// present-thread read of "the current frame") was NOT one of them, so a
// live present always showed the PREVIOUS frame's fully-flushed content
// while the just-finished frame's primitives sat in the command buffer
// until the NEXT frame's first packet arrived — one full frame late, every
// frame, forever (not just at start-up/shutdown; a finite replay recording
// merely made it visible as a hash mismatch because there is no "next
// frame" after the last present).
//
// Fix: flush here, at the top of latchHostPresentationFrame, before any
// state is read. Threading: this runs on the PRESENT thread, async to the
// GAME thread that owns processGIFPacket/captureTile/the command buffer —
// but both this function and processGIFPacket already take m_stateMutex
// (std::recursive_mutex) for their entire body (see the lock_guard right
// below and at processGIFPacket's entry). So the two threads can never be
// inside GS state at the same time; flushTiles() here runs with the exact
// same exclusivity guarantee the game thread's own tick-boundary flush
// already relies on. No new lock, no new race: this is "proper
// synchronization to flush from the latch," not a new flush point that
// needs inventing. (The alternative considered — flushing from a
// game-thread frame-completion signal, e.g. at DISPFB flip — would require
// a NEW cross-thread signal/wait the present thread doesn't otherwise need,
// since it already serializes via m_stateMutex; that would add complexity
// without removing any race, so it was rejected in favor of this simpler,
// equally-correct fix.)
void GS::flushPendingTiles()
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (GSRasterizer::tileEnabled())
    {
        m_rasterizer.flushTiles(this);
    }
    // Milestone C1: in RRV_IR_RASTER mode this drains the pending IR draw stream
    // through the shadow rasterizer (self-guarded no-op otherwise). Placed at the
    // same choke point flushTiles uses so the present latch reads a fully-rendered
    // frame; ordering is preserved because it runs before any VRAM read-back.
#if !defined(PS2X_RRV_FIELD_ONLY)
    m_rasterizer.flushIR(this);
#endif
}

void GS::presentLatchDrain()
{
    if (GSRasterizer::asyncRasterEnabled() && GSRasterizer::tileEnabled()
#if !defined(PS2X_RRV_FIELD_ONLY)
        && !rrv::ir::rrv_ir_raster_enabled()
#endif
        )
    {
        // Async: only wait for the worker so the displayed buffer is complete;
        // leave g_tileCmds (the partial next frame) for the next frame-clear
        // dispatch. flushIR is a no-op here (IR-raster excluded above).
        m_rasterizer.waitForRasterWorker();
#if !defined(PS2X_RRV_FIELD_ONLY)
        m_rasterizer.flushIR(this);
#endif
        return;
    }
    flushPendingTiles();
}

void GS::setPresentAtFrameClearEnabled(bool enabled)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    m_presentAtFrameClearEnabled = enabled;
    if (!enabled)
        m_hasCompletedFrameClearPresentation = false;
}

bool GS::hasCompletedFrameClearPresentation() const
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    return m_presentAtFrameClearEnabled && m_hasCompletedFrameClearPresentation;
}

void GS::recordScanoutDisplayFbpsUnlocked()
{
    // Record the framebuffer bases the display is actually scanning out, from
    // the live PMODE/DISPFB privileged registers. Called every present so the
    // set is populated from the first attract frame (long before the girl
    // scene) and stays current. Only enabled + validly-configured CRTCs count.
    if (!m_privRegs)
        return;
    const GSPmodeState pmode = decodePmode(m_privRegs->pmode);
    auto record = [this](uint32_t fbp) {
        for (uint32_t existing : m_scanoutDisplayFbps)
            if (existing == fbp)
                return;
        // Bound growth defensively; display bases are few in practice.
        if (m_scanoutDisplayFbps.size() >= 16u)
            m_scanoutDisplayFbps.erase(m_scanoutDisplayFbps.begin());
        m_scanoutDisplayFbps.push_back(fbp);
    };
    if (pmode.enableCrt1)
    {
        const GSFrameReg df1 = decodeDisplayFrame(m_privRegs->dispfb1);
        if (hasDisplaySetup(m_privRegs->display1, df1))
            record(df1.fbp);
    }
    if (pmode.enableCrt2)
    {
        const GSFrameReg df2 = decodeDisplayFrame(m_privRegs->dispfb2);
        if (hasDisplaySetup(m_privRegs->display2, df2))
            record(df2.fbp);
    }
}

bool GS::isScanoutDisplayFbp(uint32_t fbp) const
{
    // Empty set => not yet observed a scanout (early boot): fall back to
    // permissive so we never suppress a legitimate present before the display
    // has been configured.
    if (m_scanoutDisplayFbps.empty())
        return true;
    for (uint32_t existing : m_scanoutDisplayFbps)
        if (existing == fbp)
            return true;
    return false;
}

void GS::captureFrameClearPresentationUnlocked(const GSFrameReg &targetFrame,
                                               const GSFrameReg &oldFrame)
{
    // RRV's PATH3 FRAME_2 switch is immediately followed by the clear that starts
    // the next accumulation. Drain deferred work and snapshot the target before
    // applying FRAME_2, while that VRAM page still contains the completed frame.
    {
        static const bool s_fcDiag = std::getenv("RRV_ASYNC_RASTER_DIAG") != nullptr;
        const auto t0 = s_fcDiag ? std::chrono::steady_clock::now()
                                 : std::chrono::steady_clock::time_point{};
        if (GSRasterizer::asyncRasterEnabled() && GSRasterizer::tileEnabled()
#if !defined(PS2X_RRV_FIELD_ONLY)
            && !rrv::ir::rrv_ir_raster_enabled()
#endif
            )
        {
            // Async raster: this is the gameThread's real raster hotspot (~600-800
            // ms/s, measured). The just-accumulated frame renders on the worker into
            // the OLD fbp, while the snapshot below reads targetFrame = NEW fbp — a
            // DIFFERENT buffer already complete (content from 2 flips ago). Wait for
            // the worker to go idle first so every buffer is stable for the snapshot
            // (≈0 in steady state: the previous frame's raster overlapped this frame's
            // build), then hand this frame off and return to building the next without
            // blocking on raster. oldFbp != newFbp is guaranteed by the flip guard, so
            // the worker's writes never touch the snapshotted buffer.
            m_rasterizer.waitForRasterWorker();
            m_rasterizer.dispatchAsyncFrame(this);
        }
        else
        {
            flushPendingTiles();
        }
        if (s_fcDiag)
        {
            static std::chrono::steady_clock::time_point s_win = std::chrono::steady_clock::now();
            static uint64_t s_ns = 0, s_calls = 0;
            s_ns += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0).count());
            ++s_calls;
            const auto now = std::chrono::steady_clock::now();
            if (now - s_win >= std::chrono::seconds(1))
            {
                std::fprintf(stderr, "[gt:frameclear] calls=%llu gameThread_ms=%.1f/s\n",
                             static_cast<unsigned long long>(s_calls),
                             static_cast<double>(s_ns) / 1.0e6);
                s_win = now; s_ns = 0; s_calls = 0;
            }
        }
    }

    if (!m_privRegs || !m_vram || m_vramSize == 0u)
        return;

    const GSPmodeState pmode = decodePmode(m_privRegs->pmode);
    const GSFrameReg displayFrame1 = decodeDisplayFrame(m_privRegs->dispfb1);
    const GSFrameReg displayFrame2 = decodeDisplayFrame(m_privRegs->dispfb2);
    const bool validCrt1 = pmode.enableCrt1 && hasDisplaySetup(m_privRegs->display1, displayFrame1);
    const bool validCrt2 = pmode.enableCrt2 && hasDisplaySetup(m_privRegs->display2, displayFrame2);

    uint32_t width = 0u;
    uint32_t height = 0u;
    if (validCrt1)
        decodeDisplaySize(m_privRegs->display1, width, height, m_privRegs->smode2);
    else if (validCrt2)
        decodeDisplaySize(m_privRegs->display2, width, height, m_privRegs->smode2);
    else
        return;

    // RRV_FRAMECLEAR_SRC — which buffer this capture snapshots. Values:
    //   "target" (default, legacy) the buffer being flipped TO
    //   "old"                      the buffer just flipped away FROM
    //   "dispfb"                   the buffer the CRTC is actually scanning out
    //
    // Measured 2026-07-24 on /tmp/girl640.gsr — neither fixed side is right in
    // both scenes, so "target" vs "old" cannot be settled by a constant:
    //   city flyover (presents 120/300): target = full content (mean 107.4/98.3),
    //                                    old    = BLACK (mean 0.5/1.4)
    //   girl cinematic (present 620):    target = stale pre-bloom sky+city
    //                                             (no girl, mean 96.6),
    //                                    old    = the finished frame
    //                                             (girl present, mean 14.3)
    // RRV_FRAMECLEAR_NEWEST is the same heuristic in mirror image (swap to old
    // only when target reads black); it does not fire here because the stale
    // girl-scene target is a bright city image, not black.
    //
    // "dispfb" is the non-heuristic option: present what the display is showing,
    // which is by definition a completed frame in both cadences. Sync-only for
    // "old": in async mode the raster worker is still writing that buffer (see
    // the dispatch comment above), so it is not safe to read.
    const bool asyncActiveForSource = GSRasterizer::asyncRasterEnabled() &&
                                      GSRasterizer::tileEnabled()
#if !defined(PS2X_RRV_FIELD_ONLY)
                                      && !rrv::ir::rrv_ir_raster_enabled()
#endif
                                      ;
    enum class FrameClearSrc { Target, Old, Dispfb };
    static const FrameClearSrc s_frameClearSrc = [] {
        const char *v = std::getenv("RRV_FRAMECLEAR_SRC");
        if (v && v[0])
        {
            if (std::strcmp(v, "old") == 0) return FrameClearSrc::Old;
            if (std::strcmp(v, "dispfb") == 0) return FrameClearSrc::Dispfb;
        }
        return FrameClearSrc::Target;
    }();

    GSFrameReg sourceFrame = targetFrame;
    if (s_frameClearSrc == FrameClearSrc::Old && !asyncActiveForSource &&
        oldFrame.fbp != targetFrame.fbp)
    {
        sourceFrame = oldFrame;
    }
    else if (s_frameClearSrc == FrameClearSrc::Dispfb)
    {
        // Take the scanout base from the enabled CRTC, but keep the render
        // target's fbw/psm: DISPFB's psm field is the display format and its
        // fbw is in the same page units, yet the frame reg is what the raster
        // path wrote through, so its geometry is the safe one to decode with.
        const GSFrameReg &disp = validCrt2 ? displayFrame2
                               : validCrt1 ? displayFrame1
                                           : targetFrame;
        sourceFrame.fbp = disp.fbp;
    }
    const bool preferOld = (sourceFrame.fbp == oldFrame.fbp) &&
                           (oldFrame.fbp != targetFrame.fbp);

    std::vector<uint8_t> scratch;
    if (!copyFrameToHostRgbaUnlocked(sourceFrame, width, height, scratch,
                                     false, true, true, 0u, 0u))
    {
        return;
    }
    auditPresentConvertCall(/*site=*/0, m_vram, m_vramSize, sourceFrame, width, height, scratch);

    // RRV_FRAMECLEAR_NEWEST (default OFF) — girl-scene flicker fix. The snapshot
    // above reads targetFrame = the NEW fbp, i.e. the buffer about to be drawn.
    // In the girl scene (~18% measured) that buffer is already cleared to black,
    // so the present-latch field-fix falls through to the DISPFB-following path,
    // which alternates between the two double-buffers — and at a camera cut the
    // two buffers hold different shots, producing the ~100ms old/new/old flicker.
    // The just-completed OLD fbp (the buffer flushPendingTiles just rendered) holds
    // the newest content. When the NEW fbp is black and the OLD fbp is not, snapshot
    // the OLD fbp instead, so the present shows the newest frame with no fallthrough
    // and no stale-buffer alternation. Sync-only: in async mode the OLD fbp is being
    // rendered by the worker at this instant, so it is not yet complete to snapshot.
    GSFrameReg snapshotFrame = sourceFrame;
    static const bool s_frameClearNewest = [] {
        const char *v = std::getenv("RRV_FRAMECLEAR_NEWEST");
        return v && v[0] && v[0] != '0';
    }();
    const bool asyncActive = asyncActiveForSource;
    if (s_frameClearNewest && !preferOld && !asyncActive &&
        oldFrame.fbp != targetFrame.fbp)
    {
        const uint64_t coveredPx = static_cast<uint64_t>(width) * height;
        const bool targetBlack =
            countNonBlackPixels(scratch, width, height) * 200u < coveredPx;
        if (targetBlack)
        {
            std::vector<uint8_t> alt;
            if (copyFrameToHostRgbaUnlocked(oldFrame, width, height, alt,
                                            false, true, true, 0u, 0u))
            {
                auditPresentConvertCall(/*site=*/1, m_vram, m_vramSize, oldFrame, width, height, alt);
                if (countNonBlackPixels(alt, width, height) * 200u >= coveredPx)
                {
                    scratch.swap(alt);
                    snapshotFrame = oldFrame;
                }
            }
        }
    }

    const GSSmode2State smode2 = decodeSMode2(m_privRegs->smode2);
    if (smode2.interlaced && !smode2.frameMode)
    {
        const bool oddField = (ps2_syscalls::GetCurrentVSyncTick() & 1ull) != 0ull;
        applyFieldPresentation(scratch, width, height, oddField);
    }
    normalizePresentationAlpha(scratch, width, height);

    m_hostPresentationFrame.swap(scratch);
    m_hostPresentationWidth = width;
    m_hostPresentationHeight = height;
    m_hostPresentationDisplayFbp = snapshotFrame.fbp;
    m_hostPresentationSourceFbp = snapshotFrame.fbp;
    m_hostPresentationSourceFrame = snapshotFrame;
    m_hostPresentationSourceValid = true;
    m_hostPresentationUsedPreferred = false;
    m_hasHostPresentationFrame = true;
    m_hasCompletedFrameClearPresentation = true;
    // RRV_PRESENT_FIELD_FIX: remember whether this capture actually has content.
    // In the Release 60fps field-rendering cadence the FRAME_2 flip captures the
    // buffer that is about to be DRAWN (still black), not the just-completed one,
    // so the pinned frame is black; the latch uses this to fall through instead.
    m_frameClearCaptureNonBlack = countNonBlackPixels(m_hostPresentationFrame, width, height);
    m_frameClearReuseLatchCount = 0u;
    m_frameClearCapturePmode = m_privRegs->pmode;
    m_frameClearCaptureSmode2 = m_privRegs->smode2;
    m_frameClearCaptureDisplay1 = m_privRegs->display1;
    m_frameClearCaptureDisplay2 = m_privRegs->display2;

    static int s_diag = -1;
    if (s_diag < 0)
    {
        const char *value = std::getenv("RRV_PRESENT_AT_FRAME_CLEAR_DIAG");
        s_diag = (value && value[0] && value[0] != '0') ? 1 : 0;
    }
    if (s_diag)
    {
        const uint32_t nonBlack = countNonBlackPixels(m_hostPresentationFrame, width, height);
        const uint32_t covered = width * height;
        fprintf(stderr,
                "[present:frame-clear] path=3 oldFrame2Fbp=%u newFrame2Fbp=%u sourceFbp=%u %ux%u nonBlackPx=%u (%.1f%%)\n",
                oldFrame.fbp, targetFrame.fbp, m_hostPresentationSourceFbp, width, height,
                nonBlack, covered ? (100.0 * nonBlack / static_cast<double>(covered)) : 0.0);
    }
}

void GS::latchHostPresentationFrame()
{
    latchHostPresentationFrame(ps2_syscalls::GetCurrentVSyncTick());
}

void GS::latchHostPresentationFrame(uint64_t vsyncTick)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);

    // RRV_PRESENT_CONVERT_AUDIT (default OFF, see the anonymous-namespace block
    // above copyFrameToHostRgbaUnlocked): close out the present epoch that just
    // finished — i.e. report how many VRAM->RGBA conversions happened while
    // building the frame this present call is about to show — before touching
    // any other state. No-op when the hatch is disabled.
    auditPresentEpochFlush();

    // Learn which framebuffer bases the display scans out (runs every present,
    // before the frame-clear fast-path return below, so the set is populated
    // regardless of which present path is taken). Feeds isScanoutDisplayFbp(),
    // which the FRAME_2 capture gate uses to skip offscreen render targets.
    recordScanoutDisplayFbpsUnlocked();

    // RRV_PRESENT_FIELD_FIX (2026-07-12, additive to Codex's frame-clear path):
    // default-ON. In Release the game runs a 60fps FIELD-RENDERING cadence with
    // one present per FRAME_2 fbp flip (fbp alternates 0/70 every present). The
    // frame-clear capture, designed for the full-frame accumulate-then-flip model,
    // then snapshots the buffer being flipped TO (still black this cycle) and pins
    // it, so the reused present is black even though the DISPFB-selected buffer
    // holds real content. Measured on attract_cars.gsr: with frame-clear ON, 350
    // presents (early-boot logo/fades + late transitions) are pinned-black while
    // the DISPFB+candidate path below recovers them; core content (presents
    // 500-1999) is already 0% black and stays byte-identical. Fix: when the pinned
    // frame-clear frame is black, fall through to the DISPFB+candidate path instead
    // of reusing black. Provably non-regressing: only ever converts a black present
    // to whatever DISPFB yields (content or the same black), never the reverse.
    static int s_fieldFix = -1;
    if (s_fieldFix < 0)
    {
        const char *v = std::getenv("RRV_PRESENT_FIELD_FIX");
        s_fieldFix = (!v || (v[0] && v[0] != '0')) ? 1 : 0;
    }
    if (m_presentAtFrameClearEnabled && m_hasCompletedFrameClearPresentation)
    {
        const bool signatureMatches = m_privRegs &&
                                      m_privRegs->pmode == m_frameClearCapturePmode &&
                                      m_privRegs->smode2 == m_frameClearCaptureSmode2 &&
                                      m_privRegs->display1 == m_frameClearCaptureDisplay1 &&
                                      m_privRegs->display2 == m_frameClearCaptureDisplay2;
        const bool pinnedIsBlack = (s_fieldFix != 0) && (m_frameClearCaptureNonBlack == 0u);
        if (!signatureMatches || m_frameClearReuseLatchCount >= 12u || pinnedIsBlack)
        {
            m_hasCompletedFrameClearPresentation = false;
        }
        else
        {
            ++m_frameClearReuseLatchCount;
            const uint64_t currentTick = vsyncTick;
            // Preserve legacy ordering: deferred pixels/IR close before the present
            // hooks, while the already-copied completion pixels remain pinned.
            // (Async: wait-only — see presentLatchDrain.)
            presentLatchDrain();
#if !defined(PS2X_RRV_FIELD_ONLY)
            if (rrv::ir::rrv_ir_enabled())
            {
                rrv::ir::HookPresent pr{};
                pr.pmode = m_privRegs->pmode;   pr.smode2 = m_privRegs->smode2;
                pr.dispfb1 = m_privRegs->dispfb1; pr.display1 = m_privRegs->display1;
                pr.dispfb2 = m_privRegs->dispfb2; pr.display2 = m_privRegs->display2;
                pr.vsyncTick = currentTick;
                pr.hostSourceFbp = m_hostPresentationSourceFrame.fbp;
                pr.hostSourceFbw = m_hostPresentationSourceFrame.fbw;
                pr.hostSourcePsm = m_hostPresentationSourceFrame.psm;
                pr.hostSourceValid = m_hostPresentationSourceValid ? 1u : 0u;
                rrv::ir::hookPresent(pr);
            }
#endif
            if (rrv::gsrecord::rrv_gs_record_enabled())
            {
                rrv::gsrecord::RecordPresent pr{};
                pr.pmode = m_privRegs->pmode;   pr.smode2 = m_privRegs->smode2;
                pr.dispfb1 = m_privRegs->dispfb1; pr.display1 = m_privRegs->display1;
                pr.dispfb2 = m_privRegs->dispfb2; pr.display2 = m_privRegs->display2;
                pr.vsyncTick = currentTick;
                rrv::gsrecord::hookPresent(pr);
            }
            return;
        }
    }

    // Flush this frame's tile-binned primitives before reading VRAM below —
    // see flushPendingTiles()'s comment for the full rationale. m_stateMutex
    // is recursive, so this nested lock_guard is safe. (Async: wait-only — the
    // worker already rendered the displayed buffer; see presentLatchDrain.)
    presentLatchDrain();

    if (!m_privRegs || !m_vram || m_vramSize == 0u)
    {
        m_hostPresentationFrame.clear();
        m_hostPresentationWidth = 0u;
        m_hostPresentationHeight = 0u;
        m_hostPresentationDisplayFbp = 0u;
        m_hostPresentationSourceFbp = 0u;
        m_hostPresentationSourceFrame = {};
        m_hostPresentationSourceValid = false;
        m_hostPresentationUsedPreferred = false;
        m_hasHostPresentationFrame = false;
        return;
    }

    const GSPmodeState pmode = decodePmode(m_privRegs->pmode);
    const GSSmode2State smode2 = decodeSMode2(m_privRegs->smode2);
    const bool applyFieldMode = smode2.interlaced && !smode2.frameMode;
    const bool oddField = (vsyncTick & 1ull) != 0ull;
    const GSFrameReg displayFrame1 = decodeDisplayFrame(m_privRegs->dispfb1);
    const GSFrameReg displayFrame2 = decodeDisplayFrame(m_privRegs->dispfb2);
    const GSDisplayReadOrigin displayOrigin1 = decodeDisplayReadOrigin(m_privRegs->dispfb1);
    const GSDisplayReadOrigin displayOrigin2 = decodeDisplayReadOrigin(m_privRegs->dispfb2);

    uint32_t width1 = 0u;
    uint32_t height1 = 0u;
    uint32_t width2 = 0u;
    uint32_t height2 = 0u;
    decodeDisplaySize(m_privRegs->display1, width1, height1, m_privRegs->smode2);
    decodeDisplaySize(m_privRegs->display2, width2, height2, m_privRegs->smode2);

    // --- Milestone B4: GS-stream recorder present/vsync latch event. The
    // privileged GS registers (PMODE/SMODE2/DISPFB/DISPLAY) are ordinary EE
    // MMIO, not part of the GIF packet stream, and only change at mode
    // boundaries — so we snapshot them here (once per present) as an explicit
    // stream event rather than tracing every individual MMIO write. Same
    // fields as the B1 IR present hook above.
    if (rrv::gsrecord::rrv_gs_record_enabled())
    {
        rrv::gsrecord::RecordPresent pr{};
        pr.pmode = m_privRegs->pmode;   pr.smode2 = m_privRegs->smode2;
        pr.dispfb1 = m_privRegs->dispfb1; pr.display1 = m_privRegs->display1;
        pr.dispfb2 = m_privRegs->dispfb2; pr.display2 = m_privRegs->display2;
        pr.vsyncTick = vsyncTick;
        rrv::gsrecord::hookPresent(pr);
    }

    const bool validCrt1 = pmode.enableCrt1 && hasDisplaySetup(m_privRegs->display1, displayFrame1);
    const bool validCrt2 = pmode.enableCrt2 && hasDisplaySetup(m_privRegs->display2, displayFrame2);

    // The IR boundary closes only after this physical presenter has selected
    // its actual source. PMODE/DISPFB remain metadata; preferred-source and
    // black-buffer recovery can deliberately choose a different completed
    // frame. A null source records an unsupported composition/no-source state
    // and keeps FullFrame fail-closed.
#if !defined(PS2X_RRV_FIELD_ONLY)
    auto hookIrPresent = [&](const GSFrameReg *source)
    {
        if (!rrv::ir::rrv_ir_enabled())
            return;
        rrv::ir::HookPresent pr{};
        pr.pmode = m_privRegs->pmode;   pr.smode2 = m_privRegs->smode2;
        pr.dispfb1 = m_privRegs->dispfb1; pr.display1 = m_privRegs->display1;
        pr.dispfb2 = m_privRegs->dispfb2; pr.display2 = m_privRegs->display2;
        pr.vsyncTick = vsyncTick;
        if (source)
        {
            pr.hostSourceFbp = source->fbp;
            pr.hostSourceFbw = source->fbw;
            pr.hostSourcePsm = source->psm;
            pr.hostSourceValid = 1u;
        }
        rrv::ir::hookPresent(pr);
    };
#else
    auto hookIrPresent = [](const GSFrameReg *) {};
#endif

    auto copyDisplaySource = [&](const GSFrameReg &displayFrame,
                                 const GSDisplayReadOrigin &displayOrigin,
                                 uint32_t width,
                                 uint32_t height,
                                 bool allowPreferred,
                                 bool preserveAlpha,
                                 GSFrameReg &selectedFrame,
                                 std::vector<uint8_t> &scratch,
                                 bool &usedPreferred) -> bool
    {
        selectedFrame = displayFrame;
        scratch.clear();
        usedPreferred = false;

        if (allowPreferred &&
            m_hasPreferredDisplaySource &&
            m_preferredDisplayDestFbp == displayFrame.fbp &&
            (m_preferredDisplaySourceFrame.fbw != 0u || m_preferredDisplaySourceFrame.fbp != displayFrame.fbp))
        {
            if (copyFrameToHostRgbaUnlocked(m_preferredDisplaySourceFrame,
                                            width,
                                            height,
                                            scratch,
                                            preserveAlpha,
                                            true,
                                            false,
                                            0u,
                                            0u))
            {
                auditPresentConvertCall(/*site=*/2, m_vram, m_vramSize, m_preferredDisplaySourceFrame,
                                        width, height, scratch);
                selectedFrame = m_preferredDisplaySourceFrame;
                usedPreferred = true;
            }
        }

        if (scratch.empty())
        {
            if (!copyFrameToHostRgbaUnlocked(displayFrame,
                                             width,
                                             height,
                                             scratch,
                                             preserveAlpha,
                                             true,
                                             true,
                                             displayOrigin.x,
                                             displayOrigin.y))
            {
                return false;
            }
            auditPresentConvertCall(/*site=*/3, m_vram, m_vramSize, displayFrame, width, height, scratch);
        }

        // RRV double-buffers its display target symmetrically between two fbp
        // values (observed: 0 and 70): DISPFB can point at either one at latch
        // time, and our latch samples less often than the game flips FRAME/
        // DISPFB, so the selected buffer may not have this cycle's draw yet.
        // The candidate search below recovers the other context's real content
        // in that case; it must apply regardless of which fbp is selected, not
        // just fbp==0, or every other latch on the fbp==70 half reads back black.
        if (!usedPreferred && countNonBlackPixels(scratch, width, height) == 0u)
        {
            for (int contextIndex = 0; contextIndex < 2; ++contextIndex)
            {
                const GSFrameReg &candidate = m_ctx[contextIndex].frame;
                if (candidate.fbp == selectedFrame.fbp &&
                    candidate.fbw == selectedFrame.fbw &&
                    candidate.psm == selectedFrame.psm)
                {
                    continue;
                }

                std::vector<uint8_t> candidatePixels;
                if (!copyFrameToHostRgbaUnlocked(candidate,
                                                 width,
                                                 height,
                                                 candidatePixels,
                                                 preserveAlpha,
                                                 true,
                                                 true,
                                                 0u,
                                                 0u))
                {
                    continue;
                }
                auditPresentConvertCall(/*site=*/4, m_vram, m_vramSize, candidate, width, height,
                                        candidatePixels);

                if (countNonBlackPixels(candidatePixels, width, height) == 0u)
                {
                    continue;
                }

                selectedFrame = candidate;
                scratch.swap(candidatePixels);
                break;
            }
        }

        return true;
    };

    if (!validCrt1 && !validCrt2)
    {
        m_hostPresentationFrame.clear();
        m_hostPresentationWidth = 0u;
        m_hostPresentationHeight = 0u;
        m_hostPresentationDisplayFbp = 0u;
        m_hostPresentationSourceFbp = 0u;
        m_hostPresentationSourceFrame = {};
        m_hostPresentationSourceValid = false;
        m_hostPresentationUsedPreferred = false;
        m_hasHostPresentationFrame = false;
        hookIrPresent(nullptr);
        return;
    }

    if (validCrt1 && validCrt2)
    {
        GSFrameReg selectedFrame1{};
        GSFrameReg selectedFrame2{};
        std::vector<uint8_t> rc1;
        std::vector<uint8_t> rc2;
        bool usedPreferred1 = false;
        bool usedPreferred2 = false;

        const bool copiedCrt1 = copyDisplaySource(displayFrame1, displayOrigin1, width1, height1, false, true, selectedFrame1, rc1, usedPreferred1);
        const bool copiedCrt2 = copyDisplaySource(displayFrame2, displayOrigin2, width2, height2, false, true, selectedFrame2, rc2, usedPreferred2);

        if (copiedCrt1 && copiedCrt2)
        {
            const uint32_t width = std::max(width1, width2);
            const uint32_t height = std::max(height1, height2);
            const uint8_t bgR = static_cast<uint8_t>(m_privRegs->bgcolor & 0xFFu);
            const uint8_t bgG = static_cast<uint8_t>((m_privRegs->bgcolor >> 8) & 0xFFu);
            const uint8_t bgB = static_cast<uint8_t>((m_privRegs->bgcolor >> 16) & 0xFFu);
            const uint8_t bgA = pmode.alp;

            std::vector<uint8_t> merged(kHostFrameWidth * kHostFrameHeight * 4u, 0u);
            for (uint32_t y = 0; y < height; ++y)
            {
                uint8_t *dstRow = merged.data() + (y * kHostFrameWidth * 4u);
                for (uint32_t x = 0; x < width; ++x)
                {
                    dstRow[x * 4u + 0u] = bgR;
                    dstRow[x * 4u + 1u] = bgG;
                    dstRow[x * 4u + 2u] = bgB;
                    dstRow[x * 4u + 3u] = bgA;
                }
            }

            if (!pmode.slbg)
            {
                for (uint32_t y = 0; y < height2; ++y)
                {
                    const uint8_t *srcRow = rc2.data() + (y * kHostFrameWidth * 4u);
                    uint8_t *dstRow = merged.data() + (y * kHostFrameWidth * 4u);
                    for (uint32_t x = 0; x < width2; ++x)
                    {
                        dstRow[x * 4u + 0u] = srcRow[x * 4u + 0u];
                        dstRow[x * 4u + 1u] = srcRow[x * 4u + 1u];
                        dstRow[x * 4u + 2u] = srcRow[x * 4u + 2u];
                        dstRow[x * 4u + 3u] = srcRow[x * 4u + 3u];
                    }
                }
            }

            for (uint32_t y = 0; y < height1; ++y)
            {
                const uint8_t *srcRow = rc1.data() + (y * kHostFrameWidth * 4u);
                uint8_t *dstRow = merged.data() + (y * kHostFrameWidth * 4u);
                for (uint32_t x = 0; x < width1; ++x)
                {
                    const uint8_t srcR = srcRow[x * 4u + 0u];
                    const uint8_t srcG = srcRow[x * 4u + 1u];
                    const uint8_t srcB = srcRow[x * 4u + 2u];
                    const uint8_t srcA = srcRow[x * 4u + 3u];
                    const uint8_t dstR = dstRow[x * 4u + 0u];
                    const uint8_t dstG = dstRow[x * 4u + 1u];
                    const uint8_t dstB = dstRow[x * 4u + 2u];
                    const uint8_t dstA = dstRow[x * 4u + 3u];
                    const uint32_t factor = pmode.mmod
                                                ? static_cast<uint32_t>(pmode.alp)
                                                : std::min<uint32_t>(255u, static_cast<uint32_t>(srcA) * 2u);

                    dstRow[x * 4u + 0u] = blendPresentationChannel(srcR, dstR, factor);
                    dstRow[x * 4u + 1u] = blendPresentationChannel(srcG, dstG, factor);
                    dstRow[x * 4u + 2u] = blendPresentationChannel(srcB, dstB, factor);
                    dstRow[x * 4u + 3u] = pmode.amod ? dstA : srcA;
                }
            }

            for (uint32_t y = 0; y < height; ++y)
            {
                uint8_t *row = merged.data() + (y * kHostFrameWidth * 4u);
                for (uint32_t x = 0; x < width; ++x)
                {
                    row[x * 4u + 3u] = 255u;
                }
            }

            if (applyFieldMode)
            {
                applyFieldPresentation(merged, width, height, oddField);
            }

            m_hostPresentationFrame.swap(merged);
            m_hostPresentationWidth = width;
            m_hostPresentationHeight = height;
            m_hostPresentationDisplayFbp = displayFrame1.fbp;
            m_hostPresentationSourceFbp = selectedFrame1.fbp;
            m_hostPresentationSourceFrame = {};
            m_hostPresentationSourceValid = false;
            m_hostPresentationUsedPreferred = false;
            m_hasHostPresentationFrame = true;
            hookIrPresent(nullptr);
            return;
        }
    }

    const GSFrameReg &displayFrame = validCrt1 ? displayFrame1 : displayFrame2;
    const uint32_t width = validCrt1 ? width1 : width2;
    const uint32_t height = validCrt1 ? height1 : height2;

    GSFrameReg selectedFrame = displayFrame;
    std::vector<uint8_t> scratch;
    bool usedPreferred = false;
    const GSDisplayReadOrigin &displayOrigin = validCrt1 ? displayOrigin1 : displayOrigin2;
    if (!copyDisplaySource(displayFrame, displayOrigin, width, height, true, false, selectedFrame, scratch, usedPreferred))
    {
        m_hostPresentationFrame.clear();
        m_hostPresentationWidth = 0u;
        m_hostPresentationHeight = 0u;
        m_hostPresentationDisplayFbp = displayFrame.fbp;
        m_hostPresentationSourceFbp = 0u;
        m_hostPresentationSourceFrame = {};
        m_hostPresentationSourceValid = false;
        m_hostPresentationUsedPreferred = false;
        m_hasHostPresentationFrame = false;
        hookIrPresent(nullptr);
        return;
    }

    if (applyFieldMode)
    {
        applyFieldPresentation(scratch, width, height, oddField);
    }

    normalizePresentationAlpha(scratch, width, height);

    // RRV_PRESENT_DIAG: per-latch content probe. Logs the displayed buffer's fbp and
    // its non-black pixel count so we can tell whether the PRESENTED image cycles
    // empty->full (present latches partial buffers) or is steady (generation issue).
    {
        static int s_probe = -1;
        if (s_probe < 0) { const char *v = std::getenv("RRV_PRESENT_DIAG"); s_probe = (v && v[0] && v[0] != '0') ? 1 : 0; }
        if (s_probe)
        {
            const uint32_t nb = countNonBlackPixels(scratch, width, height);
            fprintf(stderr, "[presentdiag] tick=%llu dispFbp=%u srcFbp=%u pref=%d %ux%u nonBlackPx=%u (%.1f%%)\n",
                    static_cast<unsigned long long>(vsyncTick), displayFrame.fbp, selectedFrame.fbp,
                    usedPreferred ? 1 : 0, width, height, nb,
                    (width * height) ? (100.0 * nb / (double)(width * height)) : 0.0);
        }
    }

    // RRV_CLUT_PROBE (present-boundary tap): same 3 CLUT blocks as the upload-side
    // probe in processImageData, sampled once per ~30 present latches so we can see
    // whether the CLUT is still intact at the point a frame is actually consumed for
    // display (not just right after its own upload). Throttled -- this runs every
    // vblank so an unthrottled dump would flood stderr.
    if (m_vram)
    {
        static int s_clutProbePresent = -1;
        if (s_clutProbePresent < 0)
        {
            const char *v = std::getenv("RRV_CLUT_PROBE");
            s_clutProbePresent = (v && v[0] && v[0] != '0') ? 1 : 0;
        }
        if (s_clutProbePresent)
        {
            static uint32_t s_presentCounter = 0;
            if ((s_presentCounter++ % 30u) == 0u)
            {
                static constexpr u32 kProbeBlocks[3] = {8620u, 8596u, 8960u};
                static constexpr u32 kProbeBytes = 1024u;
                fprintf(stderr, "[clutprobe:present] latch#%u dispFbp=%u srcFbp=%u\n",
                        s_presentCounter - 1u, displayFrame.fbp, selectedFrame.fbp);
                for (u32 bi = 0; bi < 3; ++bi)
                {
                    const u32 off = kProbeBlocks[bi] * 256u;
                    u32 laneNonZero[4] = {0, 0, 0, 0};
                    uint64_t fnv = 0xcbf29ce484222325ULL;
                    for (u32 i = 0; i < kProbeBytes; i += 4)
                    {
                        for (u32 lane = 0; lane < 4; ++lane)
                        {
                            u8 b = m_vram[off + i + lane];
                            if (b != 0)
                                laneNonZero[lane]++;
                            fnv ^= b;
                            fnv *= 0x100000001b3ULL;
                        }
                    }
                    fprintf(stderr,
                            "  [clutprobe:present]   blk%u lane0(nz)=%u/256 lane1(nz)=%u/256 lane2(nz)=%u/256 lane3(nz)=%u/256 hash=%016llx\n",
                            kProbeBlocks[bi],
                            laneNonZero[0], laneNonZero[1], laneNonZero[2], laneNonZero[3],
                            static_cast<unsigned long long>(fnv));
                }
            }
        }
    }

    m_hostPresentationFrame.swap(scratch);
    m_hostPresentationWidth = width;
    m_hostPresentationHeight = height;
    m_hostPresentationDisplayFbp = displayFrame.fbp;
    m_hostPresentationSourceFbp = selectedFrame.fbp;
    m_hostPresentationSourceFrame = selectedFrame;
    m_hostPresentationSourceValid = true;
    m_hostPresentationUsedPreferred = usedPreferred;
    m_hasHostPresentationFrame = true;
    hookIrPresent(&selectedFrame);
}

bool GS::copyLatchedHostPresentationFrame(std::vector<uint8_t> &outPixels,
                                          uint32_t &outWidth,
                                          uint32_t &outHeight,
                                          uint32_t *outDisplayFbp,
                                          uint32_t *outSourceFbp,
                                          bool *outUsedPreferred) const
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (!m_hasHostPresentationFrame || m_hostPresentationFrame.empty())
    {
        outPixels.clear();
        outWidth = 0u;
        outHeight = 0u;
        if (outDisplayFbp)
            *outDisplayFbp = 0u;
        if (outSourceFbp)
            *outSourceFbp = 0u;
        if (outUsedPreferred)
            *outUsedPreferred = false;
        return false;
    }

    outWidth = m_hostPresentationWidth;
    outHeight = m_hostPresentationHeight;
    if (outDisplayFbp)
        *outDisplayFbp = m_hostPresentationDisplayFbp;
    if (outSourceFbp)
        *outSourceFbp = m_hostPresentationSourceFbp;
    if (outUsedPreferred)
        *outUsedPreferred = m_hostPresentationUsedPreferred;

    const size_t packedRowBytes = static_cast<size_t>(outWidth) * 4u;
    outPixels.assign(packedRowBytes * static_cast<size_t>(outHeight), 0u);
    if (outWidth != 0u && outHeight != 0u)
    {
        const size_t sourceRowBytes = static_cast<size_t>(kHostFrameWidth) * 4u;
        for (uint32_t y = 0; y < outHeight; ++y)
        {
            const size_t srcOffset = static_cast<size_t>(y) * sourceRowBytes;
            const size_t dstOffset = static_cast<size_t>(y) * packedRowBytes;
            if (srcOffset + packedRowBytes > m_hostPresentationFrame.size() ||
                dstOffset + packedRowBytes > outPixels.size())
            {
                outPixels.clear();
                outWidth = 0u;
                outHeight = 0u;
                if (outDisplayFbp)
                    *outDisplayFbp = 0u;
                if (outSourceFbp)
                    *outSourceFbp = 0u;
                if (outUsedPreferred)
                    *outUsedPreferred = false;
                return false;
            }

            std::memcpy(outPixels.data() + dstOffset,
                        m_hostPresentationFrame.data() + srcOffset,
                        packedRowBytes);
        }
    }
    return true;
}

void GS::processGIFPacket(const uint8_t *data, uint32_t sizeBytes)
{
    processGIFPacket(GifPathId::Path3, data, sizeBytes);
}

void GS::processGIFPacket(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes,
                          uint32_t vu1Pc, const GifPacketDiagnostic *diagnostic)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (!data || sizeBytes < 16 || !m_vram)
        return;

    struct PacketPathScope
    {
        bool &active;
        GifPathId &current;
        bool oldActive;
        GifPathId oldCurrent;
        ~PacketPathScope()
        {
            active = oldActive;
            current = oldCurrent;
        }
    } packetPathScope{m_processingGifPacket, m_currentGifPacketPath,
                      m_processingGifPacket, m_currentGifPacketPath};
    m_processingGifPacket = true;
    m_currentGifPacketPath = pathId;

    // B-1 car-live GIF capture (RRV_CAR_GIF_DIAG). Per-packet gate: is the demo
    // car slot live right now? If so, every tag below is attributed to this
    // packet's PATH + XGKICK PC + PRIM.
    carGifDiagInit();
    const bool carGifActive = g_carGifDiag.enabled && ps2_diag::carLive();
    uint32_t carGifPrim = 0u;
    m_carGifActive = carGifActive;
    m_carGifPhase = ps2_diag::demoPhase();
    m_carGifVu1Pc = vu1Pc;
    m_carGifVerts = m_carGifDrawing = m_carGifNonfinite = m_carGifOffscreen = m_carGifExtreme = 0u;
    m_carGifBBoxInit = false;
    if (carGifActive)
    {
        std::lock_guard<std::mutex> lk(g_carGifDiag.mtx);
        ++g_carGifDiag.carLivePackets;
    }

    if (diagnostic && diagnostic->carDma && diagnostic->begin <= diagnostic->end &&
        diagnostic->end <= sizeBytes)
    {
        m_carDmaGifDiagActive = true;
        m_carDmaGifDiagCurrentWrite = false;
        m_carDmaGifDiagChain = diagnostic->chainId;
        m_carDmaGifDiagBegin = diagnostic->begin;
        m_carDmaGifDiagEnd = diagnostic->end;
        m_carDmaGifDiagCallTag = diagnostic->callTag;
        m_carDmaGifDiagCallTarget = diagnostic->callTarget;
        // FNV-1a is deliberately simple and platform-independent. Hash exactly
        // the attributed byte range so repeated chains can be compared without
        // depending on host pointers or surrounding packet contents.
        m_carDmaGifDiagDigest = 14695981039346656037ull;
        for (uint32_t byteOffset = m_carDmaGifDiagBegin;
             byteOffset < m_carDmaGifDiagEnd; ++byteOffset)
        {
            m_carDmaGifDiagDigest ^= static_cast<uint64_t>(data[byteOffset]);
            m_carDmaGifDiagDigest *= 1099511628211ull;
        }
        m_carDmaGifDiagTags = 0u;
        m_carDmaGifDiagKicks = 0u;
        m_carDmaGifDiagDrawingKicks = 0u;
        m_carDmaGifDiagAdcKicks = 0u;
        m_carDmaGifDiagNonfinite = 0u;
        m_carDmaGifDiagExtreme = 0u;
        m_carDmaGifDiagXyz2Adc = 0u;
        m_carDmaGifDiagXyz3Descriptor = 0u;
        m_carDmaGifDiagAdXyz3 = 0u;
        m_carDmaGifDiagUnknownNoDraw = 0u;
        m_carDmaGifDiagCoverRecords = 0u;
        m_carDmaGifDiagSuppressedCovers = 0u;
        m_carDmaGifDiagCauseRecords = 0u;
        m_carDmaGifDiagCurrentByteOffset = 0u;
        m_carDmaGifDiagCurrentTagOffset = 0u;
        m_carDmaGifDiagCurrentSourceDesc = 0u;
        m_carDmaGifDiagCurrentTargetReg = 0u;
        m_carDmaGifDiagCurrentRegPhase = 0u;
        m_carDmaGifDiagCurrentRawAdc = false;
        m_carDmaGifDiagMinX = m_carDmaGifDiagMinY = 0.0f;
        m_carDmaGifDiagMaxX = m_carDmaGifDiagMaxY = 0.0f;

        const uint32_t entryContextIndex = m_prim.ctxt ? 1u : 0u;
        const GSContext &entryContext = m_ctx[entryContextIndex];
        std::fprintf(stderr,
                     "[car-dma:%llu] GIF ENTRY digest=%016llx ctx=%u "
                     "XYOFFSET=(%u,%u raw; %.3f,%.3f px) "
                     "SCISSOR=[%u,%u..%u,%u]\n",
                     static_cast<unsigned long long>(m_carDmaGifDiagChain),
                     static_cast<unsigned long long>(m_carDmaGifDiagDigest),
                     entryContextIndex, static_cast<uint32_t>(entryContext.xyoffset.ofx),
                     static_cast<uint32_t>(entryContext.xyoffset.ofy),
                     static_cast<double>(entryContext.xyoffset.ofx) / 16.0,
                     static_cast<double>(entryContext.xyoffset.ofy) / 16.0,
                     static_cast<uint32_t>(entryContext.scissor.x0),
                     static_cast<uint32_t>(entryContext.scissor.y0),
                     static_cast<uint32_t>(entryContext.scissor.x1),
                     static_cast<uint32_t>(entryContext.scissor.y1));
    }

    auto finishCarDmaGifDiag = [&]()
    {
        if (!m_carDmaGifDiagActive)
            return;
        std::fprintf(stderr,
                     "[car-dma:%llu] GIF TERMINAL path=%u packet-bytes=%u interval=[%u,%u) "
                     "digest=%016llx call=%08x target=%08x tags=%u kicks=%u drawing=%u "
                     "no-draw=%u XYZ2_ADC=%u XYZ3_DESCRIPTOR=%u AD_XYZ3=%u unknown-no-draw=%u "
                     "cover-records=%u suppressed-covers=%u bbox=[%.3f,%.3f..%.3f,%.3f] "
                     "extreme=%u nonfinite=%u\n",
                     static_cast<unsigned long long>(m_carDmaGifDiagChain),
                     static_cast<uint32_t>(pathId), sizeBytes, m_carDmaGifDiagBegin,
                     m_carDmaGifDiagEnd,
                     static_cast<unsigned long long>(m_carDmaGifDiagDigest),
                     m_carDmaGifDiagCallTag, m_carDmaGifDiagCallTarget,
                     m_carDmaGifDiagTags, m_carDmaGifDiagKicks, m_carDmaGifDiagDrawingKicks,
                     m_carDmaGifDiagAdcKicks, m_carDmaGifDiagXyz2Adc,
                     m_carDmaGifDiagXyz3Descriptor, m_carDmaGifDiagAdXyz3,
                     m_carDmaGifDiagUnknownNoDraw, m_carDmaGifDiagCoverRecords,
                     m_carDmaGifDiagSuppressedCovers, m_carDmaGifDiagMinX, m_carDmaGifDiagMinY,
                     m_carDmaGifDiagMaxX, m_carDmaGifDiagMaxY, m_carDmaGifDiagExtreme,
                     m_carDmaGifDiagNonfinite);
        m_carDmaGifDiagCurrentWrite = false;
        m_carDmaGifDiagActive = false;
    };

    auto carDmaContains = [&](uint32_t byteOffset, uint32_t byteCount)
    {
        return m_carDmaGifDiagActive && byteOffset >= m_carDmaGifDiagBegin &&
               static_cast<uint64_t>(byteOffset) + byteCount <= m_carDmaGifDiagEnd;
    };

    // A provenance tag is packet-scoped. In particular, PATH2/3 must reset the
    // thread-local VU1 PC rather than inheriting the preceding PATH1 XGKICK.
#if !defined(PS2X_RRV_FIELD_ONLY)
    rrv::ir::hookNoteGifSource(static_cast<uint8_t>(pathId), vu1Pc);
#endif

    // Tile-binning frame boundary (GS thread): a VSync-tick change means the
    // previous frame's geometry is complete — flush it in one parallel dispatch
    // before this frame's primitives start accumulating. The present thread thus
    // always reads a fully-rendered frame from VRAM.
    // Milestone C1: the IR-raster path (RRV_IR_RASTER) shares this frame-boundary
    // flush. flushTiles no-ops when tiles are disabled/empty; flushIR no-ops
    // unless RRV_IR_RASTER — so both defaults are unchanged.
    if (GSRasterizer::tileEnabled()
#if !defined(PS2X_RRV_FIELD_ONLY)
        || rrv::ir::rrv_ir_raster_enabled()
#endif
        )
    {
        const uint64_t tick = ps2_syscalls::GetCurrentVSyncTick();
        if (tick != m_lastTileFlushTick)
        {
            // Async raster (RRV_ASYNC_RASTER): hand the completed frame to the
            // render worker and return, so this thread starts accumulating the
            // next frame while the worker rasterizes. Every VRAM fence
            // (flushTiles at the present latch / uploads / transfers / read-back)
            // waits for the worker first, so ordering is preserved. Falls back to
            // the inline flush when async is off, tiles are off, or the IR-raster
            // path is active (which stays synchronous).
            // Diagnostic (RRV_ASYNC_RASTER_DIAG): time the gameThread's frame-
            // boundary cost. This is the number async is meant to shrink — in
            // sync mode it is the inline raster; in async mode it is only
            // dispatch + any wait() on the worker. Reported as ms spent here per
            // wall-second, robust to which scene the capture lands on.
            static const bool s_gtBoundaryDiag = std::getenv("RRV_ASYNC_RASTER_DIAG") != nullptr;
            const auto s_gtStart = s_gtBoundaryDiag ? std::chrono::steady_clock::now()
                                                    : std::chrono::steady_clock::time_point{};
            if (GSRasterizer::asyncRasterEnabled() && GSRasterizer::tileEnabled()
#if !defined(PS2X_RRV_FIELD_ONLY)
                && !rrv::ir::rrv_ir_raster_enabled()
#endif
                )
                m_rasterizer.dispatchAsyncFrame(this);
            else
                m_rasterizer.flushTiles(this);
#if !defined(PS2X_RRV_FIELD_ONLY)
            m_rasterizer.flushIR(this);
#endif
            m_lastTileFlushTick = tick;
            if (s_gtBoundaryDiag)
            {
                static std::chrono::steady_clock::time_point s_win = std::chrono::steady_clock::now();
                static uint64_t s_ns = 0, s_frames = 0;
                s_ns += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - s_gtStart).count());
                ++s_frames;
                const auto now = std::chrono::steady_clock::now();
                if (now - s_win >= std::chrono::seconds(1))
                {
                    std::fprintf(stderr, "[gt:boundary] frames=%llu gameThread_ms=%.1f/s\n",
                                 static_cast<unsigned long long>(s_frames),
                                 static_cast<double>(s_ns) / 1.0e6);
                    s_win = now; s_ns = 0; s_frames = 0;
                }
            }
        }
    }

    PS2_IF_AGRESSIVE_LOGS({
        const uint32_t packetIndex = s_debugGifPacketCount.fetch_add(1, std::memory_order_relaxed);
        if (packetIndex < 48u)
        {
            const uint64_t tagLo = loadLE64(data);
            const uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFFu);
            const uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3u);
            uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xFu);
            if (nreg == 0u)
                nreg = 16u;
            RUNTIME_LOG("[gs:gif] idx=" << packetIndex
                                        << " size=" << sizeBytes
                                        << " nloop=" << nloop
                                        << " flg=" << static_cast<uint32_t>(flg)
                                        << " nreg=" << nreg
                                        << " ctx0fbp=" << m_ctx[0].frame.fbp
                                        << " ctx1fbp=" << m_ctx[1].frame.fbp
                                        << std::endl);
        }
    });

    uint32_t offset = 0;

    // Resume an IMAGE-mode transfer left unfinished by the previous packet on
    // this path (a >QWC-limit upload split across DMA kicks). Without this the
    // continuation bytes would be misparsed as GIFtags and desync the stream.
    uint32_t &pendingImage = m_pathPendingImageBytes[static_cast<size_t>(pathId) & 3u];
    if (pendingImage > 0)
    {
        const uint32_t chunk = (pendingImage < sizeBytes) ? pendingImage : sizeBytes;
        processImageData(data, chunk);
        pendingImage -= chunk;
        offset = chunk;
    }

    while (offset + 16 <= sizeBytes)
    {
        const uint32_t tagOffset = offset;
        uint64_t tagLo = loadLE64(data + offset);
        uint64_t tagHi = loadLE64(data + offset + 8);
        offset += 16;

        m_curQ = 1.0f;

        uint32_t nloop = static_cast<uint32_t>(tagLo & 0x7FFF);
        uint8_t flg = static_cast<uint8_t>((tagLo >> 58) & 0x3);
        uint32_t nreg = static_cast<uint32_t>((tagLo >> 60) & 0xF);
        if (nreg == 0)
            nreg = 16;

        bool pre = ((tagLo >> 46) & 1) != 0;
        if (pre)
        {
            // A tag PRE write is not part of the attributed payload even when
            // the preceding payload's final register produced a vertex.
            m_carDmaGifDiagCurrentWrite = false;
            writeRegisterUnlocked(GS_REG_PRIM, (tagLo >> 47) & 0x7FF);
        }

        uint8_t regs[16];
        for (uint32_t i = 0; i < nreg; ++i)
            regs[i] = static_cast<uint8_t>((tagHi >> (i * 4)) & 0xF);

        if (carGifActive)
        {
            if (pre)
                carGifPrim = static_cast<uint32_t>((tagLo >> 47) & 0x7FFu);
            // vu1Pc is the originating XGKICK micro-PC for PATH1 (0 for 2/3).
            const uint32_t phase = ps2_diag::demoPhase();
            const uint64_t pcKey = carGifKey(phase, static_cast<uint32_t>(pathId), vu1Pc);
            const uint64_t primKey = carGifKey(phase, static_cast<uint32_t>(pathId), carGifPrim);
            std::lock_guard<std::mutex> lk(g_carGifDiag.mtx);
            ++g_carGifDiag.carLiveTags;
            ++g_carGifDiag.tagsByPhasePathPc[pcKey];
            g_carGifDiag.vertsByPhasePathPc[pcKey] += nloop;
            ++g_carGifDiag.tagsByPhasePathPrim[primKey];
            const auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - g_carGifDiag.last).count() >= 2000)
            {
                carGifDiagDumpLocked();
                g_carGifDiag.last = now;
            }
        }

        if (m_carDmaGifDiagActive)
        {
            const bool tagInInterval = carDmaContains(tagOffset, 16u);
            if (tagInInterval)
                ++m_carDmaGifDiagTags;

            uint32_t registerStride = 0u;
            uint64_t payloadBytes = 0u;
            if (flg == GIF_FMT_PACKED)
            {
                registerStride = 16u;
                payloadBytes = static_cast<uint64_t>(nloop) * nreg * registerStride;
            }
            else if (flg == GIF_FMT_REGLIST)
            {
                registerStride = 8u;
                payloadBytes = static_cast<uint64_t>(nloop) * nreg * registerStride;
            }
            else if (flg == GIF_FMT_IMAGE)
            {
                registerStride = 16u;
                payloadBytes = static_cast<uint64_t>(nloop) * registerStride;
            }

            const uint64_t payloadBegin = offset;
            const uint64_t payloadEnd = payloadBegin + payloadBytes;
            const uint64_t overlapBegin = std::max<uint64_t>(payloadBegin, m_carDmaGifDiagBegin);
            const uint64_t overlapEnd = std::min<uint64_t>(payloadEnd, m_carDmaGifDiagEnd);
            if (overlapBegin < overlapEnd)
            {
                if (m_carDmaGifDiagCoverRecords < 8u)
                {
                    static constexpr char kHexDigits[] = "0123456789abcdef";
                    char regList[17];
                    for (uint32_t i = 0; i < nreg; ++i)
                        regList[i] = kHexDigits[regs[i]];
                    regList[nreg] = '\0';

                    const int32_t phaseAtInterval =
                        (m_carDmaGifDiagBegin < payloadBegin || flg == GIF_FMT_IMAGE ||
                         registerStride == 0u)
                            ? -1
                            : static_cast<int32_t>(((overlapBegin - payloadBegin) /
                                                    registerStride) %
                                                   nreg);
                    std::fprintf(stderr,
                                 "[car-dma:%llu] GIF COVER tag-pos=%u tag-in-interval=%u "
                                 "payload=[%llu,%llu) overlap=[%llu,%llu) NLOOP=%u FLG=%u "
                                 "NREG=%u regs=%s phase-at-interval=%d PRE=%u PRIM=%03x EOP=%u\n",
                                 static_cast<unsigned long long>(m_carDmaGifDiagChain), tagOffset,
                                 tagInInterval ? 1u : 0u,
                                 static_cast<unsigned long long>(payloadBegin),
                                 static_cast<unsigned long long>(payloadEnd),
                                 static_cast<unsigned long long>(overlapBegin),
                                 static_cast<unsigned long long>(overlapEnd), nloop,
                                 static_cast<uint32_t>(flg), nreg, regList, phaseAtInterval,
                                 pre ? 1u : 0u, static_cast<uint32_t>((tagLo >> 47) & 0x7ffu),
                                 static_cast<uint32_t>((tagLo >> 15) & 1u));
                    ++m_carDmaGifDiagCoverRecords;
                }
                else
                {
                    ++m_carDmaGifDiagSuppressedCovers;
                }
            }
        }

        if (flg == GIF_FMT_PACKED)
        {
            for (uint32_t loop = 0; loop < nloop; ++loop)
            {
                for (uint32_t r = 0; r < nreg; ++r)
                {
                    if (offset + 16 > sizeBytes)
                    {
                        finishCarDmaGifDiag();
                        return;
                    }
                    const uint32_t registerOffset = offset;
                    m_carDmaGifDiagCurrentWrite = carDmaContains(registerOffset, 16u);
                    uint64_t lo = loadLE64(data + offset);
                    uint64_t hi = loadLE64(data + offset + 8);
                    if (m_carDmaGifDiagCurrentWrite)
                    {
                        m_carDmaGifDiagCurrentByteOffset = registerOffset;
                        m_carDmaGifDiagCurrentTagOffset = tagOffset;
                        m_carDmaGifDiagCurrentSourceDesc = regs[r];
                        m_carDmaGifDiagCurrentTargetReg =
                            (regs[r] == 0x0eu) ? static_cast<uint8_t>(hi & 0xffu) : regs[r];
                        m_carDmaGifDiagCurrentRegPhase = static_cast<uint8_t>(r);
                        m_carDmaGifDiagCurrentRawAdc =
                            (regs[r] == 0x04u || regs[r] == 0x05u) && ((hi >> 47) & 1u);
                    }
                    offset += 16;
                    writeRegisterPacked(regs[r], lo, hi);
                }
            }
        }
        else if (flg == GIF_FMT_REGLIST)
        {
            for (uint32_t loop = 0; loop < nloop; ++loop)
            {
                for (uint32_t r = 0; r < nreg; ++r)
                {
                    if (offset + 8 > sizeBytes)
                    {
                        finishCarDmaGifDiag();
                        return;
                    }
                    const uint32_t registerOffset = offset;
                    m_carDmaGifDiagCurrentWrite = carDmaContains(registerOffset, 8u);
                    const uint64_t value = loadLE64(data + offset);
                    if (m_carDmaGifDiagCurrentWrite)
                    {
                        m_carDmaGifDiagCurrentByteOffset = registerOffset;
                        m_carDmaGifDiagCurrentTagOffset = tagOffset;
                        m_carDmaGifDiagCurrentSourceDesc = regs[r];
                        m_carDmaGifDiagCurrentTargetReg = regs[r];
                        m_carDmaGifDiagCurrentRegPhase = static_cast<uint8_t>(r);
                        m_carDmaGifDiagCurrentRawAdc = false;
                    }
                    writeRegisterUnlocked(regs[r], value);
                    offset += 8;
                }
            }
            if ((nloop * nreg) & 1)
                offset += 8;
        }
        else if (flg == GIF_FMT_IMAGE)
        {
            uint32_t imageBytes = nloop * 16;
            if (offset + imageBytes > sizeBytes)
            {
                const uint32_t avail = sizeBytes - offset;
                // PATH1 IMAGE mode is disallowed by the GS, and a truncated
                // XGKICK packet must not poison later kicks — only PATH2/3
                // carry the deficit into the next packet.
                if (pathId != GifPathId::Path1)
                    pendingImage = imageBytes - avail;
                imageBytes = avail;
            }
            processImageData(data + offset, imageBytes);
            offset += imageBytes;
        }
        m_carDmaGifDiagCurrentWrite = false;
    }
    finishCarDmaGifDiag();

    // Flush this packet's car-live vertex bbox into the global (phase,path,pc) bin.
    if (m_carGifActive && m_carGifVerts)
    {
        const uint64_t key = carGifKey(m_carGifPhase, static_cast<uint32_t>(pathId), m_carGifVu1Pc);
        std::lock_guard<std::mutex> lk(g_carGifDiag.mtx);
        CarGifBBox &b = g_carGifDiag.bboxByPhasePathPc[key];
        if (m_carGifBBoxInit)
        {
            if (!b.init)
            {
                b.minx = m_carGifMinX; b.miny = m_carGifMinY;
                b.maxx = m_carGifMaxX; b.maxy = m_carGifMaxY;
                b.init = true;
            }
            else
            {
                b.minx = std::min(b.minx, m_carGifMinX);
                b.miny = std::min(b.miny, m_carGifMinY);
                b.maxx = std::max(b.maxx, m_carGifMaxX);
                b.maxy = std::max(b.maxy, m_carGifMaxY);
            }
        }
        b.verts += m_carGifVerts;
        b.drawing += m_carGifDrawing;
        b.nonfinite += m_carGifNonfinite;
        b.offscreen += m_carGifOffscreen;
        b.extreme += m_carGifExtreme;
    }
    m_carGifActive = false;
}

void GS::writeRegisterPacked(uint8_t regDesc, uint64_t lo, uint64_t hi)
{
    switch (regDesc)
    {
    case 0x00:
        writeRegisterUnlocked(GS_REG_PRIM, lo & 0x7FF);
        break;
    case 0x01:
        m_curR = static_cast<uint8_t>(lo & 0xFF);
        m_curG = static_cast<uint8_t>((lo >> 32) & 0xFF);
        m_curB = static_cast<uint8_t>(hi & 0xFF);
        m_curA = static_cast<uint8_t>((hi >> 32) & 0xFF);
        break;
    case 0x02:
    {
        uint32_t sBits = static_cast<uint32_t>(lo & 0xFFFFFFFF);
        uint32_t tBits = static_cast<uint32_t>((lo >> 32) & 0xFFFFFFFF);
        uint32_t qBits = static_cast<uint32_t>(hi & 0xFFFFFFFF);
        std::memcpy(&m_curS, &sBits, 4);
        std::memcpy(&m_curT, &tBits, 4);
        std::memcpy(&m_curQ, &qBits, 4);
        if (m_curQ == 0.0f)
            m_curQ = 1.0f;
        break;
    }
    case 0x03:
        m_curU = static_cast<uint16_t>(lo & 0xFFFFu);
        m_curV = static_cast<uint16_t>((lo >> 32) & 0xFFFFu);
        break;
    case 0x04:
    {
        uint16_t x = static_cast<uint16_t>(lo & 0xFFFF);
        uint16_t y = static_cast<uint16_t>((lo >> 32) & 0xFFFF);
        uint32_t z = static_cast<uint32_t>((hi >> 4) & 0xFFFFFF);
        uint8_t f = static_cast<uint8_t>((hi >> 36) & 0xFF);
        bool adk = ((hi >> 47) & 1) != 0;
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t debugIndex = s_debugGsPackedVertexCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 64u)
            {
                RUNTIME_LOG("[gs:packed-xyzf] idx=" << debugIndex
                                                    << " x=" << x
                                                    << " y=" << y
                                                    << " z=0x" << std::hex << z
                                                    << std::dec
                                                    << " fog=" << static_cast<uint32_t>(f)
                                                    << " kick=" << static_cast<uint32_t>(!adk ? 1u : 0u)
                                                    << " prim=" << static_cast<uint32_t>(m_prim.type)
                                                    << std::endl);
            }
        });
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(x) / 16.0f;
        vtx.y = static_cast<float>(y) / 16.0f;
        vtx.z = static_cast<float>(z);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = f;
        vertexKick(!adk);
        break;
    }
    case 0x05:
    {
        uint16_t x = static_cast<uint16_t>(lo & 0xFFFF);
        uint16_t y = static_cast<uint16_t>((lo >> 32) & 0xFFFF);
        uint32_t z = static_cast<uint32_t>(hi & 0xFFFFFFFF);
        bool adk = ((hi >> 47) & 1) != 0;
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t debugIndex = s_debugGsPackedVertexCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 64u)
            {
                RUNTIME_LOG("[gs:packed-xyz] idx=" << debugIndex
                                                   << " x=" << x
                                                   << " y=" << y
                                                   << " z=0x" << std::hex << z
                                                   << std::dec
                                                   << " kick=" << static_cast<uint32_t>(!adk ? 1u : 0u)
                                                   << " prim=" << static_cast<uint32_t>(m_prim.type)
                                                   << std::endl);
            }
        });
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(x) / 16.0f;
        vtx.y = static_cast<float>(y) / 16.0f;
        vtx.z = static_cast<float>(z);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = m_curFog;
        vertexKick(!adk);
        break;
    }
    case 0x0A:
        m_curFog = static_cast<uint8_t>((hi >> 36) & 0xFF);
        break;
    case 0x0C:
    {
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t debugIndex = s_debugGsPackedVertexCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 64u)
            {
                RUNTIME_LOG("[gs:packed-xyzf3] idx=" << debugIndex
                                                     << " x=" << static_cast<uint32_t>(lo & 0xFFFFu)
                                                     << " y=" << static_cast<uint32_t>((lo >> 32) & 0xFFFFu)
                                                     << " kick=0"
                                                     << " prim=" << static_cast<uint32_t>(m_prim.type)
                                                     << std::endl);
            }
        });
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(lo & 0xFFFF) / 16.0f;
        vtx.y = static_cast<float>((lo >> 32) & 0xFFFF) / 16.0f;
        vtx.z = static_cast<float>((hi >> 4) & 0xFFFFFF);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = static_cast<uint8_t>((hi >> 36) & 0xFF);
        vertexKick(false);
        break;
    }
    case 0x0D:
    {
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t debugIndex = s_debugGsPackedVertexCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 64u)
            {
                RUNTIME_LOG("[gs:packed-xyz3] idx=" << debugIndex
                                                    << " x=" << static_cast<uint32_t>(lo & 0xFFFFu)
                                                    << " y=" << static_cast<uint32_t>((lo >> 32) & 0xFFFFu)
                                                    << " kick=0"
                                                    << " prim=" << static_cast<uint32_t>(m_prim.type)
                                                    << std::endl);
            }
        });
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(lo & 0xFFFF) / 16.0f;
        vtx.y = static_cast<float>((lo >> 32) & 0xFFFF) / 16.0f;
        vtx.z = static_cast<float>(hi & 0xFFFFFFFF);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = m_curFog;
        vertexKick(false);
        break;
    }
    case 0x0E:
    {
        uint8_t addr = static_cast<uint8_t>(hi & 0xFF);
        writeRegisterUnlocked(addr, lo);
        break;
    }
    case 0x0F:
        break;
    default:
        writeRegisterUnlocked(regDesc, lo);
        break;
    }
}

void GS::writeRegister(uint8_t regAddr, uint64_t value)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    writeRegisterUnlocked(regAddr, value);
}

// Same body as writeRegister but assumes m_stateMutex is already held. The GIF
// packet path (processGIFPacket / writeRegisterPacked) holds the lock once for
// the whole packet, so the per-register calls must NOT re-lock the recursive
// mutex — that re-locking was pure overhead (heavy pthread churn in profiles).
void GS::writeRegisterUnlocked(uint8_t regAddr, uint64_t value)
{
    const bool interestingReg =
        regAddr == GS_REG_PRIM ||
        regAddr == GS_REG_RGBAQ ||
        regAddr == GS_REG_ST ||
        regAddr == GS_REG_UV ||
        regAddr == GS_REG_XYZ2 ||
        regAddr == GS_REG_XYZ3 ||
        regAddr == GS_REG_XYZF2 ||
        regAddr == GS_REG_XYZF3 ||
        regAddr == GS_REG_TEX0_1 ||
        regAddr == GS_REG_TEX0_2 ||
        regAddr == GS_REG_TEX2_1 ||
        regAddr == GS_REG_TEX2_2 ||
        regAddr == GS_REG_TEXCLUT ||
        regAddr == GS_REG_TEXA ||
        regAddr == GS_REG_XYOFFSET_1 ||
        regAddr == GS_REG_XYOFFSET_2 ||
        regAddr == GS_REG_SCISSOR_1 ||
        regAddr == GS_REG_SCISSOR_2 ||
        regAddr == GS_REG_FRAME_1 ||
        regAddr == GS_REG_FRAME_2 ||
        regAddr == GS_REG_ALPHA_1 ||
        regAddr == GS_REG_ALPHA_2 ||
        regAddr == GS_REG_TEST_1 ||
        regAddr == GS_REG_TEST_2 ||
        regAddr == GS_REG_BITBLTBUF ||
        regAddr == GS_REG_TRXPOS ||
        regAddr == GS_REG_TRXREG ||
        regAddr == GS_REG_TRXDIR;

    PS2_IF_AGRESSIVE_LOGS({
        if (interestingReg)
        {
            const uint32_t debugIndex = s_debugGsRegisterCount.fetch_add(1, std::memory_order_relaxed);
            if (debugIndex < 128u)
            {
                RUNTIME_LOG("[gs:reg] idx=" << debugIndex
                                            << " reg=0x" << std::hex << static_cast<uint32_t>(regAddr)
                                            << " value=0x" << value
                                            << std::dec
                                            << std::endl);
            }
        }
    });

    const bool isCopyRelevantReg =
        regAddr == GS_REG_PRIM ||
        regAddr == GS_REG_TEX0_2 ||
        regAddr == GS_REG_TEX1_2 ||
        regAddr == GS_REG_ALPHA_2 ||
        regAddr == GS_REG_TEST_2 ||
        regAddr == GS_REG_PABE ||
        regAddr == GS_REG_FRAME_2 ||
        regAddr == GS_REG_XYOFFSET_2 ||
        regAddr == GS_REG_SCISSOR_2;
    PS2_IF_AGRESSIVE_LOGS({
        if (isCopyRelevantReg &&
            s_debugCopyRegCount.fetch_add(1u, std::memory_order_relaxed) < 64u)
        {
            RUNTIME_LOG("[gs:copy-reg] reg=0x"
                        << std::hex << static_cast<uint32_t>(regAddr)
                        << " value=0x" << value
                        << std::dec
                        << " primCtxt=" << static_cast<uint32_t>(m_prim.ctxt)
                        << " ctx0fbp=" << m_ctx[0].frame.fbp
                        << " ctx1fbp=" << m_ctx[1].frame.fbp
                        << std::endl);
        }
    });

    switch (regAddr)
    {
    case GS_REG_PRIM:
    {
        m_prim.type = static_cast<GSPrimType>(value & 0x7);
        m_prim.iip = ((value >> 3) & 1) != 0;
        m_prim.tme = ((value >> 4) & 1) != 0;
        m_prim.fge = ((value >> 5) & 1) != 0;
        m_prim.abe = ((value >> 6) & 1) != 0;
        m_prim.aa1 = ((value >> 7) & 1) != 0;
        m_prim.fst = ((value >> 8) & 1) != 0;
        m_prim.ctxt = ((value >> 9) & 1) != 0;
        m_prim.fix = ((value >> 10) & 1) != 0;
        m_vtxCount = 0;
        m_vtxIndex = 0;
        break;
    }
    case GS_REG_RGBAQ:
    {
        m_curR = static_cast<uint8_t>(value & 0xFF);
        m_curG = static_cast<uint8_t>((value >> 8) & 0xFF);
        m_curB = static_cast<uint8_t>((value >> 16) & 0xFF);
        m_curA = static_cast<uint8_t>((value >> 24) & 0xFF);
        uint32_t qBits = static_cast<uint32_t>((value >> 32) & 0xFFFFFFFF);
        std::memcpy(&m_curQ, &qBits, 4);
        if (m_curQ == 0.0f)
            m_curQ = 1.0f;
        break;
    }
    case GS_REG_ST:
    {
        uint32_t sBits = static_cast<uint32_t>(value & 0xFFFFFFFF);
        uint32_t tBits = static_cast<uint32_t>((value >> 32) & 0xFFFFFFFF);
        std::memcpy(&m_curS, &sBits, 4);
        std::memcpy(&m_curT, &tBits, 4);
        break;
    }
    case GS_REG_UV:
    {
        m_curU = static_cast<uint16_t>(value & 0x3FFFu);
        m_curV = static_cast<uint16_t>((value >> 16) & 0x3FFFu);
        break;
    }
    case GS_REG_XYZF2:
    case GS_REG_XYZF3:
    {
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(value & 0xFFFF) / 16.0f;
        vtx.y = static_cast<float>((value >> 16) & 0xFFFF) / 16.0f;
        vtx.z = static_cast<double>((value >> 32) & 0xFFFFFF);
        vtx.fog = static_cast<uint8_t>((value >> 56) & 0xFF);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vertexKick(regAddr == GS_REG_XYZF2);
        break;
    }
    case GS_REG_XYZ2:
    case GS_REG_XYZ3:
    {
        GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        vtx.x = static_cast<float>(value & 0xFFFF) / 16.0f;
        vtx.y = static_cast<float>((value >> 16) & 0xFFFF) / 16.0f;
        vtx.z = static_cast<double>((value >> 32) & 0xFFFFFFFF);
        vtx.r = m_curR;
        vtx.g = m_curG;
        vtx.b = m_curB;
        vtx.a = m_curA;
        vtx.q = m_curQ;
        vtx.s = m_curS;
        vtx.t = m_curT;
        vtx.u = m_curU;
        vtx.v = m_curV;
        vtx.fog = m_curFog;
        vertexKick(regAddr == GS_REG_XYZ2);
        break;
    }
    case GS_REG_TEX0_1:
    case GS_REG_TEX0_2:
    {
        int ci = (regAddr == GS_REG_TEX0_2) ? 1 : 0;
        auto &t = m_ctx[ci].tex0;
        t.tbp0 = static_cast<uint32_t>(value & 0x3FFF);
        t.tbw = static_cast<uint8_t>((value >> 14) & 0x3F);
        t.psm = static_cast<uint8_t>((value >> 20) & 0x3F);
        t.tw = static_cast<uint8_t>((value >> 26) & 0xF);
        t.th = static_cast<uint8_t>((value >> 30) & 0xF);
        t.tcc = static_cast<uint8_t>((value >> 34) & 0x1);
        t.tfx = static_cast<uint8_t>((value >> 35) & 0x3);
        t.cbp = static_cast<uint32_t>((value >> 37) & 0x3FFF);
        t.cpsm = static_cast<uint8_t>((value >> 51) & 0xF);
        t.csm = static_cast<uint8_t>((value >> 55) & 0x1);
        t.csa = static_cast<uint8_t>((value >> 56) & 0x1F);
        t.cld = static_cast<uint8_t>((value >> 61) & 0x7);
        // RRV_GS_CLUT_CACHE: latch (or skip, per CLD) at TEX0-write time, not
        // at draw/sample time -- see GSRasterizer::latchClut.
        m_rasterizer.latchClut(this, ci);
        // RRV_CLUT_PROBE: at the girl's CLUT-bearing TEX0 write (the CLD latch
        // point), print the average RGB of the palette in VRAM RIGHT NOW. If it
        // is already RGB~0 here, the aux-buffer clobber precedes the latch (an
        // on-chip CLUT cache cannot rescue it -> ordering/extent fix); if it is
        // still coloured, the latch beats the clobber and the fix is to make the
        // (binned) draws sample the latched palette.
        {
            static int s_lp = -1;
            if (s_lp < 0) { const char *v = std::getenv("RRV_CLUT_PROBE"); s_lp = (v && v[0] && v[0] != '0') ? 1 : 0; }
            if (s_lp && m_vram &&
                (t.cbp == 6976u || t.cbp == 6980u || t.cbp == 6984u ||
                 t.cbp == 6992u || t.cbp == 6996u) &&
                (t.psm == 0x13 || t.psm == 0x14 || t.psm == 0x1B))
            {
                const uint32_t off = t.cbp * 256u;
                uint64_t sr = 0, sg = 0, sb = 0, sa = 0;
                for (uint32_t e = 0; e < 256u; ++e) {
                    sr += m_vram[off + e * 4 + 0]; sg += m_vram[off + e * 4 + 1];
                    sb += m_vram[off + e * 4 + 2]; sa += m_vram[off + e * 4 + 3];
                }
                fprintf(stderr, "[clutlatch] cbp=%u cld=%u avgRGBA=[%llu,%llu,%llu,%llu]\n",
                        t.cbp, t.cld, (unsigned long long)(sr/256),(unsigned long long)(sg/256),
                        (unsigned long long)(sb/256),(unsigned long long)(sa/256));
            }
        }
        break;
    }
    case GS_REG_CLAMP_1:
    case GS_REG_CLAMP_2:
    {
        int ci = (regAddr == GS_REG_CLAMP_2) ? 1 : 0;
        m_ctx[ci].clamp = value;
        break;
    }
    case GS_REG_FOG:
        m_curFog = static_cast<uint8_t>((value >> 56) & 0xFF);
        break;
    case GS_REG_TEX1_1:
    case GS_REG_TEX1_2:
    {
        int ci = (regAddr == GS_REG_TEX1_2) ? 1 : 0;
        m_ctx[ci].tex1 = value;
        break;
    }
    case GS_REG_TEX2_1:
    case GS_REG_TEX2_2:
    {
        int ci = (regAddr == GS_REG_TEX2_2) ? 1 : 0;
        auto &t = m_ctx[ci].tex0;
        t.psm = static_cast<uint8_t>((value >> 20) & 0x3F);
        t.cbp = static_cast<uint32_t>((value >> 37) & 0x3FFF);
        t.cpsm = static_cast<uint8_t>((value >> 51) & 0xF);
        t.csm = static_cast<uint8_t>((value >> 55) & 0x1);
        t.csa = static_cast<uint8_t>((value >> 56) & 0x1F);
        t.cld = static_cast<uint8_t>((value >> 61) & 0x7);
        // RRV_GS_CLUT_CACHE: TEX2 changes only CLUT-related bits (tbp0/tbw/tw/
        // th/tcc/tfx are untouched), but it can still trigger a reload.
        m_rasterizer.latchClut(this, ci);
        break;
    }
    case GS_REG_XYOFFSET_1:
    case GS_REG_XYOFFSET_2:
    {
        int ci = (regAddr == GS_REG_XYOFFSET_2) ? 1 : 0;
        m_ctx[ci].xyoffset.ofx = static_cast<uint16_t>(value & 0xFFFF);
        m_ctx[ci].xyoffset.ofy = static_cast<uint16_t>((value >> 32) & 0xFFFF);
        break;
    }
    case GS_REG_PRMODECONT:
        m_prmodecont = (value & 1) != 0;
        break;
    case GS_REG_PRMODE:
        if (!m_prmodecont)
        {
            m_prim.iip = ((value >> 3) & 1) != 0;
            m_prim.tme = ((value >> 4) & 1) != 0;
            m_prim.fge = ((value >> 5) & 1) != 0;
            m_prim.abe = ((value >> 6) & 1) != 0;
            m_prim.aa1 = ((value >> 7) & 1) != 0;
            m_prim.fst = ((value >> 8) & 1) != 0;
            m_prim.ctxt = ((value >> 9) & 1) != 0;
            m_prim.fix = ((value >> 10) & 1) != 0;
        }
        break;
    case GS_REG_TEXCLUT:
        m_texclut.cbw = static_cast<uint8_t>(value & 0x3Fu);
        m_texclut.cou = static_cast<uint8_t>((value >> 6) & 0x3Fu);
        m_texclut.cov = static_cast<uint16_t>((value >> 12) & 0x3FFu);
        break;
    case GS_REG_SCISSOR_1:
    case GS_REG_SCISSOR_2:
    {
        int ci = (regAddr == GS_REG_SCISSOR_2) ? 1 : 0;
        m_ctx[ci].scissor.x0 = static_cast<uint16_t>(value & 0x7FF);
        m_ctx[ci].scissor.x1 = static_cast<uint16_t>((value >> 16) & 0x7FF);
        m_ctx[ci].scissor.y0 = static_cast<uint16_t>((value >> 32) & 0x7FF);
        m_ctx[ci].scissor.y1 = static_cast<uint16_t>((value >> 48) & 0x7FF);
        break;
    }
    case GS_REG_ALPHA_1:
    case GS_REG_ALPHA_2:
    {
        int ci = (regAddr == GS_REG_ALPHA_2) ? 1 : 0;
        m_ctx[ci].alpha = value;
        break;
    }
    case GS_REG_TEST_1:
    case GS_REG_TEST_2:
    {
        int ci = (regAddr == GS_REG_TEST_2) ? 1 : 0;
        m_ctx[ci].test = value;
        break;
    }
    case GS_REG_FRAME_1:
    case GS_REG_FRAME_2:
    {
        int ci = (regAddr == GS_REG_FRAME_2) ? 1 : 0;
        const uint32_t newFbp = static_cast<uint32_t>(value & 0x1FFu);
        if (m_presentAtFrameClearEnabled && m_processingGifPacket &&
            m_currentGifPacketPath == GifPathId::Path3 &&
            regAddr == GS_REG_FRAME_2 && m_ctx[ci].frame.fbp != newFbp)
        {
            GSFrameReg targetFrame{};
            targetFrame.fbp = newFbp;
            targetFrame.fbw = static_cast<uint32_t>((value >> 16) & 0x3Fu);
            targetFrame.psm = static_cast<uint8_t>((value >> 24) & 0x3Fu);
            targetFrame.fbmsk = static_cast<uint32_t>((value >> 32) & 0xFFFFFFFFu);
            // RRV_PRESENT_SCANOUT_GATE (default ON) — girl-scene flicker fix.
            // The attract girl cinematic ping-pongs FRAME_2 between the main
            // display pair (fbp 0/70, girl composited over the environment) and
            // an OFFSCREEN aux buffer (fbp 210) that renders the girl alone over
            // a black clear and is never assigned to DISPFB. Capturing that aux
            // buffer presents "girl on black", which alternates with the main
            // composite every present and reads as flicker. Ground truth
            // (local/gt/gt_A2_girl.gsr) confirms DISPFB2 only ever scans out
            // 0/70, never 210. Gate the capture on the target being a buffer the
            // display actually scans out; skip offscreen render targets, leaving
            // the last good main-composite present pinned. No-op outside the
            // girl scene, where every FRAME_2 flip already targets a display
            // buffer. Opt out with RRV_PRESENT_SCANOUT_GATE=0.
            //
            // NOTE (2026-07-24, measured): also requiring the OLD fbp to be a
            // scanout buffer — i.e. only accepting a genuine 0<->70 flip — is a
            // provable NO-OP here and does NOT fix the missing girl. Every
            // capture in the girl scene is already a clean 0<->70 flip, one per
            // game frame, 100% non-black (RRV_PRESENT_AT_FRAME_CLEAR_DIAG=1 over
            // presents 610-631). No 140/210 excursion ever reaches this gate.
            // The girl bug is which SIDE of the flip gets snapshotted; see
            // RRV_FRAMECLEAR_NEWEST in captureFrameClearPresentationUnlocked.
            static const bool s_scanoutGate = [] {
                const char *v = std::getenv("RRV_PRESENT_SCANOUT_GATE");
                return !v || (v[0] && v[0] != '0');
            }();
            if (!s_scanoutGate || isScanoutDisplayFbp(newFbp))
                captureFrameClearPresentationUnlocked(targetFrame, m_ctx[ci].frame);
        }
        m_ctx[ci].frame.fbp = static_cast<uint32_t>(value & 0x1FF);
        m_ctx[ci].frame.fbw = static_cast<uint32_t>((value >> 16) & 0x3F);
        m_ctx[ci].frame.psm = static_cast<uint8_t>((value >> 24) & 0x3F);
        m_ctx[ci].frame.fbmsk = static_cast<uint32_t>((value >> 32) & 0xFFFFFFFF);
        break;
    }
    case GS_REG_ZBUF_1:
    case GS_REG_ZBUF_2:
    {
        int ci = (regAddr == GS_REG_ZBUF_2) ? 1 : 0;
        m_ctx[ci].zbuf.zbp = value & 0x1FF;
        m_ctx[ci].zbuf.psm = ((value >> 24) & 0xF) | 0x30;
        m_ctx[ci].zbuf.zmask = (value >> 32) & 1;
        break;
    }
    case GS_REG_FBA_1:
    case GS_REG_FBA_2:
    {
        int ci = (regAddr == GS_REG_FBA_2) ? 1 : 0;
        m_ctx[ci].fba = value;
        break;
    }
    case GS_REG_BITBLTBUF:
    {
        m_bitbltbuf.sbp = static_cast<uint32_t>(value & 0x3FFF);
        m_bitbltbuf.sbw = static_cast<uint8_t>((value >> 16) & 0x3F);
        m_bitbltbuf.spsm = static_cast<uint8_t>((value >> 24) & 0x3F);
        m_bitbltbuf.dbp = static_cast<uint32_t>((value >> 32) & 0x3FFF);
        m_bitbltbuf.dbw = static_cast<uint8_t>((value >> 48) & 0x3F);
        m_bitbltbuf.dpsm = static_cast<uint8_t>((value >> 56) & 0x3F);
        break;
    }
    case GS_REG_TRXPOS:
    {
        m_trxpos.ssax = static_cast<uint16_t>(value & 0x7FF);
        m_trxpos.ssay = static_cast<uint16_t>((value >> 16) & 0x7FF);
        m_trxpos.dsax = static_cast<uint16_t>((value >> 32) & 0x7FF);
        m_trxpos.dsay = static_cast<uint16_t>((value >> 48) & 0x7FF);
        m_trxpos.dir = static_cast<uint8_t>((value >> 59) & 0x3);
        break;
    }
    case GS_REG_TRXREG:
    {
        m_trxreg.rrw = static_cast<uint16_t>(value & 0xFFF);
        m_trxreg.rrh = static_cast<uint16_t>((value >> 32) & 0xFFF);
        break;
    }
    case GS_REG_TRXDIR:
    {
        m_trxdir = static_cast<uint32_t>(value & 0x3);

        // We need the transfer state to survive the call to performLocalTo*Transfer
        // This is because transfers can be broken into multiple IMAGE tags and we
        // don't want to start all over again from the initial state
        // The transfer starts officially when TRXDIR is accessed
        m_transferState.x = m_trxpos.dsax;
        m_transferState.y = m_trxpos.dsay;
        m_transferState.total_pixels = m_trxreg.rrw * m_trxreg.rrh;
        m_transferState.copied_pixels = 0;

        if (m_trxdir == 2 && m_vram)
        {
            performLocalToLocalTransfer();
        }
        else if (m_trxdir == 1 && m_vram)
        {
            performLocalToHostToBuffer();
        }
        break;
    }
    case GS_REG_HWREG:
    {
        uint8_t buf[8];
        std::memcpy(buf, &value, 8);
        processImageData(buf, 8);
        break;
    }
    case GS_REG_PABE:
        m_pabe = (value & 1u) != 0u;
        break;
    case GS_REG_TEXFLUSH:
    case GS_REG_SCANMSK:
    case GS_REG_DIMX:
    case GS_REG_DTHE:
    case GS_REG_COLCLAMP:
        break;
    case GS_REG_FOGCOL:
        // Renderer-IR D0 gap-closure: FOGCOL.FCR[0:7]/FCG[8:15]/FCB[16:23]
        // (GIFReg FOGCOL layout; single register, not per-context). Previously
        // fell into the no-op case above and was never stored anywhere.
        m_fogcol = static_cast<uint32_t>(value & 0xFFFFFFu);
        break;
    case GS_REG_MIPTBP1_1:
    case GS_REG_MIPTBP1_2:
    {
        // Renderer-IR D0 gap-closure: MIPTBP1 (mip levels 1-3 base ptr/width)
        // was previously decoded then dropped (no-op case above). Stored raw,
        // same convention as m_ctx[ci].tex1/clamp; rrv_ir_builder.cpp decodes
        // TBP1[0:13]/TBW1[14:19]/TBP2[20:33]/TBW2[34:39]/TBP3[40:53]/TBW3[54:59].
        //
        // D0 investigation finding: this case is never hit on any of the six
        // clean GSR-derived corpus streams (verified with a temporary debug
        // counter, removed before landing) — GS_REG_MIPTBP1_1/1_2/2_1/2_2 are
        // NEVER written in that corpus, even on draws whose TEX1.MTBA==0
        // ("explicit MIPTBP" mode). The mip-chain fields therefore decode to
        // (0,0) everywhere in the offline catalogue. This is consistent with
        // MIPTBP being set once at texture-upload time — likely before the
        // trimmed capture window starts — rather than per-draw/per-frame like
        // TEX0/TEX1/CLAMP; it is a corpus-provenance limit (same class as the
        // documented vu1Pc/gifPath gaps in docs/RENDERER_IR.md), not evidence
        // the decode here is wrong. A live capture would settle it.
        int ci = (regAddr == GS_REG_MIPTBP1_2) ? 1 : 0;
        m_ctx[ci].miptbp1 = value;
        break;
    }
    case GS_REG_MIPTBP2_1:
    case GS_REG_MIPTBP2_2:
    {
        // Mip levels 4-6, same layout as MIPTBP1 (TBP4/TBW4/TBP5/TBW5/TBP6/TBW6).
        int ci = (regAddr == GS_REG_MIPTBP2_2) ? 1 : 0;
        m_ctx[ci].miptbp2 = value;
        break;
    }
    case GS_REG_TEXA:
    {
        m_texa.ta0 = static_cast<uint8_t>(value & 0xFFu);
        m_texa.aem = ((value >> 15) & 0x1u) != 0u;
        m_texa.ta1 = static_cast<uint8_t>((value >> 32) & 0xFFu);
        PS2_IF_AGRESSIVE_LOGS({
            const uint32_t texaIndex = s_debugTexaWriteCount.fetch_add(1u, std::memory_order_relaxed);
            if (texaIndex < 24u)
            {
                RUNTIME_LOG("[gs:texa] idx=" << texaIndex
                                             << " value=0x" << std::hex << value
                                             << " ta0=0x" << ((value >> 0) & 0xFFu)
                                             << " aem=" << ((value >> 15) & 0x1u)
                                             << " ta1=0x" << ((value >> 32) & 0xFFu)
                                             << std::dec
                                             << std::endl);
            }
        });
        break;
    }
    case GS_REG_SIGNAL:
    {
        if (m_privRegs)
        {
            uint32_t id = static_cast<uint32_t>(value & 0xFFFFFFFF);
            uint32_t mask = static_cast<uint32_t>(value >> 32);
            uint32_t lo = static_cast<uint32_t>(m_privRegs->siglblid & 0xFFFFFFFF);
            lo = (lo & ~mask) | (id & mask);
            m_privRegs->siglblid = (m_privRegs->siglblid & 0xFFFFFFFF00000000ULL) | lo;
            m_privRegs->csr |= 0x1; rrv::guest_time::QuietBump();
        }
        break;
    }
    case GS_REG_FINISH:
    {
        if (m_privRegs)
            m_privRegs->csr |= 0x2; rrv::guest_time::QuietBump();
        break;
    }
    case GS_REG_LABEL:
    {
        if (m_privRegs)
        {
            uint32_t id = static_cast<uint32_t>(value & 0xFFFFFFFF);
            uint32_t mask = static_cast<uint32_t>(value >> 32);
            uint32_t hi = static_cast<uint32_t>(m_privRegs->siglblid >> 32);
            hi = (hi & ~mask) | (id & mask);
            m_privRegs->siglblid = (static_cast<uint64_t>(hi) << 32) | (m_privRegs->siglblid & 0xFFFFFFFF);
        }
        break;
    }
    case 0x59:
        if (m_privRegs)
            m_privRegs->dispfb1 = value;
        break;
    case 0x5a:
        if (m_privRegs)
            m_privRegs->display1 = value;
        break;
    case 0x5b:
        if (m_privRegs)
            m_privRegs->dispfb2 = value;
        break;
    case 0x5c:
        if (m_privRegs)
            m_privRegs->display2 = value;
        break;
    case 0x5f:
        if (m_privRegs)
            m_privRegs->bgcolor = value;
        break;
    default:
        break;
    }
}

void GS::performLocalToLocalTransfer()
{
    if (!m_vram)
        return;

    const u32 sbp = m_bitbltbuf.sbp;
    const u8 sbw = m_bitbltbuf.sbw;
    const u8 spsm = m_bitbltbuf.spsm;
    const u32 dbp = m_bitbltbuf.dbp;
    const u8 dbw = m_bitbltbuf.dbw;
    const u8 dpsm = m_bitbltbuf.dpsm;
    const u32 rrw = m_trxreg.rrw;
    const u32 rrh = m_trxreg.rrh;
    const u32 ssax = m_trxpos.ssax;
    const u32 ssay = m_trxpos.ssay;
    const u32 dsax = m_trxpos.dsax;
    const u32 dsay = m_trxpos.dsay;
    const u32 dir = m_trxpos.dir;

    const u32 total_pixels = rrw * rrh;

    if (total_pixels == 0)
    {
        m_trxdir = 3;
        return;
    }

#if defined(_DEBUG)
    // RRV_RUNTIME_LOG: local-to-local xfer is a flush-forcing VRAM mutation; counted
    // for the tile-binning batch-size stats ([perf:batch]).
    ps2_diag::noteUpload();
#endif

    // Texture-payload sidecar tripwire (default off, RRV_IR_TEXTURE_BLOBS=1):
    // this path has no host bytes to hash, so it never produces an upload
    // event/sidecar blob -- see the hook's doc comment in rrv_ir_hooks.h.
#if !defined(PS2X_RRV_FIELD_ONLY)
    if (rrv::ir::rrv_ir_enabled())
    {
        rrv::ir::hookLocalToLocalObserved(dbp, dbw, dpsm, static_cast<uint16_t>(rrw), static_cast<uint16_t>(rrh));
    }
#endif

    // Tile-binning: flush deferred draws before this VRAM->VRAM copy (it reads and
    // writes VRAM regions a pending draw may depend on).
    if (GSRasterizer::tileEnabled())
        m_rasterizer.flushTiles(this);
#if !defined(PS2X_RRV_FIELD_ONLY)
    m_rasterizer.flushIR(this); // C1: same ordering guarantee for the IR-raster path
#endif
    // dir bit1 (dir&2) inverts X scan, bit0 (dir&1) inverts Y scan. The src->dst
    // pixel mapping is identical for all dirs; only scan order differs (for
    // overlapping copies). Nested loops avoid the per-pixel %/÷, and the
    // read/write funcs are resolved once outside the loop instead of indexing a
    // std::function array per pixel (that dispatch dominated profiles here).
    const bool invX = (dir & 2u) != 0u;
    const bool invY = (dir & 1u) != 0u;
    auto runCopy = [&](auto &&rd, auto &&wr)
    {
        for (u32 iy = 0; iy < rrh; ++iy)
        {
            const u32 y = invY ? (rrh - iy - 1u) : iy;
            const u32 sy = y + ssay;
            const u32 dy = y + dsay;
            for (u32 ix = 0; ix < rrw; ++ix)
            {
                const u32 x = invX ? (rrw - ix - 1u) : ix;
                wr(dbp, dbw, x + dsax, dy, rd(sbp, sbw, x + ssax, sy));
            }
        }
    };

    if (spsm == 0u && dpsm == 0u)
    {
        // PSMCT32 -> PSMCT32 fast path: call the swizzle helpers directly,
        // bypassing the std::function indirection (the common intro case).
        runCopy(
            [this](u32 b, u32 w, u32 px, u32 py) -> u32 { return GSMem::ReadCT32(m_vram, b, w, px, py); },
            [this](u32 b, u32 w, u32 px, u32 py, u32 v) { GSMem::WriteCT32(m_vram, b, w, px, py, v); });
    }
    else
    {
        const ReadVramFunc &rdFn = m_read_vram_funcs[spsm & 0x3F];
        const WriteVramFunc &wrFn = m_write_vram_funcs[dpsm & 0x3F];
        runCopy(
            [&](u32 b, u32 w, u32 px, u32 py) -> u32 { return rdFn(m_vram, b, w, px, py); },
            [&](u32 b, u32 w, u32 px, u32 py, u32 v) { wrFn(m_vram, b, w, px, py, v); });
    }

    m_trxdir = 3;
}

void GS::vertexKick(bool drawing)
{
    if (m_carGifActive)
    {
        const GSVertex &v = m_vtxQueue[m_vtxCount % kMaxVerts];
        const GSContext &c = activeContext();
        const float sx = v.x - static_cast<float>(c.xyoffset.ofx) / 16.0f;
        const float sy = v.y - static_cast<float>(c.xyoffset.ofy) / 16.0f;
        ++m_carGifVerts;
        if (drawing)
            ++m_carGifDrawing;
        if (!std::isfinite(sx) || !std::isfinite(sy))
        {
            ++m_carGifNonfinite;
        }
        else
        {
            if (!m_carGifBBoxInit)
            {
                m_carGifMinX = m_carGifMaxX = sx;
                m_carGifMinY = m_carGifMaxY = sy;
                m_carGifBBoxInit = true;
            }
            else
            {
                m_carGifMinX = std::min(m_carGifMinX, sx);
                m_carGifMinY = std::min(m_carGifMinY, sy);
                m_carGifMaxX = std::max(m_carGifMaxX, sx);
                m_carGifMaxY = std::max(m_carGifMaxY, sy);
            }
            // Generous margin around the 640x512 host frame; a car within view
            // lands well inside this. offscreen ⇒ clipped/translated away.
            if (sx < -128.f || sx > 768.f || sy < -128.f || sy > 640.f)
                ++m_carGifOffscreen;
            if (std::fabs(sx) > 8192.f || std::fabs(sy) > 8192.f)
                ++m_carGifExtreme;
        }
    }

    if (m_carDmaGifDiagActive && m_carDmaGifDiagCurrentWrite)
    {
        const GSVertex &vtx = m_vtxQueue[m_vtxCount % kMaxVerts];
        const GSContext &ctx = activeContext();
        const float screenX = vtx.x - static_cast<float>(ctx.xyoffset.ofx) / 16.0f;
        const float screenY = vtx.y - static_cast<float>(ctx.xyoffset.ofy) / 16.0f;
        if (!std::isfinite(screenX) || !std::isfinite(screenY))
        {
            ++m_carDmaGifDiagNonfinite;
        }
        else
        {
            if (m_carDmaGifDiagKicks == m_carDmaGifDiagNonfinite)
            {
                m_carDmaGifDiagMinX = m_carDmaGifDiagMaxX = screenX;
                m_carDmaGifDiagMinY = m_carDmaGifDiagMaxY = screenY;
            }
            else
            {
                m_carDmaGifDiagMinX = std::min(m_carDmaGifDiagMinX, screenX);
                m_carDmaGifDiagMinY = std::min(m_carDmaGifDiagMinY, screenY);
                m_carDmaGifDiagMaxX = std::max(m_carDmaGifDiagMaxX, screenX);
                m_carDmaGifDiagMaxY = std::max(m_carDmaGifDiagMaxY, screenY);
            }
            if (std::fabs(screenX) > 8192.0f || std::fabs(screenY) > 8192.0f)
                ++m_carDmaGifDiagExtreme;
        }
        ++m_carDmaGifDiagKicks;
        if (drawing)
        {
            ++m_carDmaGifDiagDrawingKicks;
        }
        else
        {
            ++m_carDmaGifDiagAdcKicks;
            const uint8_t source = m_carDmaGifDiagCurrentSourceDesc;
            const uint8_t target = m_carDmaGifDiagCurrentTargetReg;
            const char *cause = "UNKNOWN";
            if ((source == 0x04u || source == 0x05u) && m_carDmaGifDiagCurrentRawAdc)
            {
                ++m_carDmaGifDiagXyz2Adc;
                cause = (source == 0x04u) ? "XYZF2_ADC" : "XYZ2_ADC";
            }
            else if (source == 0x0cu || source == 0x0du)
            {
                ++m_carDmaGifDiagXyz3Descriptor;
                cause = (source == 0x0cu) ? "XYZF3_DESCRIPTOR" : "XYZ3_DESCRIPTOR";
            }
            else if (source == 0x0eu && (target == GS_REG_XYZF3 || target == GS_REG_XYZ3))
            {
                ++m_carDmaGifDiagAdXyz3;
                cause = (target == GS_REG_XYZF3) ? "AD_XYZF3" : "AD_XYZ3";
            }
            else
            {
                ++m_carDmaGifDiagUnknownNoDraw;
            }

            if (m_carDmaGifDiagCauseRecords < 8u)
            {
                const uint32_t rawPrim = static_cast<uint32_t>(m_prim.type) |
                                         (static_cast<uint32_t>(m_prim.iip) << 3) |
                                         (static_cast<uint32_t>(m_prim.tme) << 4) |
                                         (static_cast<uint32_t>(m_prim.fge) << 5) |
                                         (static_cast<uint32_t>(m_prim.abe) << 6) |
                                         (static_cast<uint32_t>(m_prim.aa1) << 7) |
                                         (static_cast<uint32_t>(m_prim.fst) << 8) |
                                         (static_cast<uint32_t>(m_prim.ctxt) << 9) |
                                         (static_cast<uint32_t>(m_prim.fix) << 10);
                std::fprintf(stderr,
                             "[car-dma:%llu] NODRAW index=%u byte=%u tag=%u phase=%u "
                             "source=%02x target=%02x raw-adc=%u cause=%s PRIM=%03x\n",
                             static_cast<unsigned long long>(m_carDmaGifDiagChain),
                             m_carDmaGifDiagKicks - 1u, m_carDmaGifDiagCurrentByteOffset,
                             m_carDmaGifDiagCurrentTagOffset,
                             static_cast<uint32_t>(m_carDmaGifDiagCurrentRegPhase), source,
                             target, m_carDmaGifDiagCurrentRawAdc ? 1u : 0u, cause,
                             rawPrim);
                ++m_carDmaGifDiagCauseRecords;
            }
        }
    }
    ++m_vtxCount;
    ++m_vtxIndex;

    PS2_IF_AGRESSIVE_LOGS({
        const uint32_t debugIndex = s_debugGsVertexKickCount.fetch_add(1, std::memory_order_relaxed);
        if (debugIndex < 96u)
        {
            RUNTIME_LOG("[gs:kick] idx=" << debugIndex
                                         << " drawing=" << static_cast<uint32_t>(drawing ? 1u : 0u)
                                         << " prim=" << static_cast<uint32_t>(m_prim.type)
                                         << " vtxCount=" << m_vtxCount
                                         << std::endl);
        }
    });

    int needed = 0;
    switch (m_prim.type)
    {
    case GS_PRIM_POINT:
        needed = 1;
        break;
    case GS_PRIM_LINE:
        needed = 2;
        break;
    case GS_PRIM_LINESTRIP:
        needed = 2;
        break;
    case GS_PRIM_TRIANGLE:
        needed = 3;
        break;
    case GS_PRIM_TRISTRIP:
        needed = 3;
        break;
    case GS_PRIM_TRIFAN:
        needed = 3;
        break;
    case GS_PRIM_SPRITE:
        needed = 2;
        break;
    default:
        return;
    }

    if (m_vtxCount < needed)
        return;

    // --- Milestone B1: Renderer-IR capture (write-only observer). A complete
    // primitive is assembled in m_vtxQueue[0..needed-1]. We marshal the decoded
    // shadow state (m_ctx/m_prim/m_cur*) into the thin hook PODs and record the
    // draw in strict submission order. Guarded by a single relaxed load so with
    // RRV_IR_CAPTURE unset this is one predictable branch + zero work. Records
    // ADC (drawing==false) kicks too — stream order is the transparency sort.
#if !defined(PS2X_RRV_FIELD_ONLY)
    if (rrv::ir::rrv_ir_enabled())
    {
        const GSContext &cx = activeContext();
        rrv::ir::HookVertex hv[6];
        // m_vtxQueue holds the assembled primitive; the last kick's ADC-ness
        // applies to this primitive's terminating vertex.
        for (int i = 0; i < needed && i < 6; ++i)
        {
            const GSVertex &sv = m_vtxQueue[i];
            rrv::ir::HookVertex &d = hv[i];
            d.x = sv.x; d.y = sv.y; d.z = sv.z; d.q = sv.q; // C1: z double (landmine #3)
            d.r = sv.r; d.g = sv.g; d.b = sv.b; d.a = sv.a;
            d.s = sv.s; d.t = sv.t; d.u = sv.u; d.v = sv.v;
            d.fog = sv.fog;
            d.adc = (i == needed - 1 && !drawing) ? 1u : 0u;
        }

        rrv::ir::HookRenderState rs{};
        rs.test = cx.test; rs.alpha = cx.alpha;
        rs.fbmsk = cx.frame.fbmsk; rs.zmask = cx.zbuf.zmask ? 1u : 0u;
        rs.abe = m_prim.abe ? 1u : 0u; rs.fge = m_prim.fge ? 1u : 0u;
        rs.pabe = m_pabe ? 1u : 0u; rs.iip = m_prim.iip ? 1u : 0u;
        rs.colclamp = 0u; // COLCLAMP not tracked in field-compatible producer
        rs.fba = static_cast<uint8_t>(cx.fba & 0x1ull); // C1: writePixel reads ctx.fba
        rs.fogcol = m_fogcol; // D0 gap-closure: FOGCOL (low 24 bits, FCR/FCG/FCB)

        rrv::ir::HookMaterial mat{};
        mat.tme = m_prim.tme ? 1u : 0u; mat.fst = m_prim.fst ? 1u : 0u;
        mat.tbp0 = cx.tex0.tbp0; mat.tbw = cx.tex0.tbw; mat.psm = cx.tex0.psm;
        mat.tw = cx.tex0.tw; mat.th = cx.tex0.th; mat.tcc = cx.tex0.tcc;
        mat.tfx = cx.tex0.tfx; mat.cbp = cx.tex0.cbp; mat.cpsm = cx.tex0.cpsm;
        mat.csm = cx.tex0.csm; mat.csa = cx.tex0.csa;
        mat.cbw = m_texclut.cbw; mat.cou = m_texclut.cou; mat.cov = m_texclut.cov; // C1: CLUT addr
        mat.clamp = cx.clamp; mat.tex1 = cx.tex1;
        mat.ta0 = m_texa.ta0; mat.aem = m_texa.aem ? 1u : 0u; mat.ta1 = m_texa.ta1;
        // D0 gap-closure: MIPTBP1/2 raw registers (mip chain base ptr/width).
        mat.miptbp1 = cx.miptbp1; mat.miptbp2 = cx.miptbp2;

        rrv::ir::HookTarget tgt{};
        tgt.fbp = cx.frame.fbp; tgt.fbw = cx.frame.fbw; tgt.fpsm = cx.frame.psm;
        tgt.zbp = cx.zbuf.zbp; tgt.zpsm = cx.zbuf.psm;
        tgt.sx0 = cx.scissor.x0; tgt.sx1 = cx.scissor.x1;
        tgt.sy0 = cx.scissor.y0; tgt.sy1 = cx.scissor.y1;
        tgt.ofx = cx.xyoffset.ofx; tgt.ofy = cx.xyoffset.ofy;
        tgt.ctxt = m_prim.ctxt ? 1u : 0u;

        // IR v5 bounded full-frame checkpoint: the target-only prefilter is
        // intentionally cheap.  Drain only prior deferred work so the hook
        // below copies exact physical VRAM immediately before THIS primitive
        // is recorded/rasterized; the stricter state/geometry profile lives
        // in the IR producer once it has the draw identity.
        if (rrv::ir::wantsFullFramePreDrawCheckpoint(drawing, tgt))
            flushPendingTiles();

        rrv::ir::hookDraw(static_cast<int>(m_prim.type), drawing,
                          hv, needed, rs, mat, tgt, 0u, 0u, m_vram, m_vramSize);
    }
#endif

    // An ADC vertex (XYZ3, "vertex kick without drawing") advances the vertex
    // queue exactly like XYZ2 — only the rasterization is skipped. Games rely
    // on this to draw disconnected segments within one LINESTRIP/TRISTRIP
    // (move with XYZ3, draw with XYZ2). Returning before the queue roll left
    // stale vertices in the window and rasterized the connecting "moves".
    //
    // Milestone C1: with RRV_IR_RASTER the pixels come from flushIR() consuming
    // the IR draw stream we just recorded via hookDraw above — so we skip the
    // live rasterizer entirely and only replicate the present-source bookkeeping
    // the draw path would have performed on the real gs.
#if !defined(PS2X_RRV_FIELD_ONLY)
    if (rrv::ir::rrv_ir_raster_enabled())
    {
        if (drawing)
            GSRasterizer::captureIRSideEffects(this);
    }
    else if (rrv::ir::rrv_ir_live_full_frame_decode_only())
    {
        // F7: PCSX2 owns guest-visible raster/field timing.  This local GS
        // instance only decodes the already-arbitrated packet stream into IR
        // and maintains the exact upload/local-transfer provenance consumed
        // by the live progressive adapter.  Do not execute a second software
        // raster pass here: that would both waste a full frame and create a
        // tempting-but-invalid field presentation source.
    }
#else
    if (drawing)
#endif
    {
        // RRV_WHITE_DRAW_DIAG (B-5, default off): dump the COMPLETE raw draw
        // context + vertices for the first few white-letter primitives
        // (ctx tex0.tbp0==7072). Run on both the native recording and the
        // PCSX2 ground-truth .gsr; diff the lines to find the first field the
        // IR-level comparison missed.
        static int s_whiteDrawDiag = -1;
        if (s_whiteDrawDiag < 0)
        {
            const char *v = std::getenv("RRV_WHITE_DRAW_DIAG");
            s_whiteDrawDiag = (v && v[0] && v[0] != '0') ? 1 : 0;
        }
        if (s_whiteDrawDiag)
        {
            const GSContext &cx = activeContext();
            if (cx.tex0.tbp0 == 7072u && m_prim.tme)
            {
                static std::atomic<uint32_t> s_whiteDrawCount{0u};
                const uint32_t n = s_whiteDrawCount.fetch_add(1u);
                if (n < 6u)
                {
                    std::fprintf(stderr,
                                 "[whitedraw:%u] prim=%u iip=%u tme=%u fst=%u ctxt=%u abe=%u fge=%u fix=%u\n",
                                 n, static_cast<uint32_t>(m_prim.type), m_prim.iip, m_prim.tme,
                                 m_prim.fst, m_prim.ctxt, m_prim.abe, m_prim.fge, m_prim.fix);
                    std::fprintf(stderr,
                                 "[whitedraw:%u] frame fbp=%u fbw=%u psm=%u fbmsk=%08x zbuf zbp=%u psm=%u zmsk=%u\n",
                                 n, cx.frame.fbp, cx.frame.fbw, cx.frame.psm, cx.frame.fbmsk,
                                 cx.zbuf.zbp, cx.zbuf.psm, cx.zbuf.zmask ? 1u : 0u);
                    std::fprintf(stderr,
                                 "[whitedraw:%u] test=%016llx alpha=%016llx scissor=[%u,%u..%u,%u] ofx=%u ofy=%u fba=%llu\n",
                                 n, static_cast<unsigned long long>(cx.test),
                                 static_cast<unsigned long long>(cx.alpha),
                                 cx.scissor.x0, cx.scissor.y0, cx.scissor.x1, cx.scissor.y1,
                                 static_cast<uint32_t>(cx.xyoffset.ofx),
                                 static_cast<uint32_t>(cx.xyoffset.ofy),
                                 static_cast<unsigned long long>(cx.fba));
                    std::fprintf(stderr,
                                 "[whitedraw:%u] tex0 tbp=%u tbw=%u psm=%u tw=%u th=%u tcc=%u tfx=%u cbp=%u cpsm=%u csm=%u csa=%u clamp=%016llx tex1=%016llx texa ta0=%u aem=%u ta1=%u\n",
                                 n, cx.tex0.tbp0, cx.tex0.tbw, cx.tex0.psm, cx.tex0.tw, cx.tex0.th,
                                 cx.tex0.tcc, cx.tex0.tfx, cx.tex0.cbp, cx.tex0.cpsm, cx.tex0.csm,
                                 cx.tex0.csa, static_cast<unsigned long long>(cx.clamp),
                                 static_cast<unsigned long long>(cx.tex1),
                                 m_texa.ta0, m_texa.aem ? 1u : 0u, m_texa.ta1);
                    for (int i = 0; i < needed && i < 3; ++i)
                    {
                        const GSVertex &sv = m_vtxQueue[i];
                        std::fprintf(stderr,
                                     "[whitedraw:%u] v%d x=%.4f y=%.4f z=%.1f s=%.6f t=%.6f q=%.6f u=%u vv=%u rgba=%u,%u,%u,%u fog=%u\n",
                                     n, i, sv.x, sv.y, sv.z, sv.s, sv.t, sv.q, sv.u, sv.v,
                                     sv.r, sv.g, sv.b, sv.a, sv.fog);
                    }
                }
            }
        }
        m_rasterizer.drawPrimitive(this);
    }

    switch (m_prim.type)
    {
    case GS_PRIM_LINE:
    case GS_PRIM_TRIANGLE:
    case GS_PRIM_SPRITE:
    case GS_PRIM_POINT:
        m_vtxCount = 0;
        break;
    case GS_PRIM_LINESTRIP:
        m_vtxQueue[0] = m_vtxQueue[1];
        m_vtxCount = 1;
        break;
    case GS_PRIM_TRISTRIP:
        m_vtxQueue[0] = m_vtxQueue[1];
        m_vtxQueue[1] = m_vtxQueue[2];
        m_vtxCount = 2;
        break;
    case GS_PRIM_TRIFAN:
        m_vtxQueue[1] = m_vtxQueue[2];
        m_vtxCount = 2;
        break;
    default:
        m_vtxCount = 0;
        break;
    }
}

void GS::processImageData(const uint8_t *data, uint32_t sizeBytes)
{
    // wrong direction set
    if (m_trxdir != 0 || !m_vram)
    {
        return;
    }

    // no height and width means transfer is invalid
    if (m_trxreg.rrw == 0 || m_trxreg.rrh == 0)
    {
        return;
    }

#if defined(_DEBUG)
    // RRV_RUNTIME_LOG: host->local upload is a flush-forcing VRAM mutation; counted
    // for the tile-binning batch-size stats ([perf:batch]).
    ps2_diag::noteUpload();
#endif

    // --- Milestone B1: Renderer-IR resource-upload EVENT. Target provenance +
    // a cheap FNV-1a content hash of this chunk's transferred bytes. No decoding,
    // no host texture (that is Layer C). The IR builder uses the transfer cursor
    // reset by TRXDIR to distinguish one transfer's IMAGE continuations from a
    // back-to-back transfer start, independent of intervening draws.
#if !defined(PS2X_RRV_FIELD_ONLY)
    if (rrv::ir::rrv_ir_enabled())
    {
        rrv::ir::HookUpload up{};
        up.dbp = m_bitbltbuf.dbp; up.dbw = m_bitbltbuf.dbw; up.dpsm = m_bitbltbuf.dpsm;
        up.dsax = m_trxpos.dsax; up.dsay = m_trxpos.dsay;
        up.rrw = m_trxreg.rrw; up.rrh = m_trxreg.rrh;
        up.byteLen = sizeBytes;
        up.contentHash = rrv::ir::rrv_ir_fnv1a(data, sizeBytes);
        // `data`/`sizeBytes` are also handed to hookUpload (in addition to the
        // already-computed up.contentHash/up.byteLen): the OPTIONAL
        // texture-payload sidecar (default off, RRV_IR_TEXTURE_BLOBS=1) needs
        // the raw bytes, and must apply the SAME chunk-folding decision this
        // call already makes (the IMAGE chunks within one TRXDIR transfer
        // collapse into ONE IrDiskUpload event with an XOR-folded
        // contentHash) -- so the fold has to happen exactly once, here, not
        // duplicated at a second call site. See rrv_ir_hooks.h.
        rrv::ir::hookUpload(up, 3u /*PATH3 GIF DMA*/, data, sizeBytes);
    }
#endif

    // Tile-binning: an upload may overwrite VRAM a deferred draw samples/reads, so
    // render everything queued so far before mutating VRAM (preserves ordering).
    if (GSRasterizer::tileEnabled())
        m_rasterizer.flushTiles(this);
#if !defined(PS2X_RRV_FIELD_ONLY)
    m_rasterizer.flushIR(this); // C1: same ordering guarantee for the IR-raster path
#endif
    u32 dbp = m_bitbltbuf.dbp;
    u8 dbw = std::max<u8>(m_bitbltbuf.dbw, 1u);
    u8 dpsm = m_bitbltbuf.dpsm;

    u32 rrw = m_trxreg.rrw;
    u32 rrh = m_trxreg.rrh;
    u32 dsax = m_trxpos.dsax;
    u32 dsay = m_trxpos.dsay;

    u32 data_offset = 0;

    // remove the format branching from the loops
    // TODO: fixup copypasta
    switch (dpsm)
    {
    case GS_PSM_CT32:
        while (data_offset < sizeBytes)
        {
            u32 c;
            std::memcpy(&c, &data[data_offset], sizeof(u32));

            GSMem::WriteCT32(m_vram, dbp, dbw, m_transferState.x, m_transferState.y, c);

            m_transferState.x++;
            m_transferState.copied_pixels++;
            data_offset += 4;

            if ((m_transferState.copied_pixels % rrw) == 0)
            {
                m_transferState.x = dsax;
                m_transferState.y++;
            }

            if (m_transferState.copied_pixels >= m_transferState.total_pixels)
            {
                // deactivate the transfer
                m_trxdir = 3;
                m_transferState.total_pixels = 0;
                break;
            }
        }
        break;

    case GS_PSM_Z32:
        while (data_offset < sizeBytes)
        {
            u32 c;
            std::memcpy(&c, &data[data_offset], sizeof(u32));

            GSMem::WriteZ32(m_vram, dbp, dbw, m_transferState.x, m_transferState.y, c);

            m_transferState.x++;
            m_transferState.copied_pixels++;
            data_offset += 4;

            if ((m_transferState.copied_pixels % rrw) == 0)
            {
                m_transferState.x = dsax;
                m_transferState.y++;
            }

            if (m_transferState.copied_pixels >= m_transferState.total_pixels)
            {
                // deactivate the transfer
                m_trxdir = 3;
                m_transferState.total_pixels = 0;
                break;
            }
        }
        break;

    case GS_PSM_CT24:
        while (data_offset < sizeBytes)
        {
            u32 c;
            std::memcpy(&c, &data[data_offset], sizeof(u32));

            GSMem::WriteCT24(m_vram, dbp, dbw, m_transferState.x, m_transferState.y, c);

            m_transferState.x++;
            m_transferState.copied_pixels++;
            data_offset += 3;

            if ((m_transferState.copied_pixels % rrw) == 0)
            {
                m_transferState.x = dsax;
                m_transferState.y++;
            }

            if (m_transferState.copied_pixels >= m_transferState.total_pixels)
            {
                // deactivate the transfer
                m_trxdir = 3;
                m_transferState.total_pixels = 0;
                break;
            }
        }
        break;

    case GS_PSM_Z24:
        while (data_offset < sizeBytes)
        {
            u32 c;
            std::memcpy(&c, &data[data_offset], sizeof(u32));

            GSMem::WriteZ24(m_vram, dbp, dbw, m_transferState.x, m_transferState.y, c);

            m_transferState.x++;
            m_transferState.copied_pixels++;
            data_offset += 3;

            if ((m_transferState.copied_pixels % rrw) == 0)
            {
                m_transferState.x = dsax;
                m_transferState.y++;
            }

            if (m_transferState.copied_pixels >= m_transferState.total_pixels)
            {
                // deactivate the transfer
                m_trxdir = 3;
                m_transferState.total_pixels = 0;
                break;
            }
        }
        break;

    case GS_PSM_CT16:
        while (data_offset < sizeBytes)
        {
            u16 c;
            std::memcpy(&c, &data[data_offset], sizeof(u16));

            GSMem::WriteCT16(m_vram, dbp, dbw, m_transferState.x, m_transferState.y, c);

            m_transferState.x++;
            m_transferState.copied_pixels++;
            data_offset += 2;

            if ((m_transferState.copied_pixels % rrw) == 0)
            {
                m_transferState.x = dsax;
                m_transferState.y++;
            }

            if (m_transferState.copied_pixels >= m_transferState.total_pixels)
            {
                // deactivate the transfer
                m_trxdir = 3;
                m_transferState.total_pixels = 0;
                break;
            }
        }
        break;

    case GS_PSM_Z16:
        while (data_offset < sizeBytes)
        {
            u16 c;
            std::memcpy(&c, &data[data_offset], sizeof(u16));

            GSMem::WriteZ16(m_vram, dbp, dbw, m_transferState.x, m_transferState.y, c);

            m_transferState.x++;
            m_transferState.copied_pixels++;
            data_offset += 2;

            if ((m_transferState.copied_pixels % rrw) == 0)
            {
                m_transferState.x = dsax;
                m_transferState.y++;
            }

            if (m_transferState.copied_pixels >= m_transferState.total_pixels)
            {
                // deactivate the transfer
                m_trxdir = 3;
                m_transferState.total_pixels = 0;
                break;
            }
        }
        break;

    case GS_PSM_CT16S:
        while (data_offset < sizeBytes)
        {
            u16 c;
            std::memcpy(&c, &data[data_offset], sizeof(u16));

            GSMem::WriteCT16S(m_vram, dbp, dbw, m_transferState.x, m_transferState.y, c);

            m_transferState.x++;
            m_transferState.copied_pixels++;
            data_offset += 2;

            if ((m_transferState.copied_pixels % rrw) == 0)
            {
                m_transferState.x = dsax;
                m_transferState.y++;
            }

            if (m_transferState.copied_pixels >= m_transferState.total_pixels)
            {
                // deactivate the transfer
                m_trxdir = 3;
                m_transferState.total_pixels = 0;
                break;
            }
        }
        break;

    case GS_PSM_Z16S:
        while (data_offset < sizeBytes)
        {
            u16 c;
            std::memcpy(&c, &data[data_offset], sizeof(u16));

            GSMem::WriteZ16S(m_vram, dbp, dbw, m_transferState.x, m_transferState.y, c);

            m_transferState.x++;
            m_transferState.copied_pixels++;
            data_offset += 2;

            if ((m_transferState.copied_pixels % rrw) == 0)
            {
                m_transferState.x = dsax;
                m_transferState.y++;
            }

            if (m_transferState.copied_pixels >= m_transferState.total_pixels)
            {
                // deactivate the transfer
                m_trxdir = 3;
                m_transferState.total_pixels = 0;
                break;
            }
        }
        break;

    case GS_PSM_T8:
        while (data_offset < sizeBytes)
        {
            u8 c = data[data_offset];

            GSMem::WriteP8(m_vram, dbp, dbw, m_transferState.x, m_transferState.y, c);

            m_transferState.x++;
            m_transferState.copied_pixels++;
            data_offset += 1;

            if ((m_transferState.copied_pixels % rrw) == 0)
            {
                m_transferState.x = dsax;
                m_transferState.y++;
            }

            if (m_transferState.copied_pixels >= m_transferState.total_pixels)
            {
                // deactivate the transfer
                m_trxdir = 3;
                m_transferState.total_pixels = 0;
                break;
            }
        }
        break;

    case GS_PSM_T8H:
        while (data_offset < sizeBytes)
        {
            u8 c = data[data_offset];

            GSMem::WriteP8H(m_vram, dbp, dbw, m_transferState.x, m_transferState.y, c);

            m_transferState.x++;
            m_transferState.copied_pixels++;
            data_offset += 1;

            if ((m_transferState.copied_pixels % rrw) == 0)
            {
                m_transferState.x = dsax;
                m_transferState.y++;
            }

            if (m_transferState.copied_pixels >= m_transferState.total_pixels)
            {
                // deactivate the transfer
                m_trxdir = 3;
                m_transferState.total_pixels = 0;
                break;
            }
        }
        break;
    case GS_PSM_T4:
        while (data_offset < sizeBytes)
        {
            u8 c0 = data[data_offset] & 0xF;
            u8 c1 = (data[data_offset] >> 4) & 0xF;

            GSMem::WriteP4(m_vram, dbp, dbw, m_transferState.x, m_transferState.y, c0);
            GSMem::WriteP4(m_vram, dbp, dbw, m_transferState.x + 1, m_transferState.y, c1);

            m_transferState.x += 2;
            m_transferState.copied_pixels += 2;
            data_offset += 1;

            if ((m_transferState.copied_pixels % rrw) == 0)
            {
                m_transferState.x = dsax;
                m_transferState.y++;
            }

            if (m_transferState.copied_pixels >= m_transferState.total_pixels)
            {
                // deactivate the transfer
                m_trxdir = 3;
                m_transferState.total_pixels = 0;
                break;
            }
        }
        break;
    case GS_PSM_T4HL:
        while (data_offset < sizeBytes)
        {
            u8 c0 = data[data_offset] & 0xF;
            u8 c1 = (data[data_offset] >> 4) & 0xF;

            GSMem::WriteP4HL(m_vram, dbp, dbw, m_transferState.x, m_transferState.y, c0);
            GSMem::WriteP4HL(m_vram, dbp, dbw, m_transferState.x + 1, m_transferState.y, c1);

            m_transferState.x += 2;
            m_transferState.copied_pixels += 2;
            data_offset += 1;

            if ((m_transferState.copied_pixels % rrw) == 0)
            {
                m_transferState.x = dsax;
                m_transferState.y++;
            }

            if (m_transferState.copied_pixels >= m_transferState.total_pixels)
            {
                // deactivate the transfer
                m_trxdir = 3;
                m_transferState.total_pixels = 0;
                break;
            }
        }
        break;
    case GS_PSM_T4HH:
        while (data_offset < sizeBytes)
        {
            u8 c0 = data[data_offset] & 0xF;
            u8 c1 = (data[data_offset] >> 4) & 0xF;

            GSMem::WriteP4HH(m_vram, dbp, dbw, m_transferState.x, m_transferState.y, c0);
            GSMem::WriteP4HH(m_vram, dbp, dbw, m_transferState.x + 1, m_transferState.y, c1);

            m_transferState.x += 2;
            m_transferState.copied_pixels += 2;
            data_offset += 1;

            if ((m_transferState.copied_pixels % rrw) == 0)
            {
                m_transferState.x = dsax;
                m_transferState.y++;
            }

            if (m_transferState.copied_pixels >= m_transferState.total_pixels)
            {
                // deactivate the transfer
                m_trxdir = 3;
                m_transferState.total_pixels = 0;
                break;
            }
        }
        break;
    }

    // RRV_CLUT_PROBE: B-3 girl-CLUT investigation. After every completed
    // host->local IMAGE chunk, dump the transfer's BITBLTBUF/TRXPOS/TRXREG
    // plus a before/after summary of the 1024-byte CLUT slots at VRAM blocks
    // 8620, 8596 and 8960 (byte offset block*256). Fires after EVERY chunk
    // (each CLUT's own upload AND the later PSMT8 atlas upload), so grepping
    // the sequence gives a direct before/after picture without needing a
    // separate VRAM dump. Cheap, stderr-only, fully inert unless
    // RRV_CLUT_PROBE=1. Not for general use -- narrowly scoped debug aid.
    {
        static int s_clutProbe = -1;
        if (s_clutProbe < 0)
        {
            const char *v = std::getenv("RRV_CLUT_PROBE");
            s_clutProbe = (v && v[0] && v[0] != '0') ? 1 : 0;
        }
        if (s_clutProbe && m_vram)
        {
            // Real girl CLUT blocks per the aligned dump (TEX0 cbp=6976..6996,
            // PSMCT32). The girl's PSMT8 atlas uploads at dbp=6784 dbw=8; watch
            // whether that upload's footprint clobbers these CLUT bytes.
            static constexpr u32 kProbeBlocks[3] = {6976u, 6984u, 6996u};
            static constexpr u32 kProbeBytes = 1024u;

            fprintf(stderr,
                    "[clutprobe] dbp=%u dbw=%u dpsm=0x%02x dsax=%u dsay=%u wxh=%ux%u bytes=%u\n",
                    dbp, dbw, dpsm, dsax, dsay, rrw, rrh, sizeBytes);

            for (u32 bi = 0; bi < 3; ++bi)
            {
                const u32 kProbeOffset = kProbeBlocks[bi] * 256u;
                u32 laneNonZero[4] = {0, 0, 0, 0};
                uint64_t fnv = 0xcbf29ce484222325ULL;
                for (u32 i = 0; i < kProbeBytes; i += 4)
                {
                    for (u32 lane = 0; lane < 4; ++lane)
                    {
                        u8 b = m_vram[kProbeOffset + i + lane];
                        if (b != 0)
                            laneNonZero[lane]++;
                        fnv ^= b;
                        fnv *= 0x100000001b3ULL;
                    }
                }

                // Average RGB of the 256 PSMCT32 palette entries (linear VRAM
                // read; block bytes are the palette storage for a 16x16 CLUT).
                uint64_t sr = 0, sg = 0, sb = 0, sa = 0;
                for (u32 e = 0; e < 256; ++e)
                {
                    sr += m_vram[kProbeOffset + e * 4 + 0];
                    sg += m_vram[kProbeOffset + e * 4 + 1];
                    sb += m_vram[kProbeOffset + e * 4 + 2];
                    sa += m_vram[kProbeOffset + e * 4 + 3];
                }
                fprintf(stderr,
                        "  [clutprobe]   blk%u nz=%u/%u/%u/%u avgRGBA=[%llu,%llu,%llu,%llu] hash=%016llx\n",
                        kProbeBlocks[bi],
                        laneNonZero[0], laneNonZero[1], laneNonZero[2], laneNonZero[3],
                        (unsigned long long)(sr/256),(unsigned long long)(sg/256),
                        (unsigned long long)(sb/256),(unsigned long long)(sa/256),
                        static_cast<unsigned long long>(fnv));
            }
        }
    }
}

void GS::performLocalToHostToBuffer()
{
    // Tile-binning: this reads VRAM back to the host, so render deferred draws first.
    if (GSRasterizer::tileEnabled())
        m_rasterizer.flushTiles(this);
#if !defined(PS2X_RRV_FIELD_ONLY)
    m_rasterizer.flushIR(this); // C1: same ordering guarantee for the IR-raster path
#endif

    m_localToHostBuffer.clear();
    m_localToHostReadPos = 0;

    if (!m_vram)
        return;

    uint32_t sbp = m_bitbltbuf.sbp;
    uint8_t sbw = std::max<u8>(m_bitbltbuf.sbw, 1u);
    uint8_t spsm = m_bitbltbuf.spsm;
    uint32_t rrw = m_trxreg.rrw;
    uint32_t rrh = m_trxreg.rrh;
    uint32_t ssax = m_trxpos.ssax;
    uint32_t ssay = m_trxpos.ssay;

    u32 bpp = GSMem::BitsPerPixel(static_cast<GSMem::PixelStorageMode>(spsm));

    u32 pixel_total = rrw * rrh;
    u32 bytes_total = (pixel_total * bpp) / 8;

    m_localToHostBuffer.reserve(bytes_total);

    u32 pixel_count = 0;
    while (pixel_count < pixel_total)
    {
        const u32 x = pixel_count % rrw;
        const u32 y = pixel_count / rrw;

        const u32 v = ReadVram(spsm, sbp, sbw, x + ssax, y + ssay);

        switch (bpp)
        {
        case 32:
            m_localToHostBuffer.push_back(v & 0xFF);
            m_localToHostBuffer.push_back((v >> 8) & 0xFF);
            m_localToHostBuffer.push_back((v >> 16) & 0xFF);
            m_localToHostBuffer.push_back((v >> 24) & 0xFF);
            break;
        case 24:
            m_localToHostBuffer.push_back(v & 0xFF);
            m_localToHostBuffer.push_back((v >> 8) & 0xFF);
            m_localToHostBuffer.push_back((v >> 16) & 0xFF);
            break;
        case 16:
            m_localToHostBuffer.push_back(v & 0xFF);
            m_localToHostBuffer.push_back((v >> 8) & 0xFF);
            break;
        case 8:
            m_localToHostBuffer.push_back(v);
            break;
        case 4:
        {
            const u32 v2 = ReadVram(spsm, sbp, sbw, x + ssax + 1, y + ssay);

            m_localToHostBuffer.push_back(v | ((v2 & 0xF) << 4));
            pixel_count++;
            break;
        }
        default:
            break;
        }

        pixel_count++;
    }
}

bool GS::clearFramebufferContext(uint32_t contextIndex, uint32_t rgba)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    return clearFramebufferRect(this, m_ctx[(contextIndex != 0u) ? 1 : 0], rgba);
}

bool GS::clearActiveFramebuffer(uint32_t rgba)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    return clearFramebufferRect(this, activeContext(), rgba);
}

uint32_t GS::consumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes)
{
    std::lock_guard<std::recursive_mutex> lock(m_stateMutex);
    if (!dst || maxBytes == 0)
        return 0;
    size_t avail = m_localToHostBuffer.size() - m_localToHostReadPos;
    if (avail == 0)
        return 0;
    size_t toCopy = (avail < maxBytes) ? avail : static_cast<size_t>(maxBytes);
    std::memcpy(dst, m_localToHostBuffer.data() + m_localToHostReadPos, toCopy);
    m_localToHostReadPos += toCopy;
    return static_cast<uint32_t>(toCopy);
}
