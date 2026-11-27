#include "pbr_assets.h"
#include "gltf_loader.h"
#include "job_system.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_SIMD
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace frame_warp_lab {

namespace {

constexpr UINT k_material_size = 512;
constexpr UINT k_material_mips = 10;
constexpr UINT k_material_count = 12;
constexpr UINT k_dfg_size = 128;
constexpr UINT k_env_size = 64;
constexpr UINT k_env_mips = 6;
constexpr float k_pi = 3.14159265358979323846f;

void check_hr(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        throw std::system_error(static_cast<int>(result), std::system_category(), operation);
    }
}

struct Vec3 {
    float x;
    float y;
    float z;
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot3(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross3(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length3(Vec3 v) { return std::sqrt(std::max(0.0f, dot3(v, v))); }
inline Vec3 normalize3(Vec3 v) {
    const float len = length3(v);
    return len > 1e-6f ? Vec3{v.x / len, v.y / len, v.z / len} : Vec3{0.0f, 0.0f, 1.0f};
}
inline Vec3 lerp3(Vec3 a, Vec3 b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}
inline float saturate(float x) { return std::clamp(x, 0.0f, 1.0f); }
inline float smoothstep_f(float e0, float e1, float x) {
    const float t = saturate((x - e0) / std::max(e1 - e0, 1e-6f));
    return t * t * (3.0f - 2.0f * t);
}

inline float srgb_to_linear(float c) {
    c = saturate(c);
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

inline float linear_to_srgb(float c) {
    c = saturate(c);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

inline std::uint8_t float_to_u8(float x) {
    return static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::lround(saturate(x) * 255.0f)), 0, 255));
}

std::uint16_t float_to_half(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const std::uint32_t sign = (bits >> 16U) & 0x8000U;
    std::int32_t exp = static_cast<std::int32_t>((bits >> 23U) & 0xFFU) - 127 + 15;
    std::uint32_t mant = bits & 0x007FFFFFU;
    if (exp <= 0) {
        if (exp < -10) return static_cast<std::uint16_t>(sign);
        mant |= 0x00800000U;
        const std::uint32_t shift = static_cast<std::uint32_t>(14 - exp);
        return static_cast<std::uint16_t>(sign | (mant >> shift));
    }
    if (exp >= 31) {
        return static_cast<std::uint16_t>(sign | 0x7BFFU);
    }
    return static_cast<std::uint16_t>(sign | (static_cast<std::uint32_t>(exp) << 10U) | (mant >> 13U));
}

float hash2d(int x, int y, int seed) {
    std::uint32_t h = static_cast<std::uint32_t>(x * 374761393 + y * 668265263 + seed * 1442695041);
    h = (h ^ (h >> 13U)) * 1274126177U;
    h ^= (h >> 16U);
    return static_cast<float>(h & 0x00FFFFFFU) / static_cast<float>(0x01000000U);
}

float tileable_value_noise(float u, float v, int period, int seed) {
    const float fx = u * static_cast<float>(period);
    const float fy = v * static_cast<float>(period);
    const int x0 = static_cast<int>(std::floor(fx));
    const int y0 = static_cast<int>(std::floor(fy));
    const float tx = fx - static_cast<float>(x0);
    const float ty = fy - static_cast<float>(y0);
    const float sx = tx * tx * (3.0f - 2.0f * tx);
    const float sy = ty * ty * (3.0f - 2.0f * ty);
    auto wrap = [period](int c) {
        const int m = c % period;
        return m < 0 ? m + period : m;
    };
    const int ix0 = wrap(x0);
    const int iy0 = wrap(y0);
    const int ix1 = wrap(x0 + 1);
    const int iy1 = wrap(y0 + 1);
    const float n00 = hash2d(ix0, iy0, seed);
    const float n10 = hash2d(ix1, iy0, seed);
    const float n01 = hash2d(ix0, iy1, seed);
    const float n11 = hash2d(ix1, iy1, seed);
    const float nx0 = n00 + (n10 - n00) * sx;
    const float nx1 = n01 + (n11 - n01) * sx;
    return nx0 + (nx1 - nx0) * sy;
}

float tileable_fbm(float u, float v, int base_period, int octaves, int seed) {
    float sum = 0.0f;
    float amp = 0.5f;
    float norm = 0.0f;
    int period = base_period;
    for (int o = 0; o < octaves; ++o) {
        sum += amp * tileable_value_noise(u, v, period, seed + o * 31);
        norm += amp;
        amp *= 0.5f;
        period *= 2;
    }
    return norm > 0.0f ? sum / norm : 0.0f;
}

float tileable_voronoi_edge(float u, float v, int period, int seed) {
    const float fx = u * static_cast<float>(period);
    const float fy = v * static_cast<float>(period);
    const int cx = static_cast<int>(std::floor(fx));
    const int cy = static_cast<int>(std::floor(fy));
    auto wrap = [period](int c) {
        const int m = c % period;
        return m < 0 ? m + period : m;
    };
    float f1 = 10.0f;
    float f2 = 10.0f;
    for (int oy = -1; oy <= 1; ++oy) {
        for (int ox = -1; ox <= 1; ++ox) {
            const int gx = wrap(cx + ox);
            const int gy = wrap(cy + oy);
            const float jx = 0.15f + 0.70f * hash2d(gx, gy, seed);
            const float jy = 0.15f + 0.70f * hash2d(gx, gy, seed + 53);
            const float px = static_cast<float>(cx + ox) + jx - fx;
            const float py = static_cast<float>(cy + oy) + jy - fy;
            const float d = std::sqrt(px * px + py * py);
            if (d < f1) {
                f2 = f1;
                f1 = d;
            } else if (d < f2) {
                f2 = d;
            }
        }
    }
    return f2 - f1;
}

struct MaterialPixel {
    Vec3 albedo_linear;
    float height;
    float ao;
    float roughness;
    float metallic;
    float clearcoat;
};

MaterialPixel evaluate_material_texel(UINT slice, float u, float v) {
    MaterialPixel p{};
    p.ao = 1.0f;
    p.roughness = 0.5f;
    p.metallic = 0.0f;
    p.clearcoat = 0.0f;

    if (slice == 0U) {
        const float tile_u = std::fmod(u * 2.0f, 1.0f);
        const float tile_v = std::fmod(v * 2.0f, 1.0f);
        const int cell_x = static_cast<int>(std::floor(u * 2.0f));
        const int cell_y = static_cast<int>(std::floor(v * 2.0f));
        const float edge_u = std::min(tile_u, 1.0f - tile_u);
        const float edge_v = std::min(tile_v, 1.0f - tile_v);
        const float grout = 1.0f - smoothstep_f(0.012f, 0.032f, std::min(edge_u, edge_v));
        const float grain = tileable_fbm(u, v, 16, 4, 101);
        const float macro = tileable_fbm(u, v, 4, 3, 211);
        const float damp = smoothstep_f(0.56f, 0.74f, macro);
        const bool alt = ((cell_x + cell_y) & 1) != 0;
        const Vec3 base_a = alt ? Vec3{0.055f, 0.068f, 0.078f} : Vec3{0.038f, 0.048f, 0.056f};
        const Vec3 base_b = alt ? Vec3{0.095f, 0.112f, 0.125f} : Vec3{0.068f, 0.082f, 0.092f};
        Vec3 col = lerp3(base_a, base_b, grain);
        col = lerp3(col, Vec3{0.022f, 0.025f, 0.028f}, grout);
        col = col * (1.0f - 0.18f * damp);
        p.albedo_linear = col;
        p.height = (0.55f + 0.18f * grain + 0.08f * macro) * (1.0f - 0.85f * grout);
        p.ao = saturate(1.0f - 0.55f * grout - 0.12f * (1.0f - grain));
        p.roughness = saturate(lerp3({0.58f, 0.0f, 0.0f}, {0.16f, 0.0f, 0.0f}, damp).x + 0.16f * grain + 0.30f * grout);
        p.metallic = 0.0f;
        p.clearcoat = damp * (1.0f - grout) * 0.45f;
    } else if (slice == 1U) {
        const float pu = std::fmod(u * 2.0f, 1.0f);
        const float pv = std::fmod(v * 2.0f, 1.0f);
        const float edge_u = std::min(pu, 1.0f - pu);
        const float edge_v = std::min(pv, 1.0f - pv);
        const float seam = 1.0f - smoothstep_f(0.010f, 0.024f, std::min(edge_u, edge_v));
        const float chamfer = smoothstep_f(0.020f, 0.042f, std::min(edge_u, edge_v));
        const float brush = tileable_value_noise(u * 1.0f, v * 16.0f, 32, 307) * 0.55f
            + tileable_value_noise(u * 2.0f, v * 32.0f, 64, 313) * 0.45f;
        const float scratch = smoothstep_f(0.84f, 0.94f, tileable_value_noise(u * 24.0f, v * 3.0f, 32, 349));
        const float wear = tileable_fbm(u, v, 8, 4, 331);
        const float oil = smoothstep_f(0.62f, 0.80f, tileable_fbm(u, v, 4, 3, 367));
        const float rx = std::min(std::fabs(pu - 0.075f), std::fabs(pu - 0.925f));
        const float ry = std::min(std::fabs(pv - 0.075f), std::fabs(pv - 0.925f));
        const float rd = std::sqrt(rx * rx + ry * ry);
        const float rivet = 1.0f - smoothstep_f(0.012f, 0.022f, rd);
        Vec3 metal_col = lerp3(Vec3{0.26f, 0.30f, 0.35f}, Vec3{0.52f, 0.57f, 0.62f}, brush * 0.62f + wear * 0.38f);
        metal_col = lerp3(metal_col, Vec3{0.68f, 0.72f, 0.76f}, scratch * 0.55f);
        metal_col = metal_col * (1.0f - 0.22f * oil);
        const Vec3 gasket_col{0.025f, 0.028f, 0.032f};
        p.albedo_linear = lerp3(metal_col, gasket_col, seam);
        p.albedo_linear = lerp3(p.albedo_linear, Vec3{0.66f, 0.69f, 0.72f}, rivet * 0.6f);
        p.height = (0.50f + 0.12f * chamfer + 0.06f * brush - 0.04f * scratch) * (1.0f - 0.88f * seam) + 0.26f * rivet;
        p.ao = saturate(1.0f - 0.72f * seam - 0.12f * (1.0f - wear) - 0.10f * oil);
        p.roughness = saturate(0.20f + 0.18f * (1.0f - brush) + 0.14f * wear - 0.10f * scratch - 0.08f * oil + 0.48f * seam);
        p.metallic = saturate(0.92f * (1.0f - seam) + 0.06f * rivet);
        p.clearcoat = oil * 0.25f * (1.0f - seam);
    } else if (slice == 2U) {
        const float rib = 0.5f + 0.5f * std::sin(v * 2.0f * k_pi * 12.0f);
        const float inlay_u = std::fabs(std::fmod(u * 3.0f, 1.0f) - 0.5f);
        const float brass_strip = 1.0f - smoothstep_f(0.030f, 0.048f, inlay_u);
        const float tile_gap = 1.0f - smoothstep_f(0.008f, 0.022f, std::min(std::fmod(u * 3.0f, 1.0f), 1.0f - std::fmod(u * 3.0f, 1.0f)));
        const float grain = tileable_fbm(u, v, 12, 3, 409);
        const Vec3 ceramic = lerp3(Vec3{0.05f, 0.07f, 0.09f}, Vec3{0.11f, 0.14f, 0.18f}, rib * 0.6f + grain * 0.4f);
        const Vec3 brass = lerp3(Vec3{0.78f, 0.56f, 0.22f}, Vec3{0.96f, 0.78f, 0.38f}, grain);
        p.albedo_linear = lerp3(ceramic, brass, brass_strip);
        p.albedo_linear = p.albedo_linear * (1.0f - 0.70f * tile_gap);
        p.height = (0.35f + 0.35f * rib + 0.08f * grain) * (1.0f - brass_strip) + 0.78f * brass_strip;
        p.height *= (1.0f - 0.85f * tile_gap);
        p.ao = saturate(0.68f + 0.32f * rib - 0.55f * tile_gap);
        p.roughness = saturate(lerp3({0.54f, 0.0f, 0.0f}, {0.22f, 0.0f, 0.0f}, brass_strip).x + 0.14f * grain);
        p.metallic = brass_strip;
        p.clearcoat = (1.0f - brass_strip) * 0.18f;
    } else if (slice == 3U) {
        const float band_v = std::fmod(v * 2.0f, 1.0f);
        const float paint_mask = smoothstep_f(0.46f, 0.50f, band_v) * (1.0f - smoothstep_f(0.96f, 1.00f, band_v));
        const float seam_line = 1.0f - smoothstep_f(0.015f, 0.035f, std::fabs(band_v - 0.48f));
        const float brush = tileable_value_noise(u * 12.0f, v * 2.0f, 16, 503);
        const float micro = tileable_fbm(u, v, 16, 3, 521);
        const Vec3 enamel = lerp3(Vec3{0.68f, 0.16f, 0.02f}, Vec3{0.88f, 0.26f, 0.04f}, micro);
        const Vec3 steel = lerp3(Vec3{0.14f, 0.16f, 0.19f}, Vec3{0.26f, 0.30f, 0.34f}, brush);
        p.albedo_linear = lerp3(steel, enamel, paint_mask) * (1.0f - 0.55f * seam_line);
        p.height = 0.52f + 0.16f * paint_mask + 0.06f * brush - 0.40f * seam_line;
        p.ao = saturate(1.0f - 0.60f * seam_line - 0.08f * (1.0f - micro));
        p.roughness = saturate(lerp3({0.32f + 0.14f * brush, 0.0f, 0.0f}, {0.22f + 0.10f * micro, 0.0f, 0.0f}, paint_mask).x);
        p.metallic = (1.0f - paint_mask) * 0.90f;
        p.clearcoat = paint_mask * 0.92f;
    } else if (slice == 4U) {
        const float gu = std::fmod(u * 4.0f, 1.0f);
        const float gv = std::fmod(v * 4.0f, 1.0f);
        const int gx = static_cast<int>(std::floor(u * 4.0f));
        const int gy = static_cast<int>(std::floor(v * 4.0f));
        const float seam = 1.0f - smoothstep_f(0.018f, 0.042f, std::min(std::min(gu, 1.0f - gu), std::min(gv, 1.0f - gv)));
        const bool copper_panel = ((gx + gy) & 1) != 0;
        const float brush = tileable_value_noise(u * 2.0f, v * 16.0f, 16, 607);
        const float micro = tileable_fbm(u, v, 8, 3, 619);
        const Vec3 titanium = lerp3(Vec3{0.58f, 0.66f, 0.74f}, Vec3{0.82f, 0.88f, 0.94f}, brush * 0.7f + micro * 0.3f);
        const Vec3 copper = lerp3(Vec3{0.72f, 0.36f, 0.18f}, Vec3{0.95f, 0.58f, 0.34f}, brush * 0.6f + micro * 0.4f);
        p.albedo_linear = lerp3(copper_panel ? copper : titanium, Vec3{0.05f, 0.06f, 0.07f}, seam);
        p.height = (0.62f + 0.08f * brush) * (1.0f - 0.85f * seam);
        p.ao = saturate(1.0f - 0.70f * seam);
        p.roughness = saturate((copper_panel ? 0.17f : 0.24f) + 0.12f * (1.0f - brush) + 0.40f * seam);
        p.metallic = 1.0f - 0.85f * seam;
        p.clearcoat = (1.0f - seam) * (copper_panel ? 0.55f : 0.25f);
    } else if (slice == 5U) {
        const float brush = tileable_value_noise(u * 16.0f, v * 2.0f, 16, 709);
        const float patina = tileable_fbm(u, v, 8, 3, 727);
        p.albedo_linear = lerp3(Vec3{0.80f, 0.58f, 0.22f}, Vec3{0.98f, 0.80f, 0.40f}, brush * 0.65f + patina * 0.35f);
        p.height = 0.50f + 0.08f * brush;
        p.ao = saturate(0.88f + 0.12f * patina);
        p.roughness = saturate(0.20f + 0.14f * (1.0f - brush) + 0.08f * patina);
        p.metallic = 1.0f;
        p.clearcoat = 0.15f;
    } else if (slice == 6U) {
        const float slab_u = std::fmod(u * 2.0f, 1.0f);
        const float slab_v = std::fmod(v * 2.0f, 1.0f);
        const float joint = 1.0f - smoothstep_f(0.005f, 0.015f, std::min(std::min(slab_u, 1.0f - slab_u), std::min(slab_v, 1.0f - slab_v)));
        const float grit = tileable_fbm(u, v, 32, 4, 811);
        const float macro = tileable_fbm(u, v, 4, 4, 823);
        const float trowel = tileable_fbm(u, v, 6, 3, 839);
        const float warp_x = (tileable_fbm(u, v, 8, 3, 853) - 0.5f) * 0.085f;
        const float warp_y = (tileable_fbm(u + 0.37f, v + 0.19f, 8, 3, 859) - 0.5f) * 0.085f;
        const float crack_edge = tileable_voronoi_edge(u + warp_x, v + warp_y, 3, 863);
        const float crack_zone = smoothstep_f(0.36f, 0.66f, tileable_fbm(u, v, 2, 3, 877));
        const float crack_fine = 1.0f - smoothstep_f(0.008f, 0.026f, crack_edge);
        const float crack_hair = 1.0f - smoothstep_f(0.004f, 0.014f, tileable_voronoi_edge(u + warp_y, v - warp_x, 6, 881));
        const float crack = saturate(crack_fine * (0.45f + 0.55f * crack_zone) + crack_hair * crack_zone * 0.55f);

        const float polish_mask = smoothstep_f(0.42f, 0.72f, trowel * 0.65f + macro * 0.35f) * (1.0f - crack) * (1.0f - joint);
        const float oil_stain = smoothstep_f(0.68f, 0.84f, tileable_fbm(u + 0.13f, v + 0.41f, 5, 4, 887));
        const float scuff = smoothstep_f(0.72f, 0.88f, tileable_value_noise(u * 4.0f, v * 16.0f, 24, 907));
        const float pit = smoothstep_f(0.79f, 0.90f, tileable_fbm(u, v, 48, 3, 911));

        Vec3 concrete = lerp3(Vec3{0.115f, 0.118f, 0.122f}, Vec3{0.235f, 0.230f, 0.218f}, grit * 0.45f + macro * 0.55f);
        concrete = lerp3(concrete, concrete * 1.12f, polish_mask * 0.35f);
        concrete = lerp3(concrete, Vec3{0.048f, 0.044f, 0.036f}, oil_stain * 0.62f);
        concrete = lerp3(concrete, Vec3{0.065f, 0.064f, 0.062f}, scuff * 0.42f);
        concrete = lerp3(concrete, Vec3{0.024f, 0.024f, 0.025f}, saturate(joint + crack * 0.92f));

        p.albedo_linear = concrete;
        p.height = (0.62f + 0.14f * grit * (1.0f - 0.6f * polish_mask) + 0.06f * macro - 0.18f * pit)
            * (1.0f - 0.86f * joint) * (1.0f - 0.78f * crack);
        p.ao = saturate(0.94f - 0.62f * joint - 0.68f * crack - 0.24f * pit - 0.14f * oil_stain);
        float base_r = 0.68f + 0.16f * grit - 0.46f * polish_mask - 0.22f * oil_stain + 0.14f * scuff;
        p.roughness = saturate(lerp3({base_r, 0.0f, 0.0f}, {0.94f, 0.0f, 0.0f}, saturate(joint + crack)).x);
        p.metallic = 0.0f;
        p.clearcoat = saturate(polish_mask * 0.52f + oil_stain * 0.35f) * (1.0f - saturate(joint + crack));
    } else if (slice == 7U) {
        const float row = std::floor(v * 8.0f);
        const float bu = std::fmod(u * 4.0f + ((static_cast<int>(row) & 1) ? 0.5f : 0.0f), 1.0f);
        const float bv = std::fmod(v * 8.0f, 1.0f);
        const float mortar_u = 1.0f - smoothstep_f(0.015f, 0.036f, std::min(bu, 1.0f - bu));
        const float mortar_v = 1.0f - smoothstep_f(0.030f, 0.068f, std::min(bv, 1.0f - bv));
        const float mortar = saturate(std::max(mortar_u, mortar_v));
        const float plaster = tileable_fbm(u, v, 24, 4, 907);
        const float grime = smoothstep_f(0.54f, 0.82f, tileable_fbm(u, v, 5, 3, 929));
        const float tint_var = hash2d(static_cast<int>(std::floor(u * 4.0f + ((static_cast<int>(row) & 1) ? 0.5f : 0.0f))) % 4, static_cast<int>(row) % 8, 919);
        const Vec3 block_a{0.185f, 0.182f, 0.175f};
        const Vec3 block_b{0.275f, 0.268f, 0.252f};
        Vec3 wall = lerp3(block_a, block_b, tint_var * 0.45f + plaster * 0.55f);
        wall = wall * (1.0f - 0.24f * grime);
        wall = lerp3(wall, Vec3{0.095f, 0.092f, 0.088f}, mortar);
        p.albedo_linear = wall;
        p.height = (0.64f + 0.18f * plaster) * (1.0f - 0.80f * mortar);
        p.ao = saturate(0.95f - 0.64f * mortar - 0.14f * grime - 0.10f * (1.0f - plaster));
        p.roughness = saturate(0.76f + 0.14f * plaster + 0.10f * mortar);
        p.metallic = 0.0f;
        p.clearcoat = 0.0f;
    } else if (slice == 8U) {
        const float edge_u = std::min(u, 1.0f - u);
        const float edge_v = std::min(v, 1.0f - v);
        const float border = 1.0f - smoothstep_f(0.068f, 0.085f, std::min(edge_u, edge_v));
        const float corner_brace = (edge_u < 0.105f && edge_v < 0.105f) ? 1.0f : 0.0f;
        const float bolt_d = std::sqrt((edge_u - 0.055f) * (edge_u - 0.055f) + (edge_v - 0.055f) * (edge_v - 0.055f));
        const float bolt = (1.0f - smoothstep_f(0.012f, 0.020f, bolt_d)) * corner_brace;
        const float plank_coord = v * 5.0f;
        const int plank_id = static_cast<int>(std::floor(plank_coord)) % 5;
        const float pv_local = std::fmod(plank_coord, 1.0f);
        const float plank_gap = (1.0f - smoothstep_f(0.008f, 0.024f, std::min(pv_local, 1.0f - pv_local))) * (1.0f - border);
        const float plank_shift = hash2d(plank_id, 7, 1013);
        const float fine_fiber = tileable_fbm(u * 2.0f + plank_shift, v * 16.0f, 16, 4, 1009);
        const float grain_warp = tileable_fbm(u + plank_shift, v * 3.0f, 8, 4, 1019);
        const float kx = u - (0.28f + 0.44f * hash2d(plank_id, 19, 1021));
        const float ky = (pv_local - 0.5f) * 0.22f;
        const float knot_d = std::sqrt(kx * kx * 1.4f + ky * ky * 18.0f);
        const float knot = 1.0f - smoothstep_f(0.025f, 0.095f, knot_d);
        const float ring_phase = (v * 26.0f + grain_warp * 6.5f + knot * 3.2f) * k_pi;
        const float rings = 0.5f + 0.5f * std::sin(ring_phase);
        const float wear_scratch = smoothstep_f(0.80f, 0.93f, tileable_value_noise(u * 12.0f, v * 3.0f, 24, 1033));
        const float plank_tint = 0.86f + 0.28f * hash2d(plank_id, 3, 1039);

        Vec3 wood = lerp3(Vec3{0.16f, 0.082f, 0.032f}, Vec3{0.38f, 0.22f, 0.098f}, rings * 0.48f + fine_fiber * 0.52f) * plank_tint;
        wood = lerp3(wood, Vec3{0.075f, 0.038f, 0.016f}, knot * 0.72f);
        wood = lerp3(wood, Vec3{0.46f, 0.32f, 0.18f}, wear_scratch * 0.35f);
        const Vec3 frame_wood = wood * 0.74f;
        const Vec3 metal_brace = lerp3(Vec3{0.12f, 0.135f, 0.155f}, Vec3{0.26f, 0.28f, 0.31f}, fine_fiber);

        Vec3 col = lerp3(wood, frame_wood, border) * (1.0f - 0.62f * plank_gap);
        col = lerp3(col, metal_brace, corner_brace);
        col = lerp3(col, Vec3{0.55f, 0.58f, 0.62f}, bolt * 0.65f);

        p.albedo_linear = col;
        p.height = (0.46f + 0.11f * rings + 0.05f * fine_fiber + 0.16f * border - 0.08f * knot) * (1.0f - 0.75f * plank_gap) + 0.18f * corner_brace + 0.14f * bolt;
        p.ao = saturate(0.93f - 0.64f * plank_gap - 0.20f * knot - 0.14f * (1.0f - fine_fiber));
        const float wood_r = saturate(0.42f + 0.22f * (1.0f - rings) + 0.14f * wear_scratch + 0.30f * plank_gap);
        const float metal_r = saturate(0.24f + 0.16f * (1.0f - fine_fiber));
        p.roughness = saturate(lerp3({wood_r, 0.0f, 0.0f}, {metal_r, 0.0f, 0.0f}, corner_brace).x);
        p.metallic = corner_brace * 0.95f;
        p.clearcoat = (1.0f - corner_brace) * (1.0f - plank_gap) * (1.0f - wear_scratch) * 0.34f;
    } else if (slice == 9U) {
        const float dx = (u - 0.5f) * 2.0f;
        const float dy = (v - 0.52f) * 2.0f;
        const float r = std::sqrt(dx * dx + dy * dy);
        const float ring1 = 1.0f - smoothstep_f(0.14f, 0.17f, r);
        const float ring2 = smoothstep_f(0.32f, 0.35f, r) * (1.0f - smoothstep_f(0.46f, 0.49f, r));
        const float ring3 = smoothstep_f(0.64f, 0.67f, r) * (1.0f - smoothstep_f(0.76f, 0.79f, r));
        const float edge_wear = 1.0f - smoothstep_f(0.03f, 0.09f, std::min(std::min(u, 1.0f - u), std::min(v, 1.0f - v)));
        const float scuffs = tileable_fbm(u, v, 24, 4, 1103);
        const float bare_steel = saturate(edge_wear * 0.85f + smoothstep_f(0.72f, 0.84f, scuffs) * 0.65f);
        const Vec3 painted_cream{0.76f, 0.72f, 0.64f};
        const Vec3 target_red{0.68f, 0.06f, 0.04f};
        const Vec3 target_dark{0.08f, 0.09f, 0.10f};
        const Vec3 ar500_steel{0.58f, 0.60f, 0.63f};
        Vec3 paint = lerp3(painted_cream, target_dark, ring3);
        paint = lerp3(paint, target_red, saturate(ring1 + ring2));
        p.albedo_linear = lerp3(paint, ar500_steel, bare_steel);
        p.height = 0.58f - 0.14f * bare_steel + 0.08f * scuffs;
        p.ao = saturate(0.90f + 0.10f * scuffs - 0.12f * edge_wear);
        p.roughness = saturate(lerp3({0.48f + 0.12f * scuffs, 0.0f, 0.0f}, {0.22f + 0.10f * scuffs, 0.0f, 0.0f}, bare_steel).x);
        p.metallic = saturate(0.04f * (1.0f - bare_steel) + 0.96f * bare_steel);
        p.clearcoat = 0.08f * (1.0f - bare_steel);
    } else if (slice == 10U) {
        const float mill = tileable_value_noise(u * 2.0f, v * 32.0f, 32, 1213) * 0.58f
            + tileable_value_noise(u * 4.0f, v * 64.0f, 64, 1229) * 0.42f;
        const float oil = tileable_fbm(u, v, 8, 4, 1237);
        const float impact_scuff = smoothstep_f(0.80f, 0.92f, tileable_fbm(u, v, 20, 3, 1249));
        Vec3 steel = lerp3(Vec3{0.10f, 0.115f, 0.135f}, Vec3{0.24f, 0.27f, 0.31f}, mill * 0.65f + oil * 0.35f);
        steel = lerp3(steel, Vec3{0.46f, 0.49f, 0.54f}, impact_scuff * 0.6f);
        p.albedo_linear = steel;
        p.height = 0.54f + 0.07f * mill - 0.10f * impact_scuff;
        p.ao = saturate(0.90f + 0.10f * oil - 0.16f * impact_scuff);
        p.roughness = saturate(0.18f + 0.18f * (1.0f - mill) + 0.08f * oil - 0.08f * impact_scuff);
        p.metallic = 0.96f;
        p.clearcoat = 0.20f * (1.0f - impact_scuff);
    } else {
        const float sx = std::fabs(std::fmod((u + v) * 20.0f, 1.0f) - 0.5f) * 2.0f;
        const float sy = std::fabs(std::fmod((u - v + 2.0f) * 20.0f, 1.0f) - 0.5f) * 2.0f;
        const float wedge = saturate(1.0f - std::max(sx, sy));
        const float micro = tileable_fbm(u, v, 24, 3, 1307);
        p.albedo_linear = lerp3(Vec3{0.028f, 0.032f, 0.038f}, Vec3{0.068f, 0.074f, 0.082f}, wedge * 0.65f + micro * 0.35f);
        p.height = 0.32f + 0.50f * wedge + 0.06f * micro;
        p.ao = saturate(0.55f + 0.45f * wedge);
        p.roughness = saturate(0.68f + 0.18f * (1.0f - wedge) + 0.08f * micro);
        p.metallic = 0.0f;
        p.clearcoat = 0.0f;
    }
    return p;
}

struct MipLevelRgba8 {
    UINT width;
    UINT height;
    std::vector<std::uint8_t> pixels;
};

void build_material_mip_chain(
    UINT slice,
    std::vector<MipLevelRgba8>& out_albedo_mips,
    std::vector<MipLevelRgba8>& out_normal_mips,
    std::vector<MipLevelRgba8>& out_orm_mips) {
    out_albedo_mips.resize(k_material_mips);
    out_normal_mips.resize(k_material_mips);
    out_orm_mips.resize(k_material_mips);

    std::vector<Vec3> lin_albedo(k_material_size * k_material_size);
    std::vector<float> height_map(k_material_size * k_material_size);
    std::vector<Vec3> normals(k_material_size * k_material_size);
    std::vector<float> ao_map(k_material_size * k_material_size);
    std::vector<float> rough_map(k_material_size * k_material_size);
    std::vector<float> metal_map(k_material_size * k_material_size);
    std::vector<float> coat_map(k_material_size * k_material_size);

    for (UINT y = 0; y < k_material_size; ++y) {
        const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(k_material_size);
        for (UINT x = 0; x < k_material_size; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(k_material_size);
            const MaterialPixel mp = evaluate_material_texel(slice, u, v);
            const std::size_t idx = static_cast<std::size_t>(y) * k_material_size + x;
            lin_albedo[idx] = mp.albedo_linear;
            height_map[idx] = mp.height;
            ao_map[idx] = mp.ao;
            rough_map[idx] = mp.roughness;
            metal_map[idx] = mp.metallic;
            coat_map[idx] = mp.clearcoat;
        }
    }

    const float normal_strength = (slice == 2U) ? 4.2f : (slice == 0U || slice == 1U) ? 3.2f : 2.6f;
    for (UINT y = 0; y < k_material_size; ++y) {
        const UINT ym = (y + k_material_size - 1U) % k_material_size;
        const UINT yp = (y + 1U) % k_material_size;
        for (UINT x = 0; x < k_material_size; ++x) {
            const UINT xm = (x + k_material_size - 1U) % k_material_size;
            const UINT xp = (x + 1U) % k_material_size;
            const float h_l = height_map[static_cast<std::size_t>(y) * k_material_size + xm];
            const float h_r = height_map[static_cast<std::size_t>(y) * k_material_size + xp];
            const float h_d = height_map[static_cast<std::size_t>(ym) * k_material_size + x];
            const float h_u = height_map[static_cast<std::size_t>(yp) * k_material_size + x];
            const float dh_dx = (h_r - h_l) * normal_strength;
            const float dh_dy = (h_u - h_d) * normal_strength;
            normals[static_cast<std::size_t>(y) * k_material_size + x] = normalize3({-dh_dx, -dh_dy, 1.0f});
        }
    }

    UINT w = k_material_size;
    UINT h = k_material_size;
    for (UINT mip = 0; mip < k_material_mips; ++mip) {
        out_albedo_mips[mip] = {w, h, std::vector<std::uint8_t>(static_cast<std::size_t>(w) * h * 4U)};
        out_normal_mips[mip] = {w, h, std::vector<std::uint8_t>(static_cast<std::size_t>(w) * h * 4U)};
        out_orm_mips[mip] = {w, h, std::vector<std::uint8_t>(static_cast<std::size_t>(w) * h * 4U)};

        for (UINT y = 0; y < h; ++y) {
            for (UINT x = 0; x < w; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * w + x;
                const std::size_t o = i * 4U;
                out_albedo_mips[mip].pixels[o + 0] = float_to_u8(linear_to_srgb(lin_albedo[i].x));
                out_albedo_mips[mip].pixels[o + 1] = float_to_u8(linear_to_srgb(lin_albedo[i].y));
                out_albedo_mips[mip].pixels[o + 2] = float_to_u8(linear_to_srgb(lin_albedo[i].z));
                out_albedo_mips[mip].pixels[o + 3] = 255U;

                const Vec3 n = normalize3(normals[i]);
                out_normal_mips[mip].pixels[o + 0] = float_to_u8(n.x * 0.5f + 0.5f);
                out_normal_mips[mip].pixels[o + 1] = float_to_u8(n.y * 0.5f + 0.5f);
                out_normal_mips[mip].pixels[o + 2] = float_to_u8(n.z * 0.5f + 0.5f);
                out_normal_mips[mip].pixels[o + 3] = 255U;

                out_orm_mips[mip].pixels[o + 0] = float_to_u8(ao_map[i]);
                out_orm_mips[mip].pixels[o + 1] = float_to_u8(rough_map[i]);
                out_orm_mips[mip].pixels[o + 2] = float_to_u8(metal_map[i]);
                out_orm_mips[mip].pixels[o + 3] = float_to_u8(coat_map[i]);
            }
        }

        if (mip + 1U >= k_material_mips) break;
        const UINT nw = std::max<UINT>(1U, w / 2U);
        const UINT nh = std::max<UINT>(1U, h / 2U);
        std::vector<Vec3> next_albedo(static_cast<std::size_t>(nw) * nh);
        std::vector<Vec3> next_normals(static_cast<std::size_t>(nw) * nh);
        std::vector<float> next_ao(static_cast<std::size_t>(nw) * nh);
        std::vector<float> next_rough(static_cast<std::size_t>(nw) * nh);
        std::vector<float> next_metal(static_cast<std::size_t>(nw) * nh);
        std::vector<float> next_coat(static_cast<std::size_t>(nw) * nh);

        for (UINT y = 0; y < nh; ++y) {
            const UINT sy0 = std::min(y * 2U, h - 1U);
            const UINT sy1 = std::min(y * 2U + 1U, h - 1U);
            for (UINT x = 0; x < nw; ++x) {
                const UINT sx0 = std::min(x * 2U, w - 1U);
                const UINT sx1 = std::min(x * 2U + 1U, w - 1U);
                const std::size_t ids[4] = {
                    static_cast<std::size_t>(sy0) * w + sx0,
                    static_cast<std::size_t>(sy0) * w + sx1,
                    static_cast<std::size_t>(sy1) * w + sx0,
                    static_cast<std::size_t>(sy1) * w + sx1,
                };
                Vec3 a_sum{0.0f, 0.0f, 0.0f};
                Vec3 n_sum{0.0f, 0.0f, 0.0f};
                float ao_sum = 0.0f;
                float alpha2_sum = 0.0f;
                float m_sum = 0.0f;
                float c_sum = 0.0f;
                for (std::size_t k : ids) {
                    a_sum = a_sum + lin_albedo[k];
                    n_sum = n_sum + normals[k];
                    ao_sum += ao_map[k];
                    const float a = rough_map[k] * rough_map[k];
                    alpha2_sum += a * a;
                    m_sum += metal_map[k];
                    c_sum += coat_map[k];
                }
                const Vec3 n_avg = n_sum * 0.25f;
                const float n_len = std::clamp(length3(n_avg), 0.05f, 1.0f);
                const float toksvig_var = std::clamp((1.0f - n_len) / std::max(n_len, 0.05f), 0.0f, 0.35f);
                const float alpha2_filtered = std::clamp(alpha2_sum * 0.25f + toksvig_var * 0.5f, 0.0004f, 1.0f);
                const float perceptual_r = std::sqrt(std::sqrt(alpha2_filtered));

                const std::size_t dst = static_cast<std::size_t>(y) * nw + x;
                next_albedo[dst] = a_sum * 0.25f;
                next_normals[dst] = normalize3(n_avg);
                next_ao[dst] = ao_sum * 0.25f;
                next_rough[dst] = saturate(perceptual_r);
                next_metal[dst] = m_sum * 0.25f;
                next_coat[dst] = c_sum * 0.25f;
            }
        }
        lin_albedo = std::move(next_albedo);
        normals = std::move(next_normals);
        ao_map = std::move(next_ao);
        rough_map = std::move(next_rough);
        metal_map = std::move(next_metal);
        coat_map = std::move(next_coat);
        w = nw;
        h = nh;
    }
}

float radical_inverse_vdc(std::uint32_t bits) {
    bits = (bits << 16U) | (bits >> 16U);
    bits = ((bits & 0x55555555U) << 1U) | ((bits & 0xAAAAAAAAU) >> 1U);
    bits = ((bits & 0x33333333U) << 2U) | ((bits & 0xCCCCCCCCU) >> 2U);
    bits = ((bits & 0x0F0F0F0FU) << 4U) | ((bits & 0xF0F0F0F0U) >> 4U);
    bits = ((bits & 0x00FF00FFU) << 8U) | ((bits & 0xFF00FF00U) >> 8U);
    return static_cast<float>(bits) * 2.3283064365386963e-10f;
}

Vec3 importance_sample_ggx(float u1, float u2, float alpha, Vec3 n) {
    const float phi = 2.0f * k_pi * u1;
    const float cos_theta = std::sqrt(std::max(0.0f, (1.0f - u2) / (1.0f + (alpha * alpha - 1.0f) * u2)));
    const float sin_theta = std::sqrt(std::max(0.0f, 1.0f - cos_theta * cos_theta));
    const Vec3 h_tan{std::cos(phi) * sin_theta, std::sin(phi) * sin_theta, cos_theta};
    const Vec3 up = std::fabs(n.z) < 0.999f ? Vec3{0.0f, 0.0f, 1.0f} : Vec3{1.0f, 0.0f, 0.0f};
    const Vec3 tangent = normalize3(cross3(up, n));
    const Vec3 bitangent = cross3(n, tangent);
    return normalize3(tangent * h_tan.x + bitangent * h_tan.y + n * h_tan.z);
}

float smith_ggx_correlated_v(float nov, float nol, float alpha) {
    const float a2 = alpha * alpha;
    const float ggxv = nol * std::sqrt(std::max(1e-6f, nov * nov * (1.0f - a2) + a2));
    const float ggxl = nov * std::sqrt(std::max(1e-6f, nol * nol * (1.0f - a2) + a2));
    return 0.5f / std::max(ggxv + ggxl, 1e-5f);
}

std::vector<std::uint16_t> build_dfg_lut_fp16() {
    std::vector<std::uint16_t> lut(static_cast<std::size_t>(k_dfg_size) * k_dfg_size * 2U);
    constexpr UINT sample_count = 64U;
    for (UINT y = 0; y < k_dfg_size; ++y) {
        const float perceptual_roughness = (static_cast<float>(y) + 0.5f) / static_cast<float>(k_dfg_size);
        const float alpha = std::max(0.002f, perceptual_roughness * perceptual_roughness);
        for (UINT x = 0; x < k_dfg_size; ++x) {
            const float nov = std::max(0.002f, (static_cast<float>(x) + 0.5f) / static_cast<float>(k_dfg_size));
            const Vec3 v{std::sqrt(std::max(0.0f, 1.0f - nov * nov)), 0.0f, nov};
            const Vec3 n{0.0f, 0.0f, 1.0f};
            float scale = 0.0f;
            float bias = 0.0f;
            for (UINT i = 0; i < sample_count; ++i) {
                const float u1 = static_cast<float>(i) / static_cast<float>(sample_count);
                const float u2 = radical_inverse_vdc(i);
                const Vec3 h = importance_sample_ggx(u1, u2, alpha, n);
                const float voh = std::max(dot3(v, h), 0.0f);
                const Vec3 l = normalize3(h * (2.0f * voh) - v);
                const float nol = std::max(l.z, 0.0f);
                const float noh = std::max(h.z, 0.0f);
                if (nol > 0.0f) {
                    const float vis = smith_ggx_correlated_v(nov, nol, alpha);
                    const float g_vis = 4.0f * vis * nol * voh / std::max(noh, 1e-4f);
                    const float fc = std::pow(1.0f - voh, 5.0f);
                    scale += (1.0f - fc) * g_vis;
                    bias += fc * g_vis;
                }
            }
            scale /= static_cast<float>(sample_count);
            bias /= static_cast<float>(sample_count);
            const std::size_t dst = (static_cast<std::size_t>(y) * k_dfg_size + x) * 2U;
            lut[dst + 0] = float_to_half(saturate(scale));
            lut[dst + 1] = float_to_half(saturate(bias));
        }
    }
    return lut;
}

Vec3 evaluate_hdr_environment(Vec3 d) {
    d = normalize3(d);
    const Vec3 sun_dir = normalize3({-0.392232f, 0.784465f, -0.490290f});
    const float up_t = saturate(d.y * 0.5f + 0.5f);
    Vec3 sky = lerp3(Vec3{0.05f, 0.07f, 0.09f}, Vec3{0.18f, 0.26f, 0.36f}, smoothstep_f(0.45f, 0.95f, up_t));
    if (d.y < 0.0f) {
        sky = lerp3(Vec3{0.08f, 0.095f, 0.11f}, Vec3{0.03f, 0.035f, 0.045f}, saturate(-d.y * 1.4f));
    }
    const float sun_dot = std::max(0.0f, dot3(d, sun_dir));
    sky = sky + Vec3{6.5f, 5.6f, 4.4f} * std::pow(sun_dot, 28.0f);
    sky = sky + Vec3{1.1f, 0.92f, 0.72f} * std::pow(sun_dot, 4.0f) * 0.35f;

    const float strip_x = smoothstep_f(0.18f, 0.04f, std::fabs(d.x));
    const float strip_y = smoothstep_f(0.52f, 0.84f, d.y);
    sky = sky + Vec3{2.4f, 2.85f, 3.4f} * (strip_x * strip_y);

    const float side_rim = smoothstep_f(0.55f, 0.92f, std::fabs(d.x)) * (1.0f - std::fabs(d.y));
    const Vec3 side_col = d.x > 0.0f ? Vec3{0.28f, 0.42f, 0.54f} : Vec3{0.48f, 0.32f, 0.18f};
    sky = sky + side_col * (side_rim * 0.65f);
    return sky;
}

Vec3 cubemap_direction(UINT face, float u, float v) {
    const float a = u * 2.0f - 1.0f;
    const float b = 1.0f - v * 2.0f;
    switch (face) {
        case 0U: return normalize3({1.0f, b, -a});
        case 1U: return normalize3({-1.0f, b, a});
        case 2U: return normalize3({a, 1.0f, -b});
        case 3U: return normalize3({a, -1.0f, b});
        case 4U: return normalize3({a, b, 1.0f});
        default: return normalize3({-a, b, -1.0f});
    }
}

struct MipLevelFp16 {
    UINT width;
    UINT height;
    std::vector<std::uint16_t> pixels;
};

std::vector<MipLevelFp16> build_env_cubemap_fp16() {
    std::vector<MipLevelFp16> subresources(static_cast<std::size_t>(6U) * k_env_mips);
    for (UINT face = 0; face < 6U; ++face) {
        UINT dim = k_env_size;
        for (UINT mip = 0; mip < k_env_mips; ++mip) {
            const std::size_t sub_idx = static_cast<std::size_t>(face) * k_env_mips + mip;
            MipLevelFp16 level{dim, dim, std::vector<std::uint16_t>(static_cast<std::size_t>(dim) * dim * 4U)};
            const float perceptual_r = static_cast<float>(mip) / static_cast<float>(k_env_mips - 1U);
            const float alpha = std::max(0.002f, perceptual_r * perceptual_r);
            const bool diffuse_mip = (mip == k_env_mips - 1U);
            const UINT samples = (mip == 0U) ? 1U : (diffuse_mip ? 64U : 32U);

            for (UINT y = 0; y < dim; ++y) {
                const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(dim);
                for (UINT x = 0; x < dim; ++x) {
                    const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(dim);
                    const Vec3 n = cubemap_direction(face, u, v);
                    Vec3 accum{0.0f, 0.0f, 0.0f};
                    float weight_sum = 0.0f;
                    if (mip == 0U) {
                        accum = evaluate_hdr_environment(n);
                        weight_sum = 1.0f;
                    } else if (diffuse_mip) {
                        const Vec3 up = std::fabs(n.z) < 0.999f ? Vec3{0.0f, 0.0f, 1.0f} : Vec3{1.0f, 0.0f, 0.0f};
                        const Vec3 tangent = normalize3(cross3(up, n));
                        const Vec3 bitangent = cross3(n, tangent);
                        for (UINT i = 0; i < samples; ++i) {
                            const float u1 = (static_cast<float>(i) + 0.5f) / static_cast<float>(samples);
                            const float u2 = radical_inverse_vdc(i);
                            const float phi = 2.0f * k_pi * u1;
                            const float cos_t = std::sqrt(1.0f - u2);
                            const float sin_t = std::sqrt(u2);
                            const Vec3 l = normalize3(tangent * (std::cos(phi) * sin_t) + bitangent * (std::sin(phi) * sin_t) + n * cos_t);
                            accum = accum + evaluate_hdr_environment(l);
                            weight_sum += 1.0f;
                        }
                    } else {
                        for (UINT i = 0; i < samples; ++i) {
                            const float u1 = (static_cast<float>(i) + 0.5f) / static_cast<float>(samples);
                            const float u2 = radical_inverse_vdc(i);
                            const Vec3 h = importance_sample_ggx(u1, u2, alpha, n);
                            const Vec3 l = normalize3(h * (2.0f * dot3(n, h)) - n);
                            const float nol = std::max(0.0f, dot3(n, l));
                            if (nol > 0.0f) {
                                accum = accum + evaluate_hdr_environment(l) * nol;
                                weight_sum += nol;
                            }
                        }
                    }
                    const Vec3 final_rad = weight_sum > 1e-5f ? accum * (1.0f / weight_sum) : Vec3{0.0f, 0.0f, 0.0f};
                    const std::size_t dst = (static_cast<std::size_t>(y) * dim + x) * 4U;
                    level.pixels[dst + 0] = float_to_half(final_rad.x);
                    level.pixels[dst + 1] = float_to_half(final_rad.y);
                    level.pixels[dst + 2] = float_to_half(final_rad.z);
                    level.pixels[dst + 3] = float_to_half(1.0f);
                }
            }
            subresources[sub_idx] = std::move(level);
            dim = std::max<UINT>(1U, dim / 2U);
        }
    }
    return subresources;
}

ComPtr<ID3D12Resource> create_default_texture(
    ID3D12Device* device,
    UINT width,
    UINT height,
    UINT16 array_size,
    UINT16 mip_levels,
    DXGI_FORMAT format,
    const char* name) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = array_size;
    desc.MipLevels = mip_levels;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> tex;
    check_hr(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex)), name);
    return tex;
}

struct UploadTask {
    ID3D12Resource* destination;
    UINT subresource_count;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
    std::vector<UINT> num_rows;
    std::vector<UINT64> row_sizes;
    std::vector<const void*> src_data;
    std::vector<std::size_t> src_row_pitch;
    UINT64 staging_offset;
};

struct LoadedHeroPbrTextures {
    UINT width = 256;
    UINT height = 256;
    UINT mip_count = 9;
    bool loaded_from_disk = false;
    std::vector<MipLevelRgba8> albedo_mips;
    std::vector<MipLevelRgba8> normal_mips;
    std::vector<MipLevelRgba8> orm_mips;
};

UINT compute_mip_levels(UINT width, UINT height) {
    UINT mips = 1U;
    UINT dim = std::max(width, height);
    while (dim > 1U) {
        dim >>= 1U;
        ++mips;
    }
    return mips;
}

LoadedHeroPbrTextures build_hero_pbr_textures() {
    LoadedHeroPbrTextures result{};
    const auto diff_path = resolve_asset_path("assets/models/service_pistol/textures/service_pistol_diff_1k.jpg");
    const auto norm_path = resolve_asset_path("assets/models/service_pistol/textures/service_pistol_nor_gl_1k.jpg");
    const auto arm_path = resolve_asset_path("assets/models/service_pistol/textures/service_pistol_arm_1k.jpg");

    int dw = 0, dh = 0, dc = 0;
    int nw = 0, nh = 0, nc = 0;
    int aw = 0, ah = 0, ac = 0;
    stbi_uc* diff_img = stbi_load(diff_path.string().c_str(), &dw, &dh, &dc, 4);
    stbi_uc* norm_img = stbi_load(norm_path.string().c_str(), &nw, &nh, &nc, 4);
    stbi_uc* arm_img = stbi_load(arm_path.string().c_str(), &aw, &ah, &ac, 4);

    if (diff_img == nullptr || norm_img == nullptr || arm_img == nullptr || dw <= 0 || dh <= 0 || dw != nw || dh != nh || dw != aw || dh != ah) {
        if (diff_img != nullptr) stbi_image_free(diff_img);
        if (norm_img != nullptr) stbi_image_free(norm_img);
        if (arm_img != nullptr) stbi_image_free(arm_img);
        result.width = k_material_size;
        result.height = k_material_size;
        result.mip_count = k_material_mips;
        build_material_mip_chain(10U, result.albedo_mips, result.normal_mips, result.orm_mips);
        return result;
    }

    result.loaded_from_disk = true;
    result.width = static_cast<UINT>(dw);
    result.height = static_cast<UINT>(dh);
    result.mip_count = compute_mip_levels(result.width, result.height);

    const std::size_t pixel_count = static_cast<std::size_t>(result.width) * result.height;
    std::vector<Vec3> lin_albedo(pixel_count);
    std::vector<Vec3> normals(pixel_count);
    std::vector<float> ao_map(pixel_count);
    std::vector<float> rough_map(pixel_count);
    std::vector<float> metal_map(pixel_count);

    for (std::size_t i = 0; i < pixel_count; ++i) {
        const std::size_t o = i * 4U;
        lin_albedo[i] = {
            srgb_to_linear(static_cast<float>(diff_img[o + 0]) / 255.0f),
            srgb_to_linear(static_cast<float>(diff_img[o + 1]) / 255.0f),
            srgb_to_linear(static_cast<float>(diff_img[o + 2]) / 255.0f),
        };
        const float nx = (static_cast<float>(norm_img[o + 0]) / 255.0f) * 2.0f - 1.0f;
        const float ny = -((static_cast<float>(norm_img[o + 1]) / 255.0f) * 2.0f - 1.0f);
        const float nz = std::max(0.05f, (static_cast<float>(norm_img[o + 2]) / 255.0f) * 2.0f - 1.0f);
        normals[i] = normalize3({nx, ny, nz});

        ao_map[i] = static_cast<float>(arm_img[o + 0]) / 255.0f;
        rough_map[i] = std::clamp(static_cast<float>(arm_img[o + 1]) / 255.0f, 0.08f, 1.0f);
        metal_map[i] = static_cast<float>(arm_img[o + 2]) / 255.0f;
    }

    stbi_image_free(diff_img);
    stbi_image_free(norm_img);
    stbi_image_free(arm_img);

    result.albedo_mips.resize(result.mip_count);
    result.normal_mips.resize(result.mip_count);
    result.orm_mips.resize(result.mip_count);

    UINT w = result.width;
    UINT h = result.height;
    for (UINT mip = 0; mip < result.mip_count; ++mip) {
        result.albedo_mips[mip] = {w, h, std::vector<std::uint8_t>(static_cast<std::size_t>(w) * h * 4U)};
        result.normal_mips[mip] = {w, h, std::vector<std::uint8_t>(static_cast<std::size_t>(w) * h * 4U)};
        result.orm_mips[mip] = {w, h, std::vector<std::uint8_t>(static_cast<std::size_t>(w) * h * 4U)};
        for (UINT y = 0; y < h; ++y) {
            for (UINT x = 0; x < w; ++x) {
                const std::size_t i = static_cast<std::size_t>(y) * w + x;
                const std::size_t o = i * 4U;
                result.albedo_mips[mip].pixels[o + 0] = float_to_u8(linear_to_srgb(lin_albedo[i].x));
                result.albedo_mips[mip].pixels[o + 1] = float_to_u8(linear_to_srgb(lin_albedo[i].y));
                result.albedo_mips[mip].pixels[o + 2] = float_to_u8(linear_to_srgb(lin_albedo[i].z));
                result.albedo_mips[mip].pixels[o + 3] = 255U;

                const Vec3 n = normalize3(normals[i]);
                result.normal_mips[mip].pixels[o + 0] = float_to_u8(n.x * 0.5f + 0.5f);
                result.normal_mips[mip].pixels[o + 1] = float_to_u8(n.y * 0.5f + 0.5f);
                result.normal_mips[mip].pixels[o + 2] = float_to_u8(n.z * 0.5f + 0.5f);
                result.normal_mips[mip].pixels[o + 3] = 255U;

                result.orm_mips[mip].pixels[o + 0] = float_to_u8(ao_map[i]);
                result.orm_mips[mip].pixels[o + 1] = float_to_u8(rough_map[i]);
                result.orm_mips[mip].pixels[o + 2] = float_to_u8(metal_map[i]);
                result.orm_mips[mip].pixels[o + 3] = 0U;
            }
        }
        if (mip + 1U >= result.mip_count) {
            break;
        }
        const UINT next_w = std::max<UINT>(1U, w / 2U);
        const UINT next_h = std::max<UINT>(1U, h / 2U);
        std::vector<Vec3> next_albedo(static_cast<std::size_t>(next_w) * next_h);
        std::vector<Vec3> next_normals(static_cast<std::size_t>(next_w) * next_h);
        std::vector<float> next_ao(static_cast<std::size_t>(next_w) * next_h);
        std::vector<float> next_rough(static_cast<std::size_t>(next_w) * next_h);
        std::vector<float> next_metal(static_cast<std::size_t>(next_w) * next_h);
        for (UINT y = 0; y < next_h; ++y) {
            const UINT sy0 = std::min(y * 2U, h - 1U);
            const UINT sy1 = std::min(y * 2U + 1U, h - 1U);
            for (UINT x = 0; x < next_w; ++x) {
                const UINT sx0 = std::min(x * 2U, w - 1U);
                const UINT sx1 = std::min(x * 2U + 1U, w - 1U);
                const std::size_t ids[4] = {
                    static_cast<std::size_t>(sy0) * w + sx0,
                    static_cast<std::size_t>(sy0) * w + sx1,
                    static_cast<std::size_t>(sy1) * w + sx0,
                    static_cast<std::size_t>(sy1) * w + sx1,
                };
                Vec3 a_sum{0.0f, 0.0f, 0.0f};
                Vec3 n_sum{0.0f, 0.0f, 0.0f};
                float ao_sum = 0.0f;
                float alpha2_sum = 0.0f;
                float m_sum = 0.0f;
                for (std::size_t k : ids) {
                    a_sum = a_sum + lin_albedo[k];
                    n_sum = n_sum + normals[k];
                    ao_sum += ao_map[k];
                    const float a = rough_map[k] * rough_map[k];
                    alpha2_sum += a * a;
                    m_sum += metal_map[k];
                }
                const Vec3 n_avg = n_sum * 0.25f;
                const float n_len = std::clamp(length3(n_avg), 0.05f, 1.0f);
                const float toksvig_var = std::clamp((1.0f - n_len) / std::max(n_len, 0.05f), 0.0f, 0.35f);
                const float alpha2_filtered = std::clamp(alpha2_sum * 0.25f + toksvig_var * 0.5f, 0.0004f, 1.0f);
                const std::size_t dst = static_cast<std::size_t>(y) * next_w + x;
                next_albedo[dst] = a_sum * 0.25f;
                next_normals[dst] = normalize3(n_avg);
                next_ao[dst] = ao_sum * 0.25f;
                next_rough[dst] = saturate(std::sqrt(std::sqrt(alpha2_filtered)));
                next_metal[dst] = m_sum * 0.25f;
            }
        }
        lin_albedo = std::move(next_albedo);
        normals = std::move(next_normals);
        ao_map = std::move(next_ao);
        rough_map = std::move(next_rough);
        metal_map = std::move(next_metal);
        w = next_w;
        h = next_h;
    }

    return result;
}

}

PbrAssetBundle create_and_upload_pbr_assets(
    ID3D12Device* device,
    ID3D12CommandQueue* upload_queue,
    ID3D12CommandAllocator* command_allocator,
    ID3D12GraphicsCommandList* command_list,
    ID3D12Fence* fence,
    HANDLE fence_event,
    std::uint64_t& next_fence_value,
    D3D12_CPU_DESCRIPTOR_HANDLE srv_heap_start,
    UINT srv_descriptor_stride) {
    const auto t_build_start = std::chrono::steady_clock::now();
    PbrAssetBundle bundle{};
    LoadedHeroPbrTextures hero_tex{};
    std::array<std::vector<MipLevelRgba8>, k_material_count> albedo_data;
    std::array<std::vector<MipLevelRgba8>, k_material_count> normal_data;
    std::array<std::vector<MipLevelRgba8>, k_material_count> orm_data;
    std::vector<std::uint16_t> dfg_data;
    std::vector<MipLevelFp16> env_data;

    EngineJobSystem::global().parallel_for(static_cast<std::size_t>(k_material_count) + 3U, 1U, [&](std::size_t begin, std::size_t end) {
        for (std::size_t job = begin; job < end; ++job) {
            if (job < k_material_count) {
                const auto slice = static_cast<UINT>(job);
                build_material_mip_chain(slice, albedo_data[slice], normal_data[slice], orm_data[slice]);
            } else if (job == k_material_count) {
                hero_tex = build_hero_pbr_textures();
            } else if (job == k_material_count + 1U) {
                dfg_data = build_dfg_lut_fp16();
            } else {
                env_data = build_env_cubemap_fp16();
            }
        }
    });
    const auto t_build_end = std::chrono::steady_clock::now();
    bundle.build_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_build_end - t_build_start).count();
    bundle.hero_textures_loaded = hero_tex.loaded_from_disk;

    bundle.albedo_array = create_default_texture(device, k_material_size, k_material_size, k_material_count, k_material_mips, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, "Create PBR albedo array");
    bundle.normal_array = create_default_texture(device, k_material_size, k_material_size, k_material_count, k_material_mips, DXGI_FORMAT_R8G8B8A8_UNORM, "Create PBR normal array");
    bundle.orm_array = create_default_texture(device, k_material_size, k_material_size, k_material_count, k_material_mips, DXGI_FORMAT_R8G8B8A8_UNORM, "Create PBR ORM array");
    bundle.dfg_lut = create_default_texture(device, k_dfg_size, k_dfg_size, 1, 1, DXGI_FORMAT_R16G16_FLOAT, "Create PBR DFG LUT");
    bundle.env_cubemap = create_default_texture(device, k_env_size, k_env_size, 6, k_env_mips, DXGI_FORMAT_R16G16B16A16_FLOAT, "Create PBR env cubemap");
    bundle.hero_albedo = create_default_texture(device, hero_tex.width, hero_tex.height, 1, static_cast<UINT16>(hero_tex.mip_count), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, "Create PBR hero albedo");
    bundle.hero_normal = create_default_texture(device, hero_tex.width, hero_tex.height, 1, static_cast<UINT16>(hero_tex.mip_count), DXGI_FORMAT_R8G8B8A8_UNORM, "Create PBR hero normal");
    bundle.hero_orm = create_default_texture(device, hero_tex.width, hero_tex.height, 1, static_cast<UINT16>(hero_tex.mip_count), DXGI_FORMAT_R8G8B8A8_UNORM, "Create PBR hero ORM");

    std::vector<UploadTask> tasks;
    UINT64 total_staging_bytes = 0;

    auto add_task = [&](ID3D12Resource* tex, UINT sub_count, auto fill_src) {
        UploadTask task{};
        task.destination = tex;
        task.subresource_count = sub_count;
        task.footprints.resize(sub_count);
        task.num_rows.resize(sub_count);
        task.row_sizes.resize(sub_count);
        task.src_data.resize(sub_count);
        task.src_row_pitch.resize(sub_count);
        const D3D12_RESOURCE_DESC desc = tex->GetDesc();
        UINT64 required_bytes = 0;
        total_staging_bytes = (total_staging_bytes + 511ULL) & ~511ULL;
        task.staging_offset = total_staging_bytes;
        device->GetCopyableFootprints(&desc, 0, sub_count, task.staging_offset, task.footprints.data(), task.num_rows.data(), task.row_sizes.data(), &required_bytes);
        total_staging_bytes += required_bytes;
        fill_src(task);
        tasks.push_back(std::move(task));
    };

    const UINT mat_sub_count = k_material_count * k_material_mips;
    add_task(bundle.albedo_array.Get(), mat_sub_count, [&](UploadTask& t) {
        for (UINT s = 0; s < k_material_count; ++s) {
            for (UINT m = 0; m < k_material_mips; ++m) {
                const UINT idx = s * k_material_mips + m;
                t.src_data[idx] = albedo_data[s][m].pixels.data();
                t.src_row_pitch[idx] = static_cast<std::size_t>(albedo_data[s][m].width) * 4U;
            }
        }
    });
    add_task(bundle.normal_array.Get(), mat_sub_count, [&](UploadTask& t) {
        for (UINT s = 0; s < k_material_count; ++s) {
            for (UINT m = 0; m < k_material_mips; ++m) {
                const UINT idx = s * k_material_mips + m;
                t.src_data[idx] = normal_data[s][m].pixels.data();
                t.src_row_pitch[idx] = static_cast<std::size_t>(normal_data[s][m].width) * 4U;
            }
        }
    });
    add_task(bundle.orm_array.Get(), mat_sub_count, [&](UploadTask& t) {
        for (UINT s = 0; s < k_material_count; ++s) {
            for (UINT m = 0; m < k_material_mips; ++m) {
                const UINT idx = s * k_material_mips + m;
                t.src_data[idx] = orm_data[s][m].pixels.data();
                t.src_row_pitch[idx] = static_cast<std::size_t>(orm_data[s][m].width) * 4U;
            }
        }
    });
    add_task(bundle.dfg_lut.Get(), 1U, [&](UploadTask& t) {
        t.src_data[0] = dfg_data.data();
        t.src_row_pitch[0] = static_cast<std::size_t>(k_dfg_size) * 4U;
    });
    add_task(bundle.env_cubemap.Get(), 6U * k_env_mips, [&](UploadTask& t) {
        for (UINT i = 0; i < 6U * k_env_mips; ++i) {
            t.src_data[i] = env_data[i].pixels.data();
            t.src_row_pitch[i] = static_cast<std::size_t>(env_data[i].width) * 8U;
        }
    });
    add_task(bundle.hero_albedo.Get(), hero_tex.mip_count, [&](UploadTask& t) {
        for (UINT m = 0; m < hero_tex.mip_count; ++m) {
            t.src_data[m] = hero_tex.albedo_mips[m].pixels.data();
            t.src_row_pitch[m] = static_cast<std::size_t>(hero_tex.albedo_mips[m].width) * 4U;
        }
    });
    add_task(bundle.hero_normal.Get(), hero_tex.mip_count, [&](UploadTask& t) {
        for (UINT m = 0; m < hero_tex.mip_count; ++m) {
            t.src_data[m] = hero_tex.normal_mips[m].pixels.data();
            t.src_row_pitch[m] = static_cast<std::size_t>(hero_tex.normal_mips[m].width) * 4U;
        }
    });
    add_task(bundle.hero_orm.Get(), hero_tex.mip_count, [&](UploadTask& t) {
        for (UINT m = 0; m < hero_tex.mip_count; ++m) {
            t.src_data[m] = hero_tex.orm_mips[m].pixels.data();
            t.src_row_pitch[m] = static_cast<std::size_t>(hero_tex.orm_mips[m].width) * 4U;
        }
    });

    D3D12_HEAP_PROPERTIES upload_heap{};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC upload_desc{};
    upload_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    upload_desc.Width = total_staging_bytes;
    upload_desc.Height = 1;
    upload_desc.DepthOrArraySize = 1;
    upload_desc.MipLevels = 1;
    upload_desc.SampleDesc.Count = 1;
    upload_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> staging;
    check_hr(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&staging)), "Create PBR staging buffer");

    std::uint8_t* mapped = nullptr;
    D3D12_RANGE empty_range{0, 0};
    check_hr(staging->Map(0, &empty_range, reinterpret_cast<void**>(&mapped)), "Map PBR staging buffer");
    for (const auto& task : tasks) {
        for (UINT sub = 0; sub < task.subresource_count; ++sub) {
            const auto& fp = task.footprints[sub];
            const auto* src_bytes = static_cast<const std::uint8_t*>(task.src_data[sub]);
            std::uint8_t* dst_bytes = mapped + fp.Offset;
            const std::size_t copy_bytes = static_cast<std::size_t>(task.row_sizes[sub]);
            for (UINT row = 0; row < task.num_rows[sub]; ++row) {
                std::memcpy(
                    dst_bytes + static_cast<std::size_t>(row) * fp.Footprint.RowPitch,
                    src_bytes + static_cast<std::size_t>(row) * task.src_row_pitch[sub],
                    copy_bytes);
            }
        }
    }
    staging->Unmap(0, nullptr);

    check_hr(command_allocator->Reset(), "Reset PBR upload allocator");
    check_hr(command_list->Reset(command_allocator, nullptr), "Reset PBR upload command list");
    for (const auto& task : tasks) {
        for (UINT sub = 0; sub < task.subresource_count; ++sub) {
            D3D12_TEXTURE_COPY_LOCATION dst_loc{};
            dst_loc.pResource = task.destination;
            dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst_loc.SubresourceIndex = sub;
            D3D12_TEXTURE_COPY_LOCATION src_loc{};
            src_loc.pResource = staging.Get();
            src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src_loc.PlacedFootprint = task.footprints[sub];
            command_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);
        }
    }

    constexpr D3D12_RESOURCE_STATES final_state =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    std::array<D3D12_RESOURCE_BARRIER, 8> barriers{};
    ID3D12Resource* all_res[8] = {
        bundle.albedo_array.Get(),
        bundle.normal_array.Get(),
        bundle.orm_array.Get(),
        bundle.dfg_lut.Get(),
        bundle.env_cubemap.Get(),
        bundle.hero_albedo.Get(),
        bundle.hero_normal.Get(),
        bundle.hero_orm.Get(),
    };
    for (std::size_t i = 0; i < 8U; ++i) {
        barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[i].Transition.pResource = all_res[i];
        barriers[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[i].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[i].Transition.StateAfter = final_state;
    }
    command_list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    check_hr(command_list->Close(), "Close PBR upload command list");

    ID3D12CommandList* lists[] = {command_list};
    upload_queue->ExecuteCommandLists(1, lists);
    const std::uint64_t upload_fence = next_fence_value++;
    check_hr(upload_queue->Signal(fence, upload_fence), "Signal PBR upload fence");
    if (fence->GetCompletedValue() < upload_fence) {
        check_hr(fence->SetEventOnCompletion(upload_fence, fence_event), "Wait PBR upload fence");
        WaitForSingleObject(fence_event, 10000);
    }

    auto write_array_srv = [&](ID3D12Resource* res, DXGI_FORMAT fmt, UINT mips, D3D12_CPU_DESCRIPTOR_HANDLE dst) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = fmt;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2DArray.MostDetailedMip = 0;
        srv.Texture2DArray.MipLevels = mips;
        srv.Texture2DArray.FirstArraySlice = 0;
        srv.Texture2DArray.ArraySize = k_material_count;
        device->CreateShaderResourceView(res, &srv, dst);
    };

    D3D12_CPU_DESCRIPTOR_HANDLE cursor = srv_heap_start;
    write_array_srv(bundle.albedo_array.Get(), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, k_material_mips, cursor);
    cursor.ptr += srv_descriptor_stride;
    write_array_srv(bundle.normal_array.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, k_material_mips, cursor);
    cursor.ptr += srv_descriptor_stride;
    write_array_srv(bundle.orm_array.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, k_material_mips, cursor);
    cursor.ptr += srv_descriptor_stride;

    D3D12_SHADER_RESOURCE_VIEW_DESC dfg_srv{};
    dfg_srv.Format = DXGI_FORMAT_R16G16_FLOAT;
    dfg_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    dfg_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    dfg_srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(bundle.dfg_lut.Get(), &dfg_srv, cursor);
    cursor.ptr += srv_descriptor_stride;

    D3D12_SHADER_RESOURCE_VIEW_DESC env_srv{};
    env_srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    env_srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    env_srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    env_srv.TextureCube.MostDetailedMip = 0;
    env_srv.TextureCube.MipLevels = k_env_mips;
    device->CreateShaderResourceView(bundle.env_cubemap.Get(), &env_srv, cursor);
    cursor.ptr += srv_descriptor_stride;

    auto write_tex2d_srv = [&](ID3D12Resource* res, DXGI_FORMAT fmt, UINT mips, D3D12_CPU_DESCRIPTOR_HANDLE dst) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = fmt;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MostDetailedMip = 0;
        srv.Texture2D.MipLevels = mips;
        device->CreateShaderResourceView(res, &srv, dst);
    };
    write_tex2d_srv(bundle.hero_albedo.Get(), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, hero_tex.mip_count, cursor);
    cursor.ptr += srv_descriptor_stride;
    write_tex2d_srv(bundle.hero_normal.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, hero_tex.mip_count, cursor);
    cursor.ptr += srv_descriptor_stride;
    write_tex2d_srv(bundle.hero_orm.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, hero_tex.mip_count, cursor);

    return bundle;
}

Dust2PbrBundle create_and_upload_dust2_pbr_assets(
    ID3D12Device* device,
    ID3D12CommandQueue* upload_queue,
    ID3D12CommandAllocator* command_allocator,
    ID3D12GraphicsCommandList* command_list,
    ID3D12Fence* fence,
    HANDLE fence_event,
    std::uint64_t& next_fence_value,
    const std::vector<std::string>& texture_paths,
    D3D12_CPU_DESCRIPTOR_HANDLE srv_heap_start,
    UINT srv_descriptor_stride) {
    const auto t0 = std::chrono::steady_clock::now();
    Dust2PbrBundle bundle{};
    constexpr UINT k_d2_size = 512;
    constexpr UINT k_d2_mips = 10;
    const UINT slice_count = std::max<UINT>(1U, static_cast<UINT>(std::min<std::size_t>(texture_paths.size(), 100U)));
    bundle.material_count = slice_count;

    std::vector<std::vector<MipLevelRgba8>> albedo_data(slice_count);
    std::vector<std::vector<MipLevelRgba8>> normal_data(slice_count);
    std::vector<std::vector<MipLevelRgba8>> orm_data(slice_count);

    auto catmull_w = [](float t, float out_w[4]) {
        const float t2 = t * t;
        const float t3 = t2 * t;
        out_w[0] = -0.5f * t3 + t2 - 0.5f * t;
        out_w[1] = 1.5f * t3 - 2.5f * t2 + 1.0f;
        out_w[2] = -1.5f * t3 + 2.0f * t2 + 0.5f * t;
        out_w[3] = 0.5f * t3 - 0.5f * t2;
    };

    EngineJobSystem::global().parallel_for(slice_count, 2U, [&](std::size_t begin, std::size_t end) {
        for (std::size_t s = begin; s < end; ++s) {
            const std::size_t total_px = static_cast<std::size_t>(k_d2_size) * k_d2_size;
            std::vector<Vec3> lin_albedo(total_px, Vec3{0.58f, 0.50f, 0.39f});
            std::vector<float> alpha_map(total_px, 1.0f);
            std::vector<float> base_luma(total_px, 0.5f);
            bool has_chroma_cutout = false;

            int img_w = 0;
            int img_h = 0;
            int img_c = 0;
            stbi_uc* pixels = (s < texture_paths.size() && !texture_paths[s].empty())
                ? stbi_load(texture_paths[s].c_str(), &img_w, &img_h, &img_c, 4)
                : nullptr;
            if (pixels != nullptr && img_w > 0 && img_h > 0) {
                const std::size_t src_n = static_cast<std::size_t>(img_w) * static_cast<std::size_t>(img_h);
                std::vector<Vec3> src_rgb(src_n);
                std::vector<float> src_a(src_n, 1.0f);
                std::size_t exact_black = 0;
                std::size_t border_black = 0;
                std::size_t border_total = 0;
                for (int py = 0; py < img_h; ++py) {
                    for (int px = 0; px < img_w; ++px) {
                        const std::size_t idx = static_cast<std::size_t>(py) * static_cast<std::size_t>(img_w) + static_cast<std::size_t>(px);
                        const std::uint8_t r8 = pixels[idx * 4U + 0];
                        const std::uint8_t g8 = pixels[idx * 4U + 1];
                        const std::uint8_t b8 = pixels[idx * 4U + 2];
                        const std::uint8_t a8 = pixels[idx * 4U + 3];
                        src_rgb[idx] = {
                            srgb_to_linear(static_cast<float>(r8) / 255.0f),
                            srgb_to_linear(static_cast<float>(g8) / 255.0f),
                            srgb_to_linear(static_cast<float>(b8) / 255.0f)};
                        src_a[idx] = static_cast<float>(a8) / 255.0f;
                        const bool is_blk = (r8 <= 2U && g8 <= 2U && b8 <= 2U);
                        if (is_blk) ++exact_black;
                        if (py == 0 || py == img_h - 1 || px == 0 || px == img_w - 1) {
                            ++border_total;
                            if (is_blk) ++border_black;
                        }
                    }
                }
                const float blk_frac = static_cast<float>(exact_black) / static_cast<float>(std::max<std::size_t>(src_n, 1U));
                const float bdr_frac = static_cast<float>(border_black) / static_cast<float>(std::max<std::size_t>(border_total, 1U));
                if (blk_frac >= 0.12f && bdr_frac >= 0.25f) {
                    has_chroma_cutout = true;
                    for (std::size_t idx = 0; idx < src_n; ++idx) {
                        const std::uint8_t r8 = pixels[idx * 4U + 0];
                        const std::uint8_t g8 = pixels[idx * 4U + 1];
                        const std::uint8_t b8 = pixels[idx * 4U + 2];
                        if (r8 <= 3U && g8 <= 3U && b8 <= 3U) {
                            src_a[idx] = 0.0f;
                        }
                    }
                    std::vector<Vec3> bleed_rgb = src_rgb;
                    std::vector<std::uint8_t> valid(src_n, 0U);
                    for (std::size_t idx = 0; idx < src_n; ++idx) {
                        valid[idx] = src_a[idx] > 0.5f ? 1U : 0U;
                    }
                    for (int pass = 0; pass < 4; ++pass) {
                        std::vector<Vec3> next_rgb = bleed_rgb;
                        std::vector<std::uint8_t> next_v = valid;
                        for (int py = 0; py < img_h; ++py) {
                            for (int px = 0; px < img_w; ++px) {
                                const std::size_t idx = static_cast<std::size_t>(py) * static_cast<std::size_t>(img_w) + static_cast<std::size_t>(px);
                                if (valid[idx]) continue;
                                Vec3 sum{0.0f, 0.0f, 0.0f};
                                float cnt = 0.0f;
                                const int ox[4] = {-1, 1, 0, 0};
                                const int oy[4] = {0, 0, -1, 1};
                                for (int k = 0; k < 4; ++k) {
                                    const int nx = std::clamp(px + ox[k], 0, img_w - 1);
                                    const int ny = std::clamp(py + oy[k], 0, img_h - 1);
                                    const std::size_t ni = static_cast<std::size_t>(ny) * static_cast<std::size_t>(img_w) + static_cast<std::size_t>(nx);
                                    if (valid[ni]) {
                                        sum = sum + bleed_rgb[ni];
                                        cnt += 1.0f;
                                    }
                                }
                                if (cnt > 0.0f) {
                                    next_rgb[idx] = sum * (1.0f / cnt);
                                    next_v[idx] = 1U;
                                }
                            }
                        }
                        bleed_rgb = std::move(next_rgb);
                        valid = std::move(next_v);
                    }
                    src_rgb = std::move(bleed_rgb);
                }

                auto wrap_idx = [](int c, int dim, bool clamp_edge) -> int {
                    if (clamp_edge) return std::clamp(c, 0, dim - 1);
                    const int m = c % dim;
                    return m < 0 ? m + dim : m;
                };

                for (UINT y = 0; y < k_d2_size; ++y) {
                    const float fy = (static_cast<float>(y) + 0.5f) * static_cast<float>(img_h) / static_cast<float>(k_d2_size) - 0.5f;
                    const int yi = static_cast<int>(std::floor(fy));
                    const float ty = fy - static_cast<float>(yi);
                    float wy[4];
                    catmull_w(ty, wy);
                    const int ys[4] = {
                        wrap_idx(yi - 1, img_h, has_chroma_cutout),
                        wrap_idx(yi, img_h, has_chroma_cutout),
                        wrap_idx(yi + 1, img_h, has_chroma_cutout),
                        wrap_idx(yi + 2, img_h, has_chroma_cutout)};
                    for (UINT x = 0; x < k_d2_size; ++x) {
                        const float fx = (static_cast<float>(x) + 0.5f) * static_cast<float>(img_w) / static_cast<float>(k_d2_size) - 0.5f;
                        const int xi = static_cast<int>(std::floor(fx));
                        const float tx = fx - static_cast<float>(xi);
                        float wx[4];
                        catmull_w(tx, wx);
                        const int xs[4] = {
                            wrap_idx(xi - 1, img_w, has_chroma_cutout),
                            wrap_idx(xi, img_w, has_chroma_cutout),
                            wrap_idx(xi + 1, img_w, has_chroma_cutout),
                            wrap_idx(xi + 2, img_w, has_chroma_cutout)};
                        Vec3 col{0.0f, 0.0f, 0.0f};
                        for (int ky = 0; ky < 4; ++ky) {
                            Vec3 row_c{0.0f, 0.0f, 0.0f};
                            const std::size_t rbase = static_cast<std::size_t>(ys[ky]) * static_cast<std::size_t>(img_w);
                            for (int kx = 0; kx < 4; ++kx) {
                                row_c = row_c + src_rgb[rbase + static_cast<std::size_t>(xs[kx])] * wx[kx];
                            }
                            col = col + row_c * wy[ky];
                        }
                        col.x = saturate(col.x);
                        col.y = saturate(col.y);
                        col.z = saturate(col.z);

                        const int ax0 = wrap_idx(xi, img_w, has_chroma_cutout);
                        const int ax1 = wrap_idx(xi + 1, img_w, has_chroma_cutout);
                        const int ay0 = wrap_idx(yi, img_h, has_chroma_cutout);
                        const int ay1 = wrap_idx(yi + 1, img_h, has_chroma_cutout);
                        const float a0 = src_a[static_cast<std::size_t>(ay0) * static_cast<std::size_t>(img_w) + static_cast<std::size_t>(ax0)] * (1.0f - tx)
                            + src_a[static_cast<std::size_t>(ay0) * static_cast<std::size_t>(img_w) + static_cast<std::size_t>(ax1)] * tx;
                        const float a1 = src_a[static_cast<std::size_t>(ay1) * static_cast<std::size_t>(img_w) + static_cast<std::size_t>(ax0)] * (1.0f - tx)
                            + src_a[static_cast<std::size_t>(ay1) * static_cast<std::size_t>(img_w) + static_cast<std::size_t>(ax1)] * tx;

                        const std::size_t dst = static_cast<std::size_t>(y) * k_d2_size + x;
                        lin_albedo[dst] = col;
                        alpha_map[dst] = saturate(a0 * (1.0f - ty) + a1 * ty);
                        base_luma[dst] = saturate(col.x * 0.299f + col.y * 0.587f + col.z * 0.114f);
                    }
                }
                stbi_image_free(pixels);
            }

            double sum_r = 0.0, sum_g = 0.0, sum_b = 0.0, sum_sat = 0.0, sum_luma = 0.0, sum_luma2 = 0.0;
            double sum_gx = 0.0, sum_gy = 0.0;
            std::size_t opaque_cnt = 0;
            for (UINT y = 0; y < k_d2_size; y += 2U) {
                const UINT yp = (y + 2U) & (k_d2_size - 1U);
                for (UINT x = 0; x < k_d2_size; x += 2U) {
                    const std::size_t i = static_cast<std::size_t>(y) * k_d2_size + x;
                    if (alpha_map[i] < 0.5f) continue;
                    const Vec3& c = lin_albedo[i];
                    const float mx = std::max(c.x, std::max(c.y, c.z));
                    const float mn = std::min(c.x, std::min(c.y, c.z));
                    const float sat = mx > 1e-4f ? (mx - mn) / mx : 0.0f;
                    const float lum = base_luma[i];
                    sum_r += c.x;
                    sum_g += c.y;
                    sum_b += c.z;
                    sum_sat += sat;
                    sum_luma += lum;
                    sum_luma2 += lum * lum;
                    const UINT xp = (x + 2U) & (k_d2_size - 1U);
                    sum_gx += std::fabs(base_luma[static_cast<std::size_t>(y) * k_d2_size + xp] - lum);
                    sum_gy += std::fabs(base_luma[static_cast<std::size_t>(yp) * k_d2_size + x] - lum);
                    ++opaque_cnt;
                }
            }
            const float inv_n = 1.0f / static_cast<float>(std::max<std::size_t>(opaque_cnt, 1U));
            const float mean_r = static_cast<float>(sum_r) * inv_n;
            const float mean_g = static_cast<float>(sum_g) * inv_n;
            const float mean_b = static_cast<float>(sum_b) * inv_n;
            const float mean_sat = static_cast<float>(sum_sat) * inv_n;
            const float mean_luma = static_cast<float>(sum_luma) * inv_n;
            const float luma_std = std::sqrt(std::max(0.0f, static_cast<float>(sum_luma2) * inv_n - mean_luma * mean_luma));
            const float warmth = mean_r - mean_b;
            const float gx_gy_ratio = static_cast<float>((sum_gx + 1e-4) / (sum_gy + 1e-4));

            int mat_class = 0;
            if (has_chroma_cutout) {
                mat_class = (mean_sat > 0.24f || mean_luma > 0.28f) ? 4 : 3;
            } else if (mean_sat < 0.17f && mean_luma < 0.45f) {
                mat_class = 3;
            } else if (mean_sat > 0.44f && (mean_b > mean_g * 0.88f || mean_r > mean_g * 1.45f)) {
                mat_class = 4;
            } else if (warmth > 0.04f && mean_luma < 0.24f) {
                mat_class = 2;
            } else if (luma_std > 0.145f && mean_luma < 0.44f && warmth > 0.05f) {
                mat_class = 1;
            } else if (mean_luma > 0.62f && mean_sat < 0.28f) {
                mat_class = 5;
            } else {
                mat_class = 0;
            }

            std::vector<float> blur_luma(total_px);
            for (UINT y = 0; y < k_d2_size; ++y) {
                const UINT ym2 = (y + k_d2_size - 2U) & (k_d2_size - 1U);
                const UINT yp2 = (y + 2U) & (k_d2_size - 1U);
                const UINT ym5 = (y + k_d2_size - 5U) & (k_d2_size - 1U);
                const UINT yp5 = (y + 5U) & (k_d2_size - 1U);
                for (UINT x = 0; x < k_d2_size; ++x) {
                    const UINT xm2 = (x + k_d2_size - 2U) & (k_d2_size - 1U);
                    const UINT xp2 = (x + 2U) & (k_d2_size - 1U);
                    const UINT xm5 = (x + k_d2_size - 5U) & (k_d2_size - 1U);
                    const UINT xp5 = (x + 5U) & (k_d2_size - 1U);
                    const float c0 = base_luma[static_cast<std::size_t>(y) * k_d2_size + x];
                    const float ring1 = 0.25f * (
                        base_luma[static_cast<std::size_t>(y) * k_d2_size + xm2] +
                        base_luma[static_cast<std::size_t>(y) * k_d2_size + xp2] +
                        base_luma[static_cast<std::size_t>(ym2) * k_d2_size + x] +
                        base_luma[static_cast<std::size_t>(yp2) * k_d2_size + x]);
                    const float ring2 = 0.25f * (
                        base_luma[static_cast<std::size_t>(ym5) * k_d2_size + xm5] +
                        base_luma[static_cast<std::size_t>(ym5) * k_d2_size + xp5] +
                        base_luma[static_cast<std::size_t>(yp5) * k_d2_size + xm5] +
                        base_luma[static_cast<std::size_t>(yp5) * k_d2_size + xp5]);
                    blur_luma[static_cast<std::size_t>(y) * k_d2_size + x] = c0 * 0.24f + ring1 * 0.44f + ring2 * 0.32f;
                }
            }

            std::vector<float> height_map(total_px);
            std::vector<float> micro_detail(total_px);
            const int slice_seed = static_cast<int>(s * 97U + 13U);
            const bool vertical_wood = gx_gy_ratio > 1.15f;
            for (UINT y = 0; y < k_d2_size; ++y) {
                const float v = static_cast<float>(y) / static_cast<float>(k_d2_size);
                for (UINT x = 0; x < k_d2_size; ++x) {
                    const float u = static_cast<float>(x) / static_cast<float>(k_d2_size);
                    const std::size_t idx = static_cast<std::size_t>(y) * k_d2_size + x;
                    const float lum = base_luma[idx];
                    const float bl = blur_luma[idx];
                    const float local_rel = lum - bl;
                    const float fine_n = tileable_fbm(u, v, 32, 3, slice_seed) - 0.5f;
                    const float grain_n = tileable_value_noise(u, v, 128, slice_seed + 19) - 0.5f;
                    float h_val = lum;
                    float m_val = 0.0f;

                    if (mat_class == 0) {
                        const float grit = fine_n * 0.14f + grain_n * 0.08f;
                        const float pore = std::max(0.0f, tileable_value_noise(u, v, 64, slice_seed + 41) - 0.72f) * -0.35f;
                        h_val = saturate(lum * 0.72f + bl * 0.28f + local_rel * 0.55f + grit + pore);
                        m_val = grit + pore * 0.5f;
                    } else if (mat_class == 1) {
                        const float dome = saturate(lum * 0.65f + bl * 0.35f + local_rel * 0.85f);
                        const float pebble = fine_n * 0.16f + grain_n * 0.09f;
                        h_val = saturate(dome + pebble);
                        m_val = pebble;
                    } else if (mat_class == 2) {
                        const float along = vertical_wood ? v : u;
                        const float across = vertical_wood ? u : v;
                        const float fiber = std::sin((across * 88.0f + fine_n * 5.2f) * 6.2831853f) * 0.045f
                            + (tileable_value_noise(across, along * 0.25f, 64, slice_seed + 7) - 0.5f) * 0.12f;
                        h_val = saturate(lum * 0.80f + local_rel * 0.50f + fiber);
                        m_val = fiber;
                    } else if (mat_class == 3) {
                        const float brush = (tileable_value_noise(u * 0.5f, v * 4.0f, 32, slice_seed + 23) - 0.5f) * 0.09f + grain_n * 0.05f;
                        h_val = saturate(lum * 0.78f + local_rel * 0.65f + brush);
                        m_val = brush;
                    } else if (mat_class == 4) {
                        const float weave = (std::sin(u * 512.0f * 1.570796f) * std::sin(v * 512.0f * 1.570796f)) * 0.06f + fine_n * 0.07f;
                        h_val = saturate(lum * 0.75f + weave);
                        m_val = weave;
                    } else {
                        const float stucco = fine_n * 0.10f + grain_n * 0.05f;
                        h_val = saturate(lum * 0.82f + local_rel * 0.38f + stucco);
                        m_val = stucco;
                    }
                    height_map[idx] = h_val;
                    micro_detail[idx] = m_val;

                    Vec3 c = lin_albedo[idx];
                    const float sharp_boost = local_rel * 0.26f + m_val * 0.18f;
                    const float delight = (mat_class == 0 || mat_class == 1) ? (std::max(0.0f, 0.22f - lum) * 0.18f) : 0.0f;
                    c.x = saturate(c.x * (1.0f + sharp_boost) + delight * (c.x + 0.04f));
                    c.y = saturate(c.y * (1.0f + sharp_boost) + delight * (c.y + 0.03f));
                    c.z = saturate(c.z * (1.0f + sharp_boost) + delight * (c.z + 0.02f));
                    lin_albedo[idx] = c;
                }
            }

            std::vector<Vec3> normals(total_px);
            std::vector<float> ao_map(total_px);
            std::vector<float> rough_map(total_px);
            std::vector<float> metal_map(total_px);
            std::vector<float> coat_map(total_px);
            const float fine_strength = (mat_class == 1) ? 4.4f : (mat_class == 0) ? 3.8f : (mat_class == 2) ? 3.4f : (mat_class == 3) ? 3.2f : (mat_class == 4) ? 2.4f : 2.6f;
            const float macro_strength = (mat_class == 1) ? 2.8f : (mat_class == 0) ? 2.2f : (mat_class == 2) ? 1.8f : 1.4f;

            for (UINT y = 0; y < k_d2_size; ++y) {
                const UINT ym = (y + k_d2_size - 1U) & (k_d2_size - 1U);
                const UINT yp = (y + 1U) & (k_d2_size - 1U);
                const UINT ym3 = (y + k_d2_size - 3U) & (k_d2_size - 1U);
                const UINT yp3 = (y + 3U) & (k_d2_size - 1U);
                for (UINT x = 0; x < k_d2_size; ++x) {
                    const UINT xm = (x + k_d2_size - 1U) & (k_d2_size - 1U);
                    const UINT xp = (x + 1U) & (k_d2_size - 1U);
                    const UINT xm3 = (x + k_d2_size - 3U) & (k_d2_size - 1U);
                    const UINT xp3 = (x + 3U) & (k_d2_size - 1U);

                    const float h_tl = height_map[static_cast<std::size_t>(ym) * k_d2_size + xm];
                    const float h_tc = height_map[static_cast<std::size_t>(ym) * k_d2_size + x];
                    const float h_tr = height_map[static_cast<std::size_t>(ym) * k_d2_size + xp];
                    const float h_ml = height_map[static_cast<std::size_t>(y) * k_d2_size + xm];
                    const float h_mr = height_map[static_cast<std::size_t>(y) * k_d2_size + xp];
                    const float h_bl = height_map[static_cast<std::size_t>(yp) * k_d2_size + xm];
                    const float h_bc = height_map[static_cast<std::size_t>(yp) * k_d2_size + x];
                    const float h_br = height_map[static_cast<std::size_t>(yp) * k_d2_size + xp];

                    const float scharr_dx = ((3.0f * h_tl + 10.0f * h_ml + 3.0f * h_bl) - (3.0f * h_tr + 10.0f * h_mr + 3.0f * h_br)) * (1.0f / 16.0f);
                    const float scharr_dy = ((3.0f * h_tl + 10.0f * h_tc + 3.0f * h_tr) - (3.0f * h_bl + 10.0f * h_bc + 3.0f * h_br)) * (1.0f / 16.0f);
                    const float macro_dx = (height_map[static_cast<std::size_t>(y) * k_d2_size + xm3] - height_map[static_cast<std::size_t>(y) * k_d2_size + xp3]) * 0.5f;
                    const float macro_dy = (height_map[static_cast<std::size_t>(ym3) * k_d2_size + x] - height_map[static_cast<std::size_t>(yp3) * k_d2_size + x]) * 0.5f;

                    const std::size_t idx = static_cast<std::size_t>(y) * k_d2_size + x;
                    normals[idx] = normalize3({
                        scharr_dx * fine_strength + macro_dx * macro_strength,
                        scharr_dy * fine_strength + macro_dy * macro_strength,
                        1.0f});

                    const float hc = height_map[idx];
                    const float bl = blur_luma[idx];
                    const float cavity = hc - bl;
                    const float grad = std::fabs(scharr_dx) + std::fabs(scharr_dy);
                    const Vec3& c = lin_albedo[idx];
                    const float max_c = std::max(c.x, std::max(c.y, c.z));
                    const float min_c = std::min(c.x, std::min(c.y, c.z));
                    const float sat = max_c > 1e-4f ? (max_c - min_c) / max_c : 0.0f;

                    ao_map[idx] = std::clamp(0.68f + cavity * 2.15f + hc * 0.28f - grad * 0.35f, 0.24f, 1.0f);

                    float rough = 0.78f;
                    float metal = 0.0f;
                    float coat = 0.0f;
                    if (mat_class == 0) {
                        const float mortar_mask = saturate((bl - hc) * 4.5f);
                        rough = std::clamp(0.62f + mortar_mask * 0.32f + grad * 0.45f - micro_detail[idx] * 0.35f, 0.52f, 0.96f);
                        metal = 0.0f;
                        coat = 0.0f;
                    } else if (mat_class == 1) {
                        const float crown = saturate((hc - bl) * 3.8f + 0.35f);
                        rough = std::clamp(0.90f - crown * 0.48f + grad * 0.30f, 0.34f, 0.96f);
                        metal = 0.0f;
                        coat = crown > 0.65f ? (crown - 0.65f) * 0.22f : 0.0f;
                    } else if (mat_class == 2) {
                        const bool iron_bolt = (sat < 0.14f && hc < 0.26f);
                        if (iron_bolt) {
                            metal = 0.78f;
                            rough = 0.30f;
                            coat = 0.05f;
                        } else {
                            rough = std::clamp(0.46f + (0.5f - hc) * 0.38f + grad * 0.35f, 0.34f, 0.78f);
                            metal = 0.0f;
                            coat = std::clamp(0.14f + cavity * 0.45f, 0.04f, 0.28f);
                        }
                    } else if (mat_class == 3) {
                        const float rust_dust = saturate(sat * 3.2f + std::max(0.0f, bl - hc) * 2.5f);
                        metal = std::clamp(0.92f - rust_dust * 0.78f, 0.06f, 0.94f);
                        rough = std::clamp(0.22f + rust_dust * 0.54f + grad * 0.35f, 0.18f, 0.82f);
                        coat = (1.0f - rust_dust) * 0.16f;
                    } else if (mat_class == 4) {
                        rough = std::clamp(0.86f + grad * 0.18f - micro_detail[idx] * 0.15f, 0.78f, 0.98f);
                        metal = 0.0f;
                        coat = 0.0f;
                    } else {
                        const float painted = saturate(sat * 2.4f);
                        rough = std::clamp(0.78f - painted * 0.38f + grad * 0.30f, 0.34f, 0.90f);
                        metal = 0.0f;
                        coat = painted * 0.14f;
                    }
                    rough_map[idx] = rough;
                    metal_map[idx] = metal;
                    coat_map[idx] = coat;
                }
            }

            albedo_data[s].resize(k_d2_mips);
            normal_data[s].resize(k_d2_mips);
            orm_data[s].resize(k_d2_mips);
            UINT w = k_d2_size;
            UINT h = k_d2_size;
            for (UINT mip = 0; mip < k_d2_mips; ++mip) {
                albedo_data[s][mip] = {w, h, std::vector<std::uint8_t>(static_cast<std::size_t>(w) * h * 4U)};
                normal_data[s][mip] = {w, h, std::vector<std::uint8_t>(static_cast<std::size_t>(w) * h * 4U)};
                orm_data[s][mip] = {w, h, std::vector<std::uint8_t>(static_cast<std::size_t>(w) * h * 4U)};
                for (UINT y = 0; y < h; ++y) {
                    for (UINT x = 0; x < w; ++x) {
                        const std::size_t i = static_cast<std::size_t>(y) * w + x;
                        const std::size_t o = i * 4U;
                        albedo_data[s][mip].pixels[o + 0] = float_to_u8(linear_to_srgb(lin_albedo[i].x));
                        albedo_data[s][mip].pixels[o + 1] = float_to_u8(linear_to_srgb(lin_albedo[i].y));
                        albedo_data[s][mip].pixels[o + 2] = float_to_u8(linear_to_srgb(lin_albedo[i].z));
                        albedo_data[s][mip].pixels[o + 3] = float_to_u8(alpha_map[i]);
                        const Vec3 n = normalize3(normals[i]);
                        normal_data[s][mip].pixels[o + 0] = float_to_u8(n.x * 0.5f + 0.5f);
                        normal_data[s][mip].pixels[o + 1] = float_to_u8(n.y * 0.5f + 0.5f);
                        normal_data[s][mip].pixels[o + 2] = float_to_u8(n.z * 0.5f + 0.5f);
                        normal_data[s][mip].pixels[o + 3] = float_to_u8(height_map[i]);
                        orm_data[s][mip].pixels[o + 0] = float_to_u8(ao_map[i]);
                        orm_data[s][mip].pixels[o + 1] = float_to_u8(rough_map[i]);
                        orm_data[s][mip].pixels[o + 2] = float_to_u8(metal_map[i]);
                        orm_data[s][mip].pixels[o + 3] = float_to_u8(coat_map[i]);
                    }
                }
                if (mip + 1U >= k_d2_mips) break;
                const UINT nw = std::max<UINT>(1U, w / 2U);
                const UINT nh = std::max<UINT>(1U, h / 2U);
                std::vector<Vec3> na(static_cast<std::size_t>(nw) * nh);
                std::vector<float> nalpha(static_cast<std::size_t>(nw) * nh);
                std::vector<Vec3> nn(static_cast<std::size_t>(nw) * nh);
                std::vector<float> nhgt(static_cast<std::size_t>(nw) * nh);
                std::vector<float> nao(static_cast<std::size_t>(nw) * nh);
                std::vector<float> nr(static_cast<std::size_t>(nw) * nh);
                std::vector<float> nm(static_cast<std::size_t>(nw) * nh);
                std::vector<float> ncoat(static_cast<std::size_t>(nw) * nh);
                for (UINT y = 0; y < nh; ++y) {
                    const UINT sy0 = std::min(y * 2U, h - 1U);
                    const UINT sy1 = std::min(y * 2U + 1U, h - 1U);
                    for (UINT x = 0; x < nw; ++x) {
                        const UINT sx0 = std::min(x * 2U, w - 1U);
                        const UINT sx1 = std::min(x * 2U + 1U, w - 1U);
                        const std::size_t ids[4] = {
                            static_cast<std::size_t>(sy0) * w + sx0,
                            static_cast<std::size_t>(sy0) * w + sx1,
                            static_cast<std::size_t>(sy1) * w + sx0,
                            static_cast<std::size_t>(sy1) * w + sx1};
                        Vec3 as{0.0f, 0.0f, 0.0f};
                        float alps = 0.0f;
                        Vec3 ns{0.0f, 0.0f, 0.0f};
                        float hs = 0.0f;
                        float aos = 0.0f;
                        float a2s = 0.0f;
                        float ms = 0.0f;
                        float cs = 0.0f;
                        for (std::size_t k : ids) {
                            as = as + lin_albedo[k];
                            alps += alpha_map[k];
                            ns = ns + normals[k];
                            hs += height_map[k];
                            aos += ao_map[k];
                            const float a = rough_map[k] * rough_map[k];
                            a2s += a * a;
                            ms += metal_map[k];
                            cs += coat_map[k];
                        }
                        const Vec3 navg = ns * 0.25f;
                        const float nlen = std::clamp(length3(navg), 0.05f, 1.0f);
                        const float tvar = std::clamp((1.0f - nlen) / std::max(nlen, 0.05f), 0.0f, 0.35f);
                        const float a2f = std::clamp(a2s * 0.25f + tvar * 0.5f, 0.0004f, 1.0f);
                        const std::size_t dst = static_cast<std::size_t>(y) * nw + x;
                        na[dst] = as * 0.25f;
                        nalpha[dst] = alps * 0.25f;
                        nn[dst] = normalize3(navg);
                        nhgt[dst] = hs * 0.25f;
                        nao[dst] = aos * 0.25f;
                        nr[dst] = saturate(std::sqrt(std::sqrt(a2f)));
                        nm[dst] = ms * 0.25f;
                        ncoat[dst] = cs * 0.25f;
                    }
                }
                lin_albedo = std::move(na);
                alpha_map = std::move(nalpha);
                normals = std::move(nn);
                height_map = std::move(nhgt);
                ao_map = std::move(nao);
                rough_map = std::move(nr);
                metal_map = std::move(nm);
                coat_map = std::move(ncoat);
                w = nw;
                h = nh;
            }
        }
    });

    bundle.albedo_array = create_default_texture(device, k_d2_size, k_d2_size, static_cast<UINT16>(slice_count), k_d2_mips, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, "Create Dust2 albedo array");
    bundle.normal_array = create_default_texture(device, k_d2_size, k_d2_size, static_cast<UINT16>(slice_count), k_d2_mips, DXGI_FORMAT_R8G8B8A8_UNORM, "Create Dust2 normal array");
    bundle.orm_array = create_default_texture(device, k_d2_size, k_d2_size, static_cast<UINT16>(slice_count), k_d2_mips, DXGI_FORMAT_R8G8B8A8_UNORM, "Create Dust2 ORM array");

    std::vector<UploadTask> tasks;
    UINT64 total_staging_bytes = 0;
    const UINT sub_count = slice_count * k_d2_mips;
    auto add_task = [&](ID3D12Resource* tex, const std::vector<std::vector<MipLevelRgba8>>& data) {
        UploadTask task{};
        task.destination = tex;
        task.subresource_count = sub_count;
        task.footprints.resize(sub_count);
        task.num_rows.resize(sub_count);
        task.row_sizes.resize(sub_count);
        task.src_data.resize(sub_count);
        task.src_row_pitch.resize(sub_count);
        const D3D12_RESOURCE_DESC desc = tex->GetDesc();
        UINT64 required_bytes = 0;
        total_staging_bytes = (total_staging_bytes + 511ULL) & ~511ULL;
        task.staging_offset = total_staging_bytes;
        device->GetCopyableFootprints(&desc, 0, sub_count, task.staging_offset, task.footprints.data(), task.num_rows.data(), task.row_sizes.data(), &required_bytes);
        total_staging_bytes += required_bytes;
        for (UINT s = 0; s < slice_count; ++s) {
            for (UINT m = 0; m < k_d2_mips; ++m) {
                const UINT idx = s * k_d2_mips + m;
                task.src_data[idx] = data[s][m].pixels.data();
                task.src_row_pitch[idx] = static_cast<std::size_t>(data[s][m].width) * 4U;
            }
        }
        tasks.push_back(std::move(task));
    };

    add_task(bundle.albedo_array.Get(), albedo_data);
    add_task(bundle.normal_array.Get(), normal_data);
    add_task(bundle.orm_array.Get(), orm_data);

    D3D12_HEAP_PROPERTIES upload_heap{};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC upload_desc{};
    upload_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    upload_desc.Width = total_staging_bytes;
    upload_desc.Height = 1;
    upload_desc.DepthOrArraySize = 1;
    upload_desc.MipLevels = 1;
    upload_desc.SampleDesc.Count = 1;
    upload_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> staging;
    check_hr(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&staging)), "Create Dust2 PBR staging");

    std::uint8_t* mapped = nullptr;
    D3D12_RANGE empty_range{0, 0};
    check_hr(staging->Map(0, &empty_range, reinterpret_cast<void**>(&mapped)), "Map Dust2 PBR staging");
    for (const auto& task : tasks) {
        for (UINT sub = 0; sub < task.subresource_count; ++sub) {
            const auto& fp = task.footprints[sub];
            const auto* src_bytes = static_cast<const std::uint8_t*>(task.src_data[sub]);
            std::uint8_t* dst_bytes = mapped + fp.Offset;
            const std::size_t copy_bytes = static_cast<std::size_t>(task.row_sizes[sub]);
            for (UINT row = 0; row < task.num_rows[sub]; ++row) {
                std::memcpy(
                    dst_bytes + static_cast<std::size_t>(row) * fp.Footprint.RowPitch,
                    src_bytes + static_cast<std::size_t>(row) * task.src_row_pitch[sub],
                    copy_bytes);
            }
        }
    }
    staging->Unmap(0, nullptr);

    check_hr(command_allocator->Reset(), "Reset Dust2 PBR upload allocator");
    check_hr(command_list->Reset(command_allocator, nullptr), "Reset Dust2 PBR upload command list");
    for (const auto& task : tasks) {
        for (UINT sub = 0; sub < task.subresource_count; ++sub) {
            D3D12_TEXTURE_COPY_LOCATION dst_loc{};
            dst_loc.pResource = task.destination;
            dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst_loc.SubresourceIndex = sub;
            D3D12_TEXTURE_COPY_LOCATION src_loc{};
            src_loc.pResource = staging.Get();
            src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src_loc.PlacedFootprint = task.footprints[sub];
            command_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);
        }
    }

    constexpr D3D12_RESOURCE_STATES final_state =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    std::array<D3D12_RESOURCE_BARRIER, 3> barriers{};
    ID3D12Resource* all_res[3] = {
        bundle.albedo_array.Get(),
        bundle.normal_array.Get(),
        bundle.orm_array.Get()};
    for (std::size_t i = 0; i < 3U; ++i) {
        barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[i].Transition.pResource = all_res[i];
        barriers[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[i].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[i].Transition.StateAfter = final_state;
    }
    command_list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    check_hr(command_list->Close(), "Close Dust2 PBR upload command list");

    ID3D12CommandList* lists[] = {command_list};
    upload_queue->ExecuteCommandLists(1, lists);
    const std::uint64_t upload_fence = next_fence_value++;
    check_hr(upload_queue->Signal(fence, upload_fence), "Signal Dust2 PBR upload fence");
    if (fence->GetCompletedValue() < upload_fence) {
        check_hr(fence->SetEventOnCompletion(upload_fence, fence_event), "Wait Dust2 PBR upload fence");
        WaitForSingleObject(fence_event, 15000);
    }

    auto write_array_srv = [&](ID3D12Resource* res, DXGI_FORMAT fmt, D3D12_CPU_DESCRIPTOR_HANDLE dst) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = fmt;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2DArray.MostDetailedMip = 0;
        srv.Texture2DArray.MipLevels = k_d2_mips;
        srv.Texture2DArray.FirstArraySlice = 0;
        srv.Texture2DArray.ArraySize = slice_count;
        device->CreateShaderResourceView(res, &srv, dst);
    };

    D3D12_CPU_DESCRIPTOR_HANDLE cursor = srv_heap_start;
    write_array_srv(bundle.albedo_array.Get(), DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, cursor);
    cursor.ptr += srv_descriptor_stride;
    write_array_srv(bundle.normal_array.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, cursor);
    cursor.ptr += srv_descriptor_stride;
    write_array_srv(bundle.orm_array.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, cursor);

    const auto t1 = std::chrono::steady_clock::now();
    bundle.build_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    bundle.loaded = !texture_paths.empty();
    return bundle;
}

}
