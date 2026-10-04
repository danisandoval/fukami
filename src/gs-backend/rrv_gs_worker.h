#ifndef RRV_GS_WORKER_H
#define RRV_GS_WORKER_H

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#if defined(__linux__)
#include <pthread.h>
#endif

namespace rrv::gs
{
// One FIFO owner. submit() copies/moves an OWNED callable: captures referring
// to producer memory must instead own a copy, or be kept alive through wait().
// Costs cover queued AND executing commands until their captures are released.
// The caller supplies exact payload bytes (including owned register snapshots).
// Command count separately bounds transport bookkeeping. Oversized work throws;
// it is never silently split, bypassed, or executed on the submitting thread.
//
// Concurrent submissions linearize at admission under mutex_. If the guest has
// an authoritative order before this boundary, its existing serializer must
// cover admission too. Consumer work must not acquire that guest serializer:
// producers may block for capacity/completion while holding it.
//
// stop() rejects new/blocked submissions, drains accepted work, then joins.
// A failing command cancels the remaining queue and wakes EVERY waiter. A
// running callable cannot be forcibly interrupted. Destruction requires that
// all external users have stopped accessing this object, as for std::mutex.
class Worker final
{
public:
    using Command = std::function<void()>;
    // Must call the command exactly once, synchronously. Useful for a platform
    // autorelease pool around each command without Objective-C in this header.
    using Runner = std::function<void(const Command&)>;
    struct Limits
    {
        size_t bytes = 16u * 1024u * 1024u;
        size_t fields = 2u;
        size_t commands = 4096u;
        // Separate observation runs only. In particular worker-sync may wait
        // on every packet; its ordinary path must not add clock pairs.
        bool measureWaitTime = false;
        // Linux thread name for the [cpu] log (15 characters at most).
        const char* threadName = "rrv-gs-owner";
    };
    struct Stats
    {
        uint64_t submitted = 0, completed = 0;
        uint64_t submittedFields = 0, completedFields = 0;
        size_t outstandingBytes = 0, outstandingFields = 0, outstandingCommands = 0;
        size_t highWaterBytes = 0, highWaterFields = 0, highWaterCommands = 0;
        uint64_t backpressureWaits = 0, backpressureNanoseconds = 0;
        uint64_t completionWaits = 0, completionNanoseconds = 0;
        bool failed = false, accepting = true, finished = false;
    };

    Worker() : Worker(Limits{}) {}
    explicit Worker(Limits limits, Runner runner = {}, Command onExit = {})
        : limits_(limits), runner_(std::move(runner)), onExit_(std::move(onExit))
    {
        if (!limits_.bytes || !limits_.fields || !limits_.commands)
            throw std::invalid_argument("GS worker capacities must be positive");
        thread_ = std::thread([this] { run(); });
    }
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;
    ~Worker() { try { stop(); } catch (...) {} }

    uint64_t submit(Command command, size_t bytes = 0, size_t fields = 0)
    {
        if (!command)
            throw std::invalid_argument("GS worker empty command");
        if (bytes > limits_.bytes || fields > limits_.fields)
            throw std::length_error("GS worker command exceeds capacity");
        std::unique_lock lock(mutex_);
        rejectOwnerWait();
        checkAccepting();
        const auto ready = [&] { return failure_ || !stats_.accepting || hasCapacity(bytes, fields); };
        if (!ready())
        {
            ++stats_.backpressureWaits;
            const auto begin = limits_.measureWaitTime ? Clock::now() : Clock::time_point{};
            capacity_.wait(lock, ready);
            if (limits_.measureWaitTime) stats_.backpressureNanoseconds += elapsed(begin);
        }
        checkAccepting();
        const uint64_t sequence = stats_.submitted + 1;
        queue_.push_back({sequence, bytes, fields, std::move(command)});
        stats_.submitted = sequence;
        stats_.submittedFields += fields;
        stats_.outstandingBytes += bytes;
        stats_.outstandingFields += fields;
        ++stats_.outstandingCommands;
        stats_.highWaterBytes = std::max(stats_.highWaterBytes, stats_.outstandingBytes);
        stats_.highWaterFields = std::max(stats_.highWaterFields, stats_.outstandingFields);
        stats_.highWaterCommands = std::max(stats_.highWaterCommands, stats_.outstandingCommands);
        work_.notify_one();
        return sequence;
    }

    void wait(uint64_t sequence)
    {
        std::unique_lock lock(mutex_);
        rejectOwnerWait();
        if (sequence > stats_.submitted)
            throw std::invalid_argument("GS worker fence was never submitted");
        const auto ready = [&] { return failure_ || stats_.completed >= sequence; };
        if (!ready())
        {
            ++stats_.completionWaits;
            const auto begin = limits_.measureWaitTime ? Clock::now() : Clock::time_point{};
            completion_.wait(lock, ready);
            if (limits_.measureWaitTime) stats_.completionNanoseconds += elapsed(begin);
        }
        rethrowFailure();
    }

    // Wait for all commands accepted before this snapshot; not GPU completion.
    void fence()
    {
        uint64_t sequence;
        { std::lock_guard lock(mutex_); sequence = stats_.submitted; }
        wait(sequence);
    }

    template <typename F>
    auto invoke(F&& function, size_t bytes = 0, size_t fields = 0)
        -> std::invoke_result_t<std::decay_t<F>&>
    {
        using Result = std::invoke_result_t<std::decay_t<F>&>;
        static_assert(!std::is_reference_v<Result>, "Return an owned result");
        auto owned = std::make_shared<std::decay_t<F>>(std::forward<F>(function));
        if constexpr (std::is_void_v<Result>)
        {
            wait(submit([owned] { std::invoke(*owned); }, bytes, fields));
        }
        else
        {
            auto result = std::make_shared<std::optional<Result>>();
            wait(submit([owned, result] { result->emplace(std::invoke(*owned)); }, bytes, fields));
            return std::move(**result);
        }
    }

    Stats stats() const { std::lock_guard lock(mutex_); return stats_; }

    // True on a Worker owner thread (any Worker in the process). Lets code
    // already running as a command call the owner's resources directly
    // instead of submitting (which would self-deadlock and is rejected).
    static bool onOwnerThread() { return tCurrent() != nullptr; }
    // True only on THIS worker's owner thread. With two workers (the Gate-5
    // VU1/GS split) a command on one worker must still queue to the other.
    bool ownsThisThread() const { return tCurrent() == this; }

    // The platform owner may need to pump its event queue while the GS owner
    // tears down resources. Request first, pump until finished(), then join.
    void requestStop()
    {
        {
            std::lock_guard lock(mutex_);
            stats_.accepting = false;
        }
        wakeAll();
    }
    bool finished() const { std::lock_guard lock(mutex_); return stats_.finished; }

    void stop()
    {
        // Do not take joinMutex_ on the owner: another stop might be joining it.
        { std::lock_guard lock(mutex_); rejectOwnerWait(); }
        requestStop();
        std::lock_guard joining(joinMutex_);
        if (thread_.joinable()) thread_.join();
        std::lock_guard lock(mutex_);
        rethrowFailure();
    }

private:
    using Clock = std::chrono::steady_clock;
    struct Entry { uint64_t sequence; size_t bytes, fields; Command command; };
    static uint64_t elapsed(Clock::time_point begin)
    {
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            Clock::now() - begin).count());
    }
    bool hasCapacity(size_t bytes, size_t fields) const
    {
        return bytes <= limits_.bytes - stats_.outstandingBytes &&
            fields <= limits_.fields - stats_.outstandingFields &&
            stats_.outstandingCommands < limits_.commands;
    }
    void rejectOwnerWait() const
    {
        if (owner_ == std::this_thread::get_id())
            throw std::logic_error("GS worker cannot submit/wait/stop from its owner");
    }
    void rethrowFailure() const { if (failure_) std::rethrow_exception(failure_); }
    void checkAccepting() const
    {
        rethrowFailure();
        if (!stats_.accepting) throw std::runtime_error("GS worker stopped");
    }
    void wakeAll() { work_.notify_all(); capacity_.notify_all(); completion_.notify_all(); }
    void fail(std::exception_ptr error)
    {
        std::deque<Entry> cancelled;
        {
            std::lock_guard lock(mutex_);
            if (!failure_) failure_ = error;
            stats_.failed = true;
            stats_.accepting = false;
            stats_.outstandingBytes = stats_.outstandingFields = stats_.outstandingCommands = 0;
            cancelled.swap(queue_);
        }
        // Destroy captures outside the transport lock.
        cancelled.clear();
        wakeAll();
    }
    static const Worker*& tCurrent() { static thread_local const Worker* current = nullptr; return current; }
    void run() noexcept
    {
#if defined(__linux__)
        pthread_setname_np(pthread_self(), limits_.threadName); // [cpu] log (Gate 5)
#endif
        { std::lock_guard lock(mutex_); owner_ = std::this_thread::get_id(); }
        tCurrent() = this;
        try
        {
            for (;;)
            {
                Entry entry;
                {
                    std::unique_lock lock(mutex_);
                    work_.wait(lock, [&] { return !queue_.empty() || !stats_.accepting; });
                    if (queue_.empty()) break;
                    entry = std::move(queue_.front());
                    queue_.pop_front();
                }
                if (runner_) runner_(entry.command); else entry.command();
                entry.command = {};
                {
                    std::lock_guard lock(mutex_);
                    stats_.completed = entry.sequence;
                    stats_.completedFields += entry.fields;
                    stats_.outstandingBytes -= entry.bytes;
                    stats_.outstandingFields -= entry.fields;
                    --stats_.outstandingCommands;
                }
                capacity_.notify_all();
                completion_.notify_all();
            }
        }
        catch (...) { fail(std::current_exception()); }
        // Final owner-thread cleanup also runs after command failure. It must
        // not invoke/submit; it can release renderer resources directly.
        try { if (onExit_) { if (runner_) runner_(onExit_); else onExit_(); } }
        catch (...) { fail(std::current_exception()); }
        { std::lock_guard lock(mutex_); stats_.finished = true; }
        wakeAll();
    }

    const Limits limits_;
    Runner runner_;
    Command onExit_;
    mutable std::mutex mutex_;
    std::mutex joinMutex_;
    std::condition_variable work_, capacity_, completion_;
    std::deque<Entry> queue_;
    Stats stats_;
    std::exception_ptr failure_;
    std::thread::id owner_;
    std::thread thread_;
};
} // namespace rrv::gs

#endif
