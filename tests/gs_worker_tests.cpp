#include "../src/gs-backend/rrv_gs_worker.h"

#include <atomic>
#include <cstdlib>
#include <future>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using rrv::gs::Worker;
using namespace std::chrono_literals;

static void check(bool condition, const char* message)
{
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; std::abort(); }
}

template <typename Predicate> static void eventually(Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!predicate())
    {
        check(std::chrono::steady_clock::now() < deadline, "worker test timed out");
        std::this_thread::yield();
    }
}

template <typename F> static bool throws(F&& f)
{
    try { f(); } catch (const std::exception&) { return true; }
    return false;
}

struct Gate
{
    std::promise<void> promise;
    std::shared_future<void> future = promise.get_future().share();
    void wait() { check(future.wait_for(5s) == std::future_status::ready, "gate timed out"); }
    void open() { promise.set_value(); }
};

static void orderAndPayload()
{
    Worker worker({1024, 2, 4});
    Gate gate;
    worker.submit([&] { gate.wait(); });
    std::vector<unsigned char> producer{1, 2, 3, 4};
    std::vector<unsigned char> consumed;
    const auto payload = worker.submit([copy = producer, &consumed] { consumed = copy; }, producer.size());
    producer.assign(4, 99);
    producer.clear(); producer.shrink_to_fit();
    gate.open();
    worker.wait(payload);
    check(consumed == std::vector<unsigned char>({1, 2, 3, 4}), "payload remains owned");
    std::vector<unsigned> order;
    // Repeated admission/removal exceeds the configured queue count many times.
    for (unsigned i = 0; i < 1000; ++i)
        worker.submit([i, &order] { order.push_back(i); }, 3, i % 17 == 0 ? 1 : 0);
    worker.fence();
    for (unsigned i = 0; i < 1000; ++i) check(order[i] == i, "FIFO command order");
    check(worker.invoke([] { return std::make_unique<int>(73); }) != nullptr, "move-only owned result");
    const auto stats = worker.stats();
    check(stats.submitted == stats.completed, "fence consumed every command");
    check(stats.submittedFields == stats.completedFields, "field charges complete");
    check(stats.highWaterCommands <= 4 && stats.highWaterFields <= 2, "queue remains bounded");
    check(stats.outstandingBytes == 0, "payload capacity returned");
    worker.stop();
}

static void multipleProducers()
{
    Worker worker({1024, 2, 8});
    std::vector<unsigned> actual;
    std::map<uint64_t, unsigned> accepted;
    std::mutex records;
    std::vector<std::thread> producers;
    for (unsigned producer = 0; producer < 4; ++producer)
        producers.emplace_back([&, producer] {
            for (unsigned i = 0; i < 200; ++i)
            {
                const unsigned value = producer * 1000 + i;
                const auto sequence = worker.submit([&, value] { actual.push_back(value); }, sizeof(value));
                std::lock_guard lock(records);
                accepted.emplace(sequence, value);
            }
        });
    for (auto& producer : producers) producer.join();
    worker.fence();
    check(actual.size() == 800 && accepted.size() == 800, "all producers delivered");
    size_t i = 0;
    for (const auto& [sequence, value] : accepted)
    {
        check(sequence == i + 1 && actual[i] == value, "consumer follows accepted sequence");
        ++i;
    }
    worker.stop();
}

static void capacityAndFences()
{
    // Test each independent capacity while a command is executing, not merely
    // queued. A second command must block even though the deque itself is empty.
    for (unsigned dimension = 0; dimension < 3; ++dimension)
    {
        Worker worker({8, 1, dimension == 2 ? 1u : 4u});
        Gate gate;
        const size_t bytes = dimension == 0 ? 8 : 0;
        const size_t fields = dimension == 1 ? 1 : 0;
        const auto first = worker.submit([&] { gate.wait(); }, bytes, fields);
        auto second = std::async(std::launch::async, [&] {
            return worker.submit([] {}, dimension == 0 ? 1 : 0, dimension == 1 ? 1 : 0);
        });
        auto waiter = std::async(std::launch::async, [&] { worker.wait(first); });
        eventually([&] { auto s = worker.stats(); return s.backpressureWaits == 1 && s.completionWaits == 1; });
        check(second.wait_for(0s) == std::future_status::timeout, "full capacity blocks producer");
        check(waiter.wait_for(0s) == std::future_status::timeout, "fence awaits actual execution");
        check(throws([&] { worker.submit([] {}, 9); }), "oversized bytes rejected");
        check(throws([&] { worker.submit([] {}, 0, 2); }), "oversized fields rejected");
        check(throws([&] { worker.wait(999); }), "unknown fence rejected");
        gate.open();
        worker.wait(second.get()); waiter.get();
        check(worker.stats().highWaterBytes <= 8, "byte highwater bounded");
        check(worker.stats().highWaterFields <= 1, "field highwater bounded");
        worker.stop();
    }
}

static void failureWakesEveryWaiter()
{
    Gate gate;
    std::atomic<unsigned> finalized{0}, wrapped{0};
    std::thread::id owner, finalOwner;
    Worker worker({8, 1, 2}, [&](const Worker::Command& command) { ++wrapped; command(); },
        [&] { finalOwner = std::this_thread::get_id(); ++finalized; });
    worker.submit([&] { owner = std::this_thread::get_id(); gate.wait(); throw std::runtime_error("fake consumer failed"); }, 8);
    const auto cancelled = worker.submit([] { check(false, "cancelled command executed"); });
    std::vector<std::future<bool>> producers, waiters;
    for (unsigned i = 0; i < 4; ++i)
    {
        producers.push_back(std::async(std::launch::async, [&] { return throws([&] { worker.submit([] {}, 1); }); }));
        waiters.push_back(std::async(std::launch::async, [&] { return throws([&] { worker.wait(cancelled); }); }));
    }
    eventually([&] { auto s = worker.stats(); return s.backpressureWaits == 4 && s.completionWaits == 4; });
    gate.open();
    for (auto& producer : producers) check(producer.get(), "failure wakes blocked producer");
    for (auto& waiter : waiters) check(waiter.get(), "failure wakes completion waiter");
    eventually([&] { return worker.finished(); });
    check(throws([&] { worker.stop(); }), "stop reports consumer failure");
    check(finalized == 1 && wrapped == 2 && finalOwner == owner, "finalizer runs on owner through runner after failure");
    const auto stats = worker.stats();
    check(stats.failed && stats.completed == 0 && stats.outstandingBytes == 0 && stats.outstandingCommands == 0,
        "failure cancels queue and releases accounting");
}

static void shutdownAndOwnership()
{
    for (unsigned iteration = 0; iteration < 20; ++iteration)
    {
        Gate gate;
        std::atomic<bool> finalized{false};
        std::thread::id owner;
        const auto producer = std::this_thread::get_id();
        Worker worker({8, 1, 2}, {}, [&] {
            check(std::this_thread::get_id() == owner, "cleanup on owner"); finalized = true;
        });
        worker.submit([&] {
            owner = std::this_thread::get_id();
            check(owner != producer, "dedicated worker owns commands");
            check(throws([&] { worker.submit([] {}); }), "recursive admission rejected");
            check(throws([&] { worker.fence(); }), "recursive fence rejected");
            check(throws([&] { worker.stop(); }), "recursive stop rejected");
            gate.wait();
        }, 8);
        std::atomic<unsigned> drained{0};
        const auto last = worker.submit([&] { ++drained; });
        auto blocked = std::async(std::launch::async, [&] { return throws([&] { worker.submit([] {}, 1); }); });
        auto waiter = std::async(std::launch::async, [&] { worker.wait(last); });
        eventually([&] { return worker.stats().backpressureWaits == 1; });
        worker.requestStop();
        check(blocked.get(), "shutdown wakes blocked producer");
        check(!worker.finished(), "shutdown does not discard running work");
        check(throws([&] { worker.submit([] {}); }), "shutdown rejects new admission");
        gate.open();
        eventually([&] { return worker.finished(); });
        worker.stop(); worker.stop(); waiter.get();
        check(drained == 1 && finalized, "shutdown drains and finalizes");
    }
    unsigned destroyed = 0;
    { Worker worker; worker.submit([&] { ++destroyed; }); }
    check(destroyed == 1, "destructor joins accepted work");
}

static void ownerThreadQuery()
{
    // Gate-4 owner stream: code running as a command may call the owner's
    // resources directly; only the worker thread reports itself as owner.
    check(!Worker::onOwnerThread(), "producer is not the owner");
    Worker worker;
    check(worker.invoke([] { return Worker::onOwnerThread(); }), "command runs on the owner");
    auto other = std::async(std::launch::async, [] { return Worker::onOwnerThread(); });
    check(!other.get(), "another thread is not the owner");
    check(!Worker::onOwnerThread(), "producer is still not the owner");
}

static void twoWorkerOwnership()
{
    // Gate-5 VU1/GS split: a command on one worker is not the other's owner,
    // so it queues to it (and may wait on it) instead of calling it directly.
    Worker gs, vu;
    check(!gs.ownsThisThread() && !vu.ownsThisThread(), "producer owns neither worker");
    check(vu.invoke([&] { return vu.ownsThisThread() && !gs.ownsThisThread(); }), "vu command owns only vu");
    check(vu.invoke([&] { return gs.invoke([&] { return gs.ownsThisThread() && !vu.ownsThisThread(); }); }),
          "vu command can invoke gs, which runs on gs");
    int order = 0, seen = -1;
    vu.invoke([&] { gs.submit([&] { seen = order++; }); gs.fence(); });
    check(seen == 0 && order == 1, "vu command queues to gs and fences it");
}

int main()
{
    check(throws([] { Worker worker({0, 1, 1}); }), "invalid capacities rejected");
    orderAndPayload();
    twoWorkerOwnership();
    multipleProducers();
    capacityAndFences();
    failureWakesEveryWaiter();
    shutdownAndOwnership();
    ownerThreadQuery();
    std::cout << "GS worker tests passed\n";
}
