#ifndef RRV_WINDOW_TITLE_H
#define RRV_WINDOW_TITLE_H

// rrv_window_title.h — what the window's title bar says.
//
//   RRV-Recomp | Ridge Racer V | GS SW | Phase 4 | 30 fps (present 60)
//
// Two things worth knowing at a glance while the game runs: WHICH GS backend
// is live, and how fast the guest is actually producing frames.
//
// WHY THE RATE IS SPLIT IN TWO
//   This project has been burned by conflating them (docs/ARCHITECTURE.md
//   §FIELD, STATE.md "four cadences are distinct"). We present the same display
//   list more than once when the guest takes longer than a field to build it,
//   so presents per second is NOT the game's frame rate: in every 3D phase the
//   window presents ~60 times a second while the guest completes 30 display
//   lists. The title therefore leads with the GUEST rate — display lists
//   completed per second, counted at the `sceGsSyncV` frame boundary — and
//   appends the present rate only when the two differ, so the number a viewer
//   quotes is the honest one.
//
// COST
//   One relaxed atomic increment per guest frame (~30-60/s) and one per host
//   present, plus a single `SetWindowTitle` call at most once a second. That is
//   not measurable against a 33 ms frame. `RRV_WINDOW_TITLE_FPS=0` still leaves
//   the GS backend in the title but stops the rate from being shown or
//   computed, per the "don't pay for it" instruction.
//
// Header-only with C++17 inline variables, like rrv_cadence_diag.h and
// rrv_gs_record_hooks.h, so the vendored runtime just includes it.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if !defined(RRV_PRODUCT_HOST_SDL)
#include "raylib.h"
#endif
#include "rrv_gs_render_mode.h"

namespace rrv::hostwin
{
    inline std::atomic<uint64_t> g_guestFrames{0};

    // The attract/demo scene phase ([0x334E94], docs/instrumentation/ANCHORS.md
    // §2). Shown in the title so the phase a screenshot or a bug report refers
    // to is on the picture itself instead of being reconstructed afterwards.
    inline std::atomic<uint32_t> g_scenePhase{0};

    // Called at the guest frame boundary (one completed display list).
    inline void noteGuestFrame()
    {
        g_guestFrames.fetch_add(1, std::memory_order_relaxed);
    }

    // Called from the per-frame patch, which already reads scenePhase.
    inline void noteScenePhase(uint32_t phase)
    {
        g_scenePhase.store(phase, std::memory_order_relaxed);
    }

    // Title-bar short form of the backend names in rrv_gs_backend.h
    // ("PCSX2 Software", "PCSX2 Metal", "Legacy SW"). The title bar is narrow
    // and the vendor prefix is constant, so only the distinguishing word is
    // kept; anything unrecognised is passed through verbatim rather than
    // guessed at.
    inline const char *shortBackendName(const char *name)
    {
        if (!name || !name[0])
            return "unknown";
        if (std::strcmp(name, "PCSX2 Software") == 0)
            return "SW";
        if (std::strcmp(name, "PCSX2 Metal") == 0)
            return "Metal";
        return name;
    }

    inline std::string &baseTitleStorage()
    {
        static std::string s_base;
        return s_base;
    }

    // The title as main.cpp built it: "RRV-Recomp | <game>".
    inline void setBaseTitle(const char *title)
    {
        baseTitleStorage() = title ? title : "RRV-Recomp";
    }

    inline bool showRate()
    {
        static const bool s_on = [] {
            const char *v = std::getenv("RRV_WINDOW_TITLE_FPS");
            return !(v && v[0] == '0' && v[1] == '\0');
        }();
        return s_on;
    }

#if !defined(RRV_PRODUCT_HOST_SDL)
    // Called once per host present, from the thread that owns the window.
    // Retitles at most once a second; does nothing else.
    inline void notePresent(const char *gsBackendName, rrv::gs::GsRenderMode renderMode)
    {
        static uint64_t s_presents = 0;
        static uint64_t s_lastGuestFrames = 0;
        static std::string s_lastTitle;
        static auto s_lastUpdate = std::chrono::steady_clock::now();

        ++s_presents;

        const auto now = std::chrono::steady_clock::now();
        const double secs = std::chrono::duration<double>(now - s_lastUpdate).count();
        if (secs < 1.0)
            return;

        const uint64_t guestNow = g_guestFrames.load(std::memory_order_relaxed);
        const double guestFps = static_cast<double>(guestNow - s_lastGuestFrames) / secs;
        const double presentFps = static_cast<double>(s_presents) / secs;
        s_lastGuestFrames = guestNow;
        s_presents = 0;
        s_lastUpdate = now;

        std::string title = baseTitleStorage();
        title += " | GS ";
        title += shortBackendName(gsBackendName);
        title += " (";
        // The runtime passes the mode it composes (always field with a PCSX2
        // bridge); the title shows the published OUTPUT mode (Gate 6).
        (void)renderMode;
        title += rrv::gs::renderModeName(rrv::gs::captureRenderMode());
        title += ")";

        char phase[32];
        std::snprintf(phase, sizeof(phase), " | Phase %u",
                      g_scenePhase.load(std::memory_order_relaxed));
        title += phase;

        if (showRate())
        {
            char rate[64];
            // Only mention the present rate when it is not the same number, so
            // the common 2D case reads as a plain "60 fps".
            if (presentFps >= guestFps + 1.5)
                std::snprintf(rate, sizeof(rate), " | %.0f fps (present %.0f)",
                              guestFps, presentFps);
            else
                std::snprintf(rate, sizeof(rate), " | %.0f fps", guestFps);
            title += rate;
        }

        // Retitling is cheap but not free on every platform; skip the call when
        // the text has not actually changed.
        if (title != s_lastTitle)
        {
            SetWindowTitle(title.c_str());
            s_lastTitle = std::move(title);
        }
    }
#endif // !RRV_PRODUCT_HOST_SDL
} // namespace rrv::hostwin

#endif // RRV_WINDOW_TITLE_H
