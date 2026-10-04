#include <set>
#include <cmath>

void PS2Runtime::gate3ConfigureGuestWorkloadV1(Gate3GuestWorkloadV1 profile)
{
    if (m_gate3GuestWorkloadV1 || !m_gate3TemporalV1 || gate3TemporalV1().now() != 0 ||
        gate3ModeledEeCyclesV1() != 0)
        throw std::logic_error("Gate3 guest workload must bind once before guest execution");
    if (profile.hle_costs.empty())
        throw std::logic_error("Gate3 HLE cost map is unbound");
    auto valid_pad = [](const Gate3PadSampleV1& sample) {
        const auto& state = sample.state;
        auto axis = [](float value) { return std::isfinite(value) && value >= -1.0f && value <= 1.0f; };
        auto trigger = [](float value) { return std::isfinite(value) && value >= 0.0f && value <= 1.0f; };
        return std::isfinite(sample.deadzone) && sample.deadzone >= 0.0f && sample.deadzone < 1.0f &&
               axis(state.leftX) && axis(state.leftY) && axis(state.rightX) && axis(state.rightY) &&
               trigger(state.leftTrigger) && trigger(state.rightTrigger);
    };
    for (const auto& sample : profile.initial_pad)
        if (!valid_pad(sample)) throw std::logic_error("Gate3 initial pad sample invalid");
    std::set<uint64_t> sequences;
    uint64_t previous_cycle = 0;
    uint64_t previous_sequence = 0;
    bool first = true;
    for (const auto& item : profile.pad) {
        if (item.port >= m_gate3AdmittedPadV1.size() || !valid_pad(item.sample) ||
            (!first && std::tie(item.cycle, item.sequence) <=
                       std::tie(previous_cycle, previous_sequence)) ||
            !sequences.insert(item.sequence).second)
            throw std::logic_error("Gate3 pad admission order or identity invalid");
        first = false;
        previous_cycle = item.cycle;
        previous_sequence = item.sequence;
    }
    if (profile.cd_policy == Gate3CdPolicyV1::DeterministicNextStart && !profile.cd.empty())
        throw std::logic_error("Gate3 deterministic CD policy takes no pre-declared schedule");
    std::set<std::string> request_ids;
    uint64_t previous_cd_cycle = 0;
    uint64_t previous_callback_start = 0;
    uint64_t previous_callback_cycle = 0;
    for (const auto& item : profile.cd) {
        if (item.id.empty() || item.route.empty() || !request_ids.insert(item.id).second ||
            item.cycle < previous_cd_cycle ||
            (item.success && item.payload.size() != uint64_t(item.sectors) * 2048u) ||
            (!item.success && !item.payload.empty()))
            throw std::logic_error("Gate3 CD request identity, order or payload invalid");
        previous_cd_cycle = item.cycle;
        if (item.callback) {
            const auto& callback = *item.callback;
            if (!item.success || callback.cycle <= item.cycle ||
                callback.vblank_start == 0 || callback.vblank_start < previous_callback_start ||
                (callback.vblank_start == previous_callback_start &&
                 callback.cycle != previous_callback_cycle) ||
                callback.vblank_start >= gate3TemporalV1().terminal_starts() ||
                !sequences.insert(callback.sequence).second)
                throw std::logic_error("Gate3 CD callback admission invalid");
            previous_callback_start = callback.vblank_start;
            previous_callback_cycle = callback.cycle;
        }
    }
    m_gate3AdmittedPadV1 = profile.initial_pad;
    m_gate3GuestWorkloadV1.emplace(std::move(profile));
    for (const auto& item : m_gate3GuestWorkloadV1->pad) {
        gate3TemporalV1().Admit({item.cycle, item.sequence, [this, item] {
            if (gate3AtCutV1()) throw std::logic_error("Gate3 pad after cut");
            m_gate3AdmittedPadV1[item.port] = item.sample;
            const std::string line = "P seq=" + std::to_string(item.sequence) + " port=" +
                std::to_string(item.port) + " cyc=" + std::to_string(gate3TemporalV1().now()) + "\n";
            gate3DiagTextV1(line.c_str());
        }});
    }
    for (const auto& item : m_gate3GuestWorkloadV1->cd) {
        if (!item.callback) continue;
        const size_t index = m_gate3CdReadyV1.size();
        m_gate3CdReadyV1.push_back({item.id, item.callback->vblank_start, false, false});rrv::guest_time::QuietBump();
        const auto callback = *item.callback;
        gate3TemporalV1().Admit({callback.cycle, callback.sequence, [this, index, callback] {
            auto& record = m_gate3CdReadyV1.at(index);
            const auto last_start_cycle = gate3TemporalV1().lastStartCycle();
            if (!record.issued || gate3TemporalV1().starts() != callback.vblank_start ||
                !last_start_cycle || *last_start_cycle != callback.cycle || gate3AtCutV1())
                throw std::logic_error("Gate3 CD callback was not ready at its bound VBlank");
            record.admitted = true;
        }});
    }
}

PS2Runtime::Gate3PadSampleV1 PS2Runtime::gate3PadSampleV1(int port)
{
    if (!m_gate3GuestWorkloadV1 || port < 0 || port >= int(m_gate3AdmittedPadV1.size()))
        throw std::logic_error("Gate3 pad admission unbound");
    gate3HardwareV1();
    if (gate3AtCutV1()) throw std::logic_error("Gate3 pad read after cut");
    return m_gate3AdmittedPadV1[port];
}

void PS2Runtime::gate3ChargeNamedHleV1(R5900Context* ctx, const char* name)
{
    if (!m_gate3GuestWorkloadV1 || !ctx || !name)
        throw std::logic_error("Gate3 HLE workload unbound");
    const auto found = m_gate3GuestWorkloadV1->hle_costs.find(name);
    if (found == m_gate3GuestWorkloadV1->hle_costs.end())
        throw std::logic_error(std::string("Gate3 HLE cost unbound: ") + name);
    uint64_t remaining = found->second;
    // Inside the armed lazy window, and ending inside it: only the clock moves (gate3LazyChargeV1).
    if (gate3LazyChargeV1(ctx, remaining))
        return;
    gate3TemporalCheckpointV1(ctx);
    if (std::strncmp(name, "syscall:", 8) == 0) {
        // A kernel syscall runs with EXL set: no guest callback can enter its
        // service. HardwareTo still visits every deadline in the charge; the
        // first instruction checkpoint after the service delivers what is due.
        if (remaining != 0) gate3ChargeHleV1(remaining);
        return;
    }
    // Library and patch HLE is user-mode guest work: interruptible at each
    // exact deadline, before the host service body takes effect.
    while (remaining != 0) {
        const uint64_t now = gate3TemporalV1().now();
        if (remaining > UINT64_MAX - now)
            throw std::overflow_error("Gate3 named HLE cycle overflow");
        const uint64_t deadline = gate3TemporalV1().NextDeadline();
        if (deadline <= now) {
            gate3TemporalCheckpointV1(ctx);
            if (gate3TemporalV1().NextDeadline() <= gate3TemporalV1().now())
                throw std::logic_error("Gate3 named HLE deadline did not advance");
            continue;
        }
        const uint64_t delta = std::min(remaining, deadline - now);
        gate3ChargeHleV1(delta);
        remaining -= delta; // Nested guest work can advance now; it does not consume this HLE's cost.
        gate3TemporalCheckpointV1(ctx);
    }
}

bool PS2Runtime::gate3ShouldWaitCdHostV1(const char* route, uint32_t lbn, uint32_t sectors, bool resolvable)
{
    if (m_gate3GuestWorkloadV1 &&
        m_gate3GuestWorkloadV1->cd_policy == Gate3CdPolicyV1::DeterministicNextStart)
        return resolvable; // In-extent data is missing host data: wait. Outside it: guest-visible failure.
    if (!m_gate3GuestWorkloadV1 || m_gate3NextCdReadV1 >= m_gate3GuestWorkloadV1->cd.size())
        throw std::logic_error("Gate3 CD request unbound");
    const auto& expected = m_gate3GuestWorkloadV1->cd[m_gate3NextCdReadV1];
    return expected.success && expected.route == route && expected.lbn == lbn &&
           expected.sectors == sectors;
}

void PS2Runtime::gate3WaitCdHostV1()
{
    if (!m_gate3GuestWorkloadV1 || !m_gate3GuestWorkloadV1->wait_for_cd_data)
        throw std::logic_error("Gate3 missing host CD data; wait provider unbound");
    const uint64_t before = gate3TemporalV1().now();
    m_gate3GuestWorkloadV1->wait_for_cd_data();
    if (gate3TemporalV1().now() != before)
        throw std::logic_error("Gate3 host CD wait advanced guest time");
}

void PS2Runtime::gate3RecordCdReadV1(const char* route, uint32_t lbn, uint32_t sectors,
                                     bool iop_destination, bool success,
                                     const std::vector<uint8_t>& payload)
{
    if (m_gate3GuestWorkloadV1 &&
        m_gate3GuestWorkloadV1->cd_policy == Gate3CdPolicyV1::DeterministicNextStart) {
        if (gate3AtCutV1()) throw std::logic_error("Gate3 CD request after cut");
        uint64_t digest = 1469598103934665603ull;
        for (uint8_t byte : payload) { digest ^= byte; digest *= 1099511628211ull; }
        m_gate3CdTraceV1.push_back({m_gate3NextCdReadV1++, gate3TemporalV1().now(),
            gate3TemporalV1().starts(), route, lbn, sectors, iop_destination, success,
            payload.size(), digest});
        gate3DiagCdV1(route, lbn, sectors, iop_destination, success, payload.size(), digest);
        const std::string_view name{route};
        if (success && (name == "sceCdRead" || name == "sceCdReadIOPm"))
            gate3CdRouteCompletedV1(route, true);
        return;
    }
    if (!m_gate3GuestWorkloadV1 || m_gate3NextCdReadV1 >= m_gate3GuestWorkloadV1->cd.size())
        throw std::logic_error("Gate3 CD request unbound");
    const auto& expected = m_gate3GuestWorkloadV1->cd[m_gate3NextCdReadV1];
    if (expected.cycle != gate3TemporalV1().now() || expected.route != route ||
        expected.lbn != lbn || expected.sectors != sectors ||
        expected.iop_destination != iop_destination || expected.success != success ||
        expected.payload != payload)
        throw std::logic_error("Gate3 CD request/status/payload differs from bound schedule");
    if (expected.callback) {
        for (auto& record : m_gate3CdReadyV1)
            if (record.id == expected.id) { record.issued = true; break; }
    }
    ++m_gate3NextCdReadV1;
}

void PS2Runtime::gate3CdRouteCompletedV1(const char* route, bool success)
{
    if (!m_gate3GuestWorkloadV1)
        throw std::logic_error("Gate3 CD completion workload unbound");
    if (m_gate3GuestWorkloadV1->cd_policy != Gate3CdPolicyV1::DeterministicNextStart || !success)
        return; // Declared schedules bind callbacks per request identity.
    // One callback group per effective start drains every completion since the
    // previous start, as the product's single-invocation pump did.
    const uint64_t start = gate3TemporalV1().starts() + 1;
    if (start > gate3TemporalV1().terminal_starts()) return; // After the terminal cut no guest callback is admitted.
    if (m_gate3CdReadyV1.empty() || m_gate3CdReadyV1.back().start != start ||
        m_gate3NextCdCallbackV1 >= m_gate3CdReadyV1.size())
        m_gate3CdReadyV1.push_back({std::string(route) + "@" + std::to_string(start), start, true, true});rrv::guest_time::QuietBump();
}

void PS2Runtime::gate3SetCdCallbackV1(uint32_t handler, uint32_t gp)
{
    if (!m_gate3GuestWorkloadV1) throw std::logic_error("Gate3 CD callback workload unbound");
    m_gate3CdCallbackV1 = handler;
    m_gate3CdCallbackGpV1 = gp;
}

void PS2Runtime::gate3ServiceAdmissionsV1(R5900Context* ctx)
{
    if (!m_gate3GuestWorkloadV1 || !ctx || gate3AtCutV1() || gate3AdmissionSuppressedV1(2u))
        return;
    if (g_gate8AudioHooksV1.service) {
        g_gate8AudioHooksV1.service(this, ctx);
        if (gate3AtCutV1()) return;
    }
    while (m_gate3NextCdCallbackV1 < m_gate3CdReadyV1.size()) {
            const auto start = m_gate3CdReadyV1[m_gate3NextCdCallbackV1].start;
            const auto now_start = gate3TemporalV1().starts();
            if (start > now_start) break;
            if (start < now_start) throw std::logic_error("Gate3 CD callback VBlank missed");
            size_t end = m_gate3NextCdCallbackV1;
            while (end < m_gate3CdReadyV1.size() && m_gate3CdReadyV1[end].start == start) {
                if (!m_gate3CdReadyV1[end].issued || !m_gate3CdReadyV1[end].admitted)
                    throw std::logic_error("Gate3 CD callback admission incomplete");
                ++end;
            }
            if (!m_gate3CdCallbackV1) {
                // Product rule: completions with no registered callback are drained silently.
                if (m_gate3GuestWorkloadV1->cd_policy == Gate3CdPolicyV1::DeterministicNextStart) {
                    m_gate3NextCdCallbackV1 = end;
                    continue;
                }
                throw std::logic_error("Gate3 CD callback target unbound");
            }
            m_gate3NextCdCallbackV1 = end; // Retire group before nested guest work.
            gate3RunCallbackV1(ctx, m_gate3CdCallbackV1, m_gate3CdCallbackGpV1,
                               1u, 0u, 0u, false, 2u);
            if (gate3AtCutV1()) break;
    }
}
