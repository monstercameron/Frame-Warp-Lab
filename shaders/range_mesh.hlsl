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

float3 eval_rect_area_light(
    float3 world_pos,
    float3 n,
    float3 view_dir,
    float3 rect_center,
    float2 rect_half_xz,
    float3 radiance,
    float3 diffuse_color,
    float3 f0,
    float perceptual_roughness,
    float alpha,
    float3 energy_comp,
    float clearcoat,
    float cc_alpha) {
    float3 closest_diff = float3(
        clamp(world_pos.x, rect_center.x - rect_half_xz.x, rect_center.x + rect_half_xz.x),
        rect_center.y,
        clamp(world_pos.z, rect_center.z - rect_half_xz.y, rect_center.z + rect_half_xz.y));
    float3 to_diff = closest_diff - world_pos;
    float d2_diff = max(dot(to_diff, to_diff), 0.35);
    float dist_diff = sqrt(d2_diff);
    float3 l_diff = to_diff / dist_diff;
    float facing_diff = saturate(-l_diff.y * 1.35 + 0.15);
    float nol_diff = saturate(dot(n, l_diff));
    float area_term = (rect_half_xz.x * rect_half_xz.y * 4.0) / (d2_diff + 1.25);

    float3 refl = reflect(-view_dir, n);
    float3 mrp = closest_diff;
    if (refl.y > 0.03 && rect_center.y > world_pos.y) {
        float t_plane = (rect_center.y - world_pos.y) / refl.y;
        float3 hit_p = world_pos + refl * min(t_plane, 36.0);
        mrp = float3(
            clamp(hit_p.x, rect_center.x - rect_half_xz.x, rect_center.x + rect_half_xz.x),
            rect_center.y,
            clamp(hit_p.z, rect_center.z - rect_half_xz.y, rect_center.z + rect_half_xz.y));
    }
    float3 to_spec = mrp - world_pos;
    float d2_spec = max(dot(to_spec, to_spec), 0.25);
    float dist_spec = sqrt(d2_spec);
    float3 l_spec = to_spec / dist_spec;
    float nol_spec = saturate(dot(n, l_spec));
    float facing_spec = saturate(-l_spec.y * 1.45 + 0.12);

    float nov = max(dot(n, view_dir), 1e-4);
    float3 h = normalize(view_dir + l_spec);
    float noh = saturate(dot(n, h));
    float voh = saturate(dot(view_dir, h));
    float radius_eff = min(rect_half_xz.x, rect_half_xz.y) * 0.85;
    float alpha_prime = saturate(alpha + radius_eff / (2.0 * dist_spec));
    float norm_factor = (alpha * alpha) / max(alpha_prime * alpha_prime, 1e-4);
    float d_ggx = pbr_d_ggx(noh, alpha_prime) * norm_factor;
    float vis = pbr_v_smith_ggx_correlated(nov, max(nol_spec, 1e-4), alpha);
    float3 f = pbr_f_schlick(voh, f0, 1.0);
    float3 spec = (d_ggx * vis) * f * energy_comp * nol_spec * facing_spec;

    float3 diff = diffuse_color * k_pbr_inv_pi * nol_diff * facing_diff;
    float3 total = (diff + spec) * radiance * area_term;
    if (clearcoat > 0.005 && nol_spec > 0.0) {
        float cc_alpha_p = saturate(cc_alpha + radius_eff / (2.0 * dist_spec));
        float cc_norm = (cc_alpha * cc_alpha) / max(cc_alpha_p * cc_alpha_p, 1e-4);
        float d_cc = pbr_d_ggx(noh, cc_alpha_p) * cc_norm;
        float v_cc = pbr_v_kelemen(voh);
        float fc = (0.04 + 0.96 * pow(1.0 - voh, 5.0)) * clearcoat;
        total = total * (1.0 - fc) + radiance * area_term * (d_cc * v_cc * fc * nol_spec * facing_spec);
    }
    return total;
}

float3 eval_spot_light(
    float3 world_pos,
    float3 n,
    float3 view_dir,
    float3 light_pos,
    float3 spot_dir,
    float cos_inner,
    float cos_outer,
    float3 radiance,
    float atten_k,
    float3 diffuse_color,
    float3 f0,
    float perceptual_roughness,
    float alpha,
    float3 energy_comp,
    float clearcoat,
    float cc_alpha) {
    float3 to_l = light_pos - world_pos;
    float d2 = max(dot(to_l, to_l), 0.35);
    float3 l = to_l * rsqrt(d2);
    float cos_theta = dot(-l, spot_dir);
    float cone = saturate((cos_theta - cos_outer) / max(cos_inner - cos_outer, 1e-4));
    cone = cone * cone;
    if (cone <= 1e-4) return float3(0.0, 0.0, 0.0);
    float3 eff_rad = radiance * (cone / (1.0 + atten_k * d2));
    return pbr_eval_direct_light(n, view_dir, l, eff_rad, diffuse_color, f0, perceptual_roughness, alpha, energy_comp, clearcoat, cc_alpha);
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
    if (input.material_id == 12u) {
        if (range_flags.y > 0.5) {
            albedo = pbr_hero_albedo.SampleGrad(pbr_aniso_sampler, input.uv, duv_dx, duv_dy).rgb;
            ts_n = pbr_hero_normal.SampleGrad(pbr_aniso_sampler, input.uv, duv_dx, duv_dy).xyz * 2.0 - 1.0;
            orm = pbr_hero_orm.SampleGrad(pbr_aniso_sampler, input.uv, duv_dx, duv_dy);
        } else {
            albedo = pbr_hero_albedo.SampleLevel(pbr_aniso_sampler, input.uv, 0.0).rgb;
            ts_n = pbr_hero_normal.SampleLevel(pbr_aniso_sampler, input.uv, 0.0).xyz * 2.0 - 1.0;
            orm = pbr_hero_orm.SampleLevel(pbr_aniso_sampler, input.uv, 0.0);
        }
    } else if (range_flags.y > 0.5) {
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
    float3 color = (diffuse_color * env_diff * ao + env_spec * (f0 * dfg.x + dfg.y) * energy_comp * spec_ao) * 0.42;

    if (clearcoat > 0.005) {
        float2 dfg_cc = pbr_dfg_lut.SampleLevel(linear_clamp_sampler, float2(nov, cc_roughness), 0.0);
        float fc_ibl = (0.04 + 0.96 * pow(saturate(1.0 - nov), 5.0)) * clearcoat;
        float3 env_cc = pbr_env_cubemap.SampleLevel(linear_clamp_sampler, reflect(-view_dir, geo_n), cc_roughness * 4.0).rgb;
        color = color * (1.0 - fc_ibl) + env_cc * (0.04 * dfg_cc.x + dfg_cc.y) * clearcoat * spec_ao * 0.55;
    }

    static const float area_z[5] = { 5.4, 10.6, 15.8, 21.0, 26.2 };
    static const float3 area_rad[5] = {
        float3(10.5, 11.8, 13.8),
        float3(10.0, 11.0, 12.2),
        float3(11.2, 10.6, 9.2),
        float3(11.5, 10.4, 8.8),
        float3(10.8, 9.6, 7.8),
    };
    [unroll]
    for (int ai = 0; ai < 5; ++ai) {
        color += eval_rect_area_light(
            world_pos, n, view_dir,
            float3(0.0, 4.55, area_z[ai]), float2(1.40, 0.14),
            area_rad[ai],
            diffuse_color, f0, perceptual_roughness, alpha, energy_comp, clearcoat, cc_alpha);
    }

    color += eval_spot_light(
        world_pos, n, view_dir,
        float3(0.0, 4.24, 8.2), normalize(float3(0.0, -0.88, 0.47)),
        0.84, 0.58, float3(24.0, 22.8, 20.5), 0.075,
        diffuse_color, f0, perceptual_roughness, alpha, energy_comp, clearcoat, cc_alpha);
    color += eval_spot_light(
        world_pos, n, view_dir,
        float3(-0.35, 4.24, 14.4), normalize(float3(0.0, -0.89, 0.45)),
        0.82, 0.54, float3(25.0, 23.0, 19.5), 0.065,
        diffuse_color, f0, perceptual_roughness, alpha, energy_comp, clearcoat, cc_alpha);
    color += eval_spot_light(
        world_pos, n, view_dir,
        float3(0.25, 4.24, 21.0), normalize(float3(0.0, -0.88, 0.47)),
        0.82, 0.54, float3(26.0, 23.2, 19.0), 0.060,
        diffuse_color, f0, perceptual_roughness, alpha, energy_comp, clearcoat, cc_alpha);

    color += eval_spot_light(
        world_pos, n, view_dir,
        float3(-5.95, 3.15, 4.25), normalize(float3(0.76, -0.52, 0.39)),
        0.72, 0.25, float3(22.0, 1.4, 0.9), 0.11,
        diffuse_color, f0, perceptual_roughness, alpha, energy_comp, clearcoat, cc_alpha);
    color += eval_spot_light(
        world_pos, n, view_dir,
        float3(5.95, 3.15, 4.25), normalize(float3(-0.76, -0.52, 0.39)),
        0.72, 0.25, float3(22.0, 1.4, 0.9), 0.11,
        diffuse_color, f0, perceptual_roughness, alpha, energy_comp, clearcoat, cc_alpha);

    color += eval_spot_light(
        world_pos, n, view_dir,
        float3(-5.60, 3.38, 28.5), normalize(float3(0.68, -0.54, 0.49)),
        0.74, 0.30, float3(22.0, 10.5, 1.6), 0.075,
        diffuse_color, f0, perceptual_roughness, alpha, energy_comp, clearcoat, cc_alpha);
    color += eval_spot_light(
        world_pos, n, view_dir,
        float3(5.60, 3.38, 28.5), normalize(float3(-0.68, -0.54, 0.49)),
        0.74, 0.30, float3(22.0, 10.5, 1.6), 0.075,
        diffuse_color, f0, perceptual_roughness, alpha, energy_comp, clearcoat, cc_alpha);

    {
        static const float3 cyan_pos[2] = { float3(-2.15, 0.92, 4.35), float3(2.15, 0.92, 4.35) };
        [unroll]
        for (int ci = 0; ci < 2; ++ci) {
            float3 to_c = cyan_pos[ci] - world_pos;
            float cd2 = max(dot(to_c, to_c), 0.25);
            color += pbr_eval_direct_light(
                n, view_dir, to_c * rsqrt(cd2),
                float3(1.1, 7.4, 12.2) / (1.0 + 0.38 * cd2),
                diffuse_color, f0, perceptual_roughness, alpha, energy_comp, clearcoat, cc_alpha);
        }
    }

    if (muzzle_light_pos_intensity.w > 0.01) {
        float3 to_m = muzzle_light_pos_intensity.xyz - world_pos;
        float md2 = max(dot(to_m, to_m), 0.08);
        float3 mrad = float3(20.0, 12.0, 4.2) * (muzzle_light_pos_intensity.w / (1.0 + 1.3 * md2));
        color += pbr_eval_direct_light(n, view_dir, to_m * rsqrt(md2), mrad, diffuse_color, f0, perceptual_roughness, alpha, energy_comp, clearcoat, cc_alpha);
    }

    if (input.emissive_weight > 25.0) {
        color += float3(0.16, 0.85, 1.35) * 3.8;
    } else if (input.emissive_weight > 15.0) {
        color += float3(1.45, 0.72, 0.12) * 3.8;
    } else if (input.emissive_weight > 8.0) {
        color += float3(1.65, 0.10, 0.08) * 4.4;
    } else {
        float em_total = input.emissive_weight * max(input.tint_emissive.w, 1.0) + (input.emissive_weight < 0.01 ? input.tint_emissive.w : 0.0);
        if (em_total > 0.001) {
            float3 em_col = (input.emissive_weight > 0.5 && input.tint_emissive.w > 0.5)
                ? input.tint_emissive.rgb
                : (input.emissive_weight > 0.5 ? float3(0.96, 0.97, 1.0) : input.tint_emissive.rgb);
            color += em_col * em_total;
        }
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
