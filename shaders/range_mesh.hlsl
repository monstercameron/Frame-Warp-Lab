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
    float3 world_pos = input.world_pos;
    float3 geo_n = normalize(input.world_normal);
    float3 geo_t = normalize(input.world_tangent - geo_n * dot(input.world_tangent, geo_n));
    float3 geo_b = normalize(cross(geo_n, geo_t));

    float slice = clamp((float)input.material_id, 0.0, 11.0);
    float2 duv_dx = ddx(input.uv);
    float2 duv_dy = ddy(input.uv);

    float3 albedo;
    float3 ts_n;
    float4 orm;
    if (range_flags.y > 0.5) {
        albedo = pbr_albedo_array.SampleGrad(pbr_aniso_sampler, float3(input.uv, slice), duv_dx, duv_dy).rgb;
        ts_n = pbr_normal_array.SampleGrad(pbr_aniso_sampler, float3(input.uv, slice), duv_dx, duv_dy).xyz * 2.0 - 1.0;
        orm = pbr_orm_array.SampleGrad(pbr_aniso_sampler, float3(input.uv, slice), duv_dx, duv_dy);
    } else {
        albedo = pbr_albedo_array.SampleLevel(pbr_aniso_sampler, float3(input.uv, slice), 0.0).rgb;
        ts_n = pbr_normal_array.SampleLevel(pbr_aniso_sampler, float3(input.uv, slice), 0.0).xyz * 2.0 - 1.0;
        orm = pbr_orm_array.SampleLevel(pbr_aniso_sampler, float3(input.uv, slice), 0.0);
    }

    albedo *= input.tint_emissive.rgb;
    float3 n = normalize(geo_t * ts_n.x + geo_b * ts_n.y + geo_n * max(ts_n.z, 0.15));
    float ao = saturate(orm.r);
    float perceptual_roughness = clamp(orm.g, 0.08, 1.0);
    float metallic = saturate(orm.b);
    float clearcoat = saturate(orm.a);

    float3 view_dir = normalize(camera_position_tan_y.xyz - world_pos);
    float nov = max(dot(n, view_dir), 1e-4);

    if (range_flags.y > 0.5) {
        float3 dndu = ddx(geo_n);
        float3 dndv = ddy(geo_n);
        float geom_var = min(max(dot(dndu, dndu), dot(dndv, dndv)) * 0.5, 0.14);
        float a0 = perceptual_roughness * perceptual_roughness;
        perceptual_roughness = sqrt(sqrt(saturate(a0 * a0 + geom_var)));
    }
    float alpha = max(0.008, perceptual_roughness * perceptual_roughness);
    float cc_roughness = 0.12;
    float cc_alpha = cc_roughness * cc_roughness;

    float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float3 diffuse_color = albedo * (1.0 - metallic);
    float2 dfg = pbr_dfg_lut.SampleLevel(linear_clamp_sampler, float2(nov, perceptual_roughness), 0.0);
    float3 energy_comp = 1.0 + f0 * (rcp(max(dfg.x + dfg.y, 0.08)) - 1.0);

    float3 refl = reflect(-view_dir, n);
    float3 env_diff = pbr_env_cubemap.SampleLevel(linear_clamp_sampler, n, 5.0).rgb;
    float3 env_spec = pbr_env_cubemap.SampleLevel(linear_clamp_sampler, refl, perceptual_roughness * 4.0).rgb;
    float spec_ao = pbr_specular_ao(nov, ao, perceptual_roughness);
    float3 color = (diffuse_color * env_diff * ao + env_spec * (f0 * dfg.x + dfg.y) * energy_comp * spec_ao) * 0.72;

    if (clearcoat > 0.005) {
        float2 dfg_cc = pbr_dfg_lut.SampleLevel(linear_clamp_sampler, float2(nov, cc_roughness), 0.0);
        float fc_ibl = (0.04 + 0.96 * pow(saturate(1.0 - nov), 5.0)) * clearcoat;
        float3 env_cc = pbr_env_cubemap.SampleLevel(linear_clamp_sampler, reflect(-view_dir, geo_n), cc_roughness * 4.0).rgb;
        color = color * (1.0 - fc_ibl) + env_cc * (0.04 * dfg_cc.x + dfg_cc.y) * clearcoat * spec_ao * 0.85;
    }

    float3 key_dir = normalize(float3(-0.28, 0.86, -0.42));
    color += pbr_eval_direct_light(n, view_dir, key_dir, float3(2.2, 2.0, 1.75), diffuse_color, f0, perceptual_roughness, alpha, energy_comp, clearcoat, cc_alpha);

    static const float3 bay_lights[3] = {
        float3(0.0, 4.35, 4.0),
        float3(0.0, 4.35, 14.4),
        float3(0.0, 4.35, 24.8),
    };
    static const float3 bay_colors[3] = {
        float3(9.5, 9.8, 10.8),
        float3(11.0, 10.2, 8.8),
        float3(10.5, 9.5, 8.2),
    };
    [unroll]
    for (int li = 0; li < 3; ++li) {
        float3 to_l = bay_lights[li] - world_pos;
        float d2 = max(dot(to_l, to_l), 0.65);
        color += pbr_eval_direct_light(n, view_dir, to_l * rsqrt(d2), bay_colors[li] / (1.0 + 0.11 * d2), diffuse_color, f0, perceptual_roughness, alpha, energy_comp, clearcoat, cc_alpha);
    }

    if (muzzle_light_pos_intensity.w > 0.01) {
        float3 to_m = muzzle_light_pos_intensity.xyz - world_pos;
        float md2 = max(dot(to_m, to_m), 0.08);
        float3 mrad = float3(18.0, 11.5, 4.2) * (muzzle_light_pos_intensity.w / (1.0 + 1.4 * md2));
        color += pbr_eval_direct_light(n, view_dir, to_m * rsqrt(md2), mrad, diffuse_color, f0, perceptual_roughness, alpha, energy_comp, clearcoat, cc_alpha);
    }

    float em_total = input.emissive_weight * max(input.tint_emissive.w, 1.0) + (input.emissive_weight < 0.01 ? input.tint_emissive.w : 0.0);
    if (em_total > 0.001) {
        float3 em_col = (input.emissive_weight > 0.5 && input.tint_emissive.w > 0.5)
            ? input.tint_emissive.rgb
            : (input.emissive_weight > 0.5 ? float3(1.0, 0.96, 0.88) : input.tint_emissive.rgb);
        color += em_col * em_total;
    }

    float view_z = max(dot(world_pos - camera_position_tan_y.xyz, camera_forward_rcp_height.xyz), 0.04);
    if (draw_meta.y == 0u) {
        color = lerp(color, float3(0.08, 0.09, 0.11), saturate(view_z * (1.0 / 95.0)));
    }

    float2 current_uv = input.clip_pos.xy * float2(camera_up_rcp_width.w, camera_forward_rcp_height.w);
    float2 motion_vector = float2(0.0, 0.0);
    if (previous_camera_up_cut.w < 0.5) {
        motion_vector = project_prev_camera_uv(input.prev_world_pos) - current_uv;
    }

    SceneOutput out_px;
    out_px.color = float4(pbr_aces_filmic(color * 1.10), 1.0);
    out_px.depth = view_z;
    out_px.motion = motion_vector;
    return out_px;
}
