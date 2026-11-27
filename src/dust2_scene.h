#pragma once

#include "range_mesh.h"
#include "simulation.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace frame_warp_lab {

inline constexpr std::uint32_t k_dust2_material_base = 16;
inline constexpr std::uint32_t k_dust2_max_materials = 100;
inline constexpr std::uint32_t k_max_clustered_lights = 64;
inline constexpr std::uint32_t k_max_octree_probes = 256;

struct alignas(16) GpuClusterLight {
    float pos_radius[4];
    float dir_cos_outer[4];
    float color_atten[4];
    float params[4];
};

struct alignas(16) OctreeVisProbeGpu {
    float pos_radius[4];
    float sky_bounce[4];
    float dir_vis_pos[4];
    float dir_vis_neg[4];
};

struct Dust2LodSlice {
    std::uint32_t index_offset = 0;
    std::uint32_t index_count = 0;
};

struct Dust2OctreeNode {
    Float3 min_bounds{0.0f, 0.0f, 0.0f};
    Float3 max_bounds{0.0f, 0.0f, 0.0f};
    Float3 center{0.0f, 0.0f, 0.0f};
    float radius = 0.0f;
    std::int32_t children[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    std::array<Dust2LodSlice, 3> lods{};
    std::uint32_t probe_index = 0;
    bool is_leaf = false;
};

struct Dust2DrawChunk {
    std::uint32_t index_offset = 0;
    std::uint32_t index_count = 0;
    std::uint32_t lod_level = 0;
    std::uint32_t node_index = 0;
};

struct Dust2OctreeTelemetry {
    std::uint32_t total_nodes = 0;
    std::uint32_t leaf_nodes = 0;
    std::uint32_t visited_nodes = 0;
    std::uint32_t culled_nodes = 0;
    std::uint32_t probe_occluded_nodes = 0;
    std::uint32_t lod0_chunks = 0;
    std::uint32_t lod1_chunks = 0;
    std::uint32_t lod2_chunks = 0;
    std::uint32_t submitted_triangles = 0;
    std::uint32_t total_lod0_triangles = 0;
    std::uint32_t active_probes = 0;
    std::uint32_t active_lights = 0;
};

struct Dust2MapPackage {
    bool loaded = false;
    std::vector<RangeVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::uint32_t lod0_total_index_count = 0;
    std::vector<std::string> texture_paths;
    std::vector<Dust2OctreeNode> octree_nodes;
    std::vector<OctreeVisProbeGpu> probes;
    std::vector<GpuClusterLight> lights;
    std::vector<Float3> tour_waypoints;
    Float3 world_min{0.0f, 0.0f, 0.0f};
    Float3 world_max{0.0f, 0.0f, 0.0f};
    Float3 spawn_pos{0.0f, 1.7f, 0.0f};
    float spawn_yaw = 0.0f;
    std::uint32_t total_lod0_triangles = 0;
    std::uint32_t leaf_count = 0;
};

Dust2MapPackage load_and_build_dust2_map();

void select_dust2_octree_draws(
    const Dust2MapPackage& map,
    const CameraBasis& camera,
    float aspect_ratio,
    std::vector<Dust2DrawChunk>& out_chunks,
    Dust2OctreeTelemetry& out_telemetry);

std::vector<GpuClusterLight> build_range_cluster_lights();
std::vector<OctreeVisProbeGpu> build_range_visibility_probes();

CameraBasis sample_dust2_camera_path(
    const Dust2MapPackage& map,
    CameraPath path,
    double time_seconds,
    float vertical_fov_radians);

}
