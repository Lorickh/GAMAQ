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
#include <memory>
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
#include "urma_kv_backend.h"
#include "urma_session.h"
#include "window_scheduler.h"

namespace {
using Clock = std::chrono::steady_clock;
constexpr uint32_t kMagic = 0x48424D55;
constexpr uint32_t kVersion = 1;
constexpr uint32_t kMaxWindow = 64;

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

class UrmaWriteBackend {
public:
    UrmaWriteBackend(gamaq::UrmaSession *context, uint64_t remote_base,
                     uint32_t payload)
        : c_(context), remote_base_(remote_base), payload_(payload) {}

    int post(gamaq::TransferRequest request) {
        const uint64_t slot_offset = uint64_t{request.slot} * payload_;
        urma_sge_t local {};
        local.addr = reinterpret_cast<uint64_t>(c_->buffer()) + slot_offset;
        local.len = request.bytes;
        local.tseg = c_->local_segment();
        urma_sge_t target {};
        target.addr = remote_base_ + slot_offset;
        target.len = request.bytes;
        target.tseg = c_->remote_segment();
        urma_sg_t src {&local, 1};
        urma_sg_t dst {&target, 1};
        urma_rw_wr_t rw {};
        rw.src = src;
        rw.dst = dst;
        urma_jfs_wr_t wr {};
        wr.opcode = URMA_OPC_WRITE;
        wr.flag.bs.complete_enable = 1;
        wr.tjetty = c_->remote_jetty();
        wr.user_ctx = request.request_id;
        wr.rw = rw;
        urma_jfs_wr_t *bad = nullptr;
        return urma_post_jetty_send_wr(c_->jetty(), &wr, &bad);
    }

    int poll(gamaq::TransferCompletion *out, int max_count) {
        std::array<urma_cr_t, gamaq::kMaxCompletionBatch> completions {};
        const int count = urma_poll_jfc(c_->jfc(), max_count, completions.data());
        if (count <= 0) return count;
        if (count > max_count) return count;
        for (int i = 0; i < count; ++i) {
            const auto &completion = completions[static_cast<size_t>(i)];
            out[i] = {completion.user_ctx, completion.completion_len,
                      static_cast<int>(completion.status),
                      completion.status == URMA_CR_SUCCESS};
        }
        return count;
    }

private:
    gamaq::UrmaSession *c_;
    uint64_t remote_base_;
    uint32_t payload_;
};

int window_result_status(const gamaq::WindowRunResult &result) {
    switch (result.error) {
    case gamaq::WindowError::none: return 0;
    case gamaq::WindowError::canceled: return -ECANCELED;
    case gamaq::WindowError::request_id_exhausted: return -EOVERFLOW;
    case gamaq::WindowError::timeout: return -ETIMEDOUT;
    case gamaq::WindowError::post_error:
    case gamaq::WindowError::poll_error:
        return result.backend_status ? result.backend_status : -1;
    default: return -1;
    }
}

int run_write_window(gamaq::UrmaSession *c, uint64_t remote_worker_base, uint32_t size,
                     uint64_t operations, uint64_t timeout_ms,
                     std::atomic<bool> *stop, uint64_t *completed_operations) {
    UrmaWriteBackend backend(c, remote_worker_base, size);
    const auto result = gamaq::run_bounded_window(c->completions(), c->request_id(),
        size, operations, std::chrono::milliseconds(timeout_ms), *stop, backend);
    *completed_operations = result.completed;
    if (result.error == gamaq::WindowError::none) return 0;
    if (!result.safe_to_destroy()) c->quarantine();

    std::fprintf(stderr, "WRITE window failed: %s (submitted=%llu, completed=%llu, "
        "in_flight=%zu, drained=%d)", gamaq::window_error_message(result.error),
        static_cast<unsigned long long>(result.submitted),
        static_cast<unsigned long long>(result.completed), result.in_flight,
        result.drained ? 1 : 0);
    if (result.error == gamaq::WindowError::post_error ||
        result.error == gamaq::WindowError::poll_error)
        std::fprintf(stderr, ", backend_status=%d", result.backend_status);
    if (result.error == gamaq::WindowError::invalid_completion ||
        result.drain_error == gamaq::WindowError::invalid_completion)
        std::fprintf(stderr, ", completion=%s, user_ctx=%llu, status=%d, "
            "expected_bytes=%u, actual_bytes=%u",
            gamaq::completion_error_message(result.completion_error),
            static_cast<unsigned long long>(result.request_id),
            result.completion_status, result.expected_bytes, result.actual_bytes);
    if (result.drain_error != gamaq::WindowError::none)
        std::fprintf(stderr, ", drain_error=%s, drain_status=%d",
            gamaq::window_error_message(result.drain_error),
            result.drain_backend_status);
    std::fputc('\n', stderr);
    return window_result_status(result);
}

struct Result { uint64_t operations = 0, nanoseconds = 0; int status = 0; };

int close_session(gamaq::UrmaSession *session) {
    const auto result = session->close();
    if (result.closed()) return 0;
    if (result.state == gamaq::UrmaSessionCloseState::provider_error) {
        std::fprintf(stderr, "URMA session teardown failed at %s: status=%d; "
            "remaining resources quarantined until process exit\n",
            gamaq::urma_session_resource_message(result.resource),
            result.provider_status);
    } else {
        std::fprintf(stderr, "quarantining URMA session with %zu possibly "
            "in-flight WR(s); process exit owns final provider cleanup\n",
            result.in_flight);
    }
    return -1;
}

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
    std::vector<std::unique_ptr<gamaq::UrmaSession>> contexts;
    contexts.reserve(thread_count);
    for (uint32_t id = 0; id < thread_count; ++id) {
        auto context = std::make_unique<gamaq::UrmaSession>();
        gamaq::UrmaSessionConfig config;
        config.device = device;
        config.eid_index = eid_index;
        config.payload_bytes = size;
        config.window = window;
        config.remote_segment = remote.remote_seg;
        config.remote_jetty_id = remote.remote_jetty_id;
        const auto opened = context->open(config);
        if (!opened.ok()) {
            std::fprintf(stderr, "URMA session open failed: %s",
                gamaq::urma_session_open_error_message(opened.error));
            if (opened.provider_status)
                std::fprintf(stderr, ", provider_status=%d", opened.provider_status);
            if (opened.error == gamaq::UrmaSessionOpenError::insufficient_queue_depth)
                std::fprintf(stderr, " (window=%u, max_jfc=%u, max_jfs=%u, max_jfr=%u)",
                    window, opened.max_jfc_depth, opened.max_jfs_depth,
                    opened.max_jfr_depth);
            if (!opened.rollback.closed())
                std::fprintf(stderr, ", rollback_failed_at=%s, rollback_status=%d",
                    gamaq::urma_session_resource_message(opened.rollback.resource),
                    opened.rollback.provider_status);
            std::fputc('\n', stderr);
            for (auto &item : contexts) (void)close_session(item.get());
            return -1;
        }
        contexts.push_back(std::move(context));
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
            results[id].status = run_write_window(contexts[id].get(), region->address, size,
                warmup, completion_timeout_ms, &stop, &warmed);
            gate.arrive_and_wait();
            if (results[id].status) return;
            auto begin = Clock::now();
            results[id].status = run_write_window(contexts[id].get(), region->address, size,
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
    for (auto &context : contexts) {
        const int close_status = close_session(context.get());
        status = status ? status : close_status;
    }
    std::printf("%u,%u,%u,%llu,%.3f,%.3f,%.3f,%d\n", thread_count, size, window,
                static_cast<unsigned long long>(operations), gbps, mops, avg_us, status);
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
