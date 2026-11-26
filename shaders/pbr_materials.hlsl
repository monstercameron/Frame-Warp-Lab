Texture2DArray<float4> pbr_albedo_array : register(t3);
Texture2DArray<float4> pbr_normal_array : register(t4);
Texture2DArray<float4> pbr_orm_array : register(t5);
Texture2D<float2> pbr_dfg_lut : register(t6);
TextureCube<float4> pbr_env_cubemap : register(t7);
SamplerState pbr_aniso_sampler : register(s2);

static const float k_pbr_pi = 3.14159265359;
static const float k_pbr_inv_pi = 0.31830988618;

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

float3 surface_color_ex(float3 world_position, float material, float3 ball_center, float scene_time, uint shader_flags) {
    float3 sample_pos = world_position;
    if ((shader_flags & 2u) != 0u) {
        sample_pos += float3(scene_time * 1.4, scene_time * 0.8, scene_time * 1.9);
    }
    if ((shader_flags & 1u) != 0u) {
        if (material < 1.5) {
            float hf = fmod(floor(sample_pos.x * 10.0) + floor(sample_pos.z * 10.0) + 2000.0, 2.0);
            return lerp(float3(0.08, 0.12, 0.15), float3(0.76, 0.82, 0.84), hf);
        }
        if (material < 2.5) {
            float hf = fmod(floor(sample_pos.y * 10.0) + floor(sample_pos.z * 10.0) + 2000.0, 2.0);
            return lerp(float3(0.14, 0.19, 0.23), float3(0.72, 0.78, 0.78), hf);
        }
        if (material < 3.5) {
            float hf = fmod(floor(sample_pos.x * 12.0) + floor(sample_pos.y * 12.0) + 2000.0, 2.0);
            return lerp(float3(0.12, 0.20, 0.26), float3(0.88, 0.90, 0.84), hf);
        }
    }
    return surface_color(sample_pos, material, ball_center);
}

float trace_hard_shadow(float3 origin, float3 light_pos, float3 ball_center) {
    float3 to_light = light_pos - origin;
    float max_dist = length(to_light);
    if (max_dist < 0.05) return 1.0;
    float3 dir = to_light / max_dist;
    float t = 0.04;
    [loop]
    for (int i = 0; i < 32; ++i) {
        float d = scene_distance(origin + dir * t, ball_center).x;
        if (d < 0.006) return 0.14;
        t += max(d, 0.03);
        if (t >= max_dist - 0.05) break;
    }
    return 1.0;
}

float trace_soft_shadow(float3 origin, float3 dir, float max_dist, float k_soft, float3 ball_center) {
    float res = 1.0;
    float t = 0.04;
    [loop]
    for (int i = 0; i < 20; ++i) {
        float d = scene_distance(origin + dir * t, ball_center).x;
        if (d < 0.004) return 0.08;
        res = min(res, k_soft * d / max(t, 0.04));
        t += clamp(d, 0.04, 0.65);
        if (t >= max_dist) break;
    }
    return saturate(res);
}

float3 apply_transparent_glass(float3 base_color, float3 ray_origin, float3 ray_dir, float opaque_dist, float scene_time) {
    if (abs(ray_dir.z) < 1e-4) return base_color;
    float t_glass = (8.6 - ray_origin.z) / ray_dir.z;
    if (t_glass <= 0.08 || t_glass >= opaque_dist) return base_color;
    float3 hit = ray_origin + ray_dir * t_glass;
    if (abs(hit.x) > 2.3 || hit.y < 0.15 || hit.y > 3.05) return base_color;
    float stripe = step(0.82, frac((hit.x + hit.y) * 2.5 + scene_time * 0.25));
    float fresnel = pow(1.0 - saturate(abs(ray_dir.z)), 3.0);
    float3 glass_col = lerp(float3(0.18, 0.62, 0.78), float3(0.88, 0.96, 1.0), saturate(stripe * 0.55 + fresnel * 0.75));
    float alpha = saturate(0.36 + stripe * 0.20 + fresnel * 0.30);
    return lerp(base_color, glass_col, alpha);
}

float3 evaluate_legacy_shader_color(
    float3 world_position,
    float3 ray_origin,
    float3 ray_dir,
    float distance_travelled,
    float material,
    float3 ball_center,
    float scene_time,
    uint shader_flags) {
    float3 normal = surface_normal(world_position, ball_center);
    if ((shader_flags & 64u) != 0u) {
        float3 bump = float3(
            sin(world_position.z * 26.0 + world_position.y * 18.0),
            cos(world_position.x * 24.0 + world_position.z * 22.0),
            sin(world_position.x * 20.0 - world_position.y * 26.0));
        normal = normalize(normal + bump * 0.34);
    }
    float3 albedo = surface_color_ex(world_position, material, ball_center, scene_time, shader_flags);
    float3 sun_dir = float3(-0.392232, 0.784465, -0.490290);
    float shadow_factor = 1.0;
    float3 pt_pos = float3(
        2.1 * sin(scene_time * 1.9),
        2.2 + 0.55 * cos(scene_time * 1.4),
        10.5 + 4.2 * cos(scene_time * 1.1));
    if ((shader_flags & 32u) != 0u) {
        shadow_factor = trace_hard_shadow(world_position + normal * 0.025, pt_pos, ball_center);
    }
    float diff_weight = (shader_flags & 32u) != 0u ? (0.24 + 0.76 * saturate(dot(normal, sun_dir)) * shadow_factor) : (0.35 + 0.65 * saturate(dot(normal, sun_dir)));
    float3 color = albedo * diff_weight;
    if ((shader_flags & 4u) != 0u) {
        float3 view_dir = normalize(ray_origin - world_position);
        float3 half_vec = normalize(sun_dir + view_dir);
        float spec = pow(saturate(dot(normal, half_vec)), 48.0) * shadow_factor;
        color += float3(0.95, 0.92, 0.86) * spec * 0.85;
    }
    if ((shader_flags & 8u) != 0u) {
        float3 to_pt = pt_pos - world_position;
        float dist_sq = max(dot(to_pt, to_pt), 0.25);
        float3 l_dir = to_pt * rsqrt(dist_sq);
        float atten = 10.5 / (1.0 + 0.22 * dist_sq);
        float pt_diff = saturate(dot(normal, l_dir)) * atten * shadow_factor;
        color += albedo * float3(1.0, 0.70, 0.36) * pt_diff;
        if ((shader_flags & 4u) != 0u) {
            float3 view_dir = normalize(ray_origin - world_position);
            float3 h_pt = normalize(l_dir + view_dir);
            color += float3(1.0, 0.85, 0.65) * pow(saturate(dot(normal, h_pt)), 40.0) * atten * 0.55 * shadow_factor;
        }
    }
    if ((shader_flags & 16u) != 0u) {
        float pulse = 0.5 + 0.5 * sin(scene_time * 10.0 + world_position.z * 1.5);
        if (material > 3.5 && material < 4.5) {
            float band = step(0.72, frac(world_position.y * 2.0));
            color += float3(0.12, 0.96, 1.0) * band * (0.20 + 1.40 * pulse);
        } else if (material > 2.5 && material < 3.5) {
            float grid = max(step(0.92, frac(world_position.x * 3.0)), step(0.92, frac(world_position.y * 3.0)));
            color += float3(1.0, 0.22, 0.68) * grid * (0.15 + 1.25 * (1.0 - pulse));
        }
    }
    color = lerp(color, float3(0.12, 0.17, 0.20), saturate(distance_travelled * (1.0 / 55.0)));
    if ((shader_flags & 128u) != 0u) {
        color = apply_transparent_glass(color, ray_origin, ray_dir, distance_travelled, scene_time);
    }
    return color;
}

float pbr_d_ggx(float noh, float alpha) {
    float a2 = alpha * alpha;
    float d = (noh * a2 - noh) * noh + 1.0;
    return a2 / max(k_pbr_pi * d * d, 1e-6);
}

float pbr_v_smith_ggx_correlated(float nov, float nol, float alpha) {
    float a2 = alpha * alpha;
    float ggxv = nol * sqrt(max(1e-6, nov * nov * (1.0 - a2) + a2));
    float ggxl = nov * sqrt(max(1e-6, nol * nol * (1.0 - a2) + a2));
    return 0.5 / max(ggxv + ggxl, 1e-5);
}

float3 pbr_f_schlick(float voh, float3 f0, float f90) {
    float x = saturate(1.0 - voh);
    float x2 = x * x;
    return f0 + (f90 - f0) * (x2 * x2 * x);
}

float pbr_fd_burley(float nov, float nol, float loh, float roughness) {
    float f90 = 0.5 + 2.0 * roughness * loh * loh;
    float fl = 1.0 + (f90 - 1.0) * pow(saturate(1.0 - nol), 5.0);
    float fv = 1.0 + (f90 - 1.0) * pow(saturate(1.0 - nov), 5.0);
    return fl * fv * k_pbr_inv_pi;
}

float pbr_v_kelemen(float loh) {
    return 0.25 / max(loh * loh, 1e-4);
}

float pbr_specular_ao(float nov, float ao, float roughness) {
    return saturate(pow(nov + ao, exp2(-16.0 * roughness - 1.0)) - 1.0 + ao);
}

float3 pbr_eval_direct_light(
    float3 n,
    float3 v,
    float3 l,
    float3 radiance,
    float3 diffuse_color,
    float3 f0,
    float roughness,
    float alpha,
    float3 energy_comp,
    float clearcoat,
    float clearcoat_alpha) {
    float nol = saturate(dot(n, l));
    if (nol <= 1e-4) return float3(0.0, 0.0, 0.0);
    float nov = max(dot(n, v), 1e-4);
    float3 h = normalize(v + l);
    float noh = saturate(dot(n, h));
    float loh = saturate(dot(l, h));

    float d = pbr_d_ggx(noh, alpha);
    float vis = pbr_v_smith_ggx_correlated(nov, nol, alpha);
    float3 f = pbr_f_schlick(loh, f0, 1.0);
    float3 spec = (d * vis) * f * energy_comp;
    float3 diff = diffuse_color * pbr_fd_burley(nov, nol, loh, roughness);
    float3 brdf = diff + spec;

    if (clearcoat > 0.005) {
        float d_cc = pbr_d_ggx(noh, clearcoat_alpha);
        float v_cc = pbr_v_kelemen(loh);
        float f_cc = (0.04 + 0.96 * pow(saturate(1.0 - loh), 5.0)) * clearcoat;
        brdf = brdf * (1.0 - f_cc) + (d_cc * v_cc * f_cc);
    }
    return brdf * radiance * nol;
}

float3 pbr_aces_filmic(float3 x) {
    float3 a = x * (x + 0.0245786) - 0.000090537;
    float3 b = x * (0.983729 * x + 0.4329510) + 0.238081;
    return saturate(a / max(b, 1e-5));
}

float3 pbr_linear_to_srgb(float3 c) {
    c = saturate(c);
    float3 lo = c * 12.92;
    float3 hi = 1.055 * pow(max(c, 1e-6), 1.0 / 2.4) - 0.055;
    return lerp(hi, lo, step(c, float3(0.0031308, 0.0031308, 0.0031308)));
}

void compute_surface_uv_and_tbn(
    float3 world_position,
    float3 geo_normal,
    float material,
    float3 ball_center,
    float scene_time,
    uint shader_flags,
    out float2 out_uv,
    out float3 out_t,
    out float3 out_b,
    out float out_uv_scale,
    out float out_slice) {
    float3 p = world_position;
    if ((shader_flags & 2u) != 0u) {
        p += float3(scene_time * 1.4, scene_time * 0.8, scene_time * 1.9);
    }
    out_slice = clamp(floor(material + 0.5) - 1.0, 0.0, 5.0);
    if (material > 4.5 && material < 5.5) {
        float3 local = normalize(world_position - ball_center);
        float phi = atan2(local.z, local.x);
        float theta = acos(clamp(local.y, -1.0, 1.0));
        out_uv = float2(phi / (2.0 * k_pbr_pi) + 0.5, theta / k_pbr_pi) * 2.0;
        if ((shader_flags & 2u) != 0u) {
            out_uv += float2(scene_time * 0.5, scene_time * 0.25);
        }
        out_t = normalize(cross(float3(0.0, 1.0, 0.0), local) + float3(1e-4, 0.0, 0.0));
        out_b = normalize(cross(geo_normal, out_t));
        out_uv_scale = 0.55;
        return;
    }

    float3 an = abs(geo_normal);
    if (an.y >= an.x && an.y >= an.z) {
        out_uv_scale = (material < 1.5) ? 0.55 : 0.45;
        out_t = float3(1.0, 0.0, 0.0);
        out_b = float3(0.0, 0.0, 1.0);
        out_uv = float2(p.x, p.z) * out_uv_scale;
    } else if (an.x >= an.z) {
        out_uv_scale = (material > 3.5 && material < 4.5) ? 0.95 : 0.45;
        out_t = float3(0.0, 0.0, 1.0);
        out_b = float3(0.0, 1.0, 0.0);
        out_uv = float2(p.z, p.y) * out_uv_scale;
    } else {
        out_uv_scale = (material > 3.5 && material < 4.5) ? 0.95 : 0.42;
        out_t = float3(1.0, 0.0, 0.0);
        out_b = float3(0.0, 1.0, 0.0);
        out_uv = float2(p.x, p.y) * out_uv_scale;
    }
    out_t = normalize(out_t - geo_normal * dot(out_t, geo_normal));
    out_b = normalize(cross(geo_normal, out_t));
}

float3 evaluate_pbr_shader_color(
    float3 world_position,
    float3 ray_origin,
    float3 ray_dir,
    float3 ray_dx,
    float3 ray_dy,
    float distance_travelled,
    float material,
    float3 ball_center,
    float scene_time,
    uint packed_flags,
    SamplerState linear_clamp_samp) {
    uint shader_flags = packed_flags & 255u;
    bool use_aniso_mip = (packed_flags & 512u) != 0u;

    float3 geo_normal = surface_normal(world_position, ball_center);
    float2 uv;
    float3 tangent;
    float3 bitangent;
    float uv_scale;
    float slice;
    compute_surface_uv_and_tbn(world_position, geo_normal, material, ball_center, scene_time, shader_flags, uv, tangent, bitangent, uv_scale, slice);

    float dist_pn = dot(world_position - ray_origin, geo_normal);
    float denom_x = dot(ray_dx, geo_normal);
    float denom_y = dot(ray_dy, geo_normal);
    float t_x = abs(denom_x) > 1e-4 ? clamp(dist_pn / denom_x, 0.01, 100.0) : distance_travelled;
    float t_y = abs(denom_y) > 1e-4 ? clamp(dist_pn / denom_y, 0.01, 100.0) : distance_travelled;
    float3 dP_dx = (ray_origin + ray_dx * t_x) - world_position;
    float3 dP_dy = (ray_origin + ray_dy * t_y) - world_position;
    float max_step = max(distance_travelled * 0.14, 0.02);
    float len_x = length(dP_dx);
    float len_y = length(dP_dy);
    if (len_x > max_step) dP_dx *= (max_step / len_x);
    if (len_y > max_step) dP_dy *= (max_step / len_y);
    float2 dUV_dx = float2(dot(dP_dx, tangent), dot(dP_dx, bitangent)) * uv_scale;
    float2 dUV_dy = float2(dot(dP_dy, tangent), dot(dP_dy, bitangent)) * uv_scale;

    float3 albedo;
    float3 ts_normal;
    float4 orm;
    if (use_aniso_mip) {
        albedo = pbr_albedo_array.SampleGrad(pbr_aniso_sampler, float3(uv, slice), dUV_dx, dUV_dy).rgb;
        ts_normal = pbr_normal_array.SampleGrad(pbr_aniso_sampler, float3(uv, slice), dUV_dx, dUV_dy).xyz * 2.0 - 1.0;
        orm = pbr_orm_array.SampleGrad(pbr_aniso_sampler, float3(uv, slice), dUV_dx, dUV_dy);
    } else {
        albedo = pbr_albedo_array.SampleLevel(pbr_aniso_sampler, float3(uv, slice), 0.0).rgb;
        ts_normal = pbr_normal_array.SampleLevel(pbr_aniso_sampler, float3(uv, slice), 0.0).xyz * 2.0 - 1.0;
        orm = pbr_orm_array.SampleLevel(pbr_aniso_sampler, float3(uv, slice), 0.0);
    }

    if ((shader_flags & 1u) != 0u) {
        float2 chk_uv = uv * 12.0;
        float2 fw = max(abs(dUV_dx) + abs(dUV_dy), float2(1e-4, 1e-4)) * 12.0;
        float2 i_chk = 2.0 * (abs(frac((chk_uv - 0.5 * fw) * 0.5) - 0.5) - abs(frac((chk_uv + 0.5 * fw) * 0.5) - 0.5)) / fw;
        float filt_checker = use_aniso_mip ? saturate(0.5 - 0.5 * i_chk.x * i_chk.y) : fmod(floor(chk_uv.x) + floor(chk_uv.y) + 2000.0, 2.0);
        albedo = lerp(albedo * 0.25, albedo * 1.65 + 0.12, filt_checker);
    }

    float3 normal = normalize(tangent * ts_normal.x + bitangent * ts_normal.y + geo_normal * max(ts_normal.z, 0.15));
    if ((shader_flags & 64u) != 0u) {
        float3 bump = float3(
            sin(world_position.z * 26.0 + world_position.y * 18.0),
            cos(world_position.x * 24.0 + world_position.z * 22.0),
            sin(world_position.x * 20.0 - world_position.y * 26.0));
        normal = normalize(normal + bump * 0.28);
    }

    float ao = saturate(orm.r);
    float perceptual_roughness = clamp(orm.g, 0.08, 1.0);
    float metallic = saturate(orm.b);
    float clearcoat = saturate(orm.a);

    float3 view_dir = normalize(ray_origin - world_position);
    float nov = max(dot(normal, view_dir), 1e-4);

    if (use_aniso_mip) {
        float uv_footprint = max(dot(dUV_dx, dUV_dx), dot(dUV_dy, dUV_dy));
        float geom_curv = (material > 4.5 && material < 5.5) ? (min(len_x * len_x + len_y * len_y, 0.08) * 1.4) : 0.0;
        float spec_aa_var = min(uv_footprint * 0.35 + geom_curv, 0.16);
        float alpha0 = perceptual_roughness * perceptual_roughness;
        perceptual_roughness = sqrt(sqrt(saturate(alpha0 * alpha0 + spec_aa_var)));
    }
    float alpha = max(0.008, perceptual_roughness * perceptual_roughness);
    float clearcoat_roughness = clamp(0.10 + (use_aniso_mip ? min(max(length(dUV_dx), length(dUV_dy)) * 0.4, 0.25) : 0.0), 0.08, 0.45);
    float clearcoat_alpha = clearcoat_roughness * clearcoat_roughness;

    float3 f0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);
    float3 diffuse_color = albedo * (1.0 - metallic);

    float2 dfg = pbr_dfg_lut.SampleLevel(linear_clamp_samp, float2(nov, perceptual_roughness), 0.0);
    float3 energy_comp = 1.0 + f0 * (rcp(max(dfg.x + dfg.y, 0.08)) - 1.0);

    float3 refl = reflect(-view_dir, normal);
    float3 env_diffuse = pbr_env_cubemap.SampleLevel(linear_clamp_samp, normal, 5.0).rgb;
    float3 env_specular = pbr_env_cubemap.SampleLevel(linear_clamp_samp, refl, perceptual_roughness * 4.0).rgb;
    float spec_ao = pbr_specular_ao(nov, ao, perceptual_roughness);
    float3 ibl_spec = env_specular * (f0 * dfg.x + dfg.y) * energy_comp * spec_ao;
    float3 ibl_diff = diffuse_color * env_diffuse * ao;
    float3 color = (ibl_diff + ibl_spec) * 0.72;

    if (clearcoat > 0.005) {
        float2 dfg_cc = pbr_dfg_lut.SampleLevel(linear_clamp_samp, float2(nov, clearcoat_roughness), 0.0);
        float fc_ibl = (0.04 + 0.96 * pow(saturate(1.0 - nov), 5.0)) * clearcoat;
        float3 env_cc = pbr_env_cubemap.SampleLevel(linear_clamp_samp, reflect(-view_dir, geo_normal), clearcoat_roughness * 4.0).rgb;
        color = color * (1.0 - fc_ibl) + env_cc * (0.04 * dfg_cc.x + dfg_cc.y) * clearcoat * spec_ao * 0.85;
    }

    float3 sun_dir = normalize(float3(-0.392232, 0.784465, -0.490290));
    float key_shadow = ((shader_flags & 32u) != 0u)
        ? trace_hard_shadow(world_position + geo_normal * 0.025, float3(2.1 * sin(scene_time * 1.9), 2.2 + 0.55 * cos(scene_time * 1.4), 10.5 + 4.2 * cos(scene_time * 1.1)), ball_center)
        : trace_soft_shadow(world_position + geo_normal * 0.025, sun_dir, 9.0, 7.0, ball_center);
    color += pbr_eval_direct_light(normal, view_dir, sun_dir, float3(3.1, 2.75, 2.25) * key_shadow, diffuse_color, f0, perceptual_roughness, alpha, energy_comp, clearcoat, clearcoat_alpha);

    float3 ceil_l1 = float3(0.0, 3.35, 9.5) - world_position;
    float d1_sq = max(dot(ceil_l1, ceil_l1), 0.45);
    color += pbr_eval_direct_light(normal, view_dir, ceil_l1 * rsqrt(d1_sq), float3(6.8, 7.6, 9.0) / (1.0 + 0.24 * d1_sq), diffuse_color, f0, perceptual_roughness, alpha, energy_comp, clearcoat, clearcoat_alpha);

    float3 ceil_l2 = float3(0.0, 3.35, 19.0) - world_position;
    float d2_sq = max(dot(ceil_l2, ceil_l2), 0.45);
    color += pbr_eval_direct_light(normal, view_dir, ceil_l2 * rsqrt(d2_sq), float3(7.5, 6.2, 4.8) / (1.0 + 0.22 * d2_sq), diffuse_color, f0, perceptual_roughness, alpha, energy_comp, clearcoat, clearcoat_alpha);

    if ((shader_flags & 8u) != 0u) {
        float3 pt_pos = float3(
            2.1 * sin(scene_time * 1.9),
            2.2 + 0.55 * cos(scene_time * 1.4),
            10.5 + 4.2 * cos(scene_time * 1.1));
        float3 to_pt = pt_pos - world_position;
        float dist_sq = max(dot(to_pt, to_pt), 0.30);
        float pt_shadow = ((shader_flags & 32u) != 0u) ? key_shadow : 1.0;
        color += pbr_eval_direct_light(normal, view_dir, to_pt * rsqrt(dist_sq), float3(16.0, 10.2, 4.8) * (pt_shadow / (1.0 + 0.25 * dist_sq)), diffuse_color, f0, perceptual_roughness, alpha, energy_comp, clearcoat, clearcoat_alpha);
    }

    if ((shader_flags & 16u) != 0u) {
        float pulse = 0.5 + 0.5 * sin(scene_time * 10.0 + world_position.z * 1.5);
        if (material > 3.5 && material < 4.5) {
            float band = step(0.72, frac(world_position.y * 2.0));
            color += float3(0.25, 2.4, 2.8) * band * (0.25 + 1.4 * pulse);
        } else if (material > 2.5 && material < 3.5) {
            float grid = max(step(0.92, frac(world_position.x * 3.0)), step(0.92, frac(world_position.y * 3.0)));
            color += float3(2.8, 0.45, 1.5) * grid * (0.20 + 1.3 * (1.0 - pulse));
        }
    }

    float3 fog_linear = float3(0.032, 0.048, 0.068);
    color = lerp(color, fog_linear, saturate(distance_travelled * (1.0 / 52.0)));
    if ((shader_flags & 128u) != 0u) {
        color = apply_transparent_glass(color, ray_origin, ray_dir, distance_travelled, scene_time);
    }

    return pbr_aces_filmic(color * 1.15);
}

float3 evaluate_shader_color_unified(
    float3 world_position,
    float3 ray_origin,
    float3 ray_dir,
    float3 ray_dx,
    float3 ray_dy,
    float distance_travelled,
    float material,
    float3 ball_center,
    float scene_time,
    uint packed_flags,
    SamplerState linear_clamp_samp) {
    if ((packed_flags & 256u) != 0u) {
        return evaluate_pbr_shader_color(
            world_position,
            ray_origin,
            ray_dir,
            ray_dx,
            ray_dy,
            distance_travelled,
            material,
            ball_center,
            scene_time,
            packed_flags,
            linear_clamp_samp);
    }
    return evaluate_legacy_shader_color(
        world_position,
        ray_origin,
        ray_dir,
        distance_travelled,
        material,
        ball_center,
        scene_time,
        packed_flags & 255u);
}
