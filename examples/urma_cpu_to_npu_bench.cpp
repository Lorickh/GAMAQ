// CPU initiated URMA WRITE throughput/latency benchmark for an Ascend NPU HBM.
//
// This is the host-side replacement for run_host_process() in the sample
// described in docs/urma_cpu_to_npu_benchmark.md.  The NPU process and its
// Unix-domain-socket wire protocol are intentionally kept compatible with the
// original sample.
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <mutex>
#include <numeric>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "urma_api.h"
#include "completion_tracker.h"
#include "transfer_layout.h"

namespace {
using Clock = std::chrono::steady_clock;
constexpr uint32_t kMagic = 0x48424D55;
constexpr uint32_t kVersion = 1;
constexpr uint64_t kPageSize = 4096;
constexpr uint32_t kMaxWindow = 64;
constexpr int kMaxPollBatch = 16;

enum class ControlCommand : uint32_t { kHostWriteDone = 1, kFillHbm = 2, kDone = 3 };

// These wire structures must be built with the same URMA headers and ABI as
// the NPU process.  Raw C/C++ structures are suitable only for a local trusted
// test; a production control plane should serialize fields explicitly.
struct NpuHbmExport {
    uint32_t magic = kMagic;
    uint32_t version = kVersion;
    int32_t dev_id = 0;
    uint32_t phy_id = 0;
    uint64_t hbm_ptr = 0;
    uint64_t hbm_len = 0;
    uint64_t target_seg_handle = 0;
    uint32_t token_id = 0;
    uint32_t reserved = 0;
    urma_seg_t remote_seg {};
    urma_jetty_id_t remote_jetty_id {};
};
struct ControlRequest { uint32_t magic = kMagic, command = 0, pattern = 0, reserved = 0; };
struct ControlAck { uint32_t magic = kMagic, command = 0; int32_t status = 0; uint32_t reserved = 0; };

struct Options {
    std::string socket = "/tmp/test_h2d_ub.sock";
    std::string host_eid = "00000000007f060000100000df0a8b01";
    std::vector<uint32_t> threads {1, 2, 4, 8};
    std::vector<uint32_t> sizes {512, 1024, 2048, 4096};
    std::vector<uint32_t> windows {1};
    uint64_t iterations = 10000;
    uint64_t warmup = 100;
    uint64_t completion_timeout_ms = 5000;
};

struct StartGate {
    explicit StartGate(uint32_t count) : remaining(count) {}
    void arrive_and_wait() {
        std::unique_lock<std::mutex> lock(mu);
        if (--remaining == 0) { open = true; cv.notify_all(); }
        else cv.wait(lock, [&] { return open; });
    }
    std::mutex mu;
    std::condition_variable cv;
    uint32_t remaining;
    bool open = false;
};

struct ThreadContext {
    urma_context_t *context = nullptr;
    urma_jfc_t *jfc = nullptr;
    urma_jfr_t *jfr = nullptr;
    urma_jetty_t *jetty = nullptr;
    urma_target_seg_t *local_seg = nullptr;
    urma_target_seg_t *remote_seg = nullptr;
    urma_target_jetty_t *remote_jetty = nullptr;
    void *buffer = nullptr;
    uint64_t request_id = 0;
    gamaq::CompletionTracker completions;
    bool safe_to_destroy = true;
};

bool parse_u64(const char *s, uint64_t *out) {
    char *end = nullptr;
    errno = 0;
    unsigned long long v = std::strtoull(s, &end, 0);
    if (errno || end == s || *end) return false;
    *out = static_cast<uint64_t>(v);
    return true;
}

bool parse_list(const char *s, std::vector<uint32_t> *out) {
    out->clear();
    std::string text(s);
    size_t begin = 0;
    while (begin < text.size()) {
        size_t end = text.find(',', begin);
        uint64_t value = 0;
        std::string item = text.substr(begin, end - begin);
        if (!parse_u64(item.c_str(), &value) || value == 0 || value > UINT32_MAX) return false;
        out->push_back(static_cast<uint32_t>(value));
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return !out->empty();
}

bool parse_options(int argc, char **argv, Options *o) {
    for (int i = 1; i < argc; ++i) {
        auto value = [&](const char *name) -> const char * {
            if (++i >= argc) std::fprintf(stderr, "%s requires a value\n", name);
            return i < argc ? argv[i] : nullptr;
        };
        std::string arg(argv[i]);
        if (arg == "--socket") { const char *v = value("--socket"); if (!v) return false; o->socket = v; }
        else if (arg == "--host-eid") { const char *v = value("--host-eid"); if (!v) return false; o->host_eid = v; }
        else if (arg == "--threads") { const char *v = value("--threads"); if (!v || !parse_list(v, &o->threads)) return false; }
        else if (arg == "--sizes") { const char *v = value("--sizes"); if (!v || !parse_list(v, &o->sizes)) return false; }
        else if (arg == "--windows") { const char *v = value("--windows"); if (!v || !parse_list(v, &o->windows)) return false; }
        else if (arg == "--iterations") { const char *v = value("--iterations"); if (!v || !parse_u64(v, &o->iterations) || !o->iterations) return false; }
        else if (arg == "--warmup") { const char *v = value("--warmup"); if (!v || !parse_u64(v, &o->warmup)) return false; }
        else if (arg == "--completion-timeout-ms") { const char *v = value("--completion-timeout-ms"); if (!v || !parse_u64(v, &o->completion_timeout_ms) || !o->completion_timeout_ms || o->completion_timeout_ms > 3600000) return false; }
        else return false;
    }
    return std::all_of(o->sizes.begin(), o->sizes.end(), [](uint32_t n) {
               return n >= 512 && n <= 4096;
           }) && std::all_of(o->windows.begin(), o->windows.end(), [](uint32_t n) {
               return n <= kMaxWindow;
           });
}

bool parse_eid(const std::string &text, urma_eid_t *eid) {
    std::string s = text.rfind("0x", 0) == 0 ? text.substr(2) : text;
    if (s.size() != 32) return false;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (int i = 0; i < 16; ++i) {
        int hi = nibble(s[2 * i]), lo = nibble(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        eid->raw[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

ssize_t io_retry(int fd, void *p, size_t n, bool write_op) {
    ssize_t rc;
    do { rc = write_op ? write(fd, p, n) : read(fd, p, n); } while (rc < 0 && errno == EINTR);
    return rc;
}
int transfer_all(int fd, void *p, size_t n, bool write_op) {
    auto *bytes = static_cast<uint8_t *>(p);
    for (size_t done = 0; done < n;) {
        ssize_t rc = io_retry(fd, bytes + done, n - done, write_op);
        if (rc <= 0) return -1;
        done += static_cast<size_t>(rc);
    }
    return 0;
}
int connect_socket(const std::string &path) {
    sockaddr_un addr {};
    if (path.size() >= sizeof(addr.sun_path)) return -1;
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
    for (int retry = 0; retry < 600; ++retry) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0) return fd;
        int error = errno; close(fd);
        if (error != ENOENT && error != ECONNREFUSED) return -1;
        usleep(100000);
    }
    return -1;
}

int find_eid_index(urma_device_t *device) {
    uint32_t count = 0;
    urma_eid_info_t *list = urma_get_eid_list(device, &count);
    if (!list || !count) return -1;
    int index = static_cast<int>(list[0].eid_index);
    urma_free_eid_list(list);
    return index;
}

void destroy_context(ThreadContext *c) {
    if (!c->safe_to_destroy || !c->completions.empty()) {
        std::fprintf(stderr, "quarantining URMA context with %zu possibly in-flight WR(s); "
                             "process exit owns final provider cleanup\n",
                     c->completions.in_flight());
        return;
    }
    // Dependents are destroyed before their owners.  Do not share these
    // objects between worker threads: vendor thread-safety is not assumed.
    if (c->remote_jetty) urma_unimport_jetty(c->remote_jetty);
    if (c->remote_seg) urma_unimport_seg(c->remote_seg);
    if (c->jetty) urma_delete_jetty(c->jetty);
    if (c->local_seg) urma_unregister_seg(c->local_seg);
    if (c->jfr) urma_delete_jfr(c->jfr);
    if (c->jfc) urma_delete_jfc(c->jfc);
    if (c->context) urma_delete_context(c->context);
    std::free(c->buffer);
    *c = {};
}

int create_context(urma_device_t *device, uint32_t eid_index, uint32_t size,
                   uint32_t window, const NpuHbmExport &remote, ThreadContext *c) {
    if (!c->completions.reset(window)) return -1;
    c->context = urma_create_context(device, eid_index);
    if (!c->context) return -1;
    urma_device_attr_t attr {};
    if (urma_query_device(device, &attr)) return -1;
    urma_jfc_cfg_t jfc_cfg {};
    // One extra JFC entry is reserved for an asynchronous Jetty error CR, as
    // recommended by the public API guide for one associated Jetty.
    if (attr.dev_cap.max_jfc_depth < window + 1 ||
        attr.dev_cap.max_jfs_depth < window || !attr.dev_cap.max_jfr_depth) {
        std::fprintf(stderr, "device queue depth is smaller than window %u "
            "(max_jfc=%u, max_jfs=%u, max_jfr=%u)\n", window,
            attr.dev_cap.max_jfc_depth, attr.dev_cap.max_jfs_depth,
            attr.dev_cap.max_jfr_depth);
        return -1;
    }
    jfc_cfg.depth = window + 1;
    c->jfc = urma_create_jfc(c->context, &jfc_cfg);
    if (!c->jfc) return -1;
    urma_token_t token {};
    token.token = 0xEFCD;
    urma_jfr_cfg_t jfr_cfg {};
    jfr_cfg.depth = 1;
    jfr_cfg.flag.bs.tag_matching = URMA_NO_TAG_MATCHING;
    jfr_cfg.trans_mode = URMA_TM_RM;
    jfr_cfg.min_rnr_timer = URMA_TYPICAL_MIN_RNR_TIMER;
    jfr_cfg.jfc = c->jfc;
    jfr_cfg.token_value = token;
    jfr_cfg.max_sge = 1;
    c->jfr = urma_create_jfr(c->context, &jfr_cfg);
    if (!c->jfr) return -1;
    urma_jfs_cfg_t jfs {};
    jfs.depth = window;
    jfs.trans_mode = URMA_TM_RM;
    jfs.priority = URMA_MAX_PRIORITY;
    jfs.max_sge = 1;
    jfs.rnr_retry = URMA_TYPICAL_RNR_RETRY;
    jfs.err_timeout = URMA_TYPICAL_ERR_TIMEOUT;
    jfs.jfc = c->jfc;
    urma_jetty_cfg_t jetty_cfg {};
    jetty_cfg.flag.bs.share_jfr = 1;
    jetty_cfg.jfs_cfg = jfs;
    jetty_cfg.shared.jfr = c->jfr;
    c->jetty = urma_create_jetty(c->context, &jetty_cfg);
    if (!c->jetty) return -1;
    const size_t buffer_bytes = static_cast<size_t>(size) * window;
    c->buffer = memalign(kPageSize, buffer_bytes);
    if (!c->buffer) return -1;
    std::memset(c->buffer, 0xA5, buffer_bytes);
    urma_reg_seg_flag_t reg_flag {};
    reg_flag.bs.token_policy = URMA_TOKEN_NONE;
    reg_flag.bs.cacheable = URMA_NON_CACHEABLE;
    reg_flag.bs.access = URMA_ACCESS_READ | URMA_ACCESS_WRITE;
    urma_seg_cfg_t seg_cfg {};
    seg_cfg.va = reinterpret_cast<uint64_t>(c->buffer);
    seg_cfg.len = buffer_bytes;
    seg_cfg.token_value = token;
    seg_cfg.flag = reg_flag;
    c->local_seg = urma_register_seg(c->context, &seg_cfg);
    if (!c->local_seg) return -1;
    urma_import_seg_flag_t import_flag {};
    import_flag.bs.cacheable = URMA_NON_CACHEABLE;
    import_flag.bs.access = URMA_ACCESS_READ | URMA_ACCESS_WRITE;
    import_flag.bs.mapping = URMA_SEG_NOMAP;
    // The public API accepts a mutable segment descriptor. Import a copy so
    // the provider cannot mutate the control-plane export retained by callers.
    urma_seg_t remote_seg = remote.remote_seg;
    c->remote_seg = urma_import_seg(c->context, &remote_seg, &token, 0, import_flag);
    if (!c->remote_seg) return -1;
    urma_rjetty_t rjetty {};
    rjetty.jetty_id = remote.remote_jetty_id;
    rjetty.trans_mode = URMA_TM_RM;
    rjetty.type = URMA_JETTY;
    rjetty.tp_type = URMA_CTP;
    c->remote_jetty = urma_import_jetty(c->context, &rjetty, &token);
    return c->remote_jetty ? 0 : -1;
}

int run_write_window(ThreadContext *c, uint64_t remote_worker_base, uint32_t size,
                     uint64_t operations, uint64_t timeout_ms,
                     std::atomic<bool> *stop, uint64_t *completed_operations) {
    *completed_operations = 0;
    auto fail = [stop](int status) {
        stop->store(true, std::memory_order_relaxed);
        return status;
    };
    auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    uint64_t submitted = 0;
    uint64_t polls = 0;
    while (*completed_operations < operations) {
        const bool stopping = stop->load(std::memory_order_relaxed);
        if (stopping && c->completions.empty()) return -ECANCELED;
        while (!stop->load(std::memory_order_relaxed) && submitted < operations &&
               c->completions.can_submit()) {
            if (c->request_id == UINT64_MAX) {
                std::fprintf(stderr, "WR request id exhausted\n");
                return fail(-EOVERFLOW);
            }
            const uint64_t request_id = ++c->request_id;
            const auto reservation = c->completions.reserve(request_id, size);
            if (reservation.error != gamaq::CompletionError::none) {
                std::fprintf(stderr, "cannot reserve WR %llu: %s\n",
                    static_cast<unsigned long long>(request_id),
                    gamaq::completion_error_message(reservation.error));
                return fail(-1);
            }
            const uint64_t slot_offset = uint64_t{reservation.slot} * size;
            urma_sge_t local {};
            local.addr = reinterpret_cast<uint64_t>(c->buffer) + slot_offset;
            local.len = size;
            local.tseg = c->local_seg;
            urma_sge_t target {};
            target.addr = remote_worker_base + slot_offset;
            target.len = size;
            target.tseg = c->remote_seg;
            urma_sg_t src {&local, 1};
            urma_sg_t dst {&target, 1};
            urma_rw_wr_t rw {};
            rw.src = src;
            rw.dst = dst;
            urma_jfs_wr_t wr {};
            wr.opcode = URMA_OPC_WRITE;
            wr.flag.bs.complete_enable = 1;
            wr.tjetty = c->remote_jetty;
            wr.user_ctx = request_id;
            wr.rw = rw;
            urma_jfs_wr_t *bad = nullptr;
            const int rc = urma_post_jetty_send_wr(c->jetty, &wr, &bad);
            if (rc) {
                c->completions.cancel_unposted(request_id);
                std::fprintf(stderr, "urma_post_jetty_send_wr failed for WR %llu: %d\n",
                    static_cast<unsigned long long>(request_id), rc);
                return fail(rc);
            }
            ++submitted;
        }

        std::array<urma_cr_t, kMaxPollBatch> completions {};
        const int requested = std::min<int>(kMaxPollBatch,
            static_cast<int>(c->completions.in_flight()));
        if (!requested)
            return stop->load(std::memory_order_relaxed) ? -ECANCELED : fail(-1);
        const int count = urma_poll_jfc(c->jfc, requested, completions.data());
        if (count < 0) {
            c->safe_to_destroy = false;
            std::fprintf(stderr, "urma_poll_jfc failed with %zu WR(s) in flight: %d\n",
                c->completions.in_flight(), count);
            return fail(count);
        }
        if (count > requested) {
            c->safe_to_destroy = false;
            std::fprintf(stderr, "urma_poll_jfc returned %d CRs into a %d-entry request\n",
                         count, requested);
            return fail(-1);
        }
        if (count) {
            for (int i = 0; i < count; ++i) {
                const auto &completion = completions[static_cast<size_t>(i)];
                const auto check = c->completions.complete(completion.user_ctx,
                    completion.status == URMA_CR_SUCCESS, completion.completion_len);
                if (!check.request_retired) c->safe_to_destroy = false;
                if (check.error != gamaq::CompletionError::none) {
                    std::fprintf(stderr, "invalid completion: %s "
                        "(user_ctx=%llu, status=%d, expected_bytes=%u, actual_bytes=%u)\n",
                        gamaq::completion_error_message(check.error),
                        static_cast<unsigned long long>(completion.user_ctx),
                        static_cast<int>(completion.status), check.expected_bytes,
                        completion.completion_len);
                    return fail(-1);
                }
                ++*completed_operations;
            }
            deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
            polls = 0;
            continue;
        }
        // Busy polling is intentional. Reading the wall clock every 256 empty
        // polls bounds overhead; every successful batch refreshes the stall
        // deadline, so a long healthy run does not time out.
        if ((++polls & 0xff) == 0 && Clock::now() >= deadline) {
            c->safe_to_destroy = false;
            std::fprintf(stderr, "completion stalled with %zu WR(s) in flight after %llu ms\n",
                c->completions.in_flight(),
                static_cast<unsigned long long>(timeout_ms));
            return fail(-ETIMEDOUT);
        }
    }
    return 0;
}

struct Result { uint64_t operations = 0, nanoseconds = 0; int status = 0; };

bool check_layout(const NpuHbmExport &remote, uint32_t threads, uint32_t window,
                  uint32_t size) {
    const auto error = gamaq::validate_window_layout(remote.remote_seg.ubva.va,
        remote.hbm_len, remote.remote_seg.len, threads, window, size);
    if (error == gamaq::LayoutError::none) return true;
    std::fprintf(stderr, "invalid WRITE layout: %s (threads=%u, window=%u, payload=%u, "
        "required_bytes=%llu, hbm_bytes=%llu, segment_bytes=%llu)\n",
        gamaq::layout_error_message(error), threads, window, size,
        static_cast<unsigned long long>(uint64_t{threads} * window * size),
        static_cast<unsigned long long>(remote.hbm_len),
        static_cast<unsigned long long>(remote.remote_seg.len));
    return false;
}

int run_case(urma_device_t *device, uint32_t eid_index, const NpuHbmExport &remote,
             uint32_t thread_count, uint32_t size, uint32_t window, uint64_t warmup,
             uint64_t iterations, uint64_t completion_timeout_ms) {
    if (!check_layout(remote, thread_count, window, size)) return -1;
    std::vector<ThreadContext> contexts(thread_count);
    for (auto &context : contexts) {
        if (create_context(device, eid_index, size, window, remote, &context)) {
            for (auto &item : contexts) destroy_context(&item);
            return -1;
        }
    }
    StartGate gate(thread_count);
    std::atomic<bool> stop {false};
    std::vector<Result> results(thread_count);
    std::vector<std::thread> workers;
    for (uint32_t id = 0; id < thread_count; ++id) {
        workers.emplace_back([&, id] {
            // Validated before resource creation; never fall back to a shared
            // destination. Planning stays outside the timed path.
            const auto region = gamaq::plan_window_slot(remote.remote_seg.ubva.va,
                remote.hbm_len, remote.remote_seg.len, thread_count, window, size, id, 0);
            if (!region) {
                results[id].status = -1;
                stop.store(true, std::memory_order_relaxed);
                gate.arrive_and_wait();
                return;
            }
            uint64_t warmed = 0;
            results[id].status = run_write_window(&contexts[id], region->address, size,
                warmup, completion_timeout_ms, &stop, &warmed);
            gate.arrive_and_wait();
            if (results[id].status) return;
            auto begin = Clock::now();
            results[id].status = run_write_window(&contexts[id], region->address, size,
                iterations, completion_timeout_ms, &stop, &results[id].operations);
            results[id].nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin).count();
        });
    }
    for (auto &worker : workers) worker.join();
    int status = 0;
    uint64_t operations = 0, wall_ns = 0, sum_ns = 0;
    for (const auto &r : results) {
        status = status ? status : r.status;
        operations += r.operations;
        wall_ns = std::max(wall_ns, r.nanoseconds);
        sum_ns += r.nanoseconds;
    }
    double seconds = wall_ns / 1e9;
    double gbps = seconds ? (operations * double(size) * 8.0 / seconds / 1e9) : 0;
    double mops = seconds ? operations / seconds / 1e6 : 0;
    double avg_us = operations ? (sum_ns / 1000.0 / operations) : 0;
    std::printf("%u,%u,%u,%llu,%.3f,%.3f,%.3f,%d\n", thread_count, size, window,
                static_cast<unsigned long long>(operations), gbps, mops, avg_us, status);
    for (auto &context : contexts) destroy_context(&context);
    return status;
}

int send_control(int fd, ControlCommand command) {
    ControlRequest request {};
    request.command = static_cast<uint32_t>(command);
    if (transfer_all(fd, &request, sizeof(request), true)) return -1;
    ControlAck ack {};
    if (transfer_all(fd, &ack, sizeof(ack), false)) return -1;
    return ack.magic == kMagic && ack.command == request.command ? ack.status : -1;
}
}  // namespace

int main(int argc, char **argv) {
    Options options;
    if (!parse_options(argc, argv, &options)) {
        std::fprintf(stderr, "Usage: %s [--socket PATH] [--host-eid HEX] "
                             "[--threads 1,2,4,8] [--sizes 512,1024,2048,4096] "
                             "[--windows 1,2,4,8,16,32,64] "
                             "[--warmup N] [--iterations N] "
                             "[--completion-timeout-ms N]\n", argv[0]);
        return 2;
    }
    int fd = connect_socket(options.socket);
    if (fd < 0) { std::perror("connect"); return 1; }
    NpuHbmExport remote {};
    if (transfer_all(fd, &remote, sizeof(remote), false) || remote.magic != kMagic ||
        remote.version != kVersion) {
        std::fprintf(stderr, "invalid NPU export\n"); close(fd); return 1;
    }
    // Validate the largest footprint in the entire matrix before any URMA
    // resources or WRs, so an invalid later case cannot leave partial results.
    if (!check_layout(remote, *std::max_element(options.threads.begin(), options.threads.end()),
                      *std::max_element(options.windows.begin(), options.windows.end()),
                      *std::max_element(options.sizes.begin(), options.sizes.end()))) {
        close(fd);
        return 2;
    }
    urma_eid_t eid {};
    if (!parse_eid(options.host_eid, &eid)) { std::fprintf(stderr, "invalid host EID\n"); return 2; }
    urma_device_t *device = urma_get_device_by_eid(eid, URMA_TRANSPORT_UB);
    if (!device) { std::fprintf(stderr, "host UB device/EID not found\n"); return 1; }
    int eid_index = find_eid_index(device);
    if (eid_index < 0) { std::fprintf(stderr, "host EID index not found\n"); return 1; }
    std::puts("threads,size_bytes,window,operations,gbps,mops,"
              "avg_completion_interval_us,status");
    int status = 0;
    bool failed = false;
    for (uint32_t threads : options.threads) {
        for (uint32_t size : options.sizes) {
            for (uint32_t window : options.windows) {
                if (run_case(device, static_cast<uint32_t>(eid_index), remote, threads,
                             size, window, options.warmup, options.iterations,
                             options.completion_timeout_ms)) {
                    status = 1;
                    failed = true;
                    break;
                }
            }
            if (failed) break;
        }
        if (failed) break;
    }
    // Only a fully completed matrix may be presented to the NPU as valid.
    if (!failed) {
        if (send_control(fd, ControlCommand::kHostWriteDone)) status = 1;
        if (send_control(fd, ControlCommand::kDone)) status = 1;
    }
    close(fd);
    return status;
}
