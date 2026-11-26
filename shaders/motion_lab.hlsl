cbuffer SceneConstants : register(b0) {
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
};

SamplerState linear_clamp_sampler : register(s0);

#include "pbr_materials.hlsl"

struct VertexOutput {
    float4 position : SV_Position;
};

struct SceneOutput {
    float4 color : SV_Target0;
    float depth : SV_Target1;
    float2 motion : SV_Target2;
};

VertexOutput vertex_main(uint vertex_id : SV_VertexID) {
    VertexOutput output;
    float2 corner = float2((vertex_id << 1) & 2, vertex_id & 2);
    output.position = float4(corner * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
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

float2 project_to_previous_uv(float3 previous_world_position) {
    float3 relative = previous_world_position - previous_camera_position_rcp_tan_y.xyz;
    float view_x = dot(relative, previous_camera_right_rcp_tan_x.xyz);
    float view_y = dot(relative, previous_camera_up_cut.xyz);
    float inv_z = rcp(max(dot(relative, previous_camera_forward_unused.xyz), 0.001));
    float ndc_x = view_x * inv_z * previous_camera_right_rcp_tan_x.w;
    float ndc_y = view_y * inv_z * previous_camera_position_rcp_tan_y.w;
    return float2(ndc_x * 0.5 + 0.5, 0.5 - ndc_y * 0.5);
}

SceneOutput pixel_main(VertexOutput input) {
    float2 current_uv = input.position.xy * float2(camera_up_rcp_width.w, camera_forward_rcp_height.w);
    float2 ndc = float2(current_uv.x * 2.0 - 1.0, 1.0 - current_uv.y * 2.0);
    float3 camera_position = camera_position_tan_y.xyz;
    float3 camera_right = camera_right_tan_x.xyz;
    float3 camera_up = camera_up_rcp_width.xyz;
    float3 camera_forward = camera_forward_rcp_height.xyz;
    float tan_x = camera_right_tan_x.w;
    float tan_y = camera_position_tan_y.w;
    float3 direction = normalize(
        camera_right * (ndc.x * tan_x)
        + camera_up * (ndc.y * tan_y)
        + camera_forward);
    float3 ray_dx = normalize(
        camera_right * ((ndc.x + 2.0 * camera_up_rcp_width.w) * tan_x)
        + camera_up * (ndc.y * tan_y)
        + camera_forward);
    float3 ray_dy = normalize(
        camera_right * (ndc.x * tan_x)
        + camera_up * ((ndc.y - 2.0 * camera_forward_rcp_height.w) * tan_y)
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

    float scene_time = previous_camera_forward_unused.w;
    uint packed_flags = (uint)(previous_ball_center_unused.w + 0.5);
    uint opaque_flags = packed_flags & ~128u;
    float3 color = ((packed_flags & 256u) != 0u)
        ? pbr_aces_filmic(pbr_env_cubemap.SampleLevel(linear_clamp_sampler, direction, 0.0).rgb * 0.45)
        : float3(0.035, 0.055, 0.07);
    float linear_view_depth = 1000.0;
    float2 motion_vector = float2(0.0, 0.0);
    if (material > 0.0) {
        color = evaluate_shader_color_unified(
            world_position,
            camera_position,
            direction,
            ray_dx,
            ray_dy,
            distance_travelled,
            material,
            ball_center,
            scene_time,
            opaque_flags,
            linear_clamp_sampler);
        linear_view_depth = max(dot(world_position - camera_position, camera_forward), 0.01);
        if (previous_camera_up_cut.w < 0.5) {
            float3 previous_world_position = world_position;
            if (material > 4.5 && material < 5.5) {
                previous_world_position = world_position - (ball_center - previous_ball_center_unused.xyz);
            }
            float2 previous_uv = project_to_previous_uv(previous_world_position);
            motion_vector = previous_uv - current_uv;
        }
    }

    int stress_level = (int)(ball_center_stress.w + 0.5);
    if (stress_level > 0) {
        color += synthetic_stress_modulation(world_position, stress_level);
    }

    SceneOutput output;
    output.color = float4(max(color, 0.0), 1.0);
    output.depth = linear_view_depth;
    output.motion = motion_vector;
    return output;
}

Texture2D<float> corridor_opaque_depth_tex : register(t1);

float4 corridor_transparent_ps_main(VertexOutput input) : SV_Target0 {
    float2 cur_uv = input.position.xy * float2(camera_up_rcp_width.w, camera_forward_rcp_height.w);
    float2 ndc = float2(cur_uv.x * 2.0 - 1.0, 1.0 - cur_uv.y * 2.0);
    float3 cam_pos = camera_position_tan_y.xyz;
    float3 cam_r = camera_right_tan_x.xyz;
    float3 cam_u = camera_up_rcp_width.xyz;
    float3 cam_f = camera_forward_rcp_height.xyz;
    float3 ray_dir = normalize(cam_r * (ndc.x * camera_right_tan_x.w) + cam_u * (ndc.y * camera_position_tan_y.w) + cam_f);
    if (abs(ray_dir.z) < 1e-4) discard;
    float t_glass = (8.6 - cam_pos.z) / ray_dir.z;
    float opaque_view_z = corridor_opaque_depth_tex.Load(int3(int2(input.position.xy), 0));
    float ray_cos = max(dot(ray_dir, cam_f), 1e-4);
    float opaque_dist = opaque_view_z / ray_cos;
    if (t_glass <= 0.08 || t_glass >= opaque_dist) discard;
    float3 hit = cam_pos + ray_dir * t_glass;
    if (abs(hit.x) > 2.3 || hit.y < 0.15 || hit.y > 3.05) discard;
    float scene_time = previous_camera_forward_unused.w;
    float stripe = step(0.82, frac((hit.x + hit.y) * 2.5 + scene_time * 0.25));
    float fresnel = pow(1.0 - saturate(abs(ray_dir.z)), 3.0);
    float3 glass_col = lerp(float3(0.18, 0.62, 0.78), float3(0.88, 0.96, 1.0), saturate(stripe * 0.55 + fresnel * 0.75));
    float alpha = saturate(0.36 + stripe * 0.20 + fresnel * 0.30);
    return float4(glass_col, alpha);
}
