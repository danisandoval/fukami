// rrv_snapshot_cli.cpp — command-line surface + snapshot store for the
// developer snapshot system. No runtime dependency: everything here is
// filesystem and string work, so it stays testable without a guest image.

#include "rrv_snapshot.h"
#include "rrv_snapshot_file.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

namespace rrv::snapshot {

namespace {

bool startsWith(const std::string &s, const char *prefix)
{
    const size_t n = std::strlen(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

bool parseTrigger(const std::string &spec, TriggerKind &kind, uint64_t &value,
                  uint64_t &value2, std::string &error)
{
    if (spec == "now")
    {
        kind = TriggerKind::Now;
        value = 0u;
        return true;
    }
    if (spec == "latch")
    {
        kind = TriggerKind::Latch;
        value = 0u;
        return true;
    }

    const size_t colon = spec.find(':');
    if (colon == std::string::npos)
    {
        error = "--snapshot-at expects now | latch | vsync:<n> | seconds:<n> | "
                "pc:<hexaddr> | mem32:<hexaddr>=<hexvalue>";
        return false;
    }

    const std::string what = spec.substr(0, colon);
    const std::string rest = spec.substr(colon + 1);
    if (rest.empty())
    {
        error = "--snapshot-at " + what + " needs a value";
        return false;
    }

    if (what == "vsync")
    {
        kind = TriggerKind::Vsync;
        value = std::strtoull(rest.c_str(), nullptr, 10);
        return true;
    }
    if (what == "seconds")
    {
        kind = TriggerKind::Seconds;
        value = std::strtoull(rest.c_str(), nullptr, 10);
        return true;
    }
    if (what == "pc")
    {
        kind = TriggerKind::Pc;
        value = std::strtoull(rest.c_str(), nullptr, 16);
        return true;
    }
    if (what == "gif")
    {
        // gif:<path>:<minbytes> — fires after the first GIF packet on <path>
        // (0 = any) of at least <minbytes>. A display-list signature: for a
        // scene with no convenient state word, "the frame that submits the big
        // chain" is the most reliable marker there is, and it is a property of
        // the guest's own output rather than of wall-clock time.
        const size_t sep = rest.find(':');
        if (sep == std::string::npos)
        {
            error = "--snapshot-at gif expects <path>:<minbytes> (path 0 = any)";
            return false;
        }
        kind = TriggerKind::Gif;
        value = std::strtoull(rest.substr(0, sep).c_str(), nullptr, 10);
        value2 = std::strtoull(rest.substr(sep + 1).c_str(), nullptr, 10);
        return true;
    }
    if (what == "mem32")
    {
        // mem32:<hexaddr>=<hexvalue> — fires at the first safepoint where the
        // guest word at <addr> reads <value>. A guest-state trigger: unlike a
        // wall-clock one it names a point in the game's own execution, so the
        // same checkpoint is reproducible across runs and machines.
        const size_t eq = rest.find('=');
        if (eq == std::string::npos)
        {
            error = "--snapshot-at mem32 expects <hexaddr>=<hexvalue>";
            return false;
        }
        kind = TriggerKind::Mem32;
        value = std::strtoull(rest.substr(0, eq).c_str(), nullptr, 16);
        value2 = std::strtoull(rest.substr(eq + 1).c_str(), nullptr, 16);
        return true;
    }

    error = "unknown --snapshot-at kind '" + what + "'";
    return false;
}

} // namespace

std::string usageText(const char *programName)
{
    const std::string prog = programName ? programName : "rrv-recomp";
    std::ostringstream out;
    out << "Usage: " << prog << " [options] <boot.elf>\n"
        << "\n"
        << "Developer snapshots (docs/SNAPSHOTS.md):\n"
        << "  --snapshot <name|path>    Restore this snapshot instead of booting.\n"
        << "  --snapshot-save <name>    Capture a snapshot during this run.\n"
        << "  --snapshot-at <trigger>   When to capture. One of:\n"
        << "                              now             first dispatch safepoint\n"
        << "                              latch           first safepoint after game code\n"
        << "                                              calls requestCapture() (see\n"
        << "                                              src/patches.cpp for RRV's\n"
        << "                                              scene anchors)\n"
        << "                              gif:<path>:<n>  after a GIF packet on <path>\n"
        << "                                              (0=any) of >= <n> bytes\n"
        << "                              mem32:<a>=<v>   first safepoint where the guest\n"
        << "                                              word at hex <a> reads hex <v>\n"
        << "                              pc:<hexaddr>    first dispatch of that PC\n"
        << "                              vsync:<n>       after n vsync ticks (~n/60 s)\n"
        << "                              seconds:<n>     after n seconds of guest run time\n"
        << "                            Default: now. Prefer the guest-state triggers\n"
        << "                            (latch/mem32/pc) — they name a point in the\n"
        << "                            game's execution, so a checkpoint is reproducible.\n"
        << "  --snapshot-exit           Stop the run once the capture is written.\n"
        << "  --snapshot-trace <path>   Write per-frame guest-state hashes (EE context,\n"
        << "                            DMA/VIF registers, VU memory, GS registers,\n"
        << "                            DMA/GIF/GS/VIF counters) for run comparison.\n"
        << "  --snapshot-dir <path>     Snapshot store (default: <elf dir>/snapshots).\n"
        << "  --list-snapshots          List the store and exit.\n"
        << "  -h, --help                This text.\n"
        << "\n"
        << "Snapshots contain game RAM and GS VRAM: they are game-derived data and\n"
        << "must never be committed or shared (same rule as .p2s / .gsr captures).\n";
    return out.str();
}

ParseResult parseCommandLine(int argc, char *argv[])
{
    ParseResult result;
    Options &opts = result.options;

    auto needValue = [&](int &i, const char *name, std::string &out) -> bool {
        if (i + 1 >= argc)
        {
            result.error = std::string(name) + " needs a value";
            return false;
        }
        out = argv[++i];
        return true;
    };

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i] ? argv[i] : "";
        if (arg.empty())
        {
            continue;
        }

        if (arg == "-h" || arg == "--help")
        {
            opts.showHelp = true;
            continue;
        }
        if (arg == "--list-snapshots")
        {
            opts.listStore = true;
            continue;
        }
        if (arg == "--snapshot-exit")
        {
            opts.exitAfterSave = true;
            continue;
        }
        if (arg == "--snapshot")
        {
            if (!needValue(i, "--snapshot", opts.restoreName))
                return result;
            continue;
        }
        if (arg == "--snapshot-save")
        {
            if (!needValue(i, "--snapshot-save", opts.saveName))
                return result;
            continue;
        }
        if (arg == "--snapshot-at")
        {
            std::string spec;
            if (!needValue(i, "--snapshot-at", spec))
                return result;
            if (!parseTrigger(spec, opts.trigger, opts.triggerValue, opts.triggerValue2,
                              result.error))
                return result;
            continue;
        }
        if (arg == "--snapshot-trace")
        {
            std::string path;
            if (!needValue(i, "--snapshot-trace", path))
                return result;
            opts.tracePath = path;
            continue;
        }
        if (arg == "--snapshot-dir")
        {
            std::string dir;
            if (!needValue(i, "--snapshot-dir", dir))
                return result;
            opts.storeDir = dir;
            continue;
        }
        if (startsWith(arg, "-") && arg != "-")
        {
            result.error = "unknown option '" + arg + "'";
            return result;
        }

        if (opts.elf.empty())
        {
            opts.elf = arg;
        }
        else
        {
            result.error = "unexpected extra argument '" + arg + "'";
            return result;
        }
    }

    if (!opts.saveName.empty() && opts.trigger == TriggerKind::None)
    {
        opts.trigger = TriggerKind::Now;
    }
    if (opts.saveName.empty() && opts.trigger != TriggerKind::None)
    {
        result.error = "--snapshot-at without --snapshot-save has nothing to capture";
        return result;
    }
    if (!opts.saveName.empty() && !opts.restoreName.empty() &&
        opts.saveName == opts.restoreName)
    {
        result.error = "--snapshot and --snapshot-save name the same snapshot";
        return result;
    }

    result.ok = true;
    return result;
}

std::filesystem::path resolveSnapshotPath(const std::filesystem::path &storeDir,
                                          const std::string &nameOrPath)
{
    const std::filesystem::path candidate(nameOrPath);
    if (candidate.has_parent_path() || candidate.extension() == ".rrvsnap")
    {
        return candidate;
    }
    return storeDir / (nameOrPath + ".rrvsnap");
}

GuestImageId identifyGuestImage(const std::filesystem::path &elf)
{
    GuestImageId id;
    id.fileName = elf.filename().string();

    std::error_code ec;
    const auto size = std::filesystem::file_size(elf, ec);
    if (!ec)
    {
        id.fileBytes = static_cast<uint64_t>(size);
    }

    // ELF32 entry point lives at offset 0x18. Cheap, and enough (together with
    // the file size) to catch "you booted a different build of the game".
    std::ifstream in(elf, std::ios::binary);
    if (in)
    {
        char magic[4] = {};
        in.read(magic, sizeof(magic));
        if (in && std::memcmp(magic, "\x7F"
                                     "ELF",
                              4) == 0)
        {
            in.seekg(0x18, std::ios::beg);
            uint32_t entry = 0u;
            in.read(reinterpret_cast<char *>(&entry), sizeof(entry));
            if (in)
            {
                id.entryPoint = entry;
            }
        }
    }
    return id;
}

bool listStore(const std::filesystem::path &storeDir, std::string *error)
{
    std::error_code ec;
    if (!std::filesystem::exists(storeDir, ec))
    {
        if (error)
        {
            *error = "snapshot store " + storeDir.string() + " does not exist yet";
        }
        return false;
    }

    std::vector<std::filesystem::path> files;
    for (const auto &entry : std::filesystem::directory_iterator(storeDir, ec))
    {
        if (entry.is_regular_file(ec) && entry.path().extension() == ".rrvsnap")
        {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());

    std::cout << "Snapshot store: " << storeDir.string() << "\n";
    if (files.empty())
    {
        std::cout << "  (empty)\n";
        return true;
    }

    for (const auto &file : files)
    {
        Reader reader;
        std::string readError;
        std::cout << "  " << file.stem().string();
        if (!reader.load(file, &readError))
        {
            std::cout << "  [unreadable: " << readError << "]\n";
            continue;
        }

        const MetaMap &meta = reader.meta();
        auto value = [&](const char *key) -> std::string {
            const auto it = meta.find(key);
            return it == meta.end() ? std::string("?") : it->second;
        };

        std::error_code sizeEc;
        const auto bytes = std::filesystem::file_size(file, sizeEc);
        std::cout << "  elf=" << value("elf.name")
                  << " vsync=" << value("capture.vsync")
                  << " pc=0x" << value("capture.pc")
                  << " threads=" << value("capture.guestThreads")
                  << " size=" << (sizeEc ? 0u : bytes / (1024u * 1024u)) << "MB\n";
        const std::string note = value("note");
        if (note != "?" && !note.empty())
        {
            std::cout << "      note: " << note << "\n";
        }
    }
    return true;
}

} // namespace rrv::snapshot
