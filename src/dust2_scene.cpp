#include "dust2_scene.h"
#include "gltf_loader.h"
#include "job_system.h"

#include <cgltf.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <unordered_map>
#include <vector>

namespace frame_warp_lab {

namespace {

constexpr float k_dust2_world_scale = 42.0f;
constexpr float k_pi_f = 3.14159265358979323846f;

struct RawTri {
    std::uint32_t i0;
    std::uint32_t i1;
    std::uint32_t i2;
    Float3 centroid;
    Float3 min_b;
    Float3 max_b;
};

struct FrustumPlane {
    Float3 n;
    float d;
};

inline Float3 v3_add(Float3 a, Float3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Float3 v3_sub(Float3 a, Float3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Float3 v3_scale(Float3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float v3_dot(Float3 a, Float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Float3 v3_cross(Float3 a, Float3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float v3_len(Float3 v) { return std::sqrt(std::max(0.0f, v3_dot(v, v))); }
inline Float3 v3_norm(Float3 v, Float3 fb = {0.0f, 1.0f, 0.0f}) {
    const float l = v3_len(v);
    return l > 1e-6f ? v3_scale(v, 1.0f / l) : fb;
}
inline Float3 v3_min(Float3 a, Float3 b) { return {std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)}; }
inline Float3 v3_max(Float3 a, Float3 b) { return {std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)}; }

inline Float3 transform_col_major_point(const cgltf_float m[16], float x, float y, float z) {
    return {
        m[0] * x + m[4] * y + m[8] * z + m[12],
        m[1] * x + m[5] * y + m[9] * z + m[13],
        m[2] * x + m[6] * y + m[10] * z + m[14]};
}

inline Float3 transform_col_major_dir(const cgltf_float m[16], float x, float y, float z) {
    return {
        m[0] * x + m[4] * y + m[8] * z,
        m[1] * x + m[5] * y + m[9] * z,
        m[2] * x + m[6] * y + m[10] * z};
}

void compute_mesh_tangents(std::vector<RangeVertex>& vertices, const std::vector<std::uint32_t>& indices) {
    std::vector<Float3> tan_accum(vertices.size(), Float3{0.0f, 0.0f, 0.0f});
    for (std::size_t i = 0; i + 2U < indices.size(); i += 3U) {
        const std::uint32_t i0 = indices[i + 0U];
        const std::uint32_t i1 = indices[i + 1U];
        const std::uint32_t i2 = indices[i + 2U];
        if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) continue;
        const RangeVertex& v0 = vertices[i0];
        const RangeVertex& v1 = vertices[i1];
        const RangeVertex& v2 = vertices[i2];
        const float dx1 = v1.px - v0.px;
        const float dy1 = v1.py - v0.py;
        const float dz1 = v1.pz - v0.pz;
        const float dx2 = v2.px - v0.px;
        const float dy2 = v2.py - v0.py;
        const float dz2 = v2.pz - v0.pz;
        const float du1 = v1.u - v0.u;
        const float dv1 = v1.v - v0.v;
        const float du2 = v2.u - v0.u;
        const float dv2 = v2.v - v0.v;
        const float det = du1 * dv2 - du2 * dv1;
        if (std::fabs(det) < 1e-8f) continue;
        const float inv = 1.0f / det;
        const Float3 t{(dx1 * dv2 - dx2 * dv1) * inv, (dy1 * dv2 - dy2 * dv1) * inv, (dz1 * dv2 - dz2 * dv1) * inv};
        tan_accum[i0] = v3_add(tan_accum[i0], t);
        tan_accum[i1] = v3_add(tan_accum[i1], t);
        tan_accum[i2] = v3_add(tan_accum[i2], t);
    }
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const Float3 n = v3_norm({vertices[i].nx, vertices[i].ny, vertices[i].nz}, {0.0f, 1.0f, 0.0f});
        const Float3 fb = std::fabs(n.y) < 0.9f ? Float3{0.0f, 1.0f, 0.0f} : Float3{1.0f, 0.0f, 0.0f};
        Float3 t = tan_accum[i];
        t = v3_sub(t, v3_scale(n, v3_dot(t, n)));
        t = v3_norm(t, fb);
        vertices[i].nx = n.x;
        vertices[i].ny = n.y;
        vertices[i].nz = n.z;
        vertices[i].tx = t.x;
        vertices[i].ty = t.y;
        vertices[i].tz = t.z;
    }
}

Dust2LodSlice build_quantized_lod_indices(
    const std::vector<RangeVertex>& vertices,
    const std::vector<RawTri>& tris,
    const std::vector<std::uint32_t>& tri_ids,
    float cell_size,
    std::vector<std::uint32_t>& out_indices) {
    Dust2LodSlice slice{};
    slice.index_offset = static_cast<std::uint32_t>(out_indices.size());
    if (cell_size <= 0.0f) {
        out_indices.reserve(out_indices.size() + tri_ids.size() * 3U);
        for (std::uint32_t tid : tri_ids) {
            const RawTri& t = tris[tid];
            out_indices.push_back(t.i0);
            out_indices.push_back(t.i1);
            out_indices.push_back(t.i2);
        }
        slice.index_count = static_cast<std::uint32_t>(out_indices.size()) - slice.index_offset;
        return slice;
    }

    const float inv_cell = 1.0f / cell_size;
    std::unordered_map<std::uint64_t, std::uint32_t> rep_map;
    rep_map.reserve(tri_ids.size() * 2U);

    auto quant_rep = [&](std::uint32_t vi) -> std::uint32_t {
        const RangeVertex& v = vertices[vi];
        const auto qx = static_cast<std::int64_t>(std::floor(v.px * inv_cell)) & 0xFFFFLL;
        const auto qy = static_cast<std::int64_t>(std::floor(v.py * inv_cell)) & 0xFFFFLL;
        const auto qz = static_cast<std::int64_t>(std::floor(v.pz * inv_cell)) & 0xFFFFLL;
        const auto mat = static_cast<std::uint64_t>(v.material_id & 0xFFU);
        const std::uint64_t key = static_cast<std::uint64_t>(qx)
            | (static_cast<std::uint64_t>(qy) << 16U)
            | (static_cast<std::uint64_t>(qz) << 32U)
            | (mat << 48U);
        auto [it, inserted] = rep_map.emplace(key, vi);
        return it->second;
    };

    for (std::uint32_t tid : tri_ids) {
        const RawTri& t = tris[tid];
        const std::uint32_t r0 = quant_rep(t.i0);
        const std::uint32_t r1 = quant_rep(t.i1);
        const std::uint32_t r2 = quant_rep(t.i2);
        if (r0 == r1 || r1 == r2 || r2 == r0) {
            continue;
        }
        out_indices.push_back(r0);
        out_indices.push_back(r1);
        out_indices.push_back(r2);
    }

    if (out_indices.size() == slice.index_offset && !tri_ids.empty()) {
        const std::size_t step = std::max<std::size_t>(1U, tri_ids.size() / 24U);
        for (std::size_t i = 0; i < tri_ids.size(); i += step) {
            const RawTri& t = tris[tri_ids[i]];
            out_indices.push_back(t.i0);
            out_indices.push_back(t.i1);
            out_indices.push_back(t.i2);
        }
    }
    slice.index_count = static_cast<std::uint32_t>(out_indices.size()) - slice.index_offset;
    return slice;
}

std::int32_t build_octree_recursive(
    const std::vector<RangeVertex>& vertices,
    const std::vector<RawTri>& tris,
    const std::vector<std::uint32_t>& tri_ids,
    int depth,
    std::vector<Dust2OctreeNode>& nodes,
    std::vector<std::uint32_t>& lod0_indices,
    std::vector<std::uint32_t>& lod12_indices,
    std::vector<std::vector<std::uint32_t>>& leaf_tri_lists,
    std::uint32_t& leaf_count) {
    if (tri_ids.empty()) {
        return -1;
    }
    const auto node_idx = static_cast<std::int32_t>(nodes.size());
    nodes.emplace_back();
    leaf_tri_lists.emplace_back();

    Float3 bmin{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    Float3 bmax{-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};
    for (std::uint32_t tid : tri_ids) {
        bmin = v3_min(bmin, tris[tid].min_b);
        bmax = v3_max(bmax, tris[tid].max_b);
    }
    const Float3 center = v3_scale(v3_add(bmin, bmax), 0.5f);
    const Float3 half = v3_scale(v3_sub(bmax, bmin), 0.5f);
    const float radius = v3_len(half);

    nodes[static_cast<std::size_t>(node_idx)].min_bounds = bmin;
    nodes[static_cast<std::size_t>(node_idx)].max_bounds = bmax;
    nodes[static_cast<std::size_t>(node_idx)].center = center;
    nodes[static_cast<std::size_t>(node_idx)].radius = radius;

    constexpr int k_max_depth = 4;
    constexpr std::size_t k_leaf_tri_target = 640U;
    if (depth >= k_max_depth || tri_ids.size() <= k_leaf_tri_target) {
        auto& n = nodes[static_cast<std::size_t>(node_idx)];
        n.is_leaf = true;
        n.lods[0] = build_quantized_lod_indices(vertices, tris, tri_ids, 0.0f, lod0_indices);
        n.lods[1] = build_quantized_lod_indices(vertices, tris, tri_ids, 0.55f, lod12_indices);
        n.lods[2] = build_quantized_lod_indices(vertices, tris, tri_ids, 1.35f, lod12_indices);
        leaf_tri_lists[static_cast<std::size_t>(node_idx)] = tri_ids;
        ++leaf_count;
        return node_idx;
    }

    std::array<std::vector<std::uint32_t>, 8> octants{};
    for (std::uint32_t tid : tri_ids) {
        const Float3& c = tris[tid].centroid;
        const std::size_t oct = (c.x >= center.x ? 1U : 0U)
            | (c.y >= center.y ? 2U : 0U)
            | (c.z >= center.z ? 4U : 0U);
        octants[oct].push_back(tid);
    }

    int non_empty = 0;
    for (const auto& o : octants) {
        if (!o.empty()) ++non_empty;
    }
    if (non_empty <= 1) {
        auto& n = nodes[static_cast<std::size_t>(node_idx)];
        n.is_leaf = true;
        n.lods[0] = build_quantized_lod_indices(vertices, tris, tri_ids, 0.0f, lod0_indices);
        n.lods[1] = build_quantized_lod_indices(vertices, tris, tri_ids, 0.55f, lod12_indices);
        n.lods[2] = build_quantized_lod_indices(vertices, tris, tri_ids, 1.35f, lod12_indices);
        leaf_tri_lists[static_cast<std::size_t>(node_idx)] = tri_ids;
        ++leaf_count;
        return node_idx;
    }

    for (std::size_t o = 0; o < 8U; ++o) {
        const std::int32_t child = build_octree_recursive(
            vertices, tris, octants[o], depth + 1, nodes, lod0_indices, lod12_indices, leaf_tri_lists, leaf_count);
        nodes[static_cast<std::size_t>(node_idx)].children[o] = child;
    }
    return node_idx;
}

bool ray_aabb_hit(Float3 orig, Float3 inv_dir, Float3 bmin, Float3 bmax, float max_t) {
    const float tx1 = (bmin.x - orig.x) * inv_dir.x;
    const float tx2 = (bmax.x - orig.x) * inv_dir.x;
    float tmin = std::min(tx1, tx2);
    float tmax = std::max(tx1, tx2);
    const float ty1 = (bmin.y - orig.y) * inv_dir.y;
    const float ty2 = (bmax.y - orig.y) * inv_dir.y;
    tmin = std::max(tmin, std::min(ty1, ty2));
    tmax = std::min(tmax, std::max(ty1, ty2));
    const float tz1 = (bmin.z - orig.z) * inv_dir.z;
    const float tz2 = (bmax.z - orig.z) * inv_dir.z;
    tmin = std::max(tmin, std::min(tz1, tz2));
    tmax = std::min(tmax, std::max(tz1, tz2));
    return tmax >= std::max(0.0f, tmin) && tmin <= max_t;
}

bool ray_tri_hit(Float3 orig, Float3 dir, const RangeVertex& v0, const RangeVertex& v1, const RangeVertex& v2, float& out_t) {
    const Float3 p0{v0.px, v0.py, v0.pz};
    const Float3 e1{v1.px - v0.px, v1.py - v0.py, v1.pz - v0.pz};
    const Float3 e2{v2.px - v0.px, v2.py - v0.py, v2.pz - v0.pz};
    const Float3 pvec = v3_cross(dir, e2);
    const float det = v3_dot(e1, pvec);
    if (std::fabs(det) < 1e-7f) return false;
    const float inv_det = 1.0f / det;
    const Float3 tvec = v3_sub(orig, p0);
    const float u = v3_dot(tvec, pvec) * inv_det;
    if (u < 0.0f || u > 1.0f) return false;
    const Float3 qvec = v3_cross(tvec, e1);
    const float v = v3_dot(dir, qvec) * inv_det;
    if (v < 0.0f || u + v > 1.0f) return false;
    const float t = v3_dot(e2, qvec) * inv_det;
    if (t > 0.04f && t < out_t) {
        out_t = t;
        return true;
    }
    return false;
}

float raycast_octree(
    const std::vector<Dust2OctreeNode>& nodes,
    const std::vector<std::vector<std::uint32_t>>& leaf_tris,
    const std::vector<RangeVertex>& vertices,
    const std::vector<RawTri>& tris,
    Float3 orig,
    Float3 dir,
    float max_t,
    Float3* out_hit_normal) {
    if (nodes.empty()) return max_t;
    const Float3 inv_dir{
        std::fabs(dir.x) > 1e-6f ? 1.0f / dir.x : 1e6f,
        std::fabs(dir.y) > 1e-6f ? 1.0f / dir.y : 1e6f,
        std::fabs(dir.z) > 1e-6f ? 1.0f / dir.z : 1e6f};
    float best_t = max_t;
    std::int32_t stack[64];
    int sp = 0;
    stack[sp++] = 0;
    while (sp > 0) {
        const std::int32_t ni = stack[--sp];
        const auto& n = nodes[static_cast<std::size_t>(ni)];
        if (!ray_aabb_hit(orig, inv_dir, n.min_bounds, n.max_bounds, best_t)) {
            continue;
        }
        if (n.is_leaf) {
            for (std::uint32_t tid : leaf_tris[static_cast<std::size_t>(ni)]) {
                const RawTri& rt = tris[tid];
                float hit_t = best_t;
                if (ray_tri_hit(orig, dir, vertices[rt.i0], vertices[rt.i1], vertices[rt.i2], hit_t)) {
                    best_t = hit_t;
                    if (out_hit_normal != nullptr) {
                        *out_hit_normal = {vertices[rt.i0].nx, vertices[rt.i0].ny, vertices[rt.i0].nz};
                    }
                }
            }
        } else {
            for (int c = 0; c < 8; ++c) {
                if (n.children[c] >= 0 && sp < 64) {
                    stack[sp++] = n.children[c];
                }
            }
        }
    }
    return best_t;
}

std::array<FrustumPlane, 6> build_camera_frustum(const CameraBasis& cam, float aspect_ratio, float near_z, float far_z) {
    const float tan_y = cam.tan_half_fov_y;
    const float tan_x = tan_y * aspect_ratio;
    std::array<FrustumPlane, 6> planes{};
    auto make_plane = [&](Float3 n, Float3 pt) -> FrustumPlane {
        const Float3 nn = v3_norm(n);
        return {nn, -v3_dot(nn, pt)};
    };
    planes[0] = make_plane(cam.forward, v3_add(cam.position, v3_scale(cam.forward, near_z)));
    planes[1] = make_plane(v3_scale(cam.forward, -1.0f), v3_add(cam.position, v3_scale(cam.forward, far_z)));
    planes[2] = make_plane(v3_add(v3_scale(cam.forward, tan_x), cam.right), cam.position);
    planes[3] = make_plane(v3_sub(v3_scale(cam.forward, tan_x), cam.right), cam.position);
    planes[4] = make_plane(v3_add(v3_scale(cam.forward, tan_y), cam.up), cam.position);
    planes[5] = make_plane(v3_sub(v3_scale(cam.forward, tan_y), cam.up), cam.position);
    return planes;
}

bool aabb_in_frustum(const std::array<FrustumPlane, 6>& planes, Float3 bmin, Float3 bmax) {
    for (const auto& pl : planes) {
        const Float3 p{
            pl.n.x >= 0.0f ? bmax.x : bmin.x,
            pl.n.y >= 0.0f ? bmax.y : bmin.y,
            pl.n.z >= 0.0f ? bmax.z : bmin.z};
        if (v3_dot(pl.n, p) + pl.d < -0.15f) {
            return false;
        }
    }
    return true;
}

GpuClusterLight make_point_light(Float3 pos, float radius, Float3 color, float atten) {
    GpuClusterLight l{};
    l.pos_radius[0] = pos.x;
    l.pos_radius[1] = pos.y;
    l.pos_radius[2] = pos.z;
    l.pos_radius[3] = radius;
    l.dir_cos_outer[0] = 0.0f;
    l.dir_cos_outer[1] = -1.0f;
    l.dir_cos_outer[2] = 0.0f;
    l.dir_cos_outer[3] = -1.0f;
    l.color_atten[0] = color.x;
    l.color_atten[1] = color.y;
    l.color_atten[2] = color.z;
    l.color_atten[3] = atten;
    l.params[0] = 0.0f;
    l.params[1] = -1.0f;
    l.params[2] = 0.0f;
    l.params[3] = 0.0f;
    return l;
}

GpuClusterLight make_spot_light(Float3 pos, float radius, Float3 dir, float cos_inner, float cos_outer, Float3 color, float atten) {
    const Float3 nd = v3_norm(dir, {0.0f, -1.0f, 0.0f});
    GpuClusterLight l{};
    l.pos_radius[0] = pos.x;
    l.pos_radius[1] = pos.y;
    l.pos_radius[2] = pos.z;
    l.pos_radius[3] = radius;
    l.dir_cos_outer[0] = nd.x;
    l.dir_cos_outer[1] = nd.y;
    l.dir_cos_outer[2] = nd.z;
    l.dir_cos_outer[3] = cos_outer;
    l.color_atten[0] = color.x;
    l.color_atten[1] = color.y;
    l.color_atten[2] = color.z;
    l.color_atten[3] = atten;
    l.params[0] = 1.0f;
    l.params[1] = cos_inner;
    l.params[2] = 0.0f;
    l.params[3] = 0.0f;
    return l;
}

GpuClusterLight make_area_light(Float3 pos, float radius, Float3 color, float half_w, float half_d) {
    GpuClusterLight l{};
    l.pos_radius[0] = pos.x;
    l.pos_radius[1] = pos.y;
    l.pos_radius[2] = pos.z;
    l.pos_radius[3] = radius;
    l.dir_cos_outer[0] = 0.0f;
    l.dir_cos_outer[1] = -1.0f;
    l.dir_cos_outer[2] = 0.0f;
    l.dir_cos_outer[3] = -1.0f;
    l.color_atten[0] = color.x;
    l.color_atten[1] = color.y;
    l.color_atten[2] = color.z;
    l.color_atten[3] = 1.25f;
    l.params[0] = 2.0f;
    l.params[1] = 0.0f;
    l.params[2] = half_w;
    l.params[3] = half_d;
    return l;
}

GpuClusterLight make_sun_light(Float3 dir_to_sun, Float3 color) {
    const Float3 nd = v3_norm(dir_to_sun, {0.42f, 0.84f, 0.34f});
    GpuClusterLight l{};
    l.pos_radius[0] = nd.x * 200.0f;
    l.pos_radius[1] = nd.y * 200.0f;
    l.pos_radius[2] = nd.z * 200.0f;
    l.pos_radius[3] = 1000.0f;
    l.dir_cos_outer[0] = nd.x;
    l.dir_cos_outer[1] = nd.y;
    l.dir_cos_outer[2] = nd.z;
    l.dir_cos_outer[3] = -1.0f;
    l.color_atten[0] = color.x;
    l.color_atten[1] = color.y;
    l.color_atten[2] = color.z;
    l.color_atten[3] = 0.0f;
    l.params[0] = 3.0f;
    l.params[1] = 0.0f;
    l.params[2] = 0.0f;
    l.params[3] = 0.0f;
    return l;
}

}

std::vector<GpuClusterLight> build_range_cluster_lights() {
    std::vector<GpuClusterLight> lights;
    lights.reserve(14);
    const float area_z[5] = {5.4f, 10.6f, 15.8f, 21.0f, 26.2f};
    const Float3 area_rad[5] = {
        {10.5f, 11.8f, 13.8f},
        {10.0f, 11.0f, 12.2f},
        {11.2f, 10.6f, 9.2f},
        {11.5f, 10.4f, 8.8f},
        {10.8f, 9.6f, 7.8f},
    };
    for (int i = 0; i < 5; ++i) {
        lights.push_back(make_area_light({0.0f, 4.55f, area_z[i]}, 34.0f, area_rad[i], 1.40f, 0.14f));
    }
    lights.push_back(make_spot_light({0.0f, 4.24f, 8.2f}, 34.0f, {0.0f, -0.88f, 0.47f}, 0.84f, 0.58f, {24.0f, 22.8f, 20.5f}, 0.075f));
    lights.push_back(make_spot_light({-0.35f, 4.24f, 14.4f}, 34.0f, {0.0f, -0.89f, 0.45f}, 0.82f, 0.54f, {25.0f, 23.0f, 19.5f}, 0.065f));
    lights.push_back(make_spot_light({0.25f, 4.24f, 21.0f}, 34.0f, {0.0f, -0.88f, 0.47f}, 0.82f, 0.54f, {26.0f, 23.2f, 19.0f}, 0.060f));
    lights.push_back(make_spot_light({-5.95f, 3.15f, 4.25f}, 34.0f, {0.76f, -0.52f, 0.39f}, 0.72f, 0.25f, {22.0f, 1.4f, 0.9f}, 0.11f));
    lights.push_back(make_spot_light({5.95f, 3.15f, 4.25f}, 34.0f, {-0.76f, -0.52f, 0.39f}, 0.72f, 0.25f, {22.0f, 1.4f, 0.9f}, 0.11f));
    lights.push_back(make_spot_light({-5.60f, 3.38f, 28.5f}, 34.0f, {0.68f, -0.54f, 0.49f}, 0.74f, 0.30f, {22.0f, 10.5f, 1.6f}, 0.075f));
    lights.push_back(make_spot_light({5.60f, 3.38f, 28.5f}, 34.0f, {-0.68f, -0.54f, 0.49f}, 0.74f, 0.30f, {22.0f, 10.5f, 1.6f}, 0.075f));
    lights.push_back(make_point_light({-2.15f, 0.92f, 4.35f}, 18.0f, {1.1f, 7.4f, 12.2f}, 0.38f));
    lights.push_back(make_point_light({2.15f, 0.92f, 4.35f}, 18.0f, {1.1f, 7.4f, 12.2f}, 0.38f));
    return lights;
}

std::vector<OctreeVisProbeGpu> build_range_visibility_probes() {
    std::vector<OctreeVisProbeGpu> probes;
    probes.reserve(16);
    for (int zi = 0; zi < 4; ++zi) {
        for (int xi = -1; xi <= 1; xi += 2) {
            OctreeVisProbeGpu p{};
            p.pos_radius[0] = static_cast<float>(xi) * 2.2f;
            p.pos_radius[1] = 2.0f;
            p.pos_radius[2] = 5.0f + static_cast<float>(zi) * 7.5f;
            p.pos_radius[3] = 8.0f;
            p.sky_bounce[0] = 0.28f;
            p.sky_bounce[1] = 0.26f;
            p.sky_bounce[2] = 0.25f;
            p.sky_bounce[3] = 0.15f;
            p.dir_vis_pos[0] = 0.65f;
            p.dir_vis_pos[1] = 0.55f;
            p.dir_vis_pos[2] = 0.85f;
            p.dir_vis_pos[3] = 1.0f;
            p.dir_vis_neg[0] = 0.65f;
            p.dir_vis_neg[1] = 0.45f;
            p.dir_vis_neg[2] = 0.85f;
            p.dir_vis_neg[3] = 1.0f;
            probes.push_back(p);
        }
    }
    return probes;
}

Dust2MapPackage load_and_build_dust2_map() {
    Dust2MapPackage pkg{};
    const std::filesystem::path gltf_path = resolve_asset_path("assets/maps/de_dust2/scene.gltf");
    std::error_code ec;
    if (!std::filesystem::exists(gltf_path, ec)) {
        return pkg;
    }

    cgltf_options opts{};
    cgltf_data* data = nullptr;
    const std::string utf8_path = gltf_path.string();
    if (cgltf_parse_file(&opts, utf8_path.c_str(), &data) != cgltf_result_success || data == nullptr) {
        return pkg;
    }
    if (cgltf_load_buffers(&opts, data, utf8_path.c_str()) != cgltf_result_success) {
        cgltf_free(data);
        return pkg;
    }

    const std::filesystem::path base_dir = gltf_path.parent_path();
    const std::filesystem::path hd_dir = base_dir / "textures_hd";
    const std::size_t mat_count = std::min<std::size_t>(data->materials_count, k_dust2_max_materials);
    pkg.texture_paths.resize(mat_count);
    for (std::size_t mi = 0; mi < mat_count; ++mi) {
        const cgltf_material& mat = data->materials[mi];
        if (mat.has_pbr_metallic_roughness && mat.pbr_metallic_roughness.base_color_texture.texture != nullptr) {
            const cgltf_image* img = mat.pbr_metallic_roughness.base_color_texture.texture->image;
            if (img != nullptr && img->uri != nullptr && img->uri[0] != '\0') {
                const std::filesystem::path orig_path = base_dir / img->uri;
                const std::string stem = orig_path.stem().string();
                std::filesystem::path chosen = orig_path;
                const std::array<const char*, 4> hd_exts = {".png", ".jpg", ".jpeg", ".bmp"};
                bool found_hd = false;
                for (const char* ext : hd_exts) {
                    const std::filesystem::path cand = hd_dir / (stem + ext);
                    if (std::filesystem::exists(cand, ec)) {
                        chosen = cand;
                        found_hd = true;
                        break;
                    }
                }
                if (!found_hd && !std::filesystem::exists(chosen, ec)) {
                    const std::filesystem::path fallback_png = orig_path.parent_path() / (stem + ".png");
                    if (std::filesystem::exists(fallback_png, ec)) {
                        chosen = fallback_png;
                    }
                }
                pkg.texture_paths[mi] = chosen.string();
            }
        }
    }

    std::vector<RawTri> raw_tris;
    Float3 raw_min{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    Float3 raw_max{-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};
    double up_normal_sum = 0.0;

    for (cgltf_size ni = 0; ni < data->nodes_count; ++ni) {
        const cgltf_node& node = data->nodes[ni];
        if (node.mesh == nullptr) continue;
        cgltf_float world_m[16]{};
        cgltf_node_transform_world(&node, world_m);

        for (cgltf_size pi = 0; pi < node.mesh->primitives_count; ++pi) {
            const cgltf_primitive& prim = node.mesh->primitives[pi];
            if (prim.type != cgltf_primitive_type_triangles) continue;
            const cgltf_accessor* pos_acc = nullptr;
            const cgltf_accessor* norm_acc = nullptr;
            const cgltf_accessor* uv_acc = nullptr;
            for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
                const cgltf_attribute& attr = prim.attributes[a];
                if (attr.type == cgltf_attribute_type_position) pos_acc = attr.data;
                else if (attr.type == cgltf_attribute_type_normal) norm_acc = attr.data;
                else if (attr.type == cgltf_attribute_type_texcoord && attr.index == 0) uv_acc = attr.data;
            }
            if (pos_acc == nullptr || pos_acc->count == 0) continue;

            std::uint32_t mat_slice = 0;
            if (prim.material != nullptr && data->materials != nullptr) {
                const std::ptrdiff_t idx = prim.material - data->materials;
                if (idx >= 0 && static_cast<std::size_t>(idx) < mat_count) {
                    mat_slice = static_cast<std::uint32_t>(idx);
                }
            }
            const std::uint32_t shader_mat_id = k_dust2_material_base + mat_slice;

            const auto base_v = static_cast<std::uint32_t>(pkg.vertices.size());
            pkg.vertices.resize(base_v + static_cast<std::uint32_t>(pos_acc->count));
            for (cgltf_size v = 0; v < pos_acc->count; ++v) {
                float p[3] = {0.0f, 0.0f, 0.0f};
                float n[3] = {0.0f, 1.0f, 0.0f};
                float uv[2] = {0.0f, 0.0f};
                cgltf_accessor_read_float(pos_acc, v, p, 3);
                if (norm_acc != nullptr) cgltf_accessor_read_float(norm_acc, v, n, 3);
                if (uv_acc != nullptr) cgltf_accessor_read_float(uv_acc, v, uv, 2);

                const Float3 wp = v3_scale(transform_col_major_point(world_m, p[0], p[1], p[2]), k_dust2_world_scale);
                const Float3 wn = v3_norm(transform_col_major_dir(world_m, n[0], n[1], n[2]), {0.0f, 1.0f, 0.0f});
                up_normal_sum += static_cast<double>(wn.y);

                RangeVertex rv{};
                rv.px = wp.x;
                rv.py = wp.y;
                rv.pz = wp.z;
                rv.nx = wn.x;
                rv.ny = wn.y;
                rv.nz = wn.z;
                rv.tx = 1.0f;
                rv.ty = 0.0f;
                rv.tz = 0.0f;
                rv.u = uv[0];
                rv.v = uv[1];
                rv.material_id = shader_mat_id;
                rv.emissive_weight = 0.0f;
                pkg.vertices[base_v + static_cast<std::uint32_t>(v)] = rv;

                raw_min = v3_min(raw_min, wp);
                raw_max = v3_max(raw_max, wp);
            }

            if (prim.indices != nullptr) {
                for (cgltf_size idx = 0; idx + 2U < prim.indices->count; idx += 3U) {
                    const auto i0 = base_v + static_cast<std::uint32_t>(cgltf_accessor_read_index(prim.indices, idx + 0U));
                    const auto i1 = base_v + static_cast<std::uint32_t>(cgltf_accessor_read_index(prim.indices, idx + 1U));
                    const auto i2 = base_v + static_cast<std::uint32_t>(cgltf_accessor_read_index(prim.indices, idx + 2U));
                    if (i0 == i1 || i1 == i2 || i2 == i0) continue;
                    RawTri rt{};
                    rt.i0 = i0;
                    rt.i1 = i1;
                    rt.i2 = i2;
                    raw_tris.push_back(rt);
                }
            } else {
                for (cgltf_size idx = 0; idx + 2U < pos_acc->count; idx += 3U) {
                    const auto i0 = base_v + static_cast<std::uint32_t>(idx + 0U);
                    const auto i1 = base_v + static_cast<std::uint32_t>(idx + 1U);
                    const auto i2 = base_v + static_cast<std::uint32_t>(idx + 2U);
                    RawTri rt{};
                    rt.i0 = i0;
                    rt.i1 = i1;
                    rt.i2 = i2;
                    raw_tris.push_back(rt);
                }
            }
        }
    }
    cgltf_free(data);

    if (pkg.vertices.empty() || raw_tris.empty()) {
        return pkg;
    }

    if (up_normal_sum < -100.0) {
        raw_min = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
        raw_max = {-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};
        for (auto& v : pkg.vertices) {
            v.py = -v.py;
            v.ny = -v.ny;
            const Float3 p{v.px, v.py, v.pz};
            raw_min = v3_min(raw_min, p);
            raw_max = v3_max(raw_max, p);
        }
        for (auto& rt : raw_tris) {
            std::swap(rt.i1, rt.i2);
        }
    }

    const float y_shift = -raw_min.y;
    for (auto& v : pkg.vertices) {
        v.py += y_shift;
    }
    raw_min.y += y_shift;
    raw_max.y += y_shift;
    pkg.world_min = raw_min;
    pkg.world_max = raw_max;

    for (auto& rt : raw_tris) {
        const RangeVertex& v0 = pkg.vertices[rt.i0];
        const RangeVertex& v1 = pkg.vertices[rt.i1];
        const RangeVertex& v2 = pkg.vertices[rt.i2];
        const Float3 p0{v0.px, v0.py, v0.pz};
        const Float3 p1{v1.px, v1.py, v1.pz};
        const Float3 p2{v2.px, v2.py, v2.pz};
        rt.centroid = v3_scale(v3_add(v3_add(p0, p1), p2), 1.0f / 3.0f);
        rt.min_b = v3_min(v3_min(p0, p1), p2);
        rt.max_b = v3_max(v3_max(p0, p1), p2);
    }

    std::vector<std::uint32_t> all_tri_ids(raw_tris.size());
    for (std::size_t i = 0; i < raw_tris.size(); ++i) {
        all_tri_ids[i] = static_cast<std::uint32_t>(i);
    }

    std::vector<std::uint32_t> lod0_indices;
    std::vector<std::uint32_t> lod12_indices;
    std::vector<std::vector<std::uint32_t>> leaf_tri_lists;
    pkg.leaf_count = 0;
    build_octree_recursive(
        pkg.vertices, raw_tris, all_tri_ids, 0,
        pkg.octree_nodes, lod0_indices, lod12_indices, leaf_tri_lists, pkg.leaf_count);

    pkg.lod0_total_index_count = static_cast<std::uint32_t>(lod0_indices.size());
    pkg.total_lod0_triangles = pkg.lod0_total_index_count / 3U;
    pkg.indices.reserve(lod0_indices.size() + lod12_indices.size());
    pkg.indices.insert(pkg.indices.end(), lod0_indices.begin(), lod0_indices.end());
    pkg.indices.insert(pkg.indices.end(), lod12_indices.begin(), lod12_indices.end());
    for (auto& n : pkg.octree_nodes) {
        if (n.is_leaf) {
            n.lods[1].index_offset += pkg.lod0_total_index_count;
            n.lods[2].index_offset += pkg.lod0_total_index_count;
        }
    }

    compute_mesh_tangents(pkg.vertices, lod0_indices);

    std::vector<Float3> candidate_probe_positions;
    std::vector<bool> candidate_walkable;
    const int grid_x = 16;
    const int grid_z = 16;
    const float span_x = std::max(pkg.world_max.x - pkg.world_min.x, 1.0f);
    const float span_z = std::max(pkg.world_max.z - pkg.world_min.z, 1.0f);
    const float max_walk_y = pkg.world_min.y + 7.2f;
    for (int gz = 1; gz < grid_z - 1; ++gz) {
        for (int gx = 1; gx < grid_x - 1; ++gx) {
            const float wx = pkg.world_min.x + (static_cast<float>(gx) + 0.5f) * (span_x / static_cast<float>(grid_x));
            const float wz = pkg.world_min.z + (static_cast<float>(gz) + 0.5f) * (span_z / static_cast<float>(grid_z));
            float cur_y = pkg.world_max.y + 2.0f;
            float chosen_floor_y = -1.0f;
            bool chosen_is_street = false;
            for (int step = 0; step < 6 && cur_y > pkg.world_min.y + 0.2f; ++step) {
                Float3 hit_n{0.0f, 1.0f, 0.0f};
                const float max_d = (cur_y - pkg.world_min.y) + 4.0f;
                const float hit_t = raycast_octree(
                    pkg.octree_nodes, leaf_tri_lists, pkg.vertices, raw_tris,
                    {wx, cur_y, wz}, {0.0f, -1.0f, 0.0f}, max_d, &hit_n);
                if (hit_t >= max_d - 0.5f) break;
                const float hit_y = cur_y - hit_t;
                if (hit_n.y > 0.65f && hit_y >= pkg.world_min.y + 0.05f) {
                    const float up_clear = raycast_octree(
                        pkg.octree_nodes, leaf_tri_lists, pkg.vertices, raw_tris,
                        {wx, hit_y + 0.25f, wz}, {0.0f, 1.0f, 0.0f}, 6.0f, nullptr);
                    if (up_clear >= 2.6f) {
                        if (hit_y <= max_walk_y) {
                            chosen_floor_y = hit_y;
                            chosen_is_street = true;
                            break;
                        }
                        if (chosen_floor_y < 0.0f && hit_y < pkg.world_max.y - 2.5f) {
                            chosen_floor_y = hit_y;
                        }
                    }
                }
                cur_y = hit_y - 0.45f;
            }
            if (chosen_floor_y >= 0.0f) {
                const Float3 eye_pos{wx, chosen_floor_y + 1.65f, wz};
                if (chosen_is_street) {
                    const float c_px = raycast_octree(pkg.octree_nodes, leaf_tri_lists, pkg.vertices, raw_tris, eye_pos, {1.0f, 0.0f, 0.0f}, 4.0f, nullptr);
                    const float c_nx = raycast_octree(pkg.octree_nodes, leaf_tri_lists, pkg.vertices, raw_tris, eye_pos, {-1.0f, 0.0f, 0.0f}, 4.0f, nullptr);
                    const float c_pz = raycast_octree(pkg.octree_nodes, leaf_tri_lists, pkg.vertices, raw_tris, eye_pos, {0.0f, 0.0f, 1.0f}, 4.0f, nullptr);
                    const float c_nz = raycast_octree(pkg.octree_nodes, leaf_tri_lists, pkg.vertices, raw_tris, eye_pos, {0.0f, 0.0f, -1.0f}, 4.0f, nullptr);
                    const float min_clear = std::min(std::min(c_px, c_nx), std::min(c_pz, c_nz));
                    const bool valid_street = (min_clear >= 1.45f);
                    candidate_probe_positions.push_back(eye_pos);
                    candidate_walkable.push_back(valid_street);
                } else {
                    candidate_probe_positions.push_back(eye_pos);
                    candidate_walkable.push_back(false);
                }
            }
        }
    }

    if (candidate_probe_positions.empty()) {
        candidate_probe_positions.push_back({
            0.5f * (pkg.world_min.x + pkg.world_max.x),
            pkg.world_min.y + 3.0f,
            0.5f * (pkg.world_min.z + pkg.world_max.z)});
        candidate_walkable.push_back(true);
    }
    if (candidate_probe_positions.size() > k_max_octree_probes) {
        candidate_probe_positions.resize(k_max_octree_probes);
        candidate_walkable.resize(k_max_octree_probes);
    }

    pkg.probes.resize(candidate_probe_positions.size());
    static const std::array<Float3, 18> k_probe_rays = {{
        { 1.0f,  0.0f,  0.0f}, {-1.0f,  0.0f,  0.0f},
        { 0.0f,  1.0f,  0.0f}, { 0.0f, -1.0f,  0.0f},
        { 0.0f,  0.0f,  1.0f}, { 0.0f,  0.0f, -1.0f},
        { 0.707f,  0.707f,  0.0f}, {-0.707f,  0.707f,  0.0f},
        { 0.0f,  0.707f,  0.707f}, { 0.0f,  0.707f, -0.707f},
        { 0.577f,  0.577f,  0.577f}, {-0.577f,  0.577f,  0.577f},
        { 0.577f,  0.577f, -0.577f}, {-0.577f,  0.577f, -0.577f},
        { 0.707f,  0.0f,  0.707f}, {-0.707f,  0.0f,  0.707f},
        { 0.707f,  0.0f, -0.707f}, {-0.707f,  0.0f, -0.707f},
    }};

    EngineJobSystem::global().parallel_for(candidate_probe_positions.size(), 4U, [&](std::size_t begin, std::size_t end) {
        for (std::size_t pi = begin; pi < end; ++pi) {
            const Float3 pos = candidate_probe_positions[pi];
            float sky_hits = 0.0f;
            float up_rays = 0.0f;
            float dir_dist[6] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
            for (std::size_t ri = 0; ri < k_probe_rays.size(); ++ri) {
                const Float3 rd = v3_norm(k_probe_rays[ri]);
                const float d = raycast_octree(pkg.octree_nodes, leaf_tri_lists, pkg.vertices, raw_tris, pos, rd, 32.0f, nullptr);
                if (ri < 6U) {
                    dir_dist[ri] = std::clamp(d / 24.0f, 0.0f, 1.0f);
                }
                if (rd.y > 0.1f) {
                    up_rays += 1.0f;
                    if (d >= 31.5f) {
                        sky_hits += 1.0f;
                    }
                }
            }
            const float sky_open = up_rays > 0.0f ? (sky_hits / up_rays) : 0.5f;
            OctreeVisProbeGpu pr{};
            pr.pos_radius[0] = pos.x;
            pr.pos_radius[1] = pos.y;
            pr.pos_radius[2] = pos.z;
            pr.pos_radius[3] = 14.0f;
            pr.sky_bounce[0] = 0.34f + 0.48f * sky_open;
            pr.sky_bounce[1] = 0.30f + 0.42f * sky_open;
            pr.sky_bounce[2] = 0.24f + 0.34f * sky_open;
            pr.sky_bounce[3] = sky_open;
            pr.dir_vis_pos[0] = dir_dist[0];
            pr.dir_vis_pos[1] = dir_dist[2];
            pr.dir_vis_pos[2] = dir_dist[4];
            pr.dir_vis_pos[3] = 1.0f;
            pr.dir_vis_neg[0] = dir_dist[1];
            pr.dir_vis_neg[1] = dir_dist[3];
            pr.dir_vis_neg[2] = dir_dist[5];
            pr.dir_vis_neg[3] = 1.0f;
            pkg.probes[pi] = pr;
        }
    });

    for (auto& n : pkg.octree_nodes) {
        float best_d2 = std::numeric_limits<float>::max();
        std::uint32_t best_p = 0;
        for (std::uint32_t pi = 0; pi < static_cast<std::uint32_t>(pkg.probes.size()); ++pi) {
            const auto& pr = pkg.probes[pi];
            const float dx = n.center.x - pr.pos_radius[0];
            const float dy = n.center.y - pr.pos_radius[1];
            const float dz = n.center.z - pr.pos_radius[2];
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 < best_d2) {
                best_d2 = d2;
                best_p = pi;
            }
        }
        n.probe_index = best_p;
    }

    std::vector<std::pair<float, std::uint32_t>> scored_probes;
    scored_probes.reserve(pkg.probes.size());
    const Float3 map_center = v3_scale(v3_add(pkg.world_min, pkg.world_max), 0.5f);
    for (std::uint32_t pi = 0; pi < static_cast<std::uint32_t>(pkg.probes.size()); ++pi) {
        const auto& pr = pkg.probes[pi];
        const bool street = (pi < candidate_walkable.size()) && candidate_walkable[pi];
        const float horiz_open = pr.dir_vis_pos[0] + pr.dir_vis_neg[0] + pr.dir_vis_pos[2] + pr.dir_vis_neg[2];
        const float cd = std::hypot(pr.pos_radius[0] - map_center.x, pr.pos_radius[2] - map_center.z);
        const float wall_framing = (horiz_open > 0.45f && horiz_open < 3.2f) ? 2.0f : 0.4f;
        const float score = (street ? 20.0f : 0.0f) + wall_framing + horiz_open * 1.4f + pr.sky_bounce[3] * 1.2f - cd * 0.04f - pr.pos_radius[1] * 0.35f;
        scored_probes.push_back({score, pi});
    }
    std::sort(scored_probes.begin(), scored_probes.end(), [](const auto& a, const auto& b) { return a.first > b.first; });

    const auto best_spawn_idx = scored_probes.empty() ? 0U : scored_probes.front().second;
    pkg.spawn_pos = {
        pkg.probes[best_spawn_idx].pos_radius[0],
        pkg.probes[best_spawn_idx].pos_radius[1],
        pkg.probes[best_spawn_idx].pos_radius[2]};
    {
        const auto& sp_pr = pkg.probes[best_spawn_idx];
        const Float3 dirs[4] = {{1.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f}};
        const float vis[4] = {sp_pr.dir_vis_pos[0], sp_pr.dir_vis_neg[0], sp_pr.dir_vis_pos[2], sp_pr.dir_vis_neg[2]};
        int best_d = 0;
        for (int di = 1; di < 4; ++di) {
            if (vis[di] > vis[best_d]) best_d = di;
        }
        pkg.spawn_yaw = std::atan2(dirs[best_d].x, dirs[best_d].z);
    }

    pkg.tour_waypoints.clear();
    std::vector<Float3> raw_wps;
    for (const auto& sp : scored_probes) {
        const auto& pr = pkg.probes[sp.second];
        const Float3 wp{pr.pos_radius[0], pr.pos_radius[1], pr.pos_radius[2]};
        bool far_enough = true;
        for (const auto& existing : raw_wps) {
            if (v3_len(v3_sub(wp, existing)) < 9.0f) {
                far_enough = false;
                break;
            }
        }
        if (far_enough) {
            raw_wps.push_back(wp);
            if (raw_wps.size() >= 8U) break;
        }
    }
    if (!raw_wps.empty()) {
        std::vector<bool> used(raw_wps.size(), false);
        pkg.tour_waypoints.push_back(raw_wps[0]);
        used[0] = true;
        for (std::size_t k = 1; k < raw_wps.size(); ++k) {
            const Float3 cur = pkg.tour_waypoints.back();
            std::size_t best_j = 0;
            float best_d = std::numeric_limits<float>::max();
            for (std::size_t j = 0; j < raw_wps.size(); ++j) {
                if (used[j]) continue;
                const float d = v3_len(v3_sub(raw_wps[j], cur));
                if (d < best_d) {
                    best_d = d;
                    best_j = j;
                }
            }
            used[best_j] = true;
            pkg.tour_waypoints.push_back(raw_wps[best_j]);
        }
    }
    if (pkg.tour_waypoints.size() < 2U) {
        pkg.tour_waypoints.push_back(pkg.spawn_pos);
        pkg.tour_waypoints.push_back(v3_add(pkg.spawn_pos, {6.0f, 0.0f, 6.0f}));
    }

    pkg.lights.clear();
    pkg.lights.reserve(k_max_clustered_lights);
    pkg.lights.push_back(make_sun_light({0.62f, 0.56f, 0.55f}, {4.5f, 3.90f, 3.05f}));
    static const std::array<Float3, 6> k_dust2_palette = {{
        {24.0f, 18.5f, 11.5f},
        {18.0f, 21.0f, 25.0f},
        {26.0f, 14.0f, 6.0f},
        {9.0f, 20.0f, 24.0f},
        {24.0f, 7.5f, 5.5f},
        {22.0f, 20.5f, 17.0f},
    }};
    for (std::size_t li = 0; li < pkg.probes.size() && pkg.lights.size() < k_max_clustered_lights; ++li) {
        const std::size_t pidx = (li * 3U) % pkg.probes.size();
        const auto& pr = pkg.probes[pidx];
        const bool outdoor = pr.sky_bounce[3] > 0.45f;
        const Float3 col = k_dust2_palette[li % k_dust2_palette.size()];
        const Float3 lpos{pr.pos_radius[0], pr.pos_radius[1] + (outdoor ? 4.2f : 2.1f), pr.pos_radius[2]};
        if ((li % 3U) == 0U) {
            pkg.lights.push_back(make_spot_light(lpos, 22.0f, {0.12f * ((li & 1U) ? 1.0f : -1.0f), -0.96f, 0.14f}, 0.82f, 0.42f, col, 0.09f));
        } else {
            pkg.lights.push_back(make_point_light(lpos, 18.0f, v3_scale(col, 0.65f), 0.16f));
        }
    }
    while (pkg.lights.size() < k_max_clustered_lights) {
        const float ang = static_cast<float>(pkg.lights.size()) * 0.45f;
        const Float3 lp{
            map_center.x + std::cos(ang) * 28.0f,
            pkg.world_min.y + 6.5f,
            map_center.z + std::sin(ang) * 28.0f};
        pkg.lights.push_back(make_point_light(lp, 20.0f, {16.0f, 14.0f, 11.0f}, 0.14f));
    }

    pkg.loaded = true;
    return pkg;
}

void select_dust2_octree_draws(
    const Dust2MapPackage& map,
    const CameraBasis& camera,
    float aspect_ratio,
    std::vector<Dust2DrawChunk>& out_chunks,
    Dust2OctreeTelemetry& out_telemetry) {
    out_chunks.clear();
    out_telemetry = {};
    if (!map.loaded || map.octree_nodes.empty()) {
        return;
    }

    out_telemetry.total_nodes = static_cast<std::uint32_t>(map.octree_nodes.size());
    out_telemetry.leaf_nodes = map.leaf_count;
    out_telemetry.total_lod0_triangles = map.total_lod0_triangles;
    out_telemetry.active_probes = static_cast<std::uint32_t>(map.probes.size());
    out_telemetry.active_lights = static_cast<std::uint32_t>(map.lights.size());

    const auto planes = build_camera_frustum(camera, aspect_ratio, 0.04f, 180.0f);
    const float tan_y = std::max(camera.tan_half_fov_y, 0.1f);

    std::uint32_t cam_probe_idx = 0;
    float cam_probe_d2 = std::numeric_limits<float>::max();
    for (std::uint32_t pi = 0; pi < static_cast<std::uint32_t>(map.probes.size()); ++pi) {
        const auto& pr = map.probes[pi];
        const float dx = camera.position.x - pr.pos_radius[0];
        const float dy = camera.position.y - pr.pos_radius[1];
        const float dz = camera.position.z - pr.pos_radius[2];
        const float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < cam_probe_d2) {
            cam_probe_d2 = d2;
            cam_probe_idx = pi;
        }
    }
    const OctreeVisProbeGpu* cam_probe = map.probes.empty() ? nullptr : &map.probes[cam_probe_idx];

    std::int32_t stack[128];
    int sp = 0;
    stack[sp++] = 0;
    while (sp > 0) {
        const std::int32_t ni = stack[--sp];
        const auto& node = map.octree_nodes[static_cast<std::size_t>(ni)];
        ++out_telemetry.visited_nodes;

        if (!aabb_in_frustum(planes, node.min_bounds, node.max_bounds)) {
            ++out_telemetry.culled_nodes;
            continue;
        }

        const Float3 to_node = v3_sub(node.center, camera.position);
        const float dist = std::max(v3_len(to_node), 0.25f);
        if (cam_probe != nullptr && dist > 38.0f && node.is_leaf) {
            const Float3 dir = v3_scale(to_node, 1.0f / dist);
            const float vis_x = dir.x >= 0.0f ? cam_probe->dir_vis_pos[0] : cam_probe->dir_vis_neg[0];
            const float vis_z = dir.z >= 0.0f ? cam_probe->dir_vis_pos[2] : cam_probe->dir_vis_neg[2];
            const float dir_vis = std::fabs(dir.x) * vis_x + std::fabs(dir.z) * vis_z;
            if (dir_vis < 0.16f && cam_probe->sky_bounce[3] < 0.35f) {
                ++out_telemetry.probe_occluded_nodes;
                continue;
            }
        }

        if (node.is_leaf) {
            const float eff_dist = std::max(dist - node.radius, 0.5f);
            const float proj_size = node.radius / (eff_dist * tan_y);
            std::uint32_t lod = 0;
            if (proj_size < 0.11f) {
                lod = 2;
            } else if (proj_size < 0.28f) {
                lod = 1;
            }
            const Dust2LodSlice& slice = node.lods[lod];
            if (slice.index_count > 0U) {
                out_chunks.push_back({slice.index_offset, slice.index_count, lod, static_cast<std::uint32_t>(ni)});
                out_telemetry.submitted_triangles += slice.index_count / 3U;
                if (lod == 0U) ++out_telemetry.lod0_chunks;
                else if (lod == 1U) ++out_telemetry.lod1_chunks;
                else ++out_telemetry.lod2_chunks;
            }
        } else {
            for (int c = 0; c < 8; ++c) {
                if (node.children[c] >= 0 && sp < 128) {
                    stack[sp++] = node.children[c];
                }
            }
        }
    }
}

CameraBasis sample_dust2_camera_path(
    const Dust2MapPackage& map,
    CameraPath path,
    double time_seconds,
    float vertical_fov_radians) {
    Float3 pos = map.spawn_pos;
    float yaw = map.spawn_yaw;
    float pitch = -0.04f;
    const float t = static_cast<float>(time_seconds);
    const Float3 center = v3_scale(v3_add(map.world_min, map.world_max), 0.5f);

    if (map.tour_waypoints.size() >= 2U) {
        const std::size_t n = map.tour_waypoints.size();
        const double seg_f = std::fmod(std::max(0.0, time_seconds) * 0.18, static_cast<double>(n));
        const std::size_t i0 = static_cast<std::size_t>(seg_f) % n;
        const std::size_t i1 = (i0 + 1U) % n;
        const float u = static_cast<float>(seg_f - std::floor(seg_f));
        const float su = u * u * (3.0f - 2.0f * u);
        if (path == CameraPath::corridor_sway) {
            pos = v3_add(v3_scale(map.tour_waypoints[i0], 1.0f - su), v3_scale(map.tour_waypoints[i1], su));
            const Float3 look_dir = v3_sub(map.tour_waypoints[i1], map.tour_waypoints[i0]);
            yaw = std::atan2(look_dir.x, look_dir.z) + std::sin(t * 0.85f) * 0.28f;
            pitch = -0.05f + std::sin(t * 0.55f) * 0.04f;
        } else if (path == CameraPath::fast_pan) {
            pos = v3_add(map.spawn_pos, {std::sin(t * 0.7f) * 2.2f, 0.25f, std::cos(t * 0.7f) * 2.2f});
            yaw = map.spawn_yaw + std::sin(t * 2.1f) * 0.95f;
            pitch = -0.04f + std::cos(t * 1.4f) * 0.08f;
        } else if (path == CameraPath::occlusion_sweep) {
            pos = v3_add(v3_scale(map.tour_waypoints[i0], 1.0f - su), v3_scale(map.tour_waypoints[i1], su));
            pos.x += std::sin(t * 1.55f) * 2.8f;
            const Float3 to_c = v3_sub(center, pos);
            yaw = std::atan2(to_c.x, to_c.z) + std::sin(t * 1.15f) * 0.42f;
            pitch = -0.06f;
        } else if (path == CameraPath::pure_rotation) {
            pos = map.spawn_pos;
            yaw = map.spawn_yaw + t * 0.65f;
            pitch = -0.03f;
        } else if (path == CameraPath::camera_cut) {
            const std::size_t cut_i = static_cast<std::size_t>(std::floor(std::max(0.0, time_seconds) / 1.4)) % n;
            pos = map.tour_waypoints[cut_i];
            const Float3 to_c = v3_sub(center, pos);
            yaw = std::atan2(to_c.x, to_c.z);
            pitch = -0.05f;
        }
    }

    const float cy = std::cos(yaw);
    const float sy = std::sin(yaw);
    const float cp = std::cos(pitch);
    const float sp = std::sin(pitch);
    const Float3 fwd = v3_norm({sy * cp, sp, cy * cp}, {0.0f, 0.0f, 1.0f});
    const Float3 right = v3_norm(v3_cross({0.0f, 1.0f, 0.0f}, fwd), {1.0f, 0.0f, 0.0f});
    const Float3 up = v3_norm(v3_cross(fwd, right), {0.0f, 1.0f, 0.0f});
    return CameraBasis{
        pos,
        right,
        up,
        fwd,
        std::tan(vertical_fov_radians * 0.5f),
    };
}

}
