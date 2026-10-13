// Fukami for Windows: the Windows counterpart of fukami_app.mm and fukami_app_linux.cpp. Windows has no hook before
// main() that receives the command line, so the product's main() calls fukami::app::entry() first. When this image is
// the app's Fukami.exe (layout below) and is not yet the game process, entry() prepares the user's folders
// (first-launch CHD setup, the settings file, the save card, a session folder), starts the same image as a child
// process with the environment scripts/run_gate4_product.sh would give it, waits for it and exits with its status.
// Outside the app layout, and in the game process, entry() returns at once.
//
// Windows cannot replace a process image (no execve), so the launcher stays as the parent of the game:
//   - it holds the single-instance mutex for as long as the game runs (one process owns the save card);
//   - the game is in a job object that ends it if the launcher is ended;
//   - the menu's Restart ends the game with kRestartExit and the launcher starts it again with the settings file
//     read anew.
//
// App layout (any location, may contain spaces and non-ASCII characters):
//   Fukami\Fukami.exe             this image; SDL2.dll and the extractor's DLLs beside it
//   Fukami\resources\             fixed resource package (rrv_resource_package_win.cpp), with the GS bridge
//   Fukami\share\fukami-default.ini, share\workload\bound-workload.txt
// User data: %LOCALAPPDATA%\Fukami (fukami.ini, mc\, disc\, sessions\). The disc image (a .chd, or a .cue with its
// .bin) comes from an argument (a file dropped on Fukami.exe), FUKAMI_CHD, an image placed in the data folder, or the
// file picker. sessions\ is written only with `[game] logging = true`; otherwise the game's output goes to NUL.
// FUKAMI_DATA_DIR=<absolute folder> replaces the data folder for that launch (test runs that must not touch the
// user's settings and saves, as XDG_DATA_HOME does on Linux); it also gets its own single-instance mutex.
#include "fukami_app.h"
#include "fukami_app_common.h"
#include "fukami_settings.h"
#include "disc_extract.h"

#include <windows.h>

#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
using namespace fukami::app::detail; // kChild, discReady(), effectiveSettings(), ... (fukami_app_common.h)

// The game's exit status when the menu asks for a restart ('FKRS'); any other status ends the launcher.
constexpr DWORD kRestartExit = 0x464B5253;
constexpr const wchar_t *kInstanceMutex = L"Local\\Fukami.SingleInstance";

// ---- text ------------------------------------------------------------------
// Everything shared with the other platforms (settings, disc extraction, the environment list) is UTF-8; the Win32
// calls here are the wide ones, so nothing depends on the process code page.

std::wstring widen(std::string_view text)
{
    if (text.empty())
        return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(size > 0 ? size : 0), L'\0');
    if (size > 0)
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

std::string narrow(std::wstring_view text)
{
    if (text.empty())
        return {};
    const int size =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size > 0 ? size : 0), '\0');
    if (size > 0)
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::string text(const fs::path &path) { return narrow(path.native()); }

// The variable's value, or empty when it is not set.
std::wstring environmentValue(const wchar_t *name)
{
    std::wstring value(256, L'\0');
    for (;;)
    {
        SetLastError(ERROR_SUCCESS);
        const DWORD size = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
        if (size == 0)
            return {};
        if (size < value.size())
        {
            value.resize(size);
            return value;
        }
        value.resize(size);
    }
}

std::string systemMessage(DWORD code)
{
    wchar_t *buffer = nullptr;
    const DWORD size = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
        reinterpret_cast<wchar_t *>(&buffer), 0, nullptr);
    std::wstring message(buffer ? buffer : L"", size);
    if (buffer)
        LocalFree(buffer);
    while (!message.empty() && (message.back() == L'\n' || message.back() == L'\r' || message.back() == L' '))
        message.pop_back();
    return message.empty() ? "error " + std::to_string(code) : narrow(message);
}

// ---- folders ---------------------------------------------------------------

fs::path executablePath()
{
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;)
    {
        const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (size == 0)
            return {};
        if (size < buffer.size())
        {
            buffer.resize(size);
            return buffer;
        }
        buffer.resize(buffer.size() * 2);
    }
}

// ...\Fukami when the image is Fukami.exe next to share\fukami-default.ini, else empty.
fs::path appRoot(const fs::path &exe)
{
    if (exe.empty() || _wcsicmp(exe.filename().c_str(), L"Fukami.exe") != 0)
        return {};
    std::error_code ec;
    if (!fs::is_regular_file(exe.parent_path() / L"share" / L"fukami-default.ini", ec))
        return {};
    return exe.parent_path();
}

fs::path supportDirectory()
{
    if (const fs::path chosen = environmentValue(L"FUKAMI_DATA_DIR"); chosen.is_absolute())
        return chosen;
    wchar_t *local = nullptr;
    fs::path base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &local)) && local)
        base = local;
    if (local)
        CoTaskMemFree(local);
    if (base.empty())
        base = environmentValue(L"LOCALAPPDATA");
    if (base.empty())
        base = fs::path(environmentValue(L"USERPROFILE")) / L"AppData" / L"Local";
    return base / L"Fukami";
}

// ---- dialogs ---------------------------------------------------------------

// Shows a message box (and writes the same line to stderr); returns the button (IDOK, IDYES, ...).
int message(const std::string &title, const std::string &body, UINT buttons, UINT icon)
{
    std::fprintf(stderr, "[fukami] %s: %s\n", title.c_str(), body.c_str());
    return MessageBoxW(nullptr, widen(body).c_str(), widen(title).c_str(), buttons | icon | MB_SETFOREGROUND);
}

void error(const std::string &title, const std::string &body) { message(title, body, MB_OK, MB_ICONERROR); }

// Asks for the disc image with the system file picker; empty when cancelled.
fs::path pickChd()
{
    IFileOpenDialog *dialog = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        return {};
    fs::path chosen;
    const COMDLG_FILTERSPEC types[] = {{L"Disc images (*.chd, *.cue, *.bin)", L"*.chd;*.cue;*.bin"}};
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
    dialog->SetFileTypes(1, types);
    dialog->SetDefaultExtension(L"chd");
    dialog->SetTitle(L"Choose your Ridge Racer V (USA) disc image");
    IShellItem *item = nullptr;
    if (SUCCEEDED(dialog->Show(nullptr)) && SUCCEEDED(dialog->GetResult(&item)) && item)
    {
        wchar_t *name = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &name)) && name)
        {
            chosen = name;
            CoTaskMemFree(name);
        }
        item->Release();
    }
    dialog->Release();
    return chosen;
}

// ---- disc setup ------------------------------------------------------------

// Unpacks with the shell's progress dialog (it runs on its own thread; Cancel stops the extraction).
fukami::disc::Result extractWithProgress(const fs::path &chd, const fs::path &disc)
{
    IProgressDialog *dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ProgressDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
    {
        dialog->SetTitle(L"Fukami");
        dialog->SetLine(1, L"Unpacking your game files...", FALSE, nullptr);
        dialog->SetCancelMsg(L"Stopping...", nullptr);
        if (FAILED(dialog->StartProgressDialog(nullptr, nullptr, PROGDLG_NORMAL | PROGDLG_AUTOTIME | PROGDLG_NOMINIMIZE,
                                               nullptr)))
        {
            dialog->Release();
            dialog = nullptr;
        }
    }
    uint64_t lastStep = UINT64_MAX;
    auto result = fukami::disc::extractRr5Usa(chd, disc, [&](const fukami::disc::Progress &p) {
        if (!dialog)
            return true;
        const uint64_t total = p.bytesTotal ? p.bytesTotal : 1;
        if (const uint64_t step = p.bytesDone * 1000u / total; step != lastStep)
        {
            lastStep = step;
            dialog->SetProgress64(p.bytesDone, total);
        }
        return dialog->HasUserCancelled() == FALSE;
    });
    if (dialog)
    {
        dialog->StopProgressDialog();
        dialog->Release();
    }
    return result;
}

// First launch: find the CHD until the disc files are in place. Returns false when the user gives up.
bool setUpDisc(const fs::path &support, const fs::path &disc, const std::vector<fs::path> &argumentChds)
{
    std::vector<fs::path> candidates = argumentChds;
    if (const std::wstring named = environmentValue(L"FUKAMI_CHD"); !named.empty())
        candidates.emplace_back(named);
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(support, ec))
        if (isChd(entry.path()))
            candidates.push_back(entry.path());
    const std::string intro =
        "Fukami needs your own copy of Ridge Racer V (USA, SLUS-20002) as a CHD file, or a .cue/.bin pair.\n\nIt "
        "unpacks the game files "
        "once (about " +
        std::to_string((fukami::disc::requiredBytes() + 999999) / 1000000) + " MB) into " + text(support) +
        ". You can delete the disc image afterwards.\n\nChoose OK to select it.";
    for (size_t index = 0;; ++index)
    {
        fs::path chd;
        if (index < candidates.size())
            chd = candidates[index];
        else
        {
            if (message("Welcome to Fukami", intro, MB_OKCANCEL, MB_ICONINFORMATION) != IDOK)
                return false;
            chd = pickChd();
            if (chd.empty())
                continue; // the picker was cancelled: back to the welcome box, whose Cancel quits
        }
        std::fprintf(stderr, "[fukami] trying disc image %s\n", text(chd).c_str());
        const auto probe = fukami::disc::probeRr5Usa(chd);
        if (probe.status != fukami::disc::Status::ok)
        {
            error("This disc image can't be used", text(chd) + ":\n" + probe.message);
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
        error("The game files could not be unpacked", result.message);
    }
}

// ---- launch ------------------------------------------------------------------

[[noreturn]] void quit(int code)
{
    std::fflush(nullptr);
    ExitProcess(static_cast<UINT>(code));
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
            error("Fukami", "The settings file can't be read:\n" + text(ini));
            return std::nullopt;
        }
        const auto issues = fukami::settings::check(*doc, false);
        if (issues.empty())
            return effectiveSettings(std::move(*doc), defaults);
        const auto &issue = issues.front();
        const std::string where = issue.line > 0 ? "line " + std::to_string(issue.line) + ": " : "";
        if (message("The settings file has a problem",
                    text(ini.filename()) + " " + where + issue.message +
                        "\n\nReset it to the defaults? The old file is kept next to it as a .bad copy.",
                    MB_YESNO, MB_ICONWARNING) != IDYES)
            return std::nullopt;
        fs::path bad = ini;
        bad += L".bad";
        fs::remove(bad, ec);
        fs::rename(ini, bad, ec);
        fs::copy_file(defaults, ini, ec);
    }
}

// One argument of a CreateProcess command line, quoted the way CommandLineToArgvW and the C runtime read it back.
std::wstring quoted(const std::wstring &argument)
{
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (const wchar_t c : argument)
    {
        if (c == L'\\')
        {
            ++backslashes;
            continue;
        }
        // Backslashes are literal unless a quote follows them; then each one is doubled and the quote escaped.
        out.append(c == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
        backslashes = 0;
        out.push_back(c);
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

// A CreateProcess environment block: NAME=value strings sorted by name without case, each ended by a NUL, and a
// final NUL.
std::wstring environmentBlock(const std::vector<std::string> &env)
{
    std::vector<std::wstring> entries;
    for (const auto &entry : env)
        entries.push_back(widen(entry));
    const auto name = [](const std::wstring &entry) { return entry.substr(0, entry.find(L'=')); };
    std::stable_sort(entries.begin(), entries.end(), [&](const std::wstring &a, const std::wstring &b) {
        const std::wstring left = name(a), right = name(b);
        return CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()), right.c_str(),
                                    static_cast<int>(right.size()), TRUE) == CSTR_LESS_THAN;
    });
    std::wstring block;
    for (const auto &entry : entries)
    {
        block += entry;
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

// A file the child writes its stdout or stderr to; inheritable, and readable while the game runs.
HANDLE logFile(const fs::path &path)
{
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    return CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &inherit,
                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

// The environment the game process gets: what the settings give (the developer launcher's `env -i` list), the system
// variables a Windows process cannot do without, and the launcher's markers.
std::vector<std::string> gameEnvironment(const fukami::settings::IniDocument &doc, const fs::path &root,
                                         const fs::path &support, const fs::path &ini, const fs::path &session)
{
    fukami::settings::LaunchPaths paths;
    paths.boundWorkload = root / L"share" / L"workload" / L"bound-workload.txt";
    paths.memoryCard = support / L"mc";
    paths.session = session;
    paths.home = narrow(environmentValue(L"USERPROFILE"));
    paths.path = narrow(environmentValue(L"PATH"));
    std::error_code ec;
    const std::wstring temp = environmentValue(L"TEMP");
    paths.tmpdir = temp.empty() ? text(fs::temp_directory_path(ec)) : narrow(temp);
    std::vector<std::string> env = fukami::settings::environment(doc, paths);
    // `env -i` drops the session; Windows components (the loader, the Vulkan driver, audio, the shell folders PCSX2
    // asks for) need these, and the game its controller and Steam variables. Pass those through.
    for (const wchar_t *name :
         {L"SystemRoot", L"SystemDrive", L"windir", L"ComSpec", L"PATHEXT", L"OS", L"NUMBER_OF_PROCESSORS",
          L"PROCESSOR_ARCHITECTURE", L"PROCESSOR_IDENTIFIER", L"PROCESSOR_LEVEL", L"PROCESSOR_REVISION",
          L"COMPUTERNAME", L"USERNAME", L"USERDOMAIN", L"USERDOMAIN_ROAMINGPROFILE", L"LOGONSERVER", L"SESSIONNAME",
          L"HOMEDRIVE", L"HOMEPATH", L"APPDATA", L"LOCALAPPDATA", L"ProgramData", L"ALLUSERSPROFILE", L"PUBLIC",
          L"ProgramFiles", L"ProgramFiles(x86)", L"ProgramW6432", L"CommonProgramFiles", L"CommonProgramFiles(x86)",
          L"CommonProgramW6432", L"DriverData", L"SDL_VIDEODRIVER", L"SDL_AUDIODRIVER", L"SDL_GAMECONTROLLERCONFIG",
          L"SDL_GAMECONTROLLER_IGNORE_DEVICES", L"SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD", L"SteamAppId",
          L"SteamGameId", L"VK_ICD_FILENAMES", L"VK_DRIVER_FILES", L"VK_ADD_DRIVER_FILES", L"VK_INSTANCE_LAYERS",
          L"VK_LAYER_PATH", L"VK_LOADER_LAYERS_ENABLE", L"VK_LOADER_LAYERS_DISABLE"})
        if (const std::wstring value = environmentValue(name); !value.empty())
            env.push_back(narrow(name) + "=" + narrow(value));
    env.push_back(std::string(kChild) + "=1");
    env.push_back(std::string(kIni) + "=" + text(ini));
    env.push_back(std::string(kSupport) + "=" + text(support));
    // env(1) semantics: a later NAME wins; Windows names have no case.
    return lastWins(env, true);
}

// Starts the game and waits for it. Returns its exit status, or nullopt (after a message) when it could not start.
// `session` is empty when logging is off: the game's output then goes to NUL.
std::optional<DWORD> runGame(const fs::path &exe, const fs::path &elf, const std::vector<std::string> &env,
                             const fs::path &session)
{
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    // The runtime's diagnostic output goes to the session logs, as with ./run.sh.
    const HANDLE in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0,
                                  nullptr);
    const HANDLE out = logFile(session.empty() ? fs::path(L"NUL") : session / L"stdout.log");
    const HANDLE err = logFile(session.empty() ? fs::path(L"NUL") : session / L"stderr.log");
    const auto closeLogs = [&] {
        for (const HANDLE handle : {in, out, err})
            if (handle != INVALID_HANDLE_VALUE)
                CloseHandle(handle);
    };

    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = in;
    startup.hStdOutput = out;
    startup.hStdError = err;
    std::wstring command = quoted(exe.native()) + L" " + quoted(elf.native());
    std::wstring block = environmentBlock(env);
    PROCESS_INFORMATION process{};
    // Suspended until it is in the job, so the game can never outlive a launcher that was ended.
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE,
                        CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED, block.data(), nullptr, &startup, &process))
    {
        const std::string reason = systemMessage(GetLastError());
        closeLogs();
        error("Fukami could not start the game", reason);
        return std::nullopt;
    }
    closeLogs();
    const HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job)
    {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits);
        AssignProcessToJobObject(job, process.hProcess); // best effort: the game runs without it too
    }
    ResumeThread(process.hThread);
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD status = 1;
    GetExitCodeProcess(process.hProcess, &status);
    CloseHandle(process.hProcess);
    if (job)
        CloseHandle(job);
    return status;
}

[[noreturn]] void launchFromApp(const fs::path &exe, const fs::path &root, const std::vector<fs::path> &argumentChds)
{
    const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));
    (void)com; // the dialogs fall back (no picker, no progress window) without it; the process ends in quit()

    // One launcher at a time: it owns the save card and the session folders until its game has ended. A launch on
    // another data folder (FUKAMI_DATA_DIR) owns other files, so it has a mutex of its own.
    const std::wstring mutexName =
        environmentValue(L"FUKAMI_DATA_DIR").empty() ? kInstanceMutex : std::wstring(kInstanceMutex) + L".TestData";
    const HANDLE instance = CreateMutexW(nullptr, TRUE, mutexName.c_str());
    if (!instance || GetLastError() == ERROR_ALREADY_EXISTS)
    {
        message("Fukami", "Fukami is already running.", MB_OK, MB_ICONINFORMATION);
        quit(0);
    }

    const fs::path support = supportDirectory();
    const fs::path disc = support / L"disc";
    const fs::path ini = support / L"fukami.ini";
    const fs::path mc = support / L"mc";
    std::error_code ec;
    fs::create_directories(support, ec);

    if (!discReady(disc) && !setUpDisc(support, disc, argumentChds))
        quit(0);

    for (;;)
    {
        auto doc = loadSettings(ini, root / L"share" / L"fukami-default.ini");
        if (!doc)
            quit(1);

        // Logging off (the default): no session folder, no file. The game's output goes to NUL.
        const bool logging = fukami::settings::LaunchConfig::fromIni(*doc, false).logging;
        fs::path session;
        if (logging)
        {
            const std::string stem = timestamp() + "-" + std::to_string(GetCurrentProcessId());
            session = support / L"sessions" / stem;
            // A restart keeps the launcher's process id; two launches in the same second still get their own folder.
            for (unsigned n = 1; fs::exists(session, ec); ++n)
                session = support / L"sessions" / (stem + "-" + std::to_string(n));
            fs::create_directories(session, ec);
        }
        fs::create_directories(mc, ec);
        if (logging)
            // The card as it was at launch, so this session can become a replay later
            // (same as the developer launcher).
            fs::copy(mc, session / L"mc-start", fs::copy_options::recursive, ec);

        const std::vector<std::string> env = gameEnvironment(*doc, root, support, ini, session);
        if (logging)
            std::fprintf(stderr, "[fukami] starting the game; logs in %s\n", text(session).c_str());
        const auto status = runGame(exe, disc / L"SLUS_200.02", env, session);
        if (!status)
            quit(1);
        if (*status == kRestartExit)
            continue; // the menu's Restart: read the settings file again
        if (*status != 0)
        {
            char code[16];
            std::snprintf(code, sizeof code, "0x%08lX", static_cast<unsigned long>(*status));
            if (!logging)
                error("Fukami stopped unexpectedly", "The game ended with status " + std::string(code) +
                                                         ".\n\nTurn on Logging in the settings to keep a log of the next run.");
            else if (message("Fukami stopped unexpectedly",
                             "The game ended with status " + std::string(code) + ". Its log is in\n" + text(session) +
                                 "\n\nShow that folder?",
                             MB_YESNO, MB_ICONERROR) == IDYES)
                ShellExecuteW(nullptr, L"open", session.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        quit(static_cast<int>(*status));
    }
}

} // namespace

namespace fukami::app {

void entry(int argc, char **argv)
{
    if (std::getenv(kChild))
        return;
    // `--print-launch-env` / `--print-launch-config` are command-line queries, never an app launch
    // (the same rule as the macOS and Linux hooks).
    if (argv && argc >= 2 && argv[1] && std::strncmp(argv[1], "--print-launch-", 15) == 0)
        return;
    const fs::path exe = executablePath();
    const fs::path root = appRoot(exe);
    if (root.empty())
        return;
    // The wide command line, so a CHD path keeps every character whatever the process code page is.
    std::vector<fs::path> chds;
    int count = 0;
    if (wchar_t **wide = CommandLineToArgvW(GetCommandLineW(), &count))
    {
        for (int i = 1; i < count; ++i)
            if (isChd(wide[i]))
                chds.emplace_back(wide[i]);
        LocalFree(wide);
    }
    launchFromApp(exe, root, chds);
}

std::filesystem::path defaultIniPath()
{
    const fs::path root = appRoot(executablePath());
    return root.empty() ? fs::path{} : root / L"share" / L"fukami-default.ini";
}

void restart()
{
    if (!inApp())
        return;
    // The launcher (launchFromApp) is waiting for this process: this status makes it start the game again. The
    // product has already stopped (runtime and GS torn down); end here, as execve does on the other platforms.
    std::fflush(nullptr);
    TerminateProcess(GetCurrentProcess(), kRestartExit);
}

void reveal(const std::filesystem::path &path)
{
    if (path.empty())
        return;
    // The shell needs COM on the calling thread and may take a moment; do it off the caller's thread.
    std::thread([target = fs::path(path).make_preferred().native()] {
        if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)))
            return;
        PIDLIST_ABSOLUTE item = nullptr;
        if (SUCCEEDED(SHParseDisplayName(target.c_str(), nullptr, &item, 0, nullptr)) && item)
        {
            SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
            CoTaskMemFree(item);
        }
        CoUninitialize();
    }).detach();
}

} // namespace fukami::app
