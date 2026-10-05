#include "completion_tracker.h"

#include <cstdlib>
#include <iostream>
#include <utility>
#include <vector>

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
    require(tracker.reserve(1, 512).error == CompletionError::unconfigured_window,
            "unconfigured tracker rejects work");
    require(!tracker.reset(0), "zero-capacity window rejected");
    require(tracker.reset(2) && tracker.capacity() == 2, "configure two slots");
    require(tracker.reserve(0, 512).error == CompletionError::invalid_request, "zero id");
    require(tracker.reserve(1, 0).error == CompletionError::invalid_request, "zero bytes");
    const auto first = tracker.reserve(1, 512, 42);
    require(first.error == CompletionError::none && first.slot == 0, "reserve first slot");
    require(tracker.reserve(1, 512).error == CompletionError::duplicate_request, "duplicate id");
    require(tracker.in_flight() == 1, "duplicate does not change ownership");

    auto check = tracker.complete(9, true, 512);
    require(check.error == CompletionError::unknown_request && !check.request_retired,
            "unknown completion cannot retire another request");
    require(tracker.in_flight() == 1, "unknown completion preserves pending request");
    check = tracker.complete(1, true, 256);
    require(check.error == CompletionError::length_mismatch && check.request_retired &&
            check.expected_bytes == 512 && check.user_context == 42,
            "length mismatch retires request and preserves caller context");
    require(tracker.empty(), "length failure leaves no phantom in-flight request");

    require(tracker.reserve(2, 1024).error == CompletionError::none, "submit transport error case");
    check = tracker.complete(2, false, 0);
    require(check.error == CompletionError::transport_error && check.request_retired &&
            check.expected_bytes == 1024, "transport error retires named request");
    require(tracker.empty(), "transport error drained");

    require(tracker.reserve(3, 2048).error == CompletionError::none, "submit canceled post");
    require(tracker.cancel_unposted(3), "cancel failed post");
    require(!tracker.cancel_unposted(3) && tracker.empty(), "cancel is exact once");

    require(tracker.reset(64), "resize drained tracker");
    for (uint64_t id = 10; id < 74; ++id) {
        const auto reservation = tracker.reserve(id, static_cast<uint32_t>(id * 8));
        require(reservation.error == CompletionError::none && reservation.slot == id - 10,
                "fill 64-item window");
    }
    require(tracker.in_flight() == 64 && !tracker.can_submit(), "64-item window is full");
    require(tracker.reserve(100, 512).error == CompletionError::window_full,
            "window enforces in-flight bound");
    for (uint64_t id = 73; id >= 10; --id) {
        check = tracker.complete(id, true, static_cast<uint32_t>(id * 8));
        require(check.error == CompletionError::none && check.request_retired &&
                check.slot == id - 10, "reverse completion releases exact slot");
        const auto replacement = tracker.reserve(id + 100, 512);
        require(replacement.error == CompletionError::none && replacement.slot == id - 10,
                "only completed slot is reused");
        check = tracker.complete(id + 100, true, 512);
        require(check.error == CompletionError::none, "replacement completes");
        if (id == 10) break;
    }
    require(tracker.empty() && tracker.can_submit(), "reverse-order window drained");

    check = tracker.complete(73, false, 0);
    require(check.error == CompletionError::unknown_request && !check.request_retired,
            "duplicate completion rejected");

    // Reference scheduler: repeatedly fill each supported window, complete a
    // deliberately non-FIFO request, and prove no busy slot is ever reused.
    for (uint32_t capacity : {1, 2, 4, 8, 16, 32, 64}) {
        require(tracker.reset(capacity), "configure reference scheduler window");
        std::vector<bool> busy(capacity, false);
        std::vector<std::pair<uint64_t, uint32_t>> in_flight;
        uint64_t next = 1000;
        uint64_t completed = 0;
        constexpr uint64_t operations = 1024;
        while (completed < operations) {
            while (next < 1000 + operations && tracker.can_submit()) {
                const auto reservation = tracker.reserve(next, 4096);
                require(reservation.error == CompletionError::none,
                        "reference scheduler reserves within bound");
                require(reservation.slot < capacity && !busy[reservation.slot],
                        "reference scheduler never reuses busy slot");
                busy[reservation.slot] = true;
                in_flight.emplace_back(next++, reservation.slot);
                require(tracker.in_flight() == in_flight.size() &&
                        tracker.in_flight() <= capacity, "reference scheduler is bounded");
            }
            const size_t chosen = static_cast<size_t>((completed * 17) % in_flight.size());
            const auto request = in_flight[chosen];
            check = tracker.complete(request.first, true, 4096);
            require(check.error == CompletionError::none && check.slot == request.second,
                    "non-FIFO completion releases matching slot");
            require(busy[check.slot], "completion releases a previously busy slot");
            busy[check.slot] = false;
            in_flight[chosen] = in_flight.back();
            in_flight.pop_back();
            ++completed;
        }
        require(tracker.empty() && in_flight.empty(), "reference scheduler drains window");
    }

    std::cout << "completion_tracker: " << checks << " checks passed\n";
}
