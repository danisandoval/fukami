// disc-extract: CLI around fukami::disc, for testing the first-launch extraction path.
//   disc-extract probe <disc.chd>
//   disc-extract extract <disc.chd> <destDir>
// Exit code is the fukami::disc::Status value (0 = ok).
#include "disc_extract.h"

#include <chrono>
#include <cstdio>
#include <cstring>

namespace {
const char* statusName(fukami::disc::Status s)
{
    using S = fukami::disc::Status;
    switch (s) {
    case S::ok: return "ok";
    case S::cannotOpen: return "cannotOpen";
    case S::notChd: return "notChd";
    case S::notIso9660: return "notIso9660";
    case S::wrongDisc: return "wrongDisc";
    case S::damaged: return "damaged";
    case S::noSpace: return "noSpace";
    case S::writeFailed: return "writeFailed";
    case S::cancelled: return "cancelled";
    }
    return "?";
}

int report(const fukami::disc::Result& r)
{
    std::printf("%s: %s\n", statusName(r.status), r.message.c_str());
    return int(r.status);
}
}  // namespace

int main(int argc, char** argv)
{
    if (argc == 3 && !std::strcmp(argv[1], "probe"))
        return report(fukami::disc::probeRr5Usa(argv[2]));
    if (argc == 4 && !std::strcmp(argv[1], "extract")) {
        const auto t0 = std::chrono::steady_clock::now();
        std::string last;
        const auto r = fukami::disc::extractRr5Usa(argv[2], argv[3], [&](const fukami::disc::Progress& p) {
            if (p.currentFile != last) {
                last = p.currentFile;
                std::printf("\n%s ", last.c_str());
            }
            std::printf("\r%-14s %5.1f%%", last.c_str(), 100.0 * double(p.bytesDone) / double(p.bytesTotal));
            std::fflush(stdout);
            return true;
        });
        std::printf("\nrequired %llu bytes, took %.1f s\n", (unsigned long long)fukami::disc::requiredBytes(),
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        return report(r);
    }
    std::fprintf(stderr, "usage: disc-extract probe <disc.chd> | extract <disc.chd> <destDir>\n");
    return 64;
}
