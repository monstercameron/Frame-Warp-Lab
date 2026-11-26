cbuffer PresentConstants : register(b0) {
    float4 viewport_and_flags;
};

Texture2D<float4> scene_color_texture : register(t0);
Texture2D<float> scene_depth_texture : register(t1);
Texture2D<float2> scene_motion_texture : register(t2);
SamplerState linear_clamp_sampler : register(s0);

struct VertexOutput {
    float4 position : SV_Position;
};

VertexOutput vertex_main(uint vertex_id : SV_VertexID) {
    VertexOutput output;
    float2 corner = float2((vertex_id << 1) & 2, vertex_id & 2);
    output.position = float4(corner * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

float4 pixel_main(VertexOutput input) : SV_Target {
    float2 viewport_size = viewport_and_flags.xy;
    int diagnostic_view = (int)(viewport_and_flags.z + 0.5);
    bool overlay_enabled = viewport_and_flags.w > 0.5;
    float2 uv = input.position.xy / viewport_size;

    float3 output_color = float3(0.0, 0.0, 0.0);
    if (diagnostic_view == 1) {
        float linear_depth = scene_depth_texture.SampleLevel(linear_clamp_sampler, uv, 0);
        float normalized_depth = saturate(1.0 - linear_depth / 32.0);
        output_color = float3(normalized_depth, normalized_depth * 0.9, normalized_depth * 0.75);
    } else if (diagnostic_view == 2) {
        float2 motion = scene_motion_texture.SampleLevel(linear_clamp_sampler, uv, 0);
        float2 scaled = clamp(motion * 24.0, -1.0, 1.0);
        output_color = float3(saturate(scaled.x * 0.5 + 0.5), saturate(scaled.y * 0.5 + 0.5), saturate(length(scaled)));
    } else {
        float3 linear_color = scene_color_texture.SampleLevel(linear_clamp_sampler, uv, 0).rgb;
        output_color = pow(saturate(linear_color), 1.0 / 2.2);
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
