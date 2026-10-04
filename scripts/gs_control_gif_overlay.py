"""Narrow opt-in GIF arbiter integration for producer GS control.

The immutable producer remains the default. All substitutions are anchored and
fail if its relevant implementation changes; no source dependency is modified.
"""


def _once(text: str, old: str, new: str) -> str:
    if text.count(old) != 1:
        raise RuntimeError(f"GS control GIF overlay expected one anchor: {old[:100]!r}")
    return text.replace(old, new, 1)


def gif_header(text: str) -> str:
    text = _once(text, "#include <vector>", """#include <vector>
#include <memory>
#include "rrv_gif_control_stream.h"
""")
    text = _once(text, "    void drain();", """    void drain();

    // Startup-only candidate binding. Access is serialized by guest execution.
    void setControl(rrv::gs::Control* control)
    {
        if (m_control || m_controlStream || !m_queue.empty())
            throw std::logic_error("GS control must be bound once before GIF admission");
        m_control = control;
        if (control && control->enabled())
            m_controlStream = std::make_unique<rrv::gs::Stream>(*control);
    }
    bool controlStalled() const
    { return m_controlStream && m_control->stalled(); }
    bool controlPending() const
    { return m_controlStream && (m_controlStream->pending() || !m_queue.empty()); }
    bool controlPendingPath(int path) const
    {
        if (!m_controlStream) return false;
        if (m_controlStream->pendingPath(static_cast<uint8_t>(path))) return true;
        for (const auto& packet : m_queue)
            if (static_cast<int>(packet.pathId) == path) return true;
        return false;
    }
    const rrv::gs::Stream::Stats* controlStats() const
    { return m_controlStream ? &m_controlStream->stats() : nullptr; }
""")
    text = _once(text, "    std::vector<GifArbiterPacket> m_pool;", """    std::vector<GifArbiterPacket> m_pool;
    rrv::gs::Control* m_control = nullptr;
    std::unique_ptr<rrv::gs::Stream> m_controlStream;
    uint64_t m_controlQueuedBytes = 0;
""")
    return text


def gif_cpp(text: str) -> str:
    text = _once(text, """    if (!data || sizeBytes < 16 || !m_processFn)
        return;""", """    if (!data || sizeBytes < 16 || !m_processFn)
        return;
    if (m_controlStream)
    {
        if (sizeBytes & 15u)
            throw std::invalid_argument("GS control GIF admission requires whole qwords");
        const uint64_t held = m_controlQueuedBytes + m_controlStream->stats().pendingBytes;
        if (sizeBytes > rrv::gs::Stream::CapacityBytes ||
            held > rrv::gs::Stream::CapacityBytes - sizeBytes)
            throw std::length_error("GS control arbiter pending payload exceeds 64 MiB");
    }""")
    text = _once(text, "    m_queue.push_back(std::move(pkt));", """    m_queue.push_back(std::move(pkt));
    if (m_controlStream) m_controlQueuedBytes += sizeBytes;""")
    old = """            m_processFn(pkt.pathId, pkt.data.data(), static_cast<uint32_t>(pkt.data.size()), pkt.vu1Pc,
                        pkt.diagnostic.carDma ? &pkt.diagnostic : nullptr);"""
    new = """            if (m_controlStream)
            {
                // Own payload and provenance before any event is executed.
                // All sorted records enter the gate before FINISH eligibility
                // is considered; a stalled tail is never sorted with arrivals.
                const auto process = m_processFn;
                const auto path = pkt.pathId;
                const auto vuPc = pkt.vu1Pc;
                const auto diagnostic = pkt.diagnostic;
                m_controlStream->enqueue(static_cast<uint8_t>(path), pkt.data.data(),
                    static_cast<uint32_t>(pkt.data.size()),
                    [process, path, vuPc, diagnostic](const uint8_t* data, uint32_t size) {
                        process(path, data, size, vuPc,
                            diagnostic.carDma ? &diagnostic : nullptr);
                    });
            }
            else
            {
""" + old + """
            }"""
    text = _once(text, old, new)
    # The stream owns retained payloads; retaining another copy in the legacy
    # packet pool would make candidate memory depend on old burst sizes.
    text = _once(text, "        if (m_pool.size() < kMaxPooledPackets)",
                 "        if (!m_controlStream && m_pool.size() < kMaxPooledPackets)")
    text = _once(text, "    m_queue.clear();", """    m_queue.clear();
    if (m_controlStream)
    {
        m_controlQueuedBytes = 0;
        m_controlStream->drain();
    }""")
    return text
