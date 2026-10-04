// Unit tests for src/app/disc_extract.* on synthetic images; no game data needed.
#include "disc_extract.h"

#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace fukami::disc;
using namespace fukami::disc::detail;

namespace {
int g_failures = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
            ++g_failures;                                                              \
        }                                                                              \
    } while (0)

// ---- synthetic ISO9660 image -------------------------------------------------
class MemSource final : public SectorSource {
public:
    std::vector<uint8_t> data;
    uint32_t failAt = UINT32_MAX;  // readSector fails at this LBA
    uint32_t sectorCount() const override { return uint32_t(data.size() / kSectorSize); }
    bool readSector(uint32_t lba, uint8_t* out) override
    {
        if (lba == failAt || lba >= sectorCount())
            return false;
        std::memcpy(out, &data[size_t(lba) * kSectorSize], kSectorSize);
        return true;
    }
};

void put32(uint8_t* p, uint32_t v)  // both-endian field
{
    for (int i = 0; i < 4; ++i) {
        p[i] = uint8_t(v >> (8 * i));
        p[7 - i] = uint8_t(v >> (8 * i));
    }
}

size_t putRecord(uint8_t* p, std::string_view name, uint32_t lba, uint32_t size, bool dir)
{
    const size_t len = (33 + name.size() + 1) & ~size_t(1);
    std::memset(p, 0, len);
    p[0] = uint8_t(len);
    put32(p + 2, lba);
    put32(p + 10, size);
    p[25] = dir ? 2 : 0;
    p[32] = uint8_t(name.size());
    std::memcpy(p + 33, name.data(), name.size());
    return len;
}

struct FileSpec {
    std::string name;  // as stored on disc, e.g. "hello.txt;1"
    std::string body;
};

// Root directory at sector 18 (spans `dirSectors` sectors; records are pushed into the
// second sector after zero padding when spread=true), file data from sector 30.
MemSource buildImage(const std::vector<FileSpec>& files, bool spread = false, uint32_t dirSectors = 1)
{
    MemSource m;
    uint32_t next = 30;
    std::vector<uint32_t> lbas;
    for (const auto& f : files) {
        lbas.push_back(next);
        next += uint32_t((f.body.size() + kSectorSize - 1) / kSectorSize);
    }
    m.data.assign(size_t(next + 4) * kSectorSize, 0);
    uint8_t* pvd = &m.data[16 * size_t(kSectorSize)];
    pvd[0] = 1;
    std::memcpy(pvd + 1, "CD001", 5);
    pvd[6] = 1;
    putRecord(pvd + 156, std::string(1, '\0'), 18, dirSectors * kSectorSize, true);
    uint8_t* dir = &m.data[18 * size_t(kSectorSize)];
    size_t pos = 0;
    pos += putRecord(dir + pos, std::string(1, '\0'), 18, dirSectors * kSectorSize, true);
    pos += putRecord(dir + pos, std::string(1, '\1'), 18, dirSectors * kSectorSize, true);
    pos += putRecord(dir + pos, "SUBDIR", 25, kSectorSize, true);
    for (size_t i = 0; i < files.size(); ++i) {
        if (spread && i == files.size() / 2) {
            pos = kSectorSize;  // remaining records live in the next sector; first one is zero padding
        }
        pos += putRecord(dir + pos, files[i].name, lbas[i], uint32_t(files[i].body.size()), false);
    }
    for (size_t i = 0; i < files.size(); ++i)
        std::memcpy(&m.data[size_t(lbas[i]) * kSectorSize], files[i].body.data(), files[i].body.size());
    return m;
}

std::string sha(const std::string& s)
{
    Sha256 h;
    h.update(s.data(), s.size());
    return h.finishHex();
}

// A tiny profile over synthetic files, hashes computed here.
struct TestDisc {
    std::vector<FileSpec> specs;
    std::vector<std::string> names, hashes;
    std::vector<ExpectedFile> expected;
    Profile profile{};
    TestDisc()
    {
        specs = {{"SYSTEM.CNF;1", "BOOT2 = cdrom0:\\SLUS_200.02;1\r\nVER = 1.00\r\n"},
                 {"SLUS_200.02;1", std::string(5000, 'x')},
                 {"BIG.BIN;1", std::string(kSectorSize * 700 + 17, 'y')}};
        for (auto& s : specs) {
            names.push_back(s.name.substr(0, s.name.find(';')));
            hashes.push_back(sha(s.body));
        }
        for (size_t i = 0; i < specs.size(); ++i)
            expected.push_back({names[i], specs[i].body.size(), hashes[i]});
        profile = {expected, "cdrom0:\\slus_200.02;1"};
    }
};

fs::path scratchDir(const char* tag)
{
    fs::path p = fs::temp_directory_path() / ("rrv-disc-extract-test-" + std::to_string(::getpid()) + "-" + tag);
    fs::remove_all(p);
    return p;
}

size_t countFiles(const fs::path& d)
{
    size_t n = 0;
    std::error_code ec;
    for (auto it = fs::directory_iterator(d, ec); !ec && it != fs::directory_iterator(); ++it)
        ++n;
    return n;
}

std::string slurp(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}

// ---- tests -------------------------------------------------------------------
void testSha256()
{
    CHECK(sha("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    Sha256 h;
    h.update("a", 1);
    h.update("bc", 2);
    CHECK(h.finishHex() == sha("abc"));
}

void testNames()
{
    CHECK(namesMatch("SLUS_200.02;1", "SLUS_200.02"));
    CHECK(namesMatch("slus_200.02;1", "SLUS_200.02"));
    CHECK(namesMatch("R5.ALL", "R5.ALL"));
    CHECK(namesMatch("r5.all;12", "R5.ALL"));
    CHECK(!namesMatch("R5.ALL.BAK;1", "R5.ALL"));
    CHECK(!namesMatch("R5.AL;1", "R5.ALL"));
    CHECK(!namesMatch("R5.ALL;x", "R5.ALL"));
    CHECK(!namesMatch("", "R5.ALL"));
    CHECK(namesMatch("IOPRP15.IMG;1", "ioprp15.img"));
}

void testRootParser()
{
    TestDisc d;
    MemSource m = buildImage(d.specs);
    std::vector<IsoEntry> e;
    CHECK(parseRootDirectory(m, e).status == Status::ok);
    CHECK(e.size() == 4);  // SUBDIR + 3 files; "." and ".." skipped
    CHECK(e.size() == 4 && e[0].name == "SUBDIR" && e[0].isDir);
    CHECK(e.size() == 4 && e[3].name == "BIG.BIN;1" && !e[3].isDir && e[3].size == d.specs[2].body.size());

    // Directory spanning two sectors with zero padding at the end of the first.
    MemSource m2 = buildImage(d.specs, true, 2);
    CHECK(parseRootDirectory(m2, e).status == Status::ok);
    CHECK(e.size() == 4);
    CHECK(e.size() == 4 && e[3].name == "BIG.BIN;1");

    // No volume descriptor.
    MemSource blank;
    blank.data.assign(40 * size_t(kSectorSize), 0);
    CHECK(parseRootDirectory(blank, e).status == Status::notIso9660);
    MemSource tiny;
    tiny.data.assign(4 * size_t(kSectorSize), 0);
    CHECK(parseRootDirectory(tiny, e).status == Status::notIso9660);

    // Unreadable root sector, and a record that overruns the directory.
    MemSource bad = buildImage(d.specs);
    bad.failAt = 18;
    CHECK(parseRootDirectory(bad, e).status == Status::damaged);
    MemSource corrupt = buildImage(d.specs);
    corrupt.data[18 * size_t(kSectorSize)] = 34;  // length shorter than name length field allows
    corrupt.data[18 * size_t(kSectorSize) + 32] = 200;
    CHECK(parseRootDirectory(corrupt, e).status == Status::damaged);
}

void testProbe()
{
    TestDisc d;
    MemSource m = buildImage(d.specs);
    CHECK(probeSource(m, d.profile).status == Status::ok);

    auto specs = d.specs;
    specs.pop_back();  // BIG.BIN missing
    MemSource missing = buildImage(specs);
    CHECK(probeSource(missing, d.profile).status == Status::wrongDisc);

    specs = d.specs;
    specs[2].body.push_back('z');  // wrong size
    MemSource wrongSize = buildImage(specs);
    CHECK(probeSource(wrongSize, d.profile).status == Status::wrongDisc);

    specs = d.specs;
    specs[0].body = "BOOT2 = cdrom0:\\SLES_500.01;1\r\n" + std::string(d.specs[0].body.size() - 33, ' ');
    specs[0].body.resize(d.specs[0].body.size());
    MemSource wrongBoot = buildImage(specs);
    CHECK(probeSource(wrongBoot, d.profile).status == Status::wrongDisc);

    MemSource truncated = buildImage(d.specs);
    truncated.data.resize(truncated.data.size() - 4 * size_t(kSectorSize) - 100 * size_t(kSectorSize));
    CHECK(probeSource(truncated, d.profile).status == Status::damaged);
}

void testExtract()
{
    TestDisc d;
    const fs::path out = scratchDir("ok") / "nested" / "disc";
    MemSource m = buildImage(d.specs);
    uint64_t lastDone = 0, calls = 0;
    auto r = extractSource(m, d.profile, out, [&](const Progress& p) {
        CHECK(p.bytesDone >= lastDone && p.bytesDone <= p.bytesTotal && !p.currentFile.empty());
        lastDone = p.bytesDone;
        ++calls;
        return true;
    });
    CHECK(r.status == Status::ok);
    CHECK(calls > 3);
    CHECK(lastDone == d.specs[0].body.size() + d.specs[1].body.size() + d.specs[2].body.size());
    CHECK(countFiles(out) == 3);
    for (size_t i = 0; i < d.specs.size(); ++i)
        CHECK(slurp(out / d.names[i]) == d.specs[i].body);

    // Null progress, existing files replaced.
    CHECK(extractSource(m, d.profile, out, nullptr).status == Status::ok);
    CHECK(countFiles(out) == 3);
    fs::remove_all(out.parent_path().parent_path());

    // Content differs from the expected hash: damaged, nothing published, no temp left.
    const fs::path out2 = scratchDir("bad");
    auto specs = d.specs;
    specs[2].body[12345] = 'Q';
    MemSource bad = buildImage(specs);
    r = extractSource(bad, d.profile, out2, nullptr);
    CHECK(r.status == Status::damaged);
    CHECK(countFiles(out2) == 0);
    fs::remove_all(out2);

    // Read error mid-file.
    const fs::path out3 = scratchDir("readerr");
    MemSource rd = buildImage(d.specs);
    rd.failAt = 30 + 2 + 10;
    r = extractSource(rd, d.profile, out3, nullptr);
    CHECK(r.status == Status::damaged);
    CHECK(countFiles(out3) == 0);
    fs::remove_all(out3);

    // Cancel part way through the big file.
    const fs::path out4 = scratchDir("cancel");
    int n = 0;
    r = extractSource(m, d.profile, out4, [&](const Progress&) { return ++n < 6; });
    CHECK(r.status == Status::cancelled);
    CHECK(countFiles(out4) == 0);
    fs::remove_all(out4);

    // Wrong disc creates nothing.
    const fs::path out5 = scratchDir("wrong");
    MemSource blank;
    blank.data.assign(40 * size_t(kSectorSize), 0);
    CHECK(extractSource(blank, d.profile, out5, nullptr).status == Status::notIso9660);
    CHECK(!fs::exists(out5));

    // Destination that cannot be a directory.
    const fs::path file = scratchDir("file");
    std::ofstream(file) << "x";
    CHECK(extractSource(m, d.profile, file / "sub", nullptr).status == Status::writeFailed);
    fs::remove(file);
}

void testPublicApi()
{
    CHECK(requiredBytes() == 82741ull + 69589 + 5829 + 39629 + 481452032ull + 136157 + 8317 + 1295332 + 56);

    const fs::path dir = scratchDir("api");
    fs::create_directories(dir);
    CHECK(probeRr5Usa(dir / "missing.chd").status == Status::cannotOpen);
    CHECK(extractRr5Usa(dir / "missing.chd", dir / "out", nullptr).status == Status::cannotOpen);

    { std::ofstream(dir / "text.chd") << "this is definitely not a compressed hunks of data file"; }
    CHECK(probeRr5Usa(dir / "text.chd").status == Status::notChd);
    { std::ofstream(dir / "empty.chd"); }
    CHECK(probeRr5Usa(dir / "empty.chd").status == Status::notChd);
    CHECK(extractRr5Usa(dir / "text.chd", dir / "out", nullptr).status == Status::notChd);
    CHECK(!fs::exists(dir / "out"));

    // CHD magic followed by garbage: must fail cleanly.
    { std::ofstream f(dir / "fake.chd", std::ios::binary); f << "MComprHD" << std::string(4000, '\x7f'); }
    const Status s = probeRr5Usa(dir / "fake.chd").status;
    CHECK(s == Status::damaged || s == Status::notChd);
    CHECK(probeRr5Usa(dir).status != Status::ok);  // a directory
    fs::remove_all(dir);
}
}  // namespace

int main()
{
    testSha256();
    testNames();
    testRootParser();
    testProbe();
    testExtract();
    testPublicApi();
    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::puts("disc_extract_tests: all passed");
    return 0;
}
