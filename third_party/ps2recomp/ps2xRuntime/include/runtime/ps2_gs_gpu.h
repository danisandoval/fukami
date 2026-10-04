#ifndef PS2_GS_GPU_H
#define PS2_GS_GPU_H

#include <functional>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>
#include <memory>
#include <array>

#include "ps2_gs_rasterizer.h"
#include "ps2_gs_memory.h"
#include "ps2_gif_arbiter.h"

enum GSPrimType : uint8_t
{
    GS_PRIM_POINT = 0,
    GS_PRIM_LINE = 1,
    GS_PRIM_LINESTRIP = 2,
    GS_PRIM_TRIANGLE = 3,
    GS_PRIM_TRISTRIP = 4,
    GS_PRIM_TRIFAN = 5,
    GS_PRIM_SPRITE = 6,
};

enum GSPsm : uint8_t
{
    GS_PSM_CT32 = 0,
    GS_PSM_CT24 = 1,
    GS_PSM_CT16 = 2,
    GS_PSM_CT16S = 10,
    GS_PSM_T8 = 19,
    GS_PSM_T4 = 20,
    GS_PSM_T8H = 27,
    GS_PSM_T4HL = 36,
    GS_PSM_T4HH = 44,
    GS_PSM_Z32 = 48,
    GS_PSM_Z24 = 49,
    GS_PSM_Z16 = 50,
    GS_PSM_Z16S = 58,
};

enum GSGifFormat : uint8_t
{
    GIF_FMT_PACKED = 0,
    GIF_FMT_REGLIST = 1,
    GIF_FMT_IMAGE = 2,
    GIF_FMT_DISABLED = 3,
};

enum GSRegId : uint8_t
{
    GS_REG_PRIM = 0x00,
    GS_REG_RGBAQ = 0x01,
    GS_REG_ST = 0x02,
    GS_REG_UV = 0x03,
    GS_REG_XYZF2 = 0x04,
    GS_REG_XYZ2 = 0x05,
    GS_REG_TEX0_1 = 0x06,
    GS_REG_TEX0_2 = 0x07,
    GS_REG_CLAMP_1 = 0x08,
    GS_REG_CLAMP_2 = 0x09,
    GS_REG_FOG = 0x0A,
    GS_REG_XYZF3 = 0x0C,
    GS_REG_XYZ3 = 0x0D,
    GS_REG_AD = 0x0F,

    GS_REG_TEX1_1 = 0x14,
    GS_REG_TEX1_2 = 0x15,
    GS_REG_TEX2_1 = 0x16,
    GS_REG_TEX2_2 = 0x17,
    GS_REG_XYOFFSET_1 = 0x18,
    GS_REG_XYOFFSET_2 = 0x19,
    GS_REG_PRMODECONT = 0x1A,
    GS_REG_PRMODE = 0x1B,
    GS_REG_TEXCLUT = 0x1C,
    GS_REG_SCANMSK = 0x22,
    GS_REG_MIPTBP1_1 = 0x34,
    GS_REG_MIPTBP1_2 = 0x35,
    GS_REG_MIPTBP2_1 = 0x36,
    GS_REG_MIPTBP2_2 = 0x37,
    GS_REG_TEXA = 0x3B,
    GS_REG_FOGCOL = 0x3D,
    GS_REG_TEXFLUSH = 0x3F,
    GS_REG_SCISSOR_1 = 0x40,
    GS_REG_SCISSOR_2 = 0x41,
    GS_REG_ALPHA_1 = 0x42,
    GS_REG_ALPHA_2 = 0x43,
    GS_REG_DIMX = 0x44,
    GS_REG_DTHE = 0x45,
    GS_REG_COLCLAMP = 0x46,
    GS_REG_TEST_1 = 0x47,
    GS_REG_TEST_2 = 0x48,
    GS_REG_PABE = 0x49,
    GS_REG_FBA_1 = 0x4A,
    GS_REG_FBA_2 = 0x4B,
    GS_REG_FRAME_1 = 0x4C,
    GS_REG_FRAME_2 = 0x4D,
    GS_REG_ZBUF_1 = 0x4E,
    GS_REG_ZBUF_2 = 0x4F,
    GS_REG_BITBLTBUF = 0x50,
    GS_REG_TRXPOS = 0x51,
    GS_REG_TRXREG = 0x52,
    GS_REG_TRXDIR = 0x53,
    GS_REG_HWREG = 0x54,
    GS_REG_SIGNAL = 0x60,
    GS_REG_FINISH = 0x61,
    GS_REG_LABEL = 0x62,
};

struct GSVertex
{
    float x, y;
    // double because float isnt accurate enough for values near UINT32_MAX
    double z;
    uint8_t r, g, b, a;
    float q;
    float s, t;
    uint16_t u, v;
    uint8_t fog;
};

struct GSFrameReg
{
    uint32_t fbp;
    uint32_t fbw;
    uint8_t psm;
    uint32_t fbmsk;
};

struct GSZbufReg
{
    u32 zbp;
    u8 psm;
    bool zmask;
};

struct GSScissorReg
{
    uint16_t x0, x1, y0, y1;
};

struct GSTex0Reg
{
    uint32_t tbp0;
    uint8_t tbw;
    uint8_t psm;
    uint8_t tw;
    uint8_t th;
    uint8_t tcc;
    uint8_t tfx;
    uint32_t cbp;
    uint8_t cpsm;
    uint8_t csm;
    uint8_t csa;
    uint8_t cld;

    // RRV_GS_CLUT_CACHE (default OFF): the palette this context most recently
    // *latched* (per CLD semantics) at TEX0/TEX2 write time, decoded to RGBA8888
    // and indexed by raw (pre-swizzle) texel value 0..255. Null when the cache
    // is disabled or nothing has latched yet, in which case lookupCLUT() falls
    // back to its old live-VRAM sampling behaviour. Deliberately a shared_ptr
    // (not an inline array): GSContext (and therefore this struct) is copied
    // by value into tile-binning's TileCmd snapshot (see captureTile() in
    // ps2_gs_rasterizer.cpp) at primitive-submission time, so the copy that a
    // deferred/binned draw replays from is the exact buffer latched when that
    // primitive was submitted, not whatever VRAM holds at flush time -- this
    // is the fix for the CLUT/VRAM-aliasing corruption in the girl scene
    // (B-3). A shared_ptr copy is a cheap refcount bump, not a 1KB memcpy.
    std::shared_ptr<std::array<uint32_t, 256>> clutCache;
};

struct GSXYOffsetReg
{
    uint16_t ofx;
    uint16_t ofy;
};

struct GSTexaReg
{
    uint8_t ta0;
    bool aem;
    uint8_t ta1;
};

struct GSTexClutReg
{
    uint8_t cbw;
    uint8_t cou;
    uint16_t cov;
};

struct GSContext
{
    GSFrameReg frame;
    GSScissorReg scissor;
    GSTex0Reg tex0;
    GSXYOffsetReg xyoffset;
    GSZbufReg zbuf;
    uint64_t tex1;
    uint64_t clamp;
    uint64_t alpha;
    uint64_t test;
    uint64_t fba;
    // Renderer-IR D0 gap-closure: MIPTBP1_x/MIPTBP2_x were previously decoded
    // (GS_REG_MIPTBP1_1/1_2/2_1/2_2 in ps2_gs_gpu.cpp's writeRegisterUnlocked)
    // but discarded -- the register write landed in a shared no-op case with
    // TEXFLUSH/SCANMSK/DIMX/DTHE/COLCLAMP. Stored raw here (same convention as
    // tex1/clamp above) so the Renderer-IR capture hook (rrv_ir_hooks.h
    // HookMaterial::miptbp1/miptbp2) can decode the explicit mip-chain base
    // pointers/widths. Per-context, like tex1/clamp, because MIPTBP1_1 vs
    // MIPTBP1_2 select GS context 0/1 exactly like TEX1_1/TEX1_2.
    uint64_t miptbp1 = 0;
    uint64_t miptbp2 = 0;
};

struct GSPrimReg
{
    GSPrimType type;
    bool iip;
    bool tme;
    bool fge;
    bool abe;
    bool aa1;
    bool fst;
    bool ctxt;
    bool fix;
};

struct GSBitBltBuf
{
    uint32_t sbp;
    uint8_t sbw;
    uint8_t spsm;
    uint32_t dbp;
    uint8_t dbw;
    uint8_t dpsm;
};

struct GSTrxPos
{
    uint16_t ssax, ssay;
    uint16_t dsax, dsay;
    uint8_t dir;
};

struct GSTrxReg
{
    uint16_t rrw, rrh;
};

class GSRasterizer;

class GS
{
    friend class GSRasterizer;

public:
    GS();
    ~GS() = default;

    void init(uint8_t *vram, uint32_t vramSize, struct GSRegisters *privRegs = nullptr);
    void reset();

    // Legacy entry point: assumes PATH3 (GIF DMA).
    void processGIFPacket(const uint8_t *data, uint32_t sizeBytes);
    // Path-aware entry point. IMAGE-mode data may be split across DMA kicks
    // (QWC is 16-bit, so >1MB uploads always are); the GS keeps per-path
    // continuation state so the next packet's leading bytes are consumed as
    // image data instead of being misparsed as GIFtags.
    void processGIFPacket(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes,
                          uint32_t vu1Pc = 0,
                          const GifPacketDiagnostic *diagnostic = nullptr);
    void writeRegister(uint8_t regAddr, uint64_t value);

    const uint8_t *lockDisplaySnapshot(uint32_t &outSize);
    void unlockDisplaySnapshot();
    uint32_t getLastDisplayBaseBytes() const;
    const GSFrameReg &getContextFrame(int index) const
    {
        return m_ctx[(index != 0) ? 1 : 0].frame;
    }
    bool getPreferredDisplaySource(GSFrameReg &outSource, uint32_t &outDestFbp) const;
    // Milestone C0 fix: flush any tile-binned primitives queued for the
    // CURRENT (not-yet-finished) frame. Safe to call from either the game
    // thread (processGIFPacket's own tick-boundary flush already does this)
    // or the present thread (latchHostPresentationFrame calls this first) —
    // both paths take m_stateMutex, and it is a recursive_mutex, so a
    // same-thread re-entrant call (game thread flushing, then later calling
    // this too) is also safe. See ps2_gs_gpu.cpp for the present-lag
    // rationale.
    void flushPendingTiles();
    // Present-latch drain. In sync mode == flushPendingTiles(). In async raster
    // mode the worker owns raster and the frame-clear capture already dispatched
    // the completed frame, so this only WAITS for the worker (the displayed buffer
    // must be complete in VRAM) and must NOT drain g_tileCmds — that buffer is the
    // partial NEXT frame targeting the back buffer, and draining it on the present
    // thread would re-raster under m_stateMutex, the exact stall async removes.
    void presentLatchDrain();
    void setPresentAtFrameClearEnabled(bool enabled);
    bool hasCompletedFrameClearPresentation() const;
    void latchHostPresentationFrame();
    // Interrupt-side FullFrame close already owns the authoritative tick.
    // Passing it explicitly avoids re-entering the global vsync mutex while
    // the GS state mutex is held.
    void latchHostPresentationFrame(uint64_t vsyncTick);
    bool copyLatchedHostPresentationFrame(std::vector<uint8_t> &outPixels,
                                          uint32_t &outWidth,
                                          uint32_t &outHeight,
                                          uint32_t *outDisplayFbp = nullptr,
                                          uint32_t *outSourceFbp = nullptr,
                                          bool *outUsedPreferred = nullptr) const;
    bool clearFramebufferContext(uint32_t contextIndex, uint32_t rgba);
    bool clearActiveFramebuffer(uint32_t rgba);

    uint32_t consumeLocalToHostBytes(uint8_t *dst, uint32_t maxBytes);

    void refreshDisplaySnapshot();

    inline void WriteVram(u32 psm, uint32_t base, uint32_t bw, uint32_t x, uint32_t y, uint32_t value);
    inline u32 ReadVram(u32 psm, u32 base, u32 bw, u32 x, u32 y) const;

private:
    // Bytes of IMAGE-mode data still owed by the previous packet on each GIF
    // path (indexed by GifPathId value 1..3). Real GS tag decoding is per-path.
    uint32_t m_pathPendingImageBytes[4] = {0u, 0u, 0u, 0u};

    void snapshotVRAM();
    void captureFrameClearPresentationUnlocked(const GSFrameReg &targetFrame,
                                               const GSFrameReg &oldFrame);
    // Track which framebuffer bases DISPFB actually scans out, so the
    // frame-clear present capture can refuse to present offscreen render
    // targets (see m_scanoutDisplayFbps). Caller must hold m_stateMutex.
    void recordScanoutDisplayFbpsUnlocked();
    bool isScanoutDisplayFbp(uint32_t fbp) const;
    // Caller must already hold m_stateMutex (GIF packet path locks once per packet).
    void writeRegisterUnlocked(uint8_t regAddr, uint64_t value);
    void writeRegisterPacked(uint8_t regDesc, uint64_t lo, uint64_t hi);
    void vertexKick(bool drawing);

    void processImageData(const uint8_t *data, uint32_t sizeBytes);
    void performLocalToLocalTransfer();
    void performLocalToHostToBuffer();
    bool copyFrameToHostRgbaUnlocked(const GSFrameReg &frame,
                                     uint32_t width,
                                     uint32_t height,
                                     std::vector<uint8_t> &outPixels,
                                     bool preserveAlpha = false,
                                     bool useLocalMemoryLayout = false,
                                     bool frameBaseIsPages = true,
                                     uint32_t sourceOriginX = 0u,
                                     uint32_t sourceOriginY = 0u) const;

    // Hot path: called per-pixel by the rasterizer. Keep inline in the header so
    // it doesn't become an out-of-line cross-TU call (was ~all per-pixel overhead
    // in profiles). Body mirrors the former ps2_gs_gpu.cpp definition.
    GSContext &activeContext() { return m_ctx[m_prim.ctxt ? 1 : 0]; }

    uint8_t *m_vram = nullptr;
    uint32_t m_vramSize = 0;
    struct GSRegisters *m_privRegs = nullptr;
    mutable std::recursive_mutex m_stateMutex;

    GSContext m_ctx[2];
    GSPrimReg m_prim{};

    // RRV_GS_CLUT_CACHE: chip-level CBP0/CBP1 "last loaded CLUT source" tags
    // used by CLD=4/5 (load-if-different). Real hardware has ONE physical CLUT
    // buffer and these two trackers shared across both rendering contexts;
    // simplification note: we still latch a *separate* decoded buffer per
    // context (GSTex0Reg::clutCache) rather than modeling one shared physical
    // buffer, because that's what tile-binning's per-primitive snapshot needs
    // to stay correct (see GSTex0Reg::clutCache comment). CBP0/CBP1 themselves
    // are correctly chip-global here. 0xFFFFFFFF = "never loaded" so the first
    // real CLD=4/5 write always loads.
    uint32_t m_clutCbp0 = 0xFFFFFFFFu;
    uint32_t m_clutCbp1 = 0xFFFFFFFFu;

    uint8_t m_curR = 0x80, m_curG = 0x80, m_curB = 0x80, m_curA = 0x80;
    float m_curQ = 1.0f;
    float m_curS = 0.0f, m_curT = 0.0f;
    uint16_t m_curU = 0, m_curV = 0;
    uint8_t m_curFog = 0;

    bool m_prmodecont = true;
    bool m_pabe = false;
    GSTexaReg m_texa{0u, false, 0u};
    GSTexClutReg m_texclut{0u, 0u, 0u};
    // Renderer-IR D0 gap-closure: FOGCOL was previously decoded then discarded
    // (GS_REG_FOGCOL fell into the same no-op case as TEXFLUSH/SCANMSK/DIMX/
    // DTHE/COLCLAMP in writeRegisterUnlocked). Not per-context on real hardware
    // (single register, no _1/_2 variant), so stored once here, chip-global,
    // like m_pabe/m_texa. Low 24 bits are FCR[0:7]/FCG[8:15]/FCB[16:23]
    // (GIFReg FOGCOL layout), the rest reserved.
    uint32_t m_fogcol = 0u;

    GSBitBltBuf m_bitbltbuf{};
    GSTrxPos m_trxpos{};
    GSTrxReg m_trxreg{};
    uint32_t m_trxdir = 3;

    struct
    {
        uint32_t x{ 0 };
        uint32_t y{ 0 };
        uint32_t total_pixels{ 0 };
        uint32_t copied_pixels{ 0 };
    } m_transferState;

    static constexpr int kMaxVerts = 6;
    GSVertex m_vtxQueue[kMaxVerts];
    int m_vtxCount = 0;
    int m_vtxIndex = 0;

    // Tile-binning (RRV_RASTER_TILE): VSync tick at which the deferred command
    // buffer was last flushed. A tick change at processGIFPacket entry marks a new
    // frame => flush the just-completed frame's primitives in one parallel dispatch.
    uint64_t m_lastTileFlushTick = ~0ull;

    std::vector<uint8_t> m_displaySnapshot;
    std::mutex m_snapshotMutex;
    uint32_t m_lastDisplayBaseBytes = 0;
    GSFrameReg m_preferredDisplaySourceFrame{};
    uint32_t m_preferredDisplayDestFbp = 0;
    bool m_hasPreferredDisplaySource = false;
    std::vector<uint8_t> m_hostPresentationFrame;
    uint32_t m_hostPresentationWidth = 0;
    uint32_t m_hostPresentationHeight = 0;
    uint32_t m_hostPresentationDisplayFbp = 0;
    uint32_t m_hostPresentationSourceFbp = 0;
    GSFrameReg m_hostPresentationSourceFrame{};
    bool m_hostPresentationSourceValid = false;
    bool m_hostPresentationUsedPreferred = false;
    bool m_hasHostPresentationFrame = false;
    bool m_presentAtFrameClearEnabled = false;
    bool m_hasCompletedFrameClearPresentation = false;
    uint32_t m_frameClearReuseLatchCount = 0u;
    // Framebuffer bases the display actually scans out via DISPFB (the display
    // double-buffer, e.g. {0,70}). Populated every present from PMODE/DISPFB.
    // Offscreen render targets never enter this set — notably the attract girl
    // cinematic's aux buffer at fbp=210, which holds the girl on a black clear
    // and is never assigned to DISPFB. The frame-clear present capture gates on
    // membership so it never presents that aux buffer (the "girl on black"
    // frame that alternated with the main composite as flicker). Guarded by
    // m_stateMutex; stays tiny (a handful of unique display bases).
    std::vector<uint32_t> m_scanoutDisplayFbps;
    // RRV_PRESENT_FIELD_FIX (2026-07-12): non-black pixel count of the most
    // recent frame-clear capture, so the latch can detect a black pinned frame
    // (field/60fps cadence captures the not-yet-drawn buffer) and fall through
    // to the DISPFB+candidate path instead of presenting black.
    uint32_t m_frameClearCaptureNonBlack = 0u;
    uint64_t m_frameClearCapturePmode = 0u;
    uint64_t m_frameClearCaptureSmode2 = 0u;
    uint64_t m_frameClearCaptureDisplay1 = 0u;
    uint64_t m_frameClearCaptureDisplay2 = 0u;
    bool m_processingGifPacket = false;
    GifPathId m_currentGifPacketPath = GifPathId::Path1;
    // B-1 RRV_CAR_GIF_DIAG: packet-scoped car-live context, read by vertexKick()
    // to bin per-(phase,path,XGKICKpc) screen-space vertex bounding boxes.
    bool m_carGifActive = false;
    uint32_t m_carGifPhase = 0u;
    uint32_t m_carGifVu1Pc = 0u;
    // Per-packet screen-space bbox accumulators (packet key is constant, so no
    // per-vertex lock; flushed to the global map once at packet end).
    uint64_t m_carGifVerts = 0u, m_carGifDrawing = 0u, m_carGifNonfinite = 0u,
             m_carGifOffscreen = 0u, m_carGifExtreme = 0u;
    float m_carGifMinX = 0.f, m_carGifMinY = 0.f, m_carGifMaxX = 0.f, m_carGifMaxY = 0.f;
    bool m_carGifBBoxInit = false;
    bool m_carDmaGifDiagActive = false;
    bool m_carDmaGifDiagCurrentWrite = false;
    uint64_t m_carDmaGifDiagChain = 0u;
    uint32_t m_carDmaGifDiagBegin = 0u;
    uint32_t m_carDmaGifDiagEnd = 0u;
    uint32_t m_carDmaGifDiagCallTag = 0u;
    uint32_t m_carDmaGifDiagCallTarget = 0u;
    uint64_t m_carDmaGifDiagDigest = 0u;
    uint32_t m_carDmaGifDiagTags = 0u;
    uint32_t m_carDmaGifDiagKicks = 0u;
    uint32_t m_carDmaGifDiagDrawingKicks = 0u;
    uint32_t m_carDmaGifDiagAdcKicks = 0u;
    uint32_t m_carDmaGifDiagNonfinite = 0u;
    uint32_t m_carDmaGifDiagExtreme = 0u;
    uint32_t m_carDmaGifDiagXyz2Adc = 0u;
    uint32_t m_carDmaGifDiagXyz3Descriptor = 0u;
    uint32_t m_carDmaGifDiagAdXyz3 = 0u;
    uint32_t m_carDmaGifDiagUnknownNoDraw = 0u;
    uint32_t m_carDmaGifDiagCoverRecords = 0u;
    uint32_t m_carDmaGifDiagSuppressedCovers = 0u;
    uint32_t m_carDmaGifDiagCauseRecords = 0u;
    uint32_t m_carDmaGifDiagCurrentByteOffset = 0u;
    uint32_t m_carDmaGifDiagCurrentTagOffset = 0u;
    uint8_t m_carDmaGifDiagCurrentSourceDesc = 0u;
    uint8_t m_carDmaGifDiagCurrentTargetReg = 0u;
    uint8_t m_carDmaGifDiagCurrentRegPhase = 0u;
    bool m_carDmaGifDiagCurrentRawAdc = false;
    float m_carDmaGifDiagMinX = 0.0f;
    float m_carDmaGifDiagMinY = 0.0f;
    float m_carDmaGifDiagMaxX = 0.0f;
    float m_carDmaGifDiagMaxY = 0.0f;

    std::vector<uint8_t> m_localToHostBuffer;
    size_t m_localToHostReadPos = 0;

    GSRasterizer m_rasterizer;

    using WriteVramFunc = std::function<void(u8*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t)>;
    using ReadVramFunc = std::function<u32(u8*, u32, u32, u32, u32)>;

    // 0x40 entries: ReadVram/WriteVram index with (psm & 0x3F), whose range is
    // 0..0x3F. A 0x3F-sized array left index 0x3F out of bounds — an invalid/
    // garbage PSM (low 6 bits 0x3F) on a 3D triangle read past the end and called
    // a garbage std::function (SIGSEGV in the state-6 intro). 0x40 covers the
    // whole masked range; the unused slots get Read/WriteNull below.
    std::array<ReadVramFunc, 0x40> m_read_vram_funcs{ };
    std::array<WriteVramFunc, 0x40> m_write_vram_funcs{ };
};

inline u32 GS::ReadVram(u32 psm, u32 base, u32 bw, u32 x, u32 y) const
{
    return m_read_vram_funcs[psm & 0x3F](m_vram, base, bw, x, y);
}

inline void GS::WriteVram(u32 psm, u32 base, u32 bw, u32 x, u32 y, u32 value)
{
    m_write_vram_funcs[psm & 0x3F](m_vram, base, bw, x, y, value);
}

#endif
