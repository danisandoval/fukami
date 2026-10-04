// Fukami in-game menu: content and state. The menu is drawn by PCSX2's Dear
// ImGui inside the GS bridge (rrv_pcsx2_gs_bridge_set_overlay); this module
// decides what it shows and keeps the settings file (fukami_settings) in step.
//
// Threads. The SDL host (src/host/rrv_sdl_presentation.cpp) owns the main
// thread: it opens/closes the menu, posts input while it is open, reports the
// window's real fullscreen state and takes the menu's requests (fullscreen,
// quit, restart). The bridge calls frame() on the GS owner thread once per
// presented field. Everything shared between the two is atomic or guarded.
//
// Guest neutrality: with the menu closed, frame() makes no UI calls and
// changes nothing; the host keeps the game's pad neutral while it is open.
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

#include "fukami_settings.h"

struct RrvPcsx2GsBridgeUi;

namespace fukami::menu {

// One input event in bridge UI terms (RRV_PCSX2_GS_UI_KEY_* for keys; mouse
// positions in drawable pixels).
struct Input
{
    enum class Type : uint8_t { key, mousePosition, mouseButton, mouseWheel };
    Type type = Type::key;
    uint32_t code = 0; // key or mouse button
    bool down = false;
    float x = 0.0f;
    float y = 0.0f;
};

// Settings the menu applies at once (the rest need a restart). fullscreen is
// applied by the host; the others by the bridge (presentation only).
bool appliesLive(std::string_view section, std::string_view key);

class Menu
{
public:
    // Reads `ini` once: the values the running game was launched with, which
    // restart-only settings are compared against. Empty path: nothing is
    // saved and restart-only settings are read-only. `defaults` is the app's
    // bundled default ini (empty: none): a setting `ini` does not have shows
    // the value from there, as the launcher used it (settings::withDefaults).
    explicit Menu(std::filesystem::path ini, const std::filesystem::path &defaults = {});

    const std::filesystem::path &iniPath() const { return m_ini; }

    // ---- main thread ------------------------------------------------------
    // Set by the host once the bridge accepted the overlay; before that (and
    // with a bridge without overlay support) the menu cannot be shown.
    void setAvailable(bool available) { m_available.store(available, std::memory_order_release); }
    bool available() const { return m_available.load(std::memory_order_acquire); }
    bool isOpen() const { return m_open.load(std::memory_order_acquire); }
    void open();
    void close();
    // Queued for the next frame; ignored while the menu is closed.
    void post(const Input &input);
    // The window's real fullscreen state. With persist, a change is written
    // to the settings file (only when the file says otherwise).
    void fullscreenChanged(bool fullscreen, bool persist);
    // Requests from the menu. -1: none, 0: leave fullscreen, 1: enter it.
    int takeFullscreenRequest() { return m_fullscreenRequest.exchange(-1); }
    bool takeQuitRequest() { return m_quitRequest.exchange(false); }
    bool takeRestartRequest() { return m_restartRequest.exchange(false); }

    // ---- GS owner thread (bridge overlay callback) -------------------------
    static void frameCallback(void *menu, const RrvPcsx2GsBridgeUi *ui);
    void frame(const RrvPcsx2GsBridgeUi &ui);

private:
    void reload();
    void drawRow(const RrvPcsx2GsBridgeUi &ui, const settings::Spec &spec, int id);
    void change(const RrvPcsx2GsBridgeUi &ui, const settings::Spec &spec, const std::string &value);
    void applyLive(const RrvPcsx2GsBridgeUi &ui, const settings::Spec &spec, const std::string &value);
    bool pendingRestart(const settings::Spec &spec) const;
    bool save(std::string_view section, std::string_view key, const std::string &value);
    void setStatus(std::string status);
    std::string status() const;

    const std::filesystem::path m_ini;
    settings::IniDocument m_defaults; // the app's bundled default ini; empty outside the app
    const std::filesystem::path m_saves;
    const std::filesystem::path m_logs;
    std::map<std::string, std::string> m_launch; // "section.key" -> value at launch
    std::string m_launchRatio;                   // the ratio the game runs with

    std::atomic<bool> m_available{false};
    std::atomic<bool> m_open{false};
    std::atomic<bool> m_fullscreen{false};
    std::atomic<int> m_fullscreenRequest{-1};
    std::atomic<bool> m_quitRequest{false};
    std::atomic<bool> m_restartRequest{false};

    std::mutex m_inputMutex;
    std::deque<Input> m_inputs;

    mutable std::mutex m_saveMutex; // settings file + status line
    std::string m_status;

    // GS owner thread only.
    bool m_wasOpen = false;
    std::map<std::string, std::string> m_values; // "section.key" -> shown value
};

} // namespace fukami::menu
