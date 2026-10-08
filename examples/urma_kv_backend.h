#pragma once

#include <array>
#include <cstddef>

#include "urma_api.h"
#include "registered_region.h"
#include "window_scheduler.h"

namespace gamaq {

// Non-owning binding between the transport-independent numeric region and a
// provider handle. The session must keep the handle alive until every posted
// request has retired or the context has been quarantined.
struct UrmaRegionBinding {
    RegisteredRegionView region;
    urma_target_seg_t *target_segment = nullptr;

    bool valid() const { return target_segment != nullptr && region.bytes != 0; }
};

enum class UrmaKvBackendError : int {
    invalid_binding = -20001,
    invalid_source_range = -20002,
    invalid_destination_range = -20003,
    invalid_poll_count = -20004,
};

inline const char *urma_kv_backend_error_message(UrmaKvBackendError error) {
    switch (error) {
    case UrmaKvBackendError::invalid_binding: return "URMA KV backend binding is incomplete";
    case UrmaKvBackendError::invalid_source_range: return "URMA KV source range is invalid";
    case UrmaKvBackendError::invalid_destination_range: return "URMA KV destination range is invalid";
    case UrmaKvBackendError::invalid_poll_count: return "URMA KV completion batch is invalid";
    }
    return "unknown URMA KV backend error";
}

// URMA WRITE mapping for run_kv_put_batch(). It deliberately owns
// no context, segment, Jetty, or buffer; those lifetimes belong to the session.
// post() repeats numeric range validation at the transport boundary before
// constructing SGEs, even if the application adapter already validated them.
class UrmaKvPutBackend {
public:
    UrmaKvPutBackend(urma_jetty_t *jetty, urma_jfc_t *jfc,
                     urma_target_jetty_t *remote_jetty,
                     UrmaRegionBinding source,
                     UrmaRegionBinding destination)
        : jetty_(jetty), jfc_(jfc), remote_jetty_(remote_jetty),
          source_(source), destination_(destination) {}

    int post(TransferRequest request) {
        if (!jetty_ || !jfc_ || !remote_jetty_ ||
            !source_.valid() || !destination_.valid())
            return status(UrmaKvBackendError::invalid_binding);
        const auto plan = plan_region_write(source_.region, destination_.region,
            request.source_offset, request.destination_offset, request.bytes);
        if (plan.source_error != RegionRangeError::none)
            return status(UrmaKvBackendError::invalid_source_range);
        if (plan.destination_error != RegionRangeError::none)
            return status(UrmaKvBackendError::invalid_destination_range);

        urma_sge_t local {};
        local.addr = plan.plan.source.address;
        local.len = plan.plan.source.bytes;
        local.tseg = source_.target_segment;
        urma_sge_t target {};
        target.addr = plan.plan.destination.address;
        target.len = plan.plan.destination.bytes;
        target.tseg = destination_.target_segment;
        urma_sg_t src {&local, 1};
        urma_sg_t dst {&target, 1};
        urma_rw_wr_t rw {};
        rw.src = src;
        rw.dst = dst;
        urma_jfs_wr_t wr {};
        wr.opcode = URMA_OPC_WRITE;
        wr.flag.bs.complete_enable = 1;
        wr.tjetty = remote_jetty_;
        wr.user_ctx = request.request_id;
        wr.rw = rw;
        urma_jfs_wr_t *bad = nullptr;
        return urma_post_jetty_send_wr(jetty_, &wr, &bad);
    }

    int poll(TransferCompletion *out, int max_count) {
        if (!jfc_ || !out || max_count <= 0 || max_count > kMaxCompletionBatch)
            return status(UrmaKvBackendError::invalid_poll_count);
        std::array<urma_cr_t, kMaxCompletionBatch> completions {};
        const int count = urma_poll_jfc(jfc_, max_count, completions.data());
        if (count <= 0 || count > max_count) return count;
        for (int i = 0; i < count; ++i) {
            const auto &completion = completions[static_cast<std::size_t>(i)];
            out[i] = {completion.user_ctx, completion.completion_len,
                      static_cast<int>(completion.status),
                      completion.status == URMA_CR_SUCCESS};
        }
        return count;
    }

private:
    static int status(UrmaKvBackendError error) {
        return static_cast<int>(error);
    }

    urma_jetty_t *jetty_ = nullptr;
    urma_jfc_t *jfc_ = nullptr;
    urma_target_jetty_t *remote_jetty_ = nullptr;
    UrmaRegionBinding source_;
    UrmaRegionBinding destination_;
};

}  // namespace gamaq
