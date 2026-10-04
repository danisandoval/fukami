// Fukami in-game menu (src/app/fukami_menu.cpp) against a fake bridge UI
// table: no UI work while closed, every menu setting drawn, changes saved to
// the settings file with its comments kept, live options applied, restart-only
// options flagged, Esc/B backing out of a combo before the menu, fullscreen
// persisted only when it differs.
#include "fukami_app.h"
#include "fukami_menu.h"
#include "fukami_settings.h"

#include "../tools/pcsx2-gs-bridge/rrv_pcsx2_gs_bridge.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

// ---- fukami::app stand-ins (fukami_app.mm is the real one) -----------------
namespace {
bool g_canRestart = false;
fs::path g_saves;
fs::path g_logs;
std::vector<fs::path> g_revealed;
} // namespace

namespace fukami::app {
bool inApp() { return g_canRestart; }
std::filesystem::path iniPath() { return {}; }
std::filesystem::path defaultIniPath() { return {}; }
bool canRestart() { return g_canRestart; }
void restart() {}
std::filesystem::path savesFolder() { return g_saves; }
std::filesystem::path logsFolder() { return g_logs; }
void reveal(const std::filesystem::path &path) { g_revealed.push_back(path); }
} // namespace fukami::app

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

// ---- fake UI -----------------------------------------------------------------
std::vector<std::string> g_calls;       // every UI call, in order
std::map<std::string, int> g_intSet;    // label -> value a widget reports as changed
std::map<std::string, float> g_floatSet;
std::string g_press;                    // button label to report as activated
bool g_popup = false;

void record(const std::string &call) { g_calls.push_back(call); }
bool called(const std::string &prefix)
{
    for (const auto &call : g_calls)
        if (call.rfind(prefix, 0) == 0)
            return true;
    return false;
}
int count(const std::string &prefix)
{
    int n = 0;
    for (const auto &call : g_calls)
        n += call.rfind(prefix, 0) == 0 ? 1 : 0;
    return n;
}
std::string strip(const char *label)
{
    std::string text(label ? label : "");
    if (text.size() > 2 && text.compare(text.size() - 2, 2, " *") == 0)
        text.resize(text.size() - 2);
    return text;
}
int takeInt(const char *label, int *value)
{
    const auto it = g_intSet.find(strip(label));
    if (it == g_intSet.end())
        return 0;
    *value = it->second;
    g_intSet.erase(it);
    return 1;
}

RrvPcsx2GsBridgeUi fakeUi()
{
    RrvPcsx2GsBridgeUi ui{};
    ui.struct_size = sizeof(ui);
    ui.display_width = 1920.0f;
    ui.display_height = 1080.0f;
    ui.scale = 2.0f;
    ui.add_key = [](uint32_t key, int down) { record("key " + std::to_string(key) + " " + std::to_string(down)); };
    ui.add_mouse_position = [](float, float) { record("mouse"); };
    ui.add_mouse_button = [](uint32_t, int) { record("mouse-button"); };
    ui.add_mouse_wheel = [](float, float) { record("wheel"); };
    ui.clear_input = [] { record("clear"); };
    ui.consumes_cancel = [] { return g_popup ? 1 : 0; };
    ui.begin_menu = [](const char *title, int appearing) {
        record(std::string("begin ") + title + " " + std::to_string(appearing));
        return 1;
    };
    ui.end_menu = [] { record("end"); };
    ui.section = [](const char *label) { record(std::string("section ") + label); };
    ui.text = [](const char *text) { record(std::string("text ") + text); };
    ui.text_disabled = [](const char *text) { record(std::string("disabled-text ") + text); };
    ui.text_wrapped = [](const char *text) { record(std::string("wrapped ") + text); };
    ui.text_warning = [](const char *text) { record(std::string("warning ") + text); };
    ui.separator = [] {};
    ui.same_line = [] {};
    ui.spacing = [] {};
    ui.push_id = [](int) {};
    ui.pop_id = [] {};
    ui.begin_disabled = [](int disabled) { record("disabled " + std::to_string(disabled)); };
    ui.end_disabled = [] {};
    ui.tooltip = [](const char *) {};
    ui.default_focus = [] { record("default-focus"); };
    ui.checkbox = [](const char *label, int *value) {
        record("checkbox " + strip(label) + " " + std::to_string(*value));
        return takeInt(label, value);
    };
    ui.combo = [](const char *label, int *index, const char *const *items, int count) {
        record("combo " + strip(label) + " " + items[*index] + " of " + std::to_string(count));
        return takeInt(label, index);
    };
    ui.slider_int = [](const char *label, int *value, int min, int max) {
        record("slider " + strip(label) + " " + std::to_string(*value) + " " + std::to_string(min) + ".." +
               std::to_string(max));
        return takeInt(label, value);
    };
    ui.slider_float = [](const char *label, float *value, float, float, const char *) {
        record("slider-float " + strip(label));
        const auto it = g_floatSet.find(strip(label));
        if (it == g_floatSet.end())
            return 0;
        *value = it->second;
        g_floatSet.erase(it);
        return 1;
    };
    ui.button = [](const char *label) {
        record(std::string("button ") + label);
        if (g_press == label)
        {
            g_press.clear();
            return 1;
        }
        return 0;
    };
    ui.set_aspect = [](const char *value) {
        record(std::string("set-aspect ") + value);
        return 1;
    };
    ui.set_integer_scaling = [](int on) { record("set-integer " + std::to_string(on)); };
    ui.set_fxaa = [](int on) { record("set-fxaa " + std::to_string(on)); };
    ui.set_cas = [](int value) {
        record("set-cas " + std::to_string(value));
        return 1;
    };
    return ui;
}

std::string readFile(const fs::path &path)
{
    std::ifstream in(path);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

void frame(fukami::menu::Menu &menu)
{
    g_calls.clear();
    const RrvPcsx2GsBridgeUi ui = fakeUi();
    fukami::menu::Menu::frameCallback(&menu, &ui);
}

fukami::menu::Input key(uint32_t code, bool down)
{
    fukami::menu::Input input;
    input.code = code;
    input.down = down;
    return input;
}

const std::string kIni =
    "# Fukami settings\n"
    "[display]\n"
    "; picture\n"
    "ratio = 16:9\n"
    "fullscreen = false\n"
    "\n"
    "[rendering]\n"
    "scale = 4\n"
    "fxaa = false\n"
    "\n"
    "[env]\n"
    "RRV_CUSTOM = 1\n";

void testClosedMenuDoesNothing(const fs::path &ini)
{
    fukami::menu::Menu menu(ini);
    frame(menu);
    expect(g_calls.empty(), "no UI calls while closed and never opened");
    menu.open();
    expect(!menu.isOpen(), "the menu cannot open before the overlay is available");
    frame(menu);
    expect(g_calls.empty(), "still no UI calls");
    menu.post(key(RRV_PCSX2_GS_UI_KEY_DOWN, true));
    menu.setAvailable(true);
    menu.open();
    frame(menu);
    expect(!called("key "), "input posted while closed is dropped");
    menu.close();
    frame(menu);
    expect(g_calls == std::vector<std::string>{"clear"}, "closing releases ImGui input once");
    frame(menu);
    expect(g_calls.empty(), "and then nothing");
}

void testDrawsEveryMenuSetting(const fs::path &ini)
{
    fukami::menu::Menu menu(ini);
    menu.setAvailable(true);
    menu.open();
    frame(menu);
    expect(called("clear") && called("begin Fukami 1") && called("default-focus") && called("end"),
           "first open frame clears input, appears, focuses Resume");
    int inMenu = 0;
    for (const auto &spec : fukami::settings::schema())
    {
        if (!spec.inMenu)
            continue;
        ++inMenu;
        expect(called("checkbox " + std::string(spec.label)) || called("combo " + std::string(spec.label)) ||
                   called("slider " + std::string(spec.label)) || called("slider-float " + std::string(spec.label)),
               "row drawn for " + std::string(spec.label));
    }
    expect(count("checkbox ") + count("combo ") + count("slider ") + count("slider-float ") == inMenu,
           "exactly one row per menu setting");
    for (const char *section : {"section Display", "section Rendering", "section Input", "section Game",
                                "section Performance"})
        expect(called(section), std::string("section drawn: ") + section);
    expect(called("slider Internal resolution 4 1..8"), "integer slider shows the file's value and range");
    expect(called("combo Widescreen 16:9 of 4"), "combo shows the file's choice");
    expect(called("checkbox Split VU1 / GS 1") && called("slider Pacer spin (ms) 0 0..17"),
           "the VU1/GS split and the pacer spin are menu settings (schema defaults without a default ini)");
    for (const char *hidden : {"Unpaced", "Inline", "Headless", "Developer mode"})
        expect(!called(std::string("checkbox ") + hidden), std::string("ini only: ") + hidden);
    expect(called("wrapped Settings: " + ini.string()), "settings file path shown");
    expect(!called("warning Restart"), "nothing pending at open");
    expect(!called("section Files"), "no Files section without folders");
    frame(menu);
    expect(called("begin Fukami 0"), "later frames are not appearing");
}

void testLiveChangeSavesAndApplies(const fs::path &ini)
{
    fukami::menu::Menu menu(ini);
    menu.setAvailable(true);
    menu.open();
    frame(menu);
    g_intSet["FXAA"] = 1;
    frame(menu);
    expect(called("set-fxaa 1"), "FXAA applied live");
    const std::string text = readFile(ini);
    expect(text.find("fxaa = true\n") != std::string::npos, "FXAA saved");
    expect(text.find("# Fukami settings\n[display]\n; picture\n") == 0 && text.find("RRV_CUSTOM = 1") != std::string::npos,
           "comments and unknown sections kept");
    frame(menu);
    expect(!called("warning Restart"), "a live setting needs no restart");

    // Picture fit "" (index 0) follows the ratio the game was launched with.
    g_intSet["Picture fit"] = 0;
    frame(menu);
    expect(called("set-aspect 4:3"), "empty picture fit applies the launch ratio (4:3 without RRV_RR5_WIDESCREEN)");
    g_intSet["Picture fit"] = 6; // stretch
    frame(menu);
    expect(called("set-aspect stretch"), "picture fit applied live");
    expect(readFile(ini).find("aspect = stretch\n") != std::string::npos, "picture fit saved in [display]");
    g_intSet["Sharpening (CAS)"] = 40;
    frame(menu);
    expect(called("set-cas 40") && readFile(ini).find("cas = 40\n") != std::string::npos, "CAS applied and saved");
}

void testRestartOnlyChange(const fs::path &ini)
{
    fukami::menu::Menu menu(ini);
    menu.setAvailable(true);
    menu.open();
    frame(menu);
    g_intSet["Internal resolution"] = 6;
    frame(menu);
    expect(readFile(ini).find("scale = 6\n") != std::string::npos, "restart-only setting saved");
    frame(menu);
    expect(called("warning Restart to apply: Internal resolution."), "restart banner names the setting");
    expect(called("disabled-text Quit and relaunch to apply."), "no restart outside the app");
    expect(!called("button Restart"), "no Restart button outside the app");
    g_canRestart = true;
    g_press = "Restart";
    frame(menu);
    expect(called("button Restart") && menu.takeRestartRequest(), "Restart button requests a restart in the app");
    g_canRestart = false;
    g_intSet["Internal resolution"] = 4;
    frame(menu);
    frame(menu);
    expect(!called("warning Restart"), "back to the launch value: nothing pending");

    g_floatSet["Car detail distance"] = 2.46f;
    frame(menu);
    expect(readFile(ini).find("car_lod = 2.5\n") != std::string::npos, "number saved with one decimal");
    g_floatSet["Car detail distance"] = 4.0f;
    frame(menu);
    expect(readFile(ini).find("car_lod = 4\n") != std::string::npos, "whole number saved without .0");
}

void testBackKeys(const fs::path &ini)
{
    fukami::menu::Menu menu(ini);
    menu.setAvailable(true);
    menu.open();
    frame(menu);
    menu.post(key(RRV_PCSX2_GS_UI_KEY_DOWN, true));
    g_popup = true;
    menu.post(key(RRV_PCSX2_GS_UI_KEY_ESCAPE, true));
    frame(menu);
    expect(called("key " + std::to_string(RRV_PCSX2_GS_UI_KEY_DOWN) + " 1"), "navigation key forwarded");
    expect(called("key " + std::to_string(RRV_PCSX2_GS_UI_KEY_ESCAPE) + " 1") && menu.isOpen(),
           "Esc with a combo open goes to ImGui");
    g_popup = false;
    menu.post(key(RRV_PCSX2_GS_UI_KEY_GAMEPAD_FACE_RIGHT, true));
    frame(menu);
    expect(!menu.isOpen() && !called("begin "), "B with no popup closes the menu");
    expect(called("clear"), "closing clears ImGui input");

    menu.open();
    frame(menu);
    g_press = "Resume";
    frame(menu);
    expect(!menu.isOpen(), "Resume closes");
    menu.open();
    frame(menu);
    g_press = "Quit";
    frame(menu);
    expect(menu.takeQuitRequest() && !menu.takeQuitRequest(), "Quit requests one clean stop");
}

void testFullscreen(const fs::path &ini)
{
    fukami::menu::Menu menu(ini);
    menu.setAvailable(true);
    menu.fullscreenChanged(false, false);
    menu.open();
    frame(menu);
    expect(called("checkbox Fullscreen 0"), "fullscreen checkbox shows the window state");
    g_intSet["Fullscreen"] = 1;
    frame(menu);
    expect(menu.takeFullscreenRequest() == 1 && menu.takeFullscreenRequest() == -1, "menu requests fullscreen from the host");
    expect(readFile(ini).find("fullscreen = false\n") != std::string::npos, "the menu itself does not write fullscreen");
    menu.fullscreenChanged(true, true);
    expect(readFile(ini).find("fullscreen = true\n") != std::string::npos, "a real change is saved");
    const auto stamp = fs::last_write_time(ini);
    std::string before = readFile(ini);
    menu.fullscreenChanged(true, true);
    expect(readFile(ini) == before && fs::last_write_time(ini) == stamp, "an unchanged state is not rewritten");
    frame(menu);
    expect(called("checkbox Fullscreen 1"), "checkbox follows the window");
}

// A setting the file does not have (written by an older app version) shows the app's bundled default, as the
// launcher used it; opening the menu does not write the file, and a change saves only that setting.
void testPackageDefaults(const fs::path &ini)
{
    fs::path defaults = ini;
    defaults += ".default";
    std::ofstream(defaults, std::ios::trunc)
        << "[display]\nratio = 4:3\n[rendering]\nscale = 2\naniso = 8\n[timing]\nsplit_gs = true\npacer_spin = 17\n";
    fukami::menu::Menu menu(ini, defaults);
    menu.setAvailable(true);
    menu.open();
    frame(menu);
    expect(called("combo Anisotropic filtering 8x of 5"), "a setting missing from the file shows the app default");
    expect(called("checkbox Split VU1 / GS 1") && called("slider Pacer spin (ms) 17 0..17"),
           "the Linux package case: an old file without split_gs/pacer_spin shows the split on and spin 17");
    expect(called("slider Internal resolution 4 1..8") && called("combo Widescreen 16:9 of 4"),
           "settings the file has keep the file's value");
    expect(!called("warning Restart"), "an inherited default is not a pending change");
    expect(readFile(ini) == kIni, "showing the menu does not rewrite the file");
    g_intSet["Anisotropic filtering"] = 0;
    frame(menu);
    const std::string text = readFile(ini);
    expect(text.find("aniso = 0\n") != std::string::npos && text.find("scale = 4\n") != std::string::npos &&
               text.find("ratio = 16:9\n") != std::string::npos,
           "a change saves that setting only; the others keep the file's values");
    frame(menu);
    expect(called("combo Anisotropic filtering Off of 5") && called("warning Restart to apply: Anisotropic filtering."),
           "the user's value now wins over the app default and is pending against the launch value");
    g_intSet["Split VU1 / GS"] = 0;
    frame(menu);
    expect(readFile(ini).find("\n[timing]\nsplit_gs = false\n") != std::string::npos,
           "switching the split off in the menu writes split_gs = false (it then wins over the app default)");
    expect(readFile(ini).find("pacer_spin") == std::string::npos, "the untouched setting is still not written");
    frame(menu);
    expect(called("checkbox Split VU1 / GS 0") && called("slider Pacer spin (ms) 17 0..17"),
           "the split shows off, the spin still follows the app default");
    fs::remove(defaults);
}

void testFilesAndNoIni()
{
    g_saves = "/tmp/fukami-saves";
    g_logs = "/tmp/fukami-logs";
    fukami::menu::Menu menu{fs::path()};
    menu.setAvailable(true);
    menu.open();
    frame(menu);
    expect(called("section Files") && called("button Show saves") && called("button Show logs"), "Files buttons shown");
    expect(called("wrapped No settings file"), "no-file note");
    expect(called("disabled 1"), "restart-only rows are read-only without a file");
    g_press = "Show saves";
    frame(menu);
    expect(g_revealed.size() == 1 && g_revealed[0] == g_saves, "Show saves reveals the card folder");
    g_intSet["FXAA"] = 1;
    frame(menu);
    expect(called("set-fxaa 1"), "live settings still apply without a file");
    g_saves.clear();
    g_logs.clear();
}

} // namespace

int main()
{
    ::unsetenv("RRV_RR5_WIDESCREEN");
    const fs::path ini = fs::temp_directory_path() / ("fukami-menu-" + std::to_string(getpid()) + ".ini");
    const auto reset = [&] {
        std::ofstream(ini, std::ios::trunc) << kIni;
    };
    reset();
    testClosedMenuDoesNothing(ini);
    reset();
    testDrawsEveryMenuSetting(ini);
    reset();
    testLiveChangeSavesAndApplies(ini);
    reset();
    testRestartOnlyChange(ini);
    reset();
    testBackKeys(ini);
    reset();
    testFullscreen(ini);
    reset();
    testPackageDefaults(ini);
    testFilesAndNoIni();
    fs::remove(ini);
    if (failures)
    {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "fukami menu tests passed\n";
    return 0;
}
