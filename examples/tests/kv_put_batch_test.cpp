#include "kv_put_batch.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
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

class ReferenceBackend {
public:
    ReferenceBackend(std::vector<uint8_t> &source, std::vector<uint8_t> &destination)
        : source_(source), destination_(destination) {}

    int post(gamaq::TransferRequest request) {
        ++post_calls;
        if (fail_post_at && post_calls == fail_post_at) return post_error;
        pending.push_back(request);
        history.push_back(request);
        max_pending = std::max(max_pending, pending.size());
        return 0;
    }

    int poll(gamaq::TransferCompletion *out, int max_count) {
        ++poll_calls;
        if (poll_error) return poll_error;
        if (pending.empty()) return 0;
        const int count = std::min<int>(max_count, static_cast<int>(pending.size()));
        for (int i = 0; i < count; ++i) {
            const auto request = pending.back();
            pending.pop_back();
            const bool success = request.request_id != fail_completion_request;
            if (success) {
                std::copy_n(source_.begin() + request.source_offset, request.bytes,
                    destination_.begin() + request.destination_offset);
            }
            const uint32_t completed_bytes = request.request_id == short_completion_request
                ? request.bytes - 1 : request.bytes;
            out[i] = {request.request_id, completed_bytes,
                      success ? 0 : completion_error, success};
        }
        return count;
    }

    std::vector<gamaq::TransferRequest> pending;
    std::vector<gamaq::TransferRequest> history;
    uint64_t post_calls = 0;
    uint64_t poll_calls = 0;
    uint64_t fail_post_at = 0;
    uint64_t fail_completion_request = 0;
    uint64_t short_completion_request = 0;
    size_t max_pending = 0;
    int post_error = -17;
    int poll_error = 0;
    int completion_error = 31;

private:
    std::vector<uint8_t> &source_;
    std::vector<uint8_t> &destination_;
};

std::vector<gamaq::KvPutBlock> valid_blocks() {
    return {{10, 3, 100, 7}, {11, 40, 0, 13}, {12, 80, 32, 64},
            {13, 150, 160, 20}};
}

gamaq::KvPutBatchResult run(const std::vector<gamaq::KvPutBlock> &blocks,
                            uint32_t window, ReferenceBackend &backend,
                            uint64_t source_bytes = 256,
                            uint64_t destination_bytes = 256) {
    gamaq::CompletionTracker tracker(window);
    uint64_t request_id = 0;
    std::atomic<bool> stop {false};
    return gamaq::run_kv_put_batch(tracker, request_id, blocks, source_bytes,
        destination_bytes, std::chrono::milliseconds(10), stop, backend);
}
}  // namespace

int main() {
    using gamaq::CompletionError;
    using gamaq::KvBatchValidationError;
    using gamaq::KvItemState;
    using gamaq::WindowError;

    std::vector<uint8_t> source(256);
    for (size_t i = 0; i < source.size(); ++i)
        source[i] = static_cast<uint8_t>((i * 37 + 11) & 0xff);

    {
        std::vector<uint8_t> destination(256, 0);
        ReferenceBackend backend(source, destination);
        const auto blocks = valid_blocks();
        const auto result = run(blocks, 2, backend);
        require(result.valid() && result.window.error == WindowError::none,
                "valid KV put batch succeeds");
        require(result.window.submitted == blocks.size() &&
                result.window.completed == blocks.size() &&
                result.window.safe_to_destroy(), "valid batch completes and drains");
        require(backend.max_pending == 2, "KV batch honors configured window");
        for (size_t i = 0; i < blocks.size(); ++i) {
            const auto &block = blocks[i];
            const auto &item = result.items[i];
            require(item.block_id == block.block_id && item.request_id != 0,
                    "per-item result preserves block and request identity");
            require(backend.history[i].application_tag == block.block_id &&
                    backend.history[i].source_offset == block.source_offset &&
                    backend.history[i].destination_offset == block.destination_offset,
                    "scheduler forwards application identity and discrete offsets");
            require(item.state == KvItemState::transfer_complete &&
                    item.source_reusable && item.transfer_complete,
                    "successful completion releases source and marks transfer complete");
            require(!item.peer_consumable_proven,
                    "transport completion does not imply peer consumable");
            require(std::equal(source.begin() + block.source_offset,
                    source.begin() + block.source_offset + block.bytes,
                    destination.begin() + block.destination_offset),
                    "reference backend copies each discrete block");
        }
    }

    auto expect_invalid = [&](std::vector<gamaq::KvPutBlock> blocks,
                              KvBatchValidationError expected,
                              uint64_t source_bytes = 256,
                              uint64_t destination_bytes = 256) {
        std::vector<uint8_t> destination(256, 0);
        ReferenceBackend backend(source, destination);
        const auto result = run(blocks, 2, backend, source_bytes, destination_bytes);
        require(result.validation_error == expected, "reject invalid KV batch");
        require(backend.post_calls == 0 && result.window.submitted == 0,
                "validation fails before transport submission");
    };

    expect_invalid({{1, 0, 0, 0}}, KvBatchValidationError::zero_length);
    expect_invalid({{1, std::numeric_limits<uint64_t>::max() - 1, 0, 4}},
                   KvBatchValidationError::source_address_overflow,
                   std::numeric_limits<uint64_t>::max(), 256);
    expect_invalid({{1, 0, std::numeric_limits<uint64_t>::max() - 1, 4}},
                   KvBatchValidationError::destination_address_overflow,
                   256, std::numeric_limits<uint64_t>::max());
    expect_invalid({{1, 250, 0, 7}}, KvBatchValidationError::source_out_of_bounds);
    expect_invalid({{1, 0, 250, 7}}, KvBatchValidationError::destination_out_of_bounds);
    expect_invalid({{1, 0, 0, 8}, {1, 16, 16, 8}},
                   KvBatchValidationError::duplicate_block_id);
    expect_invalid({{1, 0, 0, 16}, {2, 16, 8, 16}},
                   KvBatchValidationError::overlapping_destination);

    {
        std::vector<uint8_t> destination(256, 0);
        ReferenceBackend backend(source, destination);
        const auto result = run({{1, 8, 0, 8}, {2, 8, 32, 8}}, 2, backend);
        require(result.window.error == WindowError::none,
                "read-only source ranges may be shared by multiple KV blocks");
        require(std::equal(destination.begin(), destination.begin() + 8,
                           destination.begin() + 32),
                "shared source is copied to distinct destinations");
    }

    {
        std::vector<uint8_t> destination(256, 0);
        ReferenceBackend backend(source, destination);
        backend.fail_post_at = 3;
        const auto result = run(valid_blocks(), 4, backend);
        require(result.window.error == WindowError::post_error &&
                result.window.submitted == 2 && result.window.completed == 2,
                "partial post failure drains only accepted KV blocks");
        require(result.items[0].state == KvItemState::transfer_complete &&
                result.items[1].state == KvItemState::transfer_complete,
                "accepted KV blocks complete after later post failure");
        require(result.items[2].state == KvItemState::post_failed &&
                result.items[2].source_reusable &&
                result.items[2].backend_status == -17,
                "failed post reports reusable source and backend status");
        require(result.items[3].state == KvItemState::not_submitted &&
                result.items[3].source_reusable,
                "items after post failure remain unsubmitted");
    }

    {
        std::vector<uint8_t> destination(256, 0);
        ReferenceBackend backend(source, destination);
        backend.poll_error = -9;
        const auto result = run(valid_blocks(), 2, backend);
        require(result.window.error == WindowError::poll_error &&
                !result.window.safe_to_destroy(), "poll failure preserves unsafe ownership");
        require(result.items[0].state == KvItemState::submitted &&
                result.items[1].state == KvItemState::submitted &&
                !result.items[0].source_reusable && !result.items[1].source_reusable,
                "poll failure keeps posted sources owned by backend");
        require(result.items[2].state == KvItemState::not_submitted &&
                result.items[3].state == KvItemState::not_submitted,
                "poll failure leaves later blocks unsubmitted");
    }

    {
        std::vector<uint8_t> destination(256, 0);
        ReferenceBackend backend(source, destination);
        backend.fail_completion_request = 2;
        const auto result = run(valid_blocks(), 2, backend);
        require(result.window.error == WindowError::invalid_completion &&
                result.window.completion_error == CompletionError::transport_error,
                "per-item transport error fails the batch");
        require(result.items[1].state == KvItemState::completion_failed &&
                result.items[1].source_reusable && !result.items[1].transfer_complete &&
                result.items[1].backend_status == 31,
                "failed named completion releases only its source ownership");
        require(result.items[0].state == KvItemState::transfer_complete &&
                result.items[2].state == KvItemState::not_submitted,
                "known completion error drains window without submitting more blocks");
    }

    {
        std::vector<uint8_t> destination(256, 0);
        ReferenceBackend backend(source, destination);
        backend.short_completion_request = 1;
        const auto result = run(valid_blocks(), 2, backend);
        require(result.items[0].state == KvItemState::completion_failed &&
                result.items[0].completion_error == CompletionError::length_mismatch &&
                result.items[0].expected_bytes == 7 && result.items[0].actual_bytes == 6,
                "short KV completion retains per-item byte evidence");
    }

    {
        std::vector<uint8_t> destination(1, 0);
        ReferenceBackend backend(source, destination);
        const auto result = run({}, 1, backend, source.size(), destination.size());
        require(result.valid() && result.items.empty() &&
                result.window.error == WindowError::none && backend.post_calls == 0,
                "empty batch is a successful no-op");
    }

    std::cout << "kv_put_batch: " << checks << " checks passed\n";
}
