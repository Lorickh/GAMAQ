#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "completion_tracker.h"

namespace gamaq {

constexpr int kMaxCompletionBatch = 16;

struct TransferRequest {
    uint64_t request_id = 0;
    uint32_t slot = 0;
    uint32_t bytes = 0;
};

struct TransferCompletion {
    uint64_t request_id = 0;
    uint32_t bytes = 0;
    int transport_status = 0;
    bool success = false;
};

enum class WindowError {
    none,
    canceled,
    request_id_exhausted,
    tracker_error,
    post_error,
    poll_error,
    invalid_poll_count,
    invalid_completion,
    timeout,
};

inline const char *window_error_message(WindowError error) {
    switch (error) {
    case WindowError::none: return "ok";
    case WindowError::canceled: return "window canceled by another worker";
    case WindowError::request_id_exhausted: return "request id space exhausted";
    case WindowError::tracker_error: return "completion tracker rejected a request";
    case WindowError::post_error: return "backend post failed";
    case WindowError::poll_error: return "backend poll failed";
    case WindowError::invalid_poll_count: return "backend returned an invalid completion count";
    case WindowError::invalid_completion: return "completion failed ownership validation";
    case WindowError::timeout: return "completion progress timed out";
    }
    return "unknown window error";
}

struct WindowRunResult {
    WindowError error = WindowError::none;
    WindowError drain_error = WindowError::none;
    CompletionError completion_error = CompletionError::none;
    int backend_status = 0;
    int drain_backend_status = 0;
    int completion_status = 0;
    uint64_t submitted = 0;
    uint64_t completed = 0;
    uint64_t request_id = 0;
    uint32_t expected_bytes = 0;
    uint32_t actual_bytes = 0;
    size_t in_flight = 0;
    bool ownership_known = true;
    bool drained = true;

    bool safe_to_destroy() const { return ownership_known && drained; }
};

// Backend is a zero-overhead compile-time boundary with two operations:
//   int post(TransferRequest)
//   int poll(TransferCompletion *out, int max_count)
// A nonzero post status means the current request was not accepted. A
// negative poll status means no completion in that call can be consumed.
template <typename Backend, typename Clock = std::chrono::steady_clock>
WindowRunResult run_bounded_window(CompletionTracker &tracker,
                                   uint64_t &next_request_id,
                                   uint32_t bytes,
                                   uint64_t operations,
                                   std::chrono::milliseconds timeout,
                                   std::atomic<bool> &stop,
                                   Backend &backend) {
    WindowRunResult result;
    auto deadline = Clock::now() + timeout;
    uint64_t empty_polls = 0;

    auto publish_error = [&](WindowError error, int backend_status = 0) {
        if (result.error == WindowError::none) {
            result.error = error;
            result.backend_status = backend_status;
        } else if (error != result.error && result.drain_error == WindowError::none) {
            result.drain_error = error;
            result.drain_backend_status = backend_status;
        }
        stop.store(true, std::memory_order_relaxed);
    };
    auto finish = [&] {
        result.in_flight = tracker.in_flight();
        result.drained = tracker.empty();
        return result;
    };

    for (;;) {
        if (result.completed == operations && tracker.empty()) return finish();

        if (result.error == WindowError::none &&
            stop.load(std::memory_order_relaxed))
            publish_error(WindowError::canceled);
        if (result.error != WindowError::none && tracker.empty()) return finish();

        while (result.error == WindowError::none && result.submitted < operations &&
               tracker.can_submit() && !stop.load(std::memory_order_relaxed)) {
            if (next_request_id == std::numeric_limits<uint64_t>::max()) {
                publish_error(WindowError::request_id_exhausted);
                break;
            }
            const uint64_t request_id = ++next_request_id;
            const auto reservation = tracker.reserve(request_id, bytes);
            if (reservation.error != CompletionError::none) {
                result.completion_error = reservation.error;
                result.request_id = request_id;
                publish_error(WindowError::tracker_error);
                break;
            }
            const int status = backend.post(
                TransferRequest{request_id, reservation.slot, bytes});
            if (status) {
                tracker.cancel_unposted(request_id);
                result.request_id = request_id;
                publish_error(WindowError::post_error, status);
                break;
            }
            ++result.submitted;
        }

        // A peer may publish stop while this worker is filling its window.
        // Convert that signal into a local cancellation, then drain only the
        // requests already accepted by this backend.
        if (result.error == WindowError::none &&
            stop.load(std::memory_order_relaxed))
            publish_error(WindowError::canceled);
        if (result.error != WindowError::none && tracker.empty()) return finish();

        const int requested = std::min<int>(kMaxCompletionBatch,
            static_cast<int>(tracker.in_flight()));
        if (!requested) {
            publish_error(WindowError::tracker_error);
            return finish();
        }

        std::array<TransferCompletion, kMaxCompletionBatch> completions {};
        const int count = backend.poll(completions.data(), requested);
        if (count < 0) {
            publish_error(WindowError::poll_error, count);
            result.ownership_known = false;
            return finish();
        }
        if (count > requested) {
            publish_error(WindowError::invalid_poll_count, count);
            result.ownership_known = false;
            return finish();
        }
        if (count) {
            for (int i = 0; i < count; ++i) {
                const auto &completion = completions[static_cast<size_t>(i)];
                const auto check = tracker.complete(completion.request_id,
                    completion.success, completion.bytes);
                if (!check.request_retired) {
                    result.completion_error = check.error;
                    result.completion_status = completion.transport_status;
                    result.request_id = completion.request_id;
                    result.actual_bytes = completion.bytes;
                    publish_error(WindowError::invalid_completion);
                    result.ownership_known = false;
                    return finish();
                }
                if (check.error != CompletionError::none) {
                    if (result.completion_error == CompletionError::none) {
                        result.completion_error = check.error;
                        result.completion_status = completion.transport_status;
                        result.request_id = completion.request_id;
                        result.expected_bytes = check.expected_bytes;
                        result.actual_bytes = completion.bytes;
                    }
                    publish_error(WindowError::invalid_completion);
                } else {
                    ++result.completed;
                }
            }
            deadline = Clock::now() + timeout;
            empty_polls = 0;
            continue;
        }

        // Clock reads are amortized in the busy-poll path. Timeout is a stall
        // bound refreshed by any batch that retires at least one request.
        if ((++empty_polls & 0xff) == 0 && Clock::now() >= deadline) {
            publish_error(WindowError::timeout);
            result.ownership_known = false;
            return finish();
        }
    }
}

}  // namespace gamaq
