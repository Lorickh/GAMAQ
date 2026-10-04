#include "window_scheduler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>

namespace {
unsigned checks = 0;
void require(bool ok, const char *message) {
    ++checks;
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

struct FakeClock {
    using duration = std::chrono::milliseconds;
    using rep = duration::rep;
    using period = duration::period;
    using time_point = std::chrono::time_point<FakeClock>;
    static constexpr bool is_steady = true;
    static time_point now() { return current; }
    static void reset() { current = time_point{}; }
    static void advance(duration value) { current += value; }
    static time_point current;
};
FakeClock::time_point FakeClock::current {};

class MockBackend {
public:
    explicit MockBackend(uint32_t window) : busy(window, false) {}

    int post(gamaq::TransferRequest request) {
        ++post_calls;
        if (fail_post_at && post_calls == fail_post_at) return post_error;
        if (request.slot >= busy.size() || busy[request.slot]) slot_reused = true;
        else busy[request.slot] = true;
        pending.push_back(request);
        max_pending = std::max(max_pending, pending.size());
        if (cancel_after_posts && post_calls == cancel_after_posts && stop)
            stop->store(true, std::memory_order_relaxed);
        return 0;
    }

    int poll(gamaq::TransferCompletion *out, int max_count) {
        ++poll_calls;
        FakeClock::advance(std::chrono::milliseconds(1));
        if (poll_error) return poll_error;
        if (oversized_poll) return max_count + 1;
        if (inject_unknown) {
            inject_unknown = false;
            out[0] = {999999, 4096, 0, true};
            return 1;
        }
        if (inject_duplicate && max_count >= 2 && !pending.empty()) {
            inject_duplicate = false;
            const auto request = pending.back();
            pending.pop_back();
            busy[request.slot] = false;
            out[0] = {request.request_id, request.bytes, 0, true};
            out[1] = out[0];
            return 2;
        }
        if (stall || pending.empty()) return 0;
        const int count = std::min<int>(max_count, static_cast<int>(pending.size()));
        for (int i = 0; i < count; ++i) {
            const auto request = pending.back();
            pending.pop_back();
            busy[request.slot] = false;
            const bool success = request.request_id != fail_completion_id;
            const uint32_t bytes = request.request_id == short_completion_id
                ? request.bytes - 1 : request.bytes;
            out[i] = {request.request_id, bytes, success ? 0 : completion_error,
                      success};
        }
        return count;
    }

    std::vector<gamaq::TransferRequest> pending;
    std::vector<bool> busy;
    std::atomic<bool> *stop = nullptr;
    size_t max_pending = 0;
    uint64_t post_calls = 0;
    uint64_t poll_calls = 0;
    uint64_t fail_post_at = 0;
    uint64_t cancel_after_posts = 0;
    uint64_t fail_completion_id = 0;
    uint64_t short_completion_id = 0;
    int post_error = -17;
    int poll_error = 0;
    int completion_error = 23;
    bool slot_reused = false;
    bool inject_unknown = false;
    bool inject_duplicate = false;
    bool oversized_poll = false;
    bool stall = false;
};

gamaq::WindowRunResult run(MockBackend &backend, uint32_t window,
                           uint64_t operations, std::atomic<bool> &stop,
                           uint64_t *request_id = nullptr) {
    gamaq::CompletionTracker tracker(window);
    uint64_t local_request_id = request_id ? *request_id : 0;
    FakeClock::reset();
    const auto result = gamaq::run_bounded_window<MockBackend, FakeClock>(
        tracker, local_request_id, 4096, operations, std::chrono::milliseconds(5),
        stop, backend);
    if (request_id) *request_id = local_request_id;
    require(result.in_flight == tracker.in_flight(), "result reports tracker in-flight count");
    require(result.drained == tracker.empty(), "result reports drain state");
    return result;
}
}  // namespace

int main() {
    using gamaq::CompletionError;
    using gamaq::WindowError;

    for (uint32_t window : {1, 2, 4, 8, 16, 32, 64}) {
        std::atomic<bool> stop {false};
        MockBackend backend(window);
        const auto result = run(backend, window, 1024, stop);
        require(result.error == WindowError::none, "healthy window succeeds");
        require(result.submitted == 1024 && result.completed == 1024,
                "healthy window accounts for every operation");
        require(result.safe_to_destroy() && backend.pending.empty(),
                "healthy window drains backend and tracker");
        require(backend.max_pending <= window && !backend.slot_reused,
                "scheduler enforces window and source-slot ownership");
    }

    {
        std::atomic<bool> stop {false};
        MockBackend backend(4);
        backend.fail_post_at = 3;
        const auto result = run(backend, 4, 100, stop);
        require(result.error == WindowError::post_error && result.backend_status == -17,
                "partial post failure preserves backend status");
        require(result.submitted == 2 && result.completed == 2 && backend.post_calls == 3,
                "partial post failure stops new submissions and drains accepted work");
        require(result.safe_to_destroy() && backend.pending.empty(),
                "partial post failure is recoverable after drain");
    }

    {
        std::atomic<bool> stop {false};
        MockBackend backend(4);
        backend.fail_post_at = 3;
        backend.poll_error = -9;
        const auto result = run(backend, 4, 100, stop);
        require(result.error == WindowError::post_error &&
                result.drain_error == WindowError::poll_error &&
                result.drain_backend_status == -9,
                "drain failure is retained behind the primary post error");
        require(!result.safe_to_destroy() && result.in_flight == 2,
                "failed drain keeps accepted requests quarantined");
    }

    {
        std::atomic<bool> stop {false};
        MockBackend backend(8);
        backend.stop = &stop;
        backend.cancel_after_posts = 3;
        const auto result = run(backend, 8, 100, stop);
        require(result.error == WindowError::canceled && backend.post_calls == 3,
                "peer cancellation prevents further posts");
        require(result.completed == 3 && result.safe_to_destroy(),
                "peer cancellation drains accepted requests");
    }

    {
        std::atomic<bool> stop {false};
        MockBackend backend(4);
        backend.fail_completion_id = 2;
        const auto result = run(backend, 4, 100, stop);
        require(result.error == WindowError::invalid_completion &&
                result.completion_error == CompletionError::transport_error,
                "known failed completion is reported");
        require(result.submitted == 4 && result.completed == 3,
                "known completion error stops refilling the window");
        require(result.safe_to_destroy() && backend.pending.empty(),
                "remaining known requests drain after completion error");
    }

    {
        std::atomic<bool> stop {false};
        MockBackend backend(4);
        backend.short_completion_id = 3;
        const auto result = run(backend, 4, 100, stop);
        require(result.error == WindowError::invalid_completion &&
                result.completion_error == CompletionError::length_mismatch &&
                result.expected_bytes == 4096 && result.actual_bytes == 4095,
                "short completion preserves expected and actual lengths");
        require(result.safe_to_destroy(), "known short completion drains safely");
    }

    {
        std::atomic<bool> stop {false};
        MockBackend backend(4);
        backend.inject_unknown = true;
        const auto result = run(backend, 4, 100, stop);
        require(result.error == WindowError::invalid_completion &&
                result.completion_error == CompletionError::unknown_request,
                "unknown completion is rejected");
        require(!result.safe_to_destroy() && result.in_flight == 4,
                "unknown completion preserves tracked ownership for quarantine");
    }

    {
        std::atomic<bool> stop {false};
        MockBackend backend(4);
        backend.inject_duplicate = true;
        const auto result = run(backend, 4, 100, stop);
        require(result.error == WindowError::invalid_completion &&
                result.completion_error == CompletionError::unknown_request &&
                result.completed == 1, "duplicate completion retires a request only once");
        require(!result.safe_to_destroy() && result.in_flight == 3,
                "duplicate completion cannot retire another request");
    }

    {
        std::atomic<bool> stop {false};
        MockBackend backend(4);
        backend.poll_error = -9;
        const auto result = run(backend, 4, 100, stop);
        require(result.error == WindowError::poll_error && result.backend_status == -9,
                "poll error preserves backend status");
        require(!result.safe_to_destroy() && result.in_flight == 4,
                "poll error cannot claim outstanding requests drained");
    }

    {
        std::atomic<bool> stop {false};
        MockBackend backend(2);
        backend.stall = true;
        const auto result = run(backend, 2, 100, stop);
        require(result.error == WindowError::timeout && backend.poll_calls == 256,
                "fake clock deterministically triggers amortized timeout check");
        require(!result.safe_to_destroy() && result.in_flight == 2,
                "timeout quarantines outstanding ownership");
    }

    {
        std::atomic<bool> stop {false};
        MockBackend backend(2);
        backend.oversized_poll = true;
        const auto result = run(backend, 2, 100, stop);
        require(result.error == WindowError::invalid_poll_count &&
                !result.safe_to_destroy(), "invalid backend count is fail-closed");
    }

    {
        std::atomic<bool> stop {false};
        MockBackend backend(1);
        uint64_t request_id = std::numeric_limits<uint64_t>::max();
        const auto result = run(backend, 1, 1, stop, &request_id);
        require(result.error == WindowError::request_id_exhausted &&
                result.safe_to_destroy() && backend.post_calls == 0,
                "request id exhaustion fails before post");
    }

    std::cout << "window_scheduler: " << checks << " checks passed\n";
}
