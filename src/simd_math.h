#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(_M_ARM64) || defined(__aarch64__)
#include <arm64_neon.h>
#define FRAME_WARP_HAS_ARM64_NEON 1
#else
#define FRAME_WARP_HAS_ARM64_NEON 0
#endif

namespace frame_warp_lab::simd {

inline constexpr const char* backend_name() {
#if FRAME_WARP_HAS_ARM64_NEON
    return "arm64_neon";
#else
    return "scalar";
#endif
}

inline void store_float4(float* destination, float x, float y, float z, float w) {
#if FRAME_WARP_HAS_ARM64_NEON
    alignas(16) const float lanes[4] = {x, y, z, w};
    vst1q_f32(destination, vld1q_f32(lanes));
#else
    destination[0] = x;
    destination[1] = y;
    destination[2] = z;
    destination[3] = w;
#endif
}

inline void store_uint4(std::uint32_t* destination, std::uint32_t x, std::uint32_t y, std::uint32_t z, std::uint32_t w) {
#if FRAME_WARP_HAS_ARM64_NEON
    alignas(16) const std::uint32_t lanes[4] = {x, y, z, w};
    vst1q_u32(destination, vld1q_u32(lanes));
#else
    destination[0] = x;
    destination[1] = y;
    destination[2] = z;
    destination[3] = w;
#endif
}

inline void copy_float4_stream(float* destination, const float* source, std::size_t float4_count) {
#if FRAME_WARP_HAS_ARM64_NEON
    std::size_t index = 0;
    for (; index + 4 <= float4_count; index += 4) {
        const float32x4_t v0 = vld1q_f32(source + (index + 0) * 4);
        const float32x4_t v1 = vld1q_f32(source + (index + 1) * 4);
        const float32x4_t v2 = vld1q_f32(source + (index + 2) * 4);
        const float32x4_t v3 = vld1q_f32(source + (index + 3) * 4);
        vst1q_f32(destination + (index + 0) * 4, v0);
        vst1q_f32(destination + (index + 1) * 4, v1);
        vst1q_f32(destination + (index + 2) * 4, v2);
        vst1q_f32(destination + (index + 3) * 4, v3);
    }
    for (; index < float4_count; ++index) {
        vst1q_f32(destination + index * 4, vld1q_f32(source + index * 4));
    }
#else
    std::memcpy(destination, source, float4_count * 4 * sizeof(float));
#endif
}

inline void copy_uint4_stream(std::uint32_t* destination, const std::uint32_t* source, std::size_t uint4_count) {
#if FRAME_WARP_HAS_ARM64_NEON
    std::size_t index = 0;
    for (; index + 4 <= uint4_count; index += 4) {
        const uint32x4_t v0 = vld1q_u32(source + (index + 0) * 4);
        const uint32x4_t v1 = vld1q_u32(source + (index + 1) * 4);
        const uint32x4_t v2 = vld1q_u32(source + (index + 2) * 4);
        const uint32x4_t v3 = vld1q_u32(source + (index + 3) * 4);
        vst1q_u32(destination + (index + 0) * 4, v0);
        vst1q_u32(destination + (index + 1) * 4, v1);
        vst1q_u32(destination + (index + 2) * 4, v2);
        vst1q_u32(destination + (index + 3) * 4, v3);
    }
    for (; index < uint4_count; ++index) {
        vst1q_u32(destination + index * 4, vld1q_u32(source + index * 4));
    }
#else
    std::memcpy(destination, source, uint4_count * 4 * sizeof(std::uint32_t));
#endif
}

inline void advance_clamped_position(
    float& x,
    float& y,
    float& z,
    float delta_x,
    float delta_y,
    float delta_z,
    float min_x,
    float min_y,
    float min_z,
    float max_x,
    float max_y,
    float max_z) {
#if FRAME_WARP_HAS_ARM64_NEON
    alignas(16) const float pos_lanes[4] = {x, y, z, 0.0f};
    alignas(16) const float delta_lanes[4] = {delta_x, delta_y, delta_z, 0.0f};
    alignas(16) const float min_lanes[4] = {min_x, min_y, min_z, 0.0f};
    alignas(16) const float max_lanes[4] = {max_x, max_y, max_z, 0.0f};
    const float32x4_t updated = vaddq_f32(vld1q_f32(pos_lanes), vld1q_f32(delta_lanes));
    const float32x4_t clamped = vminq_f32(vmaxq_f32(updated, vld1q_f32(min_lanes)), vld1q_f32(max_lanes));
    alignas(16) float out_lanes[4]{};
    vst1q_f32(out_lanes, clamped);
    x = out_lanes[0];
    y = out_lanes[1];
    z = out_lanes[2];
#else
    x = std::clamp(x + delta_x, min_x, max_x);
    y = std::clamp(y + delta_y, min_y, max_y);
    z = std::clamp(z + delta_z, min_z, max_z);
#endif
}

inline void sum_and_minmax_double(const double* data, std::size_t count, double& out_sum, double& out_min, double& out_max) {
    if (count == 0) {
        out_sum = 0.0;
        out_min = 0.0;
        out_max = 0.0;
        return;
    }
#if FRAME_WARP_HAS_ARM64_NEON
    float64x2_t sum_vec = vdupq_n_f64(0.0);
    float64x2_t min_vec = vdupq_n_f64(data[0]);
    float64x2_t max_vec = vdupq_n_f64(data[0]);
    std::size_t index = 0;
    for (; index + 2 <= count; index += 2) {
        const float64x2_t v = vld1q_f64(data + index);
        sum_vec = vaddq_f64(sum_vec, v);
        min_vec = vminq_f64(min_vec, v);
        max_vec = vmaxq_f64(max_vec, v);
    }
    double total = vaddvq_f64(sum_vec);
    double minimum = vminvq_f64(min_vec);
    double maximum = vmaxvq_f64(max_vec);
    for (; index < count; ++index) {
        total += data[index];
        if (data[index] < minimum) minimum = data[index];
        if (data[index] > maximum) maximum = data[index];
    }
    out_sum = total;
    out_min = minimum;
    out_max = maximum;
#else
    double total = 0.0;
    double minimum = data[0];
    double maximum = data[0];
    for (std::size_t index = 0; index < count; ++index) {
        total += data[index];
        if (data[index] < minimum) minimum = data[index];
        if (data[index] > maximum) maximum = data[index];
    }
    out_sum = total;
    out_min = minimum;
    out_max = maximum;
#endif
}

}
