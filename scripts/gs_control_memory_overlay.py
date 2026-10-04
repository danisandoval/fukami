"""Candidate-only GS control wiring for the pinned memory/VIF producer copy.

Applied after product-host/SPR transformations. Every edit requires one exact
anchor; this module neither opens nor modifies the immutable producer checkout.
The ordinary (control OFF) branches retain their original implementation.
"""
from m2_causal_overlay import replace_once, replace_once_in_region


def add(text, anchor, insertion, label):
    return replace_once(text, anchor, anchor + insertion, label)


def memory_header(text):
    text = '#include "rrv_gs_control.h"\n#include "rrv_gs_result_boundary.h"\n' + text
    text = add(text, '    GSRegisters gs_regs;\n', '''    // Bound to the live 19-register bank; all access stays under guest ownership.
    rrv::gs::Control m_gsControl{&gs_regs.pmode};
''', 'GS control instance')
    text = add(text, '    void processPendingTransfers();\n', '''    rrv::gs::Control &gsControl() { return m_gsControl; }
    const rrv::gs::Control &gsControl() const { return m_gsControl; }
    rrv::gs::ResultBoundary gsResultBoundary() const {
        rrv::gs::ProducerWork work = rrv::gs::ProducerWork::None;
        if (m_gifArbiter && m_gifArbiter->controlPending())
            work |= rrv::gs::ProducerWork::RendererVisibleGif;
        if (!m_gsDeferredVif1.empty())
            work |= rrv::gs::ProducerWork::DeferredVif1;
        if (!m_heldVif1Transfers.empty())
            work |= rrv::gs::ProducerWork::HeldVif1;
        if (m_gsDirectBytesRemaining || !m_gsDirectPartial.empty())
            work |= rrv::gs::ProducerWork::PartialDirect;
        if (!m_gsBlockedGifTransfers.empty())
            work |= rrv::gs::ProducerWork::BlockedGifDma;
        if (m_gsVif1CompletionPending)
            work |= rrv::gs::ProducerWork::VifCompletion;
        if (m_vif1PendingPath2ImageQwc != 0u)
            work |= rrv::gs::ProducerWork::PendingPath2Image;
        if (!m_path3MaskedFifo.empty())
            work |= rrv::gs::ProducerWork::MaskedPath3;
        if (m_gsReplayingVif1 || m_gsResuming || m_gsVifReplayDeferred)
            work |= rrv::gs::ProducerWork::ReplayOnly;
        return {work};
    }
    void resumeGsControlWork();
    void deferGsVif1(const uint8_t *data, uint32_t bytes, bool stallFollowsKick);
    size_t gsDeferredVif1Bytes() const { return m_gsDeferredVif1Bytes; }
    size_t gsBlockedGifBytes() const { return m_gsBlockedGifBytes; }
    size_t gsBlockedGifTransferCount() const { return m_gsBlockedGifTransfers.size(); }
    struct GsDeferredVif1 { std::vector<uint8_t> data; bool stallFollowsKick; };
    std::deque<GsDeferredVif1> m_gsDeferredVif1;
    size_t m_gsDeferredVif1Bytes = 0;
    size_t m_gsBlockedGifBytes = 0;
    bool m_gsReplayingVif1 = false;
    bool m_gsReplayStallFollowsKick = true;
    bool m_gsResuming = false;
    bool m_gsVif1CompletionPending = false;
    bool m_gsVifReplayDeferred = false;
    uint32_t m_gsDirectBytesRemaining = 0;
    bool m_gsDirectHl = false;
    bool m_gsDirectFirst = false;
    uint32_t m_gsDirectDeclaredQw = 0;
    uint32_t m_gsDirectImageExtra = 0;
    std::vector<uint8_t> m_gsDirectPartial;
    static constexpr size_t kGsPendingLimit = 64u * 1024u * 1024u;
''', 'GS pending public API')
    text = add(text, '        bool fromScratchpad = false;\n',
               '        bool gsOwnedNormal = false; // Preserve normal-DMA pacer eligibility after owning its payload.\n',
               'GS normal DMA provenance')
    text = add(text, '    std::vector<PendingTransfer> m_pendingGifTransfers;\n',
               '    std::vector<PendingTransfer> m_gsBlockedGifTransfers;\n',
               'GS blocked DMA storage')
    return text


def memory_cpp(text):
    text = '#include <iterator>\n' + text
    # Validate alignment first. Translate only aliases here, without touching the
    # TLB/scratchpad path. The control implementation rejects unrelated addresses.
    for bits, typ in ((8, 'uint8_t'), (16, 'uint16_t'), (32, 'uint32_t'),
                      (64, 'uint64_t'), (128, '__m128i')):
        begin = f'void PS2Memory::write{bits}(uint32_t address, {typ} value)\n{{\n'
        if bits == 8:
            anchor = begin
        else:
            anchor = ('        throw std::runtime_error("Unaligned ' + str(bits) +
                      '-bit write at address: 0x" + std::to_string(address));\n    }\n')
        lo = 'static_cast<uint64_t>(_mm_extract_epi64(value, 0))' if bits == 128 else 'value'
        insertion = ('\n    const uint32_t gsAddress = (address >= 0x80000000u && address < 0xc0000000u)\n'
                     '        ? (address & 0x1fffffffu) : address;\n'
                     f'    if (isGsPrivReg(gsAddress) && m_gsControl.write(gsAddress, {bits}u, {lo}))\n'
                     '        return;\n')
        text = add(text, anchor, insertion, f'GS write{bits} control')
    text = add(text, 'bool PS2Memory::writeIORegister(uint32_t address, uint32_t value)\n{\n',
               '    if (isGsPrivReg(address) && m_gsControl.write(address, 32u, value))\n        return true;\n',
               'GS direct IO control')
    for bits, typ in ((8, 'uint8_t'), (16, 'uint16_t')):
        begin = f'{typ} PS2Memory::read{bits}(uint32_t address)\n{{\n'
        anchor = begin if bits == 8 else ('        throw std::runtime_error("Unaligned 16-bit read at address: 0x" + std::to_string(address));\n    }\n')
        text = add(text, anchor, '''
    const uint32_t gsAddress = (address >= 0x80000000u && address < 0xc0000000u)
        ? (address & 0x1fffffffu) : address;
    if (m_gsControl.enabled() && isGsPrivReg(gsAddress))
    {
        const uint64_t *reg = gsRegPtr(gs_regs, gsAddress);
        return reg ? static_cast<''' + typ + '''>(*reg >> ((gsAddress & 7u) * 8u)) : 0;
    }
''', f'GS read{bits} control bank')
    for bits in (32, 64, 128):
        anchor = ('        throw std::runtime_error("Unaligned ' + str(bits) +
                  '-bit read at address: 0x" + std::to_string(address));\n    }\n')
        text = add(text, anchor,
                   '\n    if (m_gsControl.enabled() && address >= 0x80000000u && address < 0xc0000000u &&\n'
                   '        isGsPrivReg(address & 0x1fffffffu))\n'
                   '        address &= 0x1fffffffu;\n', f'GS read{bits} uncached alias')
    text = add(text, 'bool PS2Memory::initialize(size_t ramSize)\n{\n',
               '    if (m_gsControl.enabled())\n'
               '        throw std::logic_error("GS producer control memory is single-lifetime; construct a new runtime to reinitialize");\n',
               'GS reject control lifetime reinitialization before cleanup')
    # Full memory initialization owns a fresh lifetime. CSR RESET deliberately
    # does not clear these buffers: PCSX2 resumes after its queued SIGNAL slot.
    text = add(text, '    m_seenGifCopy = false;\n', '''    m_gsDeferredVif1.clear();
    m_gsBlockedGifTransfers.clear();
    m_gsDeferredVif1Bytes = m_gsBlockedGifBytes = 0;
    m_gsReplayingVif1 = m_gsResuming = m_gsVif1CompletionPending = false;
    m_gsVifReplayDeferred = false;
    m_gsDirectBytesRemaining = 0;
    m_gsDirectPartial.clear();
''', 'GS initialization lifecycle')
    text = add(text, 'void PS2Memory::processPendingTransfers()\n{\n', '''    if (m_gsControl.enabled())
    {
        // A new PATH3 DMA cannot enter GIF while a previous SIGNAL blocks it.
        // Own the accepted DMA source now; host/guest buffer reuse is harmless.
        if (m_gifArbiter && m_gifArbiter->controlStalled())
        {
            for (auto &p : m_pendingGifTransfers)
            {
                const uint64_t bytes = p.chainData.empty() ? uint64_t(p.qwc) * 16u : p.chainData.size();
                if (bytes > kGsPendingLimit - m_gsBlockedGifBytes ||
                    m_gsBlockedGifTransfers.size() >= 65536u)
                    throw std::runtime_error("GS control pending PATH3 capacity exceeded");
                if (p.chainData.empty() && bytes)
                {
                    p.gsOwnedNormal = true;
                    p.chainData.resize(static_cast<size_t>(bytes));
                    uint32_t src = translateAddress(p.srcAddr);
                    const uint8_t *base = p.fromScratchpad ? m_scratchpad : m_rdram;
                    const uint32_t limit = p.fromScratchpad ? PS2_SCRATCHPAD_SIZE : PS2_RAM_SIZE;
                    for (size_t n = 0; n < p.chainData.size(); ++n)
                        p.chainData[n] = base[(uint64_t(src) + n) % limit];
                }
                m_gsBlockedGifBytes += static_cast<size_t>(bytes);
                m_gsBlockedGifTransfers.push_back(std::move(p));
            }
            m_pendingGifTransfers.clear();
        }
        else if (!m_gsBlockedGifTransfers.empty())
        {
            m_pendingGifTransfers.insert(m_pendingGifTransfers.begin(),
                std::make_move_iterator(m_gsBlockedGifTransfers.begin()),
                std::make_move_iterator(m_gsBlockedGifTransfers.end()));
            m_gsBlockedGifTransfers.clear();
            m_gsBlockedGifBytes = 0;
        }
    }
''', 'GS DMA pending admission')
    text = replace_once(text, '    const bool hadVif1 = !m_pendingVif1Transfers.empty();\n',
                        '    bool hadVif1 = !m_pendingVif1Transfers.empty();\n', 'GS VIF completion eligibility')
    anchor = '    static constexpr uint32_t GIF_CHANNEL = 0x1000A000;\n'
    text = replace_once(text, anchor, '''    if (m_gsControl.enabled())
    {
        // A VIF command deferred before DIRECT has not completed its DMA work.
        // Preserve notification across retries, and never block the EE host.
        const bool vifBlocked = !m_gsDeferredVif1.empty() ||
            (m_gifArbiter && m_gifArbiter->controlStalled() && !m_heldVif1Transfers.empty());
        m_gsVif1CompletionPending = m_gsVif1CompletionPending || hadVif1;
        hadVif1 = m_gsVif1CompletionPending && !vifBlocked;
        if (hadVif1)
            m_gsVif1CompletionPending = false;
    }

''' + anchor, 'GS retain VIF completion')
    text = add(text, 'uint32_t PS2Memory::readIORegister(uint32_t address)\n{\n',
               '    if (m_gsControl.enabled() &&\n'
               '        ((address == 0x1000a000u && !m_gsBlockedGifTransfers.empty()) ||\n'
               '         (address == 0x10009000u && (m_gsVif1CompletionPending || !m_gsDeferredVif1.empty()))))\n'
               '        return m_ioRegisters[address];\n', 'GS pending DMA STR reads')
    text = replace_once_in_region(text, 'bool PS2Memory::armPath3Pacer()\n',
               'void PS2Memory::pacePath3()\n',
               '        if (p.chainData.empty())\n',
               '        if (p.chainData.empty() || p.gsOwnedNormal)\n',
               'GS owned normal DMA retains original pacer exclusion')
    # VIF reset is a VIF transfer reset, unlike CSR RESET.
    anchor = '                std::memset(&vif1_regs, 0, sizeof(vif1_regs));\n'
    text = add(text, anchor, '''                if (m_gsControl.enabled())
                {
                    m_gsDeferredVif1.clear();
                    m_gsDeferredVif1Bytes = 0;
                    m_gsVif1CompletionPending = false;
                    m_gsDirectBytesRemaining = 0;
                    m_gsDirectPartial.clear();
                }
''', 'GS VIF reset deferred cursor')
    text += '''
// GS-control candidate scheduling. Called only in the guest execution context.
void PS2Memory::deferGsVif1(const uint8_t *data, uint32_t bytes, bool stallFollowsKick)
{
    if (!bytes)
        return;
    if (bytes > kGsPendingLimit - m_gsDeferredVif1Bytes || m_gsDeferredVif1.size() >= 65536u)
        throw std::runtime_error("GS control pending VIF1 capacity exceeded");
    GsDeferredVif1 deferred{{data, data + bytes}, stallFollowsKick};
    m_gsDeferredVif1Bytes += bytes;
    // A stopped replay precedes all newer fragments already held in the deque.
    if (m_gsReplayingVif1)
    {
        m_gsVifReplayDeferred = true;
        m_gsDeferredVif1.push_front(std::move(deferred));
    }
    else
        m_gsDeferredVif1.push_back(std::move(deferred));
}

void PS2Memory::resumeGsControlWork()
{
    if (!m_gsControl.enabled() || m_gsResuming || (m_gifArbiter && m_gifArbiter->controlStalled()))
        return;
    m_gsResuming = true;
    try
    {
        while (!m_gsDeferredVif1.empty() && !(m_gifArbiter && m_gifArbiter->controlStalled()))
        {
            auto next = std::move(m_gsDeferredVif1.front());
            m_gsDeferredVif1.pop_front();
            m_gsDeferredVif1Bytes -= next.data.size();
            m_gsReplayStallFollowsKick = next.stallFollowsKick;
            m_gsVifReplayDeferred = false;
            m_gsReplayingVif1 = true;
            processVIF1Data(next.data.data(), static_cast<uint32_t>(next.data.size()));
            m_gsReplayingVif1 = false;
            if (m_gsVifReplayDeferred)
                break; // A FLUSH can still await input on another GIF path.
        }
        if (m_gsVif1CompletionPending && m_gsDeferredVif1.empty() &&
            !(m_gifArbiter && m_gifArbiter->controlStalled()))
            processVif1Queue(m_heldVif1Transfers);
        processPendingTransfers();
    }
    catch (...)
    {
        m_gsReplayingVif1 = m_gsResuming = false;
        throw;
    }
    m_gsResuming = false;
}
'''
    return text


def vif1_cpp(text):
    anchor = 'void PS2Memory::processVIF1Data(const uint8_t *data, uint32_t sizeBytes)\n{\n'
    text = add(text, anchor, '''    if (m_gsControl.enabled() && !m_gsDeferredVif1.empty() && !m_gsReplayingVif1)
    {
        if (data && sizeBytes)
            deferGsVif1(data, sizeBytes, true);
        return;
    }
''', 'GS retain later VIF fragments')
    text = replace_once(text, '    bool stallFollowsKick = true;\n',
                        '    bool stallFollowsKick = m_gsReplayingVif1 ? m_gsReplayStallFollowsKick : true;\n',
                        'GS preserve VIF pacing cursor')
    text = replace_once(text, '    while (pos + 4 <= sizeBytes)\n',
                        '    while (pos + 4 <= sizeBytes || (m_gsControl.enabled() && m_gsDirectBytesRemaining && pos < sizeBytes))\n',
                        'GS fragmented DIRECT byte tail')
    anchor = '    while (pos + 4 <= sizeBytes || (m_gsControl.enabled() && m_gsDirectBytesRemaining && pos < sizeBytes))\n    {\n'
    text = add(text, anchor, '''        if (m_gsControl.enabled() && m_gsDirectBytesRemaining)
        {
            if (m_gifArbiter && m_gifArbiter->controlStalled())
            {
                deferGsVif1(data + pos, sizeBytes - pos, stallFollowsKick);
                return;
            }
            const uint32_t take = std::min(m_gsDirectBytesRemaining, sizeBytes - pos);
            m_gsDirectPartial.insert(m_gsDirectPartial.end(), data + pos, data + pos + take);
            pos += take;
            m_gsDirectBytesRemaining -= take;
            const size_t send = m_gsDirectPartial.size() & ~size_t(15);
            if (send)
            {
                if (m_gsDirectFirst)
                {
                    const uint32_t imageQw = gifImageQwcFromTag(m_gsDirectPartial.data(), static_cast<uint32_t>(send));
                    m_gsDirectImageExtra = imageQw > m_gsDirectDeclaredQw - 1u ?
                        imageQw - (m_gsDirectDeclaredQw - 1u) : 0u;
                    m_gsDirectFirst = false;
                }
                submitGifPacket(GifPathId::Path2, m_gsDirectPartial.data(),
                                static_cast<uint32_t>(send), true, m_gsDirectHl);
                m_gsDirectPartial.erase(m_gsDirectPartial.begin(), m_gsDirectPartial.begin() + send);
            }
            if (!m_gsDirectBytesRemaining && m_gsDirectImageExtra)
            {
                m_vif1PendingPath2ImageQwc = m_gsDirectImageExtra;
                m_vif1PendingPath2DirectHl = m_gsDirectHl;
                m_gsDirectImageExtra = 0;
            }
            continue;
        }
''', 'GS DIRECT fragmented payload progression')
    anchor = '            const uint32_t declaredQw = qwCount;\n'
    text = add(text, anchor, '''            if (m_gsControl.enabled())
            {
                m_gsDirectBytesRemaining = qwCount * 16u;
                m_gsDirectHl = opcode == VIF_DIRECTHL;
                m_gsDirectFirst = true;
                m_gsDirectDeclaredQw = qwCount;
                m_gsDirectImageExtra = 0;
                continue;
            }
''', 'GS DIRECT start retained transfer')
    anchor = '        if (m_vif1PendingPath2ImageQwc != 0u)\n        {\n'
    text = add(text, anchor, '''            if (m_gsControl.enabled() && m_gifArbiter && m_gifArbiter->controlStalled())
            {
                deferGsVif1(data + pos, sizeBytes - pos, stallFollowsKick);
                return;
            }
''', 'GS VIF image continuation stall')
    # Test before the opcode has any observable effect or command counters move.
    anchor = '        uint8_t num = (cmd >> 16) & 0xFF;\n'
    text = add(text, anchor, '''
        const bool gsWait = m_gsControl.enabled() && m_gifArbiter &&
            (((opcode == VIF_DIRECT || opcode == VIF_DIRECTHL) && m_gifArbiter->controlStalled()) ||
             ((opcode == VIF_FLUSH || opcode == VIF_MSCALF) &&
              (m_gifArbiter->controlPendingPath(1) || m_gifArbiter->controlPendingPath(2))) ||
             (opcode == VIF_FLUSHA && m_gifArbiter->controlPending()));
        if (gsWait)
        {
            deferGsVif1(data + cmdPos, sizeBytes - cmdPos, stallFollowsKick);
            return;
        }
''', 'GS VIF DIRECT command stall')
    return text
