#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "window_scheduler.h"

namespace gamaq {

// Offsets are relative to caller-owned, pre-registered CPU and NPU regions.
// This adapter does not register memory or translate CPU/NPU address spaces.
struct KvPutBlock {
    uint64_t block_id = 0;
    uint64_t source_offset = 0;
    uint64_t destination_offset = 0;
    uint32_t bytes = 0;
};

enum class KvBatchValidationError {
    none,
    zero_length,
    source_address_overflow,
    destination_address_overflow,
    source_out_of_bounds,
    destination_out_of_bounds,
    duplicate_block_id,
    overlapping_destination,
};

inline const char *kv_validation_error_message(KvBatchValidationError error) {
    switch (error) {
    case KvBatchValidationError::none: return "ok";
    case KvBatchValidationError::zero_length: return "KV block length must be nonzero";
    case KvBatchValidationError::source_address_overflow: return "source range overflows uint64";
    case KvBatchValidationError::destination_address_overflow: return "destination range overflows uint64";
    case KvBatchValidationError::source_out_of_bounds: return "source range exceeds the registered region";
    case KvBatchValidationError::destination_out_of_bounds: return "destination range exceeds the registered region";
    case KvBatchValidationError::duplicate_block_id: return "KV block IDs must be unique within a batch";
    case KvBatchValidationError::overlapping_destination: return "KV destination ranges must not overlap";
    }
    return "unknown KV batch validation error";
}

enum class KvItemState {
    not_submitted,
    submitted,
    transfer_complete,
    post_failed,
    completion_failed,
};

struct KvPutItemResult {
    uint64_t block_id = 0;
    uint64_t request_id = 0;
    KvItemState state = KvItemState::not_submitted;
    CompletionError completion_error = CompletionError::none;
    int backend_status = 0;
    uint32_t expected_bytes = 0;
    uint32_t actual_bytes = 0;
    // A never-submitted or retired request no longer grants the backend
    // ownership of its CPU source range.
    bool source_reusable = true;
    bool transfer_complete = false;
    // Deliberately never inferred from a transport completion. A separate
    // peer-visible protocol event must establish this state.
    bool peer_consumable_proven = false;
};

struct KvPutBatchResult {
    KvBatchValidationError validation_error = KvBatchValidationError::none;
    uint64_t invalid_item = std::numeric_limits<uint64_t>::max();
    WindowRunResult window;
    std::vector<KvPutItemResult> items;

    bool valid() const { return validation_error == KvBatchValidationError::none; }
};

inline KvBatchValidationError validate_kv_put_batch(
        const std::vector<KvPutBlock> &blocks, uint64_t source_region_bytes,
        uint64_t destination_region_bytes, uint64_t *invalid_item) {
    constexpr uint64_t max = std::numeric_limits<uint64_t>::max();
    *invalid_item = max;
    std::vector<std::pair<uint64_t, uint64_t>> ids;
    struct Range { uint64_t begin; uint64_t end; uint64_t index; };
    std::vector<Range> destinations;
    ids.reserve(blocks.size());
    destinations.reserve(blocks.size());

    for (uint64_t index = 0; index < blocks.size(); ++index) {
        const auto &block = blocks[static_cast<size_t>(index)];
        if (!block.bytes) {
            *invalid_item = index;
            return KvBatchValidationError::zero_length;
        }
        if (block.source_offset > max - block.bytes) {
            *invalid_item = index;
            return KvBatchValidationError::source_address_overflow;
        }
        if (block.destination_offset > max - block.bytes) {
            *invalid_item = index;
            return KvBatchValidationError::destination_address_overflow;
        }
        const uint64_t source_end = block.source_offset + block.bytes;
        const uint64_t destination_end = block.destination_offset + block.bytes;
        if (source_end > source_region_bytes) {
            *invalid_item = index;
            return KvBatchValidationError::source_out_of_bounds;
        }
        if (destination_end > destination_region_bytes) {
            *invalid_item = index;
            return KvBatchValidationError::destination_out_of_bounds;
        }
        ids.emplace_back(block.block_id, index);
        destinations.push_back({block.destination_offset, destination_end, index});
    }

    std::sort(ids.begin(), ids.end());
    for (size_t i = 1; i < ids.size(); ++i) {
        if (ids[i - 1].first == ids[i].first) {
            *invalid_item = ids[i].second;
            return KvBatchValidationError::duplicate_block_id;
        }
    }
    std::sort(destinations.begin(), destinations.end(),
        [](const Range &a, const Range &b) { return a.begin < b.begin; });
    for (size_t i = 1; i < destinations.size(); ++i) {
        if (destinations[i].begin < destinations[i - 1].end) {
            *invalid_item = destinations[i].index;
            return KvBatchValidationError::overlapping_destination;
        }
    }
    return KvBatchValidationError::none;
}

class KvPutObserver {
public:
    explicit KvPutObserver(std::vector<KvPutItemResult> &items) : items_(items) {}

    void on_posted(const TransferRequest &request) {
        auto &item = items_[static_cast<size_t>(request.operation_index)];
        item.request_id = request.request_id;
        item.state = KvItemState::submitted;
        item.source_reusable = false;
    }

    void on_post_failed(const TransferRequest &request, int status) {
        auto &item = items_[static_cast<size_t>(request.operation_index)];
        item.request_id = request.request_id;
        item.state = KvItemState::post_failed;
        item.backend_status = status;
        item.source_reusable = true;
    }

    void on_completion(uint64_t operation_index, uint64_t request_id,
                       CompletionError error, int status,
                       uint32_t expected_bytes, uint32_t actual_bytes) {
        auto &item = items_[static_cast<size_t>(operation_index)];
        item.request_id = request_id;
        item.completion_error = error;
        item.backend_status = status;
        item.expected_bytes = expected_bytes;
        item.actual_bytes = actual_bytes;
        item.source_reusable = true;
        item.transfer_complete = error == CompletionError::none;
        item.state = item.transfer_complete ? KvItemState::transfer_complete
                                            : KvItemState::completion_failed;
    }

    void on_unmatched_completion(const TransferCompletion &) {}

private:
    std::vector<KvPutItemResult> &items_;
};

template <typename Backend, typename Clock = std::chrono::steady_clock>
KvPutBatchResult run_kv_put_batch(CompletionTracker &tracker,
                                  uint64_t &next_request_id,
                                  const std::vector<KvPutBlock> &blocks,
                                  uint64_t source_region_bytes,
                                  uint64_t destination_region_bytes,
                                  std::chrono::milliseconds timeout,
                                  std::atomic<bool> &stop,
                                  Backend &backend) {
    KvPutBatchResult result;
    result.items.reserve(blocks.size());
    for (const auto &block : blocks)
        result.items.push_back(KvPutItemResult{block.block_id});

    result.validation_error = validate_kv_put_batch(blocks, source_region_bytes,
        destination_region_bytes, &result.invalid_item);
    if (result.validation_error != KvBatchValidationError::none) return result;

    std::vector<TransferWork> work;
    work.reserve(blocks.size());
    for (const auto &block : blocks)
        work.push_back({block.block_id, block.source_offset,
                        block.destination_offset, block.bytes});
    KvPutObserver observer(result.items);
    result.window = run_bounded_batch<Backend, KvPutObserver, Clock>(tracker,
        next_request_id, work.data(), work.size(), timeout, stop, backend, observer);
    return result;
}

}  // namespace gamaq
