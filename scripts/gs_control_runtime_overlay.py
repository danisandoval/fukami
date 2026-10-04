"""Candidate producer ownership and existing INTC delivery integration."""
from m2_causal_overlay import replace_once

def add(t,a,b,label): return replace_once(t,a,a+b,label)

def memory_irq_header(t):
    t = '#include <exception>\n' + t
    return add(t, '    void resumeGsControlWork();\n', '''    void latchGsControlIrq() { m_ioRegisters[0x1000f000u] |= 1u; m_gsIrqPending = true; }
    void setGsControlIrqCallback(std::function<bool()> cb) { m_gsIrqDispatch = std::move(cb); }
    void pumpGsControlIrq() {
        if (!m_gsControl.enabled() || m_gsTransactionDepth || m_gsIrqDispatching || !m_gsIrqPending || !m_gsIrqDispatch) return;
        m_gsIrqPending = false;
        m_gsIrqDispatching = true;
        try { if (!m_gsIrqDispatch()) m_gsIrqPending = true; else ++m_gsIrqDispatches; }
        catch (...) { m_gsIrqDispatching = false; throw; }
        m_gsIrqDispatching = false;
    }
    void retryGsControlIrq() { if (m_ioRegisters[0x1000f000u] & 1u) m_gsIrqPending = true; pumpGsControlIrq(); }
    std::function<uint32_t(uint32_t, bool)> m_gsIntcMask;
    uint64_t gsIrqDispatches() const { return m_gsIrqDispatches; }
    struct GsTransaction {
        PS2Memory &m;
        explicit GsTransaction(PS2Memory &memory) : m(memory) { ++m.m_gsTransactionDepth; }
        ~GsTransaction() noexcept(false) { if (--m.m_gsTransactionDepth == 0 && !std::uncaught_exceptions()) m.pumpGsControlIrq(); }
    };
    std::function<bool()> m_gsIrqDispatch;
    unsigned m_gsTransactionDepth = 0;
    bool m_gsIrqPending = false, m_gsIrqDispatching = false;
    uint64_t m_gsIrqDispatches = 0;
''', 'GS deferred IRQ context')

def memory_irq_cpp(t):
    # Nested memory/decoder operations form one transaction. Guest callbacks run
    # only after parser, DMA and CSR mutations have completed.
    import re
    names = 'write8|write16|write32|write64|write128|writeIORegister|processPendingTransfers|submitGifPacket|resumeGsControlWork|processVif1Queue'
    pattern = r'((?:void|bool) PS2Memory::(?:' + names + r')\([^)]*\)\s*\{\n)'
    t, count = re.subn(pattern, r'\1    GsTransaction gsTransaction(*this);\n', t)
    if count != 10: raise ValueError(f'GS transaction anchors {count}, expected 10')
    t = add(t, 'bool PS2Memory::writeIORegister(uint32_t address, uint32_t value)\n{\n', '''    if (m_gsControl.enabled() && address == 0x1000f010u && m_gsIntcMask) {
        m_ioRegisters[address] = (value & ~1u) | (m_gsIntcMask(value, true) & 1u);
        retryGsControlIrq();
        return true;
    }
    if (m_gsControl.enabled() && address == 0x1000f000u) {
        // INTC_STAT is W1C. Preserve all unrelated latched causes.
        m_ioRegisters[address] &= ~value;
        if (value & 1u) m_gsIrqPending = false;
        return true;
    }
''', 'GS INTC acknowledge')
    # Narrow action-register stores must not RMW an already latched GS bit.
    anchor = '        writeIORegister(regAddr, newValue);\n'
    if t.count(anchor) != 2: raise ValueError('INTC narrow store anchors')
    t=t.replace(anchor, '''        if (m_gsControl.enabled() && regAddr == 0x1000f000u)
            newValue = static_cast<uint32_t>(value) << shift;
        if (m_gsControl.enabled() && regAddr == 0x1000f010u)
            newValue = (newValue & ~1u) | ((static_cast<uint32_t>(value) << shift) & 1u);
''' + anchor)
    t=add(t, 'uint32_t PS2Memory::readIORegister(uint32_t address)\n{\n',
          '    if (m_gsControl.enabled() && address == 0x1000f010u && m_gsIntcMask) return (m_ioRegisters[address] & ~1u) | (m_gsIntcMask(0, false) & 1u);\n', 'GS shared INTC mask read')
    return t

def vif_irq_cpp(t):
    return add(t, 'void PS2Memory::processVIF1Data(const uint8_t *data, uint32_t sizeBytes)\n{\n',
               '    GsTransaction gsTransaction(*this);\n', 'GS VIF transaction')

def interrupt_header(t):
    return add(t, 'namespace ps2_syscalls\n{\n',
               '    bool dispatchGsControlInterrupt(uint8_t *rdram, PS2Runtime *runtime);\n', 'GS IRQ declaration')

def interrupt_cpp(t):
    # Only a latched request reaches this service. Masked requests remain pending
    # in PS2Memory and are retried after EnableIntc or a guest safepoint.
    t = add(t, '    void EnableIntc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)\n', '''''', 'GS EnableIntc anchor')
    marker = '    void iEnableIntc(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)\n'
    t = replace_once(t, marker, '''    uint32_t gsControlIntcMask(uint32_t toggle, bool write)
    {
        std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
        if (write) g_enabled_intc_mask ^= toggle & 1u;
        return g_enabled_intc_mask;
    }

    bool dispatchGsControlInterrupt(uint8_t *rdram, PS2Runtime *runtime)
    {
        {
            std::lock_guard<std::mutex> lock(g_irq_handler_mutex);
            if (!(g_enabled_intc_mask & 1u)) return false;
        }
        PS2Runtime::GuestExecutionScope guestExecution(runtime);
        dispatchIntcHandlersForCause(rdram, runtime, 0u);
        return true;
    }

''' + marker, 'GS existing IRQ route')
    start=t.index('    void EnableIntc('); end=t.index('    bool dispatchGsControlInterrupt', start)
    part=t[start:end]
    part=replace_once(part,'        setReturnS32(ctx, KE_OK);\n',
                     '        if (runtime) runtime->memory().retryGsControlIrq();\n        setReturnS32(ctx, KE_OK);\n','GS unmask retry')
    return t[:start]+part+t[end:]

def runtime_cpp(t):
    # Existing include exposes PS2Runtime; forward declaration avoids relying on
    # a source-local original Interrupt.h winning include search order.
    anchor='void PS2Runtime::dispatchGsPacket(GifPathId pathId, const uint8_t *data, uint32_t size,\n'
    t=replace_once(t,anchor,'namespace ps2_syscalls { bool dispatchGsControlInterrupt(uint8_t *, PS2Runtime *); uint32_t gsControlIntcMask(uint32_t, bool); }\n\n'+anchor,'GS dispatch declaration')
    start=t.index('void PS2Runtime::dispatchGsPacket('); end=t.index('\nnamespace\n{\n    void copyBridgeReadback',start)
    part=t[start:end]
    part=replace_once(part,'            uint64_t csr = 0u;\n','            if (m_memory.gsControl().enabled()) return;\n            uint64_t csr = 0u;\n','GS suppress stale packet feedback')
    t=t[:start]+part+t[end:]
    t=add(t,'                            const char *operation)\n    {\n',
          '        if (runtime.memory().gsControl().enabled()) return;\n','GS suppress stale field/HLE feedback')
    anchor='    m_memory.setGifArbiter(&m_gifArbiter);\n'
    t=add(t,anchor,'''    const char *controlMode = std::getenv("RRV_GS_CONTROL");
    const bool producerControl = controlMode && std::strcmp(controlMode, "producer") == 0;
    if (controlMode && *controlMode && !producerControl && std::strcmp(controlMode, "off") != 0)
        throw std::runtime_error("RRV_GS_CONTROL must be off or producer (startup only)");
    m_memory.gsControl().configure(producerControl, {
        [this] { m_memory.latchGsControlIrq(); },
        [this] { m_gifArbiter.drain(); m_memory.resumeGsControlWork(); },
        [this] { std::string error; if (!m_gsBackend.resetGs(&error))
            throw std::runtime_error("GS control renderer reset failed: " + error); }
    });
    m_gifArbiter.setControl(producerControl ? &m_memory.gsControl() : nullptr);
    m_memory.m_gsIntcMask = ps2_syscalls::gsControlIntcMask;
    m_memory.setGsControlIrqCallback([this] {
        return ps2_syscalls::dispatchGsControlInterrupt(m_memory.getRDRAM(), this);
    });
    std::fprintf(stderr, "[GS control] mode=%s renderer execution remains synchronous\\n", producerControl ? "producer" : "off");
''','GS startup ownership')
    # Actual direct HLE sequences enter the same arbiter instead of bypassing
    # pending SIGNAL. They are accepted owned work, not an implicit result fence.
    anchor='    const uint32_t packetBytes = static_cast<uint32_t>(packet.size() * sizeof(uint64_t));\n'
    t=add(t,anchor,'''    if (m_memory.gsControl().enabled()) {
        m_memory.submitGifPacket(GifPathId::Path3, reinterpret_cast<const uint8_t *>(packet.data()), packetBytes);
        return;
    }
''','GS HLE accepted path')
    t=add(t,'void PS2Runtime::advanceActiveGsBackendField(uint64_t fieldIndex)\n{\n',
          '''    if (m_memory.gsControl().enabled()) {
        // The existing tick/field index remains authoritative. With reverse
        // readback removed, mirror only FIELD directly, never acknowledge CSR.
        constexpr uint64_t fieldBit = 1ull << 13;
        m_memory.gs().csr = (m_memory.gs().csr & ~fieldBit) | ((fieldIndex & 1u) ? fieldBit : 0u);
    }
    m_memory.pumpGsControlIrq();
''','GS tick guest context service')
    t=add(t,'        m_memory.flushHeldVif1AtFrameBoundary();\n',
          '        if (m_memory.gsControl().enabled()) { m_memory.resumeGsControlWork(); m_memory.pumpGsControlIrq(); }\n','GS guest safepoint service')
    # Genuine result operations cannot bypass any retained producer state.
    # In particular, this guard precedes the inactive-backend return so the
    # HLE fallback cannot mutate a guest destination while acknowledgement is
    # still required.  It has no side effects: only a guest CSR/reset/VIF
    # action can later make the producer quiescent.
    anchor='bool PS2Runtime::readActiveGsBackendLocalMemory(uint8_t *dst, uint32_t byteCount,\n                                                uint64_t bitbltbuf, uint64_t trxpos,\n                                                uint64_t trxreg)\n{\n'
    t=replace_once(t,anchor,'''void PS2Runtime::preflightActiveGsBackendLocalMemoryRead()
{
    // StoreImage and direct callers share this exact admission under the
    // serialized guest owner.  It deliberately performs no drain, decode, or
    // acknowledgement; retained producer work remains guest-visible until its
    // ordinary CSR/VIF control action resolves it.
    GuestExecutionScope guestExecution(this);
    if (m_memory.gsControl().enabled() &&
        !m_memory.gsResultBoundary().admits(rrv::gs::ResultOperation::ReadLocalMemory))
        throw std::runtime_error("GS control local-memory read blocked by non-quiescent guest producer work; cannot synchronously resolve guest acknowledgement");
}

''' + anchor,'GS shared real result preflight')
    t=add(t,anchor,'''    // Retain a final admission at the native result boundary: a caller that
    // did not preflight cannot consume a local-memory transaction.
    preflightActiveGsBackendLocalMemoryRead();
''','GS real result dependency')
    anchor='bool PS2Runtime::snapshotActiveGsBackendLocalMemory(std::vector<uint8_t> &outBytes)\n{\n    GuestExecutionScope guestExecution(this);\n'
    t=add(t,anchor,'''    if (m_memory.gsControl().enabled() &&
        !m_memory.gsResultBoundary().admits(rrv::gs::ResultOperation::SnapshotLocalMemory))
        throw std::runtime_error("GS control local-memory snapshot blocked by non-quiescent guest producer work; VRAM-only state is not a runtime checkpoint");
''','GS snapshot quiescence')
    anchor='bool PS2Runtime::restoreActiveGsBackendLocalMemory(const uint8_t *bytes, uint32_t byteCount)\n{\n    GuestExecutionScope guestExecution(this);\n'
    t=add(t,anchor,'''    if (m_memory.gsControl().enabled() &&
        !m_memory.gsResultBoundary().admits(rrv::gs::ResultOperation::RestoreLocalMemory))
        throw std::runtime_error("GS control local-memory restore blocked by non-quiescent guest producer work; VRAM-only state is not a runtime checkpoint");
''','GS restore quiescence')
    # Final cheap aggregates; no packet clocks/logs.
    anchor='        ps2_syscalls::detachAllGuestHostThreads();\n        m_gsBackend.shutdown();\n'
    t=add(t,anchor,'''    if (m_memory.gsControl().enabled()) {
        const auto &c = m_memory.gsControl().counters();
        std::fprintf(stderr, "[GS control totals] signal=%llu label=%llu finish=%llu stalls=%llu signal_ack=%llu finish_ack=%llu irq_requests=%llu irq_dispatch=%llu csr_writes=%llu imr_writes=%llu reset=%llu finish_delivered=%llu deferred_vif=%zu blocked_gif=%zu\\n",
            (unsigned long long)c.signals, (unsigned long long)c.labels, (unsigned long long)c.finishes,
            (unsigned long long)c.signalStalls, (unsigned long long)c.signalAcks, (unsigned long long)c.finishAcks,
            (unsigned long long)c.irqRequests, (unsigned long long)m_memory.gsIrqDispatches(),
            (unsigned long long)c.csrWrites, (unsigned long long)c.imrWrites, (unsigned long long)c.resets,
            (unsigned long long)c.finishDeliveries, m_memory.gsDeferredVif1Bytes(), m_memory.gsBlockedGifBytes());
        if (const auto *q = m_gifArbiter.controlStats())
            std::fprintf(stderr, "[GS control GIF totals] admitted_records=%llu rendered_records=%llu admitted_bytes=%llu rendered_bytes=%llu pending_bytes=%llu high_water_bytes=%llu high_water_records=%llu pending=%u\\n",
                (unsigned long long)q->submittedRecords, (unsigned long long)q->consumedRecords,
                (unsigned long long)q->submittedBytes, (unsigned long long)q->consumedBytes,
                (unsigned long long)q->pendingBytes, (unsigned long long)q->highWaterBytes,
                (unsigned long long)q->highWaterRecords, unsigned(m_gifArbiter.controlPending()));
    }
''','GS aggregate coverage')
    return '#include <cstdio>\n' + t


def system_cpp(t):
    # The actual BIOS/HLE IMR syscall is another producer, not an MMIO store.
    # Both syscall variants must share masks/unmask IRQ semantics with MMIO.
    return replace_once(t, '            runtime->memory().gs().imr = newImr;\n',
                        '''            if (runtime->memory().gsControl().enabled())
                runtime->memory().write64(0x12001010u, newImr);
            else
                runtime->memory().gs().imr = newImr;
''', 'GS HLE IMR authoritative control')
