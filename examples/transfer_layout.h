#pragma once

#include <cstdint>
#include <limits>
#include <optional>

namespace gamaq {

enum class LayoutError {
    none, empty, byte_count_overflow, hbm_too_small, segment_too_small,
    address_overflow
};

inline const char *layout_error_message(LayoutError error) {
    switch (error) {
    case LayoutError::none: return "ok";
    case LayoutError::empty: return "worker count, slot count, and payload must be nonzero";
    case LayoutError::byte_count_overflow: return "worker count * slot count * payload overflows uint64";
    case LayoutError::hbm_too_small: return "HBM allocation cannot hold disjoint worker ranges";
    case LayoutError::segment_too_small: return "registered segment cannot hold disjoint worker ranges";
    case LayoutError::address_overflow: return "last destination byte exceeds uint64 address space";
    }
    return "unknown layout error";
}

// The v1 export protocol assumes HBM byte zero corresponds to segment_base.
// This checks numeric bounds only: it cannot authenticate peer metadata, prove
// an HBM/UBVA mapping, or grant access to an imported segment.
inline LayoutError validate_window_layout(uint64_t segment_base, uint64_t hbm_bytes,
                                         uint64_t segment_bytes, uint64_t workers,
                                         uint64_t slots, uint64_t payload) {
    constexpr auto max = std::numeric_limits<uint64_t>::max();
    if (!workers || !slots || !payload) return LayoutError::empty;
    if (workers > max / slots) return LayoutError::byte_count_overflow;
    const uint64_t ranges = workers * slots;
    if (ranges > max / payload) return LayoutError::byte_count_overflow;
    const uint64_t bytes = ranges * payload;
    if (bytes > hbm_bytes) return LayoutError::hbm_too_small;
    if (bytes > segment_bytes) return LayoutError::segment_too_small;
    // Check the inclusive last byte without computing base + bytes (which
    // would overflow for an otherwise valid range ending at UINT64_MAX).
    if (bytes - 1 > max - segment_base) return LayoutError::address_overflow;
    return LayoutError::none;
}

inline LayoutError validate_write_layout(uint64_t segment_base, uint64_t hbm_bytes,
                                        uint64_t segment_bytes, uint64_t workers,
                                        uint64_t payload) {
    return validate_window_layout(segment_base, hbm_bytes, segment_bytes,
                                  workers, 1, payload);
}

struct WriteRegion {
    uint64_t offset;
    uint64_t address;
    uint64_t bytes;
};

inline std::optional<WriteRegion> plan_write_region(uint64_t segment_base,
                                                  uint64_t hbm_bytes,
                                                  uint64_t segment_bytes,
                                                  uint64_t workers,
                                                  uint64_t payload,
                                                  uint64_t worker) {
    if (worker >= workers || validate_write_layout(segment_base, hbm_bytes,
            segment_bytes, workers, payload) != LayoutError::none) return std::nullopt;
    const uint64_t offset = worker * payload;
    return WriteRegion{offset, segment_base + offset, payload};
}

inline std::optional<WriteRegion> plan_window_slot(uint64_t segment_base,
                                                  uint64_t hbm_bytes,
                                                  uint64_t segment_bytes,
                                                  uint64_t workers,
                                                  uint64_t slots,
                                                  uint64_t payload,
                                                  uint64_t worker,
                                                  uint64_t slot) {
    if (worker >= workers || slot >= slots ||
        validate_window_layout(segment_base, hbm_bytes, segment_bytes,
                               workers, slots, payload) != LayoutError::none)
        return std::nullopt;
    const uint64_t offset = (worker * slots + slot) * payload;
    return WriteRegion{offset, segment_base + offset, payload};
}

}  // namespace gamaq
