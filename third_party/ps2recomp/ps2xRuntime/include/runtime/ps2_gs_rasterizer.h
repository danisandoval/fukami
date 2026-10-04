#ifndef PS2_GS_RASTERIZER_H
#define PS2_GS_RASTERIZER_H

#include <cstdint>
#include <memory>
#include <array>
#include <vector>

class GS;
struct GSTex0Reg;
// One primitive's self-contained draw-state snapshot (defined in
// ps2_gs_rasterizer.cpp). Forward-declared here so flushTileCmds() can take the
// command list by reference, which lets the async-raster path double-buffer it
// (gameThread fills one vector while a render worker drains another).
struct GSTileCmd;

class GSRasterizer
{
public:
    void drawPrimitive(GS *gs);
    void writePixel(GS *gs, int x, int y, int z, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    uint32_t sampleTexture(GS *gs, float s, float t, float q, uint16_t u, uint16_t v);
    uint32_t lookupCLUT(GS *gs, uint8_t index, const GSTex0Reg &tex);

    // RRV_GS_CLUT_CACHE (default OFF): latch context ctxIndex's CLUT into
    // GSTex0Reg::clutCache per TEX0.CLD semantics. Call at TEX0/TEX2
    // write time (before the draw that uses it, per real hardware). No-op
    // when the env hatch is unset. See ps2_gs_gpu.h GSTex0Reg::clutCache and
    // ps2_gs_rasterizer.cpp for the CLD-case-by-case behaviour and which
    // cases are simplified.
    static bool clutCacheEnabled();
    void latchClut(GS *gs, int ctxIndex);

    // Tile-binning (RRV_RASTER_TILE, default off): when enabled, drawPrimitive
    // defers each primitive into a per-frame command buffer instead of drawing
    // immediately; flushTiles() then rasterizes the whole buffer across horizontal
    // screen strips in a single parallel dispatch (one dispatch/frame, not one per
    // primitive). Output-preserving: disjoint strips write disjoint pixels; within
    // a strip primitives replay in submission order. Callers must flushTiles()
    // before any VRAM read-back/upload/transfer to preserve ordering.
    static bool tileEnabled();
    void flushTiles(GS *gs);
    // Rasterize an explicit command list (the deferred-draw body of flushTiles).
    // flushTiles(gs) is the thin wrapper over the process-global producer buffer;
    // the async-raster worker calls this directly on the buffer it owns. Output-
    // preserving and self-contained: only VRAM (shared via gs) is mutated.
    void flushTileCmds(GS *gs, std::vector<GSTileCmd> &cmds);

    // Async raster (RRV_ASYNC_RASTER, default OFF): move the end-of-frame flush
    // off the gameThread onto a render worker. dispatchAsyncFrame() hands the
    // completed frame's command list to the worker and returns so the gameThread
    // can build the next frame; waitForRasterWorker() blocks for it, and every
    // VRAM fence (flushTiles at present-latch/upload/transfer/read-back) waits
    // first so the worker owns VRAM exclusively while it runs. No-op / not
    // dispatched unless the env hatch is set.
    static bool asyncRasterEnabled();
    void dispatchAsyncFrame(GS *gs);
    void waitForRasterWorker();

    // Milestone C1 (RRV_IR_RASTER, default off): the IR-consuming rasterization
    // path. When enabled, GS::vertexKick defers primitives into the in-memory
    // Renderer-IR draw stream instead of drawing them; flushIR() then pulls the
    // pending draws back OUT of the IR (rrv::ir::rrv_ir_raster_get) and rasterizes
    // them by reconstructing a shadow GS from the interned IR state — proving the
    // IR is a complete rasterization source. No-op unless RRV_IR_RASTER is set.
    // Callers place it at exactly the same points flushTiles() is called (present
    // latch + before any VRAM read-back/upload/transfer) so ordering matches.
    void flushIR(GS *gs);
    // Replicate the present-source bookkeeping side effects that the immediate/
    // tile draw path performs on the REAL gs, for the IR-raster path (which skips
    // drawPrimitive/captureTile). Pixels come from the IR; this keeps the
    // preferred-display-source decision identical.
    static void captureIRSideEffects(GS *gs);

private:
    void drawSprite(GS *gs);
    void drawTriangle(GS *gs);
    void drawLine(GS *gs);
    void captureTile(GS *gs); // snapshot current primitive into the tile command buffer
    // RRV_GS_CLUT_CACHE: decode the full 256-entry CLUT from current VRAM at
    // tex.cbp; called from latchClut() at TEX0/TEX2 write time. A GSRasterizer
    // member (not a free function) so it gets the same GS-friend access the
    // rest of the raster path relies on.
    std::shared_ptr<std::array<uint32_t, 256>> buildClutTable(GS *gs, const struct GSTex0Reg &tex);
};

#endif
