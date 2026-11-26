cbuffer RangeSceneConstants : register(b0) {
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
};

cbuffer RangeDrawConstants : register(b1) {
    uint4 draw_meta;
};

struct InstanceData {
    float4x4 model;
    float4x4 prev_model;
    float4 tint_emissive;
};

StructuredBuffer<InstanceData> instance_buffer : register(t0);
SamplerState linear_clamp_sampler : register(s0);

#include "pbr_materials.hlsl"

Texture2D<float4> pbr_hero_albedo : register(t8);
Texture2D<float4> pbr_hero_normal : register(t9);
Texture2D<float4> pbr_hero_orm : register(t10);

#include "range_lighting.hlsl"

struct RangeVertexIn {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float3 tangent : TANGENT;
    float2 uv : TEXCOORD0;
    uint material_id : TEXCOORD1;
    float emissive_weight : TEXCOORD2;
};

struct RangeVertexOut {
    float4 clip_pos : SV_Position;
    float3 world_pos : TEXCOORD0;
    float3 prev_world_pos : TEXCOORD1;
    float3 world_normal : TEXCOORD2;
    float3 world_tangent : TEXCOORD3;
    float2 uv : TEXCOORD4;
    nointerpolation uint material_id : TEXCOORD5;
    nointerpolation float4 tint_emissive : TEXCOORD6;
    float emissive_weight : TEXCOORD7;
};

struct SceneOutput {
    float4 color : SV_Target0;
    float depth : SV_Target1;
    float2 motion : SV_Target2;
};

struct VisBufferOut {
    uint vis_word : SV_Target0;
    float depth : SV_Target1;
    float2 motion : SV_Target2;
};

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

float2 project_prev_camera_uv(float3 prev_world_pos) {
    float3 rel = prev_world_pos - previous_camera_position_rcp_tan_y.xyz;
    float vx = dot(rel, previous_camera_right_rcp_tan_x.xyz);
    float vy = dot(rel, previous_camera_up_cut.xyz);
    float inv_z = rcp(max(dot(rel, previous_camera_forward_time.xyz), 0.01));
    float ndc_x = vx * inv_z * previous_camera_right_rcp_tan_x.w;
    float ndc_y = vy * inv_z * previous_camera_position_rcp_tan_y.w;
    return float2(ndc_x * 0.5 + 0.5, 0.5 - ndc_y * 0.5);
}

RangeVertexOut range_vs_main(RangeVertexIn input) {
    InstanceData inst = instance_buffer[draw_meta.x];
    float4 wp = mul(inst.model, float4(input.position, 1.0));
    float4 pwp = mul(inst.prev_model, float4(input.position, 1.0));
    float3 wn = normalize(mul(inst.model, float4(input.normal, 0.0)).xyz);
    float3 wt = normalize(mul(inst.model, float4(input.tangent, 0.0)).xyz);

    RangeVertexOut o;
    o.clip_pos = project_camera_clip(wp.xyz);
    o.world_pos = wp.xyz;
    o.prev_world_pos = pwp.xyz;
    o.world_normal = wn;
    o.world_tangent = wt;
    o.uv = input.uv;
    o.material_id = input.material_id;
    o.tint_emissive = inst.tint_emissive;
    o.emissive_weight = input.emissive_weight;
    return o;
}

SceneOutput range_ps_main(RangeVertexOut input) {
    float3 shaded = shade_range_surface_pbr(
        input.world_pos,
        input.world_normal,
        input.world_tangent,
        input.uv,
        ddx(input.uv),
        ddy(input.uv),
        ddx(normalize(input.world_normal)),
        ddy(normalize(input.world_normal)),
        input.material_id,
        input.tint_emissive,
        input.emissive_weight,
        draw_meta.y != 0u,
        range_flags.y > 0.5,
        camera_position_tan_y.xyz,
        camera_forward_rcp_height.xyz,
        muzzle_light_pos_intensity,
        pbr_hero_albedo,
        pbr_hero_normal,
        pbr_hero_orm,
        linear_clamp_sampler);

    float view_z = max(dot(input.world_pos - camera_position_tan_y.xyz, camera_forward_rcp_height.xyz), 0.04);
    float2 current_uv = input.clip_pos.xy * float2(camera_up_rcp_width.w, camera_forward_rcp_height.w);
    float2 motion_vector = float2(0.0, 0.0);
    if (previous_camera_up_cut.w < 0.5) {
        motion_vector = project_prev_camera_uv(input.prev_world_pos) - current_uv;
    }

    SceneOutput out_px;
    out_px.color = float4(shaded, 0.0);
    out_px.depth = view_z;
    out_px.motion = motion_vector;
    return out_px;
}

VisBufferOut range_vis_ps_main(RangeVertexOut input, uint prim_id : SV_PrimitiveID) {
    uint inst_encoded = (draw_meta.x + 1u) & 0x7Fu;
    uint vm_bit = draw_meta.y & 1u;
    uint tri_id = (draw_meta.z + prim_id) & 0xFFFFFFu;

    float view_z = max(dot(input.world_pos - camera_position_tan_y.xyz, camera_forward_rcp_height.xyz), 0.04);
    float2 current_uv = input.clip_pos.xy * float2(camera_up_rcp_width.w, camera_forward_rcp_height.w);
    float2 motion_vector = float2(0.0, 0.0);
    if (previous_camera_up_cut.w < 0.5) {
        motion_vector = project_prev_camera_uv(input.prev_world_pos) - current_uv;
    }

    VisBufferOut o;
    o.vis_word = (inst_encoded << 25u) | (vm_bit << 24u) | tri_id;
    o.depth = view_z;
    o.motion = motion_vector;
    return o;
}
