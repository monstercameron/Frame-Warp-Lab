#include "range_mesh.h"
#include "gltf_loader.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace frame_warp_lab {

namespace {

constexpr float k_pi = 3.14159265358979323846f;

struct MeshBuilder {
    std::vector<RangeVertex>& verts;
    std::vector<std::uint32_t>& indices;

    void add_quad(
        Float3 p0, Float3 p1, Float3 p2, Float3 p3,
        Float3 n, Float3 t,
        float u0, float v0, float u1, float v1,
        std::uint32_t mat_id,
        float emissive = 0.0f) {
        const auto base = static_cast<std::uint32_t>(verts.size());
        verts.push_back({p0.x, p0.y, p0.z, n.x, n.y, n.z, t.x, t.y, t.z, u0, v0, mat_id, emissive});
        verts.push_back({p1.x, p1.y, p1.z, n.x, n.y, n.z, t.x, t.y, t.z, u1, v0, mat_id, emissive});
        verts.push_back({p2.x, p2.y, p2.z, n.x, n.y, n.z, t.x, t.y, t.z, u1, v1, mat_id, emissive});
        verts.push_back({p3.x, p3.y, p3.z, n.x, n.y, n.z, t.x, t.y, t.z, u0, v1, mat_id, emissive});
        indices.push_back(base + 0U);
        indices.push_back(base + 1U);
        indices.push_back(base + 2U);
        indices.push_back(base + 0U);
        indices.push_back(base + 2U);
        indices.push_back(base + 3U);
    }

    void add_box(
        Float3 center,
        Float3 half,
        std::uint32_t mat_id,
        float uv_scale_u = 1.0f,
        float uv_scale_v = 1.0f,
        bool unit_uv = false,
        float emissive = 0.0f) {
        const float x0 = center.x - half.x;
        const float x1 = center.x + half.x;
        const float y0 = center.y - half.y;
        const float y1 = center.y + half.y;
        const float z0 = center.z - half.z;
        const float z1 = center.z + half.z;
        const float sx = unit_uv ? 1.0f : (half.x * 2.0f * uv_scale_u);
        const float sy = unit_uv ? 1.0f : (half.y * 2.0f * uv_scale_v);
        const float sz = unit_uv ? 1.0f : (half.z * 2.0f * uv_scale_u);

        add_quad({x0, y0, z0}, {x0, y1, z0}, {x1, y1, z0}, {x1, y0, z0}, {0, 0, -1}, {1, 0, 0}, 0, 0, sx, sy, mat_id, emissive);
        add_quad({x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}, {x0, y0, z1}, {0, 0, 1}, {-1, 0, 0}, 0, 0, sx, sy, mat_id, emissive);
        add_quad({x0, y0, z1}, {x0, y1, z1}, {x0, y1, z0}, {x0, y0, z0}, {-1, 0, 0}, {0, 0, -1}, 0, 0, sz, sy, mat_id, emissive);
        add_quad({x1, y0, z0}, {x1, y1, z0}, {x1, y1, z1}, {x1, y0, z1}, {1, 0, 0}, {0, 0, 1}, 0, 0, sz, sy, mat_id, emissive);
        add_quad({x0, y1, z0}, {x0, y1, z1}, {x1, y1, z1}, {x1, y1, z0}, {0, 1, 0}, {1, 0, 0}, 0, 0, sx, sz, mat_id, emissive);
        add_quad({x0, y0, z1}, {x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {0, -1, 0}, {1, 0, 0}, 0, 0, sx, sz, mat_id, emissive);
    }

    void add_slanted_box(
        Float3 bottom_center,
        Float3 top_center,
        float half_w,
        float half_d,
        std::uint32_t mat_id,
        float emissive = 0.0f) {
        const Float3 b0{bottom_center.x - half_w, bottom_center.y, bottom_center.z - half_d};
        const Float3 b1{bottom_center.x + half_w, bottom_center.y, bottom_center.z - half_d};
        const Float3 b2{bottom_center.x + half_w, bottom_center.y, bottom_center.z + half_d};
        const Float3 b3{bottom_center.x - half_w, bottom_center.y, bottom_center.z + half_d};
        const Float3 t0{top_center.x - half_w, top_center.y, top_center.z - half_d};
        const Float3 t1{top_center.x + half_w, top_center.y, top_center.z - half_d};
        const Float3 t2{top_center.x + half_w, top_center.y, top_center.z + half_d};
        const Float3 t3{top_center.x - half_w, top_center.y, top_center.z + half_d};

        add_quad(b0, t0, t1, b1, {0, -0.25f, -0.968f}, {1, 0, 0}, 0, 0, 1, 1, mat_id, emissive);
        add_quad(b2, t2, t3, b3, {0, 0.25f, 0.968f}, {-1, 0, 0}, 0, 0, 1, 1, mat_id, emissive);
        add_quad(b3, t3, t0, b0, {-1, 0, 0}, {0, 0, -1}, 0, 0, 1, 1, mat_id, emissive);
        add_quad(b1, t1, t2, b2, {1, 0, 0}, {0, 0, 1}, 0, 0, 1, 1, mat_id, emissive);
        add_quad(t0, t3, t2, t1, {0, 1, 0}, {1, 0, 0}, 0, 0, 1, 1, mat_id, emissive);
        add_quad(b3, b0, b1, b2, {0, -1, 0}, {1, 0, 0}, 0, 0, 1, 1, mat_id, emissive);
    }

    void add_cylinder_z(
        Float3 center,
        float radius,
        float half_len,
        int segments,
        std::uint32_t mat_id,
        float emissive = 0.0f) {
        const float z0 = center.z - half_len;
        const float z1 = center.z + half_len;
        for (int i = 0; i < segments; ++i) {
            const float a0 = (static_cast<float>(i) / static_cast<float>(segments)) * 2.0f * k_pi;
            const float a1 = (static_cast<float>(i + 1) / static_cast<float>(segments)) * 2.0f * k_pi;
            const float c0 = std::cos(a0);
            const float s0 = std::sin(a0);
            const float c1 = std::cos(a1);
            const float s1 = std::sin(a1);
            const float am = 0.5f * (a0 + a1);
            const Float3 n{std::cos(am), std::sin(am), 0.0f};
            const Float3 t{-std::sin(am), std::cos(am), 0.0f};
            add_quad(
                {center.x + c0 * radius, center.y + s0 * radius, z0},
                {center.x + c1 * radius, center.y + s1 * radius, z0},
                {center.x + c1 * radius, center.y + s1 * radius, z1},
                {center.x + c0 * radius, center.y + s0 * radius, z1},
                n,
                t,
                static_cast<float>(i) / static_cast<float>(segments),
                0.0f,
                static_cast<float>(i + 1) / static_cast<float>(segments),
                1.0f,
                mat_id,
                emissive);
        }
    }

    void add_cylinder_y(
        Float3 center,
        float radius,
        float half_h,
        int segments,
        std::uint32_t mat_id,
        float emissive = 0.0f) {
        const float y0 = center.y - half_h;
        const float y1 = center.y + half_h;
        for (int i = 0; i < segments; ++i) {
            const float a0 = (static_cast<float>(i) / static_cast<float>(segments)) * 2.0f * k_pi;
            const float a1 = (static_cast<float>(i + 1) / static_cast<float>(segments)) * 2.0f * k_pi;
            const float c0 = std::cos(a0);
            const float s0 = std::sin(a0);
            const float c1 = std::cos(a1);
            const float s1 = std::sin(a1);
            const float am = 0.5f * (a0 + a1);
            const Float3 n{std::cos(am), 0.0f, std::sin(am)};
            const Float3 t{-std::sin(am), 0.0f, std::cos(am)};
            add_quad(
                {center.x + c0 * radius, y0, center.z + s0 * radius},
                {center.x + c0 * radius, y1, center.z + s0 * radius},
                {center.x + c1 * radius, y1, center.z + s1 * radius},
                {center.x + c1 * radius, y0, center.z + s1 * radius},
                n,
                t,
                static_cast<float>(i) / static_cast<float>(segments),
                0.0f,
                static_cast<float>(i + 1) / static_cast<float>(segments),
                1.0f,
                mat_id,
                emissive);
        }
    }

    void append_loaded_mesh(const LoadedGltfMesh& mesh, float uniform_scale) {
        const auto base = static_cast<std::uint32_t>(verts.size());
        verts.reserve(verts.size() + mesh.vertices.size());
        for (const auto& src : mesh.vertices) {
            RangeVertex v = src;
            v.px *= uniform_scale;
            v.py *= uniform_scale;
            v.pz *= uniform_scale;
            verts.push_back(v);
        }
        indices.reserve(indices.size() + mesh.indices.size());
        for (std::uint32_t idx : mesh.indices) {
            indices.push_back(base + idx);
        }
    }
};

constexpr std::array<Float3, k_range_target_count> k_base_positions = {{
    { 0.0f, 0.14f,  9.8f},
    {-2.5f, 0.14f, 10.2f},
    { 2.5f, 0.14f, 10.2f},
    {-3.6f, 0.14f, 15.8f},
    {-1.0f, 0.14f, 16.2f},
    { 2.1f, 0.14f, 16.0f},
    {-2.1f, 0.14f, 22.6f},
    { 2.6f, 0.14f, 22.8f},
}};

const LoadedGltfMesh* find_gltf_node(const LoadedGltfAsset& asset, const char* node_name) {
    for (const auto& m : asset.meshes) {
        if (m.node_name == node_name) {
            return &m;
        }
    }
    return nullptr;
}

}

RangeGeometryPackage build_tactical_range_geometry() {
    RangeGeometryPackage pkg{};
    MeshBuilder mb{pkg.vertices, pkg.indices};

    auto begin_sub = [&](RangeMeshKind kind) -> std::uint32_t {
        const auto start = static_cast<std::uint32_t>(pkg.indices.size());
        pkg.submeshes[static_cast<std::size_t>(kind)].index_offset = start;
        return start;
    };
    auto end_sub = [&](RangeMeshKind kind, std::uint32_t start) {
        pkg.submeshes[static_cast<std::size_t>(kind)].index_count = static_cast<std::uint32_t>(pkg.indices.size()) - start;
    };

    {
        const auto s = begin_sub(RangeMeshKind::static_world);
        mb.add_quad({-6.5f, 0.0f, -2.0f}, {-6.5f, 0.0f, 32.0f}, {6.5f, 0.0f, 32.0f}, {6.5f, 0.0f, -2.0f}, {0, 1, 0}, {1, 0, 0}, 0, 0, 6.5f, 17.0f, 6U);
        mb.add_quad({-6.5f, 4.8f, 32.0f}, {-6.5f, 4.8f, -2.0f}, {6.5f, 4.8f, -2.0f}, {6.5f, 4.8f, 32.0f}, {0, -1, 0}, {1, 0, 0}, 0, 0, 5.0f, 14.0f, 1U);
        mb.add_quad({-6.5f, 0.0f, 32.0f}, {-6.5f, 4.8f, 32.0f}, {-6.5f, 4.8f, -2.0f}, {-6.5f, 0.0f, -2.0f}, {1, 0, 0}, {0, 0, -1}, 0, 0, 12.0f, 2.2f, 7U);
        mb.add_quad({6.5f, 0.0f, -2.0f}, {6.5f, 4.8f, -2.0f}, {6.5f, 4.8f, 32.0f}, {6.5f, 0.0f, 32.0f}, {-1, 0, 0}, {0, 0, 1}, 0, 0, 12.0f, 2.2f, 7U);
        mb.add_quad({-6.5f, 0.0f, 32.0f}, {-6.5f, 4.8f, 32.0f}, {6.5f, 4.8f, 32.0f}, {6.5f, 0.0f, 32.0f}, {0, 0, -1}, {1, 0, 0}, 0, 0, 5.0f, 2.2f, 2U);
        mb.add_quad({6.5f, 0.0f, -2.0f}, {6.5f, 4.8f, -2.0f}, {-6.5f, 4.8f, -2.0f}, {-6.5f, 0.0f, -2.0f}, {0, 0, 1}, {-1, 0, 0}, 0, 0, 5.0f, 2.2f, 7U);

        mb.add_box({0.0f, 0.48f, 3.6f}, {6.4f, 0.48f, 0.35f}, 6U, 0.6f, 0.6f);
        mb.add_box({0.0f, 0.98f, 3.6f}, {6.4f, 0.03f, 0.38f}, 8U, 0.8f, 0.8f);
        mb.add_box({-2.2f, 1.48f, 3.35f}, {0.04f, 0.50f, 0.60f}, 1U, 0.8f, 0.8f);
        mb.add_box({2.2f, 1.48f, 3.35f}, {0.04f, 0.50f, 0.60f}, 1U, 0.8f, 0.8f);

        for (int i = 0; i < 6; ++i) {
            const float z = 4.0f + static_cast<float>(i) * 5.2f;
            mb.add_box({-6.32f, 2.4f, z}, {0.18f, 2.4f, 0.22f}, 1U, 0.5f, 0.5f);
            mb.add_box({6.32f, 2.4f, z}, {0.18f, 2.4f, 0.22f}, 1U, 0.5f, 0.5f);
            mb.add_box({0.0f, 4.62f, z}, {6.45f, 0.16f, 0.16f}, 10U, 0.5f, 0.5f);
            mb.add_box({0.0f, 4.42f, z}, {2.6f, 0.04f, 0.14f}, 1U, 1.0f, 1.0f, true, 3.2f);
        }

        mb.add_box({-3.8f, 0.55f, 12.6f}, {0.65f, 0.55f, 0.65f}, 8U, 1.0f, 1.0f, true);
        mb.add_box({3.8f, 0.55f, 12.8f}, {0.65f, 0.55f, 0.65f}, 8U, 1.0f, 1.0f, true);
        mb.add_box({3.8f, 1.55f, 12.8f}, {0.50f, 0.45f, 0.50f}, 8U, 1.0f, 1.0f, true);
        mb.add_box({0.8f, 0.48f, 13.6f}, {0.55f, 0.48f, 0.55f}, 8U, 1.0f, 1.0f, true);
        mb.add_box({-4.2f, 0.60f, 19.2f}, {0.75f, 0.60f, 0.75f}, 8U, 1.0f, 1.0f, true);
        mb.add_box({4.0f, 0.60f, 19.4f}, {0.75f, 0.60f, 0.75f}, 8U, 1.0f, 1.0f, true);

        mb.add_box({0.0f, 0.18f, 30.8f}, {6.4f, 0.18f, 1.1f}, 0U, 0.5f, 0.5f);
        mb.add_box({0.0f, 0.01f, 10.0f}, {6.3f, 0.01f, 0.05f}, 3U, 0.5f, 0.5f);
        mb.add_box({0.0f, 0.01f, 16.0f}, {6.3f, 0.01f, 0.05f}, 3U, 0.5f, 0.5f);
        mb.add_box({0.0f, 0.01f, 22.7f}, {6.3f, 0.01f, 0.05f}, 3U, 0.5f, 0.5f);

        for (const auto& bp : k_base_positions) {
            mb.add_box({bp.x, 0.06f, bp.z}, {0.34f, 0.06f, 0.24f}, 10U, 1.0f, 1.0f, true);
            mb.add_box({bp.x - 0.24f, 0.14f, bp.z}, {0.04f, 0.07f, 0.06f}, 10U, 1.0f, 1.0f, true);
            mb.add_box({bp.x + 0.24f, 0.14f, bp.z}, {0.04f, 0.07f, 0.06f}, 10U, 1.0f, 1.0f, true);
        }
        end_sub(RangeMeshKind::static_world, s);
    }

    {
        const auto s = begin_sub(RangeMeshKind::target_plate);
        mb.add_box({0.0f, 0.12f, 0.0f}, {0.10f, 0.12f, 0.025f}, 10U, 1.0f, 1.0f, true);
        mb.add_box({0.0f, 0.64f, 0.0f}, {0.27f, 0.42f, 0.024f}, 9U, 1.0f, 1.0f, true);
        mb.add_box({0.0f, 1.24f, 0.0f}, {0.13f, 0.17f, 0.024f}, 9U, 1.0f, 1.0f, true);
        mb.add_box({0.0f, 0.64f, 0.028f}, {0.06f, 0.58f, 0.014f}, 10U, 1.0f, 1.0f, true);
        end_sub(RangeMeshKind::target_plate, s);
    }

    const LoadedGltfAsset pistol_gltf = load_gltf_meshes(
        "assets/models/service_pistol/service_pistol.gltf",
        12U,
        true,
        false);
    const LoadedGltfMesh* frame_mesh = pistol_gltf.loaded ? find_gltf_node(pistol_gltf, "service_pistol_pistol_a") : nullptr;
    const LoadedGltfMesh* slide_mesh = pistol_gltf.loaded ? find_gltf_node(pistol_gltf, "service_pistol_slide_a") : nullptr;
    const LoadedGltfMesh* hammer_mesh = pistol_gltf.loaded ? find_gltf_node(pistol_gltf, "service_pistol_hammer_a") : nullptr;
    const LoadedGltfMesh* trigger_mesh = pistol_gltf.loaded ? find_gltf_node(pistol_gltf, "service_pistol_trigger_a") : nullptr;
    const LoadedGltfMesh* mag_mesh = pistol_gltf.loaded ? find_gltf_node(pistol_gltf, "service_pistol_magazine_loaded") : nullptr;

    if (frame_mesh != nullptr && slide_mesh != nullptr) {
        constexpr float pistol_scale = 1.25f;
        pkg.hero_pistol_gltf_loaded = true;
        pkg.pistol_slide_offset = {
            slide_mesh->node_translation.x * pistol_scale,
            slide_mesh->node_translation.y * pistol_scale,
            slide_mesh->node_translation.z * pistol_scale};
        pkg.pistol_hammer_offset = hammer_mesh != nullptr
            ? Float3{hammer_mesh->node_translation.x * pistol_scale, hammer_mesh->node_translation.y * pistol_scale, hammer_mesh->node_translation.z * pistol_scale}
            : Float3{0.0f, 0.061f, -0.036f};
        pkg.pistol_trigger_offset = trigger_mesh != nullptr
            ? Float3{trigger_mesh->node_translation.x * pistol_scale, trigger_mesh->node_translation.y * pistol_scale, trigger_mesh->node_translation.z * pistol_scale}
            : Float3{0.0f, 0.044f, 0.064f};
        pkg.pistol_magazine_offset = {0.0f, -0.008f * pistol_scale, -0.012f * pistol_scale};
        pkg.pistol_muzzle_offset = {0.0f, 0.064f * pistol_scale, frame_mesh->max_bounds.z * pistol_scale + 0.012f};
        pkg.pistol_eject_offset = {0.022f * pistol_scale, (slide_mesh->node_translation.y + 0.006f) * pistol_scale, (slide_mesh->node_translation.z + 0.018f) * pistol_scale};
        const float top_sight_y = (slide_mesh->node_translation.y + slide_mesh->max_bounds.y) * pistol_scale;
        pkg.pistol_ads_sight_y = top_sight_y - 0.0025f;

        {
            const auto s = begin_sub(RangeMeshKind::pistol_frame);
            mb.append_loaded_mesh(*frame_mesh, pistol_scale);
            end_sub(RangeMeshKind::pistol_frame, s);
        }
        {
            const auto s = begin_sub(RangeMeshKind::pistol_slide);
            mb.append_loaded_mesh(*slide_mesh, pistol_scale);
            end_sub(RangeMeshKind::pistol_slide, s);
        }
        {
            const auto s = begin_sub(RangeMeshKind::pistol_barrel);
            end_sub(RangeMeshKind::pistol_barrel, s);
        }
        {
            const auto s = begin_sub(RangeMeshKind::pistol_magazine);
            if (mag_mesh != nullptr) {
                mb.append_loaded_mesh(*mag_mesh, pistol_scale);
            }
            end_sub(RangeMeshKind::pistol_magazine, s);
        }
        {
            const auto s = begin_sub(RangeMeshKind::pistol_hammer);
            if (hammer_mesh != nullptr) {
                mb.append_loaded_mesh(*hammer_mesh, pistol_scale);
            }
            end_sub(RangeMeshKind::pistol_hammer, s);
        }
        {
            const auto s = begin_sub(RangeMeshKind::pistol_trigger);
            if (trigger_mesh != nullptr) {
                mb.append_loaded_mesh(*trigger_mesh, pistol_scale);
            }
            end_sub(RangeMeshKind::pistol_trigger, s);
        }
        {
            const auto s = begin_sub(RangeMeshKind::pistol_sights);
            end_sub(RangeMeshKind::pistol_sights, s);
        }
    } else {
        {
            const auto s = begin_sub(RangeMeshKind::pistol_frame);
            mb.add_slanted_box({0.0f, -0.095f, -0.052f}, {0.0f, 0.014f, -0.020f}, 0.0155f, 0.023f, 11U);
            mb.add_box({0.0f, 0.020f, 0.040f}, {0.0155f, 0.011f, 0.092f}, 11U, 1.0f, 1.0f, true);
            mb.add_box({0.0f, 0.004f, 0.085f}, {0.013f, 0.006f, 0.036f}, 11U, 1.0f, 1.0f, true);
            mb.add_box({0.0f, -0.020f, 0.028f}, {0.008f, 0.004f, 0.032f}, 11U, 1.0f, 1.0f, true);
            mb.add_box({0.0f, -0.004f, 0.058f}, {0.008f, 0.016f, 0.004f}, 11U, 1.0f, 1.0f, true);
            end_sub(RangeMeshKind::pistol_frame, s);
        }
        {
            const auto s = begin_sub(RangeMeshKind::pistol_slide);
            mb.add_box({0.0f, 0.044f, 0.036f}, {0.0155f, 0.0145f, 0.098f}, 10U, 1.0f, 1.0f, true);
            mb.add_box({0.0f, 0.0600f, 0.036f}, {0.0115f, 0.0028f, 0.095f}, 10U, 1.0f, 1.0f, true);
            mb.add_box({-0.0095f, 0.0665f, -0.048f}, {0.0040f, 0.0050f, 0.0065f}, 10U, 1.0f, 1.0f, true);
            mb.add_box({ 0.0095f, 0.0665f, -0.048f}, {0.0040f, 0.0050f, 0.0065f}, 10U, 1.0f, 1.0f, true);
            mb.add_box({ 0.0f, 0.0675f, 0.122f}, {0.0020f, 0.0056f, 0.0055f}, 10U, 1.0f, 1.0f, true);
            end_sub(RangeMeshKind::pistol_slide, s);
        }
        {
            const auto s = begin_sub(RangeMeshKind::pistol_barrel);
            mb.add_cylinder_z({0.0f, 0.044f, 0.072f}, 0.0085f, 0.076f, 14, 4U);
            mb.add_box({0.003f, 0.0535f, 0.025f}, {0.0135f, 0.0082f, 0.024f}, 4U, 1.0f, 1.0f, true);
            end_sub(RangeMeshKind::pistol_barrel, s);
        }
        {
            const auto s = begin_sub(RangeMeshKind::pistol_magazine);
            mb.add_slanted_box({0.0f, -0.098f, -0.054f}, {0.0f, -0.008f, -0.026f}, 0.0115f, 0.017f, 10U);
            mb.add_box({0.0f, -0.102f, -0.054f}, {0.016f, 0.006f, 0.024f}, 11U, 1.0f, 1.0f, true);
            end_sub(RangeMeshKind::pistol_magazine, s);
        }
        {
            const auto s = begin_sub(RangeMeshKind::pistol_sights);
            mb.add_box({-0.0095f, 0.0675f, -0.0550f}, {0.0016f, 0.0016f, 0.0010f}, 4U, 1.0f, 1.0f, true, 1.0f);
            mb.add_box({ 0.0095f, 0.0675f, -0.0550f}, {0.0016f, 0.0016f, 0.0010f}, 4U, 1.0f, 1.0f, true, 1.0f);
            mb.add_box({ 0.0f, 0.0680f, 0.1160f}, {0.0016f, 0.0018f, 0.0010f}, 4U, 1.0f, 1.0f, true, 1.0f);
            end_sub(RangeMeshKind::pistol_sights, s);
        }
        {
            const auto s = begin_sub(RangeMeshKind::pistol_hammer);
            mb.add_box({0.0f, 0.044f, -0.060f}, {0.003f, 0.008f, 0.005f}, 10U, 1.0f, 1.0f, true);
            end_sub(RangeMeshKind::pistol_hammer, s);
        }
        {
            const auto s = begin_sub(RangeMeshKind::pistol_trigger);
            mb.add_box({0.0f, -0.004f, 0.026f}, {0.004f, 0.014f, 0.005f}, 10U, 1.0f, 1.0f, true);
            end_sub(RangeMeshKind::pistol_trigger, s);
        }
    }

    {
        const auto s = begin_sub(RangeMeshKind::muzzle_flash);
        mb.add_box({0.0f, 0.0f, 0.028f}, {0.042f, 0.0045f, 0.032f}, 5U, 1.0f, 1.0f, true, 1.0f);
        mb.add_box({0.0f, 0.0f, 0.028f}, {0.0045f, 0.042f, 0.032f}, 5U, 1.0f, 1.0f, true, 1.0f);
        mb.add_box({0.0f, 0.0f, 0.048f}, {0.008f, 0.008f, 0.042f}, 5U, 1.0f, 1.0f, true, 1.0f);
        end_sub(RangeMeshKind::muzzle_flash, s);
    }

    {
        const auto s = begin_sub(RangeMeshKind::brass_casing);
        mb.add_cylinder_y({0.0f, 0.0f, 0.0f}, 0.006f, 0.014f, 10, 5U);
        end_sub(RangeMeshKind::brass_casing, s);
    }

    {
        const auto s = begin_sub(RangeMeshKind::bullet_tracer);
        mb.add_box({0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 0.5f}, 5U, 1.0f, 1.0f, true, 1.0f);
        end_sub(RangeMeshKind::bullet_tracer, s);
    }

    {
        const auto s = begin_sub(RangeMeshKind::impact_spark);
        mb.add_box({0.0f, 0.0f, 0.0f}, {1.0f, 0.16f, 0.16f}, 5U, 1.0f, 1.0f, true, 1.0f);
        mb.add_box({0.0f, 0.0f, 0.0f}, {0.16f, 1.0f, 0.16f}, 5U, 1.0f, 1.0f, true, 1.0f);
        mb.add_box({0.0f, 0.0f, 0.0f}, {0.16f, 0.16f, 1.0f}, 5U, 1.0f, 1.0f, true, 1.0f);
        end_sub(RangeMeshKind::impact_spark, s);
    }

    return pkg;
}

}
