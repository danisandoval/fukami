// Fukami for Linux / Steam Deck (Gate 5): the Linux counterpart of
// fukami_app.mm. A startup hook that runs before main(): when this image is
// the app's bin/Fukami (layout below) and is not yet the game process, it
// prepares the user's folders (first-launch CHD setup, the settings file, the
// save card, a session folder), then re-executes the same image with the
// environment scripts/run_gate4_product.sh would give it. Outside the app
// layout, and in the re-executed game process, the hook does nothing.
//
// App layout (tools/linux-build packaging, any location, may contain spaces):
//   Fukami/Fukami                 launcher script (LD_LIBRARY_PATH=lib, exec bin/Fukami)
//   Fukami/bin/Fukami             this image
//   Fukami/lib/                   SDL2 + bridge closure
//   Fukami/share/fukami-default.ini, share/workload/bound-workload.txt,
//   Fukami/share/game/            fixed resource package (rrv_resource_package.cpp)
// User data: ${XDG_DATA_HOME:-~/.local/share}/Fukami (fukami.ini, mc/, disc/,
// sessions/). The CHD comes from the first .chd argument, FUKAMI_CHD, a .chd
// file placed in the data folder, or a zenity/kdialog file picker if present.
#include "fukami_app.h"
#include "fukami_settings.h"
#include "disc_extract.h"

#include <spawn.h>
#include <sys/wait.h>

#include <atomic>
#include <cctype>
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <optional>
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
    char resolved[PATH_MAX];
    if (!realpath("/proc/self/exe", resolved))
        return {};
    return resolved;
}

// .../Fukami when the image is Fukami/bin/Fukami next to share/fukami-default.ini.
fs::path appRoot(const fs::path &exe)
{
    const fs::path bin = exe.parent_path();
    if (exe.filename() != "Fukami" || bin.filename() != "bin")
        return {};
    std::error_code ec;
    if (!fs::is_regular_file(bin.parent_path() / "share" / "fukami-default.ini", ec))
        return {};
    return bin.parent_path();
}

std::string envOr(const char *name, const char *fallback)
{
    const char *value = std::getenv(name);
    return value && *value ? value : fallback;
}

fs::path supportDirectory()
{
    if (const char *xdg = std::getenv("XDG_DATA_HOME"); xdg && xdg[0] == '/')
        return fs::path(xdg) / "Fukami";
    return fs::path(envOr("HOME", "/tmp")) / ".local/share/Fukami";
}

// ---- dialogs (zenity or kdialog when present; always stderr) ---------------

fs::path findTool(const char *name)
{
    const std::string path = envOr("PATH", "/usr/bin:/bin");
    size_t begin = 0;
    while (begin <= path.size())
    {
        const size_t end = path.find(':', begin);
        const std::string dir = path.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        if (!dir.empty())
        {
            const fs::path candidate = fs::path(dir) / name;
            if (access(candidate.c_str(), X_OK) == 0)
                return candidate;
        }
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    return {};
}

bool haveDisplay()
{
    const char *x = std::getenv("DISPLAY");
    const char *w = std::getenv("WAYLAND_DISPLAY");
    return (x && *x) || (w && *w);
}

// Runs a tool; returns its exit status (-1 on failure) and its stdout.
int runTool(const std::vector<std::string> &args, std::string *output = nullptr)
{
    int pipeFds[2] = {-1, -1};
    if (output && pipe(pipeFds) != 0)
        return -1;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (output)
    {
        posix_spawn_file_actions_adddup2(&actions, pipeFds[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&actions, pipeFds[0]);
    }
    std::vector<char *> argv;
    std::vector<std::string> copy = args;
    for (auto &arg : copy)
        argv.push_back(arg.data());
    argv.push_back(nullptr);
    pid_t pid = 0;
    const int spawned = posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (output)
        close(pipeFds[1]);
    if (spawned != 0)
    {
        if (output)
            close(pipeFds[0]);
        return -1;
    }
    if (output)
    {
        char buffer[4096];
        ssize_t n;
        while ((n = read(pipeFds[0], buffer, sizeof buffer)) > 0)
            output->append(buffer, static_cast<size_t>(n));
        close(pipeFds[0]);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

void message(const std::string &title, const std::string &text, bool error)
{
    std::fprintf(stderr, "[fukami] %s: %s\n", title.c_str(), text.c_str());
    if (!haveDisplay())
        return;
    if (const fs::path zenity = findTool("zenity"); !zenity.empty())
        runTool({zenity.string(), error ? "--error" : "--info", "--title=" + title, "--no-markup", "--text=" + text});
    else if (const fs::path kdialog = findTool("kdialog"); !kdialog.empty())
        runTool({kdialog.string(), "--title", title, error ? "--error" : "--msgbox", text});
}

// Asks for the CHD with a file picker; empty when none is available/cancelled.
fs::path pickChd()
{
    if (!haveDisplay())
        return {};
    std::string out;
    int status = -1;
    if (const fs::path zenity = findTool("zenity"); !zenity.empty())
        status = runTool({zenity.string(), "--file-selection", "--title=Choose your Ridge Racer V (USA) disc image",
                          "--file-filter=Disc images | *.chd *.CHD *.cue *.CUE *.bin *.BIN"},
                         &out);
    else if (const fs::path kdialog = findTool("kdialog"); !kdialog.empty())
        status = runTool({kdialog.string(), "--title", "Choose your Ridge Racer V (USA) disc image", "--getopenfilename",
                          envOr("HOME", "/"), "*.chd *.CHD *.cue *.CUE *.bin *.BIN"},
                         &out);
    if (status != 0)
        return {};
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    return out;
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

bool isChd(const fs::path &path)
{
    std::string ext = path.extension().string();
    for (auto &c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::error_code ec;
    return (ext == ".chd" || ext == ".cue" || ext == ".bin") && fs::is_regular_file(path, ec);
}

// Unpacks with a text progress line on stderr and, when zenity is present, a
// progress window (closing it cancels).
fukami::disc::Result extractWithProgress(const fs::path &chd, const fs::path &disc)
{
    FILE *zenity = nullptr;
    if (haveDisplay())
        if (const fs::path tool = findTool("zenity"); !tool.empty())
        {
            const std::string command = "'" + tool.string() +
                                        "' --progress --title=Fukami --text='Unpacking your game files...' "
                                        "--percentage=0 --auto-close 2>/dev/null";
            zenity = popen(command.c_str(), "w");
        }
    const auto previousPipe = std::signal(SIGPIPE, SIG_IGN);
    int lastPercent = -1;
    bool cancelled = false;
    auto result = fukami::disc::extractRr5Usa(chd, disc, [&](const fukami::disc::Progress &p) {
        const uint64_t total = p.bytesTotal ? p.bytesTotal : 1;
        const int percent = static_cast<int>(p.bytesDone * 100u / total);
        if (percent != lastPercent)
        {
            lastPercent = percent;
            std::fprintf(stderr, "\r[fukami] unpacking game files: %3d%%", percent);
            if (zenity && !cancelled)
            {
                // zenity closes on 100 (auto-close); keep it open until done.
                if (std::fprintf(zenity, "%d\n", percent < 100 ? percent : 99) < 0 || std::fflush(zenity) != 0)
                    cancelled = true; // the user closed the window
            }
        }
        return !cancelled;
    });
    std::fprintf(stderr, "\n");
    if (zenity)
    {
        std::fprintf(zenity, "100\n");
        pclose(zenity);
    }
    std::signal(SIGPIPE, previousPipe);
    return result;
}

// First launch: find the CHD until the disc files are in place. Returns false
// when there is no usable CHD.
bool setUpDisc(const fs::path &support, const fs::path &disc, const std::vector<fs::path> &argumentChds)
{
    std::vector<fs::path> candidates = argumentChds;
    if (const char *env = std::getenv("FUKAMI_CHD"); env && *env)
        candidates.emplace_back(env);
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(support, ec))
        if (isChd(entry.path()))
            candidates.push_back(entry.path());
    const std::string intro =
        "Fukami needs your own copy of Ridge Racer V (USA, SLUS-20002) as a CHD file, or a .cue/.bin pair. It unpacks the game files "
        "once (about " +
        std::to_string((fukami::disc::requiredBytes() + 999999) / 1000000) + " MB) into " + support.string() +
        ". You can delete the disc image afterwards.";
    bool picked = false;
    for (size_t index = 0;; ++index)
    {
        fs::path chd;
        if (index < candidates.size())
            chd = candidates[index];
        else
        {
            if (!picked)
                message("Welcome to Fukami", intro, false);
            picked = true;
            chd = pickChd();
            if (chd.empty())
            {
                message("Fukami",
                        "No disc image was chosen. Start Fukami with the CHD or .cue path as its argument, set FUKAMI_CHD, or "
                        "put the .chd (or .cue and .bin) file into " +
                            support.string() + ".",
                        true);
                return false;
            }
        }
        std::fprintf(stderr, "[fukami] trying CHD %s\n", chd.c_str());
        const auto probe = fukami::disc::probeRr5Usa(chd);
        if (probe.status != fukami::disc::Status::ok)
        {
            message("This disc image can't be used", chd.string() + ": " + probe.message, true);
            continue;
        }
        const auto result = extractWithProgress(chd, disc);
        if (result.status == fukami::disc::Status::ok)
        {
            writeMarker(disc);
            return true;
        }
        if (result.status == fukami::disc::Status::cancelled)
            return false;
        message("The game files could not be unpacked", result.message, true);
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

[[noreturn]] void quit(int code)
{
    std::fflush(nullptr);
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
    auto doc = fukami::settings::IniDocument::load(ini);
    if (!doc)
    {
        message("Fukami", "The settings file can't be read: " + ini.string(), true);
        return std::nullopt;
    }
    const auto issues = fukami::settings::check(*doc, false);
    if (issues.empty())
        return effectiveSettings(std::move(*doc), defaults);
    // No modal question here (there may be no display, e.g. Steam Game Mode):
    // keep the bad file as .bad and continue with the defaults, and say so.
    const auto &issue = issues.front();
    const std::string where = issue.line > 0 ? "line " + std::to_string(issue.line) + ": " : "";
    fs::path bad = ini;
    bad += ".bad";
    fs::remove(bad, ec);
    fs::rename(ini, bad, ec);
    fs::copy_file(defaults, ini, ec);
    message("The settings file had a problem",
            ini.filename().string() + " " + where + issue.message + ". It was reset to the defaults; the old file is " +
                bad.string() + ".",
            true);
    doc = fukami::settings::IniDocument::load(ini);
    if (!doc || !fukami::settings::check(*doc, false).empty())
        return std::nullopt;
    return effectiveSettings(std::move(*doc), defaults);
}

void launchFromApp(const fs::path &exe, const fs::path &root, const std::vector<fs::path> &argumentChds)
{
    const fs::path resources = root / "share";
    const fs::path support = supportDirectory();
    const fs::path disc = support / "disc";
    const fs::path ini = support / "fukami.ini";
    const fs::path mc = support / "mc";
    std::error_code ec;
    fs::create_directories(support, ec);
    // PCSX2 (in the GS bridge) keeps its data under $XDG_CONFIG_HOME/PCSX2 and
    // its setup fails when that parent is missing (fresh Steam Deck accounts).
    if (const char *config = std::getenv("XDG_CONFIG_HOME"); config && config[0] == '/')
        fs::create_directories(config, ec);
    else
        fs::create_directories(fs::path(envOr("HOME", "/tmp")) / ".config", ec);

    if (!discReady(disc) && !setUpDisc(support, disc, argumentChds))
        quit(1);

    auto doc = loadSettings(ini, resources / "fukami-default.ini");
    if (!doc)
        quit(1);

    // Logging off (the default): no session folder, no file. The game's output goes to /dev/null.
    const bool logging = fukami::settings::LaunchConfig::fromIni(*doc, false).logging;
    fs::path session;
    if (logging)
    {
        session = support / "sessions" / (timestamp() + "-" + std::to_string(getpid()));
        fs::create_directories(session, ec);
    }
    fs::create_directories(mc, ec);
    if (logging)
        // The card as it was at launch, so this session can become a replay later
        // (same as the developer launcher).
        fs::copy(mc, session / "mc-start", fs::copy_options::recursive, ec);

    fukami::settings::LaunchPaths paths;
    paths.boundWorkload = resources / "workload" / "bound-workload.txt";
    paths.memoryCard = mc;
    paths.session = session;
    paths.libraries = root / "lib";
    paths.home = envOr("HOME", "/");
    paths.path = envOr("PATH", "/usr/local/bin:/usr/bin:/bin");
    paths.tmpdir = envOr("TMPDIR", "/tmp");
    std::vector<std::string> env;
    for (auto &entry : fukami::settings::environment(*doc, paths))
        if (entry.rfind("DYLD_LIBRARY_PATH=", 0) != 0) // macOS-only name
            env.push_back(std::move(entry));
    // The Linux loader path: the app's lib/ first, then what the launcher had.
    std::string libraries = paths.libraries.string();
    if (const char *inherited = std::getenv("LD_LIBRARY_PATH"); inherited && *inherited &&
                                                              std::string(inherited) != libraries)
        libraries += std::string(":") + inherited;
    env.push_back("LD_LIBRARY_PATH=" + libraries);
    // env -i drops the desktop session; the game still needs its display,
    // audio, controller and Steam/gamescope variables. Pass those through.
    for (const char *name : {"DISPLAY", "XAUTHORITY", "WAYLAND_DISPLAY", "XDG_RUNTIME_DIR", "XDG_SESSION_TYPE",
                             "XDG_DATA_HOME", "XDG_CONFIG_HOME", "DBUS_SESSION_BUS_ADDRESS", "PULSE_SERVER",
                             "PIPEWIRE_RUNTIME_DIR", "SDL_VIDEODRIVER", "SDL_AUDIODRIVER",
                             "SDL_GAMECONTROLLERCONFIG", "SDL_GAMECONTROLLER_IGNORE_DEVICES",
                             "SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD", "SteamDeck", "SteamAppId",
                             "SteamGameId", "STEAM_COMPAT_CLIENT_INSTALL_PATH", "GAMESCOPE_WAYLAND_DISPLAY",
                             "ENABLE_GAMESCOPE_WSI", "LANG", "LC_ALL", "VK_ICD_FILENAMES",
                             "VK_DRIVER_FILES", "VK_INSTANCE_LAYERS", "MESA_VK_WSI_PRESENT_MODE"})
        if (const char *value = std::getenv(name); value && *value)
            env.push_back(std::string(name) + "=" + value);
    env.push_back(std::string(kChild) + "=1");
    env.push_back(std::string(kIni) + "=" + ini.string());
    env.push_back(std::string(kSupport) + "=" + support.string());

    // env(1) semantics: a later NAME wins. Keep only the last occurrence of
    // each name, in first-seen order.
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

    if (logging)
        std::fprintf(stderr, "[fukami] starting the game; logs in %s\n", session.c_str());
    std::fflush(nullptr);
    // The runtime's diagnostic output goes to the session logs, as with ./run.sh.
    const int out = logging ? open((session / "stdout.log").c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)
                            : open("/dev/null", O_WRONLY | O_CLOEXEC);
    const int err = logging ? open((session / "stderr.log").c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)
                            : open("/dev/null", O_WRONLY | O_CLOEXEC);
    const int savedErr = dup(STDERR_FILENO);
    if (out >= 0)
        dup2(out, STDOUT_FILENO);
    if (err >= 0)
        dup2(err, STDERR_FILENO);
    execve(exePath.c_str(), argv, envp.data());
    const std::string reason = std::strerror(errno);
    if (savedErr >= 0)
        dup2(savedErr, STDERR_FILENO);
    message("Fukami could not start the game", reason, true);
    quit(1);
}

// glibc passes (argc, argv, envp) to ELF constructors.
__attribute__((constructor)) void fukamiAppEntry(int argc, char **argv, char **)
{
    if (std::getenv(kChild))
        return;
    // `--print-launch-env` / `--print-launch-config` are command-line queries, never an app launch
    // (the same rule as the macOS hook in fukami_app.mm).
    if (argv && argc >= 2 && argv[1] && std::strncmp(argv[1], "--print-launch-", 15) == 0)
        return;
    const fs::path exe = executablePath();
    const fs::path root = exe.empty() ? fs::path{} : appRoot(exe);
    if (root.empty())
        return;
    std::vector<fs::path> chds;
    for (int i = 1; argv && i < argc; ++i)
        if (argv[i] && isChd(argv[i]))
            chds.emplace_back(argv[i]);
    launchFromApp(exe, root, chds);
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
    const fs::path exe = executablePath();
    const fs::path root = exe.empty() ? fs::path{} : appRoot(exe);
    return root.empty() ? fs::path{} : root / "share" / "fukami-default.ini";
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
    // xdg-open cannot select a file; open the folder that holds it.
    std::error_code ec;
    std::string target = (fs::is_directory(path, ec) ? path : path.parent_path()).string();
    char tool[] = "xdg-open";
    char *argv[] = {tool, target.data(), nullptr};
    pid_t pid = 0;
    if (posix_spawnp(&pid, tool, nullptr, nullptr, argv, environ) == 0)
        std::thread([pid] { int status = 0; waitpid(pid, &status, 0); }).detach();
}

} // namespace fukami::app
