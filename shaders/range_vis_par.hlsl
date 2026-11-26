cbuffer TileSceneConstants : register(b0) {
    float4 camera_position_tan_y;
    float4 camera_right_tan_x;
    float4 camera_up_rcp_width;
    float4 camera_forward_rcp_height;
    float4 previous_camera_position_rcp_tan_y;
    float4 previous_camera_right_rcp_tan_x;
    float4 previous_camera_up_cut;
    float4 previous_camera_forward_time;
    float4 muzzle_light_pos_intensity;
    float4 range_flags;
    uint4 tile_params;
    uint4 cache_params;
    uint4 policy_params;
    uint4 prev_and_alloc_dims;
};

struct RangeVertexGpu {
    float3 position;
    float3 normal;
    float3 tangent;
    float2 uv;
    uint material_id;
    float emissive_weight;
};

struct InstanceData {
    float4x4 model;
    float4x4 prev_model;
    float4 tint_emissive;
};

Texture2D<float4> prev_color_texture : register(t0);
Texture2D<float> prev_depth_texture : register(t1);
Texture2D<uint> prev_vis_texture : register(t2);

#include "pbr_materials.hlsl"

Texture2D<float4> pbr_hero_albedo : register(t8);
Texture2D<float4> pbr_hero_normal : register(t9);
Texture2D<float4> pbr_hero_orm : register(t10);

StructuredBuffer<RangeVertexGpu> range_vertices : register(t11);
StructuredBuffer<uint> range_indices : register(t12);
StructuredBuffer<InstanceData> range_instances : register(t13);

RWTexture2D<float4> out_color_uav : register(u0);
RWTexture2D<float> out_depth_uav : register(u1);
RWTexture2D<float2> out_motion_uav : register(u2);
RWTexture2D<uint> out_vis_uav : register(u3);
RWTexture2D<uint> out_tile_map_uav : register(u4);
RWStructuredBuffer<uint> dirty_tile_list_uav : register(u5);
RWStructuredBuffer<uint> tile_candidate_meta_uav : register(u6);
RWByteAddressBuffer tile_indirect_args_uav : register(u7);

SamplerState linear_clamp_sampler : register(s0);
SamplerState point_clamp_sampler : register(s1);

#include "range_lighting.hlsl"

static const uint k_stbn_phase_packed[32] = {
    0xb0e5cb91u, 0xbe72a4b1u, 0x5ad8f73eu, 0x63a3d839u,
    0xe62164a2u, 0xc9f16cf2u, 0x7c3b90d5u, 0x804b7084u,
    0xa0f8ec7fu, 0xb6d9ea2du, 0x69515241u, 0x3e2145f1u,
    0xbd3ca8d9u, 0x7a8d7c69u, 0x847f60e5u, 0xc045f032u,
    0xe0b1b4a2u, 0x7eb19bc5u, 0xda5938d3u, 0xa36e37f8u,
    0x26e2fc61u, 0xf9c2a461u, 0x3c74807bu, 0x4085d09bu,
    0xf0ad2ae9u, 0xd6bf7ce8u, 0x5961f541u, 0x2e314251u,
    0x3db96c7du, 0x8a79d8acu, 0x748230f5u, 0x40c5e06fu,
};

groupshared uint g_dirty_pixel_count;
groupshared uint g_disoccluded_count;
groupshared uint g_moving_count;
groupshared uint g_non_sky_count;
groupshared uint g_max_age_in_tile;

uint lookup_stbn_phase(uint tx, uint ty) {
    uint row = ty & 15u;
    uint col = tx & 15u;
    uint word = k_stbn_phase_packed[(row << 1u) | (col >> 3u)];
    return (word >> ((col & 7u) * 4u)) & 15u;
}

float4 project_camera_clip(float3 world_pos) {
    float3 rel = world_pos - camera_position_tan_y.xyz;
    float vx = dot(rel, camera_right_tan_x.xyz);
    float vy = dot(rel, camera_up_rcp_width.xyz);
    float vz = dot(rel, camera_forward_rcp_height.xyz);
    float near_z = 0.04;
    float far_z = 80.0;
    float cx = vx / max(camera_right_tan_x.w, 1e-4);
    float cy = vy / max(camera_position_tan_y.w, 1e-4);
    float cz = (far_z * (vz - near_z)) / (far_z - near_z);
    return float4(cx, cy, cz, vz);
}

float3 sample_catmull_rom_5tap(float2 uv, float2 active_size, float2 inv_alloc_size, float2 uv_min, float2 uv_max) {
    float2 sample_pos = uv * active_size;
    float2 tc1 = floor(sample_pos - 0.5) + 0.5;
    float2 f = sample_pos - tc1;
    float2 f2 = f * f;
    float2 f3 = f2 * f;

    float2 w0 = f2 - 0.5 * (f3 + f);
    float2 w1 = 1.5 * f3 - 2.5 * f2 + 1.0;
    float2 w3 = 0.5 * (f3 - f2);
    float2 w2 = 1.0 - w0 - w1 - w3;

    float2 w12 = w1 + w2;
    float2 offset12 = w2 / max(w12, float2(1e-4, 1e-4));

    float2 tc0 = clamp((tc1 - 1.0) * inv_alloc_size, uv_min, uv_max);
    float2 tc3 = clamp((tc1 + 2.0) * inv_alloc_size, uv_min, uv_max);
    float2 tc12 = clamp((tc1 + offset12) * inv_alloc_size, uv_min, uv_max);

    float3 s0 = prev_color_texture.SampleLevel(linear_clamp_sampler, float2(tc12.x, tc0.y), 0).rgb;
    float3 s1 = prev_color_texture.SampleLevel(linear_clamp_sampler, float2(tc0.x, tc12.y), 0).rgb;
    float3 s2 = prev_color_texture.SampleLevel(linear_clamp_sampler, float2(tc12.x, tc12.y), 0).rgb;
    float3 s3 = prev_color_texture.SampleLevel(linear_clamp_sampler, float2(tc3.x, tc12.y), 0).rgb;
    float3 s4 = prev_color_texture.SampleLevel(linear_clamp_sampler, float2(tc12.x, tc3.y), 0).rgb;

    float wt0 = w12.x * w0.y;
    float wt1 = w0.x * w12.y;
    float wt2 = w12.x * w12.y;
    float wt3 = w3.x * w12.y;
    float wt4 = w12.x * w3.y;
    float3 bicubic = (s0 * wt0 + s1 * wt1 + s2 * wt2 + s3 * wt3 + s4 * wt4) / max(wt0 + wt1 + wt2 + wt3 + wt4, 1e-4);
    float3 box_min = min(min(s0, s1), min(s2, min(s3, s4)));
    float3 box_max = max(max(s0, s1), max(s2, max(s3, s4)));
    return clamp(bicubic, box_min, box_max);
}

float3 intersect_ray_triangle_bary(float3 ray_origin, float3 ray_dir, float3 wp0, float3 e1, float3 e2, float3 s) {
    float3 pvec = cross(ray_dir, e2);
    float det = dot(e1, pvec);
    float safe_det = (abs(det) > 1e-12) ? det : ((det >= 0.0) ? 1e-12 : -1e-12);
    float inv_det = rcp(safe_det);
    float u = dot(s, pvec) * inv_det;
    float3 qvec = cross(s, e1);
    float v = dot(ray_dir, qvec) * inv_det;
    return float3(1.0 - u - v, u, v);
}

void shade_vis_pixel(uint2 pixel_coord, uint width, uint height) {
    uint vis_word = out_vis_uav[pixel_coord];
    if (vis_word == 0u) {
        out_color_uav[pixel_coord] = float4(0.035, 0.050, 0.068, 0.0);
        return;
    }

    uint inst_idx = (vis_word >> 25u) - 1u;
    bool is_viewmodel = ((vis_word >> 24u) & 1u) != 0u;
    uint tri_id = vis_word & 0xFFFFFFu;

    uint base_i = tri_id * 3u;
    uint i0 = range_indices[base_i + 0u];
    uint i1 = range_indices[base_i + 1u];
    uint i2 = range_indices[base_i + 2u];

    RangeVertexGpu v0 = range_vertices[i0];
    RangeVertexGpu v1 = range_vertices[i1];
    RangeVertexGpu v2 = range_vertices[i2];
    InstanceData inst = range_instances[inst_idx];

    float3 wp0 = mul(inst.model, float4(v0.position, 1.0)).xyz;
    float3 wp1 = mul(inst.model, float4(v1.position, 1.0)).xyz;
    float3 wp2 = mul(inst.model, float4(v2.position, 1.0)).xyz;

    float2 curr_uv = (float2(pixel_coord) + 0.5) * float2(camera_up_rcp_width.w, camera_forward_rcp_height.w);
    float2 p_ndc = float2(curr_uv.x * 2.0 - 1.0, 1.0 - curr_uv.y * 2.0);
    float dx_ndc = 2.0 * camera_up_rcp_width.w;
    float dy_ndc = -2.0 * camera_forward_rcp_height.w;

    float3 cam_pos = camera_position_tan_y.xyz;
    float3 cam_r = camera_right_tan_x.xyz * camera_right_tan_x.w;
    float3 cam_u = camera_up_rcp_width.xyz * camera_position_tan_y.w;
    float3 cam_f = camera_forward_rcp_height.xyz;

    float3 rd = cam_r * p_ndc.x + cam_u * p_ndc.y + cam_f;
    float3 rd_x = rd + cam_r * dx_ndc;
    float3 rd_y = rd + cam_u * dy_ndc;

    float3 e1 = wp1 - wp0;
    float3 e2 = wp2 - wp0;
    float3 s = cam_pos - wp0;
    float3 b = intersect_ray_triangle_bary(cam_pos, rd, wp0, e1, e2, s);
    float3 bx = intersect_ray_triangle_bary(cam_pos, rd_x, wp0, e1, e2, s);
    float3 by = intersect_ray_triangle_bary(cam_pos, rd_y, wp0, e1, e2, s);

    float3 world_pos = wp0 * b.x + wp1 * b.y + wp2 * b.z;
    float3 wn0 = normalize(mul(inst.model, float4(v0.normal, 0.0)).xyz);
    float3 wn1 = normalize(mul(inst.model, float4(v1.normal, 0.0)).xyz);
    float3 wn2 = normalize(mul(inst.model, float4(v2.normal, 0.0)).xyz);
    float3 wt0 = normalize(mul(inst.model, float4(v0.tangent, 0.0)).xyz);
    float3 wt1 = normalize(mul(inst.model, float4(v1.tangent, 0.0)).xyz);
    float3 wt2 = normalize(mul(inst.model, float4(v2.tangent, 0.0)).xyz);

    float3 geo_n = normalize(wn0 * b.x + wn1 * b.y + wn2 * b.z);
    float3 geo_nx = normalize(wn0 * bx.x + wn1 * bx.y + wn2 * bx.z);
    float3 geo_ny = normalize(wn0 * by.x + wn1 * by.y + wn2 * by.z);
    float3 geo_t = normalize(wt0 * b.x + wt1 * b.y + wt2 * b.z);

    float2 uv = v0.uv * b.x + v1.uv * b.y + v2.uv * b.z;
    float2 uv_x = v0.uv * bx.x + v1.uv * bx.y + v2.uv * bx.z;
    float2 uv_y = v0.uv * by.x + v1.uv * by.y + v2.uv * by.z;
    float2 duv_dx = uv_x - uv;
    float2 duv_dy = uv_y - uv;
    float em_w = v0.emissive_weight * b.x + v1.emissive_weight * b.y + v2.emissive_weight * b.z;

    float3 shaded = shade_range_surface_pbr(
        world_pos,
        geo_n,
        geo_t,
        uv,
        duv_dx,
        duv_dy,
        geo_nx - geo_n,
        geo_ny - geo_n,
        v0.material_id,
        inst.tint_emissive,
        em_w,
        is_viewmodel,
        range_flags.y > 0.5,
        previous_camera_forward_time.w,
        (uint)(range_flags.w + 0.5),
        camera_position_tan_y.xyz,
        camera_forward_rcp_height.xyz,
        muzzle_light_pos_intensity,
        pbr_hero_albedo,
        pbr_hero_normal,
        pbr_hero_orm,
        linear_clamp_sampler);

    out_color_uav[pixel_coord] = float4(shaded, 0.0);
}

[numthreads(16, 16, 1)]
void range_vis_classify_main(
    uint3 group_id : SV_GroupID,
    uint3 local_id : SV_GroupThreadID,
    uint3 dispatch_id : SV_DispatchThreadID) {
    uint tiles_x = tile_params.x;
    uint tiles_y = tile_params.y;
    uint tile_budget = tile_params.z;
    uint frame_id = tile_params.w;
    bool has_prev = (cache_params.x != 0u) && (previous_camera_up_cut.w < 0.5);
    uint width = cache_params.z;
    uint height = cache_params.w;
    bool use_priority_stbn = (policy_params.x & 1u) != 0u;
    bool use_subtile_interleave = use_priority_stbn && ((policy_params.x & 2u) != 0u);
    bool use_reactive_invalidation = (policy_params.x & 4u) != 0u;
    bool use_shadow_delta_invalidation = (policy_params.x & 8u) != 0u;
    uint shader_flags = (uint)(range_flags.w + 0.5);
    float curr_scene_time = previous_camera_forward_time.w;
    float prev_scene_time = range_flags.z;
    uint total_tiles = policy_params.y;
    bool use_catmull_rom = (policy_params.z != 0u);

    if (local_id.x == 0u && local_id.y == 0u) {
        g_dirty_pixel_count = 0u;
        g_disoccluded_count = 0u;
        g_moving_count = 0u;
        g_non_sky_count = 0u;
        g_max_age_in_tile = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    uint2 pixel_coord = dispatch_id.xy;
    bool in_bounds = (pixel_coord.x < width) && (pixel_coord.y < height);
    if (in_bounds) {
        uint vis_word = out_vis_uav[pixel_coord];
        float depth = out_depth_uav[pixel_coord];
        float2 motion = out_motion_uav[pixel_coord];

        if (vis_word == 0u) {
            out_color_uav[pixel_coord] = float4(0.035, 0.050, 0.068, 0.0);
        } else {
            InterlockedAdd(g_non_sky_count, 1u);
            uint inst_enc = vis_word >> 25u;
            bool is_vm = ((vis_word >> 24u) & 1u) != 0u;
            float2 curr_uv = (float2(pixel_coord) + 0.5) * float2(camera_up_rcp_width.w, camera_forward_rcp_height.w);
            float2 prev_uv = curr_uv + motion;

            bool is_moving = is_vm || (muzzle_light_pos_intensity.w > 0.02);
            bool is_reactive_miss = false;
            bool is_shadow_delta_miss = false;
            if (inst_enc > 1u) {
                InstanceData inst = range_instances[inst_enc - 1u];
                float3 d_tr = float3(
                    inst.model[0][3] - inst.prev_model[0][3],
                    inst.model[1][3] - inst.prev_model[1][3],
                    inst.model[2][3] - inst.prev_model[2][3]);
                float d_rot = abs(inst.model[0][0] - inst.prev_model[0][0])
                    + abs(inst.model[1][1] - inst.prev_model[1][1])
                    + abs(inst.model[2][2] - inst.prev_model[2][2]);
                if (dot(d_tr, d_tr) > 1.0e-8 || d_rot > 1.0e-4 || inst.tint_emissive.w > 0.02) {
                    is_moving = true;
                }
            }
            if (use_reactive_invalidation && (shader_flags & (2u | 16u)) != 0u && !is_vm) {
                uint tri_id = vis_word & 0xFFFFFFu;
                RangeVertexGpu rv0 = range_vertices[range_indices[tri_id * 3u]];
                bool anim_uv_mat = ((shader_flags & 2u) != 0u) && (rv0.material_id == 6u || rv0.material_id == 8u);
                bool anim_em_mat = ((shader_flags & 16u) != 0u) && (rv0.emissive_weight > 8.0 || rv0.material_id == 8u);
                if (anim_uv_mat || anim_em_mat) {
                    is_reactive_miss = true;
                    is_moving = true;
                }
            }
            if (use_shadow_delta_invalidation && (shader_flags & (8u | 32u)) != 0u && !is_vm && has_prev) {
                float2 p_ndc = float2(curr_uv.x * 2.0 - 1.0, 1.0 - curr_uv.y * 2.0);
                float3 ray_d = camera_right_tan_x.xyz * (p_ndc.x * camera_right_tan_x.w)
                    + camera_up_rcp_width.xyz * (p_ndc.y * camera_position_tan_y.w)
                    + camera_forward_rcp_height.xyz;
                float3 recon_wp = camera_position_tan_y.xyz + ray_d * depth;
                uint cur_sig = eval_range_shadow_signature(recon_wp, curr_scene_time, shader_flags);
                uint prv_sig = eval_range_shadow_signature(recon_wp, prev_scene_time, shader_flags);
                if (cur_sig != prv_sig) {
                    is_shadow_delta_miss = true;
                    is_moving = true;
                }
            }
            if (is_moving) {
                InterlockedAdd(g_moving_count, 1u);
            }

            bool cache_valid = false;
            float3 cached_rgb = float3(0.035, 0.050, 0.068);
            uint next_age = 255u;

            if (has_prev && !is_reactive_miss && !is_shadow_delta_miss && prev_uv.x > 0.001 && prev_uv.x < 0.999 && prev_uv.y > 0.001 && prev_uv.y < 0.999) {
                float2 prev_active_size = float2(max(prev_and_alloc_dims.x, 1u), max(prev_and_alloc_dims.y, 1u));
                float2 inv_alloc_size = float2(rcp(float(max(prev_and_alloc_dims.z, 1u))), rcp(float(max(prev_and_alloc_dims.w, 1u))));
                float2 uv_min = 0.5 * inv_alloc_size;
                float2 uv_max = (prev_active_size - 0.5) * inv_alloc_size;
                float2 prev_tex_uv = clamp(prev_uv * prev_active_size * inv_alloc_size, uv_min, uv_max);
                int2 prev_px = clamp(int2(prev_uv * prev_active_size), int2(0, 0), int2(prev_and_alloc_dims.xy) - int2(1, 1));

                uint prev_vis = prev_vis_texture.Load(int3(prev_px, 0));
                float prev_d = prev_depth_texture.Load(int3(prev_px, 0));
                float rel_depth_err = abs(prev_d - depth) / max(depth, 0.25);

                if (prev_vis != 0u && (prev_vis >> 25u) == inst_enc && rel_depth_err < 0.065) {
                    float4 prev_col_age = prev_color_texture.Load(int3(prev_px, 0));
                    if (prev_col_age.a < 253.5) {
                        uint prev_age = (uint)clamp(prev_col_age.a, 0.0, 250.0);
                        next_age = min(prev_age + 1u, 252u);
                        if (use_catmull_rom) {
                            cached_rgb = sample_catmull_rom_5tap(prev_uv, prev_active_size, inv_alloc_size, uv_min, uv_max);
                        } else {
                            cached_rgb = prev_color_texture.SampleLevel(linear_clamp_sampler, prev_tex_uv, 0).rgb;
                        }
                        cache_valid = true;
                    }
                }
            }

            if (cache_valid) {
                out_color_uav[pixel_coord] = float4(cached_rgb, (float)next_age);
                InterlockedMax(g_max_age_in_tile, next_age);
            } else {
                float3 fallback_rgb = float3(0.035, 0.050, 0.068);
                if (has_prev) {
                    float2 prev_active_size = float2(max(prev_and_alloc_dims.x, 1u), max(prev_and_alloc_dims.y, 1u));
                    float2 inv_alloc_size = float2(rcp(float(max(prev_and_alloc_dims.z, 1u))), rcp(float(max(prev_and_alloc_dims.w, 1u))));
                    float2 uv_min = 0.5 * inv_alloc_size;
                    float2 uv_max = (prev_active_size - 0.5) * inv_alloc_size;
                    float2 clamped_uv = clamp(prev_uv, float2(0.0, 0.0), float2(1.0, 1.0)) * prev_active_size * inv_alloc_size;
                    fallback_rgb = prev_color_texture.SampleLevel(linear_clamp_sampler, clamp(clamped_uv, uv_min, uv_max), 0).rgb;
                }
                out_color_uav[pixel_coord] = float4(fallback_rgb, 255.0);
                InterlockedAdd(g_dirty_pixel_count, 1u);
                InterlockedAdd(g_disoccluded_count, 1u);
                InterlockedMax(g_max_age_in_tile, 255u);
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if (local_id.x == 0u && local_id.y == 0u && group_id.x < tiles_x && group_id.y < tiles_y) {
        uint unused_total = 0u;
        tile_indirect_args_uav.InterlockedAdd(12u, 1u, unused_total);

        uint tile_id = group_id.y * tiles_x + group_id.x;
        uint stbn_phase = lookup_stbn_phase(group_id.x, group_id.y);
        bool force_full = (!has_prev) || (tile_budget >= total_tiles);
        bool periodic_refresh = false;
        if (g_non_sky_count > 0u) {
            if (use_subtile_interleave) {
                periodic_refresh = (((frame_id + stbn_phase) & 3u) == 0u);
            } else {
                periodic_refresh = (((frame_id + stbn_phase) & 15u) == 0u);
            }
        }

        float2 tile_uv = (float2(group_id.xy) + 0.5) / float2(max(tiles_x, 1u), max(tiles_y, 1u));
        float reticle_dist = length(tile_uv - float2(0.5, 0.5));
        bool has_critical = force_full || (g_disoccluded_count > 0u) || (g_moving_count > 0u);
        bool has_dirty = has_critical || (g_dirty_pixel_count > 0u);

        if (g_non_sky_count == 0u) {
            tile_candidate_meta_uav[tile_id] = 0u;
            out_tile_map_uav[group_id.xy] = 0u;
            uint unused_cached = 0u;
            tile_indirect_args_uav.InterlockedAdd(16u, 1u, unused_cached);
        } else if (has_dirty || periodic_refresh || g_max_age_in_tile >= 16u) {
            uint bucket = 7u;
            if (force_full || (has_critical && reticle_dist < 0.35)) {
                bucket = 0u;
            } else if (has_critical) {
                bucket = 1u;
            } else if (g_dirty_pixel_count > 0u) {
                bucket = 2u;
            } else if (g_max_age_in_tile >= 24u) {
                bucket = 4u;
            } else if (reticle_dist < 0.35) {
                bucket = 5u;
            } else if (g_max_age_in_tile >= 10u) {
                bucket = 6u;
            } else {
                bucket = 7u;
            }
            uint prio_6bit = min(63u, (7u - bucket) * 9u);
            uint kind = has_dirty ? 1u : 2u;
            tile_candidate_meta_uav[tile_id] = 0x80000000u | (bucket << 24u) | (prio_6bit << 16u) | kind;
            uint unused_bucket = 0u;
            tile_indirect_args_uav.InterlockedAdd(32u + bucket * 4u, 1u, unused_bucket);
        } else {
            tile_candidate_meta_uav[tile_id] = 0u;
            out_tile_map_uav[group_id.xy] = 0u;
            uint unused_cached = 0u;
            tile_indirect_args_uav.InterlockedAdd(16u, 1u, unused_cached);
        }
    }
}

[numthreads(256, 1, 1)]
void range_vis_admit_main(uint3 dispatch_thread_id : SV_DispatchThreadID) {
    uint tiles_x = tile_params.x;
    uint total_tiles = policy_params.y;
    uint tile_budget = tile_params.z;
    bool use_priority_stbn = (policy_params.x & 1u) != 0u;
    bool use_subtile_interleave = use_priority_stbn && ((policy_params.x & 2u) != 0u);

    uint full_candidates = 0u;
    uint inter_candidates = 0u;
    [unroll]
    for (uint b = 0u; b < 4u; ++b) {
        full_candidates += tile_indirect_args_uav.Load(32u + b * 4u);
    }
    [unroll]
    for (uint b2 = 4u; b2 < 8u; ++b2) {
        inter_candidates += tile_indirect_args_uav.Load(32u + b2 * 4u);
    }

    uint full_admitted = min(full_candidates, tile_budget);
    uint rem_groups = (tile_budget > full_admitted) ? (tile_budget - full_admitted) : 0u;
    uint inter_admitted = use_subtile_interleave ? min(inter_candidates, rem_groups * 4u) : 0u;
    uint inter_groups = use_subtile_interleave ? ((inter_admitted + 3u) >> 2u) : 0u;

    uint tile_id = dispatch_thread_id.x;
    if (tile_id == 0u) {
        if (use_subtile_interleave) {
            tile_indirect_args_uav.Store(0u, full_admitted + inter_groups);
            tile_candidate_meta_uav[total_tiles] = inter_admitted;
        } else {
            tile_indirect_args_uav.Store(0u, min(full_candidates + inter_candidates, tile_budget));
            tile_candidate_meta_uav[total_tiles] = 0u;
        }
    }
    if (use_subtile_interleave && tile_id < inter_groups) {
        dirty_tile_list_uav[full_admitted + tile_id] = 0x80000000u | tile_id;
    }

    if (tile_id >= total_tiles) {
        return;
    }

    uint meta = tile_candidate_meta_uav[tile_id];
    if ((meta & 0x80000000u) == 0u) {
        return;
    }

    uint bucket = (meta >> 24u) & 7u;
    uint prio_6bit = (meta >> 16u) & 63u;
    uint state_kind = meta & 3u;
    uint tile_y = tile_id / tiles_x;
    uint tile_x = tile_id - tile_y * tiles_x;

    if (use_subtile_interleave && bucket >= 4u) {
        uint inter_bucket_start = 0u;
        [unroll]
        for (uint ib = 4u; ib < 8u; ++ib) {
            if (ib < bucket) {
                inter_bucket_start += tile_indirect_args_uav.Load(32u + ib * 4u);
            }
        }
        uint inter_slot = inter_admitted;
        if (inter_bucket_start < inter_admitted) {
            uint local_slot = 0u;
            tile_indirect_args_uav.InterlockedAdd(64u + bucket * 4u, 1u, local_slot);
            inter_slot = inter_bucket_start + local_slot;
        }
        if (inter_slot < inter_admitted) {
            dirty_tile_list_uav[65536u + inter_slot] = (tile_x & 0x7FFFu) | ((tile_y & 0x7FFFu) << 15u);
            out_tile_map_uav[uint2(tile_x, tile_y)] = (state_kind & 3u) | (prio_6bit << 2u);
            uint unused_sched = 0u;
            tile_indirect_args_uav.InterlockedAdd(20u, 1u, unused_sched);
        } else {
            out_tile_map_uav[uint2(tile_x, tile_y)] = 3u | (prio_6bit << 2u);
            uint unused_def = 0u;
            tile_indirect_args_uav.InterlockedAdd(24u, 1u, unused_def);
        }
        return;
    }

    uint global_slot = tile_budget;
    if (use_priority_stbn) {
        uint bucket_start = 0u;
        [unroll]
        for (uint b3 = 0u; b3 < 8u; ++b3) {
            if (b3 < bucket) {
                bucket_start += tile_indirect_args_uav.Load(32u + b3 * 4u);
            }
        }
        if (bucket_start < tile_budget) {
            uint local_slot = 0u;
            tile_indirect_args_uav.InterlockedAdd(64u + bucket * 4u, 1u, local_slot);
            global_slot = bucket_start + local_slot;
        }
    } else {
        tile_indirect_args_uav.InterlockedAdd(64u, 1u, global_slot);
    }

    if (global_slot < tile_budget) {
        dirty_tile_list_uav[global_slot] = (tile_x & 0x7FFFu) | ((tile_y & 0x7FFFu) << 15u);
        out_tile_map_uav[uint2(tile_x, tile_y)] = (state_kind & 3u) | (prio_6bit << 2u);
        uint unused_sched = 0u;
        tile_indirect_args_uav.InterlockedAdd(20u, 1u, unused_sched);
    } else {
        out_tile_map_uav[uint2(tile_x, tile_y)] = 3u | (prio_6bit << 2u);
        uint unused_def = 0u;
        tile_indirect_args_uav.InterlockedAdd(24u, 1u, unused_def);
        if (bucket <= 1u) {
            uint unused_crit = 0u;
            tile_indirect_args_uav.InterlockedAdd(28u, 1u, unused_crit);
        }
    }
}

[numthreads(16, 16, 1)]
void range_vis_shade_main(
    uint3 group_id : SV_GroupID,
    uint3 group_thread_id : SV_GroupThreadID) {
    uint cmd = dirty_tile_list_uav[group_id.x];
    uint tile_x = 0u;
    uint tile_y = 0u;
    uint2 pixel_coord = uint2(0u, 0u);
    if ((cmd & 0x80000000u) != 0u) {
        uint inter_group = cmd & 0x7FFFFFFFu;
        uint total_tiles = policy_params.y;
        uint inter_count = tile_candidate_meta_uav[total_tiles];
        uint quad_id = ((group_thread_id.y >> 3u) << 1u) | (group_thread_id.x >> 3u);
        uint inter_idx = inter_group * 4u + quad_id;
        if (inter_idx >= inter_count) {
            return;
        }
        uint packed = dirty_tile_list_uav[65536u + inter_idx];
        tile_x = packed & 0x7FFFu;
        tile_y = (packed >> 15u) & 0x7FFFu;
        uint sub_phase = (tile_params.w + lookup_stbn_phase(tile_x, tile_y)) & 3u;
        uint px_off = (sub_phase == 1u || sub_phase == 2u) ? 1u : 0u;
        uint py_off = (sub_phase == 1u || sub_phase == 3u) ? 1u : 0u;
        pixel_coord = uint2(
            tile_x * 16u + (group_thread_id.x & 7u) * 2u + px_off,
            tile_y * 16u + (group_thread_id.y & 7u) * 2u + py_off);
    } else {
        tile_x = cmd & 0x7FFFu;
        tile_y = (cmd >> 15u) & 0x7FFFu;
        pixel_coord = uint2(tile_x * 16u + group_thread_id.x, tile_y * 16u + group_thread_id.y);
    }

    uint width = cache_params.z;
    uint height = cache_params.w;
    if (pixel_coord.x >= width || pixel_coord.y >= height) {
        return;
    }

    shade_vis_pixel(pixel_coord, width, height);
}
