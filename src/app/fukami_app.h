// Fukami.app mode. When the product executable runs from inside a .app bundle
// (Contents/MacOS/), a startup hook (fukami_app.mm) prepares the user's data
// (first-launch CHD setup, settings, save card, session folder) and re-executes
// the same image with the environment scripts/run_gate4_product.sh would give
// it. Outside a bundle nothing changes.
#pragma once

#include <filesystem>

namespace fukami::app {

// True in the re-executed game process started by the app.
bool inApp();
// The settings file the in-game menu reads and writes: RRV_FUKAMI_INI when
// set (the app sets it; ./run.sh passes its rrv.ini), else empty.
std::filesystem::path iniPath();
// The app's bundled default ini (Resources/ or share/fukami-default.ini) when
// this image runs from inside the app, else empty. A setting the user's file
// does not have takes its value from here (settings::withDefaults()).
std::filesystem::path defaultIniPath();
// True when restart() can apply restart-required settings (app mode only).
bool canRestart();
// Re-executes the app so the settings file is read again. Returns only on
// failure (or outside the app).
void restart();

// The save card folder (RRV_GATE3_MC_ROOT) and this session's log folder
// (the folder of RRV_GATE3_RECORD_PAD); empty when unknown.
std::filesystem::path savesFolder();
std::filesystem::path logsFolder();
// Shows the path in Finder (`open -R`); safe from any thread.
void reveal(const std::filesystem::path &path);

} // namespace fukami::app
