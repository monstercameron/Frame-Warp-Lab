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

    float3 color = float3(0.035, 0.055, 0.07);
    float linear_view_depth = 1000.0;
    float2 motion_vector = float2(0.0, 0.0);
    if (material > 0.0) {
        float3 normal = surface_normal(world_position, ball_center);
        float light = 0.35 + 0.65 * saturate(dot(normal, float3(-0.392232, 0.784465, -0.490290)));
        color = surface_color(world_position, material, ball_center) * light;
        color = lerp(color, float3(0.12, 0.17, 0.20), saturate(distance_travelled * (1.0 / 55.0)));
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
