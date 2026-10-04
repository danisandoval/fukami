// Snapshot container + CLI gate (docs/SNAPSHOTS.md).
//
// Scope: everything in the snapshot module that does NOT need a guest image —
// the .rrvsnap container (write/read/CRC/alignment/tolerance rules) and the
// command-line parser. Those are the parts whose contract other tools and
// future format versions depend on.
//
// Explicitly out of scope: capture/restore against a live PS2Runtime. That
// needs a booted game and is validated by the end-to-end procedure in
// docs/SNAPSHOTS.md §6, not here.

#include "rrv_snapshot.h"
#include "rrv_snapshot_file.h"
#include "rrv_snapshot_format.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const std::string &what)
{
    if (condition)
    {
        return;
    }
    ++g_failures;
    std::cerr << "FAIL: " << what << "\n";
}

std::filesystem::path scratchDir()
{
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "rrv_snapshot_tests";
    std::filesystem::create_directories(dir);
    return dir;
}

using namespace rrv::snapshot;

void testRoundTrip(const std::filesystem::path &dir)
{
    const std::filesystem::path path = dir / "roundtrip.rrvsnap";

    // Payload sizes chosen to straddle the 16-byte chunk alignment (7 is not a
    // multiple of 16, 64 is) — an alignment bug shows up as a corrupt second
    // chunk, which is exactly what this catches.
    const std::string metaText = encodeMeta({{"name", "roundtrip"}, {"elf.bytes", "1234"}});
    std::vector<uint8_t> odd(7u);
    for (size_t i = 0; i < odd.size(); ++i)
    {
        odd[i] = static_cast<uint8_t>(i * 13u + 1u);
    }
    std::vector<uint8_t> even(64u, 0xABu);

    {
        Writer writer;
        std::string error;
        check(writer.open(path, &error), "writer opens: " + error);
        check(writer.addChunk(chunk::kMeta, 1u, metaText.data(), metaText.size(), &error),
              "meta chunk written: " + error);
        check(writer.addChunk(chunk::kScratchpad, 1u, odd.data(), odd.size(), &error),
              "odd chunk written: " + error);
        check(writer.addChunk(chunk::kEeRam, 1u, even.data(), even.size(), &error),
              "even chunk written: " + error);
        check(writer.finish(&error), "writer finishes: " + error);
    }

    check(std::filesystem::exists(path), "snapshot file exists after finish()");
    check(!std::filesystem::exists(std::filesystem::path(path).concat(".partial")),
          "temporary .partial file is cleaned up");

    Reader reader;
    std::string error;
    check(reader.load(path, &error), "reader loads: " + error);
    check(reader.header().chunkCount == 3u, "chunk count is 3");
    check(reader.meta().at("name") == "roundtrip", "meta survives the round trip");
    check(reader.meta().at("elf.bytes") == "1234", "second meta key survives");

    const Reader::Chunk *oddChunk = reader.find(chunk::kScratchpad);
    check(oddChunk != nullptr && oddChunk->data == odd, "unaligned payload round-trips");
    const Reader::Chunk *evenChunk = reader.find(chunk::kEeRam);
    check(evenChunk != nullptr && evenChunk->data == even, "aligned payload round-trips");
    check(reader.find(chunk::kGsVram) == nullptr, "absent chunk reports as absent");

    // copyChunkInto is the restore path's only entry point: it must refuse a
    // size mismatch rather than partially fill guest memory.
    std::vector<uint8_t> dst(even.size());
    check(reader.copyChunkInto(chunk::kEeRam, dst.data(), dst.size(), &error),
          "copyChunkInto accepts the exact size");
    check(dst == even, "copyChunkInto copies the right bytes");
    std::vector<uint8_t> wrongSize(even.size() + 1u, 0u);
    check(!reader.copyChunkInto(chunk::kEeRam, wrongSize.data(), wrongSize.size(), &error),
          "copyChunkInto rejects a size mismatch");
    check(!reader.copyChunkInto(chunk::kGsVram, dst.data(), dst.size(), &error),
          "copyChunkInto rejects a missing chunk");
}

void testCorruptionIsDetected(const std::filesystem::path &dir)
{
    const std::filesystem::path path = dir / "corrupt.rrvsnap";
    std::vector<uint8_t> payload(256u, 0x5Au);
    {
        Writer writer;
        std::string error;
        check(writer.open(path, &error), "corrupt-case writer opens");
        check(writer.addChunk(chunk::kEeRam, 1u, payload.data(), payload.size(), &error),
              "corrupt-case chunk written");
        check(writer.finish(&error), "corrupt-case writer finishes");
    }

    // Flip one byte deep inside the payload.
    {
        std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
        file.seekp(static_cast<std::streamoff>(kFileHeaderBytes + kChunkHeaderBytes + 100),
                   std::ios::beg);
        const char flipped = static_cast<char>(0xA5);
        file.write(&flipped, 1);
    }

    Reader reader;
    std::string error;
    check(!reader.load(path, &error), "a flipped payload byte fails the CRC check");
    check(error.find("CRC") != std::string::npos, "the CRC failure says so: " + error);
}

void testRejectsForeignFiles(const std::filesystem::path &dir)
{
    const std::filesystem::path path = dir / "notasnapshot.rrvsnap";
    {
        std::ofstream out(path, std::ios::binary);
        out << "this is not a snapshot, it is a text file";
    }

    Reader reader;
    std::string error;
    check(!reader.load(path, &error), "a non-snapshot file is rejected");

    Reader missing;
    check(!missing.load(dir / "does_not_exist.rrvsnap", &error),
          "a missing file is rejected");
}

void testCrc()
{
    // Pinned vector: CRC-32 of "123456789" is 0xCBF43926 (the standard check
    // value for this polynomial). If this drifts, existing snapshots stop
    // loading, so it is worth an explicit anchor.
    const char *data = "123456789";
    check(crc32(data, 9u) == 0xCBF43926u, "CRC-32 matches the standard check value");
    check(crc32("", 0u) == 0u, "CRC-32 of nothing is 0");
}

void testCommandLine()
{
    auto parse = [](std::vector<const char *> args) {
        std::vector<char *> argv;
        argv.reserve(args.size());
        for (const char *arg : args)
        {
            argv.push_back(const_cast<char *>(arg));
        }
        return parseCommandLine(static_cast<int>(argv.size()), argv.data());
    };

    // Back-compat: the pre-existing invocation must keep working untouched.
    {
        const ParseResult r = parse({"rrv-recomp", "local/rrv_boot.elf"});
        check(r.ok, "bare ELF argument still parses");
        check(r.options.elf == std::filesystem::path("local/rrv_boot.elf"), "ELF path kept");
        check(r.options.restoreName.empty() && r.options.saveName.empty(),
              "no snapshot work is implied by a plain run");
    }

    {
        const ParseResult r = parse({"rrv-recomp", "--snapshot", "girl", "boot.elf"});
        check(r.ok, "--snapshot parses");
        check(r.options.restoreName == "girl", "--snapshot name captured");
        check(r.options.elf == std::filesystem::path("boot.elf"), "ELF still found after options");
    }

    {
        const ParseResult r =
            parse({"rrv-recomp", "--snapshot-save", "race", "--snapshot-at", "vsync:1200",
                   "--snapshot-exit", "boot.elf"});
        check(r.ok, "save + trigger parses");
        check(r.options.trigger == TriggerKind::Vsync, "vsync trigger kind");
        check(r.options.triggerValue == 1200u, "vsync trigger value");
        check(r.options.exitAfterSave, "--snapshot-exit captured");
    }

    {
        const ParseResult r = parse({"rrv-recomp", "--snapshot-save", "x", "--snapshot-at",
                                     "pc:100abc", "boot.elf"});
        check(r.ok, "pc trigger parses");
        check(r.options.trigger == TriggerKind::Pc, "pc trigger kind");
        check(r.options.triggerValue == 0x100abcu, "pc trigger value is hex");
    }

    {
        const ParseResult r = parse({"rrv-recomp", "--snapshot-save", "x", "boot.elf"});
        check(r.ok && r.options.trigger == TriggerKind::Now,
              "--snapshot-save defaults to the 'now' trigger");
    }

    // Guest-state triggers. These are what make a checkpoint reproducible, so
    // their parsing is worth pinning: a mis-parsed address or threshold gives a
    // checkpoint at the wrong moment, which is far worse than a parse error.
    {
        const ParseResult r =
            parse({"rrv-recomp", "--snapshot-save", "girl", "--snapshot-at", "latch", "boot.elf"});
        check(r.ok && r.options.trigger == TriggerKind::Latch, "latch trigger parses");
    }

    {
        const ParseResult r = parse({"rrv-recomp", "--snapshot-save", "x", "--snapshot-at",
                                     "mem32:334E94=2", "boot.elf"});
        check(r.ok, "mem32 trigger parses");
        check(r.options.trigger == TriggerKind::Mem32, "mem32 trigger kind");
        check(r.options.triggerValue == 0x334E94u, "mem32 address is hex");
        check(r.options.triggerValue2 == 2u, "mem32 value is hex");
    }

    {
        const ParseResult r = parse({"rrv-recomp", "--snapshot-save", "x", "--snapshot-at",
                                     "gif:3:900000", "boot.elf"});
        check(r.ok, "gif signature trigger parses");
        check(r.options.trigger == TriggerKind::Gif, "gif trigger kind");
        check(r.options.triggerValue == 3u, "gif path id is decimal");
        check(r.options.triggerValue2 == 900000u, "gif size threshold is decimal");
    }

    check(!parse({"rrv-recomp", "--snapshot-save", "x", "--snapshot-at", "mem32:334E94",
                  "boot.elf"})
               .ok,
          "mem32 without a value is an error");
    check(!parse({"rrv-recomp", "--snapshot-save", "x", "--snapshot-at", "gif:3", "boot.elf"}).ok,
          "gif without a size threshold is an error");

    {
        const ParseResult r = parse({"rrv-recomp", "--snapshot-trace", "/tmp/t.txt", "boot.elf"});
        check(r.ok && r.options.tracePath == std::filesystem::path("/tmp/t.txt"),
              "--snapshot-trace parses on its own");
    }

    check(!parse({"rrv-recomp", "--snapshot"}).ok, "--snapshot without a value is an error");
    check(!parse({"rrv-recomp", "--nonsense", "boot.elf"}).ok, "unknown options are an error");
    check(!parse({"rrv-recomp", "--snapshot-at", "vsync:10", "boot.elf"}).ok,
          "--snapshot-at without --snapshot-save is an error");
    check(!parse({"rrv-recomp", "--snapshot-save", "a", "--snapshot-at", "bogus:1", "boot.elf"}).ok,
          "an unknown trigger kind is an error");
    check(!parse({"rrv-recomp", "--snapshot", "a", "--snapshot-save", "a", "boot.elf"}).ok,
          "restoring and saving the same name is an error");
}

void testPathResolution()
{
    const std::filesystem::path store = "/tmp/store";
    check(resolveSnapshotPath(store, "girl") == store / "girl.rrvsnap",
          "a bare name resolves inside the store");
    check(resolveSnapshotPath(store, "/elsewhere/x.rrvsnap") ==
              std::filesystem::path("/elsewhere/x.rrvsnap"),
          "an absolute path is used as-is");
    check(resolveSnapshotPath(store, "sub/dir/x.rrvsnap") ==
              std::filesystem::path("sub/dir/x.rrvsnap"),
          "a relative path with separators is used as-is");
}

} // namespace

int main()
{
    const std::filesystem::path dir = scratchDir();

    testCrc();
    testRoundTrip(dir);
    testCorruptionIsDetected(dir);
    testRejectsForeignFiles(dir);
    testCommandLine();
    testPathResolution();

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    if (g_failures != 0)
    {
        std::cerr << g_failures << " snapshot container check(s) failed\n";
        return 1;
    }
    std::cout << "snapshot container: all checks passed\n";
    return 0;
}
