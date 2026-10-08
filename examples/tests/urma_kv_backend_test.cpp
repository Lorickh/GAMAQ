#include "urma_kv_backend.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace {
unsigned checks = 0;
void require(bool ok, const char *message) {
    ++checks;
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

struct CapturedWrite {
    uint64_t source_address = 0;
    uint64_t destination_address = 0;
    uint32_t source_bytes = 0;
    uint32_t destination_bytes = 0;
    urma_target_seg_t *source_segment = nullptr;
    urma_target_seg_t *destination_segment = nullptr;
    urma_target_jetty_t *remote_jetty = nullptr;
    uint64_t request_id = 0;
    urma_opcode_t opcode {};
    uint32_t completion_enabled = 0;
};

CapturedWrite captured;
int post_calls = 0;
int poll_calls = 0;
urma_status_t post_status = URMA_SUCCESS;
int poll_result = 0;
urma_cr_t poll_records[gamaq::kMaxCompletionBatch] {};
}  // namespace

extern "C" urma_status_t urma_post_jetty_send_wr(
        urma_jetty_t *, urma_jfs_wr_t *wr, urma_jfs_wr_t **bad_wr) {
    ++post_calls;
    *bad_wr = nullptr;
    captured.source_address = wr->rw.src.sge[0].addr;
    captured.destination_address = wr->rw.dst.sge[0].addr;
    captured.source_bytes = wr->rw.src.sge[0].len;
    captured.destination_bytes = wr->rw.dst.sge[0].len;
    captured.source_segment = wr->rw.src.sge[0].tseg;
    captured.destination_segment = wr->rw.dst.sge[0].tseg;
    captured.remote_jetty = wr->tjetty;
    captured.request_id = wr->user_ctx;
    captured.opcode = wr->opcode;
    captured.completion_enabled = wr->flag.bs.complete_enable;
    return post_status;
}

extern "C" int urma_poll_jfc(urma_jfc_t *, int cr_count, urma_cr_t *out) {
    ++poll_calls;
    const int copy_count = poll_result > 0 && poll_result <= cr_count
        ? poll_result : 0;
    for (int i = 0; i < copy_count; ++i) out[i] = poll_records[i];
    return poll_result;
}

int main() {
    using gamaq::TransferCompletion;
    using gamaq::TransferRequest;
    using gamaq::UrmaKvBackendError;
    using gamaq::UrmaKvPutBackend;
    using gamaq::UrmaRegionBinding;

    auto *jetty = reinterpret_cast<urma_jetty_t *>(uintptr_t{0x101});
    auto *jfc = reinterpret_cast<urma_jfc_t *>(uintptr_t{0x102});
    auto *remote_jetty = reinterpret_cast<urma_target_jetty_t *>(uintptr_t{0x103});
    auto *source_segment = reinterpret_cast<urma_target_seg_t *>(uintptr_t{0x104});
    auto *destination_segment = reinterpret_cast<urma_target_seg_t *>(uintptr_t{0x105});
    const UrmaRegionBinding source {{0x1000, 256}, source_segment};
    const UrmaRegionBinding destination {{0x8000, 512}, destination_segment};
    UrmaKvPutBackend backend(jetty, jfc, remote_jetty, source, destination);

    const TransferRequest request {77, 3, 9001, 16, 40, 1, 64};
    require(backend.post(request) == URMA_SUCCESS && post_calls == 1,
            "valid KV request reaches URMA provider once");
    require(captured.source_address == 0x1010 &&
            captured.destination_address == 0x8028,
            "KV offsets resolve against separate registered bases");
    require(captured.source_bytes == 64 && captured.destination_bytes == 64,
            "URMA SGEs preserve the requested byte count");
    require(captured.source_segment == source_segment &&
            captured.destination_segment == destination_segment,
            "URMA SGEs preserve local and remote segment handles");
    require(captured.remote_jetty == remote_jetty && captured.request_id == 77,
            "URMA WR preserves target Jetty and scheduler request identity");
    require(captured.opcode == URMA_OPC_WRITE && captured.completion_enabled == 1,
            "KV backend emits a signaled URMA WRITE");

    const int calls_before_invalid = post_calls;
    require(backend.post({78, 0, 0, 240, 0, 0, 32}) ==
            static_cast<int>(UrmaKvBackendError::invalid_source_range),
            "transport boundary rejects source overrun");
    require(backend.post({79, 0, 0, 0, 500, 0, 16}) ==
            static_cast<int>(UrmaKvBackendError::invalid_destination_range),
            "transport boundary rejects destination overrun");
    require(post_calls == calls_before_invalid,
            "invalid ranges never reach the URMA provider");

    UrmaKvPutBackend incomplete(nullptr, jfc, remote_jetty, source, destination);
    require(incomplete.post(request) ==
            static_cast<int>(UrmaKvBackendError::invalid_binding),
            "incomplete session binding fails before post");

    post_status = static_cast<urma_status_t>(17);
    require(backend.post(request) == 17,
            "provider post status is preserved for scheduler error reporting");
    post_status = URMA_SUCCESS;

    poll_records[0].user_ctx = 77;
    poll_records[0].completion_len = 64;
    poll_records[0].status = URMA_CR_SUCCESS;
    poll_records[1].user_ctx = 78;
    poll_records[1].completion_len = 32;
    poll_records[1].status = URMA_CR_WR_FLUSH_ERR;
    poll_result = 2;
    TransferCompletion completions[2] {};
    require(backend.poll(completions, 2) == 2 && poll_calls == 1,
            "URMA completion batch reaches provider once");
    require(completions[0].request_id == 77 && completions[0].bytes == 64 &&
            completions[0].success,
            "successful URMA CR is normalized without losing identity");
    require(completions[1].request_id == 78 && completions[1].bytes == 32 &&
            !completions[1].success &&
            completions[1].transport_status == URMA_CR_WR_FLUSH_ERR,
            "failed URMA CR retains provider status and length");

    const int polls_before_invalid = poll_calls;
    require(backend.poll(completions, gamaq::kMaxCompletionBatch + 1) ==
            static_cast<int>(UrmaKvBackendError::invalid_poll_count) &&
            poll_calls == polls_before_invalid,
            "invalid completion batch is rejected before provider poll");

    std::cout << "urma_kv_backend: " << checks << " checks passed\n";
}
