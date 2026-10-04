// Inactive Gate-3 candidate. Host threads carry stacks, never scheduling policy.
#pragma once

#ifndef RRV_GATE4_QUIET_GENERATION_V1
#define RRV_GATE4_QUIET_GENERATION_V1
#include <atomic>
#include <cstdint>
namespace rrv::guest_time {
// Gate-4 lazy checkpoint: bumped by every change to a checkpoint quiet input.
inline std::atomic<uint64_t> quiet_generation{1};
inline void QuietBump() { quiet_generation.fetch_add(1, std::memory_order_relaxed); }
// The same bump without the bus lock, for code that only ever runs in the
// serialized producer domain (the guest-time owner, the INTC, the hardware
// commit): there it is `lock inc` five times per commit, about 0.4 ms per
// VBlank start on a Steam Deck (profile 2026-10-02). Every reader of the
// generation is in that domain too, so a reader never runs between this load
// and this store. Other threads keep bumping with QuietBump(); if one lands in
// between, the two bumps become one, and that is enough: a reader only asks
// whether the value differs from the one it armed with, the value it armed
// with is at most the value loaded here, and what is stored is one more.
inline void QuietBumpProducer() {
    quiet_generation.store(quiet_generation.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}
}
#endif
#include <condition_variable>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <tuple>

namespace rrv::guest_time {
inline thread_local int context_id = 0;
struct ThreadExit : std::exception {
    const char* what() const noexcept override { return "PS2 Thread Exit"; }
};

class Scheduler {
public:
    struct Wait {
        int id;
        uint64_t sequence;
        std::atomic<bool> resolved{false};
        int result = 0;
        uint32_t bits = 0;
        Wait(int owner, uint64_t order) : id(owner), sequence(order) {}
    };
    using Ticket = std::shared_ptr<Wait>;
private:
    struct Context {
        int priority = 1;
        uint64_t ready = 0;
        bool live = true, blocked = false, terminating = false;
        int suspended = 0;
        std::thread::id host;
        Ticket wait;
    };
    std::mutex mutex_;
    std::condition_variable cv_;
    std::map<int, Context> contexts_;
    uint64_t sequence_ = 0;
    int selected_ = 0;
    bool released_ = true;
    bool frozen_ = false;
    bool stopping_ = false;
    // Gate-4: bumped under mutex_ by every state change. quiet() skips the
    // lock when neither the state nor the context changed since a
    // checkpoint(id,-1)/terminated(id) pair answered stay and live.
    std::atomic<uint64_t> version_{1};
    std::atomic<uint64_t> quietVersion_{0};
    std::atomic<int> quietId_{0};
    void changed() { version_.fetch_add(1, std::memory_order_release); rrv::guest_time::QuietBump(); }
    Context& at(int id) { return contexts_.at(id); }
    bool eligible(const Context& c) const {
        return c.live && !c.blocked && (!c.suspended || c.terminating);
    }
    int best() const {
        int id = 0;
        for (const auto& [candidate, c] : contexts_)
            if (eligible(c) && (!id || std::tie(c.priority, c.ready) <
                std::tie(contexts_.at(id).priority, contexts_.at(id).ready))) id = candidate;
        return id;
    }
    void select() {
        if (released_) selected_ = frozen_ ? 0 : best();
        cv_.notify_all();
    }
public:
    Scheduler() { start(1, 0); } // Preserve compatible-v2's initial main priority.
    bool start(int id, int priority) {
        std::lock_guard lock(mutex_); changed();
        if (stopping_) return false;
        auto it = contexts_.find(id);
        if (it != contexts_.end() && it->second.live)
            throw std::logic_error("Gate3 duplicate live context");
        Context c; c.priority = priority; c.ready = ++sequence_;
        contexts_[id] = c;
        select();
        return true;
    }
    void enter(int id) {
        std::unique_lock lock(mutex_); changed();
        auto& c = at(id);
        const auto host = std::this_thread::get_id();
        if (c.host != std::thread::id{} && c.host != host)
            throw std::logic_error("Gate3 unregistered host guest entry");
        c.host = host;
        cv_.wait(lock, [&] { return !frozen_ && selected_ == id && eligible(c); });
        released_ = false;
    }
    // Called only AFTER dropping every recursive G level. Metadata never locks G.
    void release(int id) {
        std::lock_guard lock(mutex_); changed();
        if (selected_ != id) throw std::logic_error("Gate3 release by non-owner");
        released_ = true;
        select();
    }
    Ticket wait(int id) {
        std::lock_guard lock(mutex_); changed();
        auto& c = at(id);
        if (selected_ != id || c.wait) throw std::logic_error("Gate3 nested wait");
        c.wait = std::make_shared<Wait>(id, ++sequence_);
        c.blocked = true;
        if (stopping_) {
            auto ticket = c.wait; ticket->result = -418; ticket->resolved = true;
            c.wait.reset(); c.blocked = false; return ticket;
        }
        return c.wait;
    }
    bool resolve(const Ticket& ticket, int result, uint32_t bits = 0) {
        std::lock_guard lock(mutex_); changed();
        if (!ticket || ticket->resolved) return false;
        auto& c = at(ticket->id);
        if (c.wait != ticket) throw std::logic_error("Gate3 stale wait");
        ticket->result = result; ticket->bits = bits; ticket->resolved = true;
        c.wait.reset(); c.blocked = false; c.ready = ++sequence_;
        select();
        return true;
    }
    bool cancel(int id, int result) {
        Ticket ticket;
        { std::lock_guard lock(mutex_); ticket = at(id).wait; }
        return resolve(ticket, result);
    }
    // Predicate and park share this mutex, including cancellation-before-park.
    void park(const Ticket& ticket) {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [&] { return ticket->resolved.load(); });
    }
    void suspend(int id) {
        std::lock_guard lock(mutex_); changed(); ++at(id).suspended;
    }
    void resume(int id) {
        std::lock_guard lock(mutex_); changed();
        auto& c = at(id);
        if (!c.suspended) throw std::logic_error("Gate3 resume non-suspended context");
        if (--c.suspended == 0 && !c.blocked) c.ready = ++sequence_;
        select();
    }
    void terminate(int id, int result) {
        { std::lock_guard lock(mutex_); changed(); at(id).terminating = true; }
        cancel(id, result);
        std::lock_guard lock(mutex_); select();
    }
    void dormant(int id) {
        std::lock_guard lock(mutex_); changed();
        auto& c = at(id);
        if (c.wait) throw std::logic_error("Gate3 dormant with pending wait");
        c.live = false;
    }
    void beginExit(int id) {
        std::lock_guard lock(mutex_); changed();
        at(id).terminating = false;
        at(id).suspended = 0;
    }
    void priority(int id, int priority) {
        std::lock_guard lock(mutex_); changed();
        auto it = contexts_.find(id);
        // Dormant threads may change priority before their first StartThread.
        if (it != contexts_.end()) it->second.priority = priority;
    }
    bool precedes(const Ticket& a, const Ticket& b) {
        std::lock_guard lock(mutex_);
        return std::tie(at(a->id).priority, a->sequence) <
               std::tie(at(b->id).priority, b->sequence);
    }
    bool checkpoint(int id, int rotate = -1) {
        std::lock_guard lock(mutex_);
        auto& c = at(id);
        if (rotate >= 0) changed();
        if (rotate == c.priority) c.ready = ++sequence_;
        else if (rotate >= 0) {
            Context* first = nullptr;
            for (auto& [other, candidate] : contexts_)
                if (eligible(candidate) && candidate.priority == rotate &&
                    (!first || candidate.ready < first->ready)) first = &candidate;
            if (first) first->ready = ++sequence_;
        }
        const int next = best();
        // Equal priority retains RUN unless explicitly rotated.
        return frozen_ || !eligible(c) || (next && next != id &&
            (rotate == c.priority || at(next).priority < c.priority));
    }
    bool terminated(int id) {
        std::lock_guard lock(mutex_); return stopping_ || at(id).terminating;
    }
    uint64_t version() const { return version_.load(std::memory_order_acquire); }
    // True only if checkpoint(id,-1) would return false and terminated(id)
    // false: nothing changed since markQuiet(id, v) recorded that answer.
    bool quiet(int id) const {
        return quietId_.load(std::memory_order_relaxed) == id &&
               quietVersion_.load(std::memory_order_relaxed) == version();
    }
    // v must be version() read before the checkpoint/terminated pair.
    void markQuiet(int id, uint64_t v) {
        quietVersion_.store(v, std::memory_order_relaxed); quietId_.store(id, std::memory_order_relaxed);
        rrv::guest_time::QuietBump();
    }
    int status(int id) {
        std::lock_guard lock(mutex_);
        auto it = contexts_.find(id);
        if (it == contexts_.end() || !it->second.live) return 0x10;
        const auto& c = it->second;
        if (c.blocked) return c.suspended ? 0xc : 4;
        if (c.suspended) return 8;
        return selected_ == id && !released_ ? 1 : 2;
    }
    bool hasRunnable() { std::lock_guard lock(mutex_); return best() != 0; }
    // A hardware-admitted callback borrows this host stack under the existing
    // guest permit. Its separate wait ticket keeps the interrupted wait intact.
    void beginCallback(int parent, int callback) {
        std::lock_guard lock(mutex_); changed();
        if (selected_ != parent || released_ || contexts_.count(callback))
            throw std::logic_error("Gate3 invalid callback ownership");
        ++at(parent).suspended;
        Context child; child.priority = -1; child.ready = ++sequence_;
        child.host = std::this_thread::get_id(); contexts_.emplace(callback, child);
        selected_ = callback;
    }
    void endCallback(int parent, int callback) {
        std::lock_guard lock(mutex_); changed();
        if (selected_ != callback || at(callback).wait)
            throw std::logic_error("Gate3 callback returned with live wait");
        contexts_.erase(callback); --at(parent).suspended; selected_ = parent;
    }
    void freeze() { std::lock_guard lock(mutex_); changed(); frozen_ = true; }
    // Host stop may resolve predicates, but cleanup still acquires the one permit.
    void stop(int result) {
        std::lock_guard lock(mutex_); changed();
        stopping_ = true; frozen_ = false;
        for (auto& [id, c] : contexts_) {
            if (!c.live) continue;
            c.terminating = true; c.suspended = 0;
            if (c.wait) {
                c.wait->result = result; c.wait->resolved = true;
                c.wait.reset(); c.blocked = false; c.ready = ++sequence_;
            }
        }
        select();
    }
};
}
