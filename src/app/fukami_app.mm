// Fukami.app mode (Gate-9 APP4/APP6). A startup hook that runs before main():
// when this image is the main executable of a .app bundle and is not yet the
// game process, it prepares the user's folders (first-launch CHD setup, the
// settings file, the save card, a session folder), then re-executes the same
// image with the environment scripts/run_gate4_product.sh would give it. The
// re-executed process keeps the app's PID and bundle identity (Game Mode, Dock).
// Outside a bundle, and in the re-executed game process, the hook does nothing.
#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "fukami_app.h"
#include "fukami_settings.h"
#include "disc_extract.h"

#include <crt_externs.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <sys/wait.h>

#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

extern char **environ;

namespace {

namespace fs = std::filesystem;
constexpr const char *kChild = "RRV_FUKAMI_CHILD";
constexpr const char *kIni = "RRV_FUKAMI_INI";
constexpr const char *kSupport = "RRV_FUKAMI_SUPPORT";
constexpr const char *kVerifiedMarker = ".fukami-disc-verified";

fs::path executablePath()
{
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> raw(size + 1);
    if (_NSGetExecutablePath(raw.data(), &size) != 0)
        return {};
    char resolved[PATH_MAX];
    if (!realpath(raw.data(), resolved))
        return {};
    return resolved;
}

// .../Fukami.app/Contents when the image is Contents/MacOS/<exe>, else empty.
fs::path bundleContents(const fs::path &exe)
{
    const fs::path macos = exe.parent_path();
    const fs::path contents = macos.parent_path();
    if (macos.filename() != "MacOS" || contents.filename() != "Contents" ||
        contents.parent_path().extension() != ".app")
        return {};
    return contents;
}

fs::path supportDirectory()
{
    @autoreleasepool {
        NSURL *url = [[NSFileManager defaultManager] URLForDirectory:NSApplicationSupportDirectory
                                                            inDomain:NSUserDomainMask
                                                   appropriateForURL:nil
                                                              create:YES
                                                               error:nil];
        if (url)
            return fs::path(url.fileSystemRepresentation) / "Fukami";
        const char *home = std::getenv("HOME");
        return fs::path(home ? home : "/tmp") / "Library/Application Support/Fukami";
    }
}

NSString *ns(const std::string &text) { return [NSString stringWithUTF8String:text.c_str()]; }

void startUi()
{
    NSApplication *app = [NSApplication sharedApplication];
    [app setActivationPolicy:NSApplicationActivationPolicyRegular];
    [app activateIgnoringOtherApps:YES];
}

// Returns the index of the clicked button (0 = first).
int alert(const std::string &title, const std::string &text, std::vector<std::string> buttons,
          NSAlertStyle style = NSAlertStyleInformational)
{
    @autoreleasepool {
        startUi();
        NSAlert *box = [[NSAlert alloc] init];
        box.messageText = ns(title);
        box.informativeText = ns(text);
        box.alertStyle = style;
        for (const auto &button : buttons)
            [box addButtonWithTitle:ns(button)];
        const NSModalResponse response = [box runModal];
        return static_cast<int>(response - NSAlertFirstButtonReturn);
    }
}

// ---- disc setup ------------------------------------------------------------

bool discReady(const fs::path &disc)
{
    std::ifstream marker(disc / kVerifiedMarker);
    if (!marker)
        return false;
    std::string name;
    uintmax_t size = 0;
    int files = 0;
    while (marker >> name >> size)
    {
        std::error_code ec;
        if (fs::file_size(disc / name, ec) != size || ec)
            return false;
        ++files;
    }
    return files == 9;
}

void writeMarker(const fs::path &disc)
{
    std::ofstream marker(disc / kVerifiedMarker, std::ios::trunc);
    for (const auto &entry : fs::directory_iterator(disc))
        if (entry.is_regular_file() && entry.path().filename() != kVerifiedMarker &&
            entry.path().filename().string().front() != '.')
            marker << entry.path().filename().string() << ' ' << entry.file_size() << '\n';
}

} // namespace

@interface FukamiCancelTarget : NSObject
@property(atomic) BOOL cancelled;
- (void)cancel:(id)sender;
@end
@implementation FukamiCancelTarget
- (void)cancel:(id)sender { (void)sender; self.cancelled = YES; }
@end

namespace {

fukami::disc::Result extractWithProgress(const fs::path &chd, const fs::path &disc)
{
    @autoreleasepool {
        startUi();
        NSPanel *panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 440, 130)
                                                    styleMask:NSWindowStyleMaskTitled
                                                      backing:NSBackingStoreBuffered
                                                        defer:NO];
        panel.title = @"Fukami";
        NSTextField *label = [NSTextField labelWithString:@"Unpacking your game files…"];
        label.frame = NSMakeRect(20, 86, 400, 20);
        NSProgressIndicator *bar = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(20, 58, 400, 20)];
        bar.indeterminate = NO;
        bar.minValue = 0.0;
        bar.maxValue = 1.0;
        FukamiCancelTarget *target = [[FukamiCancelTarget alloc] init];
        NSButton *cancel = [NSButton buttonWithTitle:@"Cancel" target:target action:@selector(cancel:)];
        cancel.frame = NSMakeRect(330, 14, 90, 32);
        [panel.contentView addSubview:label];
        [panel.contentView addSubview:bar];
        [panel.contentView addSubview:cancel];
        [panel center];
        [panel makeKeyAndOrderFront:nil];

        std::atomic<uint64_t> done{0}, total{1};
        std::atomic<bool> finished{false};
        fukami::disc::Result result{fukami::disc::Status::cancelled, "Cancelled."};
        std::thread worker([&] {
            result = fukami::disc::extractRr5Usa(chd, disc, [&](const fukami::disc::Progress &p) {
                done = p.bytesDone;
                total = p.bytesTotal ? p.bytesTotal : 1;
                return !target.cancelled;
            });
            finished = true;
        });
        while (!finished)
        {
            NSEvent *event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                untilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]
                                                   inMode:NSDefaultRunLoopMode
                                                  dequeue:YES];
            if (event)
                [NSApp sendEvent:event];
            bar.doubleValue = static_cast<double>(done.load()) / static_cast<double>(total.load());
        }
        worker.join();
        [panel orderOut:nil];
        return result;
    }
}

// First launch: ask for the CHD until the disc files are in place. Returns
// false when the user quits.
bool setUpDisc(const fs::path &disc)
{
    for (;;)
    {
        const int choice = alert(
            "Welcome to Fukami",
            "Fukami needs your own copy of Ridge Racer V (USA, SLUS-20002) as a CHD file.\n\n"
            "It unpacks the game files once (about " +
                std::to_string((fukami::disc::requiredBytes() + 999999) / 1000000) +
                " MB) into your Application Support folder. You can delete the CHD afterwards.",
            {"Choose CHD…", "Quit"});
        if (choice != 0)
            return false;
        fs::path chd;
        @autoreleasepool {
            NSOpenPanel *open = [NSOpenPanel openPanel];
            open.title = @"Choose your Ridge Racer V (USA) CHD";
            open.canChooseFiles = YES;
            open.canChooseDirectories = NO;
            open.allowsMultipleSelection = NO;
            UTType *chdType = [UTType typeWithFilenameExtension:@"chd"];
            if (chdType)
                open.allowedContentTypes = @[ chdType ];
            if ([open runModal] != NSModalResponseOK || open.URLs.count == 0)
                continue;
            chd = open.URLs.firstObject.fileSystemRepresentation;
        }
        const auto probe = fukami::disc::probeRr5Usa(chd);
        if (probe.status != fukami::disc::Status::ok)
        {
            alert("This CHD can't be used", probe.message, {"OK"}, NSAlertStyleWarning);
            continue;
        }
        const auto result = extractWithProgress(chd, disc);
        if (result.status == fukami::disc::Status::ok)
        {
            writeMarker(disc);
            return true;
        }
        if (result.status != fukami::disc::Status::cancelled)
            alert("The game files could not be unpacked", result.message, {"OK"}, NSAlertStyleWarning);
    }
}

// ---- launch ------------------------------------------------------------------

std::string timestamp()
{
    char buffer[32];
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_r(&now, &utc);
    std::strftime(buffer, sizeof buffer, "%Y%m%dT%H%M%SZ", &utc);
    return buffer;
}

std::string envOr(const char *name, const char *fallback)
{
    const char *value = std::getenv(name);
    return value && *value ? value : fallback;
}

[[noreturn]] void quit(int code)
{
    std::_Exit(code);
}

// The settings to launch with: the user's file, and for every setting it does
// not have (a file written by an older version) the app's default ini, not the
// schema default. The user's file is not rewritten for that.
fukami::settings::IniDocument effectiveSettings(fukami::settings::IniDocument doc, const fs::path &defaults)
{
    const auto packaged = fukami::settings::IniDocument::load(defaults);
    if (!packaged)
        return doc;
    for (const auto &entry : fukami::settings::inherited(doc, *packaged))
        std::fprintf(stderr, "[fukami] settings: %s.%s = %s (app default; not in the settings file)\n",
                     entry.section.c_str(), entry.key.c_str(), entry.value.c_str());
    return fukami::settings::withDefaults(std::move(doc), *packaged);
}

std::optional<fukami::settings::IniDocument> loadSettings(const fs::path &ini, const fs::path &defaults)
{
    std::error_code ec;
    if (!fs::exists(ini, ec))
    {
        fs::create_directories(ini.parent_path(), ec);
        fs::copy_file(defaults, ini, ec);
    }
    for (;;)
    {
        auto doc = fukami::settings::IniDocument::load(ini);
        if (!doc)
        {
            alert("Fukami", "The settings file can't be read:\n" + ini.string(), {"Quit"}, NSAlertStyleCritical);
            return std::nullopt;
        }
        const auto issues = fukami::settings::check(*doc, false);
        if (issues.empty())
            return effectiveSettings(std::move(*doc), defaults);
        const auto &issue = issues.front();
        const std::string where = issue.line > 0 ? "line " + std::to_string(issue.line) + ": " : "";
        const int choice = alert("The settings file has a problem",
                                 ini.filename().string() + " " + where + issue.message +
                                     "\n\nReset it to the defaults? The old file is kept next to it as a .bad copy.",
                                 {"Reset to defaults", "Quit"}, NSAlertStyleWarning);
        if (choice != 0)
            return std::nullopt;
        fs::path bad = ini;
        bad += ".bad";
        fs::remove(bad, ec);
        fs::rename(ini, bad, ec);
        fs::copy_file(defaults, ini, ec);
    }
}

void launchFromBundle(const fs::path &exe, const fs::path &contents)
{
    const fs::path resources = contents / "Resources";
    const fs::path support = supportDirectory();
    const fs::path disc = support / "disc";
    const fs::path ini = support / "fukami.ini";
    const fs::path mc = support / "mc";
    std::error_code ec;
    fs::create_directories(support, ec);

    if (!discReady(disc) && !setUpDisc(disc))
        quit(0);

    auto doc = loadSettings(ini, resources / "fukami-default.ini");
    if (!doc)
        quit(1);

    const fs::path session = support / "sessions" / (timestamp() + "-" + std::to_string(getpid()));
    fs::create_directories(session, ec);
    fs::create_directories(mc, ec);
    // The card as it was at launch, so this session can become a replay later
    // (same as the developer launcher).
    fs::copy(mc, session / "mc-start", fs::copy_options::recursive, ec);

    fukami::settings::LaunchPaths paths;
    paths.boundWorkload = resources / "workload" / "bound-workload.txt";
    paths.memoryCard = mc;
    paths.session = session;
    paths.libraries = contents / "Frameworks";
    paths.home = envOr("HOME", "/");
    paths.path = envOr("PATH", "/usr/bin:/bin:/usr/sbin:/sbin");
    paths.tmpdir = envOr("TMPDIR", "/tmp");
    std::vector<std::string> env = fukami::settings::environment(*doc, paths);
    env.push_back(std::string(kChild) + "=1");
    env.push_back(std::string(kIni) + "=" + ini.string());
    env.push_back(std::string(kSupport) + "=" + support.string());

    // env(1) semantics: a later NAME wins. execve's getenv() takes the first,
    // so keep only the last occurrence of each name, in first-seen order.
    std::map<std::string, std::string> last;
    std::vector<std::string> order;
    for (const auto &entry : env)
    {
        const std::string name = entry.substr(0, entry.find('='));
        if (!last.count(name))
            order.push_back(name);
        last[name] = entry;
    }
    std::vector<std::string> finalEnv;
    for (const auto &name : order)
        finalEnv.push_back(last[name]);
    std::vector<char *> envp;
    for (auto &entry : finalEnv)
        envp.push_back(entry.data());
    envp.push_back(nullptr);

    std::string exePath = exe.string();
    std::string elf = (disc / "SLUS_200.02").string();
    char *argv[] = {exePath.data(), elf.data(), nullptr};

    // The runtime's diagnostic output goes to the session logs, as with ./run.sh.
    const int out = open((session / "stdout.log").c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    const int err = open((session / "stderr.log").c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (out >= 0)
        dup2(out, STDOUT_FILENO);
    if (err >= 0)
        dup2(err, STDERR_FILENO);
    execve(exePath.c_str(), argv, envp.data());
    const std::string reason = std::strerror(errno);
    alert("Fukami could not start the game", reason, {"Quit"}, NSAlertStyleCritical);
    quit(1);
}

__attribute__((constructor)) void fukamiAppEntry()
{
    if (std::getenv(kChild))
        return;
    // `--print-launch-env` / `--print-launch-config` are command-line queries, never an app launch.
    {
        const int argc = *_NSGetArgc();
        char **argv = *_NSGetArgv();
        if (argc >= 2 && std::strncmp(argv[1], "--print-launch-", 15) == 0)
            return;
    }
    const fs::path exe = executablePath();
    const fs::path contents = bundleContents(exe);
    if (contents.empty())
        return;
    launchFromBundle(exe, contents);
}

} // namespace

namespace fukami::app {

bool inApp() { return std::getenv(kChild) != nullptr; }

std::filesystem::path iniPath()
{
    const char *value = std::getenv(kIni);
    return value && *value ? std::filesystem::path(value) : std::filesystem::path{};
}

std::filesystem::path defaultIniPath()
{
    const fs::path contents = bundleContents(executablePath());
    if (contents.empty())
        return {};
    const fs::path defaults = contents / "Resources" / "fukami-default.ini";
    std::error_code ec;
    return fs::is_regular_file(defaults, ec) ? defaults : fs::path{};
}

bool canRestart() { return inApp(); }

void restart()
{
    if (!inApp())
        return;
    const fs::path exe = executablePath();
    if (exe.empty())
        return;
    // Drop the child marker so the startup hook reads the settings again.
    std::vector<std::string> env;
    for (char **entry = environ; *entry; ++entry)
        if (std::strncmp(*entry, kChild, std::strlen(kChild)) != 0 || (*entry)[std::strlen(kChild)] != '=')
            env.emplace_back(*entry);
    std::vector<char *> envp;
    for (auto &entry : env)
        envp.push_back(entry.data());
    envp.push_back(nullptr);
    std::string exePath = exe.string();
    char *argv[] = {exePath.data(), nullptr};
    execve(exePath.c_str(), argv, envp.data());
}

std::filesystem::path savesFolder()
{
    const char *value = std::getenv("RRV_GATE3_MC_ROOT");
    return value && *value ? std::filesystem::path(value) : std::filesystem::path{};
}

std::filesystem::path logsFolder()
{
    const char *value = std::getenv("RRV_GATE3_RECORD_PAD");
    return value && *value ? std::filesystem::path(value).parent_path() : std::filesystem::path{};
}

void reveal(const std::filesystem::path &path)
{
    if (path.empty())
        return;
    std::string target = path.string();
    char open[] = "/usr/bin/open";
    char flag[] = "-R";
    char *argv[] = {open, flag, target.data(), nullptr};
    pid_t pid = 0;
    if (posix_spawn(&pid, open, nullptr, nullptr, argv, environ) == 0)
    {
        // Reap it off the caller's thread; `open` returns at once.
        std::thread([pid] { int status = 0; waitpid(pid, &status, 0); }).detach();
    }
}

} // namespace fukami::app
