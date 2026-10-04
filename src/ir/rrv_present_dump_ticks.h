#ifndef RRV_PRESENT_DUMP_TICKS_H
#define RRV_PRESENT_DUMP_TICKS_H

#include <charconv>
#include <cctype>
#include <cstdint>
#include <set>
#include <string_view>

namespace rrv::present_dump {

// Parse the opt-in comma-separated tick selector used by the live FullFrame
// capture harness. Invalid or empty entries are ignored deliberately: this is
// a diagnostic filter, and a malformed optional entry must not alter runtime
// presentation. A set provides the required at-most-once selection even if a
// requested tick appears more than once in the environment string.
inline std::set<uint64_t> parseRequestedTicks(const char* text)
{
    std::set<uint64_t> result;
    if (!text) return result;

    std::string_view remaining{text};
    while (true)
    {
        const size_t comma = remaining.find(',');
        std::string_view token = remaining.substr(0, comma);
        while (!token.empty() && std::isspace(static_cast<unsigned char>(token.front())))
            token.remove_prefix(1);
        while (!token.empty() && std::isspace(static_cast<unsigned char>(token.back())))
            token.remove_suffix(1);

        uint64_t tick = 0;
        const char* begin = token.data();
        const char* end = begin + token.size();
        const auto [parsed, error] = std::from_chars(begin, end, tick, 10);
        if (!token.empty() && error == std::errc{} && parsed == end)
            result.insert(tick);

        if (comma == std::string_view::npos) break;
        remaining.remove_prefix(comma + 1u);
    }
    return result;
}

} // namespace rrv::present_dump

#endif
