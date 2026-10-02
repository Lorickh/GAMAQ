#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace gamaq {

enum class CompletionError {
    none,
    invalid_request,
    duplicate_request,
    unknown_request,
    transport_error,
    length_mismatch,
};

inline const char *completion_error_message(CompletionError error) {
    switch (error) {
    case CompletionError::none: return "ok";
    case CompletionError::invalid_request: return "request id and byte count must be nonzero";
    case CompletionError::duplicate_request: return "request id is already in flight";
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
};

// SDK-independent ownership tracker for signaled WRs. It deliberately does
// not infer ordering: every CR must name a currently in-flight request.
class CompletionTracker {
public:
    CompletionError submit(uint64_t request_id, uint32_t bytes) {
        if (!request_id || !bytes) return CompletionError::invalid_request;
        return pending_.emplace(request_id, bytes).second
            ? CompletionError::none : CompletionError::duplicate_request;
    }

    // A failed single-WR post did not transfer ownership to the provider.
    bool cancel_unposted(uint64_t request_id) { return pending_.erase(request_id) == 1; }

    CompletionCheck complete(uint64_t request_id, bool transport_success,
                             uint32_t completion_bytes) {
        auto item = pending_.find(request_id);
        if (item == pending_.end()) return {CompletionError::unknown_request, false, 0};
        const uint32_t expected = item->second;
        pending_.erase(item);
        if (!transport_success) return {CompletionError::transport_error, true, expected};
        if (completion_bytes != expected)
            return {CompletionError::length_mismatch, true, expected};
        return {CompletionError::none, true, expected};
    }

    size_t in_flight() const { return pending_.size(); }
    bool empty() const { return pending_.empty(); }

private:
    std::unordered_map<uint64_t, uint32_t> pending_;
};

}  // namespace gamaq
