#pragma once

#include <cstdint>
#include <limits>

namespace gamaq {

// Numeric view of a region that has already been registered with a transport.
// It does not own or grant access to the provider handle associated with it.
struct RegisteredRegionView {
    uint64_t base_address = 0;
    uint64_t bytes = 0;
};

enum class RegionRangeError {
    none,
    zero_length,
    out_of_bounds,
    address_overflow,
};

inline const char *region_range_error_message(RegionRangeError error) {
    switch (error) {
    case RegionRangeError::none: return "ok";
    case RegionRangeError::zero_length: return "region range length must be nonzero";
    case RegionRangeError::out_of_bounds: return "region range exceeds registered bytes";
    case RegionRangeError::address_overflow: return "region range exceeds uint64 address space";
    }
    return "unknown region range error";
}

struct ResolvedRegionRange {
    uint64_t address = 0;
    uint32_t bytes = 0;
};

struct RegionRangeResult {
    RegionRangeError error = RegionRangeError::none;
    ResolvedRegionRange range;

    bool ok() const { return error == RegionRangeError::none; }
};

// Resolve an offset only after checking both the registered-region capacity
// and the inclusive last address. This is numeric validation; the provider
// handle still determines whether the transport may access the range.
inline RegionRangeResult resolve_region_range(RegisteredRegionView region,
                                              uint64_t offset,
                                              uint32_t bytes) {
    constexpr uint64_t max = std::numeric_limits<uint64_t>::max();
    if (!bytes) return {RegionRangeError::zero_length, {}};
    if (offset > region.bytes || uint64_t{bytes} > region.bytes - offset)
        return {RegionRangeError::out_of_bounds, {}};
    if (offset > max - region.base_address)
        return {RegionRangeError::address_overflow, {}};
    const uint64_t address = region.base_address + offset;
    if (uint64_t{bytes} - 1 > max - address)
        return {RegionRangeError::address_overflow, {}};
    return {RegionRangeError::none, {address, bytes}};
}

struct RegionWritePlan {
    ResolvedRegionRange source;
    ResolvedRegionRange destination;
};

struct RegionWritePlanResult {
    RegionRangeError source_error = RegionRangeError::none;
    RegionRangeError destination_error = RegionRangeError::none;
    RegionWritePlan plan;

    bool ok() const {
        return source_error == RegionRangeError::none &&
               destination_error == RegionRangeError::none;
    }
};

inline RegionWritePlanResult plan_region_write(RegisteredRegionView source,
                                               RegisteredRegionView destination,
                                               uint64_t source_offset,
                                               uint64_t destination_offset,
                                               uint32_t bytes) {
    const auto source_range = resolve_region_range(source, source_offset, bytes);
    const auto destination_range = resolve_region_range(
        destination, destination_offset, bytes);
    return {source_range.error, destination_range.error,
            {source_range.range, destination_range.range}};
}

}  // namespace gamaq
