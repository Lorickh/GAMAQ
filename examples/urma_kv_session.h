#pragma once

#include <atomic>
#include <chrono>
#include <vector>

#include "kv_put_batch.h"
#include "urma_session.h"

namespace gamaq {

enum class UrmaKvSessionError {
    none,
    session_not_open,
    session_quarantined,
    session_retired,
    requests_in_flight,
    invalid_timeout,
};

inline const char *urma_kv_session_error_message(UrmaKvSessionError error) {
    switch (error) {
    case UrmaKvSessionError::none: return "ok";
    case UrmaKvSessionError::session_not_open: return "URMA session is not open";
    case UrmaKvSessionError::session_quarantined: return "URMA session ownership is unknown";
    case UrmaKvSessionError::session_retired: return "URMA session must be closed before reuse";
    case UrmaKvSessionError::requests_in_flight: return "URMA session already owns in-flight requests";
    case UrmaKvSessionError::invalid_timeout: return "KV put timeout must be positive";
    }
    return "unknown URMA KV session error";
}

struct UrmaKvPutSessionResult {
    UrmaKvSessionError error = UrmaKvSessionError::none;
    KvPutBatchResult batch;

    bool started() const { return error == UrmaKvSessionError::none; }
};

// Synchronous session-facing entry point for CPU -> NPU KV put/offload. The
// caller fills session.buffer() and describes offsets within that registered
// source region. A clean result keeps the session reusable. A drained runtime
// failure retires it so close() remains safe, while unknown ownership
// quarantines it and prevents both reuse and provider teardown.
inline UrmaKvPutSessionResult run_urma_kv_put_batch(
        UrmaSession &session, const std::vector<KvPutBlock> &blocks,
        std::chrono::milliseconds timeout, std::atomic<bool> &stop) {
    UrmaKvPutSessionResult result;
    if (session.is_quarantined()) {
        result.error = UrmaKvSessionError::session_quarantined;
        return result;
    }
    if (!session.is_open()) {
        result.error = UrmaKvSessionError::session_not_open;
        return result;
    }
    if (session.is_retired()) {
        result.error = UrmaKvSessionError::session_retired;
        return result;
    }
    if (session.in_flight()) {
        result.error = UrmaKvSessionError::requests_in_flight;
        return result;
    }
    if (timeout.count() <= 0) {
        result.error = UrmaKvSessionError::invalid_timeout;
        return result;
    }

    UrmaKvPutBackend backend(session.jetty(), session.jfc(),
        session.remote_jetty(), session.source_binding(),
        session.destination_binding());
    result.batch = run_kv_put_batch(session.completions(), session.request_id(),
        blocks, session.buffer_bytes(), session.destination_binding().region.bytes,
        timeout, stop, backend);

    if (!result.batch.valid()) return result;
    if (!result.batch.window.safe_to_destroy()) {
        session.quarantine();
    } else if (result.batch.window.error != WindowError::none &&
               result.batch.window.error != WindowError::canceled) {
        session.retire();
    }
    return result;
}

inline UrmaKvPutSessionResult run_urma_kv_put_batch(
        UrmaSession &session, const std::vector<KvPutBlock> &blocks,
        std::chrono::milliseconds timeout) {
    std::atomic<bool> stop {false};
    return run_urma_kv_put_batch(session, blocks, timeout, stop);
}

}  // namespace gamaq
