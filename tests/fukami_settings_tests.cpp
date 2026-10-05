// Fukami settings module: ini round trip, validation, the typed LaunchConfig, the [env] policy, flag overrides and the
// launch environment. The full old-launcher parity matrix is tests/test_launch_env_golden.py.
#include "fukami_settings.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace fukami::settings;

namespace {

int failures = 0;

void expect(bool ok, const std::string &what)
{
    if (!ok)
    {
        ++failures;
        std::cerr << "FAIL: " << what << "\n";
    }
}

void testRoundTrip()
{
    const std::string text =
        "# comment\n\n[display]\n; another\nratio = 16:9\nfullscreen=false\n\n[rendering]\nscale = 4\n\n[env]\nRRV_X = 1\n";
    IniDocument doc = IniDocument::parse(text);
    expect(doc.text() == text, "unchanged document round-trips byte for byte");
    expect(doc.get("DISPLAY", "Ratio").value_or("") == "16:9", "case-insensitive get");
    expect(!doc.get("display", "hud").has_value(), "absent key");
    doc.set("display", "fullscreen", "true");
    expect(doc.text().find("fullscreen = true\n") != std::string::npos, "set replaces the existing line");
    expect(doc.text().find("# comment\n\n[display]\n; another\n") == 0, "comments kept");
    doc.set("display", "hud", "race");
    const std::string after = doc.text();
    expect(after.find("fullscreen = true\nhud = race\n") != std::string::npos, "set appends after the section's last key");
    doc.set("input", "analog", "false");
    expect(doc.text().find("\n[input]\nanalog = false\n") != std::string::npos, "set creates a missing section");
    const auto env = doc.entries("env");
    expect(env.size() == 1 && env[0].first == "RRV_X" && env[0].second == "1", "[env] entries keep their spelling");

    IniDocument later = IniDocument::parse("[game]\ncar_lod = 2\ncar_lod = 3\n");
    expect(later.get("game", "car_lod").value_or("") == "3", "a later line wins, as in the shell reader");
    later.set("game", "car_lod", "5");
    expect(later.text() == "[game]\ncar_lod = 2\ncar_lod = 5\n", "set updates the winning line");

    IniDocument noNewline = IniDocument::parse("[timing]\ninline = true");
    expect(noNewline.text() == "[timing]\ninline = true", "missing final newline preserved");

    const fs::path temp = fs::temp_directory_path() / ("fukami-settings-" + std::to_string(getpid()) + ".ini");
    expect(doc.save(temp), "save");
    const auto loaded = IniDocument::load(temp);
    expect(loaded && loaded->text() == doc.text(), "save/load round trip");
    fs::remove(temp);
    const auto missing = IniDocument::load(temp);
    expect(missing && missing->text().empty(), "missing file loads as empty");
}

void testValidation()
{
    const auto ok = [](const char *section, const char *key, const char *value) {
        return valid(*find(section, key), value);
    };
    expect(ok("display", "ratio", "21:9") && !ok("display", "ratio", "5:4"), "ratio");
    expect(ok("display", "hud", "no47") && !ok("display", "hud", "none"), "hud");
    expect(ok("display", "aspect", "") && ok("display", "aspect", "stretch") && !ok("display", "aspect", "wide"), "aspect");
    expect(ok("display", "window", "") && ok("display", "window", "1920x1080") && !ok("display", "window", "1920"), "window");
    expect(ok("display", "fullscreen", "YES") && ok("display", "fullscreen", "0") && !ok("display", "fullscreen", "maybe"), "bool");
    expect(ok("rendering", "scale", "8") && !ok("rendering", "scale", "9") && !ok("rendering", "scale", "10"), "scale");
    expect(ok("rendering", "cas", "100") && !ok("rendering", "cas", "101") && !ok("rendering", "cas", "-1"), "cas");
    expect(ok("rendering", "aniso", "16") && !ok("rendering", "aniso", "3"), "aniso");
    expect(ok("rendering", "texture_filter", "ps2") && ok("rendering", "texture_filter", "bilinear") &&
               !ok("rendering", "texture_filter", "nearest") && !ok("rendering", "texture_filter", ""),
           "texture_filter");
    expect(ok("game", "car_lod", "6.5") && !ok("game", "car_lod", "best") && !ok("game", "car_lod", "1."), "car_lod");
    expect(ok("game", "draw_distance", "0") && ok("game", "draw_distance", "16") &&
               !ok("game", "draw_distance", "17") && !ok("game", "draw_distance", "2.5"),
           "draw_distance");
    expect(ok("timing", "from", "") && ok("timing", "from", "600") && !ok("timing", "from", "-1"), "from");
    expect(ok("timing", "split_gs", "true") && ok("timing", "split_gs", "off") && !ok("timing", "split_gs", "2"), "split_gs");
    expect(ok("timing", "pacer_spin", "0") && ok("timing", "pacer_spin", "17") && !ok("timing", "pacer_spin", "18") &&
               !ok("timing", "pacer_spin", "-1") && !ok("timing", "pacer_spin", "on"),
           "pacer_spin");

    IniDocument doc = IniDocument::parse("[display]\nhud = no47\nratio = 5:4\n");
    expect(value(doc, *find("display", "hud")) == "stretch", "no47 reads as stretch");
    expect(value(doc, *find("display", "ratio")) == "16:9", "invalid value falls back to the default");

    const auto issues = check(IniDocument::parse("[display]\nratio = 5:4\nbogus = 1\nnot a line\n[env]\nlower = 1\n"), false);
    std::string all;
    for (const auto &issue : issues)
        all += std::to_string(issue.line) + ":" + issue.message + "\n";
    expect(all.find("2:ratio must be 4:3, 16:9, 16:10 or 21:9") != std::string::npos, "ratio message: " + all);
    expect(all.find("3:unknown setting 'bogus'") != std::string::npos, "unknown key: " + all);
    expect(all.find("4:expected key = value") != std::string::npos, "malformed line: " + all);
    expect(all.find("[env] names must look like RRV_NAME (got 'lower')") != std::string::npos, "env name: " + all);
    expect(check(IniDocument::parse("[paths]\nworkload = x\n"), false).empty(), "[paths] ignored by the app");
}

std::vector<std::string> defaultEnvironment(const IniDocument &doc, bool replay = false, bool session = true)
{
    LaunchPaths paths;
    paths.home = "/home/test";
    paths.path = "/usr/bin:/bin";
    paths.tmpdir = "/tmp/rrv-test";
    paths.libraries = "/R/runtime/x/lib";
    paths.boundWorkload = "/W/bound-workload.txt";
    paths.memoryCard = "/R/local/product/mc";
    if (session)
        paths.session = "/R/local/product/sessions/S";
    paths.recordPad = !replay;
    return environment(LaunchConfig::fromIni(doc), paths);
}

void testLaunchConfig()
{
    // What the bash launcher produced for the repository's rrv.ini defaults (tests/fixtures/launch_env/golden.json,
    // "empty ini"), which the app and ./run.sh must keep giving the runtime. Since 2026-10-04 (owner) split_gs is on by
    // default, which adds RRV_VU1GS_SPLIT=1 right after the owner-async line (the golden file records it too).
    const std::vector<std::string> oldDefaults = {
        "HOME=/home/test", "PATH=/usr/bin:/bin", "TMPDIR=/tmp/rrv-test", "DYLD_LIBRARY_PATH=/R/runtime/x/lib",
        "RRV_GATE3_BOUND_WORKLOAD=/W/bound-workload.txt", "RRV_GATE3_MC_ROOT=/R/local/product/mc",
        "RRV_GATE3_MC_PERSISTENT=1", "RRV_GATE3_RECORD_PAD=/R/local/product/sessions/S/pad.jsonl",
        "RRV_GATE3_REALTIME=1", "RRV_PCSX2_GS_VSYNC=1", "RRV_GS_EXECUTION=worker-sync",
        "RRV_VU1GS_EXECUTION=owner-async", "RRV_VU1GS_SPLIT=1", "RRV_GS_RENDER_MODE=full", "RRV_PCSX2_GS_FULL_SCALE=4",
        "RRV_RR5_CAR_LOD=4", "RRV_RR5_FAST_UNPACK=1", "RRV_RR5_NATIVE_HOT=1", "RRV_RR5_WIDESCREEN=16:9",
        "RRV_PCSX2_GS_ASPECT=16:9", "RRV_GATE4_START_CLOCK=/R/local/product/sessions/S/start-clock.txt",
    };
    expect(defaultEnvironment(IniDocument::parse("")) == oldDefaults, "default environment equals the old launcher's");

    // Typed values.
    const LaunchConfig c = LaunchConfig::fromIni(IniDocument::parse(
        "[display]\nratio = 21:9\nhud = race\n[rendering]\nscale = 2\naa1 = off\ncas = 35\n[game]\ncar_lod = 6.5\n"));
    expect(c.ratio == "21:9" && c.hud == "race" && c.scale == 2 && !c.aa1 && c.cas == 35 && c.carLod == "6.5" &&
               c.fastUnpack && !c.developer,
           "typed fields");

    // Flags map to the same settings the ini sets, and later wins.
    std::vector<Override> o;
    const std::vector<std::string> argv = {"--no-aa1", "--ratio", "4:3", "--widescreen", "--cas"};
    expect(flagOverride(argv, 0, o) == 1 && flagOverride(argv, 1, o) == 2 && flagOverride(argv, 3, o) == 1 &&
               flagOverride(argv, 4, o) == -1 && flagOverride({"--bogus"}, 0, o) == 0,
           "flag consumption");
    IniDocument doc = IniDocument::parse("[display]\nratio = 21:9\n");
    expect(applyOverrides(doc, o).empty() && value(doc, *find("display", "ratio")) == "16:9" &&
               value(doc, *find("rendering", "aa1")) == "false",
           "overrides apply in order");
    IniDocument bad;
    std::vector<Override> wrong = {{"display", "ratio", "5:4"}, {"display", "nope", "1"}};
    expect(applyOverrides(bad, wrong).size() == 2, "invalid overrides are reported, not applied");

    // Replay: recorded input, no pad log.
    const auto replay = defaultEnvironment(IniDocument::parse(""), true);
    bool pad = false;
    for (const auto &line : replay)
        pad = pad || line.rfind("RRV_GATE3_RECORD_PAD=", 0) == 0;
    expect(!pad, "replay has no pad log");

    // Logging off (no session folder): the pad is still admitted, with no file; no start clock, no log path.
    {
        const auto noLog = defaultEnvironment(IniDocument::parse(""), false, false);
        bool live = false, file = false, clock = false;
        for (const auto &line : noLog)
        {
            live = live || line == "RRV_GATE3_LIVE_PAD=1";
            file = file || line.rfind("RRV_GATE3_RECORD_PAD=", 0) == 0;
            clock = clock || line.rfind("RRV_GATE4_START_CLOCK=", 0) == 0;
        }
        expect(live && !file && !clock, "no session: live pad admitted, no pad file, no start clock");
        const auto logged = defaultEnvironment(IniDocument::parse(""));
        bool both = false;
        for (const auto &line : logged)
            both = both || line == "RRV_GATE3_LIVE_PAD=1";
        expect(!both, "with a session the pad goes through the log, not the live switch");
        const auto replayNoLog = defaultEnvironment(IniDocument::parse(""), true, false);
        bool any = false;
        for (const auto &line : replayNoLog)
            any = any || line.rfind("RRV_GATE3_LIVE_PAD", 0) == 0 || line.rfind("RRV_GATE3_RECORD_PAD", 0) == 0;
        expect(!any, "a replay admits no live pad");
    }

    // Gate 5: split_gs adds RRV_VU1GS_SPLIT=1 only with the owner stream (not inline), and is on by default.
    const auto hasLine = [](const std::vector<std::string> &env, const std::string &line) {
        for (const auto &l : env)
            if (l == line)
                return true;
        return false;
    };
    expect(hasLine(defaultEnvironment(IniDocument::parse("")), "RRV_VU1GS_SPLIT=1"), "split_gs is on by default");
    expect(!hasLine(defaultEnvironment(IniDocument::parse("[timing]\nsplit_gs = false\n")), "RRV_VU1GS_SPLIT=1"),
           "split_gs = false keeps one owner thread");
    expect(!hasLine(defaultEnvironment(IniDocument::parse("[timing]\nsplit_gs = true\ninline = true\n")), "RRV_VU1GS_SPLIT=1"),
           "inline ignores split_gs");

    // Gate 5: pacer_spin adds RRV_GATE3_REALTIME_SPIN_MS only when paced, and is off by default.
    const auto hasPrefix = [](const std::vector<std::string> &env, const std::string &prefix) {
        for (const auto &l : env)
            if (l.rfind(prefix, 0) == 0)
                return true;
        return false;
    };
    expect(!hasPrefix(defaultEnvironment(IniDocument::parse("")), "RRV_GATE3_REALTIME_SPIN_MS="), "pacer_spin is off by default");
    expect(hasLine(defaultEnvironment(IniDocument::parse("[timing]\npacer_spin = 4\n")), "RRV_GATE3_REALTIME_SPIN_MS=4"),
           "pacer_spin = 4 sets the spin");
    expect(!hasPrefix(defaultEnvironment(IniDocument::parse("[timing]\npacer_spin = 4\nunpaced = true\n")),
                      "RRV_GATE3_REALTIME_SPIN_MS="),
           "unpaced ignores pacer_spin");

    // texture_filter: the stock filter sends nothing (so the default environment above is unchanged); bilinear
    // names PCSX2's forced bilinear for the bridge, from the ini and from the flag.
    expect(!hasPrefix(defaultEnvironment(IniDocument::parse("[rendering]\ntexture_filter = ps2\n")),
                      "RRV_PCSX2_GS_TEXTURE_FILTER="),
           "texture_filter = ps2 sends nothing");
    expect(hasLine(defaultEnvironment(IniDocument::parse("[rendering]\ntexture_filter = bilinear\n")),
                   "RRV_PCSX2_GS_TEXTURE_FILTER=bilinear"),
           "texture_filter = bilinear reaches the bridge");
    {
        std::vector<Override> filter;
        IniDocument filtered;
        expect(flagOverride({"--texture-filter", "bilinear"}, 0, filter) == 2 && applyOverrides(filtered, filter).empty() &&
                   LaunchConfig::fromIni(filtered).textureFilter == "bilinear" &&
                   flagOverride({"--texture-filter"}, 0, filter) == -1,
               "--texture-filter sets the same setting");
        std::vector<Override> wrongFilter = {{"rendering", "texture_filter", "trilinear"}};
        expect(applyOverrides(filtered, wrongFilter).size() == 1, "an unknown texture filter is refused");
    }

    // The environment is only the transport: reading it back gives the same typed view, and the same environment.
    for (const char *text : {"", "[display]\nratio = 21:9\nhud = fixed\naspect = stretch\nfullscreen = true\n",
                             "[rendering]\nscale = 2\naa1 = no\nfxaa = yes\ncas = 50\naniso = 8\nmipmap = off\n",
                             "[game]\ncar_lod = 1\ndraw_distance = 3\nfast_unpack = no\nnative_code = no\n",
                             "[timing]\nheadless = true\nunpaced = true\ninline = true\n[input]\nanalog = no\nrumble = no\n",
                             "[timing]\nsplit_gs = true\n", "[timing]\npacer_spin = 17\n",
                             "[rendering]\ntexture_filter = bilinear\n"})
    {
        const auto env = defaultEnvironment(IniDocument::parse(text));
        const auto get = [&](const char *name) -> const char * {
            static std::string held;
            const std::string prefix = std::string(name) + "=";
            for (const auto &line : env)
                if (line.rfind(prefix, 0) == 0)
                {
                    held = line.substr(prefix.size());
                    return held.c_str();
                }
            return nullptr;
        };
        LaunchConfig back = LaunchConfig::fromEnvironment(get);
        LaunchPaths paths;
        paths.home = "/home/test"; paths.path = "/usr/bin:/bin"; paths.tmpdir = "/tmp/rrv-test";
        paths.libraries = "/R/runtime/x/lib"; paths.boundWorkload = "/W/bound-workload.txt";
        paths.memoryCard = "/R/local/product/mc"; paths.session = "/R/local/product/sessions/S";
        expect(environment(back, paths) == env, std::string("environment round trip for: ") + text);
    }
}

void testEnvPolicy()
{
    expect(envAllowed("RRV_PCSX2_GS_HUD_SCALE", false) && envAllowed("RRV_RR5_WIDESCREEN", false), "settings names are allowed");
    expect(!envAllowed("RRV_VU_AOT_RECORD", false) && !envAllowed("RRV_FRONTEND_DIAG", false) &&
               !envAllowed("RRV_GATE3_SEMANTIC_TRACE", false),
           "diagnostic, recording and qualification variables are not");
    expect(envAllowed("RRV_VU_AOT_RECORD", true) && !envAllowed("HOME", true) && !envAllowed("rrv_x", true),
           "developer mode lifts the allowlist but not the name rule");
    const std::string text = "[env]\nRRV_VU_AOT_RECORD = /tmp/x\nRRV_RR5_CAR_LOD = 2\n";
    const auto refused = check(IniDocument::parse(text), false);
    expect(refused.size() == 1 && refused[0].line == 2 &&
               refused[0].message.find("developer setting") != std::string::npos,
           "a developer variable is refused without developer mode");
    expect(check(IniDocument::parse(text + "[developer]\nenabled = true\n"), false).empty(), "developer = true allows it");
    expect(check(IniDocument::parse(text), false, true).empty(), "RRV_DEVELOPER=1 (override) allows it");
    // Defence in depth: a config built without check() still drops the variable.
    expect(LaunchConfig::fromIni(IniDocument::parse(text)).extraEnv.size() == 1, "fromIni drops developer variables");
    expect(LaunchConfig::fromIni(IniDocument::parse(text + "[developer]\nenabled = true\n")).extraEnv.size() == 2,
           "fromIni keeps them in developer mode");
    expect(!check(IniDocument::parse("[game]\nratio = 4:3\n"), false).empty() &&
               check(IniDocument::parse("[game]\nratio = 4:3\n"), false)[0].message.find("belongs under [display]") !=
                   std::string::npos,
           "a key in the wrong section is an error");
    expect(LaunchConfig::fromIni(IniDocument::parse("[timing]\nfrom = 100\nunpaced = true\n")).problem().find("--from needs") == 0,
           "--from with unpaced is a problem");
}

} // namespace

// The upgrade case (Steam Deck, 2026-10-02): a fukami.ini written by an app version that had no `split_gs` and no
// `pacer_spin`, and the Linux package's default ini that sets them. The missing keys take the package values, the
// keys the file has keep theirs, and the user's document is not changed.
void testPackageDefaults()
{
    const std::string oldUser =
        "# Fukami settings (0.3.5)\n[display]\nratio = 16:9\nfullscreen = true\n\n[rendering]\nscale = 4\n\n"
        "[timing]\nunpaced = false\ninline = false\nheadless = false\n\n[env]\n";
    const std::string packaged =
        "[display]\nratio = 4:3\nfullscreen = false\n\n[rendering]\nscale = 2\n\n"
        "[timing]\nunpaced = false\ninline = false\nsplit_gs = true\npacer_spin = 17\nheadless = false\n\n"
        "[env]\nRRV_PCSX2_GS_HUD_SCALE = 0.5\n";
    const IniDocument user = IniDocument::parse(oldUser);
    const IniDocument defaults = IniDocument::parse(packaged);
    LaunchPaths paths;
    paths.session = "/s";
    const auto has = [](const std::vector<std::string> &env, const std::string &line) {
        return std::find(env.begin(), env.end(), line) != env.end();
    };
    const auto hasName = [](const std::vector<std::string> &env, const std::string &name) {
        return std::any_of(env.begin(), env.end(), [&](const std::string &line) { return line.rfind(name + "=", 0) == 0; });
    };

    // What the schema alone gives: the split on, no spin (the package's pacer_spin = 17 here is a test value).
    const auto before = environment(user, paths);
    expect(has(before, "RRV_VU1GS_SPLIT=1") && !hasName(before, "RRV_GATE3_REALTIME_SPIN_MS"),
           "schema defaults alone: an old ini has the split on and no spin");

    const auto taken = inherited(user, defaults);
    expect(taken.size() == 2 && taken[0].section == "timing" && taken[0].key == "split_gs" && taken[0].value == "true" &&
               taken[1].key == "pacer_spin" && taken[1].value == "17",
           "inherited(): exactly the two settings the old ini does not have");
    const IniDocument effective = withDefaults(user, defaults);
    const auto env = environment(effective, paths);
    expect(has(env, "RRV_VU1GS_SPLIT=1"), "old ini + package default: the VU1/GS split is on");
    expect(has(env, "RRV_GATE3_REALTIME_SPIN_MS=17"), "old ini + package default: the pacer spins 17 ms");
    expect(has(env, "RRV_PCSX2_GS_FULL_SCALE=4") && has(env, "RRV_RR5_WIDESCREEN=16:9") && has(env, "RRV_WINDOW_FULLSCREEN=1"),
           "keys the user's file has keep the user's value, not the package's");
    expect(!hasName(env, "RRV_PCSX2_GS_HUD_SCALE"), "[env] lines of the default ini are not inherited");
    expect(user.text() == oldUser, "the user's document is unchanged");
    expect(check(effective, false).empty(), "the effective document is valid");

    // A key the user set wins, also when it equals the schema default.
    const IniDocument optedOut = IniDocument::parse(oldUser + "[timing]\nsplit_gs = false\npacer_spin = 0\n");
    expect(inherited(optedOut, defaults).empty(), "nothing inherited when the file has the keys");
    const auto optedOutEnv = environment(withDefaults(optedOut, defaults), paths);
    expect(!hasName(optedOutEnv, "RRV_VU1GS_SPLIT") && !hasName(optedOutEnv, "RRV_GATE3_REALTIME_SPIN_MS"),
           "split_gs = false and pacer_spin = 0 in the user's file stay off");

    // A current file (a copy of the default ini) is unaffected: the layer adds nothing.
    expect(withDefaults(defaults, defaults).text() == defaults.text(), "a file with every key is returned as it is");
    // No default ini (./run.sh, or the file is missing): the schema defaults, as before.
    expect(withDefaults(user, IniDocument{}).text() == oldUser, "an empty default document changes nothing");
    // A value the schema refuses is not inherited; the schema default stands.
    const IniDocument badDefaults = IniDocument::parse("[timing]\nsplit_gs = maybe\npacer_spin = 99\n");
    expect(inherited(user, badDefaults).empty(), "an invalid package value is ignored");
    // A key that is only commented out counts as absent.
    const IniDocument commented = IniDocument::parse("[timing]\n# split_gs = false\n");
    expect(LaunchConfig::fromIni(withDefaults(commented, defaults)).splitGs, "a commented-out key is absent");

    // The developer tool: --defaults FILE, and the app's own default when none is given.
    const fs::path dir = fs::temp_directory_path() / ("fukami-defaults-" + std::to_string(getpid()));
    fs::create_directories(dir);
    const fs::path userPath = dir / "fukami.ini", defaultsPath = dir / "fukami-default.ini", outPath = dir / "out.txt";
    std::ofstream(userPath) << oldUser;
    std::ofstream(defaultsPath) << packaged;
    const auto tool = [&](std::vector<std::string> args, const fs::path &appDefaults) {
        std::vector<char *> argv;
        for (auto &arg : args)
            argv.push_back(arg.data());
        std::fflush(stdout);
        const int saved = dup(STDOUT_FILENO);
        std::FILE *out = std::fopen(outPath.c_str(), "w");
        dup2(fileno(out), STDOUT_FILENO);
        const int rc = launchEnvMain(static_cast<int>(argv.size()), argv.data(), appDefaults);
        std::fflush(stdout);
        dup2(saved, STDOUT_FILENO);
        close(saved);
        std::fclose(out);
        std::ifstream in(outPath);
        std::vector<std::string> lines;
        for (std::string line; std::getline(in, line);)
            lines.push_back(line);
        return std::make_pair(rc, lines);
    };
    auto [rcPlain, plain] = tool({"tool", "--print-launch-env", userPath.string()}, {});
    expect(rcPlain == 0 && has(plain, "RRV_VU1GS_SPLIT=1") && !hasName(plain, "RRV_GATE3_REALTIME_SPIN_MS"),
           "--print-launch-env without defaults: schema defaults (the developer launcher is unchanged)");
    auto [rcFlag, flagged] = tool({"tool", "--print-launch-env", userPath.string(), "--defaults", defaultsPath.string()}, {});
    expect(rcFlag == 0 && has(flagged, "RRV_VU1GS_SPLIT=1") && has(flagged, "RRV_GATE3_REALTIME_SPIN_MS=17") &&
               has(flagged, "RRV_PCSX2_GS_FULL_SCALE=4"),
           "--print-launch-env --defaults FILE: the package values for the missing keys");
    auto [rcApp, fromApp] = tool({"tool", "--print-launch-env", userPath.string()}, defaultsPath);
    expect(rcApp == 0 && fromApp == flagged, "the app's binary uses its bundled default ini without the option");
    auto [rcSet, overridden] = tool({"tool", "--print-launch-env", userPath.string(), "--set", "timing.split_gs=false"},
                                    defaultsPath);
    expect(rcSet == 0 && !hasName(overridden, "RRV_VU1GS_SPLIT") && has(overridden, "RRV_GATE3_REALTIME_SPIN_MS=17"),
           "an override still wins over a package default");
    std::ifstream reread(userPath);
    std::ostringstream text;
    text << reread.rdbuf();
    expect(text.str() == oldUser, "the user's file on disk is not rewritten");
    fs::remove_all(dir);
}

int main()
{
    testRoundTrip();
    testValidation();
    testLaunchConfig();
    testEnvPolicy();
    testPackageDefaults();
    if (failures)
    {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "fukami settings: all tests passed\n";
    return 0;
}
