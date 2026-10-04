#include "rrv_snapshot_file.h"

#include <array>
#include <cstring>
#include <ctime>
#include <sstream>
#include <system_error>

namespace rrv::snapshot {

namespace {

const std::array<uint32_t, 256> &crcTable()
{
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256u; ++i)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
            {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            t[i] = c;
        }
        return t;
    }();
    return table;
}

// Chunks start on a 16-byte boundary so a future mmap-based reader can hand out
// aligned pointers into guest-sized payloads without a copy.
uint64_t padTo(uint64_t value, uint64_t alignment)
{
    const uint64_t rem = value % alignment;
    return (rem == 0u) ? value : (value + (alignment - rem));
}

} // namespace

uint32_t crc32(const void *data, size_t bytes, uint32_t seed)
{
    const auto &table = crcTable();
    uint32_t c = seed ^ 0xFFFFFFFFu;
    const auto *p = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < bytes; ++i)
    {
        c = table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

std::string encodeMeta(const MetaMap &meta)
{
    std::string out;
    for (const auto &[key, value] : meta)
    {
        out += key;
        out += '=';
        out += value;
        out += '\n';
    }
    return out;
}

MetaMap decodeMeta(const std::string &text)
{
    MetaMap meta;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line))
    {
        if (line.empty() || line[0] == '#')
        {
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
        {
            continue;
        }
        meta.emplace(line.substr(0, eq), line.substr(eq + 1));
    }
    return meta;
}

// --- Writer -----------------------------------------------------------------

Writer::~Writer()
{
    abort();
}

bool Writer::open(const std::filesystem::path &path, std::string *error)
{
    abort();

    m_finalPath = path;
    m_tempPath = path;
    m_tempPath += ".partial";

    std::error_code ec;
    if (path.has_parent_path())
    {
        std::filesystem::create_directories(path.parent_path(), ec);
    }

    m_out.open(m_tempPath, std::ios::binary | std::ios::trunc);
    if (!m_out)
    {
        if (error)
        {
            *error = "cannot open " + m_tempPath.string() + " for writing";
        }
        return false;
    }

    FileHeader header{};
    std::memcpy(header.magic, kMagic, sizeof(header.magic));
    header.formatVersion = kFormatVersion;
    header.headerBytes = kFileHeaderBytes;
    header.createdUnixSec = 0u; // patched in finish()
    header.chunkCount = 0u;     // patched in finish()
    header.flags = 0u;

    m_out.write(reinterpret_cast<const char *>(&header), sizeof(header));
    m_bytesWritten = sizeof(header);
    m_chunkCount = 0u;
    m_open = static_cast<bool>(m_out);
    return m_open;
}

bool Writer::addChunk(uint32_t id, uint32_t version, const void *data, size_t bytes,
                      std::string *error)
{
    if (!m_open)
    {
        if (error)
        {
            *error = "snapshot writer is not open";
        }
        return false;
    }

    ChunkHeader chunk{};
    chunk.id = id;
    chunk.version = version;
    chunk.storedBytes = bytes;
    chunk.rawBytes = bytes;
    chunk.compression = static_cast<uint32_t>(Compression::None);
    chunk.crc32 = crc32(data, bytes);

    m_out.write(reinterpret_cast<const char *>(&chunk), sizeof(chunk));
    if (bytes != 0u)
    {
        m_out.write(static_cast<const char *>(data), static_cast<std::streamsize>(bytes));
    }

    const uint64_t unpadded = m_bytesWritten + sizeof(chunk) + bytes;
    const uint64_t padded = padTo(unpadded, kChunkAlign);
    for (uint64_t i = unpadded; i < padded; ++i)
    {
        m_out.put('\0');
    }
    m_bytesWritten = padded;

    if (!m_out)
    {
        if (error)
        {
            *error = "write failed for " + m_tempPath.string();
        }
        return false;
    }

    ++m_chunkCount;
    return true;
}

bool Writer::finish(std::string *error)
{
    if (!m_open)
    {
        if (error)
        {
            *error = "snapshot writer is not open";
        }
        return false;
    }

    m_out.seekp(0, std::ios::beg);
    FileHeader header{};
    std::memcpy(header.magic, kMagic, sizeof(header.magic));
    header.formatVersion = kFormatVersion;
    header.headerBytes = kFileHeaderBytes;
    header.createdUnixSec =
        static_cast<uint64_t>(std::time(nullptr) > 0 ? std::time(nullptr) : 0);
    header.chunkCount = m_chunkCount;
    header.flags = 0u;
    m_out.write(reinterpret_cast<const char *>(&header), sizeof(header));

    const bool ok = static_cast<bool>(m_out);
    m_out.close();
    m_open = false;

    if (!ok)
    {
        if (error)
        {
            *error = "failed to finalise " + m_tempPath.string();
        }
        std::error_code ec;
        std::filesystem::remove(m_tempPath, ec);
        return false;
    }

    std::error_code ec;
    std::filesystem::rename(m_tempPath, m_finalPath, ec);
    if (ec)
    {
        // Cross-device or replaced-underneath: fall back to copy+remove.
        std::filesystem::copy_file(m_tempPath, m_finalPath,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        std::filesystem::remove(m_tempPath, ec);
    }
    if (ec)
    {
        if (error)
        {
            *error = "cannot move snapshot into place: " + ec.message();
        }
        return false;
    }
    return true;
}

void Writer::abort()
{
    if (m_out.is_open())
    {
        m_out.close();
    }
    if (m_open)
    {
        std::error_code ec;
        std::filesystem::remove(m_tempPath, ec);
    }
    m_open = false;
    m_chunkCount = 0u;
    m_bytesWritten = 0u;
}

// --- Reader -----------------------------------------------------------------

bool Reader::load(const std::filesystem::path &path, std::string *error)
{
    m_chunks.clear();
    m_meta.clear();

    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        if (error)
        {
            *error = "cannot open " + path.string();
        }
        return false;
    }

    in.read(reinterpret_cast<char *>(&m_header), sizeof(m_header));
    if (!in || std::memcmp(m_header.magic, kMagic, sizeof(kMagic)) != 0)
    {
        if (error)
        {
            *error = path.string() + " is not an .rrvsnap file";
        }
        return false;
    }
    if (m_header.formatVersion != kFormatVersion)
    {
        if (error)
        {
            *error = "snapshot format version " + std::to_string(m_header.formatVersion) +
                     " (this build reads version " + std::to_string(kFormatVersion) + ")";
        }
        return false;
    }
    if (m_header.headerBytes != kFileHeaderBytes)
    {
        // A later writer may grow the header; skip the extra bytes rather than
        // mis-parsing the first chunk.
        in.seekg(static_cast<std::streamoff>(m_header.headerBytes), std::ios::beg);
    }

    uint64_t offset = m_header.headerBytes;
    for (uint32_t i = 0; i < m_header.chunkCount; ++i)
    {
        ChunkHeader header{};
        in.read(reinterpret_cast<char *>(&header), sizeof(header));
        if (!in)
        {
            if (error)
            {
                *error = "truncated snapshot: chunk header " + std::to_string(i);
            }
            return false;
        }

        if (header.compression != static_cast<uint32_t>(Compression::None))
        {
            if (error)
            {
                *error = "unsupported chunk compression in " + path.string();
            }
            return false;
        }

        Chunk chunk;
        chunk.id = header.id;
        chunk.version = header.version;
        chunk.data.resize(static_cast<size_t>(header.storedBytes));
        if (header.storedBytes != 0u)
        {
            in.read(reinterpret_cast<char *>(chunk.data.data()),
                    static_cast<std::streamsize>(header.storedBytes));
        }
        if (!in)
        {
            if (error)
            {
                *error = "truncated snapshot: chunk payload " + std::to_string(i);
            }
            return false;
        }

        const uint32_t actual = crc32(chunk.data.data(), chunk.data.size());
        if (actual != header.crc32)
        {
            if (error)
            {
                *error = "snapshot chunk " + std::to_string(i) + " failed its CRC check";
            }
            return false;
        }

        const uint64_t unpadded = offset + sizeof(header) + header.storedBytes;
        offset = padTo(unpadded, kChunkAlign);
        in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);

        if (chunk.id == chunk::kMeta)
        {
            m_meta = decodeMeta(std::string(chunk.data.begin(), chunk.data.end()));
        }
        m_chunks.push_back(std::move(chunk));
    }

    return true;
}

const Reader::Chunk *Reader::find(uint32_t id) const
{
    for (const Chunk &chunk : m_chunks)
    {
        if (chunk.id == id)
        {
            return &chunk;
        }
    }
    return nullptr;
}

bool Reader::copyChunkInto(uint32_t id, void *dst, size_t bytes, std::string *error) const
{
    const Chunk *chunk = find(id);
    if (!chunk)
    {
        if (error)
        {
            char name[5] = {static_cast<char>(id & 0xFFu), static_cast<char>((id >> 8) & 0xFFu),
                            static_cast<char>((id >> 16) & 0xFFu),
                            static_cast<char>((id >> 24) & 0xFFu), '\0'};
            *error = std::string("snapshot is missing required chunk '") + name + "'";
        }
        return false;
    }
    if (chunk->data.size() != bytes)
    {
        if (error)
        {
            char name[5] = {static_cast<char>(id & 0xFFu), static_cast<char>((id >> 8) & 0xFFu),
                            static_cast<char>((id >> 16) & 0xFFu),
                            static_cast<char>((id >> 24) & 0xFFu), '\0'};
            *error = std::string("snapshot chunk '") + name + "' is " +
                     std::to_string(chunk->data.size()) + " bytes, this build expects " +
                     std::to_string(bytes);
        }
        return false;
    }
    if (bytes != 0u)
    {
        std::memcpy(dst, chunk->data.data(), bytes);
    }
    return true;
}

} // namespace rrv::snapshot
