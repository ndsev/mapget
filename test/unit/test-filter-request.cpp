#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "mapget/service/service.h"

#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;

namespace mapget
{
/** Test the wait handoff without exported APIs or runtime instrumentation. */
struct FilterRequestWaitTest
{
    static auto lockStatus(FeatureLayerFilterTilesRequest& request)
    {
        return std::unique_lock(request.statusMutex_);
    }

    static void complete(FeatureLayerFilterTilesRequest& request, RequestStatus status)
    {
        request.setStatus(status);
    }

    static void cancel(FeatureLayerFilterTilesRequest& request) { request.cancel(); }

    static void notify(FeatureLayerFilterTilesRequest& request) { request.notifyStatus(); }

    /** A bounded counterpart of wait(), sharing the actual request mutex and condition. */
    static bool wait(FeatureLayerFilterTilesRequest& request, std::promise<void> entered)
    {
        auto lock = lockStatus(request);
        entered.set_value();
        auto const deadline = std::chrono::steady_clock::now() + 2s;
        while (!request.isDone()) {
            if (request.statusConditionVariable_.wait_until(lock, deadline) ==
                std::cv_status::timeout)
                return false;
        }
        return true;
    }
};
}  // namespace mapget

using namespace mapget;

TEST_CASE(
    "Filter terminal publication respects the wait mutex",
    "[Service][filter-request][concurrency]")
{
    auto const terminal = GENERATE(
        RequestStatus::Success,
        RequestStatus::Aborted,
        RequestStatus::NoDataSource,
        RequestStatus::Unauthorized);
    CAPTURE(static_cast<int>(terminal));
    auto request = std::make_shared<FeatureLayerFilterTilesRequest>(
        "Map",
        "Layer",
        std::vector<PartitionId>{TileId::fromTileXY(1, 0, 1)},
        FeatureLayerFilterRequest{});
    auto waitLock = FilterRequestWaitTest::lockStatus(*request);
    REQUIRE_FALSE(request->isDone());

    std::promise<void> started, completed;
    auto startedFuture = started.get_future();
    auto completedFuture = completed.get_future();
    std::thread publisher(
        [request,
         terminal,
         started = std::move(started),
         completed = std::move(completed)]() mutable
        {
            started.set_value();
            FilterRequestWaitTest::complete(*request, terminal);
            completed.set_value();
        });
    bool const entered = startedFuture.wait_for(2s) == std::future_status::ready;
    // Hold the same lock that covers wait()'s false-predicate-to-sleep interval.
    // No publisher may change the predicate or finish notification inside it.
    bool const publishedWhileLocked = completedFuture.wait_for(100ms) == std::future_status::ready;
    auto const statusWhileLocked = request->getStatus();
    waitLock.unlock();
    bool const finished = completedFuture.wait_for(2s) == std::future_status::ready;
    if (finished)
        publisher.join();
    else
        publisher.detach();  // Failure cleanup is bounded; the thread owns every capture.

    REQUIRE(entered);
    REQUIRE_FALSE(publishedWhileLocked);
    REQUIRE(statusWhileLocked == RequestStatus::Open);
    REQUIRE(finished);
    REQUIRE(request->getStatus() == terminal);
}

TEST_CASE(
    "Filter terminal callbacks wake all waiters even when they throw",
    "[Service][filter-request][concurrency]")
{
    auto const terminal = GENERATE(
        RequestStatus::Success,
        RequestStatus::Aborted,
        RequestStatus::NoDataSource,
        RequestStatus::Unauthorized);
    auto const throwing = GENERATE(0, 1, 2);
    CAPTURE(static_cast<int>(terminal), throwing);
    auto request = std::make_shared<FeatureLayerFilterTilesRequest>(
        "Map",
        "Layer",
        std::vector<PartitionId>{TileId::fromTileXY(1, 0, 1)},
        FeatureLayerFilterRequest{});
    std::vector<std::thread> waiters;
    std::vector<std::future<bool>> results;
    bool allEntered = true;
    for (int index = 0; index < 4; ++index) {
        std::promise<void> entered;
        auto started = entered.get_future();
        std::promise<bool> result;
        results.push_back(result.get_future());
        waiters.emplace_back(
            [request, entered = std::move(entered), result = std::move(result)]() mutable
            { result.set_value(FilterRequestWaitTest::wait(*request, std::move(entered))); });
        allEntered &= started.wait_for(2s) == std::future_status::ready;
    }
    // Each waiter signalled while holding statusMutex_. Acquiring it here proves
    // they have released it into the condition wait, not merely started a thread.
    {
        auto handoff = FilterRequestWaitTest::lockStatus(*request);
    }
    int callbacks = 0;
    request->onDone_ = [&, throwing](RequestStatus)
    {
        ++callbacks;
        if (throwing == 1)
            throw std::runtime_error("terminal consumer failed");
        if (throwing == 2)
            throw 42;
    };
    bool callbackThrew = false;
    try {
        FilterRequestWaitTest::complete(*request, terminal);
    }
    catch (...) {
        callbackThrew = true;
    }
    FilterRequestWaitTest::complete(*request, RequestStatus::Aborted);
    bool allWoken = true;
    for (size_t index = 0; index < waiters.size(); ++index) {
        bool const finished = results[index].wait_for(3s) == std::future_status::ready;
        if (finished) {
            waiters[index].join();
            allWoken &= results[index].get();
        }
        else {
            waiters[index].detach();  // Only owned request/promises outlive this failing test.
            allWoken = false;
        }
    }
    REQUIRE(allEntered);
    REQUIRE(allWoken);
    REQUIRE(callbackThrew == (throwing != 0));
    REQUIRE(callbacks == 1);
    REQUIRE(request->getStatus() == terminal);
}

TEST_CASE(
    "Filter terminal callbacks can reenter wait and cancellation",
    "[Service][filter-request][concurrency]")
{
    bool const cancelFirst = GENERATE(false, true);
    bool const empty = GENERATE(false, true);
    CAPTURE(cancelFirst, empty);
    auto request = std::make_shared<FeatureLayerFilterTilesRequest>(
        "Map",
        "Layer",
        empty ? std::vector<PartitionId>{} : std::vector<PartitionId>{TileId::fromTileXY(1, 0, 1)},
        FeatureLayerFilterRequest{});
    auto callbacks = std::make_shared<std::atomic_int>(0);
    request->onDone_ = [weak = std::weak_ptr(request), callbacks](RequestStatus)
    {
        ++*callbacks;
        auto current = weak.lock();
        current->wait();
        FilterRequestWaitTest::cancel(*current);
        FilterRequestWaitTest::complete(*current, RequestStatus::NoDataSource);
    };
    std::promise<void> finished;
    auto done = finished.get_future();
    std::thread publisher(
        [request, cancelFirst, empty, finished = std::move(finished)]() mutable
        {
            if (empty) {
                // Service/HttpClient explicitly notify already-complete requests on
                // submission. Preserve that behavior, including explicit re-submission.
                FilterRequestWaitTest::notify(*request);
                FilterRequestWaitTest::notify(*request);
            }
            else if (cancelFirst)
                FilterRequestWaitTest::cancel(*request);
            else
                FilterRequestWaitTest::complete(*request, RequestStatus::Success);
            finished.set_value();
        });
    bool const returned = done.wait_for(2s) == std::future_status::ready;
    if (returned)
        publisher.join();
    else
        publisher.detach();  // A lock-order regression must fail, not hang the test process.
    REQUIRE(returned);
    REQUIRE(callbacks->load() == (empty ? 2 : 1));
    REQUIRE(request->isCancelled());
    REQUIRE(
        request->getStatus() ==
        (cancelFirst && !empty ? RequestStatus::Aborted : RequestStatus::Success));
}
