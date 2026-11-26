cbuffer TileSceneConstants : register(b0) {
    float4 camera_position_tan_y;
    float4 camera_right_tan_x;
    float4 camera_up_rcp_width;
    float4 camera_forward_rcp_height;
    float4 previous_camera_position_rcp_tan_y;
    float4 previous_camera_right_rcp_tan_x;
    float4 previous_camera_up_cut;
    float4 previous_camera_forward_unused;
    float4 ball_center_stress;
    float4 previous_ball_center_unused;
    uint4 tile_params;
    uint4 cache_params;
    uint4 policy_params;
    uint4 prev_and_alloc_dims;
};

Texture2D<float4> prev_color_texture : register(t0);
Texture2D<float> prev_depth_texture : register(t1);
Texture2D<uint> prev_vis_texture : register(t2);

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

#include "pbr_materials.hlsl"

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

groupshared float g_tile_depth[16][16];
groupshared uint g_tile_mat[16][16];
groupshared float3 g_coarse_color[4][4];
groupshared uint g_coarse_mat[4][4];
groupshared uint g_coarse_block_mask;
groupshared uint g_dirty_pixel_count;
groupshared uint g_disoccluded_count;
groupshared uint g_moving_count;
groupshared uint g_edge_count;
groupshared uint g_non_sky_count;
groupshared uint g_max_age_in_tile;

float3 coarse_material_tint(uint mat_id) {
    if (mat_id == 1u) return float3(0.16, 0.21, 0.23);
    if (mat_id == 2u) return float3(0.18, 0.23, 0.26);
    if (mat_id == 3u) return float3(0.24, 0.31, 0.34);
    if (mat_id == 4u) return float3(0.38, 0.22, 0.11);
    if (mat_id == 5u) return float3(0.38, 0.54, 0.56);
    return float3(0.55, 0.45, 0.18);
}

uint lookup_stbn_phase(uint tx, uint ty) {
    uint row = ty & 15u;
    uint col = tx & 15u;
    uint word = k_stbn_phase_packed[(row << 1u) | (col >> 3u)];
    return (word >> ((col & 7u) * 4u)) & 15u;
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

float synthetic_stress_modulation(float3 world_position, int stress_level) {
    float accumulator = 0.0;
    float3 sample_position = world_position * 0.37 + float3(0.13, 0.29, 0.47);
    int iteration_count = stress_level * 48;
    [loop]
    for (int index = 0; index < iteration_count; ++index) {
        sample_position = sin(sample_position * 1.73205 + float3(0.31, 0.57, 0.83));
        accumulator += dot(sample_position, float3(0.17, 0.23, 0.19));
    }
    return accumulator * 1e-6;
}

bool project_to_previous_uv(float3 previous_world_position, out float2 out_uv, out float out_view_z) {
    float3 relative = previous_world_position - previous_camera_position_rcp_tan_y.xyz;
    float view_z = dot(relative, previous_camera_forward_unused.xyz);
    out_view_z = view_z;
    if (view_z <= 0.01) {
        out_uv = float2(-1.0, -1.0);
        return false;
    }
    float inv_z = rcp(view_z);
    float ndc_x = dot(relative, previous_camera_right_rcp_tan_x.xyz) * inv_z * previous_camera_right_rcp_tan_x.w;
    float ndc_y = dot(relative, previous_camera_up_cut.xyz) * inv_z * previous_camera_position_rcp_tan_y.w;
    out_uv = float2(ndc_x * 0.5 + 0.5, 0.5 - ndc_y * 0.5);
    return out_uv.x >= 0.0 && out_uv.x <= 1.0 && out_uv.y >= 0.0 && out_uv.y <= 1.0;
}

uint pack_vis(uint material_id, uint age, uint surface_hash) {
    return (material_id & 255u) | ((min(age, 255u) & 255u) << 8u) | ((surface_hash & 65535u) << 16u);
}

uint compute_surface_hash(float3 world_pos, float material) {
    if (material > 4.5 && material < 5.5) {
        return 50000u;
    }
    int3 cell = (int3)floor(world_pos * 1.5);
    uint h = (uint)(cell.x * 73856093 ^ cell.y * 19349663 ^ cell.z * 83492791);
    return h & 65535u;
}

[numthreads(16, 16, 1)]
void vis_classify_main(
    uint3 group_id : SV_GroupID,
    uint3 group_thread_id : SV_GroupThreadID,
    uint3 dispatch_thread_id : SV_DispatchThreadID) {
    if (group_thread_id.x == 0u && group_thread_id.y == 0u) {
        g_coarse_block_mask = 0u;
        g_dirty_pixel_count = 0u;
        g_disoccluded_count = 0u;
        g_moving_count = 0u;
        g_edge_count = 0u;
        g_non_sky_count = 0u;
        g_max_age_in_tile = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    uint viewport_width = cache_params.z;
    uint viewport_height = cache_params.w;
    bool in_bounds = dispatch_thread_id.x < viewport_width && dispatch_thread_id.y < viewport_height;

    bool pixel_dirty = false;
    bool pixel_disoccluded = false;
    bool pixel_moving = false;
    bool pixel_non_sky = false;
    bool needs_coarse_seed = false;
    uint pixel_age = 0u;
    uint priority_age = 0u;
    float linear_view_depth = 1000.0;
    float hit_distance = 0.0;
    float3 hit_world_position = camera_position_tan_y.xyz;
    uint mat_id = 0u;

    if (in_bounds) {
        float2 current_uv = (float2(dispatch_thread_id.xy) + 0.5) * float2(camera_up_rcp_width.w, camera_forward_rcp_height.w);
        float2 ndc = float2(current_uv.x * 2.0 - 1.0, 1.0 - current_uv.y * 2.0);
        float3 camera_position = camera_position_tan_y.xyz;
        float3 camera_right = camera_right_tan_x.xyz;
        float3 camera_up = camera_up_rcp_width.xyz;
        float3 camera_forward = camera_forward_rcp_height.xyz;
        float3 direction = normalize(
            camera_right * (ndc.x * camera_right_tan_x.w)
            + camera_up * (ndc.y * camera_position_tan_y.w)
            + camera_forward);
        float3 ball_center = ball_center_stress.xyz;

        float distance_travelled = 0.0;
        float material = 0.0;
        float3 world_position = camera_position;
        [loop]
        for (int step_index = 0; step_index < 88; ++step_index) {
            world_position = camera_position + direction * distance_travelled;
            float2 distance_sample = scene_distance(world_position, ball_center);
            if (distance_sample.x < 0.003) {
                material = distance_sample.y;
                break;
            }
            distance_travelled += distance_sample.x;
            if (distance_travelled > 36.0) break;
        }
        hit_distance = distance_travelled;
        hit_world_position = world_position;

        float2 motion_vector = float2(0.0, 0.0);
        mat_id = (uint)(material + 0.5);
        uint surf_hash = 0u;
        uint sky_flags_init = (uint)(previous_ball_center_unused.w + 0.5);
        float3 cached_color = ((sky_flags_init & 256u) != 0u)
            ? pbr_aces_filmic(pbr_env_cubemap.SampleLevel(linear_clamp_sampler, direction, 0.0).rgb * 0.45)
            : float3(0.035, 0.055, 0.07);
        bool cache_hit = false;
        bool has_prev_cache = (cache_params.x != 0u) && (previous_camera_up_cut.w < 0.5);

        if (material > 0.0) {
            pixel_non_sky = true;
            linear_view_depth = max(dot(world_position - camera_position, camera_forward), 0.01);
            uint s_flags = sky_flags_init & 255u;
            float s_time = previous_camera_forward_unused.w;
            bool use_reactive_inv = (policy_params.x & 4u) != 0u;
            bool use_shadow_inv = (policy_params.x & 8u) != 0u;

            uint mat_rev = 0u;
            bool reactive_mat = false;
            if ((s_flags & 2u) != 0u && mat_id >= 1u && mat_id <= 4u) {
                mat_rev = ((uint)(s_time * 60.0) + 1u) & 63u;
                reactive_mat = true;
            } else if ((s_flags & 16u) != 0u && (mat_id == 3u || mat_id == 4u)) {
                mat_rev = ((uint)(s_time * 60.0) + 1u) & 63u;
                reactive_mat = true;
            }

            uint shadow_sig = 0u;
            if ((s_flags & (8u | 32u)) != 0u) {
                float3 pt_pos = float3(
                    2.1 * sin(s_time * 1.9),
                    2.2 + 0.55 * cos(s_time * 1.4),
                    10.5 + 4.2 * cos(s_time * 1.1));
                if ((s_flags & 32u) != 0u) {
                    float hs = trace_hard_shadow(world_position + float3(0.0, 0.03, 0.0), pt_pos, ball_center);
                    shadow_sig |= (hs > 0.5) ? 1u : 0u;
                }
                if ((s_flags & 8u) != 0u) {
                    float3 to_pt = pt_pos - world_position;
                    float env = saturate(16.0 / (1.0 + 0.25 * max(dot(to_pt, to_pt), 0.30)));
                    uint q = min(31u, (uint)(env * 31.0 + 0.5));
                    shadow_sig |= (q << 1u);
                }
            }
            surf_hash = (mat_rev & 63u) | ((shadow_sig & 255u) << 6u);

            float3 previous_world_position = world_position;
            if (mat_id == 5u) {
                pixel_moving = true;
                previous_world_position = world_position - (ball_center - previous_ball_center_unused.xyz);
            }
            float2 previous_uv;
            float expected_prev_z;
            bool projected_ok = project_to_previous_uv(previous_world_position, previous_uv, expected_prev_z);
            if (previous_camera_up_cut.w < 0.5 && projected_ok) {
                motion_vector = previous_uv - current_uv;
            }

            if (has_prev_cache && projected_ok) {
                uint prev_w = max(prev_and_alloc_dims.x, 1u);
                uint prev_h = max(prev_and_alloc_dims.y, 1u);
                float2 prev_active_size = float2((float)prev_w, (float)prev_h);
                float2 alloc_size = max(float2((float)prev_and_alloc_dims.z, (float)prev_and_alloc_dims.w), prev_active_size);
                float2 inv_alloc_size = rcp(alloc_size);
                float2 prev_uv_min = 0.5 * inv_alloc_size;
                float2 prev_uv_max = max((prev_active_size - 0.5) * inv_alloc_size, prev_uv_min);
                int2 prev_coord = clamp(
                    int2(previous_uv * prev_active_size),
                    int2(0, 0),
                    int2((int)prev_w - 1, (int)prev_h - 1));
                float prev_z = prev_depth_texture.Load(int3(prev_coord, 0));
                uint prev_vis = prev_vis_texture.Load(int3(prev_coord, 0));
                uint prev_mat = prev_vis & 255u;
                uint prev_age = (prev_vis >> 8u) & 255u;
                uint prev_mat_rev = (prev_vis >> 16u) & 63u;
                uint prev_shadow_sig = (prev_vis >> 22u) & 255u;
                bool prev_authoritative = prev_age < 255u;
                bool reactive_invalid = use_reactive_inv && (reactive_mat || (prev_mat_rev != mat_rev));
                bool shadow_invalid = use_shadow_inv && (prev_shadow_sig != shadow_sig);
                if (reactive_invalid || shadow_invalid) {
                    pixel_moving = true;
                }
                if (policy_params.z != 0u) {
                    cached_color = sample_catmull_rom_5tap(
                        previous_uv,
                        prev_active_size,
                        inv_alloc_size,
                        prev_uv_min,
                        prev_uv_max);
                } else {
                    float2 prev_tex_uv = clamp(previous_uv * (prev_active_size * inv_alloc_size), prev_uv_min, prev_uv_max);
                    cached_color = prev_color_texture.SampleLevel(linear_clamp_sampler, prev_tex_uv, 0).rgb;
                }
                float rel_z_err = abs(prev_z - expected_prev_z) / max(expected_prev_z, 0.25);
                bool depth_match = rel_z_err < 0.035;
                bool mat_match = (prev_mat == mat_id);
                if (depth_match && mat_match && !pixel_moving && prev_authoritative && prev_age < cache_params.y) {
                    pixel_age = prev_age + 1u;
                    priority_age = pixel_age;
                    cache_hit = true;
                } else if (depth_match && mat_match && !pixel_moving && prev_authoritative) {
                    pixel_age = min(prev_age + 1u, 254u);
                    priority_age = pixel_age;
                } else {
                    pixel_age = 255u;
                    priority_age = cache_params.y;
                    needs_coarse_seed = true;
                    if (!pixel_moving) {
                        pixel_disoccluded = true;
                    }
                }
            } else {
                pixel_age = 255u;
                priority_age = cache_params.y;
                needs_coarse_seed = true;
                if (!pixel_moving) {
                    pixel_disoccluded = true;
                }
            }
            if (!cache_hit) {
                pixel_dirty = true;
            }
        } else {
            cache_hit = true;
            pixel_age = 0u;
            priority_age = 0u;
        }

        out_depth_uav[dispatch_thread_id.xy] = linear_view_depth;
        out_motion_uav[dispatch_thread_id.xy] = motion_vector;
        out_vis_uav[dispatch_thread_id.xy] = pack_vis(mat_id, pixel_age, surf_hash);
        out_color_uav[dispatch_thread_id.xy] = float4(cached_color, 1.0);

        if (needs_coarse_seed && policy_params.w != 0u) {
            uint block_idx = ((group_thread_id.y >> 2u) << 2u) | (group_thread_id.x >> 2u);
            uint unused_mask;
            InterlockedOr(g_coarse_block_mask, 1u << block_idx, unused_mask);
        }
    }

    g_tile_depth[group_thread_id.y][group_thread_id.x] = linear_view_depth;
    g_tile_mat[group_thread_id.y][group_thread_id.x] = mat_id;
    GroupMemoryBarrierWithGroupSync();

    if (policy_params.w != 0u && g_coarse_block_mask != 0u) {
        uint bx = group_thread_id.x >> 2u;
        uint by = group_thread_id.y >> 2u;
        uint block_idx = (by << 2u) | bx;
        if ((group_thread_id.x & 3u) == 1u && (group_thread_id.y & 3u) == 1u) {
            if (((g_coarse_block_mask >> block_idx) & 1u) != 0u && in_bounds && mat_id > 0u) {
                float3 ball_center = ball_center_stress.xyz;
                uint packed_flags = (uint)(previous_ball_center_unused.w + 0.5);
                if ((policy_params.x & 16u) != 0u) {
                    packed_flags &= ~128u;
                }
                if ((packed_flags & 256u) != 0u) {
                    float2 c_uv = (float2(dispatch_thread_id.xy) + 0.5) * float2(camera_up_rcp_width.w, camera_forward_rcp_height.w);
                    float2 c_ndc = float2(c_uv.x * 2.0 - 1.0, 1.0 - c_uv.y * 2.0);
                    float3 c_pos = camera_position_tan_y.xyz;
                    float3 c_right = camera_right_tan_x.xyz;
                    float3 c_up = camera_up_rcp_width.xyz;
                    float3 c_fwd = camera_forward_rcp_height.xyz;
                    float c_tx = camera_right_tan_x.w;
                    float c_ty = camera_position_tan_y.w;
                    float3 c_dir = normalize(c_right * (c_ndc.x * c_tx) + c_up * (c_ndc.y * c_ty) + c_fwd);
                    float3 c_dx = normalize(c_right * ((c_ndc.x + 8.0 * camera_up_rcp_width.w) * c_tx) + c_up * (c_ndc.y * c_ty) + c_fwd);
                    float3 c_dy = normalize(c_right * (c_ndc.x * c_tx) + c_up * ((c_ndc.y - 8.0 * camera_forward_rcp_height.w) * c_ty) + c_fwd);
                    float3 coarse_col = evaluate_pbr_shader_color(
                        hit_world_position,
                        c_pos,
                        c_dir,
                        c_dx,
                        c_dy,
                        hit_distance,
                        (float)mat_id,
                        ball_center,
                        previous_camera_forward_unused.w,
                        packed_flags,
                        linear_clamp_sampler);
                    g_coarse_color[by][bx] = max(coarse_col, 0.0);
                } else {
                    float3 normal = surface_normal(hit_world_position, ball_center);
                    float light = 0.35 + 0.65 * saturate(dot(normal, float3(-0.392232, 0.784465, -0.490290)));
                    float3 coarse_col = surface_color(hit_world_position, (float)mat_id, ball_center) * light;
                    coarse_col = lerp(coarse_col, float3(0.12, 0.17, 0.20), saturate(hit_distance * (1.0 / 55.0)));
                    g_coarse_color[by][bx] = max(coarse_col, 0.0);
                }
                g_coarse_mat[by][bx] = mat_id;
            } else {
                g_coarse_color[by][bx] = float3(0.035, 0.055, 0.07);
                g_coarse_mat[by][bx] = 0u;
            }
        }
        GroupMemoryBarrierWithGroupSync();

        if (needs_coarse_seed && in_bounds && mat_id > 0u) {
            float3 coarse_seed = (g_coarse_mat[by][bx] == mat_id)
                ? g_coarse_color[by][bx]
                : coarse_material_tint(mat_id);
            out_color_uav[dispatch_thread_id.xy] = float4(coarse_seed, 1.0);
        }
    }

    bool pixel_edge = false;
    if (in_bounds && group_thread_id.x < 15u && group_thread_id.y < 15u) {
        float dz_x = abs(linear_view_depth - g_tile_depth[group_thread_id.y][group_thread_id.x + 1u]);
        float dz_y = abs(linear_view_depth - g_tile_depth[group_thread_id.y + 1u][group_thread_id.x]);
        uint mat_x = g_tile_mat[group_thread_id.y][group_thread_id.x + 1u];
        uint mat_y = g_tile_mat[group_thread_id.y + 1u][group_thread_id.x];
        float rel_edge = max(dz_x, dz_y) / max(linear_view_depth, 0.5);
        pixel_edge = (rel_edge > 0.08) || (mat_x != mat_id) || (mat_y != mat_id);
    }

    uint wave_dirty = WaveActiveCountBits(pixel_dirty);
    uint wave_disoccl = WaveActiveCountBits(pixel_disoccluded);
    uint wave_moving = WaveActiveCountBits(pixel_moving);
    uint wave_edge = WaveActiveCountBits(pixel_edge);
    uint wave_non_sky = WaveActiveCountBits(pixel_non_sky);
    uint wave_max_age = WaveActiveMax(priority_age);
    if (WaveIsFirstLane()) {
        uint unused_prev;
        if (wave_dirty > 0u) InterlockedAdd(g_dirty_pixel_count, wave_dirty, unused_prev);
        if (wave_disoccl > 0u) InterlockedAdd(g_disoccluded_count, wave_disoccl, unused_prev);
        if (wave_moving > 0u) InterlockedAdd(g_moving_count, wave_moving, unused_prev);
        if (wave_edge > 0u) InterlockedAdd(g_edge_count, wave_edge, unused_prev);
        if (wave_non_sky > 0u) InterlockedAdd(g_non_sky_count, wave_non_sky, unused_prev);
        if (wave_max_age > 0u) InterlockedMax(g_max_age_in_tile, wave_max_age, unused_prev);
    }
    GroupMemoryBarrierWithGroupSync();

    if (group_thread_id.x == 0u && group_thread_id.y == 0u) {
        uint tiles_x = tile_params.x;
        uint tiles_y = tile_params.y;
        if (group_id.x < tiles_x && group_id.y < tiles_y) {
            uint frame_index = tile_params.w;
            uint refresh_period = max(cache_params.y, 4u);
            bool use_priority_stbn = (policy_params.x & 1u) != 0u;
            bool use_subtile_interleave = (policy_params.x & 2u) != 0u;
            bool has_dirty = g_dirty_pixel_count > 0u;
            uint tile_phase = use_priority_stbn
                ? ((lookup_stbn_phase(group_id.x, group_id.y) + frame_index) & 15u)
                : ((group_id.x * 7u + group_id.y * 13u + frame_index) % refresh_period);
            bool periodic_trigger = use_subtile_interleave ? ((tile_phase & 3u) == 0u) : (tile_phase == 0u);
            bool periodic_refresh = (!has_dirty) && (periodic_trigger || g_max_age_in_tile >= refresh_period);

            float disoccl_frac = float(g_disoccluded_count) * (1.0 / 256.0);
            float moving_frac = float(g_moving_count) * (1.0 / 256.0);
            float dirty_frac = float(g_dirty_pixel_count) * (1.0 / 256.0);
            float edge_frac = float(g_edge_count) * (1.0 / 256.0);
            float error_score = disoccl_frac * 10.0 + moving_frac * 7.5 + dirty_frac * 3.5 + edge_frac * 1.5;

            float2 tile_ndc = ((float2(group_id.xy) + 0.5) / float2(tiles_x, tiles_y)) * 2.0 - 1.0;
            float reticle_dist = length(tile_ndc);
            float saliency = reticle_dist < 0.28
                ? 2.0
                : (reticle_dist < 0.72 ? lerp(2.0, 1.0, (reticle_dist - 0.28) * (1.0 / 0.44)) : 0.55);

            float age_ratio = float(g_max_age_in_tile) / float(refresh_period);
            float age_urgency = 1.0 + age_ratio * 1.6 + (g_max_age_in_tile >= refresh_period ? 1.5 : 0.0);
            float cost = 0.5 + 0.5 * (float(g_non_sky_count) * (1.0 / 256.0));
            float priority = (max(error_score, 0.25) * saliency * age_urgency) / cost;
            uint prio_6bit = min(63u, (uint)(saturate(priority * (1.0 / 16.0)) * 63.0 + 0.5));

            uint unused_total;
            tile_indirect_args_uav.InterlockedAdd(12, 1u, unused_total);
            uint tile_id = group_id.y * tiles_x + group_id.x;

            if (has_dirty || periodic_refresh) {
                bool has_critical = (g_disoccluded_count > 0u) || (g_moving_count > 0u);
                uint bucket = 7u;
                if (has_critical && (priority >= 8.0 || reticle_dist < 0.40)) {
                    bucket = 0u;
                } else if (has_critical) {
                    bucket = 1u;
                } else if (has_dirty && priority >= 3.0) {
                    bucket = 2u;
                } else if (has_dirty) {
                    bucket = 3u;
                } else if (saliency >= 1.5) {
                    bucket = 4u;
                } else if (saliency >= 1.0) {
                    bucket = 5u;
                } else if (g_edge_count > 16u) {
                    bucket = 6u;
                } else {
                    bucket = 7u;
                }
                uint kind = has_dirty ? 1u : 2u;
                tile_candidate_meta_uav[tile_id] = 0x80000000u | (bucket << 24u) | (prio_6bit << 16u) | kind;
                uint unused_bucket;
                tile_indirect_args_uav.InterlockedAdd(32u + bucket * 4u, 1u, unused_bucket);
            } else {
                tile_candidate_meta_uav[tile_id] = (prio_6bit << 16u);
                out_tile_map_uav[group_id.xy] = (prio_6bit << 2u);
                uint unused_cached;
                tile_indirect_args_uav.InterlockedAdd(16, 1u, unused_cached);
            }
        }
    }
}

[numthreads(256, 1, 1)]
void tile_admit_main(uint3 dispatch_thread_id : SV_DispatchThreadID) {
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
            tile_indirect_args_uav.Store(0, full_admitted + inter_groups);
            tile_candidate_meta_uav[total_tiles] = inter_admitted;
        } else {
            tile_indirect_args_uav.Store(0, min(full_candidates + inter_candidates, tile_budget));
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
            uint unused_sched;
            tile_indirect_args_uav.InterlockedAdd(20, 1u, unused_sched);
        } else {
            out_tile_map_uav[uint2(tile_x, tile_y)] = 3u | (prio_6bit << 2u);
            uint unused_def;
            tile_indirect_args_uav.InterlockedAdd(24, 1u, unused_def);
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
        uint unused_sched;
        tile_indirect_args_uav.InterlockedAdd(20, 1u, unused_sched);
    } else {
        out_tile_map_uav[uint2(tile_x, tile_y)] = 3u | (prio_6bit << 2u);
        uint unused_def;
        tile_indirect_args_uav.InterlockedAdd(24, 1u, unused_def);
        if (bucket <= 1u) {
            uint unused_crit;
            tile_indirect_args_uav.InterlockedAdd(28, 1u, unused_crit);
        }
    }
}

[numthreads(16, 16, 1)]
void tile_shade_main(
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

    uint viewport_width = cache_params.z;
    uint viewport_height = cache_params.w;
    if (pixel_coord.x >= viewport_width || pixel_coord.y >= viewport_height) {
        return;
    }

    uint vis_word = out_vis_uav[pixel_coord];
    uint mat_id = vis_word & 255u;
    if (mat_id == 0u) {
        uint sky_flags = (uint)(previous_ball_center_unused.w + 0.5);
        float2 sky_uv = (float2(pixel_coord) + 0.5) * float2(camera_up_rcp_width.w, camera_forward_rcp_height.w);
        float2 sky_ndc = float2(sky_uv.x * 2.0 - 1.0, 1.0 - sky_uv.y * 2.0);
        float3 sky_dir = normalize(
            camera_right_tan_x.xyz * (sky_ndc.x * camera_right_tan_x.w)
            + camera_up_rcp_width.xyz * (sky_ndc.y * camera_position_tan_y.w)
            + camera_forward_rcp_height.xyz);
        float3 sky_col = ((sky_flags & 256u) != 0u)
            ? pbr_aces_filmic(pbr_env_cubemap.SampleLevel(linear_clamp_sampler, sky_dir, 0.0).rgb * 0.45)
            : float3(0.035, 0.055, 0.07);
        out_color_uav[pixel_coord] = float4(sky_col, 1.0);
        return;
    }

    float linear_view_depth = out_depth_uav[pixel_coord];
    float2 current_uv = (float2(pixel_coord) + 0.5) * float2(camera_up_rcp_width.w, camera_forward_rcp_height.w);
    float2 ndc = float2(current_uv.x * 2.0 - 1.0, 1.0 - current_uv.y * 2.0);
    float3 camera_position = camera_position_tan_y.xyz;
    float3 camera_right = camera_right_tan_x.xyz;
    float3 camera_up = camera_up_rcp_width.xyz;
    float3 camera_forward = camera_forward_rcp_height.xyz;
    float3 world_position = camera_position
        + camera_right * (ndc.x * camera_right_tan_x.w * linear_view_depth)
        + camera_up * (ndc.y * camera_position_tan_y.w * linear_view_depth)
        + camera_forward * linear_view_depth;

    float3 ball_center = ball_center_stress.xyz;
    float material = (float)mat_id;
    float3 to_hit = world_position - camera_position;
    float distance_travelled = length(to_hit);
    float3 ray_dir = to_hit / max(distance_travelled, 1e-4);
    float tan_x = camera_right_tan_x.w;
    float tan_y = camera_position_tan_y.w;
    float3 ray_dx = normalize(
        camera_right * ((ndc.x + 2.0 * camera_up_rcp_width.w) * tan_x)
        + camera_up * (ndc.y * tan_y)
        + camera_forward);
    float3 ray_dy = normalize(
        camera_right * (ndc.x * tan_x)
        + camera_up * ((ndc.y - 2.0 * camera_forward_rcp_height.w) * tan_y)
        + camera_forward);
    float scene_time = previous_camera_forward_unused.w;
    uint packed_flags = (uint)(previous_ball_center_unused.w + 0.5);
    if ((policy_params.x & 16u) != 0u) {
        packed_flags &= ~128u;
    }
    float3 color = evaluate_shader_color_unified(
        world_position,
        camera_position,
        ray_dir,
        ray_dx,
        ray_dy,
        distance_travelled,
        material,
        ball_center,
        scene_time,
        packed_flags,
        linear_clamp_sampler);

    int stress_level = (int)(ball_center_stress.w + 0.5);
    if (stress_level > 0) {
        color += synthetic_stress_modulation(world_position, stress_level);
    }

    uint prev_vis_age = (vis_word >> 8u) & 255u;
    if (policy_params.z != 0u && cache_params.x != 0u && prev_vis_age < 255u) {
        float3 prev_cached = out_color_uav[pixel_coord].rgb;
        float3 diff = color - prev_cached;
        if (length(diff) < 0.25) {
            float luma = dot(max(color, 0.0), float3(0.2126, 0.7152, 0.0722));
            float tcsf_limit = 0.08 * max(luma, 0.12);
            color += clamp(diff * 0.35, -tcsf_limit, tcsf_limit);
        }
    }

    out_color_uav[pixel_coord] = float4(max(color, 0.0), 1.0);
    out_vis_uav[pixel_coord] = (vis_word & 0xFFFF00FFu);
}
