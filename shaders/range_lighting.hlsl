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

float3 eval_range_moving_spot_pos(float scene_time) {
    return float3(3.1 * sin(scene_time * 1.65), 4.05, 11.2 + 4.4 * cos(scene_time * 1.15));
}

float eval_range_moving_shadow_vis(float3 world_pos, float scene_time) {
    float3 light_pos = eval_range_moving_spot_pos(scene_time);
    float3 blocker_c = float3(1.85 * sin(scene_time * 2.15), 2.35, 10.8 + 2.8 * sin(scene_time * 1.35));
    float blocker_r = 0.62;
    float3 seg = light_pos - world_pos;
    float seg_len2 = dot(seg, seg);
    if (seg_len2 < 1e-4) return 1.0;
    float t = saturate(dot(blocker_c - world_pos, seg) / seg_len2);
    if (t <= 0.01 || t >= 0.99) return 1.0;
    float3 closest = world_pos + seg * t;
    float3 d = closest - blocker_c;
    float dist2 = dot(d, d);
    return dist2 < (blocker_r * blocker_r) ? 0.08 : 1.0;
}

uint eval_range_shadow_signature(float3 world_pos, float scene_time, uint shader_flags) {
    uint sig = 0u;
    if ((shader_flags & 32u) != 0u) {
        float vis = eval_range_moving_shadow_vis(world_pos, scene_time);
        sig |= (vis > 0.5) ? 1u : 0u;
    }
    if ((shader_flags & 8u) != 0u) {
        float3 lp = eval_range_moving_spot_pos(scene_time);
        float3 to_l = lp - world_pos;
        float d2 = max(dot(to_l, to_l), 0.30);
        float env = saturate(18.0 / (1.0 + 0.14 * d2));
        uint q = min(31u, (uint)(env * 31.0 + 0.5));
        sig |= (q << 1u);
    }
    return sig;
}

float3 shade_range_surface_pbr(
    float3 world_pos,
    float3 geo_n,
    float3 geo_t,
    float2 uv,
    float2 duv_dx,
    float2 duv_dy,
    float3 dndu,
    float3 dndv,
    uint material_id,
    float4 tint_emissive,
    float emissive_weight,
    bool is_viewmodel,
    bool aniso_enabled,
    float scene_time,
    uint shader_flags,
    float3 cam_pos,
    float3 cam_forward,
    float4 muzzle_light_pos_intensity,
    Texture2D<float4> hero_albedo_tex,
    Texture2D<float4> hero_normal_tex,
    Texture2D<float4> hero_orm_tex,
    SamplerState clamp_sampler) {
    geo_n = normalize(geo_n);
    geo_t = normalize(geo_t - geo_n * dot(geo_t, geo_n));
    float3 geo_b = normalize(cross(geo_n, geo_t));

    if ((shader_flags & 2u) != 0u && !is_viewmodel && (material_id == 6u || material_id == 8u)) {
        uv += float2(scene_time * 0.55, scene_time * 0.35);
    }

    float slice = clamp((float)material_id, 0.0, 11.0);
    float3 albedo;
    float3 ts_n;
    float4 orm;
    if (material_id == 12u) {
        if (aniso_enabled) {
            albedo = hero_albedo_tex.SampleGrad(pbr_aniso_sampler, uv, duv_dx, duv_dy).rgb;
            ts_n = hero_normal_tex.SampleGrad(pbr_aniso_sampler, uv, duv_dx, duv_dy).xyz * 2.0 - 1.0;
            orm = hero_orm_tex.SampleGrad(pbr_aniso_sampler, uv, duv_dx, duv_dy);
        } else {
            albedo = hero_albedo_tex.SampleLevel(pbr_aniso_sampler, uv, 0.0).rgb;
            ts_n = hero_normal_tex.SampleLevel(pbr_aniso_sampler, uv, 0.0).xyz * 2.0 - 1.0;
            orm = hero_orm_tex.SampleLevel(pbr_aniso_sampler, uv, 0.0);
        }
    } else if (aniso_enabled) {
        albedo = pbr_albedo_array.SampleGrad(pbr_aniso_sampler, float3(uv, slice), duv_dx, duv_dy).rgb;
        ts_n = pbr_normal_array.SampleGrad(pbr_aniso_sampler, float3(uv, slice), duv_dx, duv_dy).xyz * 2.0 - 1.0;
        orm = pbr_orm_array.SampleGrad(pbr_aniso_sampler, float3(uv, slice), duv_dx, duv_dy);
    } else {
        albedo = pbr_albedo_array.SampleLevel(pbr_aniso_sampler, float3(uv, slice), 0.0).rgb;
        ts_n = pbr_normal_array.SampleLevel(pbr_aniso_sampler, float3(uv, slice), 0.0).xyz * 2.0 - 1.0;
        orm = pbr_orm_array.SampleLevel(pbr_aniso_sampler, float3(uv, slice), 0.0);
    }

    albedo *= tint_emissive.rgb;
    float3 n = normalize(geo_t * ts_n.x + geo_b * ts_n.y + geo_n * max(ts_n.z, 0.15));
    float ao = saturate(orm.r);
    float perceptual_roughness = clamp(orm.g, 0.08, 1.0);
    float metallic = saturate(orm.b);
    float clearcoat = saturate(orm.a);

    float3 view_dir = normalize(cam_pos - world_pos);
    float nov = max(dot(n, view_dir), 1e-4);

    if (aniso_enabled) {
        float geom_var = min(max(dot(dndu, dndu), dot(dndv, dndv)) * 0.5, 0.14);
        float a0 = perceptual_roughness * perceptual_roughness;
        perceptual_roughness = sqrt(sqrt(saturate(a0 * a0 + geom_var)));
    }
    float alpha = max(0.008, perceptual_roughness * perceptual_roughness);
    float cc_roughness = 0.12;
    float cc_alpha = cc_roughness * cc_roughness;

    float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float3 diffuse_color = albedo * (1.0 - metallic);
    float2 dfg = pbr_dfg_lut.SampleLevel(clamp_sampler, float2(nov, perceptual_roughness), 0.0);
    float3 energy_comp = 1.0 + f0 * (rcp(max(dfg.x + dfg.y, 0.08)) - 1.0);

    float3 refl = reflect(-view_dir, n);
    float3 env_diff = pbr_env_cubemap.SampleLevel(clamp_sampler, n, 5.0).rgb;
    float3 env_spec = pbr_env_cubemap.SampleLevel(clamp_sampler, refl, perceptual_roughness * 4.0).rgb;
    float spec_ao = pbr_specular_ao(nov, ao, perceptual_roughness);
    float3 color = (diffuse_color * env_diff * ao + env_spec * (f0 * dfg.x + dfg.y) * energy_comp * spec_ao) * 0.42;

    if (clearcoat > 0.005) {
        float2 dfg_cc = pbr_dfg_lut.SampleLevel(clamp_sampler, float2(nov, cc_roughness), 0.0);
        float fc_ibl = (0.04 + 0.96 * pow(saturate(1.0 - nov), 5.0)) * clearcoat;
        float3 env_cc = pbr_env_cubemap.SampleLevel(clamp_sampler, reflect(-view_dir, geo_n), cc_roughness * 4.0).rgb;
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

    if ((shader_flags & (8u | 32u)) != 0u && !is_viewmodel) {
        float3 dyn_lp = eval_range_moving_spot_pos(scene_time);
        float3 to_dyn = dyn_lp - world_pos;
        float dyn_d2 = max(dot(to_dyn, to_dyn), 0.30);
        float dyn_vis = ((shader_flags & 32u) != 0u) ? eval_range_moving_shadow_vis(world_pos, scene_time) : 1.0;
        float3 dyn_rad = float3(34.0, 26.0, 12.0) * (dyn_vis / (1.0 + 0.14 * dyn_d2));
        color += pbr_eval_direct_light(n, view_dir, to_dyn * rsqrt(dyn_d2), dyn_rad, diffuse_color, f0, perceptual_roughness, alpha, energy_comp, clearcoat, cc_alpha);
        if ((shader_flags & 32u) != 0u) {
            color *= lerp(0.32, 1.0, dyn_vis);
        }
    }

    if (muzzle_light_pos_intensity.w > 0.01) {
        float3 to_m = muzzle_light_pos_intensity.xyz - world_pos;
        float md2 = max(dot(to_m, to_m), 0.08);
        float3 mrad = float3(20.0, 12.0, 4.2) * (muzzle_light_pos_intensity.w / (1.0 + 1.3 * md2));
        color += pbr_eval_direct_light(n, view_dir, to_m * rsqrt(md2), mrad, diffuse_color, f0, perceptual_roughness, alpha, energy_comp, clearcoat, cc_alpha);
    }

    float em_pulse = ((shader_flags & 16u) != 0u) ? (0.20 + 1.65 * (0.5 + 0.5 * sin(scene_time * 9.5 + world_pos.z * 0.7))) : 1.0;
    if (emissive_weight > 25.0) {
        color += float3(0.16, 0.85, 1.35) * (3.8 * em_pulse);
    } else if (emissive_weight > 15.0) {
        color += float3(1.45, 0.72, 0.12) * (3.8 * em_pulse);
    } else if (emissive_weight > 8.0) {
        color += float3(1.65, 0.10, 0.08) * (4.4 * em_pulse);
    } else {
        float em_total = emissive_weight * max(tint_emissive.w, 1.0) + (emissive_weight < 0.01 ? tint_emissive.w : 0.0);
        if (em_total > 0.001) {
            float3 em_col = (emissive_weight > 0.5 && tint_emissive.w > 0.5)
                ? tint_emissive.rgb
                : (emissive_weight > 0.5 ? float3(0.96, 0.97, 1.0) : tint_emissive.rgb);
            color += em_col * em_total;
        }
    }
    if ((shader_flags & 16u) != 0u && !is_viewmodel && material_id == 8u) {
        float stripe = step(0.84, frac(uv.x * 2.0 + scene_time * 1.4));
        color += float3(0.18, 1.85, 2.20) * stripe * (0.35 + 0.95 * em_pulse);
    }

    float view_z = max(dot(world_pos - cam_pos, cam_forward), 0.04);
    if (!is_viewmodel) {
        color = lerp(color, float3(0.08, 0.09, 0.11), saturate(view_z * (1.0 / 95.0)));
    }

    return pbr_aces_filmic(color * 1.10);
}

void sample_range_pbr_material(
    float3 geo_n,
    float3 geo_t,
    float2 uv,
    float2 duv_dx,
    float2 duv_dy,
    float3 dndu,
    float3 dndv,
    uint material_id,
    float4 tint_emissive,
    bool is_viewmodel,
    bool aniso_enabled,
    float scene_time,
    uint shader_flags,
    Texture2D<float4> hero_albedo_tex,
    Texture2D<float4> hero_normal_tex,
    Texture2D<float4> hero_orm_tex,
    out float3 out_albedo,
    out float3 out_n,
    out float3 out_geo_n,
    out float out_ao,
    out float out_roughness,
    out float out_metallic,
    out float out_clearcoat) {
    out_geo_n = normalize(geo_n);
    geo_t = normalize(geo_t - out_geo_n * dot(geo_t, out_geo_n));
    float3 geo_b = normalize(cross(out_geo_n, geo_t));

    if ((shader_flags & 2u) != 0u && !is_viewmodel && (material_id == 6u || material_id == 8u)) {
        uv += float2(scene_time * 0.55, scene_time * 0.35);
    }

    float slice = clamp((float)material_id, 0.0, 11.0);
    float3 ts_n;
    float4 orm;
    if (material_id == 12u) {
        if (aniso_enabled) {
            out_albedo = hero_albedo_tex.SampleGrad(pbr_aniso_sampler, uv, duv_dx, duv_dy).rgb;
            ts_n = hero_normal_tex.SampleGrad(pbr_aniso_sampler, uv, duv_dx, duv_dy).xyz * 2.0 - 1.0;
            orm = hero_orm_tex.SampleGrad(pbr_aniso_sampler, uv, duv_dx, duv_dy);
        } else {
            out_albedo = hero_albedo_tex.SampleLevel(pbr_aniso_sampler, uv, 0.0).rgb;
            ts_n = hero_normal_tex.SampleLevel(pbr_aniso_sampler, uv, 0.0).xyz * 2.0 - 1.0;
            orm = hero_orm_tex.SampleLevel(pbr_aniso_sampler, uv, 0.0);
        }
    } else if (aniso_enabled) {
        out_albedo = pbr_albedo_array.SampleGrad(pbr_aniso_sampler, float3(uv, slice), duv_dx, duv_dy).rgb;
        ts_n = pbr_normal_array.SampleGrad(pbr_aniso_sampler, float3(uv, slice), duv_dx, duv_dy).xyz * 2.0 - 1.0;
        orm = pbr_orm_array.SampleGrad(pbr_aniso_sampler, float3(uv, slice), duv_dx, duv_dy);
    } else {
        out_albedo = pbr_albedo_array.SampleLevel(pbr_aniso_sampler, float3(uv, slice), 0.0).rgb;
        ts_n = pbr_normal_array.SampleLevel(pbr_aniso_sampler, float3(uv, slice), 0.0).xyz * 2.0 - 1.0;
        orm = pbr_orm_array.SampleLevel(pbr_aniso_sampler, float3(uv, slice), 0.0);
    }

    out_albedo *= tint_emissive.rgb;
    out_n = normalize(geo_t * ts_n.x + geo_b * ts_n.y + out_geo_n * max(ts_n.z, 0.15));
    out_ao = saturate(orm.r);
    out_roughness = clamp(orm.g, 0.08, 1.0);
    out_metallic = saturate(orm.b);
    out_clearcoat = saturate(orm.a);
    if (aniso_enabled) {
        float geom_var = min(max(dot(dndu, dndu), dot(dndv, dndv)) * 0.5, 0.14);
        float a0 = out_roughness * out_roughness;
        out_roughness = sqrt(sqrt(saturate(a0 * a0 + geom_var)));
    }
}

float3 eval_spot_diffuse_irr(float3 world_pos, float3 n, float3 light_pos, float3 spot_dir, float cos_inner, float cos_outer, float3 radiance, float atten_k) {
    float3 to_l = light_pos - world_pos;
    float d2 = max(dot(to_l, to_l), 0.35);
    float3 l = to_l * rsqrt(d2);
    float cos_theta = dot(-l, spot_dir);
    float cone = saturate((cos_theta - cos_outer) / max(cos_inner - cos_outer, 1e-4));
    cone = cone * cone;
    float nol = saturate(dot(n, l));
    return radiance * (k_pbr_inv_pi * nol * cone / (1.0 + atten_k * d2));
}

float3 eval_range_diffuse_irradiance(
    float3 world_pos,
    float3 n,
    float ao,
    bool is_viewmodel,
    float scene_time,
    uint shader_flags,
    float4 muzzle_light_pos_intensity,
    SamplerState clamp_sampler) {
    float3 irr = pbr_env_cubemap.SampleLevel(clamp_sampler, n, 5.0).rgb * (0.42 * ao);

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
        float3 closest_diff = float3(
            clamp(world_pos.x, -1.40, 1.40),
            4.55,
            clamp(world_pos.z, area_z[ai] - 0.14, area_z[ai] + 0.14));
        float3 to_diff = closest_diff - world_pos;
        float d2_diff = max(dot(to_diff, to_diff), 0.35);
        float3 l_diff = to_diff * rsqrt(d2_diff);
        float facing_diff = saturate(-l_diff.y * 1.35 + 0.15);
        float nol_diff = saturate(dot(n, l_diff));
        float area_term = (1.40 * 0.14 * 4.0) / (d2_diff + 1.25);
        irr += area_rad[ai] * (k_pbr_inv_pi * nol_diff * facing_diff * area_term);
    }

    irr += eval_spot_diffuse_irr(world_pos, n, float3(0.0, 4.24, 8.2), normalize(float3(0.0, -0.88, 0.47)), 0.84, 0.58, float3(24.0, 22.8, 20.5), 0.075);
    irr += eval_spot_diffuse_irr(world_pos, n, float3(-0.35, 4.24, 14.4), normalize(float3(0.0, -0.89, 0.45)), 0.82, 0.54, float3(25.0, 23.0, 19.5), 0.065);
    irr += eval_spot_diffuse_irr(world_pos, n, float3(0.25, 4.24, 21.0), normalize(float3(0.0, -0.88, 0.47)), 0.82, 0.54, float3(26.0, 23.2, 19.0), 0.060);
    irr += eval_spot_diffuse_irr(world_pos, n, float3(-5.95, 3.15, 4.25), normalize(float3(0.76, -0.52, 0.39)), 0.72, 0.25, float3(22.0, 1.4, 0.9), 0.11);
    irr += eval_spot_diffuse_irr(world_pos, n, float3(5.95, 3.15, 4.25), normalize(float3(-0.76, -0.52, 0.39)), 0.72, 0.25, float3(22.0, 1.4, 0.9), 0.11);
    irr += eval_spot_diffuse_irr(world_pos, n, float3(-5.60, 3.38, 28.5), normalize(float3(0.68, -0.54, 0.49)), 0.74, 0.30, float3(22.0, 10.5, 1.6), 0.075);
    irr += eval_spot_diffuse_irr(world_pos, n, float3(5.60, 3.38, 28.5), normalize(float3(-0.68, -0.54, 0.49)), 0.74, 0.30, float3(22.0, 10.5, 1.6), 0.075);

    static const float3 cyan_pos[2] = { float3(-2.15, 0.92, 4.35), float3(2.15, 0.92, 4.35) };
    [unroll]
    for (int ci = 0; ci < 2; ++ci) {
        float3 to_c = cyan_pos[ci] - world_pos;
        float cd2 = max(dot(to_c, to_c), 0.25);
        float nol_c = saturate(dot(n, to_c * rsqrt(cd2)));
        irr += float3(1.1, 7.4, 12.2) * (k_pbr_inv_pi * nol_c / (1.0 + 0.38 * cd2));
    }

    if ((shader_flags & (8u | 32u)) != 0u && !is_viewmodel) {
        float3 dyn_lp = eval_range_moving_spot_pos(scene_time);
        float3 to_dyn = dyn_lp - world_pos;
        float dyn_d2 = max(dot(to_dyn, to_dyn), 0.30);
        float dyn_vis = ((shader_flags & 32u) != 0u) ? eval_range_moving_shadow_vis(world_pos, scene_time) : 1.0;
        float nol_d = saturate(dot(n, to_dyn * rsqrt(dyn_d2)));
        irr += float3(34.0, 26.0, 12.0) * (k_pbr_inv_pi * nol_d * dyn_vis / (1.0 + 0.14 * dyn_d2));
        if ((shader_flags & 32u) != 0u) {
            irr *= lerp(0.32, 1.0, dyn_vis);
        }
    }

    if (muzzle_light_pos_intensity.w > 0.01) {
        float3 to_m = muzzle_light_pos_intensity.xyz - world_pos;
        float md2 = max(dot(to_m, to_m), 0.08);
        float nol_m = saturate(dot(n, to_m * rsqrt(md2)));
        irr += float3(20.0, 12.0, 4.2) * (k_pbr_inv_pi * nol_m * muzzle_light_pos_intensity.w / (1.0 + 1.3 * md2));
    }

    return irr;
}

float3 eval_spot_specular_only(
    float3 world_pos,
    float3 n,
    float3 view_dir,
    float nov,
    float3 light_pos,
    float3 spot_dir,
    float cos_inner,
    float cos_outer,
    float3 radiance,
    float atten_k,
    float3 f0,
    float alpha,
    float3 energy_comp,
    float clearcoat,
    float cc_alpha) {
    float3 to_l = light_pos - world_pos;
    float d2 = max(dot(to_l, to_l), 0.35);
    float3 l = to_l * rsqrt(d2);
    float nol = saturate(dot(n, l));
    if (nol <= 1e-4) return float3(0.0, 0.0, 0.0);
    float cos_theta = dot(-l, spot_dir);
    float cone = saturate((cos_theta - cos_outer) / max(cos_inner - cos_outer, 1e-4));
    cone = cone * cone;
    if (cone <= 1e-4) return float3(0.0, 0.0, 0.0);
    float3 eff_rad = radiance * (cone * nol / (1.0 + atten_k * d2));
    float3 h = normalize(view_dir + l);
    float noh = saturate(dot(n, h));
    float loh = saturate(dot(l, h));
    float d_ggx = pbr_d_ggx(noh, alpha);
    float vis = pbr_v_smith_ggx_correlated(nov, nol, alpha);
    float3 f = pbr_f_schlick(loh, f0, 1.0);
    float3 spec = (d_ggx * vis) * f * energy_comp * eff_rad;
    if (clearcoat > 0.005) {
        float d_cc = pbr_d_ggx(noh, cc_alpha);
        float v_cc = pbr_v_kelemen(loh);
        float fc = (0.04 + 0.96 * pow(saturate(1.0 - loh), 5.0)) * clearcoat;
        spec = spec * (1.0 - fc) + (d_cc * v_cc * fc) * eff_rad;
    }
    return spec;
}

float3 eval_range_specular_and_composite(
    float3 world_pos,
    float3 cached_diffuse_irr,
    float3 albedo,
    float3 n,
    float3 geo_n,
    float2 uv,
    float ao,
    float perceptual_roughness,
    float metallic,
    float clearcoat,
    uint material_id,
    float4 tint_emissive,
    float emissive_weight,
    bool is_viewmodel,
    float scene_time,
    uint shader_flags,
    float3 cam_pos,
    float3 cam_forward,
    float4 muzzle_light_pos_intensity,
    SamplerState clamp_sampler) {
    float3 view_dir = normalize(cam_pos - world_pos);
    float nov = max(dot(n, view_dir), 1e-4);
    float alpha = max(0.008, perceptual_roughness * perceptual_roughness);
    float cc_roughness = 0.12;
    float cc_alpha = cc_roughness * cc_roughness;
    float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float3 diffuse_color = albedo * (1.0 - metallic);
    float2 dfg = pbr_dfg_lut.SampleLevel(clamp_sampler, float2(nov, perceptual_roughness), 0.0);
    float3 energy_comp = 1.0 + f0 * (rcp(max(dfg.x + dfg.y, 0.08)) - 1.0);

    float3 refl = reflect(-view_dir, n);
    float3 env_spec = pbr_env_cubemap.SampleLevel(clamp_sampler, refl, perceptual_roughness * 4.0).rgb;
    float spec_ao = pbr_specular_ao(nov, ao, perceptual_roughness);
    float3 color = diffuse_color * cached_diffuse_irr + env_spec * (f0 * dfg.x + dfg.y) * energy_comp * spec_ao * 0.42;

    if (clearcoat > 0.005) {
        float2 dfg_cc = pbr_dfg_lut.SampleLevel(clamp_sampler, float2(nov, cc_roughness), 0.0);
        float fc_ibl = (0.04 + 0.96 * pow(saturate(1.0 - nov), 5.0)) * clearcoat;
        float3 env_cc = pbr_env_cubemap.SampleLevel(clamp_sampler, reflect(-view_dir, geo_n), cc_roughness * 4.0).rgb;
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
        float3 closest_diff = float3(
            clamp(world_pos.x, -1.40, 1.40),
            4.55,
            clamp(world_pos.z, area_z[ai] - 0.14, area_z[ai] + 0.14));
        float3 to_diff = closest_diff - world_pos;
        float d2_diff = max(dot(to_diff, to_diff), 0.35);
        float area_term = (1.40 * 0.14 * 4.0) / (d2_diff + 1.25);
        float3 mrp = closest_diff;
        if (refl.y > 0.03 && 4.55 > world_pos.y) {
            float t_plane = (4.55 - world_pos.y) / refl.y;
            float3 hit_p = world_pos + refl * min(t_plane, 36.0);
            mrp = float3(
                clamp(hit_p.x, -1.40, 1.40),
                4.55,
                clamp(hit_p.z, area_z[ai] - 0.14, area_z[ai] + 0.14));
        }
        float3 to_spec = mrp - world_pos;
        float d2_spec = max(dot(to_spec, to_spec), 0.25);
        float dist_spec = sqrt(d2_spec);
        float3 l_spec = to_spec / dist_spec;
        float nol_spec = saturate(dot(n, l_spec));
        float facing_spec = saturate(-l_spec.y * 1.45 + 0.12);
        if (nol_spec > 1e-4) {
            float3 h = normalize(view_dir + l_spec);
            float noh = saturate(dot(n, h));
            float voh = saturate(dot(view_dir, h));
            float radius_eff = 0.14 * 0.85;
            float alpha_prime = saturate(alpha + radius_eff / (2.0 * dist_spec));
            float norm_factor = (alpha * alpha) / max(alpha_prime * alpha_prime, 1e-4);
            float d_ggx = pbr_d_ggx(noh, alpha_prime) * norm_factor;
            float vis = pbr_v_smith_ggx_correlated(nov, max(nol_spec, 1e-4), alpha);
            float3 f = pbr_f_schlick(voh, f0, 1.0);
            float3 sp = (d_ggx * vis) * f * energy_comp * nol_spec * facing_spec * area_rad[ai] * area_term;
            if (clearcoat > 0.005) {
                float cc_alpha_p = saturate(cc_alpha + radius_eff / (2.0 * dist_spec));
                float cc_norm = (cc_alpha * cc_alpha) / max(cc_alpha_p * cc_alpha_p, 1e-4);
                float d_cc = pbr_d_ggx(noh, cc_alpha_p) * cc_norm;
                float v_cc = pbr_v_kelemen(voh);
                float fc = (0.04 + 0.96 * pow(1.0 - voh, 5.0)) * clearcoat;
                sp = sp * (1.0 - fc) + area_rad[ai] * area_term * (d_cc * v_cc * fc * nol_spec * facing_spec);
            }
            color += sp;
        }
    }

    color += eval_spot_specular_only(world_pos, n, view_dir, nov, float3(0.0, 4.24, 8.2), normalize(float3(0.0, -0.88, 0.47)), 0.84, 0.58, float3(24.0, 22.8, 20.5), 0.075, f0, alpha, energy_comp, clearcoat, cc_alpha);
    color += eval_spot_specular_only(world_pos, n, view_dir, nov, float3(-0.35, 4.24, 14.4), normalize(float3(0.0, -0.89, 0.45)), 0.82, 0.54, float3(25.0, 23.0, 19.5), 0.065, f0, alpha, energy_comp, clearcoat, cc_alpha);
    color += eval_spot_specular_only(world_pos, n, view_dir, nov, float3(0.25, 4.24, 21.0), normalize(float3(0.0, -0.88, 0.47)), 0.82, 0.54, float3(26.0, 23.2, 19.0), 0.060, f0, alpha, energy_comp, clearcoat, cc_alpha);
    color += eval_spot_specular_only(world_pos, n, view_dir, nov, float3(-5.95, 3.15, 4.25), normalize(float3(0.76, -0.52, 0.39)), 0.72, 0.25, float3(22.0, 1.4, 0.9), 0.11, f0, alpha, energy_comp, clearcoat, cc_alpha);
    color += eval_spot_specular_only(world_pos, n, view_dir, nov, float3(5.95, 3.15, 4.25), normalize(float3(-0.76, -0.52, 0.39)), 0.72, 0.25, float3(22.0, 1.4, 0.9), 0.11, f0, alpha, energy_comp, clearcoat, cc_alpha);
    color += eval_spot_specular_only(world_pos, n, view_dir, nov, float3(-5.60, 3.38, 28.5), normalize(float3(0.68, -0.54, 0.49)), 0.74, 0.30, float3(22.0, 10.5, 1.6), 0.075, f0, alpha, energy_comp, clearcoat, cc_alpha);
    color += eval_spot_specular_only(world_pos, n, view_dir, nov, float3(5.60, 3.38, 28.5), normalize(float3(-0.68, -0.54, 0.49)), 0.74, 0.30, float3(22.0, 10.5, 1.6), 0.075, f0, alpha, energy_comp, clearcoat, cc_alpha);

    static const float3 cyan_pos[2] = { float3(-2.15, 0.92, 4.35), float3(2.15, 0.92, 4.35) };
    [unroll]
    for (int ci = 0; ci < 2; ++ci) {
        float3 to_c = cyan_pos[ci] - world_pos;
        float cd2 = max(dot(to_c, to_c), 0.25);
        float3 lc = to_c * rsqrt(cd2);
        float nol_c = saturate(dot(n, lc));
        if (nol_c > 1e-4) {
            float3 hc = normalize(view_dir + lc);
            float d_c = pbr_d_ggx(saturate(dot(n, hc)), alpha);
            float v_c = pbr_v_smith_ggx_correlated(nov, nol_c, alpha);
            float3 f_c = pbr_f_schlick(saturate(dot(lc, hc)), f0, 1.0);
            float3 rad_c = float3(1.1, 7.4, 12.2) * (nol_c / (1.0 + 0.38 * cd2));
            float3 sp_c = (d_c * v_c) * f_c * energy_comp * rad_c;
            if (clearcoat > 0.005) {
                float d_cc = pbr_d_ggx(saturate(dot(n, hc)), cc_alpha);
                float v_cc = pbr_v_kelemen(saturate(dot(lc, hc)));
                float fc = (0.04 + 0.96 * pow(saturate(1.0 - saturate(dot(lc, hc))), 5.0)) * clearcoat;
                sp_c = sp_c * (1.0 - fc) + (d_cc * v_cc * fc) * rad_c;
            }
            color += sp_c;
        }
    }

    if ((shader_flags & (8u | 32u)) != 0u && !is_viewmodel) {
        float3 dyn_lp = eval_range_moving_spot_pos(scene_time);
        float3 to_dyn = dyn_lp - world_pos;
        float dyn_d2 = max(dot(to_dyn, to_dyn), 0.30);
        float3 l = to_dyn * rsqrt(dyn_d2);
        float nol = saturate(dot(n, l));
        float dyn_vis = ((shader_flags & 32u) != 0u) ? eval_range_moving_shadow_vis(world_pos, scene_time) : 1.0;
        if (nol > 1e-4) {
            float3 h = normalize(view_dir + l);
            float d_ggx = pbr_d_ggx(saturate(dot(n, h)), alpha);
            float vis = pbr_v_smith_ggx_correlated(nov, nol, alpha);
            float3 f = pbr_f_schlick(saturate(dot(l, h)), f0, 1.0);
            color += (d_ggx * vis) * f * energy_comp * float3(34.0, 26.0, 12.0) * (nol * dyn_vis / (1.0 + 0.14 * dyn_d2));
        }
    }

    if (muzzle_light_pos_intensity.w > 0.01) {
        float3 to_m = muzzle_light_pos_intensity.xyz - world_pos;
        float md2 = max(dot(to_m, to_m), 0.08);
        float3 l = to_m * rsqrt(md2);
        float nol = saturate(dot(n, l));
        if (nol > 1e-4) {
            float3 h = normalize(view_dir + l);
            float d_ggx = pbr_d_ggx(saturate(dot(n, h)), alpha);
            float vis = pbr_v_smith_ggx_correlated(nov, nol, alpha);
            float3 f = pbr_f_schlick(saturate(dot(l, h)), f0, 1.0);
            color += (d_ggx * vis) * f * energy_comp * float3(20.0, 12.0, 4.2) * (nol * muzzle_light_pos_intensity.w / (1.0 + 1.3 * md2));
        }
    }

    float em_pulse = ((shader_flags & 16u) != 0u) ? (0.20 + 1.65 * (0.5 + 0.5 * sin(scene_time * 9.5 + world_pos.z * 0.7))) : 1.0;
    if (emissive_weight > 25.0) {
        color += float3(0.16, 0.85, 1.35) * (3.8 * em_pulse);
    } else if (emissive_weight > 15.0) {
        color += float3(1.45, 0.72, 0.12) * (3.8 * em_pulse);
    } else if (emissive_weight > 8.0) {
        color += float3(1.65, 0.10, 0.08) * (4.4 * em_pulse);
    } else {
        float em_total = emissive_weight * max(tint_emissive.w, 1.0) + (emissive_weight < 0.01 ? tint_emissive.w : 0.0);
        if (em_total > 0.001) {
            float3 em_col = (emissive_weight > 0.5 && tint_emissive.w > 0.5)
                ? tint_emissive.rgb
                : (emissive_weight > 0.5 ? float3(0.96, 0.97, 1.0) : tint_emissive.rgb);
            color += em_col * em_total;
        }
    }
    if ((shader_flags & 16u) != 0u && !is_viewmodel && material_id == 8u) {
        float stripe = step(0.84, frac(uv.x * 2.0 + scene_time * 1.4));
        color += float3(0.18, 1.85, 2.20) * stripe * (0.35 + 0.95 * em_pulse);
    }

    float view_z = max(dot(world_pos - cam_pos, cam_forward), 0.04);
    if (!is_viewmodel) {
        color = lerp(color, float3(0.08, 0.09, 0.11), saturate(view_z * (1.0 / 95.0)));
    }

    return pbr_aces_filmic(color * 1.10);
}
