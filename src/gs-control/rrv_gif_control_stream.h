// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
// GIF tag progression adapted from PCSX2 fd9d310ccbb6b8b62c976da8886a3c8fd3a10ff3,
// pcsx2/Gif_Unit.h: Gif_Tag, Gif_Path::ExecuteGSPacket, Gif_Unit::Execute.
#pragma once

#include "rrv_gs_control.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace rrv::gs {

// Guest-context only: submit/drain/control callbacks must share the runtime's
// guest execution serialization. Sink is synchronous and must not run guest
// callbacks. A retained Sink must own any provenance it captures by value.
//
// This gate is downstream of existing path arbitration. Once a path starts a
// packet, its continuation is pinned until EOP; later records of other paths
// stay ordered. In particular, no IMAGE mid-tag preemption is supported by the
// flattened renderer Transfer<3> stream: that would require synthetic tags.
class Stream {
public:
    using Sink = std::function<void(const uint8_t*, uint32_t)>;
    static constexpr uint64_t CapacityBytes = 64ULL * 1024 * 1024;
    struct Stats {
        uint64_t submittedBytes = 0, consumedBytes = 0;
        uint64_t submittedRecords = 0, consumedRecords = 0;
        uint64_t pendingBytes = 0, highWaterBytes = 0;
        uint64_t highWaterRecords = 0, stalls = 0;
    };
    struct Cursor {
        uint8_t activePath = 0, format = 0, registerIndex = 0;
        uint32_t remainingQwords = 0;
        bool tagValid = false, eop = false;
    };

    explicit Stream(Control& control) : control_(control) {}
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

    // Full owned admission or exception, never a blocking host wait. The bound
    // charges complete retained allocations, including an already sent prefix.
    // Oversized/full admission fails before control state changes; runtime DMA
    // gating should leave not-yet-accepted transfers pending instead of retrying
    // a partially accepted record. Empty records have no observable effect.
    void submit(uint8_t path, const uint8_t* data, uint32_t size, Sink sink) {
        enqueue(path, data, size, std::move(sink));
        drain();
    }

    // Admit a complete arbiter batch before drain so FINISH eligibility sees
    // every other path already accepted in that batch.
    void enqueue(uint8_t path, const uint8_t* data, uint32_t size, Sink sink) {
        ensureHealthy();
        if (path < 1 || path > 3 || (size & 15) || (size && (!data || !sink)))
            throw std::invalid_argument("GS control stream requires path 1..3 and whole GIF qwords");
        if (!size) return;
        if (size > CapacityBytes || size > CapacityBytes - stats_.pendingBytes)
            throw std::length_error("GS control pending payload exceeds 64 MiB");
        auto record = std::make_shared<Record>(Record{path,
            std::vector<uint8_t>(data, data + size), 0, 0, std::move(sink)});
        records_.push_back(std::move(record));
        stats_.submittedBytes += size;
        ++stats_.submittedRecords;
        stats_.pendingBytes += size;
        stats_.highWaterBytes = std::max(stats_.highWaterBytes, stats_.pendingBytes);
        stats_.highWaterRecords = std::max(stats_.highWaterRecords,
            stats_.submittedRecords - stats_.consumedRecords);
    }

    // CSR SIGNAL acknowledgement invokes this in guest context. CSR RESET
    // deliberately does not discard or rewind GIF state (upstream gsCSRwrite
    // resets GS event state, not Gif_Path); resume remains an explicit caller
    // action. There is no transport-reset method which could lose owned work.
    void drain() {
        ensureHealthy();
        // CanDoGif() rejects entry before Execute's FINISH test. A SIGNAL
        // newly encountered during this call still reaches that test below.
        if (control_.stalled()) return;
        if (draining_) throw std::logic_error("reentrant GS control stream drain");
        draining_ = true;
        try {
            // A double SIGNAL at a non-EOP tag's final word returns before
            // ExecuteGSPacket invalidates that tag. Acknowledgement completes
            // this zero-remaining cursor even if no continuation bytes exist.
            if (cursor_.tagValid && !cursor_.remainingQwords) completeTag();
            while (!control_.stalled()) {
                auto record = records_.begin();
                if (cursor_.activePath)
                    record = std::find_if(records_.begin(), records_.end(),
                        [&](const auto& r) { return r->path == cursor_.activePath; });
                if (record == records_.end()) break;
                consume(*record);
                if ((*record)->offset == (*record)->data.size()) {
                    records_.erase(record);
                }
            }
            // Pinned FlushToMTGS refuses an incomplete tag. This is essential
            // across CSR RESET: renderer reset clears its GIF parser, while
            // producer GIF cursor and staged packet bytes survive that reset.
            if (!cursor_.tagValid) flush();
            bool otherPathsPending = false;
            for (uint8_t path = 1; path <= 3; ++path)
                otherPathsPending |= path != cursor_.activePath && pendingPath(path);
            // Mirrors Execute's checkPaths(APATH != n, ..., true) boundary,
            // not admission, GS consumption, or GPU completion.
            control_.finishEligible(otherPathsPending);
            draining_ = false;
        } catch (...) {
            // Control events may already be committed before a failing sink.
            // Never replay that prefix on a subsequent call.
            failed_ = true;
            draining_ = false;
            throw;
        }
    }

    bool pending() const { return cursor_.activePath || !records_.empty() || !staged_.empty(); }
    bool pendingPath(uint8_t path) const {
        if (path < 1 || path > 3) return false;
        return cursor_.activePath == path || std::any_of(records_.begin(), records_.end(),
            [path](const auto& r) { return r->path == path; });
    }
    const Stats& stats() const { return stats_; }
    const Cursor& cursor() const { return cursor_; }

private:
    struct Record {
        uint8_t path;
        std::vector<uint8_t> data;
        uint32_t offset;
        uint32_t emitted;
        Sink sink;
    };
    struct Segment {
        std::shared_ptr<Record> record;
        uint32_t offset, size;
    };
    static uint64_t load64(const uint8_t* data) {
        uint64_t result;
        std::memcpy(&result, data, sizeof(result));
        return result;
    }
    void ensureHealthy() const {
        if (failed_) throw std::runtime_error("GS control stream previously failed");
    }
    void loadTag(const uint8_t* data, uint8_t path) {
        const uint64_t tag = load64(data);
        cursor_.activePath = path;
        cursor_.eop = (tag >> 15) & 1;
        cursor_.format = (tag >> 58) & 3;
        cursor_.registerIndex = 0;
        nregs_ = (tag >> 60) & 15;
        if (!nregs_) nregs_ = 16;
        const uint32_t loops = tag & 0x7fff;
        cursor_.remainingQwords = cursor_.format == 0 ? loops * nregs_ :
            cursor_.format == 1 ? (loops * nregs_ + 1) / 2 : loops;
        cursor_.tagValid = true;
        hasAD_ = false;
        const uint64_t packedRegs = load64(data + 8);
        for (unsigned i = 0; i < nregs_; ++i) {
            regs_[i] = (packedRegs >> (i * 4)) & 15;
            hasAD_ |= regs_[i] == 0xe;
        }
        hasAD_ &= cursor_.format == 0;
    }
    void completeTag() {
        cursor_.tagValid = false;
        if (cursor_.eop) cursor_.activePath = 0;
    }
    void consume(const std::shared_ptr<Record>& owned) {
        auto& record = *owned;
        const uint32_t begin = record.offset;
        while (record.offset < record.data.size() && !control_.stalled()) {
            if (!cursor_.tagValid) {
                loadTag(record.data.data() + record.offset, record.path);
                record.offset += 16;
            }
            if (cursor_.remainingQwords) {
                const uint32_t available = (record.data.size() - record.offset) / 16;
                if (!available) break;
                if (!hasAD_) {
                    const uint32_t count = std::min(available, cursor_.remainingQwords);
                    record.offset += count * 16;
                    cursor_.remainingQwords -= count;
                } else {
                    const uint8_t* qw = record.data.data() + record.offset;
                    const bool ad = regs_[cursor_.registerIndex] == 0xe;
                    // Advance before handling a repeated SIGNAL. The reference
                    // consumes precisely this QW before stopping GIF execution.
                    record.offset += 16;
                    --cursor_.remainingQwords;
                    cursor_.registerIndex = (cursor_.registerIndex + 1) % nregs_;
                    if (ad && control_.onAD(static_cast<uint8_t>(load64(qw + 8)), load64(qw)))
                        ++stats_.stalls;
                }
            }
            if (!cursor_.remainingQwords) {
                if (control_.stalled() && !cursor_.eop) break;
                completeTag();
                // Give earlier queued records a chance at this EOP before
                // consuming another packet from the current record's tail.
                if (!cursor_.activePath) break;
            }
        }
        const uint32_t count = record.offset - begin;
        if (count) staged_.push_back({owned, begin, count});
        if (!cursor_.activePath) flush();
    }
    void flush() {
        for (const auto& segment : staged_) {
            auto& record = *segment.record;
            record.sink(record.data.data() + segment.offset, segment.size);
            stats_.consumedBytes += segment.size;
            record.emitted += segment.size;
            if (record.emitted == record.data.size()) {
                stats_.pendingBytes -= record.data.size();
                ++stats_.consumedRecords;
            }
        }
        staged_.clear();
    }

    Control& control_;
    std::deque<std::shared_ptr<Record>> records_;
    std::vector<Segment> staged_;
    Cursor cursor_;
    Stats stats_;
    std::array<uint8_t, 16> regs_{};
    uint8_t nregs_ = 0;
    bool hasAD_ = false, draining_ = false, failed_ = false;
};

} // namespace rrv::gs
