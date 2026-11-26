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
RWByteAddressBuffer tile_indirect_args_uav : register(u6);

SamplerState linear_clamp_sampler : register(s0);
SamplerState point_clamp_sampler : register(s1);

groupshared uint g_dirty_pixel_count;
groupshared uint g_max_age_in_tile;

float box_distance(float3 world_position, float3 half_size) {
    float3 offset = abs(world_position) - half_size;
    return length(max(offset, 0.0)) + min(max(offset.x, max(offset.y, offset.z)), 0.0);
}

float2 scene_distance(float3 world_position, float3 ball_center) {
    float2 nearest = float2(world_position.y, 1.0);
    float wall_distance = 4.0 - abs(world_position.x);
    if (wall_distance < nearest.x) nearest = float2(wall_distance, 2.0);
    float ceiling_distance = 3.6 - world_position.y;
    if (ceiling_distance < nearest.x) nearest = float2(ceiling_distance, 2.0);
    float end_wall_distance = 28.0 - world_position.z;
    if (end_wall_distance < nearest.x) nearest = float2(end_wall_distance, 3.0);

    float3 pillar_bounds_offset = abs(world_position - float3(0.0, 1.6, 13.2)) - float3(1.60, 1.65, 6.50);
    float pillar_bounds_distance = length(max(pillar_bounds_offset, 0.0));
    if (pillar_bounds_distance < nearest.x) {
        [unroll]
        for (int pillar_index = 0; pillar_index < 5; ++pillar_index) {
            float pillar_z = 7.0 + pillar_index * 3.1;
            float pillar_x = (pillar_index % 2 == 0) ? -1.35 : 1.35;
            float pillar_distance = box_distance(world_position - float3(pillar_x, 1.6, pillar_z), float3(0.20, 1.6, 0.20));
            if (pillar_distance < nearest.x) nearest = float2(pillar_distance, 4.0);
        }
    }

    float ball_distance = length(world_position - ball_center) - 0.65;
    if (ball_distance < nearest.x) nearest = float2(ball_distance, 5.0);

    float rail_distance = box_distance(world_position - float3(2.6, 1.2, 18.0), float3(0.025, 1.2, 4.0));
    if (rail_distance < nearest.x) nearest = float2(rail_distance, 6.0);
    return nearest;
}

float3 surface_normal(float3 world_position, float3 ball_center) {
    const float2 offset = float2(0.003, -0.003);
    return normalize(
        offset.xyy * scene_distance(world_position + offset.xyy, ball_center).x +
        offset.yyx * scene_distance(world_position + offset.yyx, ball_center).x +
        offset.yxy * scene_distance(world_position + offset.yxy, ball_center).x +
        offset.xxx * scene_distance(world_position + offset.xxx, ball_center).x);
}

float3 surface_color(float3 world_position, float material, float3 ball_center) {
    if (material < 1.5) {
        float checker = fmod(floor(world_position.x * 1.5) + floor(world_position.z * 1.5) + 200.0, 2.0);
        float floor_marking = 1.0 - smoothstep(0.015, 0.04, abs(frac(world_position.z) - 0.5));
        return lerp(float3(0.11, 0.17, 0.20), float3(0.32, 0.40, 0.42), checker) + floor_marking * 0.08;
    }
    if (material < 2.5) {
        float stripe = step(0.94, frac(world_position.z * 0.5));
        return lerp(float3(0.19, 0.25, 0.29), float3(0.63, 0.72, 0.72), stripe);
    }
    if (material < 3.5) {
        float grid = max(step(0.90, frac(world_position.x * 3.0)), step(0.90, frac(world_position.y * 3.0)));
        return lerp(float3(0.18, 0.28, 0.34), float3(0.87, 0.90, 0.82), grid);
    }
    if (material < 4.5) {
        float bands = step(0.5, frac(world_position.y * 2.0));
        return lerp(float3(0.95, 0.45, 0.09), float3(0.14, 0.18, 0.20), bands);
    }
    if (material < 5.5) {
        float3 local = world_position - ball_center;
        float checker = fmod(floor((atan2(local.z, local.x) + 3.14159) * 2.5) + floor((local.y + 1.0) * 5.0), 2.0);
        return lerp(float3(0.13, 0.56, 0.68), float3(0.92, 0.95, 0.86), checker);
    }
    return float3(0.95, 0.77, 0.27);
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
        g_dirty_pixel_count = 0u;
        g_max_age_in_tile = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    uint viewport_width = cache_params.z;
    uint viewport_height = cache_params.w;
    bool in_bounds = dispatch_thread_id.x < viewport_width && dispatch_thread_id.y < viewport_height;

    bool pixel_dirty = false;
    uint pixel_age = 0u;

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

        float linear_view_depth = 1000.0;
        float2 motion_vector = float2(0.0, 0.0);
        uint mat_id = (uint)(material + 0.5);
        uint surf_hash = 0u;
        float3 cached_color = float3(0.035, 0.055, 0.07);
        bool cache_hit = false;
        bool has_prev_cache = (cache_params.x != 0u) && (previous_camera_up_cut.w < 0.5);

        if (material > 0.0) {
            linear_view_depth = max(dot(world_position - camera_position, camera_forward), 0.01);
            surf_hash = compute_surface_hash(world_position, material);

            float3 previous_world_position = world_position;
            if (material > 4.5 && material < 5.5) {
                previous_world_position = world_position - (ball_center - previous_ball_center_unused.xyz);
            }
            float2 previous_uv;
            float expected_prev_z;
            bool projected_ok = project_to_previous_uv(previous_world_position, previous_uv, expected_prev_z);
            if (previous_camera_up_cut.w < 0.5 && projected_ok) {
                motion_vector = previous_uv - current_uv;
            }

            if (has_prev_cache && projected_ok) {
                int2 prev_coord = clamp(
                    int2(previous_uv * float2((float)viewport_width, (float)viewport_height)),
                    int2(0, 0),
                    int2((int)viewport_width - 1, (int)viewport_height - 1));
                float prev_z = prev_depth_texture.Load(int3(prev_coord, 0));
                uint prev_vis = prev_vis_texture.Load(int3(prev_coord, 0));
                uint prev_mat = prev_vis & 255u;
                uint prev_age = (prev_vis >> 8u) & 255u;
                cached_color = prev_color_texture.SampleLevel(linear_clamp_sampler, previous_uv, 0).rgb;
                float rel_z_err = abs(prev_z - expected_prev_z) / max(expected_prev_z, 0.25);
                bool depth_match = rel_z_err < 0.035;
                bool mat_match = (prev_mat == mat_id);
                bool moving_ball = (mat_id == 5u);
                if (depth_match && mat_match && !moving_ball && prev_age < cache_params.y) {
                    pixel_age = prev_age + 1u;
                    cache_hit = true;
                } else {
                    pixel_age = min(prev_age + 1u, 255u);
                }
            }
            if (!cache_hit) {
                pixel_dirty = true;
            }
        } else {
            cache_hit = true;
            pixel_age = 0u;
        }

        out_depth_uav[dispatch_thread_id.xy] = linear_view_depth;
        out_motion_uav[dispatch_thread_id.xy] = motion_vector;
        out_vis_uav[dispatch_thread_id.xy] = pack_vis(mat_id, pixel_age, surf_hash);
        out_color_uav[dispatch_thread_id.xy] = float4(cached_color, 1.0);
    }

    uint wave_dirty = WaveActiveCountBits(pixel_dirty);
    uint wave_max_age = WaveActiveMax(pixel_age);
    if (WaveIsFirstLane()) {
        if (wave_dirty > 0u) {
            uint unused_prev;
            InterlockedAdd(g_dirty_pixel_count, wave_dirty, unused_prev);
        }
        if (wave_max_age > 0u) {
            uint unused_age;
            InterlockedMax(g_max_age_in_tile, wave_max_age, unused_age);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if (group_thread_id.x == 0u && group_thread_id.y == 0u) {
        uint tiles_x = tile_params.x;
        uint tiles_y = tile_params.y;
        if (group_id.x < tiles_x && group_id.y < tiles_y) {
            uint tile_budget = tile_params.z;
            uint frame_index = tile_params.w;
            uint refresh_period = max(cache_params.y, 4u);
            bool has_dirty = g_dirty_pixel_count > 0u;
            uint tile_phase = (group_id.x * 7u + group_id.y * 13u + frame_index) % refresh_period;
            bool periodic_refresh = (!has_dirty) && (tile_phase == 0u || g_max_age_in_tile >= refresh_period);

            uint unused_total;
            tile_indirect_args_uav.InterlockedAdd(12, 1u, unused_total);

            if (has_dirty || periodic_refresh) {
                uint effective_cap = has_dirty ? tile_budget : max(1u, (tile_budget * 3u) / 4u);
                uint current_scheduled = 0u;
                bool allocated = false;
                uint cur_val = tile_indirect_args_uav.Load(0);
                [loop]
                while (cur_val < effective_cap) {
                    uint prev_val = 0u;
                    tile_indirect_args_uav.InterlockedCompareExchange(0, cur_val, cur_val + 1u, prev_val);
                    if (prev_val == cur_val) {
                        current_scheduled = cur_val;
                        allocated = true;
                        break;
                    }
                    cur_val = prev_val;
                }
                if (allocated) {
                    dirty_tile_list_uav[current_scheduled] = (group_id.x & 65535u) | ((group_id.y & 65535u) << 16u);
                    out_tile_map_uav[group_id.xy] = has_dirty ? 1u : 2u;
                    uint unused_dirty;
                    tile_indirect_args_uav.InterlockedAdd(20, 1u, unused_dirty);
                } else {
                    out_tile_map_uav[group_id.xy] = 3u;
                    uint unused_def;
                    tile_indirect_args_uav.InterlockedAdd(24, 1u, unused_def);
                }
            } else {
                out_tile_map_uav[group_id.xy] = 0u;
                uint unused_cached;
                tile_indirect_args_uav.InterlockedAdd(16, 1u, unused_cached);
            }
        }
    }
}

[numthreads(16, 16, 1)]
void tile_shade_main(
    uint3 group_id : SV_GroupID,
    uint3 group_thread_id : SV_GroupThreadID) {
    uint packed_tile = dirty_tile_list_uav[group_id.x];
    uint tile_x = packed_tile & 65535u;
    uint tile_y = (packed_tile >> 16u) & 65535u;
    uint2 pixel_coord = uint2(tile_x * 16u + group_thread_id.x, tile_y * 16u + group_thread_id.y);

    uint viewport_width = cache_params.z;
    uint viewport_height = cache_params.w;
    if (pixel_coord.x >= viewport_width || pixel_coord.y >= viewport_height) {
        return;
    }

    uint vis_word = out_vis_uav[pixel_coord];
    uint mat_id = vis_word & 255u;
    if (mat_id == 0u) {
        out_color_uav[pixel_coord] = float4(0.035, 0.055, 0.07, 1.0);
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
    float distance_travelled = length(world_position - camera_position);
    float3 normal = surface_normal(world_position, ball_center);
    float light = 0.35 + 0.65 * saturate(dot(normal, float3(-0.392232, 0.784465, -0.490290)));
    float3 color = surface_color(world_position, material, ball_center) * light;
    color = lerp(color, float3(0.12, 0.17, 0.20), saturate(distance_travelled * (1.0 / 55.0)));

    int stress_level = (int)(ball_center_stress.w + 0.5);
    if (stress_level > 0) {
        color += synthetic_stress_modulation(world_position, stress_level);
    }

    out_color_uav[pixel_coord] = float4(max(color, 0.0), 1.0);
    out_vis_uav[pixel_coord] = (vis_word & 0xFFFF00FFu);
}
