// Gate-3 bound workload loader V1. The input is produced from a validated
// workload manifest by scripts/gate3_workload_manifest.py compile, which opens
// and hash-verifies every referenced user-owned file. This parser accepts only
// that exact line format and binds the temporal profile and the guest workload
// once, before any guest instruction. It never falls back to host input,
// host time or implicit defaults.
#include <chrono>
#include <ctime>
#include <cmath>
#include <limits>
#include <type_traits>
#include <fstream>
#include <sstream>
#include <thread>

namespace {
[[noreturn]] void gate3BoundFail(size_t line, const std::string& why)
{
    throw std::runtime_error("Gate3 bound workload line " + std::to_string(line) + ": " + why);
}

template <typename T>
T gate3BoundInteger(const std::string& token, size_t line, int base = 10)
{
    if (token.empty() || token[0] == '+' || (std::is_unsigned_v<T> && token[0] == '-'))
        gate3BoundFail(line, "invalid integer '" + token + "'");
    size_t used = 0;
    try {
        if constexpr (std::is_unsigned_v<T>) {
            const unsigned long long value = std::stoull(token, &used, base);
            if (used == token.size() && value <= std::numeric_limits<T>::max()) return T(value);
        } else {
            const long long value = std::stoll(token, &used, base);
            if (used == token.size() && value >= std::numeric_limits<T>::min() &&
                value <= std::numeric_limits<T>::max()) return T(value);
        }
    } catch (const std::exception&) {}
    gate3BoundFail(line, "invalid integer '" + token + "'");
}

float gate3BoundFloat(const std::string& token, size_t line)
{
    size_t used = 0;
    try {
        const float value = std::stof(token, &used);
        if (used == token.size() && std::isfinite(value)) return value;
    } catch (const std::exception&) {}
    gate3BoundFail(line, "invalid number '" + token + "'");
}

bool gate3BoundFlag(const std::string& token, size_t line)
{
    if (token == "0") return false;
    if (token == "1") return true;
    gate3BoundFail(line, "expected 0 or 1");
}

// <connected> <device_id hex> <buttons hex> <lx> <ly> <rx> <ry> <lt> <rt>
// <analog_sticks> <analog_triggers> <rumble> <deadzone>
PS2Runtime::Gate3PadSampleV1 gate3BoundPad(const std::vector<std::string>& t, size_t first, size_t line)
{
    if (t.size() != first + 13) gate3BoundFail(line, "pad sample needs 13 fields");
    PS2Runtime::Gate3PadSampleV1 sample;
    sample.state.connected = gate3BoundFlag(t[first], line);
    sample.state.deviceId = gate3BoundInteger<uint64_t>(t[first + 1], line, 16);
    sample.state.pressedButtons = gate3BoundInteger<uint32_t>(t[first + 2], line, 16);
    sample.state.leftX = gate3BoundFloat(t[first + 3], line);
    sample.state.leftY = gate3BoundFloat(t[first + 4], line);
    sample.state.rightX = gate3BoundFloat(t[first + 5], line);
    sample.state.rightY = gate3BoundFloat(t[first + 6], line);
    sample.state.leftTrigger = gate3BoundFloat(t[first + 7], line);
    sample.state.rightTrigger = gate3BoundFloat(t[first + 8], line);
    sample.capabilities.analogSticks = gate3BoundFlag(t[first + 9], line);
    sample.capabilities.analogTriggers = gate3BoundFlag(t[first + 10], line);
    sample.capabilities.rumble = gate3BoundFlag(t[first + 11], line);
    sample.deadzone = gate3BoundFloat(t[first + 12], line);
    return sample;
}
} // namespace

std::string PS2Runtime::gate3LoadBoundWorkloadV1(const std::string& path)
{
    std::ifstream input(path);
    if (!input) throw std::runtime_error("Gate3 bound workload unreadable: " + path);
    std::vector<std::vector<std::string>> lines;
    for (std::string text; std::getline(input, text);) {
        std::istringstream words(text);
        std::vector<std::string> tokens;
        for (std::string word; words >> word;) tokens.push_back(word);
        if (tokens.empty()) gate3BoundFail(lines.size() + 1, "blank line");
        lines.push_back(std::move(tokens));
    }
    if (lines.size() < 2 || lines.front() != std::vector<std::string>{"rrv-gate3-bound-workload", "1"})
        gate3BoundFail(1, "expected header 'rrv-gate3-bound-workload 1'");
    const auto& last = lines.back();
    if (last.size() != 2 || last[0] != "end" ||
        gate3BoundInteger<size_t>(last[1], lines.size()) != lines.size() - 1)
        gate3BoundFail(lines.size(), "expected 'end <line count>' (truncated file?)");

    std::optional<std::string> manifest;
    std::optional<uint32_t> intcStat, intcMask;
    std::optional<bool> alarmHsync;
    std::optional<int64_t> epoch;
    std::optional<int32_t> offset;
    std::optional<Gate3CdPolicyV1> policy;
    std::optional<uint64_t> waitBudgetMs;
    std::optional<uint64_t> terminalStarts;
    std::optional<bool> persistentStorage, rtcHostBoot;
    std::array<bool, 2> initialSeen{};
    Gate3GuestWorkloadV1 workload;
    auto once = [](auto& slot, auto value, size_t line) {
        if (slot) gate3BoundFail(line, "duplicate key");
        slot = value;
    };
    for (size_t index = 1; index + 1 < lines.size(); ++index) {
        const auto& t = lines[index];
        const size_t line = index + 1;
        const std::string& key = t[0];
        auto arity = [&](size_t count) { if (t.size() != count) gate3BoundFail(line, key + " arity"); };
        if (key == "manifest_sha256") {
            arity(2);
            if (t[1].size() != 64 || t[1].find_first_not_of("0123456789abcdef") != std::string::npos)
                gate3BoundFail(line, "manifest_sha256 must be 64 lowercase hex digits");
            once(manifest, t[1], line);
        } else if (key == "intc_stat") { arity(2); once(intcStat, gate3BoundInteger<uint32_t>(t[1], line), line);
        } else if (key == "intc_mask") { arity(2); once(intcMask, gate3BoundInteger<uint32_t>(t[1], line), line);
        } else if (key == "alarm_unit") {
            arity(2);
            // Only the source-bound kernel unit is loadable (see TemporalProfile).
            if (t[1] != "hsync") gate3BoundFail(line, "only alarm_unit hsync is loadable");
            once(alarmHsync, true, line);
        } else if (key == "rtc_epoch") { arity(2); once(epoch, gate3BoundInteger<int64_t>(t[1], line), line);
        } else if (key == "rtc_source") {
            arity(2);
            if (t[1] != "host_boot") gate3BoundFail(line, "only rtc_source host_boot is loadable (the bound rtc_epoch is the default)");
            once(rtcHostBoot, true, line);
        } else if (key == "rtc_offset_minutes") { arity(2); once(offset, gate3BoundInteger<int32_t>(t[1], line), line);
        } else if (key == "pad_initial") {
            const auto port = gate3BoundInteger<unsigned>(t.size() > 1 ? t[1] : "", line);
            if (port > 1 || initialSeen[port]) gate3BoundFail(line, "pad_initial port invalid or duplicate");
            initialSeen[port] = true;
            workload.initial_pad[port] = gate3BoundPad(t, 2, line);
        } else if (key == "pad") {
            if (t.size() < 4) gate3BoundFail(line, "pad arity");
            Gate3PadAdmissionV1 item;
            item.cycle = gate3BoundInteger<uint64_t>(t[1], line);
            item.sequence = gate3BoundInteger<uint64_t>(t[2], line);
            item.port = gate3BoundInteger<unsigned>(t[3], line);
            item.sample = gate3BoundPad(t, 4, line);
            workload.pad.push_back(item);
        } else if (key == "cd_policy") {
            arity(2);
            if (t[1] != "deterministic-next-start")
                gate3BoundFail(line, "only cd_policy deterministic-next-start is loadable");
            once(policy, Gate3CdPolicyV1::DeterministicNextStart, line);
        } else if (key == "cd_wait_budget_ms") {
            arity(2);
            const auto value = gate3BoundInteger<uint64_t>(t[1], line);
            if (!value) gate3BoundFail(line, "cd_wait_budget_ms must be positive");
            once(waitBudgetMs, value, line);
        } else if (key == "terminal_starts") {
            arity(2);
            const auto value = gate3BoundInteger<uint64_t>(t[1], line);
            if (!value) gate3BoundFail(line, "terminal_starts must be positive");
            once(terminalStarts, value, line);
        } else if (key == "storage") {
            arity(2);
            if (t[1] != "persistent_user_card")
                gate3BoundFail(line, "only storage persistent_user_card is loadable (fresh_zeroed is the default)");
            once(persistentStorage, true, line);
        } else if (key == "hle") {
            arity(3);
            if (!workload.hle_costs.emplace(t[1], gate3BoundInteger<uint64_t>(t[2], line)).second)
                gate3BoundFail(line, "duplicate hle cost " + t[1]);
        } else {
            gate3BoundFail(line, "unknown key " + key);
        }
    }
    if (!manifest || !intcStat || !intcMask || !alarmHsync || !epoch || !offset || !policy || !waitBudgetMs ||
        !initialSeen[0] || !initialSeen[1] || workload.hle_costs.empty())
        throw std::runtime_error("Gate3 bound workload is missing a required binding");
    // Only a product workload (persistent user card) may run without a terminal; a
    // qualification workload is a bounded run and must declare where it cuts.
    if (!terminalStarts && !persistentStorage)
        throw std::runtime_error("Gate3 bound workload without terminal_starts requires storage persistent_user_card");
    workload.cd_policy = *policy;
    workload.persistent_storage = persistentStorage.value_or(false);
    // Product sessions read the host clock ONCE, here, so saves carry the real date; guest time
    // then runs from that epoch as usual, deterministic within the session. Only an unbounded
    // product workload (persistent card, no terminal) may ask for it: every bounded workload
    // (qualification, replay) keeps its fixed bound epoch, so traces and digests are unchanged.
    if (rtcHostBoot) {
        if (terminalStarts || !persistentStorage)
            throw std::runtime_error("Gate3 rtc_source host_boot requires storage persistent_user_card and no terminal_starts");
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_r(&now, &local);
        epoch = static_cast<int64_t>(now);
        offset = static_cast<int32_t>(local.tm_gmtoff / 60);
    }
    const auto budget = std::chrono::milliseconds(*waitBudgetMs);
    // Host-only wait for user-owned CD data inside its verified extent, with one
    // cumulative budget for the run. Guest time is fixed while waiting
    // (gate3WaitCdHostV1 checks it); exhausting the budget fails the run.
    workload.wait_for_cd_data = [budget, waited = std::chrono::milliseconds(0)]() mutable {
        if (waited >= budget) throw std::runtime_error("Gate3 CD host data wait budget exhausted");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        waited += std::chrono::milliseconds(5);
    };
    rrv::guest_time::TemporalProfile profile{*intcStat, *intcMask, std::nullopt, *epoch, *offset};
    profile.alarm_hsync_ticks = true;
    profile.terminal_starts = terminalStarts.value_or(rrv::guest_time::kNoTerminal);
    gate3ConfigureTemporalV1(profile);
    gate3ConfigureGuestWorkloadV1(std::move(workload));
    return *manifest;
}
