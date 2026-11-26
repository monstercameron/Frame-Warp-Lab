cbuffer PresentConstants : register(b0) {
    float4 viewport_and_flags;
    float4 warp_flags_and_aspect;
    float4 source_camera_position_tan_y;
    float4 source_camera_right_unused;
    float4 source_camera_up_unused;
    float4 source_camera_forward_unused;
    float4 target_camera_position_tan_y;
    float4 target_camera_right_unused;
    float4 target_camera_up_unused;
    float4 target_camera_forward_unused;
};

Texture2D<float4> scene_color_texture : register(t0);
Texture2D<float> scene_depth_texture : register(t1);
Texture2D<float2> scene_motion_texture : register(t2);
RWByteAddressBuffer reprojection_stats : register(u1);
SamplerState linear_clamp_sampler : register(s0);
SamplerState point_clamp_sampler : register(s1);

struct VertexOutput {
    float4 position : SV_Position;
};

struct ReprojectionSample {
    float2 source_uv;
    bool valid;
};

VertexOutput vertex_main(uint vertex_id : SV_VertexID) {
    VertexOutput output;
    float2 corner = float2((vertex_id << 1) & 2, vertex_id & 2);
    output.position = float4(corner * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

bool project_world_to_camera_uv(
    float3 world_position,
    float3 camera_position,
    float3 camera_right,
    float3 camera_up,
    float3 camera_forward,
    float tan_half_y,
    float aspect_ratio,
    out float2 out_uv,
    out float out_view_z) {
    float3 relative = world_position - camera_position;
    float view_x = dot(relative, camera_right);
    float view_y = dot(relative, camera_up);
    float view_z = dot(relative, camera_forward);
    out_view_z = view_z;
    if (view_z <= 0.01) {
        out_uv = float2(-1.0, -1.0);
        return false;
    }
    float tan_half_x = tan_half_y * aspect_ratio;
    float ndc_x = view_x / (view_z * tan_half_x);
    float ndc_y = view_y / (view_z * tan_half_y);
    out_uv = float2(ndc_x * 0.5 + 0.5, 0.5 - ndc_y * 0.5);
    return out_uv.x >= 0.0 && out_uv.x <= 1.0 && out_uv.y >= 0.0 && out_uv.y <= 1.0;
}

float3 reconstruct_source_world_position(float2 source_uv, float linear_view_depth, float aspect_ratio) {
    float2 ndc = float2(source_uv.x * 2.0 - 1.0, 1.0 - source_uv.y * 2.0);
    float tan_half_y = source_camera_position_tan_y.w;
    float tan_half_x = tan_half_y * aspect_ratio;
    return source_camera_position_tan_y.xyz
        + source_camera_right_unused.xyz * (ndc.x * tan_half_x * linear_view_depth)
        + source_camera_up_unused.xyz * (ndc.y * tan_half_y * linear_view_depth)
        + source_camera_forward_unused.xyz * linear_view_depth;
}

ReprojectionSample evaluate_reprojection(float2 target_uv, float2 viewport_size, float aspect_ratio) {
    ReprojectionSample result;
    result.source_uv = target_uv;
    result.valid = false;

    float2 target_ndc = float2(target_uv.x * 2.0 - 1.0, 1.0 - target_uv.y * 2.0);
    float target_tan_y = target_camera_position_tan_y.w;
    float target_tan_x = target_tan_y * aspect_ratio;
    float3 target_origin = target_camera_position_tan_y.xyz;
    float3 target_ray = normalize(
        target_camera_right_unused.xyz * (target_ndc.x * target_tan_x)
        + target_camera_up_unused.xyz * (target_ndc.y * target_tan_y)
        + target_camera_forward_unused.xyz);

    float source_ray_z = dot(target_ray, source_camera_forward_unused.xyz);
    if (source_ray_z <= 0.05) {
        return result;
    }

    float source_tan_y = source_camera_position_tan_y.w;
    float source_tan_x = source_tan_y * aspect_ratio;
    float2 initial_ndc = float2(
        dot(target_ray, source_camera_right_unused.xyz) / (source_ray_z * source_tan_x),
        dot(target_ray, source_camera_up_unused.xyz) / (source_ray_z * source_tan_y));
    float2 candidate_uv = float2(initial_ndc.x * 0.5 + 0.5, 0.5 - initial_ndc.y * 0.5);
    if (candidate_uv.x < 0.0 || candidate_uv.x > 1.0 || candidate_uv.y < 0.0 || candidate_uv.y > 1.0) {
        return result;
    }

    float sampled_depth = scene_depth_texture.SampleLevel(point_clamp_sampler, candidate_uv, 0);
    float ray_distance = sampled_depth / max(source_ray_z, 0.1);
    float2 projected_uv = candidate_uv;
    float projected_source_z = sampled_depth;
    bool in_bounds = true;

    [unroll]
    for (int iteration = 0; iteration < 4; ++iteration) {
        float3 candidate_world = target_origin + target_ray * ray_distance;
        in_bounds = project_world_to_camera_uv(
            candidate_world,
            source_camera_position_tan_y.xyz,
            source_camera_right_unused.xyz,
            source_camera_up_unused.xyz,
            source_camera_forward_unused.xyz,
            source_tan_y,
            aspect_ratio,
            projected_uv,
            projected_source_z);
        if (!in_bounds) {
            return result;
        }
        sampled_depth = scene_depth_texture.SampleLevel(point_clamp_sampler, projected_uv, 0);
        float3 surface_world = reconstruct_source_world_position(projected_uv, sampled_depth, aspect_ratio);
        ray_distance = max(dot(surface_world - target_origin, target_ray), 0.05);
    }

    float3 matched_world = reconstruct_source_world_position(projected_uv, sampled_depth, aspect_ratio);
    float2 roundtrip_uv;
    float roundtrip_target_z;
    if (!project_world_to_camera_uv(
            matched_world,
            target_origin,
            target_camera_right_unused.xyz,
            target_camera_up_unused.xyz,
            target_camera_forward_unused.xyz,
            target_tan_y,
            aspect_ratio,
            roundtrip_uv,
            roundtrip_target_z)) {
        return result;
    }

    float pixel_error = length((roundtrip_uv - target_uv) * viewport_size);
    float relative_depth_error = abs(projected_source_z - sampled_depth) / max(sampled_depth, 0.25);

    float2 texel = 1.0 / viewport_size;
    float depth_left = scene_depth_texture.SampleLevel(point_clamp_sampler, projected_uv - float2(texel.x, 0.0), 0);
    float depth_right = scene_depth_texture.SampleLevel(point_clamp_sampler, projected_uv + float2(texel.x, 0.0), 0);
    float depth_up = scene_depth_texture.SampleLevel(point_clamp_sampler, projected_uv - float2(0.0, texel.y), 0);
    float depth_down = scene_depth_texture.SampleLevel(point_clamp_sampler, projected_uv + float2(0.0, texel.y), 0);
    float max_neighbor_jump = max(
        max(abs(depth_left - sampled_depth), abs(depth_right - sampled_depth)),
        max(abs(depth_up - sampled_depth), abs(depth_down - sampled_depth))) / max(sampled_depth, 0.25);

    bool consistent = (pixel_error <= 1.75) && (relative_depth_error <= 0.045) && !(max_neighbor_jump > 0.18 && pixel_error > 0.65);
    result.source_uv = projected_uv;
    result.valid = consistent;
    return result;
}

float4 pixel_main(VertexOutput input) : SV_Target {
    float2 viewport_size = viewport_and_flags.xy;
    int diagnostic_view = (int)(viewport_and_flags.z + 0.5);
    bool overlay_enabled = viewport_and_flags.w > 0.5;
    bool warp_active = warp_flags_and_aspect.x > 0.5;
    float aspect_ratio = warp_flags_and_aspect.y;
    float2 uv = input.position.xy / viewport_size;

    float2 sample_uv = uv;
    bool pixel_valid = true;
    if (warp_active) {
        ReprojectionSample reprojection = evaluate_reprojection(uv, viewport_size, aspect_ratio);
        sample_uv = saturate(reprojection.source_uv);
        pixel_valid = reprojection.valid;
        if (!pixel_valid) {
            uint previous_count;
            reprojection_stats.InterlockedAdd(0, 1, previous_count);
        }
    }

    float3 output_color = float3(0.0, 0.0, 0.0);
    if (diagnostic_view == 1) {
        float linear_depth = scene_depth_texture.SampleLevel(point_clamp_sampler, sample_uv, 0);
        float normalized_depth = saturate(1.0 - linear_depth / 32.0);
        output_color = float3(normalized_depth, normalized_depth * 0.9, normalized_depth * 0.75);
    } else if (diagnostic_view == 2) {
        float2 motion = scene_motion_texture.SampleLevel(point_clamp_sampler, sample_uv, 0);
        float2 scaled = clamp(motion * 24.0, -1.0, 1.0);
        output_color = float3(saturate(scaled.x * 0.5 + 0.5), saturate(scaled.y * 0.5 + 0.5), saturate(length(scaled)));
    } else {
        float3 linear_color = scene_color_texture.SampleLevel(linear_clamp_sampler, sample_uv, 0).rgb;
        output_color = pow(saturate(linear_color), 1.0 / 2.2);
        if (warp_active && !pixel_valid) {
            if (diagnostic_view == 3) {
                output_color = float3(1.0, 0.12, 0.78);
            } else {
                float3 fallback_color = pow(saturate(scene_color_texture.SampleLevel(linear_clamp_sampler, uv, 0).rgb), 1.0 / 2.2);
                output_color = lerp(output_color, fallback_color, 0.65);
            }
        }
    }

    if (overlay_enabled) {
        float2 center_offset = abs(input.position.xy - viewport_size * 0.5);
        bool horizontal_arm = center_offset.y < 1.25 && center_offset.x > 4.0 && center_offset.x < 14.0;
        bool vertical_arm = center_offset.x < 1.25 && center_offset.y > 4.0 && center_offset.y < 14.0;
        bool center_dot = center_offset.x < 1.25 && center_offset.y < 1.25;
        if (horizontal_arm || vertical_arm || center_dot) {
            output_color = float3(0.95, 0.98, 1.0);
        }
    }

    return float4(output_color, 1.0);
}
