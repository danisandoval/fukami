// Fukami settings: the one schema and the one translation from rrv.ini (and
// command-line overrides) to the runtime's environment. It reads and writes
// the ini in place (comments, blank lines, unknown sections and key order are
// kept), turns the settings into a typed LaunchConfig and that into the
// environment the game process receives. The app launcher, the in-game menu,
// the developer launcher (`Fukami --print-launch-env`) and the
// tests all use this module; nothing else translates the ini.
#pragma once

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fukami::settings {

// ---- ini document --------------------------------------------------------
class IniDocument
{
public:
    static IniDocument parse(std::string_view text);
    // Missing file -> empty document; unreadable file -> nullopt.
    static std::optional<IniDocument> load(const std::filesystem::path &path);

    // Keys and sections compare case-insensitively. get() returns nullopt when
    // the key is absent (a present but empty value returns "").
    std::optional<std::string> get(std::string_view section, std::string_view key) const;
    // Replaces the value on the key's existing line, or appends `key = value`
    // at the end of the section (creating the section at the end if needed).
    void set(std::string_view section, std::string_view key, std::string_view value);
    // [env] lines (NAME = value) in file order.
    std::vector<std::pair<std::string, std::string>> entries(std::string_view section) const;
    // Every key/value line: section, key, value, 1-based line number.
    // `key` is lower-case; `name` keeps the spelling as written.
    struct Entry { std::string section, key, value; int line; std::string name; };
    std::vector<Entry> all() const;
    // 1-based line numbers of lines that are neither blank, comment, section
    // header nor key = value.
    std::vector<int> malformed() const;

    std::string text() const;
    // Atomic write (temp file + rename). Returns false on failure.
    bool save(const std::filesystem::path &path) const;

private:
    struct Line
    {
        std::string raw;     // the line as it was read (or as set() wrote it)
        std::string section; // lower-case section the line belongs to
        std::string key;     // lower-case key, empty for non key/value lines
        std::string value;   // trimmed value
    };
    std::vector<Line> m_lines;
    bool m_trailingNewline = true;
};

// ---- schema --------------------------------------------------------------
enum class Kind { boolean, choice, integer, number, size, text };
enum class Apply { live, restart };

struct Spec
{
    std::string_view section;
    std::string_view key;
    std::string_view label;       // short menu label
    std::string_view help;        // one line for the menu
    Kind kind;
    std::string_view defaultValue;
    std::vector<std::string_view> choices; // Kind::choice (and integer lists)
    double minValue = 0.0;                 // Kind::integer / number
    double maxValue = 0.0;
    Apply apply = Apply::restart;
    bool inMenu = true;                    // false: advanced, ini only
};

const std::vector<Spec> &schema();
const Spec *find(std::string_view section, std::string_view key);
// True when `value` is legal for the spec (same rules as run_gate4_product.sh).
bool valid(const Spec &spec, std::string_view value);
// The effective value: the ini's if present (and valid), else the default.
std::string value(const IniDocument &doc, const Spec &spec);
bool boolValue(std::string_view value); // true|yes|on|1 (case-insensitive)

// ---- launch configuration -------------------------------------------------
// [env] passthrough policy. Without `[developer] enabled = true` (or, for the
// developer launcher, RRV_DEVELOPER=1 in its environment) only the names the
// settings themselves map to may be set from the ini; every other RRV_*
// variable (diagnostics, qualification switches, anything that writes to disk)
// needs developer mode.
const std::vector<std::string_view> &userEnvAllowlist();
bool envAllowed(std::string_view name, bool developer);

struct LaunchConfig
{
    // [display]
    std::string ratio = "16:9", hud = "stretch", aspect, window;
    bool fullscreen = false, integerScaling = false, presentPacing = true;
    // [rendering]
    std::string renderMode = "full";
    int scale = 4;
    bool aa1 = true, fxaa = false;
    int cas = 0, aniso = 0;
    bool mipmap = true;
    // [input]
    bool analog = true, rumble = true;
    // [game]
    std::string carLod = "4";
    int drawDistance = 0;
    bool fastUnpack = true, nativeCode = true;
    // [timing]
    bool unpaced = false, inlineExecution = false, headless = false;
    bool splitGs = false; // VU1 and GS on two threads (RRV_VU1GS_SPLIT=1); owner-async only
    int pacerSpinMs = 0;  // paced only: busy-wait the last N ms of each wait (RRV_GATE3_REALTIME_SPIN_MS)
    std::string fastForwardFrom;
    // [paths] (developer launcher only; the app ignores them)
    std::string runtime, workload, replay, elf;
    // [developer]
    bool developer = false;
    // [env] lines the policy allows, in file order; applied last.
    std::vector<std::pair<std::string, std::string>> extraEnv;

    // Built from a document whose values are valid (see check()); an invalid
    // value falls back to its default.
    static LaunchConfig fromIni(const IniDocument &doc, bool developerOverride = false);
    // The inverse of environment() for the settings it carries: what a game
    // process was launched with. `get` is getenv-like (null when unset).
    template <class Getter> static LaunchConfig fromEnvironment(Getter get);
    // Empty when consistent, else the launcher's complaint.
    std::string problem() const;
};

struct Override { std::string section, key, value; };
// Maps a command-line flag to the setting it overrides (`--no-aa1` ->
// rendering.aa1=false, `--ratio 4:3` -> display.ratio=4:3, ...). Returns the
// number of argv items consumed, 0 if `argv[0]` is not a settings flag, or -1
// when the flag needs a value that is missing.
int flagOverride(const std::vector<std::string> &argv, size_t index, std::vector<Override> &out);
// Applies overrides on top of a document. A value that is invalid for its
// setting is reported (and not applied).
std::vector<std::string> applyOverrides(IniDocument &doc, const std::vector<Override> &overrides);

// ---- package defaults -------------------------------------------------------
// The app ships its own defaults in a bundled ini (Fukami.app Resources/ and
// the Linux package's share/fukami-default.ini; the Linux one sets `scale = 2`).
// The launchers copy that file
// to the user's fukami.ini once, so a file written by an older app version has
// no line for a setting added later. Such a setting takes the bundled ini's
// value, not the schema default. Only schema settings are inherited ([env]
// lines are not), a bundled value that is not valid is ignored, and a key the
// user's file has is never touched.
//
// inherited(): the settings `doc` does not have, with the value `defaults`
// gives them. withDefaults(): `doc` with those filled in. It is the document
// to launch with and to show; it is never saved (the user's file stays as it
// is, a missing key keeps following the app's default across upgrades).
std::vector<Override> inherited(const IniDocument &doc, const IniDocument &defaults);
IniDocument withDefaults(IniDocument doc, const IniDocument &defaults);

// ---- launch environment --------------------------------------------------
struct LaunchPaths
{
    std::filesystem::path boundWorkload; // .../bound-workload.txt
    std::filesystem::path memoryCard;    // RRV_GATE3_MC_ROOT
    std::filesystem::path session;       // this session's folder
    std::filesystem::path libraries;     // DYLD_LIBRARY_PATH
    std::filesystem::path menuIni;       // RRV_FUKAMI_INI (the in-game menu's file); empty: none
    bool recordPad = true;               // false for a replay: recorded input, no pad log
    std::string home;
    std::string path;
    std::string tmpdir;
};

struct Issue { int line; std::string message; };

// Validates every known key. Unknown keys, keys in the wrong section, bad
// values and [env] names the policy refuses are reported; the [paths] section
// is checked only when strict is true (the app ignores [paths]; it owns its
// own folders). `developerOverride` is the developer launcher's
// RRV_DEVELOPER=1; the app never sets it.
std::vector<Issue> check(const IniDocument &doc, bool strict, bool developerOverride = false);

// NAME=value strings the game process receives, in the order the launcher has
// always passed them to `env -i`.
std::vector<std::string> environment(const LaunchConfig &config, const LaunchPaths &paths);
std::vector<std::string> environment(const IniDocument &doc, const LaunchPaths &paths);

// `Fukami --print-launch-env INI [options]` and
// `--print-launch-config INI [options]`: the developer launcher's single
// translation of rrv.ini and flags. Prints to stdout, errors to stderr
// ("INI: line N: message", exit 2). Options: --set SECTION.KEY=VALUE,
// --root DIR, --workload-bound FILE, --memory-card DIR, --session DIR,
// --libraries DIR, --replay, --developer, --menu-ini, --defaults FILE (the
// package defaults for keys INI does not have, see withDefaults()) and
// `--flags ARGS...` (the launcher's own command line, last on the line:
// settings flags such as `--no-aa1`, `--ratio 4:3`, `--workload DIR`, and the
// ELF path). `appDefaults` is the bundled default ini of the app this binary
// runs from (fukami::app::defaultIniPath(), empty outside an app); it is used
// when no --defaults is given, so the app's own binary answers with what an
// app launch would use.
int launchEnvMain(int argc, char **argv, const std::filesystem::path &appDefaults = {});

template <class Getter> LaunchConfig LaunchConfig::fromEnvironment(Getter get)
{
    const auto text = [&](const char *name) { return std::string(get(name) ? get(name) : ""); };
    const auto set = [&](const char *name) { return get(name) && *get(name); };
    LaunchConfig c;
    c.headless = text("RRV_GATE3_HEADLESS") == "1";
    c.unpaced = !set("RRV_GATE3_REALTIME");
    c.fastForwardFrom = text("RRV_GATE3_FAST_FORWARD");
    c.inlineExecution = text("RRV_VU1GS_EXECUTION") != "owner-async";
    c.splitGs = text("RRV_VU1GS_SPLIT") == "1";
    c.pacerSpinMs = set("RRV_GATE3_REALTIME_SPIN_MS") ? std::atoi(text("RRV_GATE3_REALTIME_SPIN_MS").c_str()) : 0;
    if (set("RRV_GS_RENDER_MODE"))
        c.renderMode = text("RRV_GS_RENDER_MODE");
    if (set("RRV_PCSX2_GS_FULL_SCALE"))
        c.scale = std::atoi(text("RRV_PCSX2_GS_FULL_SCALE").c_str());
    c.aa1 = text("RRV_PCSX2_GS_FULL_HWAA1") != "0";
    c.fxaa = text("RRV_PCSX2_GS_FXAA") == "1";
    c.cas = set("RRV_PCSX2_GS_CAS") ? std::atoi(text("RRV_PCSX2_GS_CAS").c_str()) : 0;
    c.aniso = set("RRV_PCSX2_GS_ANISO") ? std::atoi(text("RRV_PCSX2_GS_ANISO").c_str()) : 0;
    c.mipmap = text("RRV_PCSX2_GS_FULL_HWMIPMAP") != "0";
    c.analog = text("RRV_PAD_ANALOG") != "0";
    c.rumble = text("RRV_PAD_RUMBLE") != "0";
    c.carLod = set("RRV_RR5_CAR_LOD") ? text("RRV_RR5_CAR_LOD") : "1";
    c.drawDistance = set("RRV_RR5_DRAW_DISTANCE") ? std::atoi(text("RRV_RR5_DRAW_DISTANCE").c_str()) : 0;
    c.fastUnpack = text("RRV_RR5_FAST_UNPACK") == "1";
    c.nativeCode = text("RRV_RR5_NATIVE_HOT") == "1";
    c.ratio = set("RRV_RR5_WIDESCREEN") ? text("RRV_RR5_WIDESCREEN") : "4:3";
    c.hud = !set("RRV_PCSX2_GS_HUD_SCALE") ? "stretch" : (text("RRV_PCSX2_GS_HUD_MODE") == "race" ? "race" : "fixed");
    c.aspect = text("RRV_PCSX2_GS_ASPECT");
    c.window = text("RRV_WINDOW_SIZE");
    c.fullscreen = text("RRV_WINDOW_FULLSCREEN") == "1";
    c.integerScaling = text("RRV_PCSX2_GS_INTEGER_SCALING") == "1";
    c.presentPacing = text("RRV_PCSX2_GS_PRESENT_PACING") != "0";
    return c;
}

} // namespace fukami::settings
