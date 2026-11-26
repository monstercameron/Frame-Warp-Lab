#include "gltf_loader.h"

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace frame_warp_lab {

namespace {

std::filesystem::path executable_directory() {
    wchar_t module_path[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(nullptr, module_path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return std::filesystem::current_path();
    }
    return std::filesystem::path(module_path).parent_path();
}

Float3 remap_vec3(float gx, float gy, float gz, bool x_forward_to_z_forward) {
    if (x_forward_to_z_forward) {
        return {gz, gy, gx};
    }
    return {gx, gy, gz};
}

Float3 normalize_or(Float3 v, Float3 fallback) {
    const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len < 1e-6f) {
        return fallback;
    }
    return {v.x / len, v.y / len, v.z / len};
}

void compute_smooth_tangents(std::vector<RangeVertex>& vertices, const std::vector<std::uint32_t>& indices) {
    std::vector<Float3> tan_accum(vertices.size(), Float3{0.0f, 0.0f, 0.0f});
    for (std::size_t i = 0; i + 2U < indices.size(); i += 3U) {
        const std::uint32_t i0 = indices[i + 0U];
        const std::uint32_t i1 = indices[i + 1U];
        const std::uint32_t i2 = indices[i + 2U];
        if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) {
            continue;
        }
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
        if (std::fabs(det) < 1e-8f) {
            continue;
        }
        const float inv_det = 1.0f / det;
        const Float3 t{
            (dx1 * dv2 - dx2 * dv1) * inv_det,
            (dy1 * dv2 - dy2 * dv1) * inv_det,
            (dz1 * dv2 - dz2 * dv1) * inv_det};
        tan_accum[i0] = {tan_accum[i0].x + t.x, tan_accum[i0].y + t.y, tan_accum[i0].z + t.z};
        tan_accum[i1] = {tan_accum[i1].x + t.x, tan_accum[i1].y + t.y, tan_accum[i1].z + t.z};
        tan_accum[i2] = {tan_accum[i2].x + t.x, tan_accum[i2].y + t.y, tan_accum[i2].z + t.z};
    }

    for (std::size_t i = 0; i < vertices.size(); ++i) {
        const Float3 n = normalize_or({vertices[i].nx, vertices[i].ny, vertices[i].nz}, {0.0f, 1.0f, 0.0f});
        const Float3 fallback = std::fabs(n.y) < 0.9f ? Float3{0.0f, 1.0f, 0.0f} : Float3{1.0f, 0.0f, 0.0f};
        Float3 t = tan_accum[i];
        const float d = t.x * n.x + t.y * n.y + t.z * n.z;
        t = {t.x - n.x * d, t.y - n.y * d, t.z - n.z * d};
        t = normalize_or(t, fallback);
        vertices[i].nx = n.x;
        vertices[i].ny = n.y;
        vertices[i].nz = n.z;
        vertices[i].tx = t.x;
        vertices[i].ty = t.y;
        vertices[i].tz = t.z;
    }
}

}

std::filesystem::path resolve_asset_path(const std::filesystem::path& relative_path) {
    const std::filesystem::path exe_dir = executable_directory();
    const std::array<std::filesystem::path, 5> candidates = {{
        exe_dir / relative_path,
        std::filesystem::current_path() / relative_path,
        exe_dir.parent_path() / relative_path,
        exe_dir.parent_path().parent_path() / relative_path,
        exe_dir.parent_path().parent_path().parent_path() / relative_path,
    }};
    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
    }
    return exe_dir / relative_path;
}

LoadedGltfAsset load_gltf_meshes(
    const std::filesystem::path& relative_gltf_path,
    std::uint32_t material_id,
    bool gltf_x_forward_to_engine_z_forward,
    bool bake_node_translation) {
    LoadedGltfAsset asset{};
    const std::filesystem::path resolved = resolve_asset_path(relative_gltf_path);
    std::error_code ec;
    if (!std::filesystem::exists(resolved, ec)) {
        return asset;
    }
    const std::string path_utf8 = resolved.string();
    cgltf_options options{};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&options, path_utf8.c_str(), &data) != cgltf_result_success || data == nullptr) {
        return asset;
    }
    if (cgltf_load_buffers(&options, data, path_utf8.c_str()) != cgltf_result_success) {
        cgltf_free(data);
        return asset;
    }

    asset.loaded = true;
    asset.source_path = path_utf8;

    for (cgltf_size node_idx = 0; node_idx < data->nodes_count; ++node_idx) {
        const cgltf_node& node = data->nodes[node_idx];
        if (node.mesh == nullptr) {
            continue;
        }
        LoadedGltfMesh out_mesh{};
        out_mesh.node_name = node.name != nullptr ? std::string(node.name) : ("node_" + std::to_string(node_idx));
        const Float3 node_t = node.has_translation
            ? remap_vec3(node.translation[0], node.translation[1], node.translation[2], gltf_x_forward_to_engine_z_forward)
            : Float3{0.0f, 0.0f, 0.0f};
        out_mesh.node_translation = node_t;
        const Float3 baked_offset = bake_node_translation ? node_t : Float3{0.0f, 0.0f, 0.0f};

        Float3 bmin{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
        Float3 bmax{-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};

        for (cgltf_size prim_idx = 0; prim_idx < node.mesh->primitives_count; ++prim_idx) {
            const cgltf_primitive& prim = node.mesh->primitives[prim_idx];
            if (prim.type != cgltf_primitive_type_triangles) {
                continue;
            }
            const cgltf_accessor* pos_acc = nullptr;
            const cgltf_accessor* norm_acc = nullptr;
            const cgltf_accessor* uv_acc = nullptr;
            for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
                const cgltf_attribute& attr = prim.attributes[a];
                if (attr.type == cgltf_attribute_type_position) pos_acc = attr.data;
                else if (attr.type == cgltf_attribute_type_normal) norm_acc = attr.data;
                else if (attr.type == cgltf_attribute_type_texcoord && attr.index == 0) uv_acc = attr.data;
            }
            if (pos_acc == nullptr || pos_acc->count == 0) {
                continue;
            }

            const auto base_vertex = static_cast<std::uint32_t>(out_mesh.vertices.size());
            out_mesh.vertices.resize(base_vertex + static_cast<std::uint32_t>(pos_acc->count));
            for (cgltf_size v = 0; v < pos_acc->count; ++v) {
                float p[3] = {0.0f, 0.0f, 0.0f};
                float n[3] = {0.0f, 1.0f, 0.0f};
                float uv[2] = {0.0f, 0.0f};
                cgltf_accessor_read_float(pos_acc, v, p, 3);
                if (norm_acc != nullptr) cgltf_accessor_read_float(norm_acc, v, n, 3);
                if (uv_acc != nullptr) cgltf_accessor_read_float(uv_acc, v, uv, 2);

                const Float3 rp = remap_vec3(p[0], p[1], p[2], gltf_x_forward_to_engine_z_forward);
                const Float3 rn = remap_vec3(n[0], n[1], n[2], gltf_x_forward_to_engine_z_forward);
                RangeVertex vert{};
                vert.px = rp.x + baked_offset.x;
                vert.py = rp.y + baked_offset.y;
                vert.pz = rp.z + baked_offset.z;
                vert.nx = rn.x;
                vert.ny = rn.y;
                vert.nz = rn.z;
                vert.tx = 1.0f;
                vert.ty = 0.0f;
                vert.tz = 0.0f;
                vert.u = uv[0];
                vert.v = uv[1];
                vert.material_id = material_id;
                vert.emissive_weight = 0.0f;
                out_mesh.vertices[base_vertex + static_cast<std::uint32_t>(v)] = vert;

                bmin.x = std::min(bmin.x, vert.px);
                bmin.y = std::min(bmin.y, vert.py);
                bmin.z = std::min(bmin.z, vert.pz);
                bmax.x = std::max(bmax.x, vert.px);
                bmax.y = std::max(bmax.y, vert.py);
                bmax.z = std::max(bmax.z, vert.pz);
            }

            if (prim.indices != nullptr) {
                const cgltf_size ic = prim.indices->count;
                for (cgltf_size idx = 0; idx + 2U < ic; idx += 3U) {
                    const auto i0 = base_vertex + static_cast<std::uint32_t>(cgltf_accessor_read_index(prim.indices, idx + 0U));
                    const auto i1 = base_vertex + static_cast<std::uint32_t>(cgltf_accessor_read_index(prim.indices, idx + 1U));
                    const auto i2 = base_vertex + static_cast<std::uint32_t>(cgltf_accessor_read_index(prim.indices, idx + 2U));
                    if (gltf_x_forward_to_engine_z_forward) {
                        out_mesh.indices.push_back(i0);
                        out_mesh.indices.push_back(i2);
                        out_mesh.indices.push_back(i1);
                    } else {
                        out_mesh.indices.push_back(i0);
                        out_mesh.indices.push_back(i1);
                        out_mesh.indices.push_back(i2);
                    }
                }
            } else {
                for (cgltf_size idx = 0; idx + 2U < pos_acc->count; idx += 3U) {
                    const auto i0 = base_vertex + static_cast<std::uint32_t>(idx + 0U);
                    const auto i1 = base_vertex + static_cast<std::uint32_t>(idx + 1U);
                    const auto i2 = base_vertex + static_cast<std::uint32_t>(idx + 2U);
                    if (gltf_x_forward_to_engine_z_forward) {
                        out_mesh.indices.push_back(i0);
                        out_mesh.indices.push_back(i2);
                        out_mesh.indices.push_back(i1);
                    } else {
                        out_mesh.indices.push_back(i0);
                        out_mesh.indices.push_back(i1);
                        out_mesh.indices.push_back(i2);
                    }
                }
            }
        }

        if (!out_mesh.vertices.empty() && !out_mesh.indices.empty()) {
            compute_smooth_tangents(out_mesh.vertices, out_mesh.indices);
            out_mesh.min_bounds = bmin;
            out_mesh.max_bounds = bmax;
            asset.meshes.push_back(std::move(out_mesh));
        }
    }

    cgltf_free(data);
    return asset;
}

}
