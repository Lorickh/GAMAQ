#include "completion_tracker.h"

#include <cstdlib>
#include <iostream>

namespace {
unsigned checks = 0;
void require(bool ok, const char *message) {
    ++checks;
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
}

int main() {
    using gamaq::CompletionError;
    using gamaq::CompletionTracker;

    CompletionTracker tracker;
    require(tracker.empty(), "starts empty");
    require(tracker.submit(0, 512) == CompletionError::invalid_request, "zero id");
    require(tracker.submit(1, 0) == CompletionError::invalid_request, "zero bytes");
    require(tracker.submit(1, 512) == CompletionError::none, "submit request");
    require(tracker.submit(1, 512) == CompletionError::duplicate_request, "duplicate id");
    require(tracker.in_flight() == 1, "duplicate does not change ownership");

    auto check = tracker.complete(9, true, 512);
    require(check.error == CompletionError::unknown_request && !check.request_retired,
            "unknown completion cannot retire another request");
    require(tracker.in_flight() == 1, "unknown completion preserves pending request");
    check = tracker.complete(1, true, 256);
    require(check.error == CompletionError::length_mismatch && check.request_retired &&
            check.expected_bytes == 512, "length mismatch retires named request as failed");
    require(tracker.empty(), "length failure leaves no phantom in-flight request");

    require(tracker.submit(2, 1024) == CompletionError::none, "submit transport error case");
    check = tracker.complete(2, false, 0);
    require(check.error == CompletionError::transport_error && check.request_retired &&
            check.expected_bytes == 1024, "transport error retires named request");
    require(tracker.empty(), "transport error drained");

    require(tracker.submit(3, 2048) == CompletionError::none, "submit canceled post");
    require(tracker.cancel_unposted(3), "cancel failed post");
    require(!tracker.cancel_unposted(3) && tracker.empty(), "cancel is exact once");

    // Future windows may complete out of submission order. Every completion
    // remains correlated by user_ctx rather than queue position.
    for (uint64_t id = 10; id < 74; ++id)
        require(tracker.submit(id, static_cast<uint32_t>(id * 8)) == CompletionError::none,
                "submit window item");
    require(tracker.in_flight() == 64, "64-item bounded window");
    for (uint64_t id = 73; id >= 10; --id) {
        check = tracker.complete(id, true, static_cast<uint32_t>(id * 8));
        require(check.error == CompletionError::none && check.request_retired,
                "reverse-order completion");
        if (id == 10) break;
    }
    require(tracker.empty(), "reverse-order window drained");

    check = tracker.complete(73, false, 0);
    require(check.error == CompletionError::unknown_request && !check.request_retired,
            "duplicate completion rejected");

    std::cout << "completion_tracker: " << checks << " checks passed\n";
}
