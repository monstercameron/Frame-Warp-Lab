#ifndef PRESENT_VARIANT
#define PRESENT_VARIANT 4
#endif

cbuffer PresentConstants : register(b0) {
    float4 viewport_and_flags;
    float4 warp_flags_and_aspect;
    float4 synthesis_params;
    float4 source_camera_position_tan_y;
    float4 source_camera_right_rcp_width;
    float4 source_camera_up_rcp_height;
    float4 source_camera_forward_tan_x;
    float4 target_camera_position_tan_y;
    float4 target_camera_right_rcp_tan_x;
    float4 target_camera_up_rcp_tan_y;
    float4 target_camera_forward_tan_x;
    float4 previous_camera_position_tan_y;
    float4 previous_camera_right_rcp_tan_x;
    float4 previous_camera_up_rcp_tan_y;
    float4 previous_camera_forward_tan_x;
    float4 vrs_params;
    float4 reprojection_thresholds;
    float4 homography_col_x;
    float4 homography_col_y;
    float4 homography_col_z;
    float4 prev_presented_camera_position_tan_y;
    float4 prev_presented_camera_right_rcp_tan_x;
    float4 prev_presented_camera_up_rcp_tan_y;
    float4 prev_presented_camera_forward_reset;
    float4 dlss_motion_params;
    float4 source_subrect_params;
    float4 prev_and_alloc_params;
};

cbuffer HudConstants : register(b1) {
    float4 hud_panel_rect;
    float4 hud_graph_rect;
    float4 hud_graph_scale;
    uint4 hud_text_meta;
    uint4 hud_text_packed[16];
    float4 hud_graph_samples[128];
    float4 tactical_flags;
    float4 tactical_combat;
    uint4 tactical_ammo_hp;
    uint4 tactical_score_meta;
    float4 tactical_player_radar;
    float4 tactical_targets[8];
};

Texture2D<float4> scene_color_texture : register(t0);
Texture2D<float> scene_depth_texture : register(t1);
Texture2D<float2> scene_motion_texture : register(t2);
Texture2D<float4> prev_color_texture : register(t3);
Texture2D<float> prev_depth_texture : register(t4);
Texture2D<float2> prev_motion_texture : register(t5);
Texture2D<uint> scene_tile_map_texture : register(t6);
RWByteAddressBuffer reprojection_stats : register(u1);
SamplerState linear_clamp_sampler : register(s0);
SamplerState point_clamp_sampler : register(s1);

struct VertexOutput {
    float4 position : SV_Position;
};

struct ReprojectionSample {
    float2 source_uv;
    float linear_depth;
    bool valid;
};

VertexOutput vertex_main(uint vertex_id : SV_VertexID) {
    VertexOutput output;
    float2 corner = float2((vertex_id << 1) & 2, vertex_id & 2);
    output.position = float4(corner * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

float2 map_subrect_uv(float2 norm_uv, float2 uv_scale, float2 inv_alloc_size) {
    float2 uv_min = 0.5 * inv_alloc_size;
    float2 uv_max = max(uv_scale - 0.5 * inv_alloc_size, uv_min);
    return clamp(norm_uv * uv_scale, uv_min, uv_max);
}

float3 sample_catmull_rom_5tap_tex(Texture2D<float4> tex, float2 norm_uv, float2 active_size, float2 uv_scale, float2 inv_alloc_size) {
    float2 sample_pos = norm_uv * active_size;
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

    float2 uv_min = 0.5 * inv_alloc_size;
    float2 uv_max = max(uv_scale - 0.5 * inv_alloc_size, uv_min);
    float2 tc0 = clamp((tc1 - 1.0) * inv_alloc_size, uv_min, uv_max);
    float2 tc3 = clamp((tc1 + 2.0) * inv_alloc_size, uv_min, uv_max);
    float2 tc12 = clamp((tc1 + offset12) * inv_alloc_size, uv_min, uv_max);

    float3 s0 = tex.SampleLevel(linear_clamp_sampler, float2(tc12.x, tc0.y), 0).rgb;
    float3 s1 = tex.SampleLevel(linear_clamp_sampler, float2(tc0.x, tc12.y), 0).rgb;
    float3 s2 = tex.SampleLevel(linear_clamp_sampler, float2(tc12.x, tc12.y), 0).rgb;
    float3 s3 = tex.SampleLevel(linear_clamp_sampler, float2(tc3.x, tc12.y), 0).rgb;
    float3 s4 = tex.SampleLevel(linear_clamp_sampler, float2(tc12.x, tc3.y), 0).rgb;

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

float3 sample_cas_upscale_tex(Texture2D<float4> tex, float2 norm_uv, float2 active_size, float2 uv_scale, float2 inv_alloc_size) {
    float3 e = sample_catmull_rom_5tap_tex(tex, norm_uv, active_size, uv_scale, inv_alloc_size);
    float2 uv_min = 0.5 * inv_alloc_size;
    float2 uv_max = max(uv_scale - 0.5 * inv_alloc_size, uv_min);
    float2 center_uv = clamp(norm_uv * uv_scale, uv_min, uv_max);
    float3 b = tex.SampleLevel(linear_clamp_sampler, clamp(center_uv + float2(0.0, -inv_alloc_size.y), uv_min, uv_max), 0).rgb;
    float3 d = tex.SampleLevel(linear_clamp_sampler, clamp(center_uv + float2(-inv_alloc_size.x, 0.0), uv_min, uv_max), 0).rgb;
    float3 f = tex.SampleLevel(linear_clamp_sampler, clamp(center_uv + float2(inv_alloc_size.x, 0.0), uv_min, uv_max), 0).rgb;
    float3 h = tex.SampleLevel(linear_clamp_sampler, clamp(center_uv + float2(0.0, inv_alloc_size.y), uv_min, uv_max), 0).rgb;

    float3 min_rgb = min(min(min(d, e), min(f, b)), h);
    float3 max_rgb = max(max(max(d, e), max(f, b)), h);
    float3 rcp_max = rcp(max(max_rgb, float3(1e-4, 1e-4, 1e-4)));
    float3 amp = saturate(min(min_rgb, 1.0 - max_rgb) * rcp_max);
    amp = sqrt(amp);
    float3 w = amp * (-0.185);
    float3 result = (e + (b + d + f + h) * w) * rcp(1.0 + 4.0 * w);
    return clamp(result, min_rgb * 0.92, max_rgb * 1.06 + 0.01);
}

float3 sample_upscaled_color(Texture2D<float4> tex, float2 norm_uv, float2 active_size, float2 uv_scale, float2 inv_alloc_size, int filter_mode) {
    if (filter_mode == 1) {
        return sample_catmull_rom_5tap_tex(tex, norm_uv, active_size, uv_scale, inv_alloc_size);
    }
    if (filter_mode == 2) {
        return sample_cas_upscale_tex(tex, norm_uv, active_size, uv_scale, inv_alloc_size);
    }
    float2 tex_uv = map_subrect_uv(norm_uv, uv_scale, inv_alloc_size);
    if (filter_mode == 3) {
        return tex.SampleLevel(point_clamp_sampler, tex_uv, 0).rgb;
    }
    return tex.SampleLevel(linear_clamp_sampler, tex_uv, 0).rgb;
}

#if PRESENT_VARIANT != 0

bool is_viewmodel_tex(Texture2D<float4> tex, float2 norm_uv, float2 uv_scale, float2 inv_alloc_size) {
    return tex.SampleLevel(point_clamp_sampler, map_subrect_uv(norm_uv, uv_scale, inv_alloc_size), 0).a < -0.5;
}

bool is_viewmodel_guarded(Texture2D<float4> tex, float2 norm_uv, float2 texel_size, float2 uv_scale, float2 inv_alloc_size) {
    if (is_viewmodel_tex(tex, norm_uv, uv_scale, inv_alloc_size)) {
        return true;
    }
    float2 step_uv = texel_size * 2.5;
    if (is_viewmodel_tex(tex, saturate(norm_uv + float2(step_uv.x, 0.0)), uv_scale, inv_alloc_size)) return true;
    if (is_viewmodel_tex(tex, saturate(norm_uv - float2(step_uv.x, 0.0)), uv_scale, inv_alloc_size)) return true;
    if (is_viewmodel_tex(tex, saturate(norm_uv + float2(0.0, step_uv.y)), uv_scale, inv_alloc_size)) return true;
    if (is_viewmodel_tex(tex, saturate(norm_uv - float2(0.0, step_uv.y)), uv_scale, inv_alloc_size)) return true;
    if (is_viewmodel_tex(tex, saturate(norm_uv + float2(step_uv.x, step_uv.y)), uv_scale, inv_alloc_size)) return true;
    if (is_viewmodel_tex(tex, saturate(norm_uv + float2(-step_uv.x, step_uv.y)), uv_scale, inv_alloc_size)) return true;
    if (is_viewmodel_tex(tex, saturate(norm_uv + float2(step_uv.x, -step_uv.y)), uv_scale, inv_alloc_size)) return true;
    if (is_viewmodel_tex(tex, saturate(norm_uv + float2(-step_uv.x, -step_uv.y)), uv_scale, inv_alloc_size)) return true;
    return false;
}

bool project_world_to_camera_uv(
    float3 world_position,
    float3 camera_position,
    float3 camera_right,
    float3 camera_up,
    float3 camera_forward,
    float rcp_tan_half_x,
    float rcp_tan_half_y,
    out float2 out_uv,
    out float out_view_z) {
    float3 relative = world_position - camera_position;
    float view_z = dot(relative, camera_forward);
    out_view_z = view_z;
    if (view_z <= 0.01) {
        out_uv = float2(-1.0, -1.0);
        return false;
    }
    float inv_z = rcp(view_z);
    float ndc_x = dot(relative, camera_right) * inv_z * rcp_tan_half_x;
    float ndc_y = dot(relative, camera_up) * inv_z * rcp_tan_half_y;
    out_uv = float2(ndc_x * 0.5 + 0.5, 0.5 - ndc_y * 0.5);
    return out_uv.x >= 0.0 && out_uv.x <= 1.0 && out_uv.y >= 0.0 && out_uv.y <= 1.0;
}

float3 reconstruct_camera_world_position(
    float2 sample_uv,
    float linear_view_depth,
    float3 camera_position,
    float3 camera_right,
    float3 camera_up,
    float3 camera_forward,
    float tan_half_x,
    float tan_half_y) {
    float2 ndc = float2(sample_uv.x * 2.0 - 1.0, 1.0 - sample_uv.y * 2.0);
    return camera_position
        + camera_right * (ndc.x * tan_half_x * linear_view_depth)
        + camera_up * (ndc.y * tan_half_y * linear_view_depth)
        + camera_forward * linear_view_depth;
}

ReprojectionSample evaluate_rotation_homography(float2 target_uv) {
    ReprojectionSample result;
    result.source_uv = target_uv;
    result.linear_depth = 1000.0;
    result.valid = false;

    float2 target_ndc = float2(target_uv.x * 2.0 - 1.0, 1.0 - target_uv.y * 2.0);
    float3 projected = homography_col_x.xyz * target_ndc.x
        + homography_col_y.xyz * target_ndc.y
        + homography_col_z.xyz;
    if (projected.z <= 0.05) {
        return result;
    }
    float inv_z = rcp(projected.z);
    float2 src_uv = float2(projected.x * inv_z * 0.5 + 0.5, 0.5 - projected.y * inv_z * 0.5);
    result.source_uv = src_uv;
    result.valid = src_uv.x >= 0.0 && src_uv.x <= 1.0 && src_uv.y >= 0.0 && src_uv.y <= 1.0;
    return result;
}

ReprojectionSample evaluate_reprojection_camera(
    Texture2D<float> depth_texture,
    Texture2D<float4> color_texture,
    float3 ref_position,
    float3 ref_right,
    float3 ref_up,
    float3 ref_forward,
    float ref_tan_x,
    float ref_tan_y,
    float2 target_uv,
    float2 viewport_size,
    float2 texel_size,
    float2 depth_uv_scale,
    float2 inv_alloc_size,
    int iteration_count,
    bool check_neighbors) {
    ReprojectionSample result;
    result.source_uv = target_uv;
    result.linear_depth = 1000.0;
    result.valid = false;

    float2 target_ndc = float2(target_uv.x * 2.0 - 1.0, 1.0 - target_uv.y * 2.0);
    float3 target_origin = target_camera_position_tan_y.xyz;
    float3 target_ray = normalize(
        target_camera_right_rcp_tan_x.xyz * (target_ndc.x * target_camera_forward_tan_x.w)
        + target_camera_up_rcp_tan_y.xyz * (target_ndc.y * target_camera_position_tan_y.w)
        + target_camera_forward_tan_x.xyz);

    float ref_ray_z = dot(target_ray, ref_forward);
    if (ref_ray_z <= 0.05) {
        return result;
    }

    float rcp_ref_tan_x = rcp(ref_tan_x);
    float rcp_ref_tan_y = rcp(ref_tan_y);
    float inv_ref_ray_z = rcp(ref_ray_z);
    float2 initial_ndc = float2(
        dot(target_ray, ref_right) * inv_ref_ray_z * rcp_ref_tan_x,
        dot(target_ray, ref_up) * inv_ref_ray_z * rcp_ref_tan_y);
    float2 candidate_uv = float2(initial_ndc.x * 0.5 + 0.5, 0.5 - initial_ndc.y * 0.5);
    if (candidate_uv.x < 0.0 || candidate_uv.x > 1.0 || candidate_uv.y < 0.0 || candidate_uv.y > 1.0) {
        return result;
    }

    float sampled_depth = depth_texture.SampleLevel(point_clamp_sampler, map_subrect_uv(candidate_uv, depth_uv_scale, inv_alloc_size), 0);
    if (is_viewmodel_tex(color_texture, candidate_uv, depth_uv_scale, inv_alloc_size)) {
        sampled_depth = max(sampled_depth, 4.0);
    }
    float ray_distance = sampled_depth * rcp(max(ref_ray_z, 0.1));
    float2 projected_uv = candidate_uv;
    float projected_ref_z = sampled_depth;

    [unroll]
    for (int iteration = 0; iteration < 4; ++iteration) {
        if (iteration >= iteration_count) break;
        float3 candidate_world = target_origin + target_ray * ray_distance;
        if (!project_world_to_camera_uv(
                candidate_world,
                ref_position,
                ref_right,
                ref_up,
                ref_forward,
                rcp_ref_tan_x,
                rcp_ref_tan_y,
                projected_uv,
                projected_ref_z)) {
            return result;
        }
        sampled_depth = depth_texture.SampleLevel(point_clamp_sampler, map_subrect_uv(projected_uv, depth_uv_scale, inv_alloc_size), 0);
        if (is_viewmodel_tex(color_texture, projected_uv, depth_uv_scale, inv_alloc_size)) {
            break;
        }
        float3 surface_world = reconstruct_camera_world_position(
            projected_uv, sampled_depth, ref_position, ref_right, ref_up, ref_forward, ref_tan_x, ref_tan_y);
        ray_distance = max(dot(surface_world - target_origin, target_ray), 0.05);
    }

    if (is_viewmodel_guarded(color_texture, projected_uv, texel_size, depth_uv_scale, inv_alloc_size)) {
        result.source_uv = projected_uv;
        result.linear_depth = sampled_depth;
        result.valid = false;
        return result;
    }

    float3 matched_world = reconstruct_camera_world_position(
        projected_uv, sampled_depth, ref_position, ref_right, ref_up, ref_forward, ref_tan_x, ref_tan_y);
    float2 roundtrip_uv;
    float roundtrip_target_z;
    if (!project_world_to_camera_uv(
            matched_world,
            target_origin,
            target_camera_right_rcp_tan_x.xyz,
            target_camera_up_rcp_tan_y.xyz,
            target_camera_forward_tan_x.xyz,
            target_camera_right_rcp_tan_x.w,
            target_camera_up_rcp_tan_y.w,
            roundtrip_uv,
            roundtrip_target_z)) {
        return result;
    }

    float inv_safe_depth = rcp(max(sampled_depth, 0.25));
    float pixel_error = length((roundtrip_uv - target_uv) * viewport_size);
    float relative_depth_error = abs(projected_ref_z - sampled_depth) * inv_safe_depth;

    bool consistent = (pixel_error <= reprojection_thresholds.x) && (relative_depth_error <= reprojection_thresholds.y);
    if (check_neighbors && consistent) {
        float depth_left = depth_texture.SampleLevel(point_clamp_sampler, map_subrect_uv(projected_uv - float2(texel_size.x, 0.0), depth_uv_scale, inv_alloc_size), 0);
        float depth_right = depth_texture.SampleLevel(point_clamp_sampler, map_subrect_uv(projected_uv + float2(texel_size.x, 0.0), depth_uv_scale, inv_alloc_size), 0);
        float depth_up = depth_texture.SampleLevel(point_clamp_sampler, map_subrect_uv(projected_uv - float2(0.0, texel_size.y), depth_uv_scale, inv_alloc_size), 0);
        float depth_down = depth_texture.SampleLevel(point_clamp_sampler, map_subrect_uv(projected_uv + float2(0.0, texel_size.y), depth_uv_scale, inv_alloc_size), 0);
        float max_neighbor_jump = max(
            max(abs(depth_left - sampled_depth), abs(depth_right - sampled_depth)),
            max(abs(depth_up - sampled_depth), abs(depth_down - sampled_depth))) * inv_safe_depth;
        if (max_neighbor_jump > reprojection_thresholds.z && pixel_error > reprojection_thresholds.w) {
            consistent = false;
        }
    }
    result.source_uv = projected_uv;
    result.linear_depth = sampled_depth;
    result.valid = consistent;
    return result;
}

float3 sample_conservative_hole_fill(float2 sample_uv, float2 texel_size, float2 src_uv_scale, float2 inv_alloc_size, out float out_bg_depth, out float2 out_bg_uv) {
    float2 clamped_uv = saturate(sample_uv);
    bool center_vm = is_viewmodel_tex(scene_color_texture, clamped_uv, src_uv_scale, inv_alloc_size);
    float best_depth = center_vm
        ? -1.0
        : scene_depth_texture.SampleLevel(point_clamp_sampler, map_subrect_uv(clamped_uv, src_uv_scale, inv_alloc_size), 0);
    float2 best_bg_uv = clamped_uv;
    static const float2 dirs[8] = {
        float2(-1.0,  0.0), float2( 1.0,  0.0), float2( 0.0, -1.0), float2( 0.0,  1.0),
        float2(-0.7, -0.7), float2( 0.7, -0.7), float2(-0.7,  0.7), float2( 0.7,  0.7)
    };
    static const float rings[3] = { 4.0, 14.0, 32.0 };
    [unroll]
    for (int r = 0; r < 3; ++r) {
        float2 step_uv = texel_size * rings[r];
        [unroll]
        for (int b = 0; b < 8; ++b) {
            float2 tap_uv = saturate(clamped_uv + dirs[b] * step_uv);
            if (is_viewmodel_tex(scene_color_texture, tap_uv, src_uv_scale, inv_alloc_size)) {
                continue;
            }
            float tap_depth = scene_depth_texture.SampleLevel(point_clamp_sampler, map_subrect_uv(tap_uv, src_uv_scale, inv_alloc_size), 0);
            if (tap_depth > best_depth) {
                best_depth = tap_depth;
                best_bg_uv = tap_uv;
            }
        }
        if (best_depth > 0.0 && !center_vm) {
            break;
        }
        if (best_depth > 0.0 && r >= 1) {
            break;
        }
    }
    if (best_depth < 0.0) {
        best_depth = scene_depth_texture.SampleLevel(point_clamp_sampler, map_subrect_uv(clamped_uv, src_uv_scale, inv_alloc_size), 0);
        best_bg_uv = clamped_uv;
    }
    out_bg_depth = best_depth;
    out_bg_uv = best_bg_uv;
    return pow(saturate(scene_color_texture.SampleLevel(point_clamp_sampler, map_subrect_uv(best_bg_uv, src_uv_scale, inv_alloc_size), 0).rgb), 1.0 / 2.2);
}

#endif

#if PRESENT_VARIANT == 4

float2 sample_independent_object_motion(float2 sample_uv, float2 src_uv_scale, float2 inv_alloc_size) {
    float2 mapped_uv = map_subrect_uv(sample_uv, src_uv_scale, inv_alloc_size);
    float2 total_mv = scene_motion_texture.SampleLevel(point_clamp_sampler, mapped_uv, 0);
    float depth_val = scene_depth_texture.SampleLevel(point_clamp_sampler, mapped_uv, 0);
    float3 world_pos = reconstruct_camera_world_position(
        sample_uv,
        depth_val,
        source_camera_position_tan_y.xyz,
        source_camera_right_rcp_width.xyz,
        source_camera_up_rcp_height.xyz,
        source_camera_forward_tan_x.xyz,
        source_camera_forward_tan_x.w,
        source_camera_position_tan_y.w);
    float2 static_prev_uv;
    float static_prev_z;
    if (!project_world_to_camera_uv(
            world_pos,
            previous_camera_position_tan_y.xyz,
            previous_camera_right_rcp_tan_x.xyz,
            previous_camera_up_rcp_tan_y.xyz,
            previous_camera_forward_tan_x.xyz,
            previous_camera_right_rcp_tan_x.w,
            previous_camera_up_rcp_tan_y.w,
            static_prev_uv,
            static_prev_z)) {
        return float2(0.0, 0.0);
    }
    float2 static_mv = static_prev_uv - sample_uv;
    return total_mv - static_mv;
}

ReprojectionSample evaluate_frame_gen(float2 uv, float2 viewport_size, float2 texel_size, float2 src_uv_scale, float2 inv_alloc_size, float alpha) {
    ReprojectionSample result;
    result.source_uv = uv;
    result.linear_depth = 1000.0;
    result.valid = false;

    float2 search_radius = texel_size * 4.0;
    float2 best_uv = uv;
    float nearest_depth = scene_depth_texture.SampleLevel(point_clamp_sampler, map_subrect_uv(uv, src_uv_scale, inv_alloc_size), 0);
    float2 offsets[4] = {
        float2(-search_radius.x, 0.0),
        float2(search_radius.x, 0.0),
        float2(0.0, -search_radius.y),
        float2(0.0, search_radius.y)
    };
    [unroll]
    for (int i = 0; i < 4; ++i) {
        float2 tap_uv = saturate(uv + offsets[i]);
        if (is_viewmodel_tex(scene_color_texture, tap_uv, src_uv_scale, inv_alloc_size)) {
            continue;
        }
        float tap_depth = scene_depth_texture.SampleLevel(point_clamp_sampler, map_subrect_uv(tap_uv, src_uv_scale, inv_alloc_size), 0);
        if (tap_depth < nearest_depth) {
            nearest_depth = tap_depth;
            best_uv = tap_uv;
        }
    }

    float2 mv = scene_motion_texture.SampleLevel(point_clamp_sampler, map_subrect_uv(best_uv, src_uv_scale, inv_alloc_size), 0);
    float2 candidate_uv = uv + mv * alpha;
    if (candidate_uv.x < 0.0 || candidate_uv.x > 1.0 || candidate_uv.y < 0.0 || candidate_uv.y > 1.0) {
        return result;
    }

    if (is_viewmodel_tex(scene_color_texture, candidate_uv, src_uv_scale, inv_alloc_size)) {
        result.source_uv = candidate_uv;
        result.valid = false;
        return result;
    }

    float2 refined_mv = scene_motion_texture.SampleLevel(point_clamp_sampler, map_subrect_uv(candidate_uv, src_uv_scale, inv_alloc_size), 0);
    float2 final_uv = uv + refined_mv * alpha;
    if (final_uv.x < 0.0 || final_uv.x > 1.0 || final_uv.y < 0.0 || final_uv.y > 1.0) {
        return result;
    }

    if (is_viewmodel_guarded(scene_color_texture, final_uv, texel_size, src_uv_scale, inv_alloc_size)) {
        result.source_uv = final_uv;
        result.valid = false;
        return result;
    }

    float2 verified_mv = scene_motion_texture.SampleLevel(point_clamp_sampler, map_subrect_uv(final_uv, src_uv_scale, inv_alloc_size), 0);
    float sampled_depth = scene_depth_texture.SampleLevel(point_clamp_sampler, map_subrect_uv(final_uv, src_uv_scale, inv_alloc_size), 0);
    float motion_disagreement_px = length((verified_mv - refined_mv) * alpha * viewport_size);
    float inv_safe_depth = rcp(max(sampled_depth, 0.25));
    float depth_l = scene_depth_texture.SampleLevel(point_clamp_sampler, map_subrect_uv(final_uv - float2(texel_size.x, 0.0), src_uv_scale, inv_alloc_size), 0);
    float depth_r = scene_depth_texture.SampleLevel(point_clamp_sampler, map_subrect_uv(final_uv + float2(texel_size.x, 0.0), src_uv_scale, inv_alloc_size), 0);
    float depth_u = scene_depth_texture.SampleLevel(point_clamp_sampler, map_subrect_uv(final_uv - float2(0.0, texel_size.y), src_uv_scale, inv_alloc_size), 0);
    float depth_d = scene_depth_texture.SampleLevel(point_clamp_sampler, map_subrect_uv(final_uv + float2(0.0, texel_size.y), src_uv_scale, inv_alloc_size), 0);
    float max_jump = max(
        max(abs(depth_l - sampled_depth), abs(depth_r - sampled_depth)),
        max(abs(depth_u - sampled_depth), abs(depth_d - sampled_depth))) * inv_safe_depth;

    bool consistent = (motion_disagreement_px <= 3.5) && !(max_jump > 0.22 && motion_disagreement_px > 1.0);
    result.source_uv = final_uv;
    result.linear_depth = sampled_depth;
    result.valid = consistent;
    return result;
}

#endif

uint pack_rows_0_to_3(uint r0, uint r1, uint r2, uint r3) {
    return (r0 & 31u) | ((r1 & 31u) << 5u) | ((r2 & 31u) << 10u) | ((r3 & 31u) << 15u);
}

uint pack_rows_4_to_6(uint r4, uint r5, uint r6) {
    return (r4 & 31u) | ((r5 & 31u) << 5u) | ((r6 & 31u) << 10u);
}

uint2 glyph_bitmap_5x7(uint ascii_code) {
    if (ascii_code == 48u) return uint2(pack_rows_0_to_3(14, 17, 19, 21), pack_rows_4_to_6(25, 17, 14));
    if (ascii_code == 49u) return uint2(pack_rows_0_to_3(4, 12, 4, 4), pack_rows_4_to_6(4, 4, 14));
    if (ascii_code == 50u) return uint2(pack_rows_0_to_3(14, 17, 1, 2), pack_rows_4_to_6(4, 8, 31));
    if (ascii_code == 51u) return uint2(pack_rows_0_to_3(30, 1, 1, 14), pack_rows_4_to_6(1, 1, 30));
    if (ascii_code == 52u) return uint2(pack_rows_0_to_3(2, 6, 10, 18), pack_rows_4_to_6(31, 2, 2));
    if (ascii_code == 53u) return uint2(pack_rows_0_to_3(31, 16, 30, 1), pack_rows_4_to_6(1, 17, 14));
    if (ascii_code == 54u) return uint2(pack_rows_0_to_3(14, 16, 30, 17), pack_rows_4_to_6(17, 17, 14));
    if (ascii_code == 55u) return uint2(pack_rows_0_to_3(31, 1, 2, 4), pack_rows_4_to_6(8, 8, 8));
    if (ascii_code == 56u) return uint2(pack_rows_0_to_3(14, 17, 17, 14), pack_rows_4_to_6(17, 17, 14));
    if (ascii_code == 57u) return uint2(pack_rows_0_to_3(14, 17, 17, 15), pack_rows_4_to_6(1, 2, 12));

    if (ascii_code == 65u) return uint2(pack_rows_0_to_3(14, 17, 17, 31), pack_rows_4_to_6(17, 17, 17));
    if (ascii_code == 66u) return uint2(pack_rows_0_to_3(30, 17, 17, 30), pack_rows_4_to_6(17, 17, 30));
    if (ascii_code == 67u) return uint2(pack_rows_0_to_3(14, 17, 16, 16), pack_rows_4_to_6(16, 17, 14));
    if (ascii_code == 68u) return uint2(pack_rows_0_to_3(30, 17, 17, 17), pack_rows_4_to_6(17, 17, 30));
    if (ascii_code == 69u) return uint2(pack_rows_0_to_3(31, 16, 16, 30), pack_rows_4_to_6(16, 16, 31));
    if (ascii_code == 70u) return uint2(pack_rows_0_to_3(31, 16, 16, 30), pack_rows_4_to_6(16, 16, 16));
    if (ascii_code == 71u) return uint2(pack_rows_0_to_3(14, 17, 16, 23), pack_rows_4_to_6(17, 17, 14));
    if (ascii_code == 72u) return uint2(pack_rows_0_to_3(17, 17, 17, 31), pack_rows_4_to_6(17, 17, 17));
    if (ascii_code == 73u) return uint2(pack_rows_0_to_3(14, 4, 4, 4), pack_rows_4_to_6(4, 4, 14));
    if (ascii_code == 74u) return uint2(pack_rows_0_to_3(7, 2, 2, 2), pack_rows_4_to_6(18, 18, 12));
    if (ascii_code == 75u) return uint2(pack_rows_0_to_3(17, 18, 20, 24), pack_rows_4_to_6(20, 18, 17));
    if (ascii_code == 76u) return uint2(pack_rows_0_to_3(16, 16, 16, 16), pack_rows_4_to_6(16, 16, 31));
    if (ascii_code == 77u) return uint2(pack_rows_0_to_3(17, 27, 21, 21), pack_rows_4_to_6(17, 17, 17));
    if (ascii_code == 78u) return uint2(pack_rows_0_to_3(17, 25, 21, 19), pack_rows_4_to_6(17, 17, 17));
    if (ascii_code == 79u) return uint2(pack_rows_0_to_3(14, 17, 17, 17), pack_rows_4_to_6(17, 17, 14));
    if (ascii_code == 80u) return uint2(pack_rows_0_to_3(30, 17, 17, 30), pack_rows_4_to_6(16, 16, 16));
    if (ascii_code == 81u) return uint2(pack_rows_0_to_3(14, 17, 17, 17), pack_rows_4_to_6(21, 18, 13));
    if (ascii_code == 82u) return uint2(pack_rows_0_to_3(30, 17, 17, 30), pack_rows_4_to_6(20, 18, 17));
    if (ascii_code == 83u) return uint2(pack_rows_0_to_3(15, 16, 16, 14), pack_rows_4_to_6(1, 1, 30));
    if (ascii_code == 84u) return uint2(pack_rows_0_to_3(31, 4, 4, 4), pack_rows_4_to_6(4, 4, 4));
    if (ascii_code == 85u) return uint2(pack_rows_0_to_3(17, 17, 17, 17), pack_rows_4_to_6(17, 17, 14));
    if (ascii_code == 86u) return uint2(pack_rows_0_to_3(17, 17, 17, 17), pack_rows_4_to_6(17, 10, 4));
    if (ascii_code == 87u) return uint2(pack_rows_0_to_3(17, 17, 17, 21), pack_rows_4_to_6(21, 27, 17));
    if (ascii_code == 88u) return uint2(pack_rows_0_to_3(17, 17, 10, 4), pack_rows_4_to_6(10, 17, 17));
    if (ascii_code == 89u) return uint2(pack_rows_0_to_3(17, 17, 10, 4), pack_rows_4_to_6(4, 4, 4));
    if (ascii_code == 90u) return uint2(pack_rows_0_to_3(31, 1, 2, 4), pack_rows_4_to_6(8, 16, 31));

    if (ascii_code == 46u) return uint2(pack_rows_0_to_3(0, 0, 0, 0), pack_rows_4_to_6(0, 12, 12));
    if (ascii_code == 58u) return uint2(pack_rows_0_to_3(0, 12, 12, 0), pack_rows_4_to_6(12, 12, 0));
    if (ascii_code == 47u) return uint2(pack_rows_0_to_3(1, 1, 2, 4), pack_rows_4_to_6(8, 16, 16));
    if (ascii_code == 37u) return uint2(pack_rows_0_to_3(25, 25, 2, 4), pack_rows_4_to_6(8, 19, 19));
    if (ascii_code == 45u) return uint2(pack_rows_0_to_3(0, 0, 0, 31), pack_rows_4_to_6(0, 0, 0));
    if (ascii_code == 43u) return uint2(pack_rows_0_to_3(0, 4, 4, 31), pack_rows_4_to_6(4, 4, 0));
    if (ascii_code == 91u) return uint2(pack_rows_0_to_3(14, 8, 8, 8), pack_rows_4_to_6(8, 8, 14));
    if (ascii_code == 93u) return uint2(pack_rows_0_to_3(14, 2, 2, 2), pack_rows_4_to_6(2, 2, 14));
    if (ascii_code == 95u) return uint2(pack_rows_0_to_3(0, 0, 0, 0), pack_rows_4_to_6(0, 0, 31));
    return uint2(0u, 0u);
}

bool sample_glyph_pixel(uint ascii_code, uint local_x, uint local_y) {
    if (ascii_code <= 32u || local_x >= 5u || local_y >= 7u) return false;
    uint2 bits = glyph_bitmap_5x7(ascii_code);
    uint row_bits = (local_y < 4u)
        ? ((bits.x >> (local_y * 5u)) & 31u)
        : ((bits.y >> ((local_y - 4u) * 5u)) & 31u);
    uint column_mask = 1u << (4u - local_x);
    return (row_bits & column_mask) != 0u;
}

uint read_hud_char(uint line_index, uint col_index) {
    uint char_index = line_index * 64u + col_index;
    uint word_index = char_index >> 2u;
    uint vec_index = word_index >> 2u;
    uint comp_index = word_index & 3u;
    uint byte_shift = (char_index & 3u) * 8u;
    uint4 vector_word = hud_text_packed[vec_index];
    uint word = (comp_index == 0u) ? vector_word.x : (comp_index == 1u) ? vector_word.y : (comp_index == 2u) ? vector_word.z : vector_word.w;
    return (word >> byte_shift) & 255u;
}

float3 frame_kind_color(float frame_kind) {
    if (frame_kind < 0.5) return float3(0.28, 0.84, 0.96);
    if (frame_kind < 1.5) return float3(0.96, 0.74, 0.24);
    if (frame_kind < 2.5) return float3(0.26, 0.92, 0.54);
    if (frame_kind < 3.5) return float3(0.98, 0.26, 0.68);
    if (frame_kind < 4.5) return float3(0.68, 0.46, 0.98);
    return float3(0.14, 0.96, 0.78);
}

float3 composite_hud_layer(float2 pixel_pos, float3 base_color) {
    float2 panel_min = hud_panel_rect.xy;
    float2 panel_size = hud_panel_rect.zw;
    float2 panel_max = panel_min + panel_size;
    if (panel_size.x <= 1.0 || panel_size.y <= 1.0) return base_color;
    if (pixel_pos.x < panel_min.x || pixel_pos.x >= panel_max.x || pixel_pos.y < panel_min.y || pixel_pos.y >= panel_max.y) {
        return base_color;
    }

    float3 result = lerp(base_color, float3(0.05, 0.08, 0.11), 0.80);
    bool on_panel_border = pixel_pos.x < panel_min.x + 1.5 || pixel_pos.x >= panel_max.x - 1.5
        || pixel_pos.y < panel_min.y + 1.5 || pixel_pos.y >= panel_max.y - 1.5;
    if (on_panel_border) {
        result = lerp(result, float3(0.32, 0.48, 0.58), 0.75);
    }

    uint line_count = hud_text_meta.x;
    uint chars_per_line = hud_text_meta.y;
    uint char_scale = max(hud_text_meta.z, 1u);
    float2 text_origin = panel_min + float2(10.0, 8.0);
    float char_advance_x = 6.0 * (float)char_scale;
    float line_advance_y = 9.0 * (float)char_scale;
    float2 text_local = pixel_pos - text_origin;
    if (text_local.x >= 0.0 && text_local.y >= 0.0) {
        uint line_index = (uint)(text_local.y / line_advance_y);
        uint col_index = (uint)(text_local.x / char_advance_x);
        if (line_index < line_count && col_index < chars_per_line) {
            uint glyph_x = (uint)((text_local.x - (float)col_index * char_advance_x) / (float)char_scale);
            uint glyph_y = (uint)((text_local.y - (float)line_index * line_advance_y) / (float)char_scale);
            uint ascii_code = read_hud_char(line_index, col_index);
            if (sample_glyph_pixel(ascii_code, glyph_x, glyph_y)) {
                float3 text_color = (line_index == 0u) ? float3(0.96, 0.99, 1.0)
                    : (line_index == 1u) ? float3(0.76, 0.90, 0.96)
                    : (line_index == 2u) ? float3(0.84, 0.88, 0.80)
                    : float3(0.62, 0.72, 0.78);
                return text_color;
            }
        }
    }

    float2 graph_min = hud_graph_rect.xy;
    float2 graph_size = hud_graph_rect.zw;
    float2 graph_max = graph_min + graph_size;
    if (pixel_pos.x >= graph_min.x && pixel_pos.x < graph_max.x && pixel_pos.y >= graph_min.y && pixel_pos.y < graph_max.y) {
        result = lerp(base_color, float3(0.03, 0.05, 0.07), 0.90);
        float max_ms = max(hud_graph_scale.x, 1.0);
        float target_ms = hud_graph_scale.y;
        float norm_x = saturate((pixel_pos.x - graph_min.x) / graph_size.x);
        float pixel_ms = saturate((graph_max.y - pixel_pos.y) / graph_size.y) * max_ms;

        float ref_120_y = graph_max.y - saturate(target_ms / max_ms) * graph_size.y;
        float ref_60_y = graph_max.y - saturate(16.667 / max_ms) * graph_size.y;
        if (abs(pixel_pos.y - ref_120_y) < 0.75) {
            result = lerp(result, float3(0.24, 0.65, 0.42), 0.70);
        }
        if (abs(pixel_pos.y - ref_60_y) < 0.75) {
            result = lerp(result, float3(0.70, 0.50, 0.22), 0.60);
        }

        uint sample_slot = min((uint)(norm_x * 128.0), 127u);
        uint write_head = (uint)(hud_graph_scale.w + 0.5);
        uint history_index = (write_head + sample_slot) & 127u;
        float4 sample_data = hud_graph_samples[history_index];
        float present_ms = sample_data.x;
        float gpu_ms = sample_data.y;
        float3 bar_color = frame_kind_color(sample_data.z);

        if (present_ms > 0.0) {
            float sample_y = graph_max.y - saturate(present_ms / max_ms) * graph_size.y;
            if (pixel_pos.y >= sample_y) {
                result = lerp(result, bar_color, 0.32);
            }
            if (abs(pixel_pos.y - sample_y) < 1.35) {
                result = bar_color;
            }
        }
        if (gpu_ms > 0.0 && pixel_ms <= gpu_ms) {
            result = lerp(result, float3(0.98, 0.94, 0.48), 0.75);
        }

        bool on_graph_border = pixel_pos.x < graph_min.x + 1.0 || pixel_pos.x >= graph_max.x - 1.0
            || pixel_pos.y < graph_min.y + 1.0 || pixel_pos.y >= graph_max.y - 1.0;
        if (on_graph_border) {
            result = float3(0.26, 0.38, 0.46);
        }
    }

    return result;
}

bool draw_uint_digits(float2 pixel_pos, float2 origin, uint value, uint scale, out bool hit_outline) {
    hit_outline = false;
    uint digits[6];
    uint count = 0u;
    uint v = value;
    do {
        digits[count++] = 48u + (v % 10u);
        v /= 10u;
    } while (v > 0u && count < 6u);

    float adv = 6.0 * (float)scale;
    float2 rel = pixel_pos - origin;
    if (rel.x < -1.0 || rel.y < -1.0 || rel.x >= adv * (float)count + 1.0 || rel.y >= 7.0 * (float)scale + 1.0) {
        return false;
    }
    if (rel.x >= 0.0 && rel.y >= 0.0 && rel.x < adv * (float)count && rel.y < 7.0 * (float)scale) {
        uint idx = (uint)(rel.x / adv);
        uint rev = count - 1u - idx;
        uint gx = (uint)((rel.x - (float)idx * adv) / (float)scale);
        uint gy = (uint)(rel.y / (float)scale);
        if (sample_glyph_pixel(digits[rev], gx, gy)) {
            return true;
        }
    }
    return false;
}

bool draw_label_4(float2 pixel_pos, float2 origin, uint c0, uint c1, uint c2, uint c3, uint scale) {
    float adv = 6.0 * (float)scale;
    float2 rel = pixel_pos - origin;
    if (rel.x < 0.0 || rel.y < 0.0 || rel.x >= adv * 4.0 || rel.y >= 7.0 * (float)scale) return false;
    uint idx = (uint)(rel.x / adv);
    uint gx = (uint)((rel.x - (float)idx * adv) / (float)scale);
    uint gy = (uint)(rel.y / (float)scale);
    uint ch = (idx == 0u) ? c0 : (idx == 1u) ? c1 : (idx == 2u) ? c2 : c3;
    return sample_glyph_pixel(ch, gx, gy);
}

float3 composite_tactical_hud(float2 pixel_pos, float2 viewport_size, float3 color) {
    float ads_alpha = tactical_flags.y;
    float spread = tactical_flags.z;
    float hm_alpha = tactical_flags.w;
    bool hm_headshot = tactical_combat.x > 0.5;
    float reload_prog = tactical_combat.y;

    float2 center = viewport_size * 0.5;
    float2 dc = pixel_pos - center;
    float2 adc = abs(dc);

    float ch_vis = saturate(1.0 - ads_alpha * 0.88);
    if (ch_vis > 0.02) {
        float gap = 5.0 + spread * 18.0;
        float arm_len = 8.5;
        bool h_in = adc.y < 1.15 && adc.x >= gap && adc.x <= gap + arm_len;
        bool v_in = adc.x < 1.15 && adc.y >= gap && adc.y <= gap + arm_len;
        bool h_out = adc.y < 2.25 && adc.x >= gap - 1.1 && adc.x <= gap + arm_len + 1.1;
        bool v_out = adc.x < 2.25 && adc.y >= gap - 1.1 && adc.y <= gap + arm_len + 1.1;
        if (h_in || v_in) {
            color = lerp(color, float3(0.20, 1.0, 0.34), ch_vis);
        } else if (h_out || v_out) {
            color = lerp(color, float3(0.02, 0.03, 0.03), ch_vis * 0.85);
        }
    }

    if (hm_alpha > 0.01) {
        float d_diag1 = abs(dc.x - dc.y) * 0.70710678;
        float d_diag2 = abs(dc.x + dc.y) * 0.70710678;
        float r = length(dc);
        float r_min = 6.0;
        float r_max = hm_headshot ? 17.0 : 13.5;
        bool on_x = (min(d_diag1, d_diag2) < 1.35) && (r >= r_min) && (r <= r_max);
        bool on_x_out = (min(d_diag1, d_diag2) < 2.45) && (r >= r_min - 1.0) && (r <= r_max + 1.0);
        float3 hm_col = hm_headshot ? float3(1.0, 0.14, 0.14) : float3(0.98, 0.99, 1.0);
        if (on_x) {
            color = lerp(color, hm_col, hm_alpha);
        } else if (on_x_out) {
            color = lerp(color, float3(0.02, 0.02, 0.02), hm_alpha * 0.8);
        }
    }

    float radar_size = 166.0;
    float2 radar_min = float2(14.0, 14.0);
    float2 radar_max = radar_min + float2(radar_size, radar_size);
    if (tactical_flags.x > 1.5 && pixel_pos.x >= radar_min.x && pixel_pos.x < radar_max.x && pixel_pos.y >= radar_min.y && pixel_pos.y < radar_max.y) {
        float2 rp = pixel_pos - radar_min;
        float2 rn = rp / radar_size;
        color = lerp(color, float3(0.04, 0.07, 0.09), 0.84);
        bool on_border = rp.x < 1.5 || rp.x >= radar_size - 1.5 || rp.y < 1.5 || rp.y >= radar_size - 1.5;
        if (on_border) color = float3(0.32, 0.56, 0.68);
        if (rp.y < 20.0 && !on_border) {
            color = lerp(color, float3(0.07, 0.13, 0.17), 0.75);
            if (draw_label_4(pixel_pos, radar_min + float2(8.0, 4.0), 77u, 65u, 80u, 32u, 2u)) {
                color = float3(0.84, 0.95, 1.0);
            }
            if (draw_label_4(pixel_pos, radar_min + float2(78.0, 4.0), 84u, 71u, 84u, 58u, 2u)) {
                color = float3(0.60, 0.78, 0.86);
            }
            bool unused_o;
            if (draw_uint_digits(pixel_pos, radar_min + float2(130.0, 4.0), tactical_score_meta.w, 2u, unused_o)) {
                color = float3(1.0, 0.76, 0.24);
            }
        } else if (!on_border) {
            float2 map_min = radar_min + float2(10.0, 26.0);
            float2 map_size = float2(radar_size - 20.0, radar_size - 34.0);
            float2 mp = (pixel_pos - map_min) / map_size;
            if (mp.x >= 0.0 && mp.x <= 1.0 && mp.y >= 0.0 && mp.y <= 1.0) {
                float wx = (mp.x - 0.5) * 14.0;
                float wz = (1.0 - mp.y) * 34.0 - 2.0;
                if (abs(wx) <= 6.5 && wz >= -2.0 && wz <= 32.0) {
                    color = lerp(color, float3(0.11, 0.16, 0.19), 0.65);
                    if (abs(abs(wx) - 6.5) < 0.25 || abs(wz - 32.0) < 0.35 || abs(wz - 3.6) < 0.28) {
                        color = float3(0.36, 0.52, 0.60);
                    }
                    if (abs(wz - 10.0) < 0.14 || abs(wz - 16.0) < 0.14 || abs(wz - 22.7) < 0.14) {
                        color = lerp(color, float3(0.28, 0.40, 0.46), 0.55);
                    }
                    bool in_crate = (abs(wx + 3.8) < 0.65 && abs(wz - 12.6) < 0.65)
                        || (abs(wx - 3.8) < 0.65 && abs(wz - 12.8) < 0.65)
                        || (abs(wx - 0.8) < 0.55 && abs(wz - 13.6) < 0.55)
                        || (abs(wx + 4.2) < 0.75 && abs(wz - 19.2) < 0.75)
                        || (abs(wx - 4.0) < 0.75 && abs(wz - 19.4) < 0.75);
                    if (in_crate) {
                        color = float3(0.46, 0.34, 0.20);
                    }
                }

                float2 p_xz = tactical_player_radar.xy;
                float2 p_fwd = float2(tactical_player_radar.w, tactical_player_radar.z);
                float2 rel_p = float2(wx, wz) - p_xz;
                float dist_p = length(rel_p);
                if (dist_p > 0.3 && dist_p < 7.0) {
                    float cos_cone = dot(rel_p / dist_p, p_fwd);
                    if (cos_cone > 0.82) {
                        color = lerp(color, float3(0.22, 0.86, 0.98), 0.22 * (1.0 - dist_p / 7.0));
                    }
                }
                if (dist_p < 0.58) {
                    color = float3(0.24, 0.96, 1.0);
                }

                [unroll]
                for (int ti = 0; ti < 8; ++ti) {
                    float4 td = tactical_targets[ti];
                    float d_t = length(float2(wx - td.x, wz - td.y));
                    if (d_t < 0.62) {
                        color = (td.w > 0.15)
                            ? float3(1.0, 0.95, 0.25)
                            : (td.z > 0.5 ? float3(1.0, 0.30, 0.18) : float3(0.32, 0.36, 0.40));
                    }
                }
            }
        }
        return color;
    }

    float2 hp_min = float2(18.0, viewport_size.y - 74.0);
    float2 hp_size = float2(252.0, 56.0);
    if (pixel_pos.x >= hp_min.x && pixel_pos.x < hp_min.x + hp_size.x && pixel_pos.y >= hp_min.y && pixel_pos.y < hp_min.y + hp_size.y) {
        float2 lp = pixel_pos - hp_min;
        color = lerp(color, float3(0.05, 0.08, 0.10), 0.82);
        if (lp.x < 1.5 || lp.x >= hp_size.x - 1.5 || lp.y < 1.5 || lp.y >= hp_size.y - 1.5) {
            color = float3(0.26, 0.42, 0.48);
        }
        float2 cross_c = lp - float2(22.0, 22.0);
        if ((abs(cross_c.x) < 3.0 && abs(cross_c.y) < 9.0) || (abs(cross_c.y) < 3.0 && abs(cross_c.x) < 9.0)) {
            color = float3(0.24, 0.94, 0.46);
        }
        bool unused_o;
        if (draw_uint_digits(pixel_pos, hp_min + float2(38.0, 12.0), tactical_ammo_hp.x, 3u, unused_o)) {
            color = float3(0.96, 0.99, 1.0);
        }
        if (draw_label_4(pixel_pos, hp_min + float2(138.0, 16.0), 65u, 82u, 77u, 32u, 2u)) {
            color = float3(0.38, 0.82, 0.98);
        }
        if (draw_uint_digits(pixel_pos, hp_min + float2(180.0, 12.0), tactical_ammo_hp.y, 3u, unused_o)) {
            color = float3(0.74, 0.92, 1.0);
        }
        if (lp.y >= 42.0 && lp.y <= 48.0) {
            if (lp.x >= 12.0 && lp.x <= 120.0) {
                float fill = saturate(tactical_combat.z);
                color = ((lp.x - 12.0) / 108.0 <= fill) ? float3(0.24, 0.90, 0.46) : float3(0.14, 0.18, 0.20);
            } else if (lp.x >= 136.0 && lp.x <= 240.0) {
                float fill = saturate(tactical_combat.w);
                color = ((lp.x - 136.0) / 104.0 <= fill) ? float3(0.28, 0.78, 0.98) : float3(0.14, 0.18, 0.20);
            }
        }
        return color;
    }

    float2 am_size = float2(276.0, 68.0);
    float2 am_min = float2(viewport_size.x - am_size.x - 18.0, viewport_size.y - am_size.y - 18.0);
    if (pixel_pos.x >= am_min.x && pixel_pos.x < am_min.x + am_size.x && pixel_pos.y >= am_min.y && pixel_pos.y < am_min.y + am_size.y) {
        float2 lp = pixel_pos - am_min;
        color = lerp(color, float3(0.05, 0.08, 0.10), 0.82);
        if (lp.x < 1.5 || lp.x >= am_size.x - 1.5 || lp.y < 1.5 || lp.y >= am_size.y - 1.5) {
            color = float3(0.32, 0.46, 0.52);
        }
        bool unused_o;
        float3 ammo_col = (tactical_ammo_hp.z <= 3u) ? float3(1.0, 0.34, 0.24) : float3(0.98, 0.99, 1.0);
        if (draw_uint_digits(pixel_pos, am_min + float2(16.0, 10.0), tactical_ammo_hp.z, 3u, unused_o)) {
            color = ammo_col;
        }
        if (draw_label_4(pixel_pos, am_min + float2(60.0, 14.0), 47u, 32u, 32u, 32u, 2u)) {
            color = float3(0.55, 0.65, 0.72);
        }
        if (draw_uint_digits(pixel_pos, am_min + float2(78.0, 14.0), tactical_ammo_hp.w, 2u, unused_o)) {
            color = float3(0.76, 0.84, 0.90);
        }
        bool is_ads = ads_alpha > 0.5;
        if (draw_label_4(pixel_pos, am_min + float2(142.0, 14.0), 85u, 83u, 80u, 32u, 2u)) {
            color = float3(0.88, 0.76, 0.36);
        }
        if (is_ads) {
            if (draw_label_4(pixel_pos, am_min + float2(194.0, 14.0), 65u, 68u, 83u, 32u, 2u)) {
                color = float3(0.24, 0.96, 0.56);
            }
        } else {
            if (draw_label_4(pixel_pos, am_min + float2(194.0, 14.0), 72u, 73u, 80u, 32u, 2u)) {
                color = float3(0.58, 0.72, 0.82);
            }
        }
        if (draw_label_4(pixel_pos, am_min + float2(16.0, 42.0), 80u, 84u, 83u, 58u, 2u)) {
            color = float3(0.62, 0.74, 0.82);
        }
        if (draw_uint_digits(pixel_pos, am_min + float2(68.0, 42.0), tactical_score_meta.z, 2u, unused_o)) {
            color = float3(1.0, 0.84, 0.28);
        }
        if (draw_label_4(pixel_pos, am_min + float2(168.0, 42.0), 72u, 83u, 58u, 32u, 2u)) {
            color = float3(1.0, 0.36, 0.32);
        }
        if (draw_uint_digits(pixel_pos, am_min + float2(210.0, 42.0), tactical_score_meta.y, 2u, unused_o)) {
            color = float3(1.0, 0.64, 0.56);
        }
        if (reload_prog > 0.01 && lp.y >= 58.0 && lp.y <= 63.0 && lp.x >= 14.0 && lp.x <= am_size.x - 14.0) {
            float rf = (lp.x - 14.0) / (am_size.x - 28.0);
            color = (rf <= reload_prog) ? float3(1.0, 0.82, 0.24) : float3(0.16, 0.20, 0.24);
        }
        return color;
    }

    return color;
}

float3 composite_overlays(float2 pixel_pos, float2 viewport_size, float3 output_color) {
    bool overlay_enabled = viewport_and_flags.w > 0.5;
    bool tactical_enabled = tactical_flags.x > 0.5;
    if (overlay_enabled) {
        if (tactical_enabled) {
            output_color = composite_tactical_hud(pixel_pos, viewport_size, output_color);
        } else {
            float2 center_offset = abs(pixel_pos - viewport_size * 0.5);
            bool horizontal_arm = center_offset.y < 1.25 && center_offset.x > 4.0 && center_offset.x < 14.0;
            bool vertical_arm = center_offset.x < 1.25 && center_offset.y > 4.0 && center_offset.y < 14.0;
            bool center_dot = center_offset.x < 1.25 && center_offset.y < 1.25;
            if (horizontal_arm || vertical_arm || center_dot) {
                output_color = float3(0.95, 0.98, 1.0);
            }
        }
        output_color = composite_hud_layer(pixel_pos, output_color);
    }

    bool flash_enabled = warp_flags_and_aspect.z > 0.5;
    if (flash_enabled) {
        bool in_flash_box = pixel_pos.x >= 0.0 && pixel_pos.x < 72.0
            && abs(pixel_pos.y - viewport_size.y * 0.5) < 48.0;
        if (in_flash_box) {
            bool flash_lit = warp_flags_and_aspect.w > 0.5;
            output_color = flash_lit ? float3(1.0, 1.0, 1.0) : float3(0.02, 0.02, 0.02);
        }
    }
    return output_color;
}

#if defined(PRESENT_OUTPUT_DLSS_INPUTS) && PRESENT_OUTPUT_DLSS_INPUTS
struct PresentOutput {
    float4 backbuffer : SV_Target0;
    float4 hudless : SV_Target1;
    float clip_depth : SV_Target2;
    float2 motion : SV_Target3;
};

PresentOutput pixel_main(VertexOutput input) {
#else
float4 pixel_main(VertexOutput input) : SV_Target {
#endif
    float2 viewport_size = viewport_and_flags.xy;
    float2 display_texel_size = float2(source_camera_right_rcp_width.w, source_camera_up_rcp_height.w);
    float2 uv = input.position.xy * display_texel_size;
    float2 source_size = max(source_subrect_params.xy, float2(1.0, 1.0));
    float2 src_uv_scale = max(source_subrect_params.zw, float2(1e-4, 1e-4));
    float2 prev_uv_scale = max(prev_and_alloc_params.xy, float2(1e-4, 1e-4));
    float2 inv_alloc_size = max(prev_and_alloc_params.zw, float2(1e-6, 1e-6));
    float2 prev_active_size = max(prev_uv_scale / inv_alloc_size, float2(1.0, 1.0));
    float2 texel_size = rcp(source_size);
    float2 prev_texel_size = rcp(prev_active_size);
    int upscale_filter = (int)(homography_col_z.w + 0.5);

#if PRESENT_VARIANT == 0
    float3 linear_color = sample_upscaled_color(scene_color_texture, uv, source_size, src_uv_scale, inv_alloc_size, upscale_filter);
    float3 output_color = pow(saturate(linear_color), 1.0 / 2.2);
    return float4(composite_overlays(input.position.xy, viewport_size, output_color), 1.0);
#elif PRESENT_VARIANT == 1
    bool target_is_viewmodel = is_viewmodel_tex(scene_color_texture, uv, src_uv_scale, inv_alloc_size);
    bool collect_stats = synthesis_params.w > 0.5;
    if (target_is_viewmodel) {
        float3 vm_color = pow(saturate(sample_upscaled_color(scene_color_texture, uv, source_size, src_uv_scale, inv_alloc_size, upscale_filter)), 1.0 / 2.2);
        return float4(composite_overlays(input.position.xy, viewport_size, vm_color), 1.0);
    }
    ReprojectionSample rot = evaluate_rotation_homography(uv);
    float2 sample_uv = saturate(rot.source_uv);
    bool pixel_valid = rot.valid && !is_viewmodel_guarded(scene_color_texture, sample_uv, texel_size, src_uv_scale, inv_alloc_size);
    bool recovered_from_history = false;
    bool has_previous_history = synthesis_params.z > 0.5;
    if (!pixel_valid && has_previous_history) {
        ReprojectionSample hist = evaluate_reprojection_camera(
            prev_depth_texture,
            prev_color_texture,
            previous_camera_position_tan_y.xyz,
            previous_camera_right_rcp_tan_x.xyz,
            previous_camera_up_rcp_tan_y.xyz,
            previous_camera_forward_tan_x.xyz,
            previous_camera_forward_tan_x.w,
            previous_camera_position_tan_y.w,
            uv,
            viewport_size,
            prev_texel_size,
            prev_uv_scale,
            inv_alloc_size,
            2,
            false);
        if (hist.valid) {
            sample_uv = saturate(hist.source_uv);
            pixel_valid = true;
            recovered_from_history = true;
        }
    }
    if (collect_stats) {
        uint invalid_wave = WaveActiveCountBits(!pixel_valid);
        if (WaveIsFirstLane() && invalid_wave > 0u) {
            uint previous_invalid;
            reprojection_stats.InterlockedAdd(0, invalid_wave, previous_invalid);
        }
    }
    float unused_bg_depth = 1000.0;
    float2 unused_bg_uv = sample_uv;
    float3 output_color = pixel_valid
        ? pow(saturate(recovered_from_history
            ? sample_upscaled_color(prev_color_texture, sample_uv, prev_active_size, prev_uv_scale, inv_alloc_size, upscale_filter)
            : sample_upscaled_color(scene_color_texture, sample_uv, source_size, src_uv_scale, inv_alloc_size, upscale_filter)), 1.0 / 2.2)
        : sample_conservative_hole_fill(sample_uv, texel_size, src_uv_scale, inv_alloc_size, unused_bg_depth, unused_bg_uv);
    return float4(composite_overlays(input.position.xy, viewport_size, output_color), 1.0);
#elif PRESENT_VARIANT == 2 || PRESENT_VARIANT == 3
    bool target_is_viewmodel = is_viewmodel_tex(scene_color_texture, uv, src_uv_scale, inv_alloc_size);
    bool collect_stats = synthesis_params.w > 0.5;
    if (target_is_viewmodel) {
        float3 vm_color = pow(saturate(sample_upscaled_color(scene_color_texture, uv, source_size, src_uv_scale, inv_alloc_size, upscale_filter)), 1.0 / 2.2);
        return float4(composite_overlays(input.position.xy, viewport_size, vm_color), 1.0);
    }
    ReprojectionSample reproj = evaluate_reprojection_camera(
        scene_depth_texture,
        scene_color_texture,
        source_camera_position_tan_y.xyz,
        source_camera_right_rcp_width.xyz,
        source_camera_up_rcp_height.xyz,
        source_camera_forward_tan_x.xyz,
        source_camera_forward_tan_x.w,
        source_camera_position_tan_y.w,
        uv,
        viewport_size,
        texel_size,
        src_uv_scale,
        inv_alloc_size,
        PRESENT_VARIANT == 2 ? 2 : 4,
        PRESENT_VARIANT == 3);
    float2 sample_uv = saturate(reproj.source_uv);
    bool pixel_valid = reproj.valid;
    bool recovered_from_history = false;
    bool has_previous_history = synthesis_params.z > 0.5;
    if (!pixel_valid && has_previous_history) {
        ReprojectionSample hist = evaluate_reprojection_camera(
            prev_depth_texture,
            prev_color_texture,
            previous_camera_position_tan_y.xyz,
            previous_camera_right_rcp_tan_x.xyz,
            previous_camera_up_rcp_tan_y.xyz,
            previous_camera_forward_tan_x.xyz,
            previous_camera_forward_tan_x.w,
            previous_camera_position_tan_y.w,
            uv,
            viewport_size,
            prev_texel_size,
            prev_uv_scale,
            inv_alloc_size,
            PRESENT_VARIANT == 2 ? 2 : 4,
            PRESENT_VARIANT == 3);
        if (hist.valid) {
            sample_uv = saturate(hist.source_uv);
            pixel_valid = true;
            recovered_from_history = true;
        }
    }
    if (collect_stats) {
        uint invalid_wave = WaveActiveCountBits(!pixel_valid);
        if (WaveIsFirstLane() && invalid_wave > 0u) {
            uint previous_invalid;
            reprojection_stats.InterlockedAdd(0, invalid_wave, previous_invalid);
        }
    }
    float unused_bg_depth = 1000.0;
    float2 unused_bg_uv = sample_uv;
    float3 output_color = pixel_valid
        ? pow(saturate(recovered_from_history
            ? sample_upscaled_color(prev_color_texture, sample_uv, prev_active_size, prev_uv_scale, inv_alloc_size, upscale_filter)
            : sample_upscaled_color(scene_color_texture, sample_uv, source_size, src_uv_scale, inv_alloc_size, upscale_filter)), 1.0 / 2.2)
        : sample_conservative_hole_fill(sample_uv, texel_size, src_uv_scale, inv_alloc_size, unused_bg_depth, unused_bg_uv);
    return float4(composite_overlays(input.position.xy, viewport_size, output_color), 1.0);
#else
    int diagnostic_view = (int)(viewport_and_flags.z + 0.5);
    int synthesis_mode = (int)(synthesis_params.x + 0.5);
    float extrapolation_alpha = synthesis_params.y;
    bool has_previous_history = synthesis_params.z > 0.5;
    bool collect_stats = synthesis_params.w > 0.5;
    bool synthesis_active = synthesis_mode > 0;
    bool target_is_viewmodel = is_viewmodel_tex(scene_color_texture, uv, src_uv_scale, inv_alloc_size);

    float2 sample_uv = uv;
    bool pixel_valid = true;
    bool recovered_from_history = false;

    if (synthesis_mode == 1 && !target_is_viewmodel) {
        ReprojectionSample reproj = evaluate_reprojection_camera(
            scene_depth_texture,
            scene_color_texture,
            source_camera_position_tan_y.xyz,
            source_camera_right_rcp_width.xyz,
            source_camera_up_rcp_height.xyz,
            source_camera_forward_tan_x.xyz,
            source_camera_forward_tan_x.w,
            source_camera_position_tan_y.w,
            uv,
            viewport_size,
            texel_size,
            src_uv_scale,
            inv_alloc_size,
            4,
            true);
        sample_uv = saturate(reproj.source_uv);
        pixel_valid = reproj.valid;
        if (!pixel_valid && has_previous_history) {
            ReprojectionSample hist = evaluate_reprojection_camera(
                prev_depth_texture,
                prev_color_texture,
                previous_camera_position_tan_y.xyz,
                previous_camera_right_rcp_tan_x.xyz,
                previous_camera_up_rcp_tan_y.xyz,
                previous_camera_forward_tan_x.xyz,
                previous_camera_forward_tan_x.w,
                previous_camera_position_tan_y.w,
                uv,
                viewport_size,
                prev_texel_size,
                prev_uv_scale,
                inv_alloc_size,
                4,
                true);
            if (hist.valid) {
                sample_uv = saturate(hist.source_uv);
                pixel_valid = true;
                recovered_from_history = true;
            }
        }
    } else if (synthesis_mode == 2 && !target_is_viewmodel) {
        ReprojectionSample gen = evaluate_frame_gen(uv, viewport_size, texel_size, src_uv_scale, inv_alloc_size, extrapolation_alpha);
        sample_uv = saturate(gen.source_uv);
        pixel_valid = gen.valid;
        if (!pixel_valid && has_previous_history) {
            ReprojectionSample hist = evaluate_reprojection_camera(
                prev_depth_texture,
                prev_color_texture,
                previous_camera_position_tan_y.xyz,
                previous_camera_right_rcp_tan_x.xyz,
                previous_camera_up_rcp_tan_y.xyz,
                previous_camera_forward_tan_x.xyz,
                previous_camera_forward_tan_x.w,
                previous_camera_position_tan_y.w,
                uv,
                viewport_size,
                prev_texel_size,
                prev_uv_scale,
                inv_alloc_size,
                4,
                true);
            if (hist.valid) {
                sample_uv = saturate(hist.source_uv);
                pixel_valid = true;
                recovered_from_history = true;
            }
        }
    } else if (synthesis_mode == 3 && !target_is_viewmodel) {
        ReprojectionSample reproj = evaluate_reprojection_camera(
            scene_depth_texture,
            scene_color_texture,
            source_camera_position_tan_y.xyz,
            source_camera_right_rcp_width.xyz,
            source_camera_up_rcp_height.xyz,
            source_camera_forward_tan_x.xyz,
            source_camera_forward_tan_x.w,
            source_camera_position_tan_y.w,
            uv,
            viewport_size,
            texel_size,
            src_uv_scale,
            inv_alloc_size,
            4,
            true);
        sample_uv = saturate(reproj.source_uv);
        pixel_valid = reproj.valid;

        if (pixel_valid && has_previous_history && extrapolation_alpha > 0.01) {
            float2 obj_mv = sample_independent_object_motion(sample_uv, src_uv_scale, inv_alloc_size);
            float obj_speed = length(obj_mv);
            if (obj_speed < 0.0008) {
                float2 probe_radius = texel_size * 6.0;
                float2 probes[4] = {
                    float2(-probe_radius.x, 0.0),
                    float2(probe_radius.x, 0.0),
                    float2(0.0, -probe_radius.y),
                    float2(0.0, probe_radius.y)
                };
                [unroll]
                for (int p = 0; p < 4; ++p) {
                    float2 probe_uv = saturate(sample_uv + probes[p]);
                    if (is_viewmodel_tex(scene_color_texture, probe_uv, src_uv_scale, inv_alloc_size)) {
                        continue;
                    }
                    float2 probe_mv = sample_independent_object_motion(probe_uv, src_uv_scale, inv_alloc_size);
                    float probe_speed = length(probe_mv);
                    if (probe_speed > obj_speed) {
                        obj_speed = probe_speed;
                        obj_mv = probe_mv;
                    }
                }
            }
            if (obj_speed > 0.0008) {
                float2 extrapolated_uv = sample_uv + obj_mv * extrapolation_alpha;
                if (extrapolated_uv.x >= 0.0 && extrapolated_uv.x <= 1.0 && extrapolated_uv.y >= 0.0 && extrapolated_uv.y <= 1.0
                    && !is_viewmodel_guarded(scene_color_texture, extrapolated_uv, texel_size, src_uv_scale, inv_alloc_size)) {
                    float2 verify_mv = sample_independent_object_motion(extrapolated_uv, src_uv_scale, inv_alloc_size);
                    if (length(verify_mv) > 0.0006 && length((verify_mv - obj_mv) * viewport_size) < 12.0) {
                        sample_uv = extrapolated_uv;
                    }
                }
            }
        }

        if (!pixel_valid && has_previous_history) {
            ReprojectionSample hist = evaluate_reprojection_camera(
                prev_depth_texture,
                prev_color_texture,
                previous_camera_position_tan_y.xyz,
                previous_camera_right_rcp_tan_x.xyz,
                previous_camera_up_rcp_tan_y.xyz,
                previous_camera_forward_tan_x.xyz,
                previous_camera_forward_tan_x.w,
                previous_camera_position_tan_y.w,
                uv,
                viewport_size,
                prev_texel_size,
                prev_uv_scale,
                inv_alloc_size,
                4,
                true);
            if (hist.valid) {
                sample_uv = saturate(hist.source_uv);
                pixel_valid = true;
                recovered_from_history = true;
            }
        }
    }

    if (synthesis_active && collect_stats) {
        uint invalid_wave = WaveActiveCountBits(!pixel_valid);
        uint recovered_wave = WaveActiveCountBits(recovered_from_history);
        if (WaveIsFirstLane()) {
            uint previous_val;
            if (invalid_wave > 0u) reprojection_stats.InterlockedAdd(0, invalid_wave, previous_val);
            if (recovered_wave > 0u) reprojection_stats.InterlockedAdd(4, recovered_wave, previous_val);
        }
    }

    float filled_depth = 1000.0;
    float2 filled_uv = sample_uv;
    bool used_hole_fill = false;
    float3 output_color = float3(0.0, 0.0, 0.0);
    if (diagnostic_view == 1) {
        float linear_depth = recovered_from_history
            ? prev_depth_texture.SampleLevel(point_clamp_sampler, map_subrect_uv(sample_uv, prev_uv_scale, inv_alloc_size), 0)
            : scene_depth_texture.SampleLevel(point_clamp_sampler, map_subrect_uv(sample_uv, src_uv_scale, inv_alloc_size), 0);
        float normalized_depth = saturate(1.0 - linear_depth / 32.0);
        output_color = float3(normalized_depth, normalized_depth * 0.9, normalized_depth * 0.75);
    } else if (diagnostic_view == 2) {
        float2 motion = recovered_from_history
            ? prev_motion_texture.SampleLevel(point_clamp_sampler, map_subrect_uv(sample_uv, prev_uv_scale, inv_alloc_size), 0)
            : scene_motion_texture.SampleLevel(point_clamp_sampler, map_subrect_uv(sample_uv, src_uv_scale, inv_alloc_size), 0);
        float2 scaled = clamp(motion * 24.0, -1.0, 1.0);
        output_color = float3(saturate(scaled.x * 0.5 + 0.5), saturate(scaled.y * 0.5 + 0.5), saturate(length(scaled)));
    } else {
        float3 linear_color = recovered_from_history
            ? sample_upscaled_color(prev_color_texture, sample_uv, prev_active_size, prev_uv_scale, inv_alloc_size, upscale_filter)
            : sample_upscaled_color(scene_color_texture, sample_uv, source_size, src_uv_scale, inv_alloc_size, upscale_filter);
        output_color = pow(saturate(linear_color), 1.0 / 2.2);
        if (synthesis_active && recovered_from_history && diagnostic_view == 3) {
            output_color = float3(0.08, 0.95, 0.85);
        } else if (synthesis_active && !pixel_valid) {
            if (diagnostic_view == 3) {
                output_color = float3(1.0, 0.12, 0.78);
            } else {
                output_color = sample_conservative_hole_fill(sample_uv, texel_size, src_uv_scale, inv_alloc_size, filled_depth, filled_uv);
                used_hole_fill = true;
            }
        }
        if (diagnostic_view == 4) {
            int active_vrs = (int)(vrs_params.x + 0.5);
            float tile_size = max(vrs_params.y, 8.0);
            bool extra_rates = vrs_params.z > 0.5;
            float3 vrs_tint = float3(0.18, 0.86, 0.42);
            if (active_vrs == 1) {
                vrs_tint = float3(0.95, 0.64, 0.14);
            } else if (active_vrs == 2) {
                float2 tile_index = floor(input.position.xy / tile_size);
                float2 tile_count = max(ceil(viewport_size / tile_size), float2(1.0, 1.0));
                float2 tile_uv = (tile_index + 0.5) / tile_count;
                float2 centered = (tile_uv - 0.5) * 2.0;
                float radial = length(centered);
                if (radial < 0.36) {
                    vrs_tint = float3(0.18, 0.86, 0.42);
                } else if (radial < 0.65) {
                    vrs_tint = float3(0.20, 0.68, 0.96);
                } else if (radial < 0.90 || !extra_rates) {
                    vrs_tint = float3(0.95, 0.64, 0.14);
                } else {
                    vrs_tint = float3(0.96, 0.24, 0.24);
                }
            }
            output_color = lerp(output_color, vrs_tint, 0.48);
            float2 in_tile = fmod(input.position.xy, tile_size);
            if (in_tile.x < 1.0 || in_tile.y < 1.0) {
                output_color = lerp(output_color, float3(0.06, 0.08, 0.10), 0.55);
            }
        }
        if (diagnostic_view == 5) {
            float2 src_pixel = uv * source_size;
            uint2 tile_coord = (uint2)floor(src_pixel / 16.0);
            uint tile_state = scene_tile_map_texture.Load(int3(tile_coord, 0)) & 3u;
            float3 tile_tint = float3(0.16, 0.84, 0.42);
            if (tile_state == 1u) {
                tile_tint = float3(0.98, 0.34, 0.18);
            } else if (tile_state == 2u) {
                tile_tint = float3(0.18, 0.78, 0.96);
            } else if (tile_state == 3u) {
                tile_tint = float3(0.96, 0.18, 0.82);
            }
            output_color = lerp(output_color, tile_tint, 0.46);
            float2 in_tile = fmod(src_pixel, 16.0);
            if (in_tile.x < 1.0 || in_tile.y < 1.0) {
                output_color = lerp(output_color, float3(0.05, 0.07, 0.09), 0.55);
            }
        }
        if (diagnostic_view == 6) {
            float2 src_pixel = uv * source_size;
            uint2 tile_coord = (uint2)floor(src_pixel / 16.0);
            uint packed_word = scene_tile_map_texture.Load(int3(tile_coord, 0));
            float prio = float((packed_word >> 2u) & 63u) * (1.0 / 63.0);
            float3 heat = prio < 0.33
                ? lerp(float3(0.06, 0.14, 0.38), float3(0.10, 0.78, 0.88), prio * 3.0)
                : (prio < 0.66
                    ? lerp(float3(0.10, 0.78, 0.88), float3(0.98, 0.88, 0.16), (prio - 0.33) * 3.0)
                    : lerp(float3(0.98, 0.88, 0.16), float3(1.0, 0.14, 0.12), (prio - 0.66) * 3.0));
            output_color = lerp(output_color, heat, 0.56);
            float2 in_tile = fmod(src_pixel, 16.0);
            if (in_tile.x < 1.0 || in_tile.y < 1.0) {
                output_color = lerp(output_color, float3(0.05, 0.07, 0.09), 0.50);
            }
        }
    }

#if defined(PRESENT_OUTPUT_DLSS_INPUTS) && PRESENT_OUTPUT_DLSS_INPUTS
    float near_z = dlss_motion_params.y;
    float far_z = dlss_motion_params.z;
    float object_motion_scale = dlss_motion_params.x;
    bool reset_dlss_motion = prev_presented_camera_forward_reset.w > 0.5;

    float2 geom_uv = used_hole_fill ? filled_uv : sample_uv;
    float sample_linear_depth = used_hole_fill
        ? filled_depth
        : (recovered_from_history
            ? prev_depth_texture.SampleLevel(point_clamp_sampler, map_subrect_uv(geom_uv, prev_uv_scale, inv_alloc_size), 0)
            : scene_depth_texture.SampleLevel(point_clamp_sampler, map_subrect_uv(geom_uv, src_uv_scale, inv_alloc_size), 0));

    float out_clip_depth = 1.0;
    float2 out_motion = float2(0.0, 0.0);

    if (sample_linear_depth < 900.0) {
        float3 world_pos = recovered_from_history
            ? reconstruct_camera_world_position(
                geom_uv,
                sample_linear_depth,
                previous_camera_position_tan_y.xyz,
                previous_camera_right_rcp_tan_x.xyz,
                previous_camera_up_rcp_tan_y.xyz,
                previous_camera_forward_tan_x.xyz,
                previous_camera_forward_tan_x.w,
                previous_camera_position_tan_y.w)
            : reconstruct_camera_world_position(
                geom_uv,
                sample_linear_depth,
                source_camera_position_tan_y.xyz,
                source_camera_right_rcp_width.xyz,
                source_camera_up_rcp_height.xyz,
                source_camera_forward_tan_x.xyz,
                source_camera_forward_tan_x.w,
                source_camera_position_tan_y.w);

        float target_view_z = max(dot(world_pos - target_camera_position_tan_y.xyz, target_camera_forward_tan_x.xyz), near_z);
        out_clip_depth = saturate((far_z * (target_view_z - near_z)) / ((far_z - near_z) * target_view_z));

        if (!reset_dlss_motion) {
            if (target_is_viewmodel) {
                out_motion = scene_motion_texture.SampleLevel(point_clamp_sampler, map_subrect_uv(geom_uv, src_uv_scale, inv_alloc_size), 0) * object_motion_scale;
            } else {
                float2 prev_pres_uv;
                float prev_pres_z;
                if (project_world_to_camera_uv(
                        world_pos,
                        prev_presented_camera_position_tan_y.xyz,
                        prev_presented_camera_right_rcp_tan_x.xyz,
                        prev_presented_camera_up_rcp_tan_y.xyz,
                        prev_presented_camera_forward_reset.xyz,
                        prev_presented_camera_right_rcp_tan_x.w,
                        prev_presented_camera_up_rcp_tan_y.w,
                        prev_pres_uv,
                        prev_pres_z)) {
                    out_motion = prev_pres_uv - uv;
                }
                if (!recovered_from_history && !used_hole_fill && pixel_valid) {
                    float2 obj_mv = sample_independent_object_motion(geom_uv, src_uv_scale, inv_alloc_size);
                    out_motion += obj_mv * object_motion_scale;
                }
            }
        }
    } else if (!reset_dlss_motion) {
        float2 target_ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
        float3 target_ray = normalize(
            target_camera_right_rcp_tan_x.xyz * (target_ndc.x * target_camera_forward_tan_x.w)
            + target_camera_up_rcp_tan_y.xyz * (target_ndc.y * target_camera_position_tan_y.w)
            + target_camera_forward_tan_x.xyz);
        float3 far_world = target_camera_position_tan_y.xyz + target_ray * far_z;
        float2 prev_sky_uv;
        float prev_sky_z;
        if (project_world_to_camera_uv(
                far_world,
                prev_presented_camera_position_tan_y.xyz,
                prev_presented_camera_right_rcp_tan_x.xyz,
                prev_presented_camera_up_rcp_tan_y.xyz,
                prev_presented_camera_forward_reset.xyz,
                prev_presented_camera_right_rcp_tan_x.w,
                prev_presented_camera_up_rcp_tan_y.w,
                prev_sky_uv,
                prev_sky_z)) {
            out_motion = prev_sky_uv - uv;
        }
    }

    PresentOutput dlss_out;
    dlss_out.backbuffer = float4(composite_overlays(input.position.xy, viewport_size, output_color), 1.0);
    dlss_out.hudless = float4(output_color, 1.0);
    dlss_out.clip_depth = out_clip_depth;
    dlss_out.motion = out_motion;
    return dlss_out;
#else
    return float4(composite_overlays(input.position.xy, viewport_size, output_color), 1.0);
#endif
#endif
}
