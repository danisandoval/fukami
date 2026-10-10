// Fukami in-game menu; see fukami_menu.h.
#include "fukami_menu.h"

#include "fukami_app.h"
#include "fukami_settings.h"

// The bridge ABI header is plain C with no PCSX2 types; the host only uses its
// UI table and key codes (the bridge itself is loaded at run time).
#include "../../tools/pcsx2-gs-bridge/rrv_pcsx2_gs_bridge.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>
#if defined(__APPLE__)
#define RRV_FILE_MANAGER_NAME "Finder"
#else
#define RRV_FILE_MANAGER_NAME "the file manager"
#endif


namespace fukami::menu {
namespace {

std::string id(std::string_view section, std::string_view key)
{
    return std::string(section) + "." + std::string(key);
}

// [timing] holds the host speed switches the menu shows (split_gs, pacer_spin); its other keys are ini only.
constexpr std::string_view kSections[] = {"display", "rendering", "input", "game", "timing"};

const char *sectionTitle(std::string_view section)
{
    if (section == "display") return "Display";
    if (section == "rendering") return "Rendering";
    if (section == "input") return "Input";
    if (section == "game") return "Game";
    if (section == "timing") return "Performance";
    return "Other";
}

std::string choiceName(const settings::Spec &spec, std::string_view choice)
{
    if (spec.key == "aspect" && choice.empty())
        return "Same as Widescreen";
    if (choice.empty())
        return "Automatic";
    if (spec.key == "aniso")
        return choice == "0" ? std::string("Off") : std::string(choice) + "x";
    if (spec.key == "texture_filter")
        return choice == "ps2" ? "PS2 (stock)" : "Bilinear";
    if (spec.key == "render_mode")
        return choice == "full" ? "Full frames" : "Fields";
    std::string name(choice);
    if (name[0] >= 'a' && name[0] <= 'z')
        name[0] = static_cast<char>(name[0] - 'a' + 'A');
    return name;
}

long toLong(const std::string &value, long fallback)
{
    char *end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    return end && end != value.c_str() && *end == '\0' ? parsed : fallback;
}

double toDouble(const std::string &value, double fallback)
{
    char *end = nullptr;
    const double parsed = std::strtod(value.c_str(), &end);
    return end && end != value.c_str() && *end == '\0' ? parsed : fallback;
}

// One decimal, without a trailing ".0" (car_lod: [0-9]+([.][0-9]+)?).
std::string formatNumber(double value)
{
    char text[32];
    std::snprintf(text, sizeof text, "%.1f", std::round(value * 10.0) / 10.0);
    std::string out(text);
    if (out.size() > 2 && out.compare(out.size() - 2, 2, ".0") == 0)
        out.resize(out.size() - 2);
    return out;
}

bool isBackKey(const Input &input)
{
    return input.type == Input::Type::key && input.down &&
           (input.code == RRV_PCSX2_GS_UI_KEY_ESCAPE || input.code == RRV_PCSX2_GS_UI_KEY_GAMEPAD_FACE_RIGHT);
}

} // namespace

bool appliesLive(std::string_view section, std::string_view key)
{
    // fullscreen: the host toggles the window. The others are presentation
    // options the bridge changes between two fields (RrvPcsx2GsBridgeUi set_*).
    return (section == "display" && (key == "fullscreen" || key == "aspect" || key == "integer_scaling")) ||
           (section == "rendering" && (key == "fxaa" || key == "cas"));
}

Menu::Menu(std::filesystem::path ini, const std::filesystem::path &defaults) :
    m_ini(std::move(ini)), m_saves(app::savesFolder()), m_logs(app::logsFolder())
{
    std::optional<settings::IniDocument> doc;
    if (!m_ini.empty())
        doc = settings::IniDocument::load(m_ini);
    if (!m_ini.empty() && !defaults.empty())
        m_defaults = settings::IniDocument::load(defaults).value_or(settings::IniDocument{});
    const settings::IniDocument shown = settings::withDefaults(doc ? *doc : settings::IniDocument{}, m_defaults);
    for (const settings::Spec &spec : settings::schema())
        m_launch[id(spec.section, spec.key)] = settings::value(shown, spec);
    m_values = m_launch;
    // The picture fit that an empty `aspect` stands for is the ratio this
    // process was launched with (run_gate4_product.sh: aspect defaults to it;
    // RRV_RR5_WIDESCREEN is set only for ratios other than 4:3).
    const char *ratio = std::getenv("RRV_RR5_WIDESCREEN");
    m_launchRatio = ratio && *ratio ? ratio : "4:3";
    if (!m_ini.empty() && !doc)
        m_status = "Could not read the settings file.";
}

void Menu::open()
{
    if (available() && !m_open.exchange(true, std::memory_order_acq_rel))
        std::fprintf(stderr, "[fukami-menu] open\n");
}

void Menu::close()
{
    if (m_open.exchange(false, std::memory_order_acq_rel))
        std::fprintf(stderr, "[fukami-menu] closed\n");
    std::lock_guard<std::mutex> lock(m_inputMutex);
    m_inputs.clear();
}

void Menu::post(const Input &input)
{
    if (!isOpen())
        return;
    std::lock_guard<std::mutex> lock(m_inputMutex);
    // A stalled GS owner must not grow this without bound; keep the newest.
    if (m_inputs.size() >= 256)
        m_inputs.pop_front();
    m_inputs.push_back(input);
}

void Menu::fullscreenChanged(bool fullscreen, bool persist)
{
    m_fullscreen.store(fullscreen, std::memory_order_release);
    if (!persist || m_ini.empty())
        return;
    std::lock_guard<std::mutex> lock(m_saveMutex);
    auto doc = settings::IniDocument::load(m_ini);
    if (!doc)
    {
        m_status = "Could not read the settings file.";
        return;
    }
    const auto current = doc->get("display", "fullscreen");
    if (current && settings::boolValue(*current) == fullscreen)
        return;
    doc->set("display", "fullscreen", fullscreen ? "true" : "false");
    if (!doc->save(m_ini))
        m_status = "Could not save the settings file.";
    else
        std::fprintf(stderr, "[fukami-menu] saved display.fullscreen=%s\n", fullscreen ? "true" : "false");
}

void Menu::frameCallback(void *menu, const RrvPcsx2GsBridgeUi *ui)
{
    if (!menu || !ui || ui->struct_size < sizeof(RrvPcsx2GsBridgeUi))
        return;
    // Never unwind into the bridge (built without exceptions).
    try
    {
        static_cast<Menu *>(menu)->frame(*ui);
    }
    catch (...)
    {
        std::fprintf(stderr, "[fukami-menu] frame failed; menu closed\n");
        static_cast<Menu *>(menu)->close();
    }
}

void Menu::reload()
{
    if (m_ini.empty())
        return; // nothing to read; keep this session's values
    const auto doc = settings::IniDocument::load(m_ini);
    if (!doc)
    {
        setStatus("Could not read the settings file.");
        return;
    }
    // What the next launch would use: a setting the file does not have follows the app's default ini.
    const settings::IniDocument shown = settings::withDefaults(*doc, m_defaults);
    for (const settings::Spec &spec : settings::schema())
        m_values[id(spec.section, spec.key)] = settings::value(shown, spec);
}

void Menu::frame(const RrvPcsx2GsBridgeUi &ui)
{
    std::deque<Input> inputs;
    {
        std::lock_guard<std::mutex> lock(m_inputMutex);
        inputs.swap(m_inputs);
    }
    if (!isOpen())
    {
        if (m_wasOpen)
        {
            // Release anything ImGui still holds down from the menu session.
            m_wasOpen = false;
            ui.clear_input();
        }
        return;
    }
    const bool appearing = !m_wasOpen;
    m_wasOpen = true;
    if (appearing)
    {
        ui.clear_input();
        reload();
    }
    for (const Input &input : inputs)
    {
        // Esc / B back out: close an open combo (or finish a slider) first,
        // else the menu.
        if (isBackKey(input) && !ui.consumes_cancel())
        {
            close();
            break;
        }
        switch (input.type)
        {
        case Input::Type::key: ui.add_key(input.code, input.down ? 1 : 0); break;
        case Input::Type::mousePosition: ui.add_mouse_position(input.x, input.y); break;
        case Input::Type::mouseButton: ui.add_mouse_button(input.code, input.down ? 1 : 0); break;
        case Input::Type::mouseWheel: ui.add_mouse_wheel(input.x, input.y); break;
        }
    }
    if (!isOpen())
    {
        m_wasOpen = false;
        ui.clear_input();
        return;
    }

    if (ui.begin_menu("Fukami", appearing ? 1 : 0))
    {
        if (ui.button("Resume"))
            close();
        if (appearing)
            ui.default_focus();
        ui.tooltip("Back to the game (Esc, F1 or the controller's Home button).");
        ui.same_line();
        if (ui.button("Quit"))
            m_quitRequest.store(true);
        ui.tooltip("Quit the game. Your save card is kept.");

        std::vector<std::string> pending;
        for (const settings::Spec &spec : settings::schema())
            if (spec.inMenu && pendingRestart(spec))
                pending.emplace_back(spec.label);
        if (!pending.empty())
        {
            std::string names;
            for (const std::string &name : pending)
                names += (names.empty() ? "" : ", ") + name;
            ui.spacing();
            ui.text_warning(("Restart to apply: " + names + ".").c_str());
            if (app::canRestart())
            {
                if (ui.button("Restart"))
                    m_restartRequest.store(true);
                ui.tooltip("Quit and start again with the new settings.");
            }
            else
                ui.text_disabled("Quit and relaunch to apply.");
        }

        int row = 0;
        for (std::string_view section : kSections)
        {
            ui.section(sectionTitle(section));
            for (const settings::Spec &spec : settings::schema())
                if (spec.inMenu && spec.section == section)
                    drawRow(ui, spec, row++);
        }
        if (!pending.empty())
            ui.text_disabled("* restart to apply");

        if (!m_saves.empty() || !m_logs.empty())
        {
            ui.section("Files");
            if (!m_saves.empty())
            {
                if (ui.button("Show saves"))
                    app::reveal(m_saves);
                ui.tooltip("Show the save card folder in " RRV_FILE_MANAGER_NAME ".");
            }
            if (!m_logs.empty())
            {
                if (!m_saves.empty())
                    ui.same_line();
                if (ui.button("Show logs"))
                    app::reveal(m_logs);
                ui.tooltip("Show this session's log folder in " RRV_FILE_MANAGER_NAME ".");
            }
        }
        ui.spacing();
        ui.separator();
        if (m_ini.empty())
            ui.text_wrapped("No settings file: changes last until you quit.");
        else
            ui.text_wrapped(("Settings: " + m_ini.string()).c_str());
        const std::string line = status();
        if (!line.empty())
            ui.text_warning(line.c_str());
    }
    ui.end_menu();
}

void Menu::drawRow(const RrvPcsx2GsBridgeUi &ui, const settings::Spec &spec, int rowId)
{
    const std::string key = id(spec.section, spec.key);
    const std::string value = spec.key == "fullscreen" && spec.section == "display"
        ? (m_fullscreen.load(std::memory_order_acquire) ? "true" : "false")
        : m_values[key];
    const bool live = appliesLive(spec.section, spec.key);
    // Without a settings file a restart-only change could never apply.
    const bool readOnly = !live && m_ini.empty();
    const std::string label = std::string(spec.label) + (pendingRestart(spec) ? " *" : "");
    std::string help(spec.help);
    if (!live)
        help += help.empty() ? "Applies after a restart." : " Applies after a restart.";

    ui.push_id(rowId);
    ui.begin_disabled(readOnly ? 1 : 0);
    switch (spec.kind)
    {
    case settings::Kind::boolean:
    {
        int checked = settings::boolValue(value) ? 1 : 0;
        if (ui.checkbox(label.c_str(), &checked))
            change(ui, spec, checked ? "true" : "false");
        break;
    }
    case settings::Kind::choice:
    {
        std::vector<std::string> names;
        std::vector<const char *> items;
        for (std::string_view choice : spec.choices)
            names.push_back(choiceName(spec, choice));
        for (const std::string &name : names)
            items.push_back(name.c_str());
        const auto found = std::find(spec.choices.begin(), spec.choices.end(), value);
        int index = found == spec.choices.end() ? 0 : static_cast<int>(found - spec.choices.begin());
        if (!items.empty() && ui.combo(label.c_str(), &index, items.data(), static_cast<int>(items.size())) &&
            index >= 0 && index < static_cast<int>(spec.choices.size()))
            change(ui, spec, std::string(spec.choices[static_cast<size_t>(index)]));
        break;
    }
    case settings::Kind::integer:
    {
        const int min = static_cast<int>(spec.minValue), max = static_cast<int>(spec.maxValue);
        int number = static_cast<int>(std::clamp<long>(toLong(value, min), min, max));
        if (ui.slider_int(label.c_str(), &number, min, max))
            change(ui, spec, std::to_string(number));
        break;
    }
    case settings::Kind::number:
    {
        const float min = static_cast<float>(spec.minValue), max = static_cast<float>(spec.maxValue);
        float number = static_cast<float>(std::clamp(toDouble(value, min), spec.minValue, spec.maxValue));
        if (ui.slider_float(label.c_str(), &number, min, max, "%.1f"))
            change(ui, spec, formatNumber(number));
        break;
    }
    case settings::Kind::size:
    case settings::Kind::text:
        ui.text_disabled((label + ": " + (value.empty() ? "(automatic)" : value)).c_str());
        break;
    }
    ui.tooltip(help.c_str());
    ui.end_disabled();
    ui.pop_id();
}

void Menu::change(const RrvPcsx2GsBridgeUi &ui, const settings::Spec &spec, const std::string &value)
{
    if (!settings::valid(spec, value))
    {
        setStatus("Not a valid value for " + std::string(spec.label) + ": " + value);
        return;
    }
    const std::string key = id(spec.section, spec.key);
    if (spec.section == "display" && spec.key == "fullscreen")
    {
        // The host toggles the window on the main thread and writes the file
        // when the window actually changes (F11, the green button, the menu).
        m_fullscreenRequest.store(settings::boolValue(value) ? 1 : 0);
        return;
    }
    m_values[key] = value;
    if (save(spec.section, spec.key, value))
        std::fprintf(stderr, "[fukami-menu] saved %s=%s\n", key.c_str(), value.c_str());
    applyLive(ui, spec, value);
}

void Menu::applyLive(const RrvPcsx2GsBridgeUi &ui, const settings::Spec &spec, const std::string &value)
{
    if (spec.section == "display" && spec.key == "aspect")
        ui.set_aspect(value.empty() ? m_launchRatio.c_str() : value.c_str());
    else if (spec.section == "display" && spec.key == "integer_scaling")
        ui.set_integer_scaling(settings::boolValue(value) ? 1 : 0);
    else if (spec.section == "rendering" && spec.key == "fxaa")
        ui.set_fxaa(settings::boolValue(value) ? 1 : 0);
    else if (spec.section == "rendering" && spec.key == "cas")
        ui.set_cas(static_cast<int>(std::clamp<long>(toLong(value, 0), 0, 100)));
}

bool Menu::pendingRestart(const settings::Spec &spec) const
{
    if (appliesLive(spec.section, spec.key))
        return false;
    const std::string key = id(spec.section, spec.key);
    const auto shown = m_values.find(key);
    const auto launched = m_launch.find(key);
    return shown != m_values.end() && launched != m_launch.end() && shown->second != launched->second;
}

bool Menu::save(std::string_view section, std::string_view key, const std::string &value)
{
    std::lock_guard<std::mutex> lock(m_saveMutex);
    if (m_ini.empty())
        return false;
    auto doc = settings::IniDocument::load(m_ini);
    if (!doc)
    {
        m_status = "Could not read the settings file.";
        return false;
    }
    doc->set(section, key, value);
    if (!doc->save(m_ini))
    {
        m_status = "Could not save the settings file.";
        return false;
    }
    m_status.clear();
    return true;
}

void Menu::setStatus(std::string status)
{
    std::lock_guard<std::mutex> lock(m_saveMutex);
    m_status = std::move(status);
}

std::string Menu::status() const
{
    std::lock_guard<std::mutex> lock(m_saveMutex);
    return m_status;
}

} // namespace fukami::menu
