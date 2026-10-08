#include "urma_kv_session.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {
unsigned checks = 0;
void require(bool ok, const char *message) {
    ++checks;
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

enum class OpenFault {
    none,
    context,
    query,
    jfc,
    jfr,
    jetty,
    local_segment,
    remote_segment,
    remote_jetty,
};

OpenFault open_fault = OpenFault::none;
gamaq::UrmaSessionResource close_fault = gamaq::UrmaSessionResource::none;
std::vector<std::string> events;
int live_resources = 0;
uint32_t max_jfc_depth = 65;
uint32_t max_jfs_depth = 64;
uint32_t max_jfr_depth = 1;
uint32_t captured_jfc_depth = 0;
uint32_t captured_jfs_depth = 0;
uint64_t captured_buffer_address = 0;
uint64_t captured_buffer_bytes = 0;
struct CapturedWrite {
    uint64_t request_id = 0;
    uint64_t source_address = 0;
    uint64_t destination_address = 0;
    uint32_t bytes = 0;
    urma_target_seg_t *source_segment = nullptr;
    urma_target_seg_t *destination_segment = nullptr;
};
std::vector<CapturedWrite> pending_writes;
std::vector<CapturedWrite> write_history;
int post_calls = 0;
int poll_calls = 0;
int fail_post_at = 0;
int poll_error = 0;
bool fail_next_completion = false;
urma_target_seg_t *const local_segment_handle =
    reinterpret_cast<urma_target_seg_t *>(uintptr_t{0x106});
urma_target_seg_t *const remote_segment_handle =
    reinterpret_cast<urma_target_seg_t *>(uintptr_t{0x107});

template <typename T>
T *handle(uintptr_t value) {
    return reinterpret_cast<T *>(value);
}

void reset_provider() {
    open_fault = OpenFault::none;
    close_fault = gamaq::UrmaSessionResource::none;
    events.clear();
    live_resources = 0;
    max_jfc_depth = 65;
    max_jfs_depth = 64;
    max_jfr_depth = 1;
    captured_jfc_depth = 0;
    captured_jfs_depth = 0;
    captured_buffer_address = 0;
    captured_buffer_bytes = 0;
    pending_writes.clear();
    write_history.clear();
    post_calls = 0;
    poll_calls = 0;
    fail_post_at = 0;
    poll_error = 0;
    fail_next_completion = false;
}

urma_status_t close_status(gamaq::UrmaSessionResource resource) {
    return close_fault == resource ? static_cast<urma_status_t>(77) : URMA_SUCCESS;
}

gamaq::UrmaSessionConfig config() {
    gamaq::UrmaSessionConfig value;
    value.device = handle<urma_device_t>(0x100);
    value.eid_index = 3;
    value.payload_bytes = 512;
    value.window = 4;
    value.remote_segment.ubva.va = 0x800000;
    value.remote_segment.len = 16384;
    value.source_fill = 0x5A;
    return value;
}
}  // namespace

extern "C" urma_context_t *urma_create_context(urma_device_t *, uint32_t) {
    events.emplace_back("create_context");
    if (open_fault == OpenFault::context) return nullptr;
    ++live_resources;
    return handle<urma_context_t>(0x101);
}

extern "C" urma_status_t urma_query_device(urma_device_t *, urma_device_attr_t *attr) {
    events.emplace_back("query_device");
    attr->dev_cap.max_jfc_depth = max_jfc_depth;
    attr->dev_cap.max_jfs_depth = max_jfs_depth;
    attr->dev_cap.max_jfr_depth = max_jfr_depth;
    return open_fault == OpenFault::query ? static_cast<urma_status_t>(41) : URMA_SUCCESS;
}

extern "C" urma_jfc_t *urma_create_jfc(urma_context_t *, urma_jfc_cfg_t *cfg) {
    events.emplace_back("create_jfc");
    captured_jfc_depth = cfg->depth;
    if (open_fault == OpenFault::jfc) return nullptr;
    ++live_resources;
    return handle<urma_jfc_t>(0x102);
}

extern "C" urma_jfr_t *urma_create_jfr(urma_context_t *, urma_jfr_cfg_t *) {
    events.emplace_back("create_jfr");
    if (open_fault == OpenFault::jfr) return nullptr;
    ++live_resources;
    return handle<urma_jfr_t>(0x103);
}

extern "C" urma_jetty_t *urma_create_jetty(urma_context_t *, urma_jetty_cfg_t *cfg) {
    events.emplace_back("create_jetty");
    captured_jfs_depth = cfg->jfs_cfg.depth;
    if (open_fault == OpenFault::jetty) return nullptr;
    ++live_resources;
    return handle<urma_jetty_t>(0x104);
}

extern "C" urma_target_seg_t *urma_register_seg(urma_context_t *, urma_seg_cfg_t *cfg) {
    events.emplace_back("register_segment");
    captured_buffer_address = cfg->va;
    captured_buffer_bytes = cfg->len;
    if (open_fault == OpenFault::local_segment) return nullptr;
    ++live_resources;
    return local_segment_handle;
}

extern "C" urma_target_seg_t *urma_import_seg(
        urma_context_t *, urma_seg_t *, urma_token_t *, uint64_t,
        urma_import_seg_flag_t) {
    events.emplace_back("import_segment");
    if (open_fault == OpenFault::remote_segment) return nullptr;
    ++live_resources;
    return remote_segment_handle;
}

extern "C" urma_target_jetty_t *urma_import_jetty(
        urma_context_t *, urma_rjetty_t *, urma_token_t *) {
    events.emplace_back("import_jetty");
    if (open_fault == OpenFault::remote_jetty) return nullptr;
    ++live_resources;
    return handle<urma_target_jetty_t>(0x108);
}

extern "C" urma_status_t urma_unimport_jetty(urma_target_jetty_t *) {
    events.emplace_back("unimport_jetty");
    const auto status = close_status(gamaq::UrmaSessionResource::remote_jetty);
    if (status == URMA_SUCCESS) --live_resources;
    return status;
}

extern "C" urma_status_t urma_unimport_seg(urma_target_seg_t *) {
    events.emplace_back("unimport_segment");
    const auto status = close_status(gamaq::UrmaSessionResource::remote_segment);
    if (status == URMA_SUCCESS) --live_resources;
    return status;
}

extern "C" urma_status_t urma_delete_jetty(urma_jetty_t *) {
    events.emplace_back("delete_jetty");
    const auto status = close_status(gamaq::UrmaSessionResource::local_jetty);
    if (status == URMA_SUCCESS) --live_resources;
    return status;
}

extern "C" urma_status_t urma_unregister_seg(urma_target_seg_t *) {
    events.emplace_back("unregister_segment");
    const auto status = close_status(gamaq::UrmaSessionResource::local_segment);
    if (status == URMA_SUCCESS) --live_resources;
    return status;
}

extern "C" urma_status_t urma_delete_jfr(urma_jfr_t *) {
    events.emplace_back("delete_jfr");
    const auto status = close_status(gamaq::UrmaSessionResource::receive_queue);
    if (status == URMA_SUCCESS) --live_resources;
    return status;
}

extern "C" urma_status_t urma_delete_jfc(urma_jfc_t *) {
    events.emplace_back("delete_jfc");
    const auto status = close_status(gamaq::UrmaSessionResource::completion_queue);
    if (status == URMA_SUCCESS) --live_resources;
    return status;
}

extern "C" urma_status_t urma_delete_context(urma_context_t *) {
    events.emplace_back("delete_context");
    const auto status = close_status(gamaq::UrmaSessionResource::context);
    if (status == URMA_SUCCESS) --live_resources;
    return status;
}

extern "C" urma_status_t urma_post_jetty_send_wr(
        urma_jetty_t *, urma_jfs_wr_t *wr, urma_jfs_wr_t **bad_wr) {
    ++post_calls;
    *bad_wr = nullptr;
    if (fail_post_at && post_calls == fail_post_at)
        return static_cast<urma_status_t>(19);
    const auto &source = wr->rw.src.sge[0];
    const auto &destination = wr->rw.dst.sge[0];
    const CapturedWrite captured {wr->user_ctx, source.addr, destination.addr,
        source.len, source.tseg, destination.tseg};
    pending_writes.push_back(captured);
    write_history.push_back(captured);
    return URMA_SUCCESS;
}

extern "C" int urma_poll_jfc(urma_jfc_t *, int max_count, urma_cr_t *out) {
    ++poll_calls;
    if (poll_error) return poll_error;
    if (pending_writes.empty()) return 0;
    const int count = std::min<int>(max_count,
        static_cast<int>(pending_writes.size()));
    for (int i = 0; i < count; ++i) {
        const auto captured = pending_writes.back();
        pending_writes.pop_back();
        out[i].user_ctx = captured.request_id;
        out[i].completion_len = captured.bytes;
        if (fail_next_completion) {
            out[i].status = URMA_CR_WR_FLUSH_ERR;
            fail_next_completion = false;
        } else {
            out[i].status = URMA_CR_SUCCESS;
        }
    }
    return count;
}

int main() {
    using gamaq::CompletionError;
    using gamaq::KvBatchValidationError;
    using gamaq::UrmaSession;
    using gamaq::UrmaSessionCloseState;
    using gamaq::UrmaSessionOpenError;
    using gamaq::UrmaSessionResource;
    using gamaq::UrmaKvSessionError;
    using gamaq::WindowError;

    reset_provider();
    {
        UrmaSession session;
        const auto opened = session.open(config());
        require(opened.ok() && session.is_open() && !session.is_quarantined(),
                "healthy session opens");
        require(captured_jfc_depth == 5 && captured_jfs_depth == 4,
                "session sizes JFC and JFS from the bounded window");
        require(session.buffer_bytes() == 2048 && captured_buffer_bytes == 2048 &&
                captured_buffer_address == reinterpret_cast<uint64_t>(session.buffer()),
                "session allocates and registers one reusable window buffer");
        const auto *bytes = static_cast<const uint8_t *>(session.buffer());
        require(bytes[0] == 0x5A && bytes[2047] == 0x5A,
                "session initializes the complete registered source buffer");
        require(session.completions().capacity() == 4 && session.request_id() == 0,
                "session owns completion and request-id state");
        const auto source = session.source_binding();
        const auto destination = session.destination_binding();
        require(source.valid() && source.region.base_address == captured_buffer_address &&
                source.region.bytes == 2048 &&
                destination.valid() && destination.region.base_address == 0x800000 &&
                destination.region.bytes == 16384,
                "session exports reusable local and remote region bindings");

        const auto closed = session.close();
        require(closed.state == UrmaSessionCloseState::closed && live_resources == 0,
                "healthy session releases every provider resource");
        const std::vector<std::string> teardown {
            "unimport_jetty", "unimport_segment", "delete_jetty",
            "unregister_segment", "delete_jfr", "delete_jfc", "delete_context"};
        require(events.size() >= teardown.size() &&
                std::equal(teardown.begin(), teardown.end(),
                           events.end() - static_cast<ptrdiff_t>(teardown.size())),
                "session teardown follows dependency order");
        require(session.close().state == UrmaSessionCloseState::already_closed,
                "session close is idempotent after success");
    }

    const std::vector<std::pair<OpenFault, UrmaSessionOpenError>> open_failures {
        {OpenFault::context, UrmaSessionOpenError::create_context},
        {OpenFault::query, UrmaSessionOpenError::query_device},
        {OpenFault::jfc, UrmaSessionOpenError::create_jfc},
        {OpenFault::jfr, UrmaSessionOpenError::create_jfr},
        {OpenFault::jetty, UrmaSessionOpenError::create_jetty},
        {OpenFault::local_segment, UrmaSessionOpenError::register_segment},
        {OpenFault::remote_segment, UrmaSessionOpenError::import_segment},
        {OpenFault::remote_jetty, UrmaSessionOpenError::import_jetty},
    };
    for (const auto &[fault, expected] : open_failures) {
        reset_provider();
        UrmaSession session;
        open_fault = fault;
        const auto opened = session.open(config());
        require(opened.error == expected, "open reports the exact failed stage");
        require(opened.rollback.closed() && live_resources == 0,
                "partial open rolls back every acquired provider resource");
        require(session.close().state == UrmaSessionCloseState::already_closed,
                "rolled-back session owns no resources");
    }

    reset_provider();
    {
        UrmaSession session;
        max_jfc_depth = 4;
        const auto opened = session.open(config());
        require(opened.error == UrmaSessionOpenError::insufficient_queue_depth &&
                opened.max_jfc_depth == 4 && opened.max_jfs_depth == 64,
                "queue capability failure retains device evidence");
        require(opened.rollback.closed() && live_resources == 0,
                "capability rejection releases its context");
    }

    reset_provider();
    {
        UrmaSession session;
        require(session.open(config()).ok(), "open session for in-flight close test");
        require(session.completions().reserve(1, 512).error == CompletionError::none,
                "reserve a provider-owned request");
        const size_t event_count = events.size();
        auto closed = session.close();
        require(closed.state == UrmaSessionCloseState::in_flight &&
                closed.in_flight == 1 && events.size() == event_count,
                "close never destroys resources while a known WR is in flight");
        require(session.completions().complete(1, true, 512).request_retired,
                "known completion drains the session");
        closed = session.close();
        require(closed.state == UrmaSessionCloseState::closed && live_resources == 0,
                "close may be retried after a proven drain");
    }

    reset_provider();
    {
        UrmaSession session;
        require(session.open(config()).ok(), "open session for quarantine test");
        require(session.completions().reserve(2, 512).error == CompletionError::none,
                "reserve unknown in-flight request");
        session.quarantine();
        const size_t event_count = events.size();
        const auto closed = session.close();
        require(closed.state == UrmaSessionCloseState::quarantined &&
                events.size() == event_count && live_resources == 7,
                "unknown ownership keeps all provider resources quarantined");
    }

    reset_provider();
    {
        UrmaSession session;
        require(session.open(config()).ok(), "open session for teardown failure test");
        close_fault = UrmaSessionResource::remote_segment;
        auto closed = session.close();
        require(closed.state == UrmaSessionCloseState::provider_error &&
                closed.resource == UrmaSessionResource::remote_segment &&
                closed.provider_status == 77,
                "teardown reports the first provider failure");
        const size_t event_count = events.size();
        closed = session.close();
        require(closed.state == UrmaSessionCloseState::quarantined &&
                events.size() == event_count,
                "teardown failure stops cascading destroys and disables retry");
    }

    reset_provider();
    {
        UrmaSession session;
        open_fault = OpenFault::remote_jetty;
        close_fault = UrmaSessionResource::remote_segment;
        const auto opened = session.open(config());
        require(opened.error == UrmaSessionOpenError::import_jetty &&
                opened.rollback.state == UrmaSessionCloseState::provider_error &&
                opened.rollback.resource == UrmaSessionResource::remote_segment,
                "open result preserves primary and rollback failures");
        require(session.is_quarantined(),
                "failed rollback leaves remaining resources fail-closed");
    }

    reset_provider();
    {
        UrmaSession session;
        require(session.open(config()).ok(), "open session for KV put entry point");
        const std::vector<gamaq::KvPutBlock> blocks {
            {100, 16, 128, 64}, {101, 256, 2048, 128}, {102, 1024, 4096, 32}};
        const auto first = gamaq::run_urma_kv_put_batch(
            session, blocks, std::chrono::milliseconds(10));
        require(first.started() && first.batch.valid() &&
                first.batch.window.error == WindowError::none &&
                first.batch.window.completed == blocks.size(),
                "session-facing KV put completes a discrete batch");
        require(session.ready_for_submission() && !session.is_retired(),
                "clean batch leaves the persistent session reusable");
        require(write_history.size() == blocks.size() &&
                write_history[0].source_address == captured_buffer_address + 16 &&
                write_history[0].destination_address == 0x800000 + 128 &&
                write_history[0].source_segment == local_segment_handle &&
                write_history[0].destination_segment == remote_segment_handle,
                "session entry point binds KV offsets to owned URMA regions");
        const uint64_t first_last_id = first.batch.items.back().request_id;
        const auto second = gamaq::run_urma_kv_put_batch(session,
            {{103, 64, 8192, 16}}, std::chrono::milliseconds(10));
        require(second.batch.window.error == WindowError::none &&
                second.batch.items[0].request_id > first_last_id,
                "sequential batches reuse resources and preserve request identity");
        require(session.close().state == UrmaSessionCloseState::closed &&
                live_resources == 0, "reused KV session closes after a proven drain");
    }

    reset_provider();
    {
        UrmaSession session;
        require(session.open(config()).ok(), "open session for preflight rejection");
        const auto invalid = gamaq::run_urma_kv_put_batch(session,
            {{1, 0, 0, 0}}, std::chrono::milliseconds(10));
        require(invalid.started() && invalid.batch.validation_error ==
                KvBatchValidationError::zero_length && post_calls == 0 &&
                session.ready_for_submission(),
                "invalid KV batch never reaches provider or retires session");
        const auto timeout = gamaq::run_urma_kv_put_batch(session,
            {{1, 0, 0, 8}}, std::chrono::milliseconds(0));
        require(timeout.error == UrmaKvSessionError::invalid_timeout &&
                post_calls == 0 && session.ready_for_submission(),
                "invalid timeout is rejected before provider ownership");
        std::atomic<bool> stop {true};
        const auto canceled = gamaq::run_urma_kv_put_batch(session,
            {{1, 0, 0, 8}}, std::chrono::milliseconds(10), stop);
        require(canceled.batch.window.error == WindowError::canceled &&
                canceled.batch.window.safe_to_destroy() && post_calls == 0 &&
                session.ready_for_submission(),
                "drained external cancellation does not poison healthy session");
        require(session.close().closed(), "preflight-only session closes normally");
    }

    reset_provider();
    {
        UrmaSession session;
        require(session.open(config()).ok(), "open session for busy rejection");
        require(session.completions().reserve(55, 8).error == CompletionError::none,
                "seed an existing provider-owned request");
        const auto busy = gamaq::run_urma_kv_put_batch(session,
            {{1, 0, 0, 8}}, std::chrono::milliseconds(10));
        require(busy.error == UrmaKvSessionError::requests_in_flight &&
                post_calls == 0,
                "KV entry point refuses to mix batches in one tracker");
        require(session.completions().complete(55, true, 8).request_retired,
                "retire seeded request before close");
        require(session.close().closed(), "drained busy session closes normally");
    }

    reset_provider();
    {
        UrmaSession session;
        require(session.open(config()).ok(), "open session for post failure");
        fail_post_at = 1;
        const auto failed = gamaq::run_urma_kv_put_batch(session,
            {{1, 0, 0, 8}}, std::chrono::milliseconds(10));
        require(failed.batch.window.error == WindowError::post_error &&
                failed.batch.window.safe_to_destroy() && session.is_retired() &&
                !session.is_quarantined(),
                "drained post failure retires but does not quarantine session");
        const auto retry = gamaq::run_urma_kv_put_batch(session,
            {{2, 8, 8, 8}}, std::chrono::milliseconds(10));
        require(retry.error == UrmaKvSessionError::session_retired && post_calls == 1,
                "retired session rejects unsupported implicit recovery");
        require(session.close().closed() && live_resources == 0,
                "retired and drained session remains safely closeable");
    }

    reset_provider();
    {
        UrmaSession session;
        require(session.open(config()).ok(), "open session for completion failure");
        fail_next_completion = true;
        const auto failed = gamaq::run_urma_kv_put_batch(session,
            {{1, 0, 0, 8}}, std::chrono::milliseconds(10));
        require(failed.batch.window.error == WindowError::invalid_completion &&
                failed.batch.window.safe_to_destroy() && session.is_retired(),
                "named transport error drains ownership and retires session");
        require(failed.batch.items[0].source_reusable &&
                !failed.batch.items[0].transfer_complete,
                "named failed completion releases source without claiming transfer");
        require(session.close().closed(), "drained completion failure can close");
    }

    reset_provider();
    {
        UrmaSession session;
        require(session.open(config()).ok(), "open session for unknown ownership");
        poll_error = -29;
        const auto failed = gamaq::run_urma_kv_put_batch(session,
            {{1, 0, 0, 8}}, std::chrono::milliseconds(10));
        require(failed.batch.window.error == WindowError::poll_error &&
                !failed.batch.window.safe_to_destroy() &&
                session.is_quarantined(),
                "poll failure quarantines session with unknown provider ownership");
        const auto retry = gamaq::run_urma_kv_put_batch(session,
            {{2, 8, 8, 8}}, std::chrono::milliseconds(10));
        require(retry.error == UrmaKvSessionError::session_quarantined &&
                post_calls == 1,
                "quarantined session rejects further KV submission");
        require(session.close().state == UrmaSessionCloseState::quarantined &&
                live_resources == 7,
                "quarantined session skips provider teardown");
    }

    std::cout << "urma_session: " << checks << " checks passed\n";
}
