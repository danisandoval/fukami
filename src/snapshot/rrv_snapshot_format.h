// rrv_snapshot_format.h — on-disk container for developer execution snapshots
// (.rrvsnap).  See docs/SNAPSHOTS.md for the design rationale; this header is
// the normative description of the bytes.
//
// Design constraints this format answers:
//
//   1. FORWARD/BACKWARD TOLERANT.  A snapshot is a developer artifact whose
//      producer (this runtime) changes daily.  The container is therefore a
//      flat list of self-describing chunks: a reader skips chunks it does not
//      know, and a writer may add chunks without invalidating old files.  Only
//      the chunks a restore *requires* (see kRequiredChunks in the reader) make
//      a file unusable when missing.
//
//   2. IDENTITY-CHECKED.  A snapshot is only meaningful against the exact guest
//      image it was taken from, and (for now) against a compatible runtime
//      build — the R5900Context blob is a raw struct dump.  META records the
//      ELF identity and the context struct size so a mismatch is a clear
//      error instead of a mysterious crash.
//
//   3. NO GAME BYTES IN GIT.  A snapshot contains 32 MB of game RAM and 4 MB of
//      GS VRAM: it is game-derived data in the same class as a .p2s or .gsr and
//      is never committed (see .gitignore).  The default store is local/, which
//      is already ignored wholesale.
//
// Layout:
//
//   FileHeader                      (64 bytes, little-endian)
//   ChunkHeader + payload           (repeated chunkCount times, 16-byte aligned)
//
// All integers are little-endian (the only hosts in play are ARM64/x86-64 LE).

#ifndef RRV_SNAPSHOT_FORMAT_H
#define RRV_SNAPSHOT_FORMAT_H

#include <cstddef>
#include <cstdint>

namespace rrv::snapshot {

inline constexpr char kMagic[8] = {'R', 'R', 'V', 'S', 'N', 'A', 'P', '\0'};

// Bump when a change breaks readers. Additive chunk work does NOT bump this.
inline constexpr uint32_t kFormatVersion = 1u;

inline constexpr uint32_t kFileHeaderBytes = 64u;
inline constexpr uint32_t kChunkHeaderBytes = 32u;
inline constexpr uint32_t kChunkAlign = 16u;

constexpr uint32_t fourcc(const char (&s)[5])
{
    return static_cast<uint32_t>(static_cast<unsigned char>(s[0])) |
           (static_cast<uint32_t>(static_cast<unsigned char>(s[1])) << 8) |
           (static_cast<uint32_t>(static_cast<unsigned char>(s[2])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(s[3])) << 24);
}

// --- Chunk identifiers -------------------------------------------------------
// Grouped by the subsystem that owns the state, because that is also the order
// in which a restore has to apply them (memory first, then devices, then the
// CPU/HLE state that points into memory).
namespace chunk {

// Text metadata (key=value lines, UTF-8, '\n' separated). Always first.
inline constexpr uint32_t kMeta = fourcc("META");

// Guest memory images — raw, exactly the size the runtime allocates.
inline constexpr uint32_t kEeRam = fourcc("EERM");      // 32 MB main RAM
inline constexpr uint32_t kScratchpad = fourcc("SPRM"); // 16 KB EE scratchpad
inline constexpr uint32_t kIopRam = fourcc("IOPR");     // 2 MB IOP RAM
inline constexpr uint32_t kVu0Code = fourcc("V0CD");    // 4 KB VU0 micro mem
inline constexpr uint32_t kVu0Data = fourcc("V0DT");    // 4 KB VU0 data mem
inline constexpr uint32_t kVu1Code = fourcc("V1CD");    // 16 KB VU1 micro mem
inline constexpr uint32_t kVu1Data = fourcc("V1DT");    // 16 KB VU1 data mem

// Device register state.
inline constexpr uint32_t kGsVram = fourcc("GSVR");    // 4 MB GS local memory
inline constexpr uint32_t kGsPrivRegs = fourcc("GSRG"); // 19 x uint64 GSRegisters
inline constexpr uint32_t kVifRegs = fourcc("VIFR");   // VIF0 then VIF1
inline constexpr uint32_t kDmaRegs = fourcc("DMAR");   // 10 x DMARegisters
inline constexpr uint32_t kIoRegs = fourcc("IORG");    // sparse map: (addr,value) pairs

// EE CPU state — raw R5900Context dump for the main game thread. Struct size is
// recorded in META; a size mismatch rejects the file.
inline constexpr uint32_t kEeContext = fourcc("EECX");

// HLE kernel object state (see docs/SNAPSHOTS.md §5 for what is and is not
// covered).  Guest threads carry one R5900Context each, inline.
inline constexpr uint32_t kThreads = fourcc("THRD");
inline constexpr uint32_t kSemaphores = fourcc("KSEM");
inline constexpr uint32_t kEventFlags = fourcc("KEVF");

// INTC/DMAC interrupt-handler tables. The guest registers these once during
// boot; a restore does not run boot, so without this chunk the tables are empty
// and every interrupt is silently dropped. Measured 2026-08-01: that is why a
// restored girl-scene run submitted no PATH3 display list at all (max packet
// 320 B) where a cold boot submits 1,845,152 B — the chain is launched from the
// GIF DMA-completion handler. Payload: IrqHandlerChunkHeader, then `count`
// fixed-size records.
inline constexpr uint32_t kIrqHandlers = fourcc("KIRQ");

// docs/DESIGN_GS_FIELD_MODEL.md M4. The emulated field state (fieldIndex, the
// sole counter Kernel/Syscalls/Interrupt.cpp owns -- see GsFieldState) plus
// the VSyncFlagRegistration the guest installed via SetVSyncFlag. Both are
// host-side state a restore cannot re-derive from guest memory: fieldIndex is
// a free-running host counter with no guest-visible backing store, and the
// registration is recorded in a host-side global the guest's boot-time
// SetVSyncFlag call fills exactly once, the same way AddIntcHandler fills
// KIRQ's tables. A NEW chunk rather than extending GSRG or KIRQ: the
// container is explicitly forward/backward tolerant (readers skip unknown
// chunks), so this cannot invalidate any snapshot captured before it existed
// -- it is simply absent from those, and a restore treats that exactly like
// a pre-KIRQ snapshot treats a missing KIRQ chunk (falls back to the fresh
// runtime's initial state, fieldIndex 0).
inline constexpr uint32_t kVSync = fourcc("VSYN");

} // namespace chunk

enum class Compression : uint32_t
{
    None = 0u,
};

#pragma pack(push, 1)

struct FileHeader
{
    char magic[8];           // kMagic
    uint32_t formatVersion;  // kFormatVersion
    uint32_t headerBytes;    // kFileHeaderBytes (lets a later version grow this)
    uint64_t createdUnixSec; // wall clock at capture, informational only
    uint32_t chunkCount;
    uint32_t flags;      // reserved, 0
    uint8_t reserved[32]; // zero-filled
};
static_assert(sizeof(FileHeader) == kFileHeaderBytes, "FileHeader must be 64 bytes");

struct ChunkHeader
{
    uint32_t id;                // chunk::k*
    uint32_t version;           // per-chunk payload version, starts at 1
    uint64_t storedBytes;       // bytes on disk (== rawBytes while Compression::None)
    uint64_t rawBytes;          // payload size after decompression
    uint32_t compression;       // Compression
    uint32_t crc32;             // CRC-32 (IEEE) of the STORED bytes
};
static_assert(sizeof(ChunkHeader) == kChunkHeaderBytes, "ChunkHeader must be 32 bytes");

// --- Chunk payload records ---------------------------------------------------
// These are POD records written back-to-back; each chunk's payload is
// `count * sizeof(record)`, with count derived from rawBytes. Keeping them
// fixed-size (no strings, no pointers) is what makes the format trivially
// forward-portable and byte-checkable.

// chunk::kIoRegs — one per live entry of PS2Memory::m_ioRegisters.
struct IoRegRecord
{
    uint32_t address;
    uint32_t value;
};

// chunk::kThreads — one per live HLE guest thread, followed inline by that
// thread's R5900Context blob (contextBytes, as recorded in META).
struct ThreadRecord
{
    int32_t tid;
    uint32_t entry;
    uint32_t stack;
    uint32_t stackSize;
    uint32_t gp;
    uint32_t priority;
    uint32_t currentPriority;
    uint32_t attr;
    uint32_t option;
    uint32_t arg;
    uint32_t tlsBase;
    int32_t status;    // THS_*
    int32_t waitType;  // TSW_*
    int32_t waitId;
    int32_t wakeupCount;
    int32_t suspendCount;
    uint32_t flags;    // bit0 = started, bit1 = ownsStack
    uint32_t reserved;
};

inline constexpr uint32_t kThreadFlagStarted = 1u << 0;
inline constexpr uint32_t kThreadFlagOwnsStack = 1u << 1;
// Set when only `tid` and the inline context are meaningful and every other
// field of the record is zero-filled. Format v1 writes this for every thread:
// the HLE ThreadInfo table lives inside the vendored kernel and is not exported
// yet (docs/SNAPSHOTS.md §7, S2). A restore refuses such records rather than
// resuming a thread with invented scheduling state.
inline constexpr uint32_t kThreadFlagContextOnly = 1u << 2;

// chunk::kSemaphores
struct SemaphoreRecord
{
    int32_t id;
    int32_t count;
    int32_t maxCount;
    int32_t initCount;
    int32_t waiters;
    uint32_t attr;
    uint32_t option;
    uint32_t reserved;
};

// chunk::kIrqHandlers — a fixed header followed by `count` IrqHandlerRecords.
// The counters travel with the records because AddIntcHandler/AddDmacHandler
// derive a new handler's id and dispatch order from them, so restoring the
// table without them would make a handler registered after the restore collide
// with one already in it. The final three words were reserved in the original
// KIRQ layout. They now describe optional async-callback stack watermarks while
// retaining the legacy 48-byte header and record offset.
inline constexpr uint32_t kIrqHandlerChunkFlagAsyncCallbackStackWatermarks = 1u << 0;
struct IrqHandlerChunkHeader
{
    uint32_t count;
    uint32_t enabledIntcMask;
    uint32_t enabledDmacMask;
    int32_t nextIntcHandlerId;
    int32_t nextDmacHandlerId;
    int32_t intcHeadOrder;
    int32_t intcTailOrder;
    int32_t dmacHeadOrder;
    int32_t dmacTailOrder;
    uint32_t flags;
    uint32_t asyncCallbackStackTop;
    uint32_t asyncCallbackStackFloor;
};
static_assert(sizeof(IrqHandlerChunkHeader) == 48u,
              "KIRQ header must retain its legacy record offset");

struct IrqHandlerRecord
{
    uint32_t table; // 0 = INTC, 1 = DMAC
    int32_t id;
    uint32_t cause;
    uint32_t handler; // guest PC of the handler function
    uint32_t arg;
    uint32_t gp;
    uint32_t sp;
    uint32_t enabled;
    int32_t order;
    uint32_t reserved;
};

// chunk::kVSync — a single fixed-size record, no header/count needed (there is
// exactly one field state and one VSyncFlagRegistration in this runtime).
struct VSyncStateRecord
{
    uint64_t fieldIndex;      // docs/DESIGN_GS_FIELD_MODEL.md GsFieldState.fieldIndex
    uint32_t vsyncFlagAddr;   // VSyncFlagRegistration.flagAddr
    uint32_t vsyncTickAddr;   // VSyncFlagRegistration.tickAddr
    uint32_t reserved[2];     // zero-filled; room to grow without a new chunk
};

// chunk::kEventFlags
struct EventFlagRecord
{
    int32_t id;
    uint32_t attr;
    uint32_t option;
    uint32_t initBits;
    uint32_t bits;
    int32_t waiters;
    uint32_t reserved[2];
};

#pragma pack(pop)

// CRC-32 (IEEE 802.3, reflected, init/final 0xFFFFFFFF) — the same polynomial
// zlib/PNG use. Implemented here so the module has no external dependency.
uint32_t crc32(const void *data, size_t bytes, uint32_t seed = 0u);

} // namespace rrv::snapshot

#endif // RRV_SNAPSHOT_FORMAT_H
