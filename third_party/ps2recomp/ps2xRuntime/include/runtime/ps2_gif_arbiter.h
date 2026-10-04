#ifndef PS2_GIF_ARBITER_H
#define PS2_GIF_ARBITER_H

#include <cstdint>
#include <functional>
#include <vector>

enum class GifPathId : uint8_t
{
    Path1 = 1,
    Path2 = 2,
    Path3 = 3,
};

struct GifPacketDiagnostic
{
    bool carDma = false;
    uint64_t chainId = 0u;
    uint32_t begin = 0u;
    uint32_t end = 0u;
    uint32_t callTag = 0u;
    uint32_t callTarget = 0u;
};

struct GifArbiterPacket
{
    GifPathId pathId;
    // PATH1 provenance: the VU1 micro-PC of the XGKICK that emitted this
    // packet. PATH2/3 deliberately carry zero.
    uint32_t vu1Pc = 0;
    bool path2DirectHl = false;
    bool path3Image = false;
    GifPacketDiagnostic diagnostic;
    std::vector<uint8_t> data;
};

class GifArbiter
{
public:
    using ProcessPacketFn =
        std::function<void(GifPathId, const uint8_t *, uint32_t, uint32_t,
                           const GifPacketDiagnostic *)>;

    GifArbiter() = default;
    explicit GifArbiter(ProcessPacketFn processFn);

    void setProcessPacketFn(ProcessPacketFn fn) { m_processFn = std::move(fn); }

    void submit(GifPathId pathId, const uint8_t *data, uint32_t sizeBytes,
                bool path2DirectHl = false, uint32_t vu1Pc = 0,
                const GifPacketDiagnostic *diagnostic = nullptr);

    void drain();

private:
    ProcessPacketFn m_processFn;
    std::vector<GifArbiterPacket> m_queue;
    // Free list of drained packets; their data buffers are reused on submit so the
    // hot GIF path does not malloc/free a vector per packet every frame.
    std::vector<GifArbiterPacket> m_pool;

    static bool isImagePacket(const uint8_t *data, uint32_t sizeBytes);
    static uint8_t pathPriority(GifPathId id);
};

#endif
