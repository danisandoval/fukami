// See fukami_app_common.h. Also the part of fukami_app.h that only reads the game process's environment, which is
// the same on every platform.
#include "fukami_app_common.h"
#include "fukami_app.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>

namespace fukami::app::detail {

namespace fs = std::filesystem;

bool discReady(const fs::path &disc)
{
    std::ifstream marker(disc / kVerifiedMarker);
    if (!marker)
        return false;
    std::string name;
    uintmax_t size = 0;
    int files = 0;
    while (marker >> name >> size)
    {
        std::error_code ec;
        if (fs::file_size(disc / name, ec) != size || ec)
            return false;
        ++files;
    }
    return files == 9;
}

void writeMarker(const fs::path &disc)
{
    std::ofstream marker(disc / kVerifiedMarker, std::ios::trunc);
    for (const auto &entry : fs::directory_iterator(disc))
        if (entry.is_regular_file() && entry.path().filename() != kVerifiedMarker &&
            entry.path().filename().string().front() != '.')
            marker << entry.path().filename().string() << ' ' << entry.file_size() << '\n';
}

bool isChd(const fs::path &path)
{
    std::string ext = path.extension().string();
    for (auto &c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::error_code ec;
    return (ext == ".chd" || ext == ".cue" || ext == ".bin") && fs::is_regular_file(path, ec);
}

std::string timestamp()
{
    char buffer[32];
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    std::strftime(buffer, sizeof buffer, "%Y%m%dT%H%M%SZ", &utc);
    return buffer;
}

fukami::settings::IniDocument effectiveSettings(fukami::settings::IniDocument doc, const fs::path &defaults)
{
    const auto packaged = fukami::settings::IniDocument::load(defaults);
    if (!packaged)
        return doc;
    for (const auto &entry : fukami::settings::inherited(doc, *packaged))
        std::fprintf(stderr, "[fukami] settings: %s.%s = %s (app default; not in the settings file)\n",
                     entry.section.c_str(), entry.key.c_str(), entry.value.c_str());
    return fukami::settings::withDefaults(std::move(doc), *packaged);
}

std::vector<std::string> lastWins(const std::vector<std::string> &env, bool ignoreCase)
{
    std::map<std::string, std::string> last;
    std::vector<std::string> order;
    for (const auto &entry : env)
    {
        std::string name = entry.substr(0, entry.find('='));
        if (ignoreCase)
            for (auto &c : name)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (!last.count(name))
            order.push_back(name);
        last[name] = entry;
    }
    std::vector<std::string> out;
    for (const auto &name : order)
        out.push_back(last[name]);
    return out;
}

} // namespace fukami::app::detail

namespace fukami::app {

bool inApp() { return std::getenv(detail::kChild) != nullptr; }

std::filesystem::path iniPath()
{
    const char *value = std::getenv(detail::kIni);
    return value && *value ? std::filesystem::path(value) : std::filesystem::path{};
}

bool canRestart() { return inApp(); }

std::filesystem::path savesFolder()
{
    const char *value = std::getenv("RRV_GATE3_MC_ROOT");
    return value && *value ? std::filesystem::path(value) : std::filesystem::path{};
}

std::filesystem::path logsFolder()
{
    const char *value = std::getenv("RRV_GATE3_RECORD_PAD");
    return value && *value ? std::filesystem::path(value).parent_path() : std::filesystem::path{};
}

} // namespace fukami::app
