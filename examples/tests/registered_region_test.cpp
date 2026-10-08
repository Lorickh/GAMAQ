#include "registered_region.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
unsigned checks = 0;
void require(bool ok, const char *message) {
    ++checks;
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
}  // namespace

int main() {
    using gamaq::RegisteredRegionView;
    using gamaq::RegionRangeError;
    using gamaq::plan_region_write;
    using gamaq::resolve_region_range;
    constexpr uint64_t max = std::numeric_limits<uint64_t>::max();

    const RegisteredRegionView region {0x1000, 4096};
    auto range = resolve_region_range(region, 512, 1024);
    require(range.ok() && range.range.address == 0x1200 && range.range.bytes == 1024,
            "resolve an interior registered range");
    range = resolve_region_range(region, 0, 4096);
    require(range.ok() && range.range.address == 0x1000,
            "allow an exact registered-region fit");
    require(resolve_region_range(region, 0, 0).error == RegionRangeError::zero_length,
            "reject a zero-length transfer");
    require(resolve_region_range(region, 4096, 1).error == RegionRangeError::out_of_bounds,
            "reject a range starting at the exclusive end");
    require(resolve_region_range(region, 4095, 2).error == RegionRangeError::out_of_bounds,
            "reject a one-byte capacity overrun");
    require(resolve_region_range({max, 2}, 1, 1).error ==
            RegionRangeError::address_overflow, "reject base plus offset overflow");
    require(resolve_region_range({max - 2, 4}, 0, 4).error ==
            RegionRangeError::address_overflow, "reject inclusive last-byte overflow");
    range = resolve_region_range({max - 3, 4}, 0, 4);
    require(range.ok() && range.range.address == max - 3,
            "allow a range whose inclusive last byte is UINT64_MAX");

    auto write = plan_region_write({100, 64}, {1000, 128}, 7, 31, 16);
    require(write.ok() && write.plan.source.address == 107 &&
            write.plan.destination.address == 1031,
            "plan source and destination offsets independently");
    write = plan_region_write({100, 8}, {1000, 128}, 0, 0, 16);
    require(write.source_error == RegionRangeError::out_of_bounds &&
            write.destination_error == RegionRangeError::none,
            "retain source-side validation evidence");
    write = plan_region_write({100, 64}, {max - 4, 8}, 0, 2, 4);
    require(write.source_error == RegionRangeError::none &&
            write.destination_error == RegionRangeError::address_overflow,
            "retain destination-side address overflow evidence");

    // Exhaustively compare a small domain with an independent capacity and
    // last-byte oracle. This exercises offsets at and around both boundaries.
    for (uint64_t capacity = 0; capacity <= 32; ++capacity) {
        for (uint64_t offset = 0; offset <= 34; ++offset) {
            for (uint32_t bytes = 0; bytes <= 16; ++bytes) {
                const auto result = resolve_region_range({max - 40, capacity},
                                                          offset, bytes);
                const bool capacity_ok = bytes != 0 && offset <= capacity &&
                    uint64_t{bytes} <= capacity - offset;
                const bool address_ok = offset <= 40 && bytes != 0 &&
                    uint64_t{bytes} - 1 <= 40 - offset;
                require(result.ok() == (capacity_ok && address_ok),
                        "small-domain region oracle");
                if (result.ok()) {
                    require(result.range.address == max - 40 + offset &&
                            result.range.bytes == bytes,
                            "valid region preserves resolved address and length");
                }
            }
        }
    }

    std::cout << "registered_region: " << checks << " checks passed\n";
}
