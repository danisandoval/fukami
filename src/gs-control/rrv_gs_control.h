// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+
// Producer event transitions adapted from PCSX2 fd9d310ccbb6b8b62c976da8886a3c8fd3a10ff3:
// pcsx2/Gif_Unit.cpp (Gif_HandlerAD, Gif_FinishIRQ), pcsx2/GS.cpp
// (gsCSRwrite, IMRwrite, gsWrite8/16/32/64/128). This is not GPU completion.
#pragma once

#include <cstdint>
#include <functional>
#include <utility>

namespace rrv::gs {

// All calls, bank access and hooks belong to the serialized guest execution
// context. No renderer thread may call this object. The IRQ hook only latches
// INTC; it must not execute guest handlers. resumeGif may reenter this object.
class Control {
public:
    struct Hooks {
        std::function<void()> latchIrq;
        std::function<void()> resumeGif;
        std::function<void()> resetRenderer;
    };
    struct Counters {
        uint64_t signals = 0, labels = 0, finishes = 0, signalStalls = 0;
        uint64_t signalAcks = 0, finishAcks = 0, irqRequests = 0;
        uint64_t csrWrites = 0, imrWrites = 0, siglblWrites = 0;
        uint64_t resets = 0, resumes = 0, finishEligibilityChecks = 0;
        uint64_t finishDeliveries = 0;
    };

    explicit Control(uint64_t* regs19) : regs_(regs19) {}
    // Startup configuration only; does not replace the caller's initial bank.
    void configure(bool enabled, Hooks hooks = {}) {
        enabled_ = enabled;
        hooks_ = std::move(hooks);
    }
    bool enabled() const { return enabled_; }
    bool stalled() const { return signalQueued_; }
    bool finishPending() const { return finishPending_; }
    bool finishFired() const { return finishFired_; }
    const Counters& counters() const { return counts_; }

    // Only invoke for a decoded, accepted packed A+D QW, never raw payload.
    // The caller consumes the QW before obeying a true return, retains its GIF
    // cursor and must stop progression across all paths until acknowledgement.
    bool onAD(uint8_t reg, uint64_t data) {
        if (!enabled_) return false;
        if (reg == 0x60) {
            ++counts_.signals;
            if (csr() & 1) {
                if (!signalQueued_) {
                    signalData_ = data;
                    signalQueued_ = true;
                    ++counts_.signalStalls;
                    return true;
                }
            } else {
                updateId(false, data);
                if (!(imr() & 0x100)) irq();
                csr() |= 1;
            }
        } else if (reg == 0x61) {
            ++counts_.finishes;
            finishFired_ = false;
            finishPending_ = true;
        } else if (reg == 0x62) {
            ++counts_.labels;
            updateId(true, data);
        }
        return false;
    }

    // Caller supplies Gif_Unit::Execute's condition: no eligible buffered work
    // on paths OTHER than APATH (Gif_Unit.h checkPaths(..., true)). An active
    // incomplete path alone does not prohibit FINISH. Do not call at enqueue.
    void finishEligible(bool otherPathsPending) {
        if (!enabled_) return;
        ++counts_.finishEligibilityChecks;
        if (otherPathsPending) return;
        if (finishPending_) {
            csr() |= 2;
            finishPending_ = false;
            ++counts_.finishDeliveries;
        }
        if ((csr() & 2) && !(imr() & 0x200) && !finishFired_) {
            irq();
            finishFired_ = true;
        }
    }

    // Width is in bits. Accept physical addresses or GS-bank offsets. The
    // compact 19-register ABI stores the low 64 bits of each 128-bit slot;
    // padding writes are consumed without storage, as in the existing bank.
    bool write(uint32_t address, unsigned width, uint64_t valueLo) {
        if (!enabled_) return false;
        if (width != 8 && width != 16 && width != 32 && width != 64 && width != 128)
            return false;
        if (address >= 0x12000000 && address < 0x12002000) address -= 0x12000000;
        const uint32_t slot = address & ~uint32_t(15);
        const unsigned byte = address & 15;
        unsigned index;
        if (slot == 0x1000) index = 15;
        else if (slot == 0x1010) index = 16;
        else if (slot == 0x1080) index = 18;
        else return false;
        const unsigned bytes = width / 8;
        if (byte % bytes || byte + bytes > 16) return false;
        if (index == 15) {
            ++counts_.csrWrites;
            if ((width == 8 && byte < 4) || (width == 16 && byte < 4) || byte == 0) {
                const uint32_t value = static_cast<uint32_t>(valueLo);
                const uint32_t narrowed = width == 8 ? value & 0xff :
                    width == 16 ? value & 0xffff : value;
                writeCSR(narrowed << (byte * 8));
                return true;
            }
        } else if (index == 16) {
            ++counts_.imrWrites;
            // The reference deliberately has no special IMR 8-bit handler.
            if (byte == 0 && width >= 16) {
                uint32_t value = static_cast<uint32_t>(valueLo);
                if (width == 16) value &= 0xffff;
                if ((csr() & 0x1f) & ((~value & imr()) >> 8)) irq();
                imr() = (imr() & 0xffffffff00000000ULL) | (value & 0x1f00) | 0x6000;
                return true;
            }
        } else ++counts_.siglblWrites;
        if (byte < 8) {
            const unsigned storedBytes = bytes > 8 ? 8 : bytes;
            const uint64_t mask = storedBytes == 8 ? ~uint64_t(0) :
                (uint64_t(1) << (storedBytes * 8)) - 1;
            regs_[index] = (regs_[index] & ~(mask << (byte * 8))) |
                ((valueLo & mask) << (byte * 8));
        }
        return true;
    }

private:
    uint64_t& csr() { return regs_[15]; }
    uint64_t& imr() { return regs_[16]; }
    void irq() {
        ++counts_.irqRequests;
        if (hooks_.latchIrq) hooks_.latchIrq();
    }
    void updateId(bool label, uint64_t data) {
        const unsigned shift = label ? 32 : 0;
        const uint32_t old = static_cast<uint32_t>(regs_[18] >> shift);
        const uint32_t mask = static_cast<uint32_t>(data >> 32);
        const uint32_t id = (old & ~mask) | (static_cast<uint32_t>(data) & mask);
        const uint64_t bankMask = uint64_t(0xffffffff) << shift;
        regs_[18] = (regs_[18] & ~bankMask) | (uint64_t(id) << shift);
    }
    void writeCSR(uint32_t value) {
        if (value & 0x200) {
            ++counts_.resets;
            signalQueued_ = false;
            signalData_ = 0;
            finishFired_ = true;
            finishPending_ = false;
            // Explicit RRV boundary: the existing tick owner retains FIELD.
            // Other privileged registers and event latches reset as upstream.
            const uint64_t field = csr() & 0x2000;
            for (unsigned i = 0; i != 19; ++i) regs_[i] = 0;
            csr() = 0x551b4000 | field;
            imr() = 0x7f00;
            if (hooks_.resetRenderer) hooks_.resetRenderer();
        }
        // FLUSH is a no-op in the reference (no emulated GS FIFO).
        if (value & 1) {
            ++counts_.signalAcks;
            const bool resume = csr() & 1;
            if (signalQueued_) {
                updateId(false, signalData_);
                if (!(imr() & 0x100)) irq();
                csr() |= 1;
            } else csr() &= ~uint64_t(1);
            signalQueued_ = false;
            if (resume) {
                ++counts_.resumes;
                if (hooks_.resumeGif) hooks_.resumeGif();
            }
        }
        // Must follow synchronous GIF resume: one CSR write can acknowledge
        // a FINISH newly processed by that resume (GS.cpp gsCSRwrite order).
        if (value & 2) {
            ++counts_.finishAcks;
            csr() &= ~uint64_t(2);
            finishFired_ = false;
            finishPending_ = false;
        }
        csr() &= ~uint64_t(value & 0x1c);
    }

    uint64_t* regs_;
    bool enabled_ = false, signalQueued_ = false;
    bool finishPending_ = false, finishFired_ = false;
    uint64_t signalData_ = 0;
    Hooks hooks_;
    Counters counts_;
};

} // namespace rrv::gs
