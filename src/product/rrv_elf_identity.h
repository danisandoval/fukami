// Runtime identity of the game ELF the recompiled game code belongs to.
//
// generated/rr5/output is the C++ extraction of exactly one executable (SLUS_200.02, Ridge Racer V,
// USA). Running it against any other ELF is undefined behaviour at best, so the product refuses to
// start: it hashes the ELF it was asked to load and compares it with the SHA-256 recorded in
// generated/rr5/source-manifest.json (game_input.sha256), embedded at configure time as
// RRV_GAME_ELF_SHA256. Both launch routes (./run.sh and the Fukami.app first-launch path, which
// re-executes this binary with the extracted SLUS_200.02) reach main() with the ELF path.
#pragma once

#include "disc_extract.h"

#include <filesystem>
#include <fstream>
#include <ostream>
#include <string>
#include <string_view>

namespace rrv::product
{
// Returns true when the file's SHA-256 equals expectedSha256 (lowercase hex). Otherwise writes a clear
// message to `log` and returns false.
inline bool verifyGameElfIdentity(const std::filesystem::path &elf, std::string_view expectedSha256,
                                  std::ostream &log)
{
    std::ifstream in(elf, std::ios::binary);
    if (!in)
    {
        log << "[rrv-product] cannot open the game ELF: " << elf.string() << '\n';
        return false;
    }
    fukami::disc::detail::Sha256 hash;
    char buffer[1 << 16];
    while (in)
    {
        in.read(buffer, sizeof buffer);
        if (in.gcount() > 0)
            hash.update(buffer, static_cast<size_t>(in.gcount()));
    }
    if (in.bad())
    {
        log << "[rrv-product] error while reading the game ELF: " << elf.string() << '\n';
        return false;
    }
    const std::string actual = hash.finishHex();
    if (actual != expectedSha256)
    {
        log << "[rrv-product] this is not the game ELF this build was made from.\n"
            << "  file:     " << elf.string() << "\n"
            << "  SHA-256:  " << actual << "\n"
            << "  expected: " << expectedSha256 << "  (SLUS_200.02, Ridge Racer V, USA, SLUS-20002)\n"
            << "The recompiled game code only works with an unmodified SLUS_200.02 from your own copy of that disc.\n";
        return false;
    }
    log << "[rrv-product] game ELF verified sha256=" << actual << '\n';
    return true;
}
} // namespace rrv::product
