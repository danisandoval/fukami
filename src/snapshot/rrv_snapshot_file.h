// rrv_snapshot_file.h — reader/writer for the .rrvsnap container described in
// rrv_snapshot_format.h.
//
// Deliberately free of every runtime dependency: this translation unit knows
// about bytes and chunks, not about PS2Runtime.  That split is what lets the
// container be unit-tested on its own (tests/snapshot_container_tests.cpp) and
// lets offline tools read snapshots without linking the runtime.

#ifndef RRV_SNAPSHOT_FILE_H
#define RRV_SNAPSHOT_FILE_H

#include "rrv_snapshot_format.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace rrv::snapshot {

// Ordered key=value metadata (chunk::kMeta). Ordered rather than hashed so a
// snapshot's META block is byte-stable for the same inputs, which makes file
// diffs between two captures readable.
using MetaMap = std::map<std::string, std::string>;

std::string encodeMeta(const MetaMap &meta);
MetaMap decodeMeta(const std::string &text);

// --- Writer -----------------------------------------------------------------
// Streams chunks straight to disk (a snapshot is ~40 MB; buffering it twice in
// RAM while the guest is frozen is needless latency). Writes to a temporary
// sibling file and renames on finish(), so a crash mid-capture can never leave
// a half-written snapshot under a name the loader would trust.
class Writer
{
public:
    Writer() = default;
    ~Writer();

    Writer(const Writer &) = delete;
    Writer &operator=(const Writer &) = delete;

    bool open(const std::filesystem::path &path, std::string *error);
    bool addChunk(uint32_t id, uint32_t version, const void *data, size_t bytes,
                  std::string *error);
    bool finish(std::string *error);
    void abort();

    [[nodiscard]] uint64_t bytesWritten() const { return m_bytesWritten; }

private:
    std::filesystem::path m_finalPath;
    std::filesystem::path m_tempPath;
    std::ofstream m_out;
    uint32_t m_chunkCount = 0u;
    uint64_t m_bytesWritten = 0u;
    bool m_open = false;
};

// --- Reader -----------------------------------------------------------------
// Loads the whole file. A snapshot is read exactly once, at startup, before any
// guest code runs; simplicity beats laziness here.
class Reader
{
public:
    struct Chunk
    {
        uint32_t id = 0u;
        uint32_t version = 0u;
        std::vector<uint8_t> data;
    };

    bool load(const std::filesystem::path &path, std::string *error);

    [[nodiscard]] const Chunk *find(uint32_t id) const;
    [[nodiscard]] const MetaMap &meta() const { return m_meta; }
    [[nodiscard]] const std::vector<Chunk> &chunks() const { return m_chunks; }
    [[nodiscard]] const FileHeader &header() const { return m_header; }

    // Convenience: copy a chunk's payload over a fixed-size destination.
    // Returns false (and leaves dst untouched) when the chunk is absent or its
    // size disagrees — a snapshot from a build with a different memory map must
    // fail loudly, not silently under/over-fill guest RAM.
    bool copyChunkInto(uint32_t id, void *dst, size_t bytes, std::string *error) const;

private:
    FileHeader m_header{};
    std::vector<Chunk> m_chunks;
    MetaMap m_meta;
};

} // namespace rrv::snapshot

#endif // RRV_SNAPSHOT_FILE_H
