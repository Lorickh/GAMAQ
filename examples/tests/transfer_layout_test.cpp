#include "transfer_layout.h"

#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
unsigned checks = 0;
void require(bool ok, const char *message) {
    ++checks;
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
}

int main() {
    using gamaq::LayoutError;
    using gamaq::plan_write_region;
    using gamaq::validate_write_layout;
    constexpr auto max = std::numeric_limits<uint64_t>::max();

    // Regressions: the old 8 KiB fallback sent workers 2 and 3 to worker 0.
    require(validate_write_layout(0x10000, 8192, 8192, 4, 4096) ==
            LayoutError::hbm_too_small, "reject old overlapping fallback");
    require(!plan_write_region(0x10000, 8192, 8192, 4, 4096, 0),
            "reject entire layout, including individually fitting worker");
    require(validate_write_layout(0x10000, 16384, 8192, 4, 4096) ==
            LayoutError::segment_too_small, "HBM length does not authorize segment overrun");
    const auto last = plan_write_region(0x10000, 8192, 8192, 2, 4096, 1);
    require(last && last->offset == 4096 && last->address == 0x11000 &&
            last->bytes == 4096, "exact fit preserves legacy valid address");
    require(!plan_write_region(0x10000, 8192, 8192, 2, 4096, 2), "reject worker index");
    require(validate_write_layout(0, max, max, 0, 1) == LayoutError::empty, "zero workers");
    require(validate_write_layout(0, max, max, 1, 0) == LayoutError::empty, "zero payload");
    require(validate_write_layout(0, 0, max, 1, 1) == LayoutError::hbm_too_small, "zero HBM");
    require(validate_write_layout(0, max, 0, 1, 1) == LayoutError::segment_too_small, "zero segment");
    require(validate_write_layout(0, max, max, max, 2) ==
            LayoutError::byte_count_overflow, "multiplication wrap");
    require(validate_write_layout(max - 4094, 4096, 4096, 1, 4096) ==
            LayoutError::address_overflow, "destination last-byte wrap");
    require(validate_write_layout(max - 4095, 4096, 4096, 1, 4096) ==
            LayoutError::none, "inclusive last byte UINT64_MAX is representable");
    const auto top = plan_write_region(max - 1, 2, 2, 2, 1, 1);
    require(top && top->address == max, "last worker at address-space boundary");
    require(validate_write_layout(0, max, max, max, 1) == LayoutError::none,
            "large valid layout without allocation");

    // Small-domain coverage: independently enumerate the expected next byte
    // and check capacity, adjacency, and disjointness without large arithmetic.
    for (uint64_t workers = 1; workers <= 8; ++workers) {
        for (uint64_t payload = 1; payload <= 16; ++payload) {
            const uint64_t required = workers * payload;
            for (uint64_t capacity = 0; capacity <= required + 1; ++capacity) {
                const bool fits = required <= capacity;
                require((validate_write_layout(100, capacity, max, workers, payload) ==
                         LayoutError::none) == fits, "HBM capacity sweep");
                require((validate_write_layout(100, max, capacity, workers, payload) ==
                         LayoutError::none) == fits, "segment capacity sweep");
            }
            uint64_t next_byte = 100;
            for (uint64_t id = 0; id < workers; ++id) {
                const auto region = plan_write_region(100, required, required, workers, payload, id);
                require(region && region->address == next_byte, "no overlap and no holes");
                next_byte += payload;
                require(next_byte <= 100 + required, "bounded destination end");
            }
        }
    }
    // Requested benchmark matrix, including 16/32 workers, needs 128 KiB.
    for (uint64_t workers : {1, 2, 4, 8, 16, 32})
        for (uint64_t payload : {512, 1024, 2048, 4096})
            require(validate_write_layout(0x120000000000, 131072, 131072, workers,
                    payload) == LayoutError::none, "requested benchmark matrix");

    std::cout << "transfer_layout: " << checks << " checks passed\n";
}
