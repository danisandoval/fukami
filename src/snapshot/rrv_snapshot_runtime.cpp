// rrv_snapshot_runtime.cpp — capture and restore of a live PS2Runtime.
//
// This is the only translation unit in the snapshot module that knows what a
// PS2Runtime is. It implements the three hooks declared in rrv_snapshot_hooks.h
// plus configure() from rrv_snapshot.h.
//
// The safepoint argument (docs/SNAPSHOTS.md §3), in short:
//
//   Recompiled functions call each other as ordinary host calls, so the host
//   stack normally mirrors the guest call stack. That mirror is REDUNDANT: the
//   recompiler registers every return address (`jal` site + 8) and every loop
//   back-edge as a resumable entry point, and each generated function opens
//   with `switch (ctx->pc) { case <resume>: goto label_...; }`. A `jr $ra`
//   therefore just sets ctx->pc and returns; the dispatcher can pick the guest
//   up again from ctx->pc alone. Consequently a capture taken at the top of
//   PS2Runtime::dispatchLoop() — where the calling thread holds no recompiled
//   frames — is fully described by {guest memory, device state, R5900Context},
//   and restoring those and re-entering the dispatcher resumes execution.
//
//   Anywhere else it is not: a capture taken mid-call would need the host
//   frames back, and nothing can rebuild them. Hence exactly one capture site.

#include "rrv_snapshot.h"
#include "rrv_snapshot_file.h"
#include "rrv_snapshot_format.h"
#include "rrv_snapshot_hooks.h"

#include "ps2_runtime.h"
#include "ps2_syscalls.h"
#include "Syscalls/Interrupt.h" // INTC/DMAC handler-table export/import

// One deliberate cross-module call (see armReferenceRecorder below): a
// checkpoint and the GS recording used to validate it must start at the SAME
// logical instant, and only this module knows when that instant is.
#include "rrv_gs_record_hooks.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace rrv::snapshot {

namespace {

// PS2Memory::initialize() allocates a fixed 2 MB IOP RAM block and exports no
// size constant; mirror it here so a future change to either side shows up as a
// chunk-size mismatch at restore rather than a silent overrun.
constexpr size_t kIopRamBytes = 2u * 1024u * 1024u;

struct State
{
    // Armed flags — read on the dispatch hot path, so plain atomics.
    std::atomic<bool> anyEnabled{false};
    std::atomic<bool> saveArmed{false};
    std::atomic<bool> restoreArmed{false};
    std::atomic<bool> capturing{false};

    Options options;
    std::filesystem::path savePath;
    std::filesystem::path restorePath;
    GuestImageId guestImage;

    std::chrono::steady_clock::time_point guestStart{};
    std::atomic<bool> guestStartValid{false};

    // TriggerKind::Latch — set by requestCapture() from outside this module.
    std::atomic<bool> latchRequested{false};
    std::mutex latchReasonMutex;
    std::string latchReason;

    // --snapshot-trace. When a checkpoint is involved the trace only starts at
    // the checkpoint instant, so a continuous run's trace and a restored run's
    // trace cover the same window and can be diffed line by line.
    std::atomic<bool> traceEnabled{false};
    std::atomic<bool> traceGated{false};   // true => wait for the checkpoint
    std::atomic<bool> checkpointReached{false};
    std::ofstream trace;
    uint64_t traceSamples = 0u;
    uint64_t traceBaseTick = 0u;
    uint64_t traceLastTick = 0u;
    uint64_t traceBaseCounters[4] = {};
    bool traceBaseValid = false;

    // Live guest-thread contexts, published by the kernel's thread workers.
    // Read only from a thread that is itself parked at a safepoint and holding
    // the guest execution mutex.
    std::mutex threadsMutex;
    std::unordered_map<int32_t, R5900Context *> threads;
};

State &state()
{
    static State s;
    return s;
}

const char *chunkName(uint32_t id, char (&buf)[5])
{
    buf[0] = static_cast<char>(id & 0xFFu);
    buf[1] = static_cast<char>((id >> 8) & 0xFFu);
    buf[2] = static_cast<char>((id >> 16) & 0xFFu);
    buf[3] = static_cast<char>((id >> 24) & 0xFFu);
    buf[4] = '\0';
    return buf;
}

std::string hex(uint64_t v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%llx", static_cast<unsigned long long>(v));
    return buf;
}

// FNV-1a. Only ever compared against itself (run vs run), so any stable
// non-cryptographic hash does; this one needs no dependency and no buffering.
uint64_t fnv1a(const void *data, size_t bytes, uint64_t seed = 0xCBF29CE484222325ull)
{
    const auto *p = static_cast<const uint8_t *>(data);
    uint64_t h = seed;
    for (size_t i = 0; i < bytes; ++i)
    {
        h ^= p[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

// The reference GS recording for a checkpoint must begin at the same logical
// instant as the checkpoint itself, or a continuous-vs-restored comparison is
// comparing different windows. With RRV_GS_RECORD_ARM=1 the recorder writes
// nothing until it is triggered, so both a capture and a restore trigger it
// here — the two runs then start recording at exactly the same guest instant.
// No-op unless RRV_GS_RECORD + RRV_GS_RECORD_ARM are set.
void armReferenceRecorder(const char *what)
{
    if (!rrv::gsrecord::rrv_gs_record_armed())
    {
        return;
    }
    rrv::gsrecord::rrv_gs_record_trigger();
    std::cout << "[snapshot] armed GS recording at " << what << std::endl;
}

// --- per-frame state trace (--snapshot-trace) --------------------------------
//
// Sampled at a safepoint (never from another thread), once per vsync tick, so
// every line is a consistent view of guest-visible machine state. Two runs that
// execute the same guest work produce the same lines; the first differing line
// localises a divergence to one frame and one subsystem.
void writeTraceSample(State &s, PS2Runtime *runtime, R5900Context *ctx, uint64_t tick)
{
    PS2Memory &memory = runtime->memory();

    const uint64_t counters[4] = {memory.dmaStartCount(), memory.gifCopyCount(),
                                  memory.gsWriteCount(), memory.vifWriteCount()};
    if (!s.traceBaseValid)
    {
        s.traceBaseTick = tick;
        std::memcpy(s.traceBaseCounters, counters, sizeof(counters));
        s.traceBaseValid = true;
    }

    VIFRegisters vif[2] = {memory.vif0_regs, memory.vif1_regs};

    s.trace << "sample=" << s.traceSamples
            << " tick=" << (tick - s.traceBaseTick)
            << " pc=" << hex(ctx->pc)
            << " ctx=" << hex(fnv1a(ctx, sizeof(R5900Context)))
            << " dma=" << hex(fnv1a(memory.dma_regs, sizeof(memory.dma_regs)))
            << " vif=" << hex(fnv1a(vif, sizeof(vif)))
            << " vu0d=" << hex(fnv1a(memory.getVU0Data(), PS2_VU0_DATA_SIZE))
            << " vu1c=" << hex(fnv1a(memory.getVU1Code(), PS2_VU1_CODE_SIZE))
            << " vu1d=" << hex(fnv1a(memory.getVU1Data(), PS2_VU1_DATA_SIZE))
            << " gsreg=" << hex(fnv1a(&memory.gs(), sizeof(GSRegisters)))
            << " dmaStart=" << (counters[0] - s.traceBaseCounters[0])
            << " gifCopy=" << (counters[1] - s.traceBaseCounters[1])
            << " gsWrite=" << (counters[2] - s.traceBaseCounters[2])
            << " vifWrite=" << (counters[3] - s.traceBaseCounters[3])
            << "\n";
    ++s.traceSamples;
    if ((s.traceSamples & 0x3Fu) == 0u)
    {
        s.trace.flush();
    }
}

// --- capture ----------------------------------------------------------------

bool writeChunk(Writer &writer, uint32_t id, const void *data, size_t bytes)
{
    std::string error;
    if (writer.addChunk(id, 1u, data, bytes, &error))
    {
        return true;
    }
    char buf[5];
    std::cerr << "[snapshot] failed writing chunk '" << chunkName(id, buf) << "': " << error
              << "\n";
    return false;
}

bool performCapture(PS2Runtime *runtime, R5900Context *ctx)
{
    State &s = state();
    PS2Memory &memory = runtime->memory();

    // A PCSX2 GS checkpoint needs more than local VRAM: its GIF path parser,
    // transfer continuation and drawing registers are renderer-owned state.
    // ABI v3 deliberately exposes VRAM for GSR/diagnostics, not a complete
    // resumable GS freeze image. Refuse rather than create a misleading
    // checkpoint which can corrupt the first post-restore draw.
    if (runtime->activeGsBackend())
    {
        std::cerr << "[snapshot] capture refused: PCSX2 GS full-state freeze/restore "
                     "is not implemented (ABI v3 exposes local VRAM only)\n";
        return false;
    }

    // Anything still queued here is DMA the runtime has accepted but not yet
    // expanded. It is host-side state the snapshot does not model, so a capture
    // that lands on top of one is reported rather than silently lossy.
    const size_t pending = memory.m_pendingGifTransfers.size() +
                           memory.m_pendingVif0Transfers.size() +
                           memory.m_pendingVif1Transfers.size() +
                           memory.m_heldVif1Transfers.size();

    size_t liveThreads = 0u;
    std::vector<std::pair<int32_t, R5900Context *>> threadList;
    {
        std::lock_guard<std::mutex> lock(s.threadsMutex);
        threadList.assign(s.threads.begin(), s.threads.end());
        liveThreads = threadList.size();
    }

    Writer writer;
    std::string error;
    if (!writer.open(s.savePath, &error))
    {
        std::cerr << "[snapshot] " << error << "\n";
        return false;
    }

    MetaMap meta;
    meta["format"] = "rrvsnap";
    meta["format.version"] = std::to_string(kFormatVersion);
    meta["name"] = s.options.saveName;
    meta["elf.name"] = s.guestImage.fileName;
    meta["elf.bytes"] = std::to_string(s.guestImage.fileBytes);
    meta["elf.entry"] = hex(s.guestImage.entryPoint);
    meta["context.bytes"] = std::to_string(sizeof(R5900Context));
    {
        // Record WHAT produced this checkpoint, so a stored snapshot documents
        // its own trigger and can be reproduced.
        std::string trigger;
        switch (s.options.trigger)
        {
        case TriggerKind::Now: trigger = "now"; break;
        case TriggerKind::Vsync: trigger = "vsync:" + std::to_string(s.options.triggerValue); break;
        case TriggerKind::Seconds: trigger = "seconds:" + std::to_string(s.options.triggerValue); break;
        case TriggerKind::Pc: trigger = "pc:" + hex(s.options.triggerValue); break;
        case TriggerKind::Mem32:
            trigger = "mem32:" + hex(s.options.triggerValue) + "=" + hex(s.options.triggerValue2);
            break;
        case TriggerKind::Gif:
            trigger = "gif:" + std::to_string(s.options.triggerValue) + ":" +
                      std::to_string(s.options.triggerValue2);
            break;
        case TriggerKind::Latch:
        {
            std::lock_guard<std::mutex> lock(s.latchReasonMutex);
            trigger = "latch:" + (s.latchReason.empty() ? std::string("unnamed") : s.latchReason);
            break;
        }
        case TriggerKind::None: trigger = "none"; break;
        }
        meta["capture.trigger"] = trigger;
    }
    meta["capture.pc"] = hex(ctx->pc);
    meta["capture.vsync"] = std::to_string(ps2_syscalls::GetCurrentVSyncTick());
    meta["capture.insnCount"] = std::to_string(ctx->insn_count);
    meta["capture.guestThreads"] = std::to_string(liveThreads);
    meta["capture.pendingTransfers"] = std::to_string(pending);
    // Exported here rather than at the chunk below so the count can go into
    // META, which is written first.
    std::vector<ps2_syscalls::IrqHandlerSnapshotRecord> irqHandlers;
    ps2_syscalls::IrqHandlerSnapshotCounters irqCounters{};
    ps2_syscalls::exportIrqHandlerState(irqHandlers, irqCounters);
    const PS2Runtime::AsyncCallbackStackWatermarks asyncCallbackStackWatermarks =
        runtime->asyncCallbackStackWatermarks();
    meta["capture.irqHandlers"] = std::to_string(irqHandlers.size());
    meta["capture.asyncCallbackStackTop"] = hex(asyncCallbackStackWatermarks.top);
    meta["capture.asyncCallbackStackFloor"] = hex(asyncCallbackStackWatermarks.floor);
    meta["memory.eeRam"] = std::to_string(PS2_RAM_SIZE);
    meta["memory.gsVram"] = std::to_string(PS2_GS_VRAM_SIZE);
    if (pending != 0u)
    {
        meta["warning.pendingTransfers"] =
            "captured with queued DMA transfers; those are not restored";
    }
    if (liveThreads != 0u)
    {
        meta["warning.guestThreads"] =
            "captured with live guest threads; format v1 cannot resume them";
    }

    const std::string metaText = encodeMeta(meta);
    bool ok = writeChunk(writer, chunk::kMeta, metaText.data(), metaText.size());

    // Guest memory.
    ok = ok && writeChunk(writer, chunk::kEeRam, memory.getRDRAM(), PS2_RAM_SIZE);
    ok = ok && writeChunk(writer, chunk::kScratchpad, memory.getScratchpad(),
                          PS2_SCRATCHPAD_SIZE);
    if (memory.getIOPRAM())
    {
        ok = ok && writeChunk(writer, chunk::kIopRam, memory.getIOPRAM(), kIopRamBytes);
    }
    ok = ok && writeChunk(writer, chunk::kVu0Code, memory.getVU0Code(), PS2_VU0_CODE_SIZE);
    ok = ok && writeChunk(writer, chunk::kVu0Data, memory.getVU0Data(), PS2_VU0_DATA_SIZE);
    ok = ok && writeChunk(writer, chunk::kVu1Code, memory.getVU1Code(), PS2_VU1_CODE_SIZE);
    ok = ok && writeChunk(writer, chunk::kVu1Data, memory.getVU1Data(), PS2_VU1_DATA_SIZE);

    // Devices.
    ok = ok && writeChunk(writer, chunk::kGsVram, memory.getGSVRAM(), PS2_GS_VRAM_SIZE);
    ok = ok && writeChunk(writer, chunk::kGsPrivRegs, &memory.gs(), sizeof(GSRegisters));
    {
        VIFRegisters vif[2] = {memory.vif0_regs, memory.vif1_regs};
        ok = ok && writeChunk(writer, chunk::kVifRegs, vif, sizeof(vif));
    }
    ok = ok && writeChunk(writer, chunk::kDmaRegs, memory.dma_regs, sizeof(memory.dma_regs));
    {
        std::vector<IoRegRecord> ioRegs;
        ioRegs.reserve(memory.m_ioRegisters.size());
        for (const auto &[address, value] : memory.m_ioRegisters)
        {
            ioRegs.push_back(IoRegRecord{address, value});
        }
        // Sorted so two captures of the same state produce identical bytes.
        std::sort(ioRegs.begin(), ioRegs.end(),
                  [](const IoRegRecord &a, const IoRegRecord &b) { return a.address < b.address; });
        ok = ok && writeChunk(writer, chunk::kIoRegs, ioRegs.data(),
                              ioRegs.size() * sizeof(IoRegRecord));
    }

    // EE CPU state (the capturing thread is the main game thread).
    ok = ok && writeChunk(writer, chunk::kEeContext, ctx, sizeof(R5900Context));

    // Guest thread inventory. Format v1 records tid + context only; see
    // kThreadFlagContextOnly.
    {
        std::vector<uint8_t> payload;
        payload.reserve(threadList.size() * (sizeof(ThreadRecord) + sizeof(R5900Context)));
        for (const auto &[tid, threadCtx] : threadList)
        {
            ThreadRecord record{};
            record.tid = tid;
            record.flags = kThreadFlagContextOnly;
            const auto *recordBytes = reinterpret_cast<const uint8_t *>(&record);
            payload.insert(payload.end(), recordBytes, recordBytes + sizeof(record));
            const auto *ctxBytes = reinterpret_cast<const uint8_t *>(threadCtx);
            payload.insert(payload.end(), ctxBytes, ctxBytes + sizeof(R5900Context));
        }
        ok = ok && writeChunk(writer, chunk::kThreads, payload.data(), payload.size());
    }

    // INTC/DMAC handler tables. These are pure host-side HLE state that the
    // guest fills once during boot, so a restore cannot reconstruct them from
    // guest memory the way it can for everything above — see chunk::kIrqHandlers.
    {
        const auto &handlers = irqHandlers;
        const auto &counters = irqCounters;

        IrqHandlerChunkHeader header{};
        header.count = static_cast<uint32_t>(handlers.size());
        header.enabledIntcMask = counters.enabledIntcMask;
        header.enabledDmacMask = counters.enabledDmacMask;
        header.nextIntcHandlerId = counters.nextIntcHandlerId;
        header.nextDmacHandlerId = counters.nextDmacHandlerId;
        header.intcHeadOrder = counters.intcHeadOrder;
        header.intcTailOrder = counters.intcTailOrder;
        header.dmacHeadOrder = counters.dmacHeadOrder;
        header.dmacTailOrder = counters.dmacTailOrder;
        header.flags = kIrqHandlerChunkFlagAsyncCallbackStackWatermarks;
        header.asyncCallbackStackTop = asyncCallbackStackWatermarks.top;
        header.asyncCallbackStackFloor = asyncCallbackStackWatermarks.floor;

        std::vector<uint8_t> payload;
        payload.reserve(sizeof(header) + handlers.size() * sizeof(IrqHandlerRecord));
        const auto *headerBytes = reinterpret_cast<const uint8_t *>(&header);
        payload.insert(payload.end(), headerBytes, headerBytes + sizeof(header));
        for (const auto &h : handlers)
        {
            IrqHandlerRecord record{};
            record.table = h.table;
            record.id = h.id;
            record.cause = h.cause;
            record.handler = h.handler;
            record.arg = h.arg;
            record.gp = h.gp;
            record.sp = h.sp;
            record.enabled = h.enabled;
            record.order = h.order;
            const auto *recordBytes = reinterpret_cast<const uint8_t *>(&record);
            payload.insert(payload.end(), recordBytes, recordBytes + sizeof(record));
        }
        ok = ok && writeChunk(writer, chunk::kIrqHandlers, payload.data(), payload.size());
    }

    // docs/DESIGN_GS_FIELD_MODEL.md M4: the emulated field state + the
    // VSyncFlagRegistration, both host-side state a restore cannot re-derive
    // from guest memory alone -- the same class of gap kIrqHandlers closes
    // above, and for the same reason (the guest registered it once, during a
    // boot a restored process never runs).
    {
        const ps2_syscalls::GsFieldState fieldState = ps2_syscalls::GetCurrentField();
        const ps2_syscalls::interrupt_state::VSyncFlagRegistration reg =
            ps2_syscalls::GetVSyncFlagRegistration();
        VSyncStateRecord record{};
        record.fieldIndex = fieldState.fieldIndex;
        record.vsyncFlagAddr = reg.flagAddr;
        record.vsyncTickAddr = reg.tickAddr;
        ok = ok && writeChunk(writer, chunk::kVSync, &record, sizeof(record));
    }

    if (!ok)
    {
        writer.abort();
        return false;
    }
    if (!writer.finish(&error))
    {
        std::cerr << "[snapshot] " << error << "\n";
        return false;
    }

    std::cout << "[snapshot] saved '" << s.options.saveName << "' -> " << s.savePath.string()
              << " (pc=0x" << hex(ctx->pc) << " vsync=" << meta["capture.vsync"]
              << " threads=" << liveThreads << " pendingDma=" << pending << ")" << std::endl;
    return true;
}

// --- restore ----------------------------------------------------------------

bool checkGuestImage(const Reader &reader, std::string &error)
{
    const State &s = state();
    const MetaMap &meta = reader.meta();

    auto get = [&](const char *key) -> std::string {
        const auto it = meta.find(key);
        return it == meta.end() ? std::string() : it->second;
    };

    const std::string contextBytes = get("context.bytes");
    if (!contextBytes.empty() && contextBytes != std::to_string(sizeof(R5900Context)))
    {
        error = "snapshot was taken by a build whose R5900Context is " + contextBytes +
                " bytes; this build's is " + std::to_string(sizeof(R5900Context)) +
                ". Re-capture with this build.";
        return false;
    }

    const std::string elfBytes = get("elf.bytes");
    if (!elfBytes.empty() && s.guestImage.fileBytes != 0u &&
        elfBytes != std::to_string(s.guestImage.fileBytes))
    {
        error = "snapshot was taken from a different guest image (" + get("elf.name") + ", " +
                elfBytes + " bytes)";
        return false;
    }
    return true;
}

bool applyRestore(PS2Runtime *runtime)
{
    State &s = state();

    if (runtime->activeGsBackend())
    {
        std::cerr << "[snapshot] restore refused: PCSX2 GS full-state freeze/restore "
                     "is not implemented (ABI v3 exposes local VRAM only)\n";
        return false;
    }

    Reader reader;
    std::string error;
    if (!reader.load(s.restorePath, &error))
    {
        std::cerr << "[snapshot] restore failed: " << error << "\n";
        return false;
    }
    if (!checkGuestImage(reader, error))
    {
        std::cerr << "[snapshot] restore refused: " << error << "\n";
        return false;
    }

    PS2Memory &memory = runtime->memory();

    // Guest memory. EE RAM and the CPU context are required; everything else is
    // applied when present so an older snapshot still boots.
    if (!reader.copyChunkInto(chunk::kEeRam, memory.getRDRAM(), PS2_RAM_SIZE, &error) ||
        !reader.copyChunkInto(chunk::kEeContext, &runtime->cpu(), sizeof(R5900Context), &error))
    {
        std::cerr << "[snapshot] restore failed: " << error << "\n";
        return false;
    }

    auto optional = [&](uint32_t id, void *dst, size_t bytes) {
        if (reader.find(id) == nullptr)
        {
            return;
        }
        std::string localError;
        if (!reader.copyChunkInto(id, dst, bytes, &localError))
        {
            std::cerr << "[snapshot] warning: " << localError << " (chunk skipped)\n";
        }
    };

    optional(chunk::kScratchpad, memory.getScratchpad(), PS2_SCRATCHPAD_SIZE);
    if (memory.getIOPRAM())
    {
        optional(chunk::kIopRam, memory.getIOPRAM(), kIopRamBytes);
    }
    optional(chunk::kVu0Code, memory.getVU0Code(), PS2_VU0_CODE_SIZE);
    optional(chunk::kVu0Data, memory.getVU0Data(), PS2_VU0_DATA_SIZE);
    optional(chunk::kVu1Code, memory.getVU1Code(), PS2_VU1_CODE_SIZE);
    optional(chunk::kVu1Data, memory.getVU1Data(), PS2_VU1_DATA_SIZE);
    optional(chunk::kGsVram, memory.getGSVRAM(), PS2_GS_VRAM_SIZE);
    optional(chunk::kGsPrivRegs, &memory.gs(), sizeof(GSRegisters));
    optional(chunk::kDmaRegs, memory.dma_regs, sizeof(memory.dma_regs));
    {
        VIFRegisters vif[2] = {};
        if (reader.find(chunk::kVifRegs) != nullptr)
        {
            std::string localError;
            if (reader.copyChunkInto(chunk::kVifRegs, vif, sizeof(vif), &localError))
            {
                memory.vif0_regs = vif[0];
                memory.vif1_regs = vif[1];
            }
            else
            {
                std::cerr << "[snapshot] warning: " << localError << " (chunk skipped)\n";
            }
        }
    }

    if (const Reader::Chunk *ioRegs = reader.find(chunk::kIoRegs))
    {
        const size_t count = ioRegs->data.size() / sizeof(IoRegRecord);
        const auto *records = reinterpret_cast<const IoRegRecord *>(ioRegs->data.data());
        memory.m_ioRegisters.clear();
        for (size_t i = 0; i < count; ++i)
        {
            memory.m_ioRegisters[records[i].address] = records[i].value;
        }
    }

    // Guest threads. Format v1 records their contexts but cannot re-create the
    // kernel-side scheduling state, so a snapshot that has any is refused
    // instead of resumed into a half-built thread table (docs/SNAPSHOTS.md §7).
    if (const Reader::Chunk *threads = reader.find(chunk::kThreads))
    {
        const size_t stride = sizeof(ThreadRecord) + sizeof(R5900Context);
        const size_t count = stride == 0u ? 0u : threads->data.size() / stride;
        if (count != 0u)
        {
            std::cerr << "[snapshot] restore refused: snapshot contains " << count
                      << " live guest thread(s); this build can only resume the main game "
                         "thread (docs/SNAPSHOTS.md §7).\n";
            return false;
        }
    }

    // INTC/DMAC handler tables. Without these the restored process has an empty
    // handler table — the guest registered its handlers during a boot this
    // process never ran — and every interrupt is dropped silently, which for RRV
    // means the DMA-completion handler that launches the frame's PATH3 display
    // list never runs. A snapshot written before this chunk existed cannot be
    // resumed faithfully, so say so rather than reproducing that failure.
    {
        // Restoring the table is correct but NOT YET SUFFICIENT, so it is opt-in
        // (RRV_SNAPSHOT_RESTORE_IRQ=1) until the second half lands. Measured
        // 2026-08-01: with the table restored the guest's DMA-completion handler
        // does run — sub_0021FE88 appears in the dispatch trace and launches the
        // successor chain — but the main game thread then reaches pc=0 with
        // sp=<async-handler stack> inside the function the handler interrupted,
        // i.e. the nested handler clobbers the interrupted context, and the
        // thread exits after one present. A cold boot runs the identical handler
        // for 300+ presents with zero pc-zero events, so the missing piece is
        // more host-side state the restore does not rebuild, not the table.
        // Default-off keeps every validated snapshot result unchanged.
        static const bool restoreIrq = [] {
            const char *v = std::getenv("RRV_SNAPSHOT_RESTORE_IRQ");
            return v && v[0] && std::strcmp(v, "0") != 0;
        }();

        const Reader::Chunk *irq = reader.find(chunk::kIrqHandlers);
        if (!irq || irq->data.size() < sizeof(IrqHandlerChunkHeader))
        {
            std::cerr << "[snapshot] WARNING: no interrupt-handler table in this snapshot "
                         "(pre-KIRQ capture). Guest INTC/DMAC handlers will not run, so "
                         "any display list launched from a DMA-completion handler is lost "
                         "— re-capture the checkpoint. See docs/SNAPSHOTS.md §7.\n";
        }
        else if (!restoreIrq)
        {
            std::cerr << "[snapshot] NOTE: guest INTC/DMAC handlers are NOT restored "
                         "(RRV_SNAPSHOT_RESTORE_IRQ=1 to try). Any display list the game "
                         "launches from a DMA-completion handler is absent from this run — "
                         "for RRV's attract character that is the whole PATH3 stream. Use a "
                         "cold boot for scenes that depend on it. See docs/SNAPSHOTS.md §7.\n";
        }
        else
        {
            IrqHandlerChunkHeader header{};
            std::memcpy(&header, irq->data.data(), sizeof(header));

            // KIRQ's original three reserved words were all zero. A missing
            // watermark flag therefore deliberately keeps the freshly-loaded
            // runtime's allocator state, preserving old-snapshot behaviour.
            if ((header.flags & kIrqHandlerChunkFlagAsyncCallbackStackWatermarks) != 0u &&
                !runtime->restoreAsyncCallbackStackWatermarks(
                    {header.asyncCallbackStackTop, header.asyncCallbackStackFloor}))
            {
                std::cerr << "[snapshot] warning: invalid async callback-stack watermarks "
                             "in KIRQ (fresh allocator state retained)\n";
            }
            const size_t available =
                (irq->data.size() - sizeof(header)) / sizeof(IrqHandlerRecord);
            const size_t count = std::min<size_t>(header.count, available);

            std::vector<ps2_syscalls::IrqHandlerSnapshotRecord> handlers;
            handlers.reserve(count);
            for (size_t i = 0; i < count; ++i)
            {
                IrqHandlerRecord record{};
                std::memcpy(&record,
                            irq->data.data() + sizeof(header) + i * sizeof(IrqHandlerRecord),
                            sizeof(record));
                handlers.push_back(ps2_syscalls::IrqHandlerSnapshotRecord{
                    record.table, record.id, record.cause, record.handler, record.arg,
                    record.gp, record.sp, record.enabled, record.order});
            }

            ps2_syscalls::IrqHandlerSnapshotCounters counters{};
            counters.enabledIntcMask = header.enabledIntcMask;
            counters.enabledDmacMask = header.enabledDmacMask;
            counters.nextIntcHandlerId = header.nextIntcHandlerId;
            counters.nextDmacHandlerId = header.nextDmacHandlerId;
            counters.intcHeadOrder = header.intcHeadOrder;
            counters.intcTailOrder = header.intcTailOrder;
            counters.dmacHeadOrder = header.dmacHeadOrder;
            counters.dmacTailOrder = header.dmacTailOrder;
            ps2_syscalls::importIrqHandlerState(handlers, counters);
        }
    }

    // docs/DESIGN_GS_FIELD_MODEL.md M4: field state + VSyncFlagRegistration.
    // Wires up SetCurrentVSyncTickForReplay(), previously dead code (defined,
    // exported, called from nowhere) -- without this a restored run silently
    // restarted the field sequence at fieldIndex 0 with guest state captured
    // at an arbitrary other fieldIndex, and signalVSyncFlag's guest-RAM writes
    // went to whatever the fresh runtime's (empty) registration pointed at,
    // i.e. nowhere.
    {
        if (const Reader::Chunk *vsync = reader.find(chunk::kVSync))
        {
            if (vsync->data.size() >= sizeof(VSyncStateRecord))
            {
                VSyncStateRecord record{};
                std::memcpy(&record, vsync->data.data(), sizeof(record));
                ps2_syscalls::SetCurrentVSyncTickForReplay(record.fieldIndex);
                ps2_syscalls::SetVSyncFlagRegistrationForReplay(
                    ps2_syscalls::interrupt_state::VSyncFlagRegistration{
                        record.vsyncFlagAddr, record.vsyncTickAddr});
            }
            else
            {
                std::cerr << "[snapshot] warning: VSYN chunk truncated (field state not restored)\n";
            }
        }
        else
        {
            std::cerr << "[snapshot] NOTE: no field-state chunk (VSYN) in this snapshot "
                         "(pre-M4 capture). fieldIndex restarts at 0 and the guest's VSync "
                         "flag/tick registration is not restored -- re-capture the checkpoint. "
                         "See docs/SNAPSHOTS.md.\n";
        }
    }

    const auto &meta = reader.meta();
    const auto pcIt = meta.find("capture.pc");
    std::cout << "[snapshot] restored '" << s.options.restoreName << "' from "
              << s.restorePath.string() << " (pc=0x"
              << (pcIt == meta.end() ? hex(runtime->cpu().pc) : pcIt->second) << ")"
              << std::endl;
    return true;
}

} // namespace

// --- public API --------------------------------------------------------------

bool configure(const Options &options, std::string *error)
{
    State &s = state();
    s.options = options;
    s.guestImage = identifyGuestImage(options.elf);

    if (!options.saveName.empty())
    {
        s.savePath = resolveSnapshotPath(options.storeDir, options.saveName);
        s.saveArmed.store(true, std::memory_order_relaxed);
    }

    if (!options.restoreName.empty())
    {
        s.restorePath = resolveSnapshotPath(options.storeDir, options.restoreName);
        std::error_code ec;
        if (!std::filesystem::exists(s.restorePath, ec))
        {
            if (error)
            {
                *error = "snapshot '" + options.restoreName + "' not found (" +
                         s.restorePath.string() + ")";
            }
            return false;
        }
        s.restoreArmed.store(true, std::memory_order_relaxed);
    }

    if (!options.tracePath.empty())
    {
        std::error_code ec;
        if (options.tracePath.has_parent_path())
        {
            std::filesystem::create_directories(options.tracePath.parent_path(), ec);
        }
        s.trace.open(options.tracePath, std::ios::trunc);
        if (!s.trace)
        {
            if (error)
            {
                *error = "cannot open trace file " + options.tracePath.string();
            }
            return false;
        }
        s.trace << "# rrv snapshot state trace — one sample per vsync tick, taken at a\n"
                   "# dispatch safepoint. Compare two runs line by line; the first\n"
                   "# differing line localises a divergence to one frame and subsystem.\n";
        s.traceEnabled.store(true, std::memory_order_relaxed);
        // A trace that accompanies a checkpoint starts AT the checkpoint; a
        // standalone trace starts immediately.
        s.traceGated.store(!options.saveName.empty() || !options.restoreName.empty(),
                           std::memory_order_relaxed);
    }

    s.anyEnabled.store(s.saveArmed.load(std::memory_order_relaxed) ||
                           s.restoreArmed.load(std::memory_order_relaxed) ||
                           s.traceEnabled.load(std::memory_order_relaxed),
                       std::memory_order_release);
    return true;
}

bool ownsRecorderArming()
{
    const State &s = state();
    return s.saveArmed.load(std::memory_order_relaxed) ||
           s.restoreArmed.load(std::memory_order_relaxed);
}

// Arm the latch that the safepoint checks. Shared by the two things allowed to
// set it: guest code at a scene predicate (requestCapture) and the GIF ingress
// hook (hookGifPacket). Each caller is responsible for checking that ITS OWN
// trigger kind is the configured one before calling — see requestCapture.
static void armLatch(State &s, const char *reason)
{
    if (s.latchRequested.load(std::memory_order_relaxed))
    {
        return; // already armed; first request wins, so the point is well defined
    }
    {
        std::lock_guard<std::mutex> lock(s.latchReasonMutex);
        if (s.latchReason.empty() && reason)
        {
            s.latchReason = reason;
        }
    }
    s.latchRequested.store(true, std::memory_order_release);
}

void requestCapture(const char *reason)
{
    State &s = state();
    // Only a `latch` trigger may be armed from guest code. Accepting
    // TriggerKind::Gif here as well (as this did until 2026-08-01) let the
    // scene predicate in src/patches.cpp arm a `gif:<path>:<bytes>` capture,
    // so a display-list-signature checkpoint could silently be taken at the
    // scene anchor instead — a checkpoint whose recorded trigger describes a
    // packet that was never seen. Measured: `--snapshot-at gif:3:1500000`
    // captured at vsync=1 with no PATH3 packet of that size anywhere in the
    // run. hookGifPacket arms the same latch after checking for Gif itself.
    if (!s.saveArmed.load(std::memory_order_relaxed) ||
        s.options.trigger != TriggerKind::Latch)
    {
        return;
    }
    armLatch(s, reason);
}

bool enabled()
{
    return state().anyEnabled.load(std::memory_order_relaxed);
}

bool restorePending()
{
    return state().restoreArmed.load(std::memory_order_relaxed);
}

void hookRunStart(PS2Runtime *runtime)
{
    State &s = state();
    if (!runtime || !s.anyEnabled.load(std::memory_order_acquire))
    {
        return;
    }

    s.guestStart = std::chrono::steady_clock::now();
    s.guestStartValid.store(true, std::memory_order_release);

    if (!s.restoreArmed.load(std::memory_order_relaxed))
    {
        return;
    }

    if (!applyRestore(runtime))
    {
        // A failed restore must not silently fall through to a normal boot: the
        // developer asked for a specific state and would otherwise spend a while
        // wondering why the game is at the title screen.
        std::cerr << "[snapshot] aborting: --snapshot could not be honoured\n";
        runtime->requestStop();
    }
    else
    {
        // Same instant as the capture-side call, so a continuous run and a
        // restored run produce reference recordings of the same window.
        armReferenceRecorder("restore");
        s.checkpointReached.store(true, std::memory_order_release);
    }
    s.restoreArmed.store(false, std::memory_order_relaxed);
}

void hookDispatchBoundary(PS2Runtime *runtime, R5900Context *ctx)
{
    State &s = state();
    if (!runtime || !ctx)
    {
        return;
    }

    // --snapshot-trace. Throttled the same way as the clock triggers: the tick
    // read takes a mutex, so it is sampled once per 4096 safepoints, which is
    // still far finer than the ~60 Hz sampling rate we actually want.
    if (s.traceEnabled.load(std::memory_order_relaxed) &&
        (!s.traceGated.load(std::memory_order_relaxed) ||
         s.checkpointReached.load(std::memory_order_acquire)))
    {
        // Finer than the capture triggers' throttle: a comparison trace is only
        // useful at frame resolution, and the guest reaches far fewer safepoints
        // per frame in wait-heavy phases than the trigger throttle assumes.
        static thread_local uint64_t tracePoll = 0u;
        if ((++tracePoll & 0xFFu) == 0u)
        {
            const uint64_t tick = ps2_syscalls::GetCurrentVSyncTick();
            if (tick != s.traceLastTick)
            {
                s.traceLastTick = tick;
                PS2Runtime::GuestExecutionScope freeze(runtime);
                writeTraceSample(s, runtime, ctx, tick);
            }
        }
    }

    if (!s.saveArmed.load(std::memory_order_relaxed))
    {
        return;
    }

    // Trigger evaluation. The PC trigger is a register compare and is checked
    // every time; the others touch a clock or a mutex-guarded counter, so they
    // are sampled once every kPollMask+1 safepoints. At ~10^6 dispatches/second
    // that is still sub-millisecond resolution.
    constexpr uint64_t kPollMask = 0xFFFu;
    bool fire = false;
    switch (s.options.trigger)
    {
    case TriggerKind::Now:
        fire = true;
        break;
    case TriggerKind::Pc:
        fire = (static_cast<uint64_t>(ctx->pc) == s.options.triggerValue);
        break;
    case TriggerKind::Latch:
    case TriggerKind::Gif:
        // Both are set from outside the safepoint (game code / the GIF ingress
        // hook); the capture itself still happens here, where it is valid.
        fire = s.latchRequested.load(std::memory_order_acquire);
        break;
    case TriggerKind::Mem32:
    {
        // Direct RDRAM read: this runs at every safepoint, and the address is a
        // developer-supplied constant, so the full load path (exceptions, MMIO
        // decode) would be both wasteful and wrong here.
        const uint8_t *rdram = runtime->memory().getRDRAM();
        if (rdram)
        {
            uint32_t word = 0u;
            std::memcpy(&word, rdram + (static_cast<uint32_t>(s.options.triggerValue) &
                                        (PS2_RAM_MASK & ~3u)),
                        sizeof(word));
            fire = (static_cast<uint64_t>(word) == s.options.triggerValue2);
        }
        break;
    }
    case TriggerKind::Vsync:
    case TriggerKind::Seconds:
    {
        static thread_local uint64_t poll = 0u;
        if ((++poll & kPollMask) != 0u)
        {
            return;
        }
        if (s.options.trigger == TriggerKind::Vsync)
        {
            fire = ps2_syscalls::GetCurrentVSyncTick() >= s.options.triggerValue;
        }
        else if (s.guestStartValid.load(std::memory_order_acquire))
        {
            const auto elapsed = std::chrono::steady_clock::now() - s.guestStart;
            fire = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >=
                   static_cast<int64_t>(s.options.triggerValue);
        }
        break;
    }
    case TriggerKind::None:
        break;
    }

    if (!fire)
    {
        return;
    }

    bool expected = false;
    if (!s.capturing.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
    {
        return; // another thread got here first
    }

    // Freeze the rest of the guest. Every other guest thread is then either
    // blocked at its own safepoint waiting for this mutex, or parked inside an
    // HLE wait (which releases it, see GuestExecutionReleaseScope). Either way
    // nothing mutates guest memory while the image is written out.
    bool captured = false;
    {
        PS2Runtime::GuestExecutionScope freeze(runtime);
        captured = performCapture(runtime, ctx);
        if (captured)
        {
            // Inside the freeze, so the reference recording starts at exactly
            // the captured instant with no guest work in between.
            armReferenceRecorder("capture");
            s.checkpointReached.store(true, std::memory_order_release);
        }
    }

    s.saveArmed.store(false, std::memory_order_relaxed);
    s.anyEnabled.store(s.restoreArmed.load(std::memory_order_relaxed) ||
                           s.traceEnabled.load(std::memory_order_relaxed),
                       std::memory_order_release);
    s.capturing.store(false, std::memory_order_release);

    if (s.options.exitAfterSave)
    {
        std::cout << "[snapshot] --snapshot-exit: stopping the run" << std::endl;
        runtime->requestStop();
    }
}

void hookGifPacket(uint8_t pathId, uint32_t sizeBytes)
{
    State &s = state();
    if (!s.saveArmed.load(std::memory_order_relaxed) ||
        s.options.trigger != TriggerKind::Gif)
    {
        return;
    }
    const uint64_t wantPath = s.options.triggerValue;
    if (wantPath != 0u && static_cast<uint64_t>(pathId) != wantPath)
    {
        return;
    }
    if (static_cast<uint64_t>(sizeBytes) < s.options.triggerValue2)
    {
        return;
    }
    // Arm the same latch the scene anchors use: this runs on the guest thread
    // deep inside DMA expansion, which is emphatically not a safepoint. Arms
    // directly rather than through requestCapture(), which is the guest-code
    // entry point and only accepts a `latch` trigger.
    armLatch(s, "gif-signature");
}

void hookGuestThreadStart(int32_t tid, R5900Context *ctx)
{
    State &s = state();
    if (!s.anyEnabled.load(std::memory_order_relaxed) || !ctx)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(s.threadsMutex);
    s.threads[tid] = ctx;
}

void hookGuestThreadExit(int32_t tid)
{
    State &s = state();
    if (!s.anyEnabled.load(std::memory_order_relaxed))
    {
        return;
    }
    std::lock_guard<std::mutex> lock(s.threadsMutex);
    s.threads.erase(tid);
}

} // namespace rrv::snapshot
