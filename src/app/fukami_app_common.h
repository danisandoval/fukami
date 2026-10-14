// What the three app layers (fukami_app.mm, fukami_app_linux.cpp, fukami_app_win.cpp) share: the names of the
// launcher-to-game environment variables, the unpacked-disc marker, the settings a launch uses and the environment
// de-duplication. Dialogs, folders and process start stay in each platform's file.
#pragma once

#include "fukami_settings.h"

#include <filesystem>
#include <string>
#include <vector>

namespace fukami::app::detail {

inline constexpr const char *kChild = "RRV_FUKAMI_CHILD";     // set in the game process the launcher starts
inline constexpr const char *kIni = "RRV_FUKAMI_INI";         // the settings file of this launch
inline constexpr const char *kSupport = "RRV_FUKAMI_SUPPORT"; // the user's data folder
inline constexpr const char *kVerifiedMarker = ".fukami-disc-verified";

// True when the unpacked disc folder holds the nine files its marker lists, with their sizes.
bool discReady(const std::filesystem::path &disc);
// Writes the marker for a freshly unpacked disc folder.
void writeMarker(const std::filesystem::path &disc);
// A regular file named *.chd, *.cue or *.bin (any case): a disc image the extractor may accept.
bool isChd(const std::filesystem::path &path);

// UTC, YYYYMMDDTHHMMSSZ: the first part of a session folder's name.
std::string timestamp();

// The settings to launch with: the user's file, and for every setting it does
// not have (a file written by an older version) the app's default ini, not the
// schema default. The user's file is not rewritten for that.
fukami::settings::IniDocument effectiveSettings(fukami::settings::IniDocument doc,
                                                const std::filesystem::path &defaults);

// env(1) semantics for a list of NAME=value strings: a later NAME wins. Keeps only the last occurrence of each
// name, in first-seen order. Names compare exactly, or without case when `ignoreCase` (Windows).
std::vector<std::string> lastWins(const std::vector<std::string> &env, bool ignoreCase = false);

} // namespace fukami::app::detail
