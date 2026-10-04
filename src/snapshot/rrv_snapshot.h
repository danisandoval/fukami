// rrv_snapshot.h — outer (host-side) API of the developer snapshot system.
//
// main.cpp uses this; the vendored runtime never sees it (it only sees
// rrv_snapshot_hooks.h).  Responsibilities:
//
//   * parse the snapshot command-line options,
//   * resolve a snapshot NAME to a file in the snapshot store,
//   * arm a capture or a restore before the runtime starts,
//   * list the store.
//
// A "name" is any filesystem-safe identifier — `boot`, `namco_logo`,
// `attract_start`, `girl`, `race`, `bug1234_repro`. The system attaches no
// meaning to any particular name: the well-known set is a convention documented
// in docs/SNAPSHOTS.md, not a table in the code. That is deliberate — a
// game-specific checkpoint table would make this a Ridge-Racer-V debugging hack
// instead of a reusable part of the runtime.

#ifndef RRV_SNAPSHOT_H
#define RRV_SNAPSHOT_H

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

class PS2Runtime;

namespace rrv::snapshot {

// When a capture fires, relative to the start of guest execution.
enum class TriggerKind
{
    None,
    Now,     // the first dispatch safepoint after the guest starts
    Vsync,   // after N vsync ticks (the runtime's ~60 Hz frame counter)
    Seconds, // after N wall-clock seconds of guest execution
    Pc,      // the first time the dispatcher is entered with ctx->pc == value
    Mem32,   // the first safepoint at which a guest word equals a value
    Latch,   // the first safepoint after some other code calls requestCapture()
    Gif,     // the first safepoint after a GIF packet matching a size signature
};

// Arms a Latch capture from outside the snapshot module.
//
// This is how a checkpoint gets a *guest-state* trigger without teaching the
// snapshot framework anything about a particular game: the condition is
// evaluated wherever that knowledge already lives (a patch, a diagnostic), and
// the capture still happens at the next dispatch safepoint, which is the only
// place it is valid. Idempotent, cheap, and a no-op unless `--snapshot-at
// latch` armed a capture.
//
// `reason` is recorded in the snapshot's metadata so a stored checkpoint says
// what condition produced it.
void requestCapture(const char *reason);

// True when this module will trigger the (armed) GS recorder itself, at the
// checkpoint instant.
//
// Anything else that also knows how to trigger the recorder — src/patches.cpp
// arms it at RRV's scene anchors — must stand down while this is true, or the
// two runs being compared start recording at slightly different instants: the
// scene-anchor predicate is evaluated inside guest code, the checkpoint is taken
// at the next dispatch safepoint, and the packets emitted in between land in one
// recording and not the other. Making the checkpoint instant authoritative is
// what lets a continuous and a restored recording be compared packet for packet.
bool ownsRecorderArming();

struct Options
{
    std::filesystem::path elf;       // positional argument (unchanged behaviour)
    std::filesystem::path storeDir;  // --snapshot-dir, default <elf dir>/snapshots
    std::string restoreName;         // --snapshot <name|path>
    std::string saveName;            // --snapshot-save <name>
    TriggerKind trigger = TriggerKind::None;
    uint64_t triggerValue = 0u;      // --snapshot-at <kind>:<value> (mem32: address)
    uint64_t triggerValue2 = 0u;     // mem32: the value the word must equal
    std::filesystem::path tracePath; // --snapshot-trace, per-frame state hashes
    bool exitAfterSave = false;      // --snapshot-exit
    bool listStore = false;          // --list-snapshots
    bool showHelp = false;           // --help / -h
};

struct ParseResult
{
    bool ok = false;
    std::string error;
    Options options;
};

// Parses argv. Unknown options are an error; a bare positional argument is the
// ELF path, so `rrv-recomp local/rrv_boot.elf` keeps working exactly as before.
ParseResult parseCommandLine(int argc, char *argv[]);

// Usage text for --help and for argument errors.
std::string usageText(const char *programName);

// Prints every snapshot in `storeDir` with its metadata. Returns false when the
// directory does not exist.
bool listStore(const std::filesystem::path &storeDir, std::string *error);

// Arms the system from parsed options. Must be called before PS2Runtime::run().
// Returns false (with `error` set) when the request cannot be satisfied — e.g.
// `--snapshot foo` naming a snapshot that is not in the store. Nothing is
// touched in the runtime here; the work happens in the hooks.
bool configure(const Options &options, std::string *error);

// Resolves a name or path to a snapshot file. A value containing a path
// separator or ending in .rrvsnap is taken as a path; anything else is
// <storeDir>/<name>.rrvsnap.
std::filesystem::path resolveSnapshotPath(const std::filesystem::path &storeDir,
                                          const std::string &nameOrPath);

// Identity of the guest image a snapshot must be restored against. Recorded in
// META at capture and compared at restore, so a snapshot taken from a different
// ELF (or a truncated one) fails with a clear message instead of executing
// somebody else's memory image.
struct GuestImageId
{
    std::string fileName;
    uint64_t fileBytes = 0u;
    uint64_t entryPoint = 0u;
};
GuestImageId identifyGuestImage(const std::filesystem::path &elf);

} // namespace rrv::snapshot

#endif // RRV_SNAPSHOT_H
