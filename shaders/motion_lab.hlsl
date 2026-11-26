cbuffer SceneParameters : register(b0) {
    float scene_time_seconds;
    float viewport_width;
    float viewport_height;
};

struct VertexOutput {
    float4 position : SV_Position;
};

VertexOutput vertex_main(uint vertex_id : SV_VertexID) {
    VertexOutput output;
    float2 position = float2((vertex_id << 1) & 2, vertex_id & 2);
    output.position = float4(position * float2(2, -2) + float2(-1, 1), 0, 1);
    return output;
}

float box_distance(float3 world_position, float3 half_size) {
    float3 offset = abs(world_position) - half_size;
    return length(max(offset, 0)) + min(max(offset.x, max(offset.y, offset.z)), 0);
}

float2 scene_distance(float3 world_position) {
    float2 nearest = float2(world_position.y, 1);
    float wall_distance = 4.0 - abs(world_position.x);
    if (wall_distance < nearest.x) nearest = float2(wall_distance, 2);
    float ceiling_distance = 3.6 - world_position.y;
    if (ceiling_distance < nearest.x) nearest = float2(ceiling_distance, 2);
    float end_wall_distance = 28.0 - world_position.z;
    if (end_wall_distance < nearest.x) nearest = float2(end_wall_distance, 3);
    for (int pillar_index = 0; pillar_index < 5; ++pillar_index) {
        float pillar_z = 7.0 + pillar_index * 3.1;
        float pillar_x = (pillar_index % 2 == 0) ? -1.35 : 1.35;
        float pillar_distance = box_distance(world_position - float3(pillar_x, 1.6, pillar_z), float3(0.20, 1.6, 0.20));
        if (pillar_distance < nearest.x) nearest = float2(pillar_distance, 4);
    }
    float3 ball_center = float3(1.8 * sin(scene_time_seconds * 1.3), 1.05, 12.2);
    float ball_distance = length(world_position - ball_center) - 0.65;
    if (ball_distance < nearest.x) nearest = float2(ball_distance, 5);
    float rail_distance = box_distance(world_position - float3(2.6, 1.2, 18), float3(0.025, 1.2, 4));
    if (rail_distance < nearest.x) nearest = float2(rail_distance, 6);
    return nearest;
}

float3 surface_normal(float3 world_position) {
    float epsilon = 0.003;
    return normalize(float3(
        scene_distance(world_position + float3(epsilon, 0, 0)).x - scene_distance(world_position - float3(epsilon, 0, 0)).x,
        scene_distance(world_position + float3(0, epsilon, 0)).x - scene_distance(world_position - float3(0, epsilon, 0)).x,
        scene_distance(world_position + float3(0, 0, epsilon)).x - scene_distance(world_position - float3(0, 0, epsilon)).x));
}

float3 surface_color(float3 world_position, float material) {
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
        float grid = max(step(0.90, frac(world_position.x * 3)), step(0.90, frac(world_position.y * 3)));
        return lerp(float3(0.18, 0.28, 0.34), float3(0.87, 0.90, 0.82), grid);
    }
    if (material < 4.5) {
        float bands = step(0.5, frac(world_position.y * 2.0));
        return lerp(float3(0.95, 0.45, 0.09), float3(0.14, 0.18, 0.20), bands);
    }
    if (material < 5.5) {
        float3 ball_center = float3(1.8 * sin(scene_time_seconds * 1.3), 1.05, 12.2);
        float3 local = world_position - ball_center;
        float checker = fmod(floor((atan2(local.z, local.x) + 3.14159) * 2.5) + floor((local.y + 1) * 5), 2);
        return lerp(float3(0.13, 0.56, 0.68), float3(0.92, 0.95, 0.86), checker);
    }
    return float3(0.95, 0.77, 0.27);
}

float4 pixel_main(VertexOutput input) : SV_Target {
    float2 screen = (2.0 * input.position.xy - float2(viewport_width, viewport_height)) / viewport_height;
    screen.y = -screen.y;
    float3 camera_position = float3(0.8 * sin(scene_time_seconds * 0.45), 1.45, 1.2 + 0.55 * sin(scene_time_seconds * 0.22));
    float camera_yaw = 0.16 * sin(scene_time_seconds * 0.35);
    float3 direction = normalize(float3(screen.x * 0.8, screen.y * 0.8, 1.4));
    direction.xz = float2(direction.x * cos(camera_yaw) + direction.z * sin(camera_yaw),
                          direction.z * cos(camera_yaw) - direction.x * sin(camera_yaw));
    float distance_travelled = 0;
    float material = 0;
    float3 world_position = camera_position;
    for (int step_index = 0; step_index < 100; ++step_index) {
        world_position = camera_position + direction * distance_travelled;
        float2 distance_sample = scene_distance(world_position);
        if (distance_sample.x < 0.003) { material = distance_sample.y; break; }
        distance_travelled += distance_sample.x;
        if (distance_travelled > 45) break;
    }
    float3 color = float3(0.035, 0.055, 0.07);
    if (material > 0) {
        float3 normal = surface_normal(world_position);
        float light = 0.35 + 0.65 * saturate(dot(normal, normalize(float3(-0.4, 0.8, -0.5))));
        color = surface_color(world_position, material) * light;
        color = lerp(color, float3(0.12, 0.17, 0.20), saturate(distance_travelled / 55.0));
    }
    return float4(pow(saturate(color), 1.0 / 2.2), 1);
}

