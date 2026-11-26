#pragma once

#include "tactical_range.h"

#include <array>
#include <cstdint>
#include <vector>

namespace frame_warp_lab {

struct RangeVertex {
    float px, py, pz;
    float nx, ny, nz;
    float tx, ty, tz;
    float u, v;
    std::uint32_t material_id;
    float emissive_weight;
};

struct RangeSubmesh {
    std::uint32_t index_offset;
    std::uint32_t index_count;
};

struct RangeGeometryPackage {
    std::vector<RangeVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::array<RangeSubmesh, static_cast<std::size_t>(RangeMeshKind::count)> submeshes{};
};

RangeGeometryPackage build_tactical_range_geometry();

}
