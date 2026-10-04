#ifndef PS2_MEMORY_H
#define PS2_MEMORY_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <utility>
#include <vector>
#include <unordered_map>
#include <atomic>
// Gate 5 (scripts/vu_aot_overlay.py): changes to VU0 [0] / VU1 [1] micro memory.
inline std::atomic<uint32_t> g_rrvVuCodeGen[2] = {1u, 1u};
inline void rrvVuCodeChanged(int unit) { g_rrvVuCodeGen[unit].fetch_add(1u, std::memory_order_relaxed); }
#include <iostream>

#include "ps2_gif_arbiter.h"
#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(USE_SSE2NEON)
#include "sse2neon.h"
#else
#include <immintrin.h> // For SSE/AVX instructions
#include <smmintrin.h> // For SSE4.1 instructions
#endif

constexpr uint32_t PS2_RAM_SIZE = 32u * 1024u * 1024u; // 32MB
constexpr uint32_t PS2_RAM_MASK = PS2_RAM_SIZE - 1u;   // Mask for 32MB alignment
constexpr uint32_t PS2_RAM_BASE = 0x00000000;          // Physical base of RDRAM
constexpr uint32_t PS2_SCRATCHPAD_BASE = 0x70000000;
constexpr uint32_t PS2_SCRATCHPAD_ALIAS_BASE = 0xF0000000;
constexpr uint32_t PS2_SCRATCHPAD_SIZE = 16u * 1024u;  // 16KB
constexpr uint32_t PS2_IO_BASE = 0x10000000;           // Base for many I/O regs (Timers, DMAC, INTC)
constexpr uint32_t PS2_IO_SIZE = 0x10000;              // 64KB
constexpr uint32_t PS2_BIOS_BASE = 0x1FC00000;         // Or BFC00000 depending on KSEG
constexpr uint32_t PS2_BIOS_SIZE = 4u * 1024u * 1024u; // 4MB

constexpr uint32_t PS2_VU0_CODE_BASE = 0x11000000; // Base address as seen from EE
constexpr uint32_t PS2_VU0_DATA_BASE = 0x11004000;
constexpr uint32_t PS2_VU0_CODE_SIZE = 4u * 1024u; // 4KB Micro Memory
constexpr uint32_t PS2_VU0_DATA_SIZE = 4u * 1024u; // 4KB Data Memory (VU Mem)

constexpr uint32_t PS2_VU1_CODE_BASE = 0x11008000;
constexpr uint32_t PS2_VU1_DATA_BASE = 0x1100C000;
constexpr uint32_t PS2_VU1_MEM_BASE = PS2_VU1_CODE_BASE; // Alias used by older code paths
constexpr uint32_t PS2_VU1_CODE_SIZE = 16u * 1024u;      // 16KB Micro Memory
constexpr uint32_t PS2_VU1_DATA_SIZE = 16u * 1024u;      // 16KB Data Memory (VU Mem)

constexpr uint32_t PS2_GS_BASE = 0x12000000;
constexpr uint32_t PS2_GS_PRIV_REG_BASE = PS2_GS_BASE; // GS Privileged Registers
constexpr uint32_t PS2_GS_PRIV_REG_SIZE = 0x2000;
constexpr size_t PS2_GS_VRAM_SIZE = 4u * 1024u * 1024u; // 4MB GS VRAM

// RRV_GIF_PATH_LATENCY is enabled by default.  Keep its interpretation pure so
// the default-on contract and the explicit rollback hatch can be regression
// tested without process-global getenv caching.
struct RrvGifPathLatencyConfig
{
    bool enabled = true;
    uint64_t smallPacketBytes = 4096u;
};

// Threshold measured 2026-08-23 against both scenes that depend on this model,
// on the same build, replayed through the PCSX2 GS:
//
//   value | girl (A2, B-3)              | phase-39 burn-to-logo
//   ------+-----------------------------+----------------------------
//   0     | BROKEN - city gone, the     | works
//         | B-3 late-CLEAR signature    |
//   4096  | works                       | BROKEN - black, permanently
//   256   | works                       | works
//
// 4096 held the phase-39 composite's PATH3 packets back for the whole frame, so
// the VIF1 chain was released only by the dispatch-loop backstop, after the tick
// had already advanced -- landing that pass in the other double-buffer half.
// Since that composite feeds back on itself to keep the logo on screen, one such
// frame made it black for good. 256 releases the chain against the composite's
// own packets instead, and the control above shows it does NOT cost the girl
// scene, which is the defect this whole model exists to fix.
constexpr uint64_t RRV_GIF_PATH_LATENCY_DEFAULT_SMALL_PACKET_BYTES = 256u;

inline RrvGifPathLatencyConfig rrvParseGifPathLatencyConfig(const char *value)
{
    RrvGifPathLatencyConfig config{true, RRV_GIF_PATH_LATENCY_DEFAULT_SMALL_PACKET_BYTES};
    if (!value || value[0] == '\0')
        return config;
    if (value[0] == '0' && value[1] == '\0')
    {
        config.enabled = false;
        return config;
    }

    uint64_t parsed = 0u;
    for (const char *cursor = value; *cursor != '\0'; ++cursor)
    {
        if (*cursor < '0' || *cursor > '9')
            return config;
        const uint64_t digit = static_cast<uint64_t>(*cursor - '0');
        if (parsed > (UINT64_MAX - digit) / 10u)
            return config;
        parsed = parsed * 10u + digit;
    }

    // =1 selects the default just like the historical opt-in.  A threshold
    // override must be greater than one, so malformed/zero values cannot turn
    // every PATH3 transfer into a "large" packet.
    if (parsed > 1u)
        config.smallPacketBytes = parsed;
    return config;
}

// RRV_GIF_PATH3_INTERLEAVE — cross-channel DMA slicing (default on; =0 rollback).
//
// RRV_GIF_PATH_LATENCY holds a kicked VIF1 chain so a SMALL PATH3 packet can
// still overtake it, which is the B-3 girl fix. It does nothing for a LARGE
// PATH3 chain: that branch flushes the held VIF1 queue whole and only then
// submits the PATH3, so every XGKICK of a 725 KB geometry chain reaches the GS
// before the first byte of the same frame's 570 KB texture/composite chain.
// Hardware's DMAC round-robins channel 1 and channel 2 instead, so the two
// chains move at the same byte rate and PATH3 lands BETWEEN the XGKICKs
// (docs/TESTING.md T-P7-CHANNEL-ATOMIC).
//
// This models that: while a held VIF1 chain is expanded, PATH3 is released in
// GIFtag-sized pieces, keeping its byte cursor in step with the VIF1 chain's.
// `ratio256` is PATH3 bytes released per 256 VIF1 bytes consumed; 256 (= 1.0)
// is equal bandwidth, the physical default. The split is always at a GIFtag
// boundary, which is exactly where the real GIF may switch paths, so per-path
// tag/IMAGE continuation state stays consistent.
struct RrvGifPath3InterleaveConfig
{
    bool enabled = true;
    uint32_t ratio256 = 256u;
};

constexpr uint32_t RRV_GIF_PATH3_INTERLEAVE_DEFAULT_RATIO256 = 256u;

inline RrvGifPath3InterleaveConfig rrvParseGifPath3InterleaveConfig(const char *value)
{
    RrvGifPath3InterleaveConfig config{true, RRV_GIF_PATH3_INTERLEAVE_DEFAULT_RATIO256};
    if (!value || value[0] == '\0')
        return config;
    if (value[0] == '0' && value[1] == '\0')
    {
        config.enabled = false;
        return config;
    }

    uint64_t parsed = 0u;
    for (const char *cursor = value; *cursor != '\0'; ++cursor)
    {
        if (*cursor < '0' || *cursor > '9')
            return config;
        const uint64_t digit = static_cast<uint64_t>(*cursor - '0');
        if (parsed > (UINT64_MAX - digit) / 10u)
            return config;
        parsed = parsed * 10u + digit;
    }

    // =1 selects the default, like the historical opt-in spellings. Any other
    // value is a ratio override in 1/256ths; 0 was already handled as "off".
    if (parsed > 1u)
        config.ratio256 = static_cast<uint32_t>(parsed);
    return config;
}

// RRV_GIF_PATH3_HAZARD — the buffer-hazard barrier on the pacer above.
//
// The interleave is correct for a PATH3 chain that CO-RENDERS with PATH1 (the
// phase-7 sky, the composites, the uploads), and wrong for one that CONSUMES
// what PATH1 is still producing. The flyover's RIDGE CITY FM overlay is the
// second kind: one 57 KB packet draws a depth-graded alpha stencil that reads
// the scene's Z buffer, so releasing it mid-expansion grades it against an
// unfinished depth buffer and the geometry drawn afterwards buries the
// lettering (docs/TESTING.md T-FLY-OVERLAY-2/3).
//
// The discriminator is a read-after-write on a buffer, not "does the packet
// touch shared drawing state" — that was measured and falsified, because
// nearly every packet writes a context register. A packet that read-modify-
// writes the same buffer PATH1 writes is ordinary co-rendering; a packet that
// READS a buffer PATH1 writes without writing it back is a dependency on work
// that has not happened yet. Measured over one display list each, that rule
// picks exactly ONE packet on both sides — the same 57,248 B / 3,580-vertex
// stencil pass — at chain position 4 of 14 in the flyover (so it holds almost
// everything, as the flyover needs) and 342 of 349 in phase 7 (so 64 % of the
// chain still goes early, as phase 7 needs).
//
// Only the Z buffer is tested. That is the measured mechanism, and it avoids
// the double-buffer trap: PATH3 samples the OTHER colour buffer routinely, and
// a colour rule would treat the whole composite chain as hazardous.
struct RrvGsBufferShadow
{
    uint16_t zbp[2] = {0u, 0u};
    uint8_t zmsk[2] = {1u, 1u};
    uint8_t zte[2] = {0u, 0u};
    uint8_t ztst[2] = {0u, 0u};
    uint8_t ctx = 0u;
};

inline bool rrvGifPathLatencyFrameBackstopDue(const RrvGifPathLatencyConfig &config,
                                              bool hasHeldVif1,
                                              uint64_t currentVSyncTick,
                                              uint64_t heldVif1Tick)
{
    // A tick transition, including natural counter wrap, releases a hold.  The
    // same tick is the in-frame ordering race this feature intentionally keeps.
    return config.enabled && hasHeldVif1 && currentVSyncTick != heldVif1Tick;
}

inline constexpr uint32_t PS2_FIO_O_RDONLY = 0x0001;
inline constexpr uint32_t PS2_FIO_O_WRONLY = 0x0002;
inline constexpr uint32_t PS2_FIO_O_RDWR = 0x0003;
inline constexpr uint32_t PS2_FIO_O_NBLOCK = 0x0010;
inline constexpr uint32_t PS2_FIO_O_APPEND = 0x0100;
inline constexpr uint32_t PS2_FIO_O_CREAT = 0x0200;
inline constexpr uint32_t PS2_FIO_O_TRUNC = 0x0400;
inline constexpr uint32_t PS2_FIO_O_EXCL = 0x0800;
inline constexpr uint32_t PS2_FIO_O_NOWAIT = 0x8000;

inline constexpr uint32_t PS2_FIO_SEEK_SET = 0;
inline constexpr uint32_t PS2_FIO_SEEK_CUR = 1;
inline constexpr uint32_t PS2_FIO_SEEK_END = 2;

inline constexpr uint32_t PS2_FIO_S_IFDIR = 0x1000;
inline constexpr uint32_t PS2_FIO_S_IFREG = 0x2000;

static_assert((PS2_RAM_SIZE & (PS2_RAM_SIZE - 1u)) == 0u, "PS2_RAM_SIZE must be a power of two");
static_assert(PS2_RAM_MASK == (PS2_RAM_SIZE - 1u), "PS2_RAM_MASK must match PS2_RAM_SIZE");

inline std::atomic<uint8_t *> &ps2ScratchpadHostPtrStorage()
{
    static std::atomic<uint8_t *> ptr{nullptr};
    return ptr;
}

inline void ps2SetScratchpadHostPtr(uint8_t *ptr)
{
    ps2ScratchpadHostPtrStorage().store(ptr, std::memory_order_relaxed);
}

inline uint8_t *ps2GetScratchpadHostPtr()
{
    return ps2ScratchpadHostPtrStorage().load(std::memory_order_relaxed);
}

inline bool ps2IsScratchpadAddress(uint32_t addr)
{
    if (addr >= PS2_SCRATCHPAD_BASE && addr < (PS2_SCRATCHPAD_BASE + PS2_SCRATCHPAD_SIZE))
    {
        return true;
    }

    if ((addr & 0x80000000u) != 0u)
    {
        const uint32_t lower = addr & 0x7FFFFFFFu;
        return lower >= PS2_SCRATCHPAD_BASE &&
               lower < (PS2_SCRATCHPAD_BASE + PS2_SCRATCHPAD_SIZE);
    }

    return false;
}

inline uint32_t ps2ScratchpadOffset(uint32_t addr)
{
    if (addr >= PS2_SCRATCHPAD_BASE && addr < (PS2_SCRATCHPAD_BASE + PS2_SCRATCHPAD_SIZE))
    {
        return addr - PS2_SCRATCHPAD_BASE;
    }

    const uint32_t lower = addr & 0x7FFFFFFFu;
    return lower - PS2_SCRATCHPAD_BASE;
}

inline bool ps2ResolveGuestPointer(uint32_t addr, uint32_t &offset, bool &scratch)
{
    if (ps2IsScratchpadAddress(addr))
    {
        scratch = true;
        offset = ps2ScratchpadOffset(addr);
        return true;
    }

    uint32_t phys = 0;
    if (addr < 0x20000000u)
    {
        phys = addr;
    }
    else if ((addr >= 0x20000000u && addr < 0x40000000u) ||
             (addr >= 0x80000000u && addr < 0xC0000000u))
    {
        phys = addr & 0x1FFFFFFFu;
    }

    if (phys >= PS2_RAM_SIZE)
    {
        phys &= PS2_RAM_MASK;
    }

    scratch = false;
    offset = phys;
    return true;
}
inline uint8_t *getMemPtr(uint8_t *rdram, uint32_t addr)
{
    if (rdram == nullptr)
    {
        return nullptr;
    }

    uint32_t offset = 0;
    bool scratch = false;
    if (!ps2ResolveGuestPointer(addr, offset, scratch))
    {
        return nullptr;
    }

    if (scratch)
    {
        uint8_t *scratchpad = ps2GetScratchpadHostPtr();
        return scratchpad ? (scratchpad + offset) : nullptr;
    }
    return rdram + offset;
}

inline const uint8_t *getConstMemPtr(const uint8_t *rdram, uint32_t addr)
{
    if (rdram == nullptr)
    {
        return nullptr;
    }

    uint32_t offset = 0;
    bool scratch = false;
    if (!ps2ResolveGuestPointer(addr, offset, scratch))
    {
        return nullptr;
    }

    if (scratch)
    {
        const uint8_t *scratchpad = ps2GetScratchpadHostPtr();
        return scratchpad ? (scratchpad + offset) : nullptr;
    }
    return rdram + offset;
}

// PS2 GS (Graphics Synthesizer) registers
struct GSRegisters
{
    uint64_t pmode;    // Pixel mode
    uint64_t smode1;   // Sync mode 1
    uint64_t smode2;   // Sync mode 2
    uint64_t srfsh;    // Refresh control
    uint64_t synch1;   // Synchronization control 1
    uint64_t synch2;   // Synchronization control 2
    uint64_t syncv;    // Synchronization control V
    uint64_t dispfb1;  // Display buffer 1
    uint64_t display1; // Display area 1
    uint64_t dispfb2;  // Display buffer 2
    uint64_t display2; // Display area 2
    uint64_t extbuf;   // External buffer
    uint64_t extdata;  // External data
    uint64_t extwrite; // External write
    uint64_t bgcolor;  // Background color
    uint64_t csr;      // Status
    uint64_t imr;      // Interrupt mask
    uint64_t busdir;   // Bus direction
    uint64_t siglblid; // Signal label ID
};
static_assert(sizeof(GSRegisters) == (19u * sizeof(uint64_t)), "GSRegisters layout changed unexpectedly");
static_assert(alignof(GSRegisters) == alignof(uint64_t), "GSRegisters alignment must remain 64-bit");

// PS2 VIF (VPU Interface) registers
struct VIFRegisters
{
    uint32_t stat;   // Status
    uint32_t fbrst;  // VIF Force Break
    uint32_t err;    // Error status
    uint32_t mark;   // Interrupt control
    uint32_t cycle;  // Transfer mode
    uint32_t mode;   // Mode control
    uint32_t num;    // Data amount counter
    uint32_t mask;   // Data mask
    uint32_t code;   // VIFcode
    uint32_t itops;  // ITOP save
    uint32_t base;   // Base address
    uint32_t ofst;   // Offset
    uint32_t tops;   // TOPS
    uint32_t itop;   // ITOP
    uint32_t top;    // TOP
    uint32_t row[4]; // Transfer row data
    uint32_t col[4]; // Transfer column data
};
static_assert(sizeof(VIFRegisters) == (23u * sizeof(uint32_t)), "VIFRegisters layout changed unexpectedly");

// PS2 DMA registers
struct DMARegisters
{
    uint32_t chcr; // Channel control
    uint32_t madr; // Memory address
    uint32_t qwc;  // Quadword count
    uint32_t tadr; // Tag address
    uint32_t asr0; // Address stack 0
    uint32_t asr1; // Address stack 1
    uint32_t sadr; // Source address
};
static_assert(sizeof(DMARegisters) == (7u * sizeof(uint32_t)), "DMARegisters layout changed unexpectedly");

struct JumpTable
{
    uint32_t address = 0;          // Base address of the jump table
    uint32_t baseRegister = 0;     // Register used for index
    std::vector<uint32_t> targets; // Jump targets
};

class PS2Memory
{
public:
    PS2Memory();
    ~PS2Memory();

    PS2Memory(const PS2Memory &) = delete;
    PS2Memory &operator=(const PS2Memory &) = delete;
    PS2Memory(PS2Memory &&) = delete;
    PS2Memory &operator=(PS2Memory &&) = delete;

    // Initialize memory
    bool initialize(size_t ramSize = PS2_RAM_SIZE);

    // Memory access methods
    uint8_t *getRDRAM() { return m_rdram; }
    uint8_t *getScratchpad() { return m_scratchpad; }
    uint8_t *getIOPRAM() { return iop_ram; }
    uint64_t dmaStartCount() const { return m_dmaStartCount.load(std::memory_order_relaxed); }
    uint64_t gifCopyCount() const { return m_gifCopyCount.load(std::memory_order_relaxed); }
    uint64_t gsWriteCount() const { return m_gsWriteCount.load(std::memory_order_relaxed); }
    uint64_t vifWriteCount() const { return m_vifWriteCount.load(std::memory_order_relaxed); }

    // Called in the serialized guest execution domain, including direct HLE
    // memory() accesses. A false result leaves the original memory path intact.
    using Gate3MmioRead = std::function<bool(uint32_t, unsigned, uint64_t &)>;
    using Gate3MmioWrite = std::function<bool(uint32_t, unsigned, uint64_t)>;
    void gate3SetMmio(Gate3MmioRead read, Gate3MmioWrite write)
    {
        m_gate3MmioRead = std::move(read);
        m_gate3MmioWrite = std::move(write);
    }

    // Read/write memory
    uint8_t read8(uint32_t address);
    uint16_t read16(uint32_t address);
    uint32_t read32(uint32_t address);
    uint64_t read64(uint32_t address);
    __m128i read128(uint32_t address);

    void write8(uint32_t address, uint8_t value);
    void write16(uint32_t address, uint16_t value);
    void write32(uint32_t address, uint32_t value);
    void write64(uint32_t address, uint64_t value);
    void write128(uint32_t address, __m128i value);

    // TLB handling
    uint32_t translateAddress(uint32_t virtualAddress);
    bool tlbRead(uint32_t index, uint32_t &vpn, uint32_t &pfn, uint32_t &mask, bool &valid) const;
    bool tlbWrite(uint32_t index, uint32_t vpn, uint32_t pfn, uint32_t mask, bool valid);
    int32_t tlbProbe(uint32_t vpn) const;
    size_t tlbEntryCount() const { return m_tlbEntries.size(); }

    // Hardware register interface
    bool writeIORegister(uint32_t address, uint32_t value);
    uint32_t readIORegister(uint32_t address);

    using GifPacketCallback = std::function<void(const uint8_t *, uint32_t)>;
    void setGifPacketCallback(GifPacketCallback cb) { m_gifPacketCallback = std::move(cb); }
    void setGifArbiter(GifArbiter *arbiter) { m_gifArbiter = arbiter; }

    using Vu1MscalCallback = std::function<void(uint32_t startPC, uint32_t itop, uint32_t top)>;
    void setVu1MscalCallback(Vu1MscalCallback cb) { m_vu1MscalCallback = std::move(cb); }
    using Vu1MscntCallback = std::function<void(uint32_t itop, uint32_t top)>;
    void setVu1MscntCallback(Vu1MscntCallback cb) { m_vu1MscntCallback = std::move(cb); }

    using DmacCompletionCallback = std::function<void(uint32_t cause)>;
    void setDmacCompletionCallback(DmacCompletionCallback cb) { m_dmacCompletionCallback = std::move(cb); }

    uint8_t *getVU1Code() { return m_vu1Code; }
    const uint8_t *getVU1Code() const { return m_vu1Code; }
    uint8_t *getVU1Data() { return m_vu1Data; }
    const uint8_t *getVU1Data() const { return m_vu1Data; }
    uint8_t *getVU0Code() { return m_vu0Code; }
    const uint8_t *getVU0Code() const { return m_vu0Code; }
    uint8_t *getVU0Data() { return m_vu0Data; }
    const uint8_t *getVU0Data() const { return m_vu0Data; }

    bool isPath3Masked() const { return m_path3Masked; }
    void flushMaskedPath3Packets(bool drainImmediately = true);

    void submitGifPacket(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes,
                         bool drainImmediately = true, bool path2DirectHl = false,
                         uint32_t vu1Pc = 0,
                         const GifPacketDiagnostic *diagnostic = nullptr);
    void processGIFPacket(uint32_t srcPhysAddr, uint32_t qwCount);
    void processGIFPacket(const uint8_t *data, uint32_t sizeBytes);
    void processVIF0Data(uint32_t srcPhysAddr, uint32_t sizeBytes);
    void processVIF0Data(const uint8_t *data, uint32_t sizeBytes);
    void processVIF1Data(uint32_t srcPhysAddr, uint32_t sizeBytes);
    void processVIF1Data(const uint8_t *data, uint32_t sizeBytes);
    // Gate-4 VU1+GS owner stream (scripts/gate4_owner_stream_overlay.py).
    // post(command, bytes, fields, sync) runs owner work in order; sync makes
    // the EE wait for it. fence() waits for all posted work and publishes it.
    struct OwnerStreamHooksV1
    {
        std::function<void(std::function<void()>, size_t, size_t, bool)> post;
        std::function<void()> fence;
    };
    void setOwnerStreamV1(OwnerStreamHooksV1 hooks) { m_ownerStreamV1 = std::move(hooks); }
    bool ownerStreamV1() const { return static_cast<bool>(m_ownerStreamV1.post); }
    void ownerFenceV1() { if (m_ownerStreamV1.fence) m_ownerStreamV1.fence(); }
    void ownerPostV1(std::function<void()> command, size_t bytes, size_t fields, bool sync);
    uint64_t ownerStreamCommandsV1() const { return m_ownerCommandsV1; }
    uint64_t ownerStreamSyncCommandsV1() const { return m_ownerSyncCommandsV1; }
    void processPendingTransfers();

    int pollDmaRegisters();

    // Track code modifications for self-modifying code
    void registerCodeRegion(uint32_t start, uint32_t end);
    bool isCodeAddress(uint32_t address) const;
    bool isCodeModified(uint32_t address, uint32_t size);
    void clearModifiedFlag(uint32_t address, uint32_t size);

    // GS register accessors
    GSRegisters &gs() { return gs_regs; }
    const GSRegisters &gs() const { return gs_regs; }
    uint8_t *getGSVRAM() { return m_gsVRAM; }
    const uint8_t *getGSVRAM() const { return m_gsVRAM; }
    bool hasSeenGifCopy() const { return m_seenGifCopy; }
    // Main RAM (32MB)
    uint8_t *m_rdram;

    // Scratchpad memory (16KB)
    uint8_t *m_scratchpad;

    // IOP RAM (2MB)
    uint8_t *iop_ram;

    bool m_seenGifCopy;
    std::atomic<uint64_t> m_dmaStartCount{0};
    std::atomic<uint64_t> m_gifCopyCount{0};
    std::atomic<uint64_t> m_gsWriteCount{0};
    std::atomic<uint64_t> m_vifWriteCount{0};
    // I/O registers
    std::unordered_map<uint32_t, uint32_t> m_ioRegisters;

    // Registers
    GSRegisters gs_regs;
    uint8_t *m_gsVRAM;
    VIFRegisters vif0_regs;
    VIFRegisters vif1_regs;
    DMARegisters dma_regs[10]; // 10 DMA channels

    // TLB entries
    struct TLBEntry
    {
        uint32_t vpn;
        uint32_t pfn;
        uint32_t mask;
        bool valid;
    };

    std::vector<TLBEntry> m_tlbEntries;

    GifPacketCallback m_gifPacketCallback;
    GifArbiter *m_gifArbiter = nullptr;
    Vu1MscalCallback m_vu1MscalCallback;
    Vu1MscntCallback m_vu1MscntCallback;
    DmacCompletionCallback m_dmacCompletionCallback;
    std::deque<uint32_t> m_pendingDmacCompletionCauses;
    bool m_dispatchingDmacCompletions = false;

    uint8_t *m_vu1Code = nullptr;
    uint8_t *m_vu1Data = nullptr;
    uint8_t *m_vu0Code = nullptr;
    uint8_t *m_vu0Data = nullptr;
    bool m_path3Masked = false;
    uint32_t m_vif1PendingPath2ImageQwc = 0u;
    bool m_vif1PendingPath2DirectHl = false;
    static int eeTimerDecode(uint32_t address, uint32_t *regIndex);

    bool m_carDmaVifDiagActive = false;
    uint64_t m_carDmaVifDiagChain = 0u;
    uint32_t m_carDmaVifDiagBegin = 0u;
    uint32_t m_carDmaVifDiagEnd = 0u;
    struct MaskedPath3Packet
    {
        std::vector<uint8_t> data;
        GifPacketDiagnostic diagnostic;
    };
    std::vector<MaskedPath3Packet> m_path3MaskedFifo;

    struct PendingTransfer
    {
        struct SourceBlock
        {
            size_t offset = 0;
            uint32_t source = 0;
            uint32_t bytes = 0;
        };
        bool fromScratchpad = false;
        uint32_t srcAddr = 0;
        uint32_t qwc = 0;
        std::vector<uint8_t> chainData;
        uint64_t traceChainId = 0;
        std::vector<SourceBlock> sourceBlocks;
        bool carDmaDiag = false;
        uint64_t carDmaDiagChain = 0u;
        uint32_t carDmaDiagBegin = 0u;
        uint32_t carDmaDiagEnd = 0u;
        uint32_t carDmaDiagCallTag = 0u;
        uint32_t carDmaDiagCallTarget = 0u;
    };
    std::vector<PendingTransfer> m_pendingGifTransfers;
    std::vector<PendingTransfer> m_pendingVif0Transfers;
    std::vector<PendingTransfer> m_pendingVif1Transfers;
    // RRV_GIF_PATH_LATENCY (default on; =0 rollback): VIF1 chains
    // whose expansion is being held back so that a small PATH3 packet kicked
    // right after them can still reach the GS first, as it does on hardware.
    std::vector<PendingTransfer> m_heldVif1Transfers;
    uint64_t m_vif1TraceNextChainId = 0;
    uint64_t m_vif1TraceActiveChainId = 0;
    const uint8_t *m_vif1TraceDataBase = nullptr;
    const std::vector<PendingTransfer::SourceBlock> *m_vif1TraceSourceBlocks = nullptr;
    // vsync tick at which the queue above became non-empty, for the frame-
    // boundary backstop below.
    uint64_t m_heldVif1Tick = 0u;
    // Gate-4 owner stream: EE-side mirror of the held queue (owner state).
    OwnerStreamHooksV1 m_ownerStreamV1;
    size_t m_heldMirrorCountV1 = 0u;
    bool m_heldMirrorNormalV1 = false;
    bool m_arbiterDirtyV1 = false;
    uint64_t m_ownerCommandsV1 = 0u, m_ownerSyncCommandsV1 = 0u;
    void processPendingTransfersStreamV1();
    void ownerGifLoopV1(std::vector<PendingTransfer> &gif);
    bool processPendingVif0V1();
    void completePendingTransfersV1(bool hadGif, bool hadVif0, bool hadVif1);
    void ownerFlushHeldV1(const char *reason);
    void ownerFenceForVuWriteV1(uint32_t physAddr);
    // RRV_GIF_PATH_LATENCY backstop. The hold reproduces an in-frame race; it is
    // not a queue. On hardware a VIF1 chain's PATH1 output always reaches the GS
    // within the frame that kicked it, so a chain must never outlive its own
    // frame. Without this, the hold is released only by a large PATH3 kick or the
    // NEXT frame's VIF1 kick — and in the attract flyover, which has no large
    // PATH3 packet, that means every frame's geometry is deferred into the next
    // one (measured: 94 of 94 holds crossed a vblank, up to 4 ticks).
    // Call only from the dispatch safepoint: this expands VU1 and submits GIF
    // packets, so it needs a point with no live guest host frames.
    void flushHeldVif1AtFrameBoundary();

    // Release a held VIF1 chain at the END OF ITS OWN FRAME, from the guest's
    // own frame barrier (sceGsSyncV), before the vsync tick advances.
    //
    // flushHeldVif1AtFrameBoundary() above fires on `currentTick != heldTick`,
    // i.e. only once the frame is already OVER. The chain then expands inside
    // the NEXT frame and carries its own frame's FRAME register with it, so the
    // two halves of a multi-pass composite land in different double-buffer
    // halves. Measured in the phase-39 "burn to RIDGE RACER V logo" sequence:
    // hardware writes all four FRAME registers of a frame to one FBP, we wrote
    // the first to the other half every frame, so neither buffer ever held the
    // whole composite -- and because that composite feeds back on itself to keep
    // the logo on screen, one bad frame made it black permanently.
    //
    // Releasing here keeps the in-frame race the feature exists to reproduce
    // (small PATH3 packets still overtake the chain within the frame) while
    // guaranteeing the chain cannot outlive the frame that kicked it.
    void flushHeldVif1AtGuestFrameEnd();
    void processVif1Queue(std::vector<PendingTransfer> &queue);

    // RRV_GIF_PATH3_INTERLEAVE — see the config struct above. One large PATH3
    // chain, pre-split at its GIFtag boundaries so it can be released a piece at
    // a time while the VIF1 chain beside it is being expanded.
    struct Path3PacedChain
    {
        std::vector<uint8_t> data;
        // Byte offset of every GIFtag in `data`, terminated by data.size(), so
        // [tagOffsets[i], tagOffsets[i+1]) is one complete GIF packet.
        std::vector<uint32_t> tagOffsets;
        size_t nextTag = 0u;
        // RRV_GIF_PATH3_HAZARD: index of the first piece that reads a Z buffer
        // PATH1 writes without writing it back. Nothing at or after it may be
        // released early; the flush past the VIF1 expansion sends it.
        size_t hazardTag = SIZE_MAX;
        GifPacketDiagnostic diagnostic;
        bool hasDiagnostic = false;
    };
    std::vector<Path3PacedChain> m_path3Paced;
    size_t m_path3PacedChain = 0u;        // chain currently being released
    uint64_t m_path3PacedTotalBytes = 0u; // bytes across all paced chains
    uint64_t m_path3PacedSentBytes = 0u;  // bytes already submitted
    uint64_t m_path3PacedVif1Base = 0u;   // VIF1 bytes consumed by earlier chains
    uint64_t m_path3PacedVif1Cursor = 0u; // VIF1 chain bytes consumed so far
    uint64_t m_path3PacedPath1Bytes = 0u; // PATH1/PATH2 GIF bytes emitted so far
    bool m_path3PacedActive = false;
    bool m_path3PacingNow = false;        // re-entry guard
    bool m_path3PacedFlushing = false;    // the flush ignores the hazard barrier

    // RRV_GIF_PATH3_HAZARD — latched GS buffer state, fed by every submit on
    // every path, and the Z pages PATH1 has actually drawn into. The pacer arms
    // before the VIF1 chain is expanded, so the PATH1 set is necessarily the one
    // observed up to the previous list; RR5 keeps one Z buffer for the whole
    // scene, so that is stable.
    RrvGsBufferShadow m_gsShadow;
    uint16_t m_path1ZPages[2] = {0u, 0u};
    uint8_t m_path1ZPageCount = 0u;

    // Walk a GIF packet for buffer state only: latch ZBUF/TEST/PRIM, and when
    // the packet is PATH1 and draws, record the Z page it writes.
    void trackGifBufferState(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes);
    // True if [begin, stop) reads a Z page PATH1 writes and does not write that
    // page itself. `shadow` is advanced across the span so consecutive pieces
    // are judged against the state their predecessors latched.
    bool path3PieceHazardous(RrvGsBufferShadow &shadow, const uint8_t *data,
                             uint32_t begin, uint32_t stop) const;

    // Move the payload-bearing prefix of m_pendingGifTransfers into the pacer.
    // Returns true if anything was taken (the caller must then keep `hadGif`
    // true, or the channel's DMAC completion is lost — see phase 6).
    bool armPath3Pacer(std::vector<PendingTransfer> &gif);
    // Release PATH3 packets until its byte cursor has caught up with the work
    // the other channels have done. Called at both arbitration points: the VIF1
    // interpreter's VIFcode boundary (channel 1 fetching, GIF idle) and each
    // PATH1/PATH2 submit (the GIF itself becoming free).
    void pacePath3();
    // Submit everything still held and disarm. Always called after the VIF1
    // queue is expanded, so no PATH3 byte can outlive its own kick.
    void flushPath3Pacer();

    struct CodeRegion
    {
        uint32_t start;
        uint32_t end;
        std::vector<bool> modified; // Bitmap of modified 4-byte blocks
    };
    std::vector<CodeRegion> m_codeRegions;

    bool isAddressInRegion(uint32_t address, const CodeRegion &region);
    void markModified(uint32_t address, uint32_t size);
    bool isScratchpad(uint32_t address) const;

private:
    // Temporal MMIO is owned by the producer Domain. Unclaimed addresses
    // retain the original GS/GIF, DMA, SPR, IOP and memory behavior.
    Gate3MmioRead m_gate3MmioRead;
    Gate3MmioWrite m_gate3MmioWrite;
};

#endif // PS2_MEMORY_H
