#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace gamaq {

enum class CompletionError {
    none,
    invalid_request,
    unconfigured_window,
    duplicate_request,
    window_full,
    unknown_request,
    transport_error,
    length_mismatch,
};

inline const char *completion_error_message(CompletionError error) {
    switch (error) {
    case CompletionError::none: return "ok";
    case CompletionError::invalid_request: return "request id and byte count must be nonzero";
    case CompletionError::unconfigured_window: return "completion window is not configured";
    case CompletionError::duplicate_request: return "request id is already in flight";
    case CompletionError::window_full: return "completion window has no free source slot";
    case CompletionError::unknown_request: return "completion does not match an in-flight request";
    case CompletionError::transport_error: return "completion reports a transport error";
    case CompletionError::length_mismatch: return "completion length differs from the submitted byte count";
    }
    return "unknown completion error";
}

struct CompletionCheck {
    CompletionError error = CompletionError::none;
    bool request_retired = false;
    uint32_t expected_bytes = 0;
    uint32_t slot = 0;
};

struct SubmissionReservation {
    CompletionError error = CompletionError::none;
    uint32_t slot = 0;
};

// SDK-independent ownership tracker for signaled WRs and their reusable
// source slots. It deliberately does not infer ordering: every CR must name a
// currently in-flight request, and only that request's slot is released.
class CompletionTracker {
public:
    CompletionTracker() = default;
    explicit CompletionTracker(uint32_t capacity) { reset(capacity); }

    bool reset(uint32_t capacity) {
        if (!pending_.empty() || !capacity) return false;
        free_slots_.clear();
        free_slots_.reserve(capacity);
        for (uint32_t slot = capacity; slot > 0; --slot)
            free_slots_.push_back(slot - 1);
        capacity_ = capacity;
        return true;
    }

    SubmissionReservation reserve(uint64_t request_id, uint32_t bytes) {
        if (!request_id || !bytes)
            return {CompletionError::invalid_request, 0};
        if (!capacity_)
            return {CompletionError::unconfigured_window, 0};
        if (pending_.find(request_id) != pending_.end())
            return {CompletionError::duplicate_request, 0};
        if (free_slots_.empty())
            return {CompletionError::window_full, 0};
        const uint32_t slot = free_slots_.back();
        free_slots_.pop_back();
        pending_.emplace(request_id, Pending{bytes, slot});
        return {CompletionError::none, slot};
    }

    // A failed single-WR post did not transfer ownership to the provider.
    bool cancel_unposted(uint64_t request_id) {
        auto item = pending_.find(request_id);
        if (item == pending_.end()) return false;
        free_slots_.push_back(item->second.slot);
        pending_.erase(item);
        return true;
    }

    CompletionCheck complete(uint64_t request_id, bool transport_success,
                             uint32_t completion_bytes) {
        auto item = pending_.find(request_id);
        if (item == pending_.end())
            return {CompletionError::unknown_request, false, 0, 0};
        const Pending pending = item->second;
        free_slots_.push_back(pending.slot);
        pending_.erase(item);
        if (!transport_success)
            return {CompletionError::transport_error, true, pending.bytes, pending.slot};
        if (completion_bytes != pending.bytes)
            return {CompletionError::length_mismatch, true, pending.bytes, pending.slot};
        return {CompletionError::none, true, pending.bytes, pending.slot};
    }

    size_t in_flight() const { return pending_.size(); }
    bool empty() const { return pending_.empty(); }
    bool can_submit() const { return !free_slots_.empty(); }
    uint32_t capacity() const { return capacity_; }

private:
    struct Pending { uint32_t bytes; uint32_t slot; };
    uint32_t capacity_ = 0;
    std::vector<uint32_t> free_slots_;
    std::unordered_map<uint64_t, Pending> pending_;
};

}  // namespace gamaq
