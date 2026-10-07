#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "urma_api.h"
#include "completion_tracker.h"
#include "urma_kv_backend.h"

namespace gamaq {

enum class UrmaSessionResource {
    none,
    remote_jetty,
    remote_segment,
    local_jetty,
    local_segment,
    receive_queue,
    completion_queue,
    context,
};

inline const char *urma_session_resource_message(UrmaSessionResource resource) {
    switch (resource) {
    case UrmaSessionResource::none: return "none";
    case UrmaSessionResource::remote_jetty: return "remote Jetty";
    case UrmaSessionResource::remote_segment: return "remote segment";
    case UrmaSessionResource::local_jetty: return "local Jetty";
    case UrmaSessionResource::local_segment: return "local segment";
    case UrmaSessionResource::receive_queue: return "JFR";
    case UrmaSessionResource::completion_queue: return "JFC";
    case UrmaSessionResource::context: return "context";
    }
    return "unknown URMA resource";
}

enum class UrmaSessionCloseState {
    closed,
    already_closed,
    in_flight,
    quarantined,
    provider_error,
};

struct UrmaSessionCloseResult {
    UrmaSessionCloseState state = UrmaSessionCloseState::already_closed;
    UrmaSessionResource resource = UrmaSessionResource::none;
    int provider_status = 0;
    size_t in_flight = 0;

    bool closed() const {
        return state == UrmaSessionCloseState::closed ||
               state == UrmaSessionCloseState::already_closed;
    }
};

enum class UrmaSessionOpenError {
    none,
    already_open,
    invalid_config,
    tracker_config,
    create_context,
    query_device,
    insufficient_queue_depth,
    create_jfc,
    create_jfr,
    create_jetty,
    allocate_buffer,
    register_segment,
    import_segment,
    import_jetty,
};

inline const char *urma_session_open_error_message(UrmaSessionOpenError error) {
    switch (error) {
    case UrmaSessionOpenError::none: return "ok";
    case UrmaSessionOpenError::already_open: return "session already owns resources";
    case UrmaSessionOpenError::invalid_config: return "session configuration is invalid";
    case UrmaSessionOpenError::tracker_config: return "completion tracker rejected the window";
    case UrmaSessionOpenError::create_context: return "failed to create URMA context";
    case UrmaSessionOpenError::query_device: return "failed to query URMA device";
    case UrmaSessionOpenError::insufficient_queue_depth: return "device queue depth is insufficient";
    case UrmaSessionOpenError::create_jfc: return "failed to create JFC";
    case UrmaSessionOpenError::create_jfr: return "failed to create JFR";
    case UrmaSessionOpenError::create_jetty: return "failed to create Jetty";
    case UrmaSessionOpenError::allocate_buffer: return "failed to allocate aligned source buffer";
    case UrmaSessionOpenError::register_segment: return "failed to register source segment";
    case UrmaSessionOpenError::import_segment: return "failed to import destination segment";
    case UrmaSessionOpenError::import_jetty: return "failed to import destination Jetty";
    }
    return "unknown URMA session error";
}

struct UrmaSessionOpenResult {
    UrmaSessionOpenError error = UrmaSessionOpenError::none;
    int provider_status = 0;
    uint32_t max_jfc_depth = 0;
    uint32_t max_jfs_depth = 0;
    uint32_t max_jfr_depth = 0;
    UrmaSessionCloseResult rollback;

    bool ok() const { return error == UrmaSessionOpenError::none; }
};

struct UrmaSessionConfig {
    urma_device_t *device = nullptr;
    uint32_t eid_index = 0;
    uint32_t payload_bytes = 0;
    uint32_t window = 0;
    urma_seg_t remote_segment {};
    urma_jetty_id_t remote_jetty_id {};
    uint64_t token = 0xEFCD;
    uint8_t source_fill = 0xA5;
};

// Owns one worker's URMA data-plane resources. It is intentionally neither
// copyable nor movable so resource addresses remain stable while WRs are in
// flight. Callers may retry close() after a known request set drains, but a
// session explicitly quarantined by an unknown completion/poll failure will
// never destroy provider resources in-process.
class UrmaSession {
public:
    UrmaSession() = default;
    UrmaSession(const UrmaSession &) = delete;
    UrmaSession &operator=(const UrmaSession &) = delete;
    UrmaSession(UrmaSession &&) = delete;
    UrmaSession &operator=(UrmaSession &&) = delete;
    ~UrmaSession() { (void)close(); }

    UrmaSessionOpenResult open(const UrmaSessionConfig &config) {
        if (owns_resources())
            return immediate_failure(UrmaSessionOpenError::already_open);
        if (!config.device || !config.payload_bytes || !config.window ||
            config.window == std::numeric_limits<uint32_t>::max() ||
            !config.remote_segment.len)
            return immediate_failure(UrmaSessionOpenError::invalid_config);
        if (config.payload_bytes > std::numeric_limits<size_t>::max() / config.window)
            return immediate_failure(UrmaSessionOpenError::invalid_config);
        if (!completions_.reset(config.window))
            return immediate_failure(UrmaSessionOpenError::tracker_config);

        safe_to_destroy_ = true;
        teardown_failed_ = false;
        capabilities_ = {};
        request_id_ = 0;
        payload_bytes_ = config.payload_bytes;
        window_ = config.window;
        buffer_bytes_ = static_cast<size_t>(config.payload_bytes) * config.window;
        remote_descriptor_ = config.remote_segment;

        context_ = urma_create_context(config.device, config.eid_index);
        if (!context_) return fail(UrmaSessionOpenError::create_context);

        urma_device_attr_t attr {};
        const urma_status_t query_status = urma_query_device(config.device, &attr);
        capabilities_.max_jfc_depth = attr.dev_cap.max_jfc_depth;
        capabilities_.max_jfs_depth = attr.dev_cap.max_jfs_depth;
        capabilities_.max_jfr_depth = attr.dev_cap.max_jfr_depth;
        if (query_status != URMA_SUCCESS)
            return fail(UrmaSessionOpenError::query_device,
                        static_cast<int>(query_status));
        if (attr.dev_cap.max_jfc_depth < config.window + 1 ||
            attr.dev_cap.max_jfs_depth < config.window ||
            !attr.dev_cap.max_jfr_depth)
            return fail(UrmaSessionOpenError::insufficient_queue_depth);

        urma_jfc_cfg_t jfc_config {};
        jfc_config.depth = config.window + 1;
        jfc_ = urma_create_jfc(context_, &jfc_config);
        if (!jfc_) return fail(UrmaSessionOpenError::create_jfc);

        urma_token_t token {};
        token.token = config.token;
        urma_jfr_cfg_t jfr_config {};
        jfr_config.depth = 1;
        jfr_config.flag.bs.tag_matching = URMA_NO_TAG_MATCHING;
        jfr_config.trans_mode = URMA_TM_RM;
        jfr_config.min_rnr_timer = URMA_TYPICAL_MIN_RNR_TIMER;
        jfr_config.jfc = jfc_;
        jfr_config.token_value = token;
        jfr_config.max_sge = 1;
        jfr_ = urma_create_jfr(context_, &jfr_config);
        if (!jfr_) return fail(UrmaSessionOpenError::create_jfr);

        urma_jfs_cfg_t jfs_config {};
        jfs_config.depth = config.window;
        jfs_config.trans_mode = URMA_TM_RM;
        jfs_config.priority = URMA_MAX_PRIORITY;
        jfs_config.max_sge = 1;
        jfs_config.rnr_retry = URMA_TYPICAL_RNR_RETRY;
        jfs_config.err_timeout = URMA_TYPICAL_ERR_TIMEOUT;
        jfs_config.jfc = jfc_;
        urma_jetty_cfg_t jetty_config {};
        jetty_config.flag.bs.share_jfr = 1;
        jetty_config.jfs_cfg = jfs_config;
        jetty_config.shared.jfr = jfr_;
        jetty_ = urma_create_jetty(context_, &jetty_config);
        if (!jetty_) return fail(UrmaSessionOpenError::create_jetty);

        if (posix_memalign(&buffer_, 4096, buffer_bytes_))
            return fail(UrmaSessionOpenError::allocate_buffer);
        std::memset(buffer_, config.source_fill, buffer_bytes_);

        urma_reg_seg_flag_t register_flag {};
        register_flag.bs.token_policy = URMA_TOKEN_NONE;
        register_flag.bs.cacheable = URMA_NON_CACHEABLE;
        register_flag.bs.access = URMA_ACCESS_READ | URMA_ACCESS_WRITE;
        urma_seg_cfg_t segment_config {};
        segment_config.va = reinterpret_cast<uint64_t>(buffer_);
        segment_config.len = buffer_bytes_;
        segment_config.token_value = token;
        segment_config.flag = register_flag;
        local_segment_ = urma_register_seg(context_, &segment_config);
        if (!local_segment_) return fail(UrmaSessionOpenError::register_segment);

        urma_import_seg_flag_t import_flag {};
        import_flag.bs.cacheable = URMA_NON_CACHEABLE;
        import_flag.bs.access = URMA_ACCESS_READ | URMA_ACCESS_WRITE;
        import_flag.bs.mapping = URMA_SEG_NOMAP;
        urma_seg_t remote_copy = config.remote_segment;
        remote_segment_ = urma_import_seg(context_, &remote_copy, &token, 0, import_flag);
        if (!remote_segment_) return fail(UrmaSessionOpenError::import_segment);

        urma_rjetty_t remote_jetty_config {};
        remote_jetty_config.jetty_id = config.remote_jetty_id;
        remote_jetty_config.trans_mode = URMA_TM_RM;
        remote_jetty_config.type = URMA_JETTY;
        remote_jetty_config.tp_type = URMA_CTP;
        remote_jetty_ = urma_import_jetty(context_, &remote_jetty_config, &token);
        if (!remote_jetty_) return fail(UrmaSessionOpenError::import_jetty);

        open_ = true;
        return success_result();
    }

    UrmaSessionCloseResult close() {
        if (!owns_resources())
            return {UrmaSessionCloseState::already_closed,
                    UrmaSessionResource::none, 0, 0};
        if (!safe_to_destroy_ || teardown_failed_)
            return {UrmaSessionCloseState::quarantined,
                    UrmaSessionResource::none, 0, completions_.in_flight()};
        if (!completions_.empty())
            return {UrmaSessionCloseState::in_flight,
                    UrmaSessionResource::none, 0, completions_.in_flight()};
        return release_resources();
    }

    void quarantine() { safe_to_destroy_ = false; }
    bool is_open() const { return open_; }
    bool is_quarantined() const { return !safe_to_destroy_ || teardown_failed_; }
    size_t in_flight() const { return completions_.in_flight(); }
    uint32_t payload_bytes() const { return payload_bytes_; }
    uint32_t window() const { return window_; }
    size_t buffer_bytes() const { return buffer_bytes_; }

    void *buffer() const { return buffer_; }
    urma_context_t *context() const { return context_; }
    urma_jfc_t *jfc() const { return jfc_; }
    urma_jfr_t *jfr() const { return jfr_; }
    urma_jetty_t *jetty() const { return jetty_; }
    urma_target_seg_t *local_segment() const { return local_segment_; }
    urma_target_seg_t *remote_segment() const { return remote_segment_; }
    urma_target_jetty_t *remote_jetty() const { return remote_jetty_; }
    CompletionTracker &completions() { return completions_; }
    uint64_t &request_id() { return request_id_; }

    UrmaRegionBinding source_binding() const {
        return {{reinterpret_cast<uint64_t>(buffer_), buffer_bytes_}, local_segment_};
    }
    UrmaRegionBinding destination_binding() const {
        return {{remote_descriptor_.ubva.va, remote_descriptor_.len}, remote_segment_};
    }

private:
    static UrmaSessionOpenResult immediate_failure(UrmaSessionOpenError error) {
        UrmaSessionOpenResult result;
        result.error = error;
        return result;
    }

    UrmaSessionOpenResult success_result() const {
        UrmaSessionOpenResult result;
        result.max_jfc_depth = capabilities_.max_jfc_depth;
        result.max_jfs_depth = capabilities_.max_jfs_depth;
        result.max_jfr_depth = capabilities_.max_jfr_depth;
        return result;
    }

    UrmaSessionOpenResult fail(UrmaSessionOpenError error, int provider_status = 0) {
        UrmaSessionOpenResult result = success_result();
        result.error = error;
        result.provider_status = provider_status;
        result.rollback = release_resources();
        return result;
    }

    bool owns_resources() const {
        return context_ || jfc_ || jfr_ || jetty_ || buffer_ ||
               local_segment_ || remote_segment_ || remote_jetty_;
    }

    UrmaSessionCloseResult provider_failure(UrmaSessionResource resource,
                                            urma_status_t status) {
        safe_to_destroy_ = false;
        teardown_failed_ = true;
        return {UrmaSessionCloseState::provider_error, resource,
                static_cast<int>(status), completions_.in_flight()};
    }

    UrmaSessionCloseResult release_resources() {
        urma_status_t status = URMA_SUCCESS;
        if (remote_jetty_) {
            status = urma_unimport_jetty(remote_jetty_);
            if (status != URMA_SUCCESS)
                return provider_failure(UrmaSessionResource::remote_jetty, status);
            remote_jetty_ = nullptr;
        }
        if (remote_segment_) {
            status = urma_unimport_seg(remote_segment_);
            if (status != URMA_SUCCESS)
                return provider_failure(UrmaSessionResource::remote_segment, status);
            remote_segment_ = nullptr;
        }
        if (jetty_) {
            status = urma_delete_jetty(jetty_);
            if (status != URMA_SUCCESS)
                return provider_failure(UrmaSessionResource::local_jetty, status);
            jetty_ = nullptr;
        }
        if (local_segment_) {
            status = urma_unregister_seg(local_segment_);
            if (status != URMA_SUCCESS)
                return provider_failure(UrmaSessionResource::local_segment, status);
            local_segment_ = nullptr;
        }
        std::free(buffer_);
        buffer_ = nullptr;
        buffer_bytes_ = 0;
        if (jfr_) {
            status = urma_delete_jfr(jfr_);
            if (status != URMA_SUCCESS)
                return provider_failure(UrmaSessionResource::receive_queue, status);
            jfr_ = nullptr;
        }
        if (jfc_) {
            status = urma_delete_jfc(jfc_);
            if (status != URMA_SUCCESS)
                return provider_failure(UrmaSessionResource::completion_queue, status);
            jfc_ = nullptr;
        }
        if (context_) {
            status = urma_delete_context(context_);
            if (status != URMA_SUCCESS)
                return provider_failure(UrmaSessionResource::context, status);
            context_ = nullptr;
        }
        open_ = false;
        payload_bytes_ = 0;
        window_ = 0;
        capabilities_ = {};
        remote_descriptor_ = {};
        return {UrmaSessionCloseState::closed,
                UrmaSessionResource::none, 0, 0};
    }

    struct Capabilities {
        uint32_t max_jfc_depth = 0;
        uint32_t max_jfs_depth = 0;
        uint32_t max_jfr_depth = 0;
    } capabilities_;

    urma_context_t *context_ = nullptr;
    urma_jfc_t *jfc_ = nullptr;
    urma_jfr_t *jfr_ = nullptr;
    urma_jetty_t *jetty_ = nullptr;
    urma_target_seg_t *local_segment_ = nullptr;
    urma_target_seg_t *remote_segment_ = nullptr;
    urma_target_jetty_t *remote_jetty_ = nullptr;
    void *buffer_ = nullptr;
    size_t buffer_bytes_ = 0;
    uint32_t payload_bytes_ = 0;
    uint32_t window_ = 0;
    uint64_t request_id_ = 0;
    CompletionTracker completions_;
    urma_seg_t remote_descriptor_ {};
    bool open_ = false;
    bool safe_to_destroy_ = true;
    bool teardown_failed_ = false;
};

}  // namespace gamaq
