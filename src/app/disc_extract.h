// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team (sector layout adapted from PCSX2 2.8.2
//   pcsx2/CDVD/ChdFileReader.cpp, ChdFileReader::Open2/ParseTOC/ReadChunk); 2026 RRV-Recomp
// SPDX-License-Identifier: GPL-3.0+
// Ridge Racer V (USA, SLUS-20002) disc extraction from a user-supplied CHD.
//
// Reads the ISO9660 root directory of the user's own dump and writes the nine
// files the runtime needs into a directory of the caller's choosing. Nothing
// game-derived is embedded here: only file names, sizes and SHA-256 fingerprints.
//
// CHD access uses libchdr (BSD-3-Clause) built from the pinned PCSX2 2.8.2 tree;
// the CD/DVD sector layout handling follows pcsx2/CDVD/ChdFileReader.cpp
// (PCSX2 2.8.2, GPL-3.0+, SPDX-FileCopyrightText 2002-2026 PCSX2 Dev Team).
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fukami::disc {

struct Progress {
    uint64_t bytesDone;
    uint64_t bytesTotal;
    std::string currentFile;
};

enum class Status {
    ok,
    cannotOpen,   // the file cannot be opened for reading
    notChd,       // opened, but not a CHD
    notIso9660,   // a CHD, but not a CD/DVD image with an ISO9660 filesystem
    wrongDisc,    // a disc, but not Ridge Racer V (USA)
    damaged,      // read error, or contents do not match the expected fingerprints
    noSpace,
    writeFailed,
    cancelled,
};

struct Result {
    Status status;
    std::string message;  // short, user-facing
};

// Checks the disc image (a CHD, or a .cue/.bin pair), extracts the expected files from the ISO9660 root directory into
// destDir (created if missing) via temp files + atomic rename, verifies every file's
// SHA-256 against the expected list, and returns ok only if all nine match.
// progress may be null; returning false from it cancels (partial temp files removed).
Result extractRr5Usa(const std::filesystem::path& chd, const std::filesystem::path& destDir,
                     const std::function<bool(const Progress&)>& progress);

// Fast check (no extraction): is this CHD the right disc? Reads the ISO9660 root
// directory and SYSTEM.CNF (must name cdrom0:\SLUS_200.02;1) and the file sizes.
Result probeRr5Usa(const std::filesystem::path& chd);

uint64_t requiredBytes();  // total size of the nine files

// Building blocks, exposed so the unit tests can run on synthetic images.
namespace detail {

constexpr uint32_t kSectorSize = 2048;

struct ExpectedFile {
    std::string_view name;    // canonical name as on disc, without ";1"
    uint64_t size;
    std::string_view sha256;  // lowercase hex
};

struct Profile {
    std::span<const ExpectedFile> files;
    std::string_view boot;  // text SYSTEM.CNF must name, e.g. cdrom0:\SLUS_200.02;1
};

const Profile& rr5UsaProfile();

// 2048-byte user-data sectors, addressed by LBA.
class SectorSource {
public:
    virtual ~SectorSource() = default;
    virtual uint32_t sectorCount() const = 0;
    virtual bool readSector(uint32_t lba, uint8_t* out2048) = 0;
};

struct IsoEntry {
    std::string name;  // as on disc, ";version" suffix stripped
    uint32_t lba;
    uint32_t size;
    bool isDir;
};

// Case-insensitive; ignores a trailing ";<digits>" on isoName.
bool namesMatch(std::string_view isoName, std::string_view expected);

// Walks the root directory extent of the primary volume descriptor at sector 16.
// Returns ok, notIso9660 (no PVD / bad root record) or damaged (unreadable/corrupt).
Result parseRootDirectory(SectorSource& src, std::vector<IsoEntry>& out);

// Root directory + expected names/sizes + SYSTEM.CNF boot line. No writes.
Result probeSource(SectorSource& src, const Profile& profile, std::vector<IsoEntry>* matched = nullptr);

Result extractSource(SectorSource& src, const Profile& profile, const std::filesystem::path& destDir,
                     const std::function<bool(const Progress&)>& progress);

// Opens a CHD (CD track metadata or DVD) as a 2048-byte sector source.
// On failure returns null and fills err.
std::unique_ptr<SectorSource> openChd(const std::filesystem::path& path, Result& err);

// Opens a raw image: a .cue sheet (its first data track, in the .bin it names), or a bare .bin/.iso
// (sector size guessed from the file size). Same contract as openChd.
std::unique_ptr<SectorSource> openCue(const std::filesystem::path& path, Result& err);
std::unique_ptr<SectorSource> openBin(const std::filesystem::path& path, Result& err);
// By extension: .cue, .bin/.iso, otherwise a CHD.
std::unique_ptr<SectorSource> openImage(const std::filesystem::path& path, Result& err);

class Sha256 {
public:
    Sha256();
    void update(const void* data, size_t len);
    std::string finishHex();  // lowercase
private:
    alignas(8) unsigned char ctx_[128];
};

}  // namespace detail
}  // namespace fukami::disc
