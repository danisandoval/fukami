// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team (sector layout adapted from PCSX2 2.8.2
//   pcsx2/CDVD/ChdFileReader.cpp, ChdFileReader::Open2/ParseTOC/ReadChunk); 2026 RRV-Recomp
// SPDX-License-Identifier: GPL-3.0+
// See disc_extract.h. CHD sector layout follows PCSX2 2.8.2 pcsx2/CDVD/ChdFileReader.cpp
// (GPL-3.0+); libchdr is BSD-3-Clause (3rdparty/libchdr/LICENSE.txt in that tree).
#include "disc_extract.h"

#if defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#endif
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <libchdr/cdrom.h>
#include <libchdr/chd.h>
#include <numeric>

namespace fukami::disc {
namespace detail {
namespace {

constexpr ExpectedFile kRr5Files[] = {
    {"IOPRP15.IMG", 82741, "d265e934cbe50de4b82d24dc67e629430676dce469890bd41ad3d824cee1485f"},
    {"MCMAN.IRX", 69589, "546a6afcd3b15d08158d5de1243f84f129af4e86bd4bf507adfb6a070e332fb9"},
    {"MCSERV.IRX", 5829, "55541207ebb75b04feb75ee363c264adf55f8cefff2528dc9acc5e0c3b88b933"},
    {"PADMAN.IRX", 39629, "3570d7619f69c2687ad5cde26dd07374300a2a02f92c53bff877dd31a484c6ef"},
    {"R5.ALL", 481452032, "b1d5255db87ca8f02c8330174fb62d779924cda6b7daa917c5f3922d88e20970"},
    {"RSPU2DRV.IRX", 136157, "9e4063dc76d8c21eea7e06aca8ec8b132731f8323d7787851d979a6c6dfac133"},
    {"SIO2MAN.IRX", 8317, "d86c1c84564bfb3d6a06ad0f851e0c3af8a8045d8e2e692b5e4cf0327ea1ebb1"},
    {"SLUS_200.02", 1295332, "fb5b4d3f6d74384cee9da4c8b954b40a0205da0a9ae7db9e68d670156f5e706f"},
    {"SYSTEM.CNF", 56, "5fa5bdb8c34d8432869749baa3f63aa86b79d90cbdd345af3ff2d89947dca4de"},
};

constexpr uint32_t kMaxDirBytes = 4u << 20;  // sanity cap for the root directory extent
constexpr uint32_t kMaxCnfBytes = 4096;
constexpr uint32_t kChunkSectors = 256;      // 512 KiB per write / progress step

uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }

char lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

Result ok() { return {Status::ok, {}}; }

uint64_t totalBytes(const Profile& p)
{
    return std::accumulate(p.files.begin(), p.files.end(), uint64_t{0},
                           [](uint64_t a, const ExpectedFile& f) { return a + f.size; });
}

bool readBytes(SectorSource& src, uint32_t lba, uint32_t size, std::vector<uint8_t>& out)
{
    const uint32_t sectors = (size + kSectorSize - 1) / kSectorSize;
    if (uint64_t(lba) + sectors > src.sectorCount())
        return false;
    out.resize(size_t(sectors) * kSectorSize);
    for (uint32_t i = 0; i < sectors; ++i)
        if (!src.readSector(lba + i, out.data() + size_t(i) * kSectorSize))
            return false;
    out.resize(size);
    return true;
}

// ---------------------------------------------------------------------------
// CHD-backed source
// ---------------------------------------------------------------------------
constexpr uint32_t kDvdTag = CHD_MAKE_TAG('D', 'V', 'D', ' ');

class ChdSource final : public SectorSource {
public:
    ~ChdSource() override
    {
        if (chd_)
            chd_close(chd_);
    }

    Result open(const std::filesystem::path& path)
    {
        chd_error e = chd_open(path.c_str(), CHD_OPEN_READ, nullptr, &chd_);
        if (e != CHDERR_NONE) {
            chd_ = nullptr;
            if (e == CHDERR_REQUIRES_PARENT)
                return {Status::damaged, "This CHD needs a parent CHD file, which is not supported."};
            if (e == CHDERR_UNSUPPORTED_VERSION)
                return {Status::notChd, "This CHD file uses an unsupported version."};
            return {Status::damaged, "The CHD file is damaged or incomplete."};
        }
        const chd_header* h = chd_get_header(chd_);
        hunkBytes_ = h->hunkbytes;
        unitBytes_ = h->unitbytes;
        if (hunkBytes_ == 0 || unitBytes_ == 0 || hunkBytes_ % unitBytes_ != 0)
            return {Status::damaged, "The CHD file header is invalid."};
        hunk_.resize(hunkBytes_);

        uint32_t offsets[4] = {0, 16, 24, 8};
        int candidates = 0;
        if (!parseCdTrack(offsets[0])) {
            // No CD track metadata: DVD image (raw 2048-byte sectors) or something else.
            char buf[16];
            uint32_t len = 0;
            const bool dvd = chd_get_metadata(chd_, kDvdTag, 0, buf, sizeof buf, &len, nullptr, nullptr) == CHDERR_NONE ||
                             unitBytes_ == kSectorSize;
            if (!dvd)
                return {Status::notIso9660, "This CHD is not a CD or DVD image."};
            dataOffset_ = 0;
            firstFrame_ = 0;
            sectors_ = uint32_t(std::min<uint64_t>(h->logicalbytes / kSectorSize, uint64_t(h->unitcount) * unitBytes_ / kSectorSize));
            candidates = 0;  // fixed layout
        } else {
            candidates = 4;
        }
        if (unitBytes_ < kSectorSize)
            return {Status::damaged, "The CHD file header is invalid."};

        // CD frames: use the offset implied by the track type, but confirm it against the
        // ISO9660 signature at sector 16 and fall back to the other known layouts.
        if (candidates) {
            uint8_t sec[kSectorSize];
            bool found = false;
            for (int i = 0; i < candidates && !found; ++i) {
                if (offsets[i] + kSectorSize > unitBytes_)
                    continue;
                dataOffset_ = offsets[i];
                found = readSector(16, sec) && std::memcmp(sec + 1, "CD001", 5) == 0;
            }
            if (!found)
                dataOffset_ = offsets[0];  // let the ISO9660 parser report notIso9660/damaged
        }
        return ok();
    }

    uint32_t sectorCount() const override { return sectors_; }

    bool readSector(uint32_t lba, uint8_t* out) override
    {
        if (lba >= sectors_)
            return false;
        const uint64_t frame = uint64_t(firstFrame_) + lba;
        const uint32_t perHunk = hunkBytes_ / unitBytes_;
        const uint64_t hunk = frame / perHunk;
        if (hunk != cachedHunk_) {
            cachedHunk_ = UINT64_MAX;
            if (chd_read(chd_, uint32_t(hunk), hunk_.data()) != CHDERR_NONE)
                return false;
            cachedHunk_ = hunk;
        }
        std::memcpy(out, hunk_.data() + (frame % perHunk) * unitBytes_ + dataOffset_, kSectorSize);
        return true;
    }

private:
    // Track 1 metadata (v2, then v1). Sets sectors_/firstFrame_/dataOffset_; false if absent.
    bool parseCdTrack(uint32_t& typeOffset)
    {
        char meta[256], type[256], sub[256], pgType[256], pgSub[256];
        int track = 0, frames = 0, pregap = 0, postgap = 0;
        bool found = false;
        for (uint32_t idx = 0; idx < CD_MAX_TRACKS && !found; ++idx) {
            uint32_t len = 0;
            pregap = 0;
            pgType[0] = 0;
            if (chd_get_metadata(chd_, CDROM_TRACK_METADATA2_TAG, idx, meta, sizeof meta, &len, nullptr, nullptr) == CHDERR_NONE) {
                if (std::sscanf(meta, CDROM_TRACK_METADATA2_FORMAT, &track, type, sub, &frames, &pregap, pgType, pgSub, &postgap) != 8)
                    return false;
            } else if (chd_get_metadata(chd_, CDROM_TRACK_METADATA_TAG, idx, meta, sizeof meta, &len, nullptr, nullptr) == CHDERR_NONE) {
                if (std::sscanf(meta, CDROM_TRACK_METADATA_FORMAT, &track, type, sub, &frames) != 4)
                    return false;
            } else {
                return false;
            }
            found = (track == 1);
        }
        if (!found || frames <= 0)
            return false;
        const std::string_view t = type;
        // Offset of the 2048 user bytes inside one CHD frame, by track type (chdman naming).
        typeOffset = t == "MODE1_RAW" ? 16 : t == "MODE2_RAW" ? 24 : t == "MODE2_FORM_MIX" || t == "MODE2" ? 8 : 0;
        // A pregap of type "V..." is stored in the frame data ahead of the track.
        firstFrame_ = pgType[0] == 'V' ? uint32_t(pregap) : 0;
        sectors_ = uint32_t(frames);
        return true;
    }

    chd_file* chd_ = nullptr;
    uint32_t hunkBytes_ = 0, unitBytes_ = 0, dataOffset_ = 0, firstFrame_ = 0, sectors_ = 0;
    std::vector<uint8_t> hunk_;
    uint64_t cachedHunk_ = UINT64_MAX;
};

// ---------------------------------------------------------------------------
// Extraction plumbing
// ---------------------------------------------------------------------------
// Removes any temp file that was not committed (renamed) - also on cancel/error paths.
class TempFiles {
public:
    ~TempFiles()
    {
        std::error_code ec;
        for (const auto& p : paths_)
            std::filesystem::remove(p, ec);
    }
    void add(std::filesystem::path p) { paths_.push_back(std::move(p)); }
    void committed(size_t i) { paths_[i].clear(); }
    const std::filesystem::path& path(size_t i) const { return paths_[i]; }
    size_t size() const { return paths_.size(); }
private:
    std::vector<std::filesystem::path> paths_;
};

class Fd {
public:
    explicit Fd(int fd) : fd_(fd) {}
    ~Fd() { close(); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    bool valid() const { return fd_ >= 0; }
    int get() const { return fd_; }
    bool close()
    {
        if (fd_ < 0)
            return true;
        const bool good = ::close(fd_) == 0;
        fd_ = -1;
        return good;
    }
private:
    int fd_;
};

Status writeStatus(int err) { return (err == ENOSPC || err == EDQUOT) ? Status::noSpace : Status::writeFailed; }

Result writeError(int err)
{
    if (writeStatus(err) == Status::noSpace)
        return {Status::noSpace, "The destination ran out of free space."};
    return {Status::writeFailed, std::string("Could not write to the destination folder: ") + std::strerror(err)};
}

bool writeAll(int fd, const uint8_t* p, size_t n, int& err)
{
    while (n) {
        const ssize_t w = ::write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            err = errno;
            return false;
        }
        p += w;
        n -= size_t(w);
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public detail API
// ---------------------------------------------------------------------------
const Profile& rr5UsaProfile()
{
    static const Profile p{kRr5Files, "cdrom0:\\slus_200.02;1"};
    return p;
}

#if defined(__APPLE__)
Sha256::Sha256()
{
    static_assert(sizeof(CC_SHA256_CTX) <= sizeof(ctx_));
    CC_SHA256_Init(reinterpret_cast<CC_SHA256_CTX*>(ctx_));
}

void Sha256::update(const void* data, size_t len)
{
    auto* c = reinterpret_cast<CC_SHA256_CTX*>(ctx_);
    const auto* p = static_cast<const uint8_t*>(data);
    while (len) {  // CC_LONG is 32-bit
        const size_t n = std::min<size_t>(len, 1u << 30);
        CC_SHA256_Update(c, p, CC_LONG(n));
        p += n;
        len -= n;
    }
}

std::string Sha256::finishHex()
{
    unsigned char d[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(d, reinterpret_cast<CC_SHA256_CTX*>(ctx_));
    static const char hex[] = "0123456789abcdef";
    std::string s;
    for (unsigned char b : d) {
        s += hex[b >> 4];
        s += hex[b & 15];
    }
    return s;
}
#else
// Linux (Gate 5): self-contained FIPS 180-4 SHA-256 (no OpenSSL on the Steam
// Deck runtime). Written for RRV-Recomp from the FIPS 180-4 specification; same
// algorithm as src/host/rrv_resource_package.cpp's Sha256.
namespace {
struct PortableSha256 {
    uint32_t state[8];
    uint64_t bits;
    uint32_t used;
    unsigned char block[64];
};
static_assert(sizeof(PortableSha256) <= 128);

uint32_t rotr32(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

void sha256Transform(PortableSha256& c)
{
    static constexpr uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (uint32_t(c.block[4 * i]) << 24) | (uint32_t(c.block[4 * i + 1]) << 16) |
               (uint32_t(c.block[4 * i + 2]) << 8) | c.block[4 * i + 3];
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = c.state[0], b = c.state[1], cc = c.state[2], d = c.state[3];
    uint32_t e = c.state[4], f = c.state[5], g = c.state[6], h = c.state[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t t1 = h + (rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
        const uint32_t t2 = (rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22)) + ((a & b) ^ (a & cc) ^ (b & cc));
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c.state[0] += a; c.state[1] += b; c.state[2] += cc; c.state[3] += d;
    c.state[4] += e; c.state[5] += f; c.state[6] += g; c.state[7] += h;
}
}  // namespace

Sha256::Sha256()
{
    auto* c = reinterpret_cast<PortableSha256*>(ctx_);
    static constexpr uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                         0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::memcpy(c->state, init, sizeof init);
    c->bits = 0;
    c->used = 0;
}

void Sha256::update(const void* data, size_t len)
{
    auto* c = reinterpret_cast<PortableSha256*>(ctx_);
    const auto* p = static_cast<const uint8_t*>(data);
    c->bits += uint64_t(len) * 8u;
    while (len) {
        const size_t n = std::min<size_t>(len, 64u - c->used);
        std::memcpy(c->block + c->used, p, n);
        c->used += uint32_t(n);
        p += n;
        len -= n;
        if (c->used == 64u) {
            sha256Transform(*c);
            c->used = 0;
        }
    }
}

std::string Sha256::finishHex()
{
    auto* c = reinterpret_cast<PortableSha256*>(ctx_);
    const uint64_t bits = c->bits;
    c->block[c->used++] = 0x80;
    if (c->used > 56u) {
        std::memset(c->block + c->used, 0, 64u - c->used);
        sha256Transform(*c);
        c->used = 0;
    }
    std::memset(c->block + c->used, 0, 56u - c->used);
    for (int i = 0; i < 8; ++i)
        c->block[56 + i] = static_cast<unsigned char>(bits >> (56 - 8 * i));
    sha256Transform(*c);
    static const char hex[] = "0123456789abcdef";
    std::string s;
    for (uint32_t word : c->state)
        for (int shift = 28; shift >= 0; shift -= 4)
            s += hex[(word >> shift) & 15];
    return s;
}
#endif

bool namesMatch(std::string_view isoName, std::string_view expected)
{
    if (const size_t semi = isoName.rfind(';'); semi != std::string_view::npos) {
        const std::string_view ver = isoName.substr(semi + 1);
        if (!ver.empty() && std::all_of(ver.begin(), ver.end(), [](char c) { return c >= '0' && c <= '9'; }))
            isoName = isoName.substr(0, semi);
    }
    return isoName.size() == expected.size() &&
           std::equal(isoName.begin(), isoName.end(), expected.begin(), [](char a, char b) { return lower(a) == lower(b); });
}

Result parseRootDirectory(SectorSource& src, std::vector<IsoEntry>& out)
{
    out.clear();
    uint8_t pvd[kSectorSize];
    if (src.sectorCount() <= 16 || !src.readSector(16, pvd))
        return {Status::notIso9660, "This disc image has no ISO9660 file system."};
    if (pvd[0] != 1 || std::memcmp(pvd + 1, "CD001", 5) != 0)
        return {Status::notIso9660, "This disc image has no ISO9660 file system."};

    const uint8_t* root = pvd + 156;
    const uint32_t rootLba = le32(root + 2), rootSize = le32(root + 10);
    if (root[0] < 34 || !(root[25] & 2) || rootSize == 0 || rootSize > kMaxDirBytes)
        return {Status::notIso9660, "The disc image's root directory is invalid."};

    std::vector<uint8_t> dir;
    if (!readBytes(src, rootLba, rootSize, dir))
        return {Status::damaged, "The disc image's root directory cannot be read."};

    size_t pos = 0;
    while (pos < dir.size()) {
        const uint8_t len = dir[pos];
        if (len == 0) {  // zero padding to the end of the sector
            pos = (pos / kSectorSize + 1) * kSectorSize;
            continue;
        }
        if (len < 34 || pos + len > dir.size() || 33u + dir[pos + 32] > len)
            return {Status::damaged, "The disc image's root directory is corrupt."};
        const uint8_t* r = &dir[pos];
        const uint8_t nameLen = r[32];
        // "." and ".." are encoded as a single 0x00 / 0x01 byte.
        if (!(nameLen == 1 && r[33] <= 1)) {
            IsoEntry e;
            e.name.assign(reinterpret_cast<const char*>(r + 33), nameLen);
            e.lba = le32(r + 2);
            e.size = le32(r + 10);
            e.isDir = (r[25] & 2) != 0;
            out.push_back(std::move(e));
        }
        pos += len;
    }
    return ok();
}

Result probeSource(SectorSource& src, const Profile& profile, std::vector<IsoEntry>* matched)
{
    std::vector<IsoEntry> entries;
    if (Result r = parseRootDirectory(src, entries); r.status != Status::ok)
        return r;

    std::vector<IsoEntry> found;
    for (const ExpectedFile& want : profile.files) {
        const auto it = std::find_if(entries.begin(), entries.end(),
                                     [&](const IsoEntry& e) { return !e.isDir && namesMatch(e.name, want.name); });
        if (it == entries.end() || it->size != want.size)
            return {Status::wrongDisc, "This is not the Ridge Racer V (USA) disc."};
        if (uint64_t(it->lba) + (it->size + kSectorSize - 1) / kSectorSize > src.sectorCount())
            return {Status::damaged, "The disc image is incomplete."};
        found.push_back(*it);
    }

    // SYSTEM.CNF must name the expected boot executable.
    for (size_t i = 0; i < profile.files.size(); ++i) {
        if (!namesMatch(profile.files[i].name, "SYSTEM.CNF"))
            continue;
        std::vector<uint8_t> cnf;
        if (found[i].size > kMaxCnfBytes || !readBytes(src, found[i].lba, found[i].size, cnf))
            return {Status::damaged, "SYSTEM.CNF cannot be read from the disc image."};
        std::string text;  // lowercase, no whitespace, '/' folded to '\'
        for (uint8_t c : cnf)
            if (c > ' ')
                text += c == '/' ? '\\' : lower(char(c));
        if (text.find(std::string("boot2=") + std::string(profile.boot)) == std::string::npos)
            return {Status::wrongDisc, "This is not the Ridge Racer V (USA) disc."};
    }
    if (matched)
        *matched = std::move(found);
    return ok();
}

Result extractSource(SectorSource& src, const Profile& profile, const std::filesystem::path& destDir,
                     const std::function<bool(const Progress&)>& progress)
{
    namespace fs = std::filesystem;
    std::vector<IsoEntry> files;
    if (Result r = probeSource(src, profile, &files); r.status != Status::ok)
        return r;

    const uint64_t total = totalBytes(profile);
    std::error_code ec;
    fs::create_directories(destDir, ec);
    if (ec || !fs::is_directory(destDir, ec))
        return {Status::writeFailed, "Could not create the destination folder."};
    const fs::space_info space = fs::space(destDir, ec);
    if (!ec && space.available < total + (1u << 20))
        return {Status::noSpace, "Not enough free space: " + std::to_string((total + (1u << 20) + 999999) / 1000000) + " MB needed."};

    TempFiles temps;
    uint64_t done = 0;
    Progress prog{0, total, {}};
    auto report = [&]() {
        prog.bytesDone = done;
        return !progress || progress(prog);
    };
    std::vector<uint8_t> buf(size_t(kChunkSectors) * kSectorSize);

    for (size_t i = 0; i < profile.files.size(); ++i) {
        const ExpectedFile& want = profile.files[i];
        prog.currentFile = std::string(want.name);
        const fs::path tmp = destDir / (".fukami-" + std::string(want.name) + ".part");
        temps.add(tmp);
        Fd fd(::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644));
        if (!fd.valid())
            return writeError(errno);
        if (!report())
            return {Status::cancelled, "Cancelled."};

        Sha256 sha;
        uint64_t left = want.size;
        uint32_t lba = files[i].lba;
        while (left) {
            const uint32_t sectors = uint32_t(std::min<uint64_t>(kChunkSectors, (left + kSectorSize - 1) / kSectorSize));
            for (uint32_t s = 0; s < sectors; ++s)
                if (!src.readSector(lba + s, buf.data() + size_t(s) * kSectorSize))
                    return {Status::damaged, "A read error occurred in the disc image (" + prog.currentFile + ")."};
            const size_t n = size_t(std::min<uint64_t>(left, uint64_t(sectors) * kSectorSize));
            sha.update(buf.data(), n);
            int err = 0;
            if (!writeAll(fd.get(), buf.data(), n, err))
                return writeError(err);
            lba += sectors;
            left -= n;
            done += n;
            if (!report())
                return {Status::cancelled, "Cancelled."};
        }
        if (sha.finishHex() != want.sha256)
            return {Status::damaged, prog.currentFile + " does not match the expected checksum. The disc image is damaged or modified."};
        if (::fsync(fd.get()) != 0 || !fd.close())
            return writeError(errno);
    }

    // Every file verified: publish them.
    for (size_t i = 0; i < profile.files.size(); ++i) {
        fs::rename(temps.path(i), destDir / profile.files[i].name, ec);
        if (ec)
            return {Status::writeFailed, "Could not finish writing " + std::string(profile.files[i].name) + "."};
        temps.committed(i);
    }
    return ok();
}

std::unique_ptr<SectorSource> openChd(const std::filesystem::path& path, Result& err)
{
    static constexpr char kMagic[8] = {'M', 'C', 'o', 'm', 'p', 'r', 'H', 'D'};
    {
        std::ifstream f(path, std::ios::binary);
        if (!f) {
            err = {Status::cannotOpen, "The disc image cannot be opened."};
            return nullptr;
        }
        char head[8] = {};
        f.read(head, sizeof head);
        if (f.gcount() != sizeof head || std::memcmp(head, kMagic, sizeof head) != 0) {
            err = {Status::notChd, "This file is not a CHD disc image."};
            return nullptr;
        }
    }
    auto chd = std::make_unique<ChdSource>();
    err = chd->open(path);
    return err.status == Status::ok ? std::move(chd) : nullptr;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Public API. Nothing throws out of these.
// ---------------------------------------------------------------------------
namespace {
template <typename F>
Result guarded(F&& f)
{
    try {
        return f();
    } catch (...) {
        return {Status::damaged, "An unexpected error occurred while reading the disc image."};
    }
}
}  // namespace

uint64_t requiredBytes() { return detail::totalBytes(detail::rr5UsaProfile()); }

Result probeRr5Usa(const std::filesystem::path& chd)
{
    return guarded([&]() -> Result {
        Result err{Status::ok, {}};
        auto src = detail::openChd(chd, err);
        if (!src)
            return err;
        return detail::probeSource(*src, detail::rr5UsaProfile());
    });
}

Result extractRr5Usa(const std::filesystem::path& chd, const std::filesystem::path& destDir,
                     const std::function<bool(const Progress&)>& progress)
{
    return guarded([&]() -> Result {
        Result err{Status::ok, {}};
        auto src = detail::openChd(chd, err);
        if (!src)
            return err;
        return detail::extractSource(*src, detail::rr5UsaProfile(), destDir, progress);
    });
}

}  // namespace fukami::disc
