// Fukami.app settings; see fukami_settings.h. The validation rules and the
// environment mapping mirror scripts/run_gate4_product.sh (rrv.ini reader and
// the `extra` list) so a launch from the app and from ./run.sh with the same
// ini give the runtime the same environment (Gate-9 APP6).
#include "fukami_settings.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <system_error>

namespace fukami::settings {
namespace {

std::string lower(std::string_view text)
{
    std::string out(text);
    for (char &c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string_view trim(std::string_view text)
{
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v'; };
    while (!text.empty() && space(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && space(text.back()))
        text.remove_suffix(1);
    return text;
}

bool isBool(std::string_view value)
{
    const std::string v = lower(value);
    return v == "true" || v == "yes" || v == "on" || v == "1" || v == "false" || v == "no" || v == "off" || v == "0";
}

bool matches(std::string_view value, const char *pattern)
{
    return std::regex_match(std::string(value), std::regex(pattern));
}

bool oneOf(std::string_view value, const std::vector<std::string_view> &choices)
{
    return std::find(choices.begin(), choices.end(), value) != choices.end();
}

} // namespace

// ---- ini document --------------------------------------------------------

IniDocument IniDocument::parse(std::string_view text)
{
    IniDocument doc;
    doc.m_trailingNewline = text.empty() || text.back() == '\n';
    std::string section;
    size_t start = 0;
    while (start < text.size())
    {
        size_t end = text.find('\n', start);
        if (end == std::string_view::npos)
            end = text.size();
        Line line;
        line.raw = std::string(text.substr(start, end - start));
        std::string_view body = trim(line.raw);
        if (!body.empty() && body.front() == '[' && body.back() == ']')
        {
            section = lower(trim(body.substr(1, body.size() - 2)));
        }
        else if (!body.empty() && body.front() != '#' && body.front() != ';')
        {
            const size_t eq = body.find('=');
            if (eq != std::string_view::npos)
            {
                line.key = lower(trim(body.substr(0, eq)));
                line.value = std::string(trim(body.substr(eq + 1)));
            }
        }
        line.section = section;
        doc.m_lines.push_back(std::move(line));
        start = end + 1;
    }
    return doc;
}

std::optional<IniDocument> IniDocument::load(const std::filesystem::path &path)
{
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        return IniDocument{};
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::nullopt;
    std::ostringstream text;
    text << in.rdbuf();
    if (in.bad())
        return std::nullopt;
    return parse(text.str());
}

std::optional<std::string> IniDocument::get(std::string_view section, std::string_view key) const
{
    const std::string s = lower(section), k = lower(key);
    std::optional<std::string> found;
    for (const Line &line : m_lines)
        if (!line.key.empty() && line.section == s && line.key == k)
            found = line.value; // the shell reader lets a later line win
    return found;
}

void IniDocument::set(std::string_view section, std::string_view key, std::string_view value)
{
    const std::string s = lower(section), k = lower(key);
    Line *last = nullptr;
    for (Line &line : m_lines)
        if (!line.key.empty() && line.section == s && line.key == k)
            last = &line;
    if (last)
    {
        // Keep the line's own spelling of the key and its leading indentation.
        const size_t eq = last->raw.find('=');
        std::string prefix = last->raw.substr(0, eq);
        while (!prefix.empty() && (prefix.back() == ' ' || prefix.back() == '\t'))
            prefix.pop_back();
        last->raw = prefix + " = " + std::string(value);
        last->value = std::string(value);
        return;
    }
    // Append at the end of the section: after its last key line, or right
    // after the header when it has none.
    ptrdiff_t insertAt = -1;
    bool sectionSeen = false;
    for (size_t i = 0; i < m_lines.size(); ++i)
    {
        const std::string_view body = trim(m_lines[i].raw);
        const bool header = !body.empty() && body.front() == '[' && body.back() == ']';
        if (header && m_lines[i].section == s)
        {
            sectionSeen = true;
            insertAt = static_cast<ptrdiff_t>(i) + 1;
        }
        else if (sectionSeen && m_lines[i].section == s && !m_lines[i].key.empty())
            insertAt = static_cast<ptrdiff_t>(i) + 1;
    }
    Line line;
    line.raw = std::string(key) + " = " + std::string(value);
    line.section = s;
    line.key = k;
    line.value = std::string(value);
    if (!sectionSeen)
    {
        if (!m_lines.empty() && !trim(m_lines.back().raw).empty())
            m_lines.push_back(Line{"", m_lines.back().section, "", ""});
        m_lines.push_back(Line{"[" + std::string(section) + "]", s, "", ""});
        m_lines.push_back(std::move(line));
        return;
    }
    m_lines.insert(m_lines.begin() + insertAt, std::move(line));
}

std::vector<std::pair<std::string, std::string>> IniDocument::entries(std::string_view section) const
{
    const std::string s = lower(section);
    std::vector<std::pair<std::string, std::string>> out;
    for (const Line &line : m_lines)
    {
        if (line.key.empty() || line.section != s)
            continue;
        const size_t eq = line.raw.find('=');
        out.emplace_back(std::string(trim(std::string_view(line.raw).substr(0, eq))), line.value);
    }
    return out;
}

std::vector<IniDocument::Entry> IniDocument::all() const
{
    std::vector<Entry> out;
    for (size_t i = 0; i < m_lines.size(); ++i)
        if (!m_lines[i].key.empty())
        {
            const std::string_view body = trim(m_lines[i].raw);
            const size_t eq = body.find('=');
            out.push_back({m_lines[i].section, m_lines[i].key, m_lines[i].value, static_cast<int>(i) + 1,
                           std::string(trim(body.substr(0, eq)))});
        }
    return out;
}

std::vector<int> IniDocument::malformed() const
{
    std::vector<int> out;
    for (size_t i = 0; i < m_lines.size(); ++i)
    {
        const std::string_view body = trim(m_lines[i].raw);
        if (body.empty() || body.front() == '#' || body.front() == ';' || !m_lines[i].key.empty())
            continue;
        if (body.front() == '[' && body.back() == ']')
            continue;
        out.push_back(static_cast<int>(i) + 1);
    }
    return out;
}

std::string IniDocument::text() const
{
    std::string out;
    for (size_t i = 0; i < m_lines.size(); ++i)
    {
        out += m_lines[i].raw;
        if (i + 1 < m_lines.size() || m_trailingNewline)
            out += '\n';
    }
    return out;
}

bool IniDocument::save(const std::filesystem::path &path) const
{
    std::error_code ec;
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path(), ec);
    std::filesystem::path temp = path;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        const std::string body = text();
        out.write(body.data(), static_cast<std::streamsize>(body.size()));
        out.flush();
        if (!out)
            return false;
    }
    std::filesystem::rename(temp, path, ec);
    if (ec)
    {
        std::filesystem::remove(temp, ec);
        return false;
    }
    return true;
}

// ---- schema --------------------------------------------------------------

const std::vector<Spec> &schema()
{
    static const std::vector<Spec> specs = {
        // [display]
        {"display", "ratio", "Widescreen", "Real widescreen: the game renders a wider view. 4:3 is the stock game.",
         Kind::choice, "16:9", {"4:3", "16:9", "16:10", "21:9"}},
        {"display", "hud", "HUD", "How 2D (HUD, menus, text) shows on a wide picture.",
         Kind::choice, "stretch", {"stretch", "fixed", "race"}},
        {"display", "aspect", "Picture fit", "How the picture fits the window. Empty follows Widescreen.",
         Kind::choice, "", {"", "auto", "4:3", "16:9", "16:10", "21:9", "stretch"}, 0, 0, Apply::live},
        {"display", "window", "Window size", "Starting window size, WIDTHxHEIGHT. Empty is automatic.",
         Kind::size, "", {}, 0, 0, Apply::restart, false},
        {"display", "fullscreen", "Fullscreen", "Fill the screen. Also F11 or Cmd+Ctrl+F.",
         Kind::boolean, "false", {}, 0, 0, Apply::live},
        {"display", "integer_scaling", "Integer scaling", "Snap the picture to whole-number sizes.",
         Kind::boolean, "false", {}, 0, 0, Apply::live},
        {"display", "present_pacing", "Even frame pacing",
         "Shows every frame for the same time (smooth motion). Off: about 6-9 ms less delay, some judder.",
         Kind::boolean, "true"},
        // [rendering]
        {"rendering", "render_mode", "Render mode", "full: progressive full frames. field: the older half-height output.",
         Kind::choice, "full", {"full", "field"}},
        {"rendering", "scale", "Internal resolution", "Rendering scale (full mode). Higher is sharper and slower.",
         Kind::integer, "4", {}, 1, 8},
        {"rendering", "aa1", "Edge anti-aliasing (AA1)", "PCSX2's emulation of the PS2's AA1 edge smoothing.",
         Kind::boolean, "true"},
        {"rendering", "fxaa", "FXAA", "Smooths jagged edges on the finished picture.",
         Kind::boolean, "false", {}, 0, 0, Apply::live},
        {"rendering", "cas", "Sharpening (CAS)", "Sharpen the finished picture, 0 = off.",
         Kind::integer, "0", {}, 0, 100, Apply::live},
        {"rendering", "aniso", "Anisotropic filtering", "Keeps textures at steep angles sharp.",
         Kind::choice, "0", {"0", "2", "4", "8", "16"}},
        {"rendering", "mipmap", "Mipmaps", "Emulate the PS2's mipmaps (on is what the hardware does).",
         Kind::boolean, "true"},
        // [input]
        {"input", "analog", "Analog controller", "DualShock 2 sticks and trigger pressure. Off: d-pad only.",
         Kind::boolean, "true"},
        {"input", "rumble", "Rumble", "Controller vibration.",
         Kind::boolean, "true"},
        // [game]
        {"game", "car_lod", "Car detail distance", "Keeps cars detailed farther away. 1 is the stock game.",
         Kind::number, "4", {}, 1, 16},
        {"game", "draw_distance", "Scenery draw distance",
         "Draws track scenery this many sections further ahead. 0 is the stock game; 2 is the sweet spot.",
         Kind::integer, "0", {}, 0, 16},
        {"game", "fast_unpack", "Fast loading",
         "Unpacks loaded game data instantly, so loading does not freeze the picture. Off is the stock game's timing.",
         Kind::boolean, "true"},
        {"game", "native_code", "Native hot code",
         "Runs the game's busiest code as native code: same result, less CPU. Off runs all of it as recompiled code.",
         Kind::boolean, "true"},
        // [timing]: split_gs and pacer_spin are in the menu ("Performance"); the rest is advanced, ini only
        {"timing", "unpaced", "Unpaced", "Run as fast as the host can (speed tests).",
         Kind::boolean, "false", {}, 0, 0, Apply::restart, false},
        {"timing", "inline", "Inline", "Run VIF1, VU1 and GS on the EE thread.",
         Kind::boolean, "false", {}, 0, 0, Apply::restart, false},
        // Gate 5: the VU1/GS split. On by default on every host (owner decision 2026-10-04); a user ini
        // without the key takes the bundled default ini's value (withDefaults()). In the menu
        // ("Performance"), like pacer_spin: both are user switches.
        {"timing", "split_gs", "Split VU1 / GS",
         "Run VU1 and GS on two threads instead of one (faster on slower CPUs such as the Steam Deck).",
         Kind::boolean, "true"},
        // Gate 5: the pacer busy-waits the last N ms of its wait so the CPU keeps
        // its clock between fields (Steam Deck: 3.5 -> 2.4 GHz when the game
        // thread sleeps). 0 (sleep) on every host by default (owner decision 2026-10-04); on a Steam Deck
        // a value up to 17 steadies the frame rate at the cost of power.
        {"timing", "pacer_spin", "Pacer spin (ms)",
         "Busy-wait the last N ms before each field instead of sleeping (steadier on Steam Deck, uses more power). "
         "0 sleeps.",
         Kind::integer, "0", {}, 0, 17},
        {"timing", "headless", "Headless", "No window.",
         Kind::boolean, "false", {}, 0, 0, Apply::restart, false},
        {"timing", "from", "Fast-forward to", "Replay debugging only.",
         Kind::text, "", {}, 0, 0, Apply::restart, false},
        // [developer] (advanced): lifts the [env] allowlist
        {"developer", "enabled", "Developer mode", "Allow any RRV_* variable in [env] (diagnostics, qualification).",
         Kind::boolean, "false", {}, 0, 0, Apply::restart, false},
        // [paths] (developer launcher only; the app ignores them)
        {"paths", "runtime", "Runtime", "", Kind::text, "", {}, 0, 0, Apply::restart, false},
        {"paths", "workload", "Workload", "", Kind::text, "", {}, 0, 0, Apply::restart, false},
        {"paths", "replay", "Replay", "", Kind::text, "", {}, 0, 0, Apply::restart, false},
        {"paths", "elf", "ELF", "", Kind::text, "", {}, 0, 0, Apply::restart, false},
    };
    return specs;
}

const Spec *find(std::string_view section, std::string_view key)
{
    const std::string s = lower(section), k = lower(key);
    for (const Spec &spec : schema())
        if (spec.section == s && spec.key == k)
            return &spec;
    return nullptr;
}

bool boolValue(std::string_view value)
{
    const std::string v = lower(value);
    return v == "true" || v == "yes" || v == "on" || v == "1";
}

bool valid(const Spec &spec, std::string_view value)
{
    const std::string key(spec.key);
    if (key == "hud")
        return value == "fixed" || value == "race" || value == "stretch" || value == "no47";
    if (key == "scale")
        return value.size() == 1 && value[0] >= '1' && value[0] <= '8';
    if (key == "cas")
        return matches(value, "[0-9]+") && std::stoll(std::string(value).substr(0, 18)) <= 100;
    if (key == "car_lod")
        return matches(value, "[0-9]+([.][0-9]+)?");
    if (key == "draw_distance")
        return matches(value, "[0-9]+") && std::stoll(std::string(value).substr(0, 18)) <= 16;
    if (key == "pacer_spin")
        return matches(value, "[0-9]+") && std::stoll(std::string(value).substr(0, 18)) <= 17;
    if (key == "from")
        return value.empty() || matches(value, "[0-9]+");
    switch (spec.kind)
    {
    case Kind::boolean: return isBool(value);
    case Kind::choice: return oneOf(value, spec.choices);
    case Kind::size: return value.empty() || matches(value, "[0-9]+x[0-9]+");
    case Kind::integer:
    case Kind::number:
    case Kind::text: return true;
    }
    return true;
}

std::string value(const IniDocument &doc, const Spec &spec)
{
    const auto raw = doc.get(spec.section, spec.key);
    if (raw && valid(spec, *raw))
    {
        if (spec.key == "hud" && *raw == "no47")
            return "stretch";
        return *raw;
    }
    return std::string(spec.defaultValue);
}

const std::vector<std::string_view> &userEnvAllowlist()
{
    // The names the settings themselves map to (presentation, rendering, input and RR5 enhancements). Everything
    // else (diagnostics, qualification switches, anything that writes game data to disk) is developer-only.
    static const std::vector<std::string_view> names = {
        "RRV_GS_RENDER_MODE",           "RRV_PCSX2_GS_FULL_SCALE",       "RRV_PCSX2_GS_FULL_HWAA1",
        "RRV_PCSX2_GS_FXAA",            "RRV_PCSX2_GS_CAS",              "RRV_PCSX2_GS_ANISO",
        "RRV_PCSX2_GS_FULL_HWMIPMAP",   "RRV_PCSX2_GS_VSYNC",            "RRV_PCSX2_GS_PRESENT_PACING",
        "RRV_PCSX2_GS_ASPECT",          "RRV_PCSX2_GS_HUD_SCALE",        "RRV_PCSX2_GS_HUD_MODE",
        "RRV_PCSX2_GS_INTEGER_SCALING", "RRV_WINDOW_SIZE",               "RRV_WINDOW_FULLSCREEN",
        "RRV_PAD_ANALOG",               "RRV_PAD_RUMBLE",                "RRV_RR5_CAR_LOD",
        "RRV_RR5_DRAW_DISTANCE",        "RRV_RR5_FAST_UNPACK",           "RRV_RR5_NATIVE_HOT",
        "RRV_RR5_WIDESCREEN",
    };
    return names;
}

bool envAllowed(std::string_view name, bool developer)
{
    if (!matches(name, "RRV_[A-Z0-9_]+"))
        return false;
    return developer || oneOf(name, userEnvAllowlist());
}

namespace {

const std::map<std::string, std::string> &messages()
{
    static const std::map<std::string, std::string> table = {
        {"ratio", "ratio must be 4:3, 16:9, 16:10 or 21:9"},
        {"hud", "hud must be fixed, race or stretch"},
        {"aspect", "aspect must be empty, auto, 4:3, 16:9, 16:10, 21:9 or stretch"},
        {"window", "window must be empty or WIDTHxHEIGHT, e.g. 1920x1080"},
        {"render_mode", "render_mode must be full or field"},
        {"scale", "scale must be 1 to 8"},
        {"cas", "cas must be 0 to 100 (0 = off)"},
        {"aniso", "aniso must be 0, 2, 4, 8 or 16"},
        {"car_lod", "car_lod must be a number (1 = stock game)"},
        {"draw_distance", "draw_distance must be 0 to 16 (0 = stock game)"},
        {"pacer_spin", "pacer_spin must be 0 to 17 (ms; 0 = sleep)"},
        {"from", "from must be a start number (empty = no fast-forward)"},
    };
    return table;
}

std::string messageFor(const Spec &spec)
{
    const auto it = messages().find(std::string(spec.key));
    return it != messages().end() ? it->second : std::string(spec.key) + " must be true or false";
}

const char *kFromWithUnpaced = "--from needs real-time pacing (it fast-forwards to that start, then paces); drop --unpaced.";

} // namespace

std::vector<Issue> check(const IniDocument &doc, bool strict, bool developerOverride)
{
    std::vector<Issue> issues;
    const bool developer = developerOverride || boolValue(value(doc, *find("developer", "enabled")));
    for (int line : doc.malformed())
        issues.push_back({line, "expected key = value"});
    for (const auto &entry : doc.all())
    {
        if (entry.section == "env")
        {
            if (!matches(entry.name, "RRV_[A-Z0-9_]+"))
                issues.push_back({entry.line, "[env] names must look like RRV_NAME (got '" + entry.name + "')"});
            else if (!envAllowed(entry.name, developer))
                issues.push_back({entry.line, "[env] " + entry.name +
                                                  " is a developer setting: put 'enabled = true' under [developer] to use it"});
            continue;
        }
        const Spec *spec = find(entry.section, entry.key);
        if (!spec)
        {
            const Spec *elsewhere = nullptr;
            for (const Spec &s : schema())
                if (s.key == entry.key)
                    elsewhere = &s;
            issues.push_back({entry.line, elsewhere ? "'" + entry.key + "' belongs under [" + std::string(elsewhere->section) + "]"
                                                    : "unknown setting '" + entry.key + "'"});
            continue;
        }
        if (spec->section == "paths" && !strict)
            continue;
        if (!valid(*spec, entry.value))
            issues.push_back({entry.line, messageFor(*spec)});
    }
    std::stable_sort(issues.begin(), issues.end(), [](const Issue &a, const Issue &b) {
        return (a.line == 0 ? 1 << 30 : a.line) < (b.line == 0 ? 1 << 30 : b.line);
    });
    return issues;
}

LaunchConfig LaunchConfig::fromIni(const IniDocument &doc, const bool developerOverride)
{
    const auto get = [&](std::string_view section, std::string_view key) { return value(doc, *find(section, key)); };
    LaunchConfig c;
    c.ratio = get("display", "ratio");
    c.hud = get("display", "hud");
    c.aspect = get("display", "aspect");
    c.window = get("display", "window");
    c.fullscreen = boolValue(get("display", "fullscreen"));
    c.integerScaling = boolValue(get("display", "integer_scaling"));
    c.presentPacing = boolValue(get("display", "present_pacing"));
    c.renderMode = get("rendering", "render_mode");
    c.scale = std::stoi(get("rendering", "scale"));
    c.aa1 = boolValue(get("rendering", "aa1"));
    c.fxaa = boolValue(get("rendering", "fxaa"));
    c.cas = std::stoi(get("rendering", "cas"));
    c.aniso = std::stoi(get("rendering", "aniso"));
    c.mipmap = boolValue(get("rendering", "mipmap"));
    c.analog = boolValue(get("input", "analog"));
    c.rumble = boolValue(get("input", "rumble"));
    c.carLod = get("game", "car_lod");
    c.drawDistance = std::stoi(get("game", "draw_distance"));
    c.fastUnpack = boolValue(get("game", "fast_unpack"));
    c.nativeCode = boolValue(get("game", "native_code"));
    c.unpaced = boolValue(get("timing", "unpaced"));
    c.inlineExecution = boolValue(get("timing", "inline"));
    c.splitGs = boolValue(get("timing", "split_gs"));
    c.pacerSpinMs = std::stoi(get("timing", "pacer_spin"));
    c.headless = boolValue(get("timing", "headless"));
    c.fastForwardFrom = get("timing", "from");
    c.runtime = get("paths", "runtime");
    c.workload = get("paths", "workload");
    c.replay = get("paths", "replay");
    c.elf = get("paths", "elf");
    c.developer = developerOverride || boolValue(get("developer", "enabled"));
    // The policy again, so a config built without check() still cannot carry a developer variable.
    for (const auto &[name, val] : doc.entries("env"))
        if (envAllowed(name, c.developer))
            c.extraEnv.emplace_back(name, val);
    return c;
}

std::string LaunchConfig::problem() const
{
    if (!fastForwardFrom.empty() && unpaced)
        return kFromWithUnpaced;
    return {};
}

std::vector<std::string> environment(const LaunchConfig &c, const LaunchPaths &paths)
{
    std::vector<std::string> env = {
        "HOME=" + paths.home,
        "PATH=" + paths.path,
        "TMPDIR=" + paths.tmpdir,
        "DYLD_LIBRARY_PATH=" + paths.libraries.string(),
        "RRV_GATE3_BOUND_WORKLOAD=" + paths.boundWorkload.string(),
        "RRV_GATE3_MC_ROOT=" + paths.memoryCard.string(),
        "RRV_GATE3_MC_PERSISTENT=1",
    };
    if (paths.recordPad)
        env.push_back("RRV_GATE3_RECORD_PAD=" + (paths.session / "pad.jsonl").string());
    if (c.headless)
        env.push_back("RRV_GATE3_HEADLESS=1");
    if (!c.unpaced)
        env.push_back("RRV_GATE3_REALTIME=1");
    if (!c.unpaced && c.pacerSpinMs > 0)
        env.push_back("RRV_GATE3_REALTIME_SPIN_MS=" + std::to_string(c.pacerSpinMs));
    if (!c.fastForwardFrom.empty())
        env.push_back("RRV_GATE3_FAST_FORWARD=" + c.fastForwardFrom);
    if (!c.headless && !c.unpaced)
        env.push_back("RRV_PCSX2_GS_VSYNC=1");
    if (!c.inlineExecution)
    {
        env.push_back("RRV_GS_EXECUTION=worker-sync");
        env.push_back("RRV_VU1GS_EXECUTION=owner-async");
        if (c.splitGs)
            env.push_back("RRV_VU1GS_SPLIT=1");
    }
    env.push_back("RRV_GS_RENDER_MODE=" + c.renderMode);
    env.push_back("RRV_PCSX2_GS_FULL_SCALE=" + std::to_string(c.scale));
    if (!c.aa1)
        env.push_back("RRV_PCSX2_GS_FULL_HWAA1=0");
    if (c.fxaa)
        env.push_back("RRV_PCSX2_GS_FXAA=1");
    if (c.cas > 0)
        env.push_back("RRV_PCSX2_GS_CAS=" + std::to_string(c.cas));
    if (c.aniso != 0)
        env.push_back("RRV_PCSX2_GS_ANISO=" + std::to_string(c.aniso));
    if (!c.mipmap)
        env.push_back("RRV_PCSX2_GS_FULL_HWMIPMAP=0");
    if (!c.analog)
        env.push_back("RRV_PAD_ANALOG=0");
    if (!c.rumble)
        env.push_back("RRV_PAD_RUMBLE=0");
    if (c.carLod != "1")
        env.push_back("RRV_RR5_CAR_LOD=" + c.carLod);
    if (c.drawDistance > 0)
        env.push_back("RRV_RR5_DRAW_DISTANCE=" + std::to_string(c.drawDistance));
    if (c.fastUnpack)
        env.push_back("RRV_RR5_FAST_UNPACK=1");
    if (c.nativeCode)
        env.push_back("RRV_RR5_NATIVE_HOT=1");
    std::string aspect = c.aspect;
    if (!c.ratio.empty() && c.ratio != "4:3")
        env.push_back("RRV_RR5_WIDESCREEN=" + c.ratio);
    if (!c.ratio.empty() && aspect.empty())
        aspect = c.ratio;
    if (!c.ratio.empty() && c.ratio != "4:3" && c.hud != "stretch")
    {
        const double rw = std::stod(c.ratio.substr(0, c.ratio.find(':')));
        const double rh = std::stod(c.ratio.substr(c.ratio.find(':') + 1));
        char scale[64];
        // zsh printed $(( float )) with %.17g; the fixture was recorded from that.
        std::snprintf(scale, sizeof scale, "%.17g", (4.0 / 3.0) / (rw * 1.0 / rh));
        env.push_back(std::string("RRV_PCSX2_GS_HUD_SCALE=") + scale);
        if (c.hud == "race")
            env.push_back("RRV_PCSX2_GS_HUD_MODE=race");
    }
    if (!aspect.empty())
        env.push_back("RRV_PCSX2_GS_ASPECT=" + aspect);
    if (!c.window.empty())
        env.push_back("RRV_WINDOW_SIZE=" + c.window);
    if (c.fullscreen)
        env.push_back("RRV_WINDOW_FULLSCREEN=1");
    if (c.integerScaling)
        env.push_back("RRV_PCSX2_GS_INTEGER_SCALING=1");
    if (!c.presentPacing)
        env.push_back("RRV_PCSX2_GS_PRESENT_PACING=0");
    env.push_back("RRV_GATE4_START_CLOCK=" + (paths.session / "start-clock.txt").string());
    if (!paths.menuIni.empty())
        env.push_back("RRV_FUKAMI_INI=" + paths.menuIni.string());
    for (const auto &[name, val] : c.extraEnv)
        env.push_back(name + "=" + val);
    return env;
}

std::vector<std::string> environment(const IniDocument &doc, const LaunchPaths &paths)
{
    return environment(LaunchConfig::fromIni(doc), paths);
}

// ---- command-line flags -----------------------------------------------------

int flagOverride(const std::vector<std::string> &argv, const size_t index, std::vector<Override> &out)
{
    const std::string &flag = argv[index];
    struct Plain { const char *flag, *section, *key, *value; };
    static const Plain plain[] = {
        {"--headless", "timing", "headless", "true"},     {"--unpaced", "timing", "unpaced", "true"},
        {"--inline", "timing", "inline", "true"},         {"--threaded", "timing", "inline", "false"},
        {"--no-aa1", "rendering", "aa1", "false"},        {"--fxaa", "rendering", "fxaa", "true"},
        {"--no-mipmap", "rendering", "mipmap", "false"},  {"--no-analog", "input", "analog", "false"},
        {"--no-rumble", "input", "rumble", "false"},      {"--no-fast-unpack", "game", "fast_unpack", "false"},
        {"--no-native-code", "game", "native_code", "false"}, {"--fullscreen", "display", "fullscreen", "true"},
        {"--integer-scaling", "display", "integer_scaling", "true"},
        {"--no-present-pacing", "display", "present_pacing", "false"},
        {"--widescreen", "display", "ratio", "16:9"},     {"--stretched-hud", "display", "hud", "stretch"},
    };
    for (const Plain &p : plain)
        if (flag == p.flag)
        {
            out.push_back({p.section, p.key, p.value});
            return 1;
        }
    struct Valued { const char *flag, *section, *key; };
    static const Valued valued[] = {
        {"--cas", "rendering", "cas"},           {"--aniso", "rendering", "aniso"},
        {"--car-lod", "game", "car_lod"},        {"--draw-distance", "game", "draw_distance"},
        {"--ratio", "display", "ratio"},         {"--hud", "display", "hud"},
        {"--aspect", "display", "aspect"},       {"--window", "display", "window"},
        {"--scale", "rendering", "scale"},       {"--render-mode", "rendering", "render_mode"},
        {"--from", "timing", "from"},            {"--workload", "paths", "workload"},
        {"--runtime", "paths", "runtime"},       {"--replay", "paths", "replay"},
    };
    for (const Valued &v : valued)
        if (flag == v.flag)
        {
            if (index + 1 >= argv.size())
                return -1;
            out.push_back({v.section, v.key, argv[index + 1]});
            return 2;
        }
    return 0;
}

std::vector<std::string> applyOverrides(IniDocument &doc, const std::vector<Override> &overrides)
{
    std::vector<std::string> problems;
    for (const Override &o : overrides)
    {
        const Spec *spec = find(o.section, o.key);
        if (!spec || !valid(*spec, o.value))
        {
            problems.push_back(spec ? messageFor(*spec) : "unknown setting '" + o.section + "." + o.key + "'");
            continue;
        }
        doc.set(o.section, o.key, o.value);
    }
    return problems;
}

// ---- package defaults ---------------------------------------------------------

std::vector<Override> inherited(const IniDocument &doc, const IniDocument &defaults)
{
    std::vector<Override> out;
    for (const Spec &spec : schema())
    {
        if (doc.get(spec.section, spec.key))
            continue;
        const auto packaged = defaults.get(spec.section, spec.key);
        if (packaged && valid(spec, *packaged))
            out.push_back({std::string(spec.section), std::string(spec.key), *packaged});
    }
    return out;
}

IniDocument withDefaults(IniDocument doc, const IniDocument &defaults)
{
    for (const Override &o : inherited(doc, defaults))
        doc.set(o.section, o.key, o.value);
    return doc;
}

// ---- developer launcher entry ------------------------------------------------

int launchEnvMain(int argc, char **argv, const std::filesystem::path &appDefaults)
{
    const auto fail = [](const std::string &text) {
        std::fprintf(stderr, "%s\n", text.c_str());
        return 2;
    };
    if (argc < 3)
        return fail("usage: --print-launch-env|--print-launch-config INI [--set SECTION.KEY=VALUE] [--root DIR] "
                    "[--workload-bound FILE] [--memory-card DIR] [--session DIR] [--libraries DIR] [--replay] "
                    "[--developer] [--menu-ini] [--defaults FILE]");
    const bool wantEnv = std::string(argv[1]) == "--print-launch-env";
    const std::filesystem::path iniPath = argv[2];
    LaunchPaths paths;
    std::filesystem::path root, defaultsPath = appDefaults;
    std::vector<Override> overrides;
    bool developer = false, menuIni = false;
    std::vector<std::string> flags;
    for (int i = 3; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--flags")
        {
            // Everything after is the launcher's own command line (./run.sh's flags and the ELF path).
            flags.assign(argv + i + 1, argv + argc);
            break;
        }
        const auto next = [&](std::string &into) {
            if (i + 1 >= argc)
                return false;
            into = argv[++i];
            return true;
        };
        std::string text;
        if (arg == "--set")
        {
            if (!next(text) || text.find('=') == std::string::npos || text.find('.') == std::string::npos)
                return fail("--set needs SECTION.KEY=VALUE");
            const size_t eq = text.find('='), dot = text.find('.');
            overrides.push_back({text.substr(0, dot), text.substr(dot + 1, eq - dot - 1), text.substr(eq + 1)});
        }
        else if (arg == "--root" && next(text)) root = text;
        else if (arg == "--workload-bound" && next(text)) paths.boundWorkload = text;
        else if (arg == "--memory-card" && next(text)) paths.memoryCard = text;
        else if (arg == "--session" && next(text)) paths.session = text;
        else if (arg == "--libraries" && next(text)) paths.libraries = text;
        else if (arg == "--defaults" && next(text)) defaultsPath = text;
        else if (arg == "--replay") paths.recordPad = false;
        else if (arg == "--developer") developer = true;
        else if (arg == "--menu-ini") menuIni = true;
        else
            return fail("unknown or incomplete option '" + arg + "'");
    }
    auto doc = IniDocument::load(iniPath);
    if (!doc)
        return fail(iniPath.string() + ": cannot be read");
    const auto issues = check(*doc, true, developer);
    if (!issues.empty())
        return fail(iniPath.string() + (issues.front().line > 0 ? ": line " + std::to_string(issues.front().line) : "") +
                    ": " + issues.front().message);
    if (!defaultsPath.empty())
    {
        // Keys INI does not have take the package defaults (the app launchers do the same).
        const auto defaults = IniDocument::load(defaultsPath);
        if (!defaults)
            return fail(defaultsPath.string() + ": cannot be read");
        *doc = withDefaults(std::move(*doc), *defaults);
    }
    std::string elfFlag;
    for (size_t i = 0; i < flags.size();)
    {
        const std::string &flag = flags[i];
        if (flag == "--config")
        {
            i += 2; // the launcher already used it to pick the ini
            continue;
        }
        if (flag == "--help" || flag == "-h")
        {
            ++i;
            continue;
        }
        if (flag.empty() || flag.front() != '-')
        {
            if (!elfFlag.empty())
                return fail("only one ELF path may be given (got '" + elfFlag + "' and '" + flag + "')");
            elfFlag = flag;
            ++i;
            continue;
        }
        const int used = flagOverride(flags, i, overrides);
        if (used < 0)
            return fail("option " + flag + " needs a value");
        if (used == 0)
            return fail("unknown option '" + flag + "' (./run.sh --help lists the options)");
        i += static_cast<size_t>(used);
    }
    if (!elfFlag.empty())
        overrides.push_back({"paths", "elf", elfFlag});
    // Folders and files given on the command line are relative to where it was typed (the runtime is a name).
    for (Override &o : overrides)
        if (o.section == "paths" && o.key != "runtime" && !o.value.empty())
            o.value = std::filesystem::weakly_canonical(std::filesystem::absolute(o.value)).string();
    const auto problems = applyOverrides(*doc, overrides);
    if (!problems.empty())
        return fail(problems.front());
    const LaunchConfig config = LaunchConfig::fromIni(*doc, developer);
    if (const std::string problem = config.problem(); !problem.empty())
        return fail(problem);

    const auto getenvOr = [](const char *name, const char *fallback) {
        const char *v = std::getenv(name);
        return std::string(v ? v : fallback);
    };
    if (wantEnv)
    {
        paths.home = getenvOr("HOME", "");
        paths.path = getenvOr("PATH", "");
        paths.tmpdir = getenvOr("TMPDIR", "/tmp");
        if (menuIni)
            paths.menuIni = iniPath;
        for (const std::string &line : environment(config, paths))
            std::printf("%s\n", line.c_str());
        return 0;
    }
    // Paths in the ini are relative to the repository root.
    const auto resolve = [&](const std::string &p) {
        if (p.empty() || p.front() == '/' || root.empty())
            return p;
        return (root / p).string();
    };
    std::printf("runtime=%s\nworkload=%s\nreplay=%s\nelf=%s\nheadless=%s\nunpaced=%s\ninline=%s\nfrom=%s\n"
                "render_mode=%s\nscale=%d\ncar_lod=%s\ndraw_distance=%d\ndeveloper=%s\n",
                config.runtime.c_str(), resolve(config.workload).c_str(), resolve(config.replay).c_str(),
                resolve(config.elf).c_str(), config.headless ? "true" : "false", config.unpaced ? "true" : "false",
                config.inlineExecution ? "true" : "false", config.fastForwardFrom.c_str(), config.renderMode.c_str(),
                config.scale, config.carLod.c_str(), config.drawDistance, config.developer ? "true" : "false");
    return 0;
}

} // namespace fukami::settings
