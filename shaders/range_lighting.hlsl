struct GpuClusterLight {
    float4 pos_radius;
    float4 dir_cos_outer;
    float4 color_atten;
    float4 params;
};

struct OctreeVisProbeGpu {
    float4 pos_radius;
    float4 sky_bounce;
    float4 dir_vis_pos;
    float4 dir_vis_neg;
};

Texture2DArray<float4> dust2_albedo_array : register(t16);
Texture2DArray<float4> dust2_normal_array : register(t17);
Texture2DArray<float4> dust2_orm_array : register(t18);
StructuredBuffer<GpuClusterLight> cluster_lights_srv : register(t19);
StructuredBuffer<OctreeVisProbeGpu> octree_probes_srv : register(t20);
#ifdef ENABLE_DXR_INLINE
RaytracingAccelerationStructure scene_tlas_srv : register(t21);
#endif
Texture2DArray<float4> arsenal_albedo_array : register(t23);
Texture2DArray<float4> arsenal_normal_array : register(t24);
Texture2DArray<float4> arsenal_orm_array : register(t25);

float3 eval_octree_probe_irradiance(float3 world_pos, float3 n, uint probe_count, bool is_dust2) {
    if (probe_count == 0u) {
        return is_dust2 ? float3(0.45, 0.42, 0.36) : float3(0.18, 0.19, 0.21);
    }
    uint step_n = max(1u, probe_count / 32u);
    float3 acc_rad = float3(0.0, 0.0, 0.0);
    float acc_w = 0.0;
    [loop]
    for (uint i = 0u; i < probe_count; i += step_n) {
        OctreeVisProbeGpu pr = octree_probes_srv[i];
        float3 d = world_pos - pr.pos_radius.xyz;
        float dist2 = dot(d, d);
        float r = max(pr.pos_radius.w, 4.0);
        if (dist2 > r * r * 2.5) continue;
        float w = rcp(1.0 + dist2 * (4.0 / (r * r)));
        float vx = n.x >= 0.0 ? pr.dir_vis_pos.x : pr.dir_vis_neg.x;
        float vy = n.y >= 0.0 ? pr.dir_vis_pos.y : pr.dir_vis_neg.y;
        float vz = n.z >= 0.0 ? pr.dir_vis_pos.z : pr.dir_vis_neg.z;
        float dir_open = saturate(abs(n.x) * vx + abs(n.y) * vy + abs(n.z) * vz);
        float3 sky_tint = is_dust2 ? float3(0.75, 0.85, 1.05) : float3(0.32, 0.36, 0.42);
        float3 probe_rad = pr.sky_bounce.rgb * (0.48 + 0.52 * dir_open) + sky_tint * (pr.sky_bounce.w * saturate(n.y * 0.5 + 0.5) * 0.55);
        acc_rad += probe_rad * w;
        acc_w += w;
    }
    if (acc_w > 1e-4) {
        return acc_rad / acc_w;
    }
    return is_dust2 ? float3(0.48, 0.44, 0.38) : float3(0.20, 0.21, 0.24);
}

#ifdef ENABLE_DXR_INLINE
float trace_dxr_sun_visibility(float3 world_pos, float3 n, float3 sun_dir) {
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> rq;
    RayDesc ray;
    ray.Origin = world_pos + n * 0.06;
    ray.Direction = sun_dir;
    ray.TMin = 0.04;
    ray.TMax = 140.0;
    rq.TraceRayInline(scene_tlas_srv, RAY_FLAG_NONE, 0xFFu, ray);
    rq.Proceed();
    return (rq.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? 0.08 : 1.0;
}

float3 eval_dxr_rtgi_irradiance(
    float3 world_pos,
    float3 n,
    uint probe_count,
    bool is_dust2,
    uint frame_id,
    bool is_viewmodel) {
    if (is_viewmodel) {
        return eval_octree_probe_irradiance(world_pos, n, probe_count, is_dust2) * 0.65;
    }
    float3 up = abs(n.y) < 0.99 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    float3 t = normalize(cross(up, n));
    float3 b = cross(n, t);

    uint h = asuint(floor(world_pos.x * 29.0)) * 73856093u
        ^ asuint(floor(world_pos.y * 29.0)) * 19349663u
        ^ asuint(floor(world_pos.z * 29.0)) * 83492791u
        ^ ((frame_id & 15u) * 2654435761u);
    h ^= (h >> 16u);
    h *= 0x7feb352du;
    float base_phi = (float)(h & 1023u) * (6.2831853 / 1024.0);
    float jitter = (float)((h >> 10u) & 255u) * (1.0 / 256.0);

    float3 origin = world_pos + n * 0.055;
    float3 gi_acc = float3(0.0, 0.0, 0.0);
    float3 sky_col = is_dust2 ? float3(0.75, 0.92, 1.25) : float3(0.45, 0.48, 0.55);
    float3 sun_dir = normalize(float3(0.62, 0.56, 0.55));
    [loop]
    for (uint s = 0u; s < 8u; ++s) {
        float u1 = ((float)s + 0.15 + 0.70 * jitter) * (1.0 / 8.0);
        float phi = base_phi + (float)s * 2.39996323;
        float r = sqrt(saturate(u1));
        float z = sqrt(max(1.0 - saturate(u1), 1e-4));
        float3 dir = normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + n * z);

        RayQuery<RAY_FLAG_FORCE_OPAQUE> rq;
        RayDesc ray;
        ray.Origin = origin;
        ray.Direction = dir;
        ray.TMin = 0.05;
        ray.TMax = is_dust2 ? 42.0 : 28.0;
        rq.TraceRayInline(scene_tlas_srv, RAY_FLAG_NONE, 0xFFu, ray);
        rq.Proceed();

        if (rq.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
            float hit_t = rq.CommittedRayT();
            float3 hp = origin + dir * hit_t;
            float3 bounce_rad = float3(0.0, 0.0, 0.0);
            if (is_dust2) {
                RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> srq;
                RayDesc sray;
                sray.Origin = hp - dir * 0.08;
                sray.Direction = sun_dir;
                sray.TMin = 0.05;
                sray.TMax = 90.0;
                srq.TraceRayInline(scene_tlas_srv, RAY_FLAG_NONE, 0xFFu, sray);
                srq.Proceed();
                float hit_sun_vis = (srq.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? 0.08 : 1.0;
                float3 warm_sand = float3(0.72, 0.56, 0.36);
                float3 sun_bounce = warm_sand * float3(2.35, 1.85, 1.25) * hit_sun_vis;
                float3 ambient_bounce = warm_sand * float3(0.32, 0.34, 0.40);
                bounce_rad = sun_bounce + ambient_bounce;
            } else {
                float ceil_strip = step(4.35, hp.y) * step(abs(hp.x), 1.55);
                float red_side = step(5.85, abs(hp.x)) * exp(-abs(hp.z - 5.2) * 0.25);
                float amb_side = step(5.45, abs(hp.x)) * exp(-abs(hp.z - 28.0) * 0.20);
                bounce_rad = float3(0.16, 0.17, 0.20) + float3(2.8, 3.0, 3.3) * ceil_strip + float3(1.35, 0.16, 0.12) * red_side + float3(1.25, 0.65, 0.14) * amb_side;
            }
            float ao_term = smoothstep(0.15, 2.25, hit_t);
            gi_acc += bounce_rad * (0.28 + 0.72 * ao_term);
        } else {
            float horizon = saturate(dir.y * 0.9 + 0.15);
            gi_acc += lerp(float3(0.95, 0.80, 0.60), sky_col, horizon);
        }
    }
    return gi_acc * (1.15 / 8.0);
}
#endif

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

float3 eval_range_deform_local_pos(float3 bind_pos, uint mesh_kind, float scene_time, uint shader_flags) {
    if ((shader_flags & 256u) == 0u || mesh_kind != 1u) {
        return bind_pos;
    }
    float flex = saturate(bind_pos.y + 0.55);
    float3 p = bind_pos;
    p.x += 0.095 * sin(scene_time * 3.8 + bind_pos.y * 2.6) * flex;
    p.z += 0.055 * cos(scene_time * 2.9 + bind_pos.y * 3.1) * flex;
    return p;
}

bool eval_range_alpha_cutout_discard(float2 uv, uint material_id, uint shader_flags) {
    if ((shader_flags & 512u) == 0u || material_id != 6u) {
        return false;
    }
    float2 g = abs(frac(uv * 18.0 + float2(uv.y * 18.0, -uv.x * 18.0)) - 0.5);
    float wire = min(g.x, g.y);
    return wire > 0.16;
}

float3 eval_range_multibounce_gi(float3 world_pos, float3 n, float scene_time, uint shader_flags) {
    if ((shader_flags & 1024u) == 0u) {
        return float3(0.0, 0.0, 0.0);
    }
    float3 up = abs(n.y) < 0.99 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    float3 t = normalize(cross(up, n));
    float3 b = cross(n, t);
    float3 room_min = float3(-6.4, 0.0, -1.5);
    float3 room_max = float3( 6.4, 4.7, 31.5);
    float3 gi_acc = float3(0.0, 0.0, 0.0);
    uint seed = asuint(floor(world_pos.x * 37.0)) * 73856093u ^ asuint(floor(world_pos.z * 37.0)) * 83492791u;
    float base_phi = (float)(seed & 1023u) * (6.2831853 / 1024.0);
    [loop]
    for (uint s = 0u; s < 16u; ++s) {
        float u1 = ((float)s + 0.5) * (1.0 / 16.0);
        float phi = base_phi + (float)s * 2.39996323;
        float r = sqrt(u1);
        float z = sqrt(max(1.0 - u1, 1e-4));
        float3 dir = normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + n * z);
        float3 inv_d = rcp(select(abs(dir) > 1e-4, dir, float3(1e-4, 1e-4, 1e-4)));
        float3 t0 = (room_min - world_pos) * inv_d;
        float3 t1 = (room_max - world_pos) * inv_d;
        float3 tmax = max(t0, t1);
        float hit_t = min(tmax.x, min(tmax.y, tmax.z));
        hit_t = clamp(hit_t, 0.05, 36.0);
        float3 hp = world_pos + dir * hit_t;

        float3 wall_albedo = float3(0.32, 0.34, 0.36);
        float3 wall_rad = float3(0.0, 0.0, 0.0);
        if (hp.y > 4.45) {
            wall_albedo = float3(0.28, 0.30, 0.33);
            float strip = step(abs(hp.x), 1.45) * step(abs(frac((hp.z - 2.8) * (1.0 / 5.2)) - 0.5), 0.14);
            wall_rad = float3(3.4, 3.6, 3.9) * strip + float3(0.38, 0.40, 0.44);
        } else if (hp.y < 0.12) {
            wall_albedo = float3(0.26, 0.27, 0.29);
            float bay = exp(-abs(hp.z - 14.0) * 0.08);
            wall_rad = float3(0.55, 0.56, 0.58) * (0.45 + 0.55 * bay);
        } else if (abs(hp.x) > 6.15) {
            wall_albedo = float3(0.38, 0.22, 0.18);
            float red_glow = exp(-dot(hp - float3(sign(hp.x) * 5.95, 2.4, 5.2), hp - float3(sign(hp.x) * 5.95, 2.4, 5.2)) * 0.12);
            float amb_glow = exp(-dot(hp - float3(sign(hp.x) * 5.60, 2.5, 28.0), hp - float3(sign(hp.x) * 5.60, 2.5, 28.0)) * 0.09);
            wall_rad = float3(0.25, 0.26, 0.28) + float3(1.65, 0.18, 0.14) * red_glow + float3(1.55, 0.78, 0.18) * amb_glow;
        } else if (hp.z > 30.8) {
            wall_albedo = float3(0.22, 0.24, 0.27);
            wall_rad = float3(0.42, 0.34, 0.24);
        } else {
            float cyan_glow = exp(-dot(hp.xy - float2(0.0, 1.0), hp.xy - float2(0.0, 1.0)) * 0.18);
            wall_rad = float3(0.18, 0.22, 0.26) + float3(0.14, 0.85, 1.30) * cyan_glow;
        }

        float occ = 1.0;
        if (hit_t > 1.2 && hp.y < 2.2) {
            float d_lane = abs(frac((world_pos.x + dir.x * 1.1) * 0.45) - 0.5);
            occ = lerp(0.55, 1.0, smoothstep(0.04, 0.14, d_lane));
        }
        float atten = 1.0 / (1.0 + 0.035 * hit_t * hit_t);
        gi_acc += (wall_albedo * wall_rad * occ * (0.45 + 0.55 * atten));
    }
    return gi_acc * (0.95 / 16.0);
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

bool test_gpu_light_against_aabb(uint light_idx, uint active_light_count, float3 aabb_min, float3 aabb_max) {
    if (light_idx >= active_light_count) {
        return false;
    }
    GpuClusterLight lt = cluster_lights_srv[light_idx];
    uint ltype = (uint)(lt.params.x + 0.5);
    if (ltype == 3u) {
        return true;
    }
    float3 lp = lt.pos_radius.xyz;
    float lr = lt.pos_radius.w;
    float3 clamped = clamp(lp, aabb_min, aabb_max);
    float3 d = clamped - lp;
    if (dot(d, d) > lr * lr) {
        return false;
    }
    if (ltype == 1u) {
        float3 dir = lt.dir_cos_outer.xyz;
        float cos_o = lt.dir_cos_outer.w;
        float sin_o = sqrt(max(1.0 - cos_o * cos_o, 1e-4));
        float3 center = 0.5 * (aabb_min + aabb_max);
        float radius = length(aabb_max - center) + 0.05;
        float3 v = center - lp;
        float d_axis = dot(v, dir);
        if (d_axis < -radius) {
            return false;
        }
        float v_len_sq = dot(v, v);
        float d_perp = sqrt(max(v_len_sq - d_axis * d_axis, 0.0));
        if (d_axis * sin_o + radius < d_perp * cos_o) {
            return false;
        }
    }
    return true;
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

float3 eval_single_gpu_light_full(
    GpuClusterLight lt,
    float3 world_pos,
    float3 n,
    float3 view_dir,
    float3 diffuse_color,
    float3 f0,
    float perceptual_roughness,
    float alpha,
    float3 energy_comp,
    float clearcoat,
    float cc_alpha,
    bool dxr_shadow_ok,
    bool is_viewmodel) {
    uint ltype = (uint)(lt.params.x + 0.5);
    if (ltype == 2u) {
        return eval_rect_area_light(
            world_pos, n, view_dir,
            lt.pos_radius.xyz, lt.params.zw, lt.color_atten.rgb,
            diffuse_color, f0, perceptual_roughness, alpha, energy_comp, clearcoat, cc_alpha);
    }
    if (ltype == 3u) {
        float3 l = lt.dir_cos_outer.xyz;
        float vis = 1.0;
#ifdef ENABLE_DXR_INLINE
        if (dxr_shadow_ok && !is_viewmodel && dot(n, l) > 0.01) {
            vis = trace_dxr_sun_visibility(world_pos, n, l);
        }
#endif
        return pbr_eval_direct_light(n, view_dir, l, lt.color_atten.rgb * vis, diffuse_color, f0, perceptual_roughness, alpha, energy_comp, clearcoat, cc_alpha);
    }
    float3 to_l = lt.pos_radius.xyz - world_pos;
    float d2 = dot(to_l, to_l);
    if (d2 > lt.pos_radius.w * lt.pos_radius.w) {
        return float3(0.0, 0.0, 0.0);
    }
    if (ltype == 1u) {
        return eval_spot_light(
            world_pos, n, view_dir,
            lt.pos_radius.xyz, lt.dir_cos_outer.xyz, lt.params.y, lt.dir_cos_outer.w,
            lt.color_atten.rgb, lt.color_atten.w,
            diffuse_color, f0, perceptual_roughness, alpha, energy_comp, clearcoat, cc_alpha);
    }
    float safe_d2 = max(d2, 0.25);
    float3 rad = lt.color_atten.rgb / (1.0 + lt.color_atten.w * safe_d2);
    return pbr_eval_direct_light(n, view_dir, to_l * rsqrt(safe_d2), rad, diffuse_color, f0, perceptual_roughness, alpha, energy_comp, clearcoat, cc_alpha);
}

float3 eval_single_gpu_light_diffuse(
    GpuClusterLight lt,
    float3 world_pos,
    float3 n,
    bool dxr_shadow_ok,
    bool is_viewmodel) {
    uint ltype = (uint)(lt.params.x + 0.5);
    if (ltype == 2u) {
        float3 closest = float3(
            clamp(world_pos.x, lt.pos_radius.x - lt.params.z, lt.pos_radius.x + lt.params.z),
            lt.pos_radius.y,
            clamp(world_pos.z, lt.pos_radius.z - lt.params.w, lt.pos_radius.z + lt.params.w));
        float3 to_diff = closest - world_pos;
        float d2 = max(dot(to_diff, to_diff), 0.35);
        float3 l = to_diff * rsqrt(d2);
        float facing = saturate(-l.y * 1.35 + 0.15);
        float nol = saturate(dot(n, l));
        float area_term = (lt.params.z * lt.params.w * 4.0) / (d2 + 1.25);
        return lt.color_atten.rgb * (k_pbr_inv_pi * nol * facing * area_term);
    }
    if (ltype == 3u) {
        float3 l = lt.dir_cos_outer.xyz;
        float nol = saturate(dot(n, l));
        if (nol <= 1e-4) return float3(0.0, 0.0, 0.0);
        float vis = 1.0;
#ifdef ENABLE_DXR_INLINE
        if (dxr_shadow_ok && !is_viewmodel) {
            vis = trace_dxr_sun_visibility(world_pos, n, l);
        }
#endif
        return lt.color_atten.rgb * (k_pbr_inv_pi * nol * vis);
    }
    float3 to_l = lt.pos_radius.xyz - world_pos;
    float d2 = dot(to_l, to_l);
    if (d2 > lt.pos_radius.w * lt.pos_radius.w) {
        return float3(0.0, 0.0, 0.0);
    }
    if (ltype == 1u) {
        return eval_spot_diffuse_irr(world_pos, n, lt.pos_radius.xyz, lt.dir_cos_outer.xyz, lt.params.y, lt.dir_cos_outer.w, lt.color_atten.rgb, lt.color_atten.w);
    }
    float safe_d2 = max(d2, 0.25);
    float nol = saturate(dot(n, to_l * rsqrt(safe_d2)));
    return lt.color_atten.rgb * (k_pbr_inv_pi * nol / (1.0 + lt.color_atten.w * safe_d2));
}

float3 eval_single_gpu_light_specular(
    GpuClusterLight lt,
    float3 world_pos,
    float3 n,
    float3 view_dir,
    float nov,
    float3 refl,
    float3 f0,
    float alpha,
    float3 energy_comp,
    float clearcoat,
    float cc_alpha) {
    uint ltype = (uint)(lt.params.x + 0.5);
    if (ltype == 2u) {
        float3 closest_diff = float3(
            clamp(world_pos.x, lt.pos_radius.x - lt.params.z, lt.pos_radius.x + lt.params.z),
            lt.pos_radius.y,
            clamp(world_pos.z, lt.pos_radius.z - lt.params.w, lt.pos_radius.z + lt.params.w));
        float3 to_diff = closest_diff - world_pos;
        float d2_diff = max(dot(to_diff, to_diff), 0.35);
        float area_term = (lt.params.z * lt.params.w * 4.0) / (d2_diff + 1.25);
        float3 mrp = closest_diff;
        if (refl.y > 0.03 && lt.pos_radius.y > world_pos.y) {
            float t_plane = (lt.pos_radius.y - world_pos.y) / refl.y;
            float3 hit_p = world_pos + refl * min(t_plane, 36.0);
            mrp = float3(
                clamp(hit_p.x, lt.pos_radius.x - lt.params.z, lt.pos_radius.x + lt.params.z),
                lt.pos_radius.y,
                clamp(hit_p.z, lt.pos_radius.z - lt.params.w, lt.pos_radius.z + lt.params.w));
        }
        float3 to_spec = mrp - world_pos;
        float d2_spec = max(dot(to_spec, to_spec), 0.25);
        float dist_spec = sqrt(d2_spec);
        float3 l_spec = to_spec / dist_spec;
        float nol_spec = saturate(dot(n, l_spec));
        float facing_spec = saturate(-l_spec.y * 1.45 + 0.12);
        if (nol_spec <= 1e-4) return float3(0.0, 0.0, 0.0);
        float3 h = normalize(view_dir + l_spec);
        float noh = saturate(dot(n, h));
        float voh = saturate(dot(view_dir, h));
        float radius_eff = min(lt.params.z, lt.params.w) * 0.85;
        float alpha_prime = saturate(alpha + radius_eff / (2.0 * dist_spec));
        float norm_factor = (alpha * alpha) / max(alpha_prime * alpha_prime, 1e-4);
        float d_ggx = pbr_d_ggx(noh, alpha_prime) * norm_factor;
        float vis = pbr_v_smith_ggx_correlated(nov, max(nol_spec, 1e-4), alpha);
        float3 f = pbr_f_schlick(voh, f0, 1.0);
        float3 sp = (d_ggx * vis) * f * energy_comp * nol_spec * facing_spec * lt.color_atten.rgb * area_term;
        if (clearcoat > 0.005) {
            float cc_alpha_p = saturate(cc_alpha + radius_eff / (2.0 * dist_spec));
            float cc_norm = (cc_alpha * cc_alpha) / max(cc_alpha_p * cc_alpha_p, 1e-4);
            float d_cc = pbr_d_ggx(noh, cc_alpha_p) * cc_norm;
            float v_cc = pbr_v_kelemen(voh);
            float fc = (0.04 + 0.96 * pow(1.0 - voh, 5.0)) * clearcoat;
            sp = sp * (1.0 - fc) + lt.color_atten.rgb * area_term * (d_cc * v_cc * fc * nol_spec * facing_spec);
        }
        return sp;
    }
    if (ltype == 3u) {
        float3 l = lt.dir_cos_outer.xyz;
        float nol = saturate(dot(n, l));
        if (nol <= 1e-4) return float3(0.0, 0.0, 0.0);
        float3 h = normalize(view_dir + l);
        float d_ggx = pbr_d_ggx(saturate(dot(n, h)), alpha);
        float vis = pbr_v_smith_ggx_correlated(nov, nol, alpha);
        float3 f = pbr_f_schlick(saturate(dot(l, h)), f0, 1.0);
        return (d_ggx * vis) * f * energy_comp * lt.color_atten.rgb * nol;
    }
    float3 to_l = lt.pos_radius.xyz - world_pos;
    float d2 = dot(to_l, to_l);
    if (d2 > lt.pos_radius.w * lt.pos_radius.w) {
        return float3(0.0, 0.0, 0.0);
    }
    if (ltype == 1u) {
        return eval_spot_specular_only(
            world_pos, n, view_dir, nov,
            lt.pos_radius.xyz, lt.dir_cos_outer.xyz, lt.params.y, lt.dir_cos_outer.w,
            lt.color_atten.rgb, lt.color_atten.w, f0, alpha, energy_comp, clearcoat, cc_alpha);
    }
    float safe_d2 = max(d2, 0.25);
    float3 l = to_l * rsqrt(safe_d2);
    float nol = saturate(dot(n, l));
    if (nol <= 1e-4) return float3(0.0, 0.0, 0.0);
    float3 h = normalize(view_dir + l);
    float d_ggx = pbr_d_ggx(saturate(dot(n, h)), alpha);
    float vis = pbr_v_smith_ggx_correlated(nov, nol, alpha);
    float3 f = pbr_f_schlick(saturate(dot(l, h)), f0, 1.0);
    float3 rad = lt.color_atten.rgb * (nol / (1.0 + lt.color_atten.w * safe_d2));
    float3 sp = (d_ggx * vis) * f * energy_comp * rad;
    if (clearcoat > 0.005) {
        float d_cc = pbr_d_ggx(saturate(dot(n, h)), cc_alpha);
        float v_cc = pbr_v_kelemen(saturate(dot(l, h)));
        float fc = (0.04 + 0.96 * pow(saturate(1.0 - saturate(dot(l, h))), 5.0)) * clearcoat;
        sp = sp * (1.0 - fc) + (d_cc * v_cc * fc) * rad;
    }
    return sp;
}

float2 eval_dust2_pom_uv(
    float2 uv,
    float d2_slice,
    float3 world_pos,
    float3 cam_pos,
    float3 geo_t,
    float3 geo_b,
    float3 geo_n) {
    float3 to_cam = cam_pos - world_pos;
    float dist = length(to_cam);
    if (dist > 24.0 || dist < 0.15) {
        return uv;
    }
    float fade = 1.0 - smoothstep(10.0, 24.0, dist);
    float3 v_dir = to_cam / dist;
    float vz = dot(v_dir, geo_n);
    if (vz <= 0.18) {
        return uv;
    }
    float alpha0 = dust2_albedo_array.SampleLevel(pbr_aniso_sampler, float3(uv, d2_slice), 0.0).a;
    if (alpha0 < 0.95) {
        return uv;
    }
    float2 v_ts = float2(dot(v_dir, geo_t), dot(v_dir, geo_b));
    float2 p_step = (v_ts / max(vz, 0.35)) * (0.018 * fade * 0.25);
    float layer_d = 0.25;
    float cur_layer = 0.0;
    float2 cur_uv = uv;
    float cur_h = 1.0 - dust2_normal_array.SampleLevel(pbr_aniso_sampler, float3(cur_uv, d2_slice), 0.0).a;
    [unroll]
    for (int i = 0; i < 4; ++i) {
        if (cur_layer < cur_h) {
            cur_uv -= p_step;
            cur_h = 1.0 - dust2_normal_array.SampleLevel(pbr_aniso_sampler, float3(cur_uv, d2_slice), 0.0).a;
            cur_layer += layer_d;
        }
    }
    float2 prev_uv = cur_uv + p_step;
    float after_d = cur_h - cur_layer;
    float before_d = (1.0 - dust2_normal_array.SampleLevel(pbr_aniso_sampler, float3(prev_uv, d2_slice), 0.0).a) - (cur_layer - layer_d);
    float weight = saturate(after_d / max(after_d - before_d, 1e-4));
    return lerp(cur_uv, prev_uv, weight);
}

void sample_range_pbr_material(
    float3 world_pos,
    float3 cam_pos,
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

    float3 ts_n;
    float4 orm;
    if ((material_id & 0x40000000u) != 0u) {
        float a_slice = (float)(material_id & 0xFFu);
        if (aniso_enabled) {
            out_albedo = arsenal_albedo_array.SampleGrad(pbr_aniso_sampler, float3(uv, a_slice), duv_dx, duv_dy).rgb;
            ts_n = arsenal_normal_array.SampleGrad(pbr_aniso_sampler, float3(uv, a_slice), duv_dx, duv_dy).xyz * 2.0 - 1.0;
            orm = arsenal_orm_array.SampleGrad(pbr_aniso_sampler, float3(uv, a_slice), duv_dx, duv_dy);
        } else {
            out_albedo = arsenal_albedo_array.SampleLevel(pbr_aniso_sampler, float3(uv, a_slice), 0.0).rgb;
            ts_n = arsenal_normal_array.SampleLevel(pbr_aniso_sampler, float3(uv, a_slice), 0.0).xyz * 2.0 - 1.0;
            orm = arsenal_orm_array.SampleLevel(pbr_aniso_sampler, float3(uv, a_slice), 0.0);
        }
    } else if ((material_id & 0x80000000u) != 0u) {
        float3 srgb = float3(
            (float)(material_id & 255u),
            (float)((material_id >> 8u) & 255u),
            (float)((material_id >> 16u) & 255u)) * (1.0 / 255.0);
        out_albedo = pow(saturate(srgb), 2.2);
        float rough_n = (float)((material_id >> 24u) & 15u) * (1.0 / 15.0);
        float metal_n = (float)((material_id >> 28u) & 7u) * (1.0 / 7.0);
        float micro = sin(world_pos.x * 95.0) * cos(world_pos.y * 95.0) * sin(world_pos.z * 95.0);
        ts_n = normalize(float3(micro * 0.035, -micro * 0.035, 1.0));
        orm = float4(1.0, clamp(0.10 + rough_n * 0.85, 0.10, 0.96), metal_n, metal_n > 0.45 ? 0.18 : 0.04);
    } else if (material_id >= 16u) {
        float d2_slice = clamp((float)(material_id - 16u), 0.0, 99.0);
        float2 d2_uv = aniso_enabled ? eval_dust2_pom_uv(uv, d2_slice, world_pos, cam_pos, geo_t, geo_b, out_geo_n) : uv;
        if (aniso_enabled) {
            out_albedo = dust2_albedo_array.SampleGrad(pbr_aniso_sampler, float3(d2_uv, d2_slice), duv_dx, duv_dy).rgb;
            ts_n = dust2_normal_array.SampleGrad(pbr_aniso_sampler, float3(d2_uv, d2_slice), duv_dx, duv_dy).xyz * 2.0 - 1.0;
            orm = dust2_orm_array.SampleGrad(pbr_aniso_sampler, float3(d2_uv, d2_slice), duv_dx, duv_dy);
        } else {
            out_albedo = dust2_albedo_array.SampleLevel(pbr_aniso_sampler, float3(d2_uv, d2_slice), 0.0).rgb;
            ts_n = dust2_normal_array.SampleLevel(pbr_aniso_sampler, float3(d2_uv, d2_slice), 0.0).xyz * 2.0 - 1.0;
            orm = dust2_orm_array.SampleLevel(pbr_aniso_sampler, float3(d2_uv, d2_slice), 0.0);
        }
    } else if (material_id == 12u) {
        if (aniso_enabled) {
            out_albedo = hero_albedo_tex.SampleGrad(pbr_aniso_sampler, uv, duv_dx, duv_dy).rgb;
            ts_n = hero_normal_tex.SampleGrad(pbr_aniso_sampler, uv, duv_dx, duv_dy).xyz * 2.0 - 1.0;
            orm = hero_orm_tex.SampleGrad(pbr_aniso_sampler, uv, duv_dx, duv_dy);
        } else {
            out_albedo = hero_albedo_tex.SampleLevel(pbr_aniso_sampler, uv, 0.0).rgb;
            ts_n = hero_normal_tex.SampleLevel(pbr_aniso_sampler, uv, 0.0).xyz * 2.0 - 1.0;
            orm = hero_orm_tex.SampleLevel(pbr_aniso_sampler, uv, 0.0);
        }
    } else {
        float slice = clamp((float)material_id, 0.0, 11.0);
        if (aniso_enabled) {
            out_albedo = pbr_albedo_array.SampleGrad(pbr_aniso_sampler, float3(uv, slice), duv_dx, duv_dy).rgb;
            ts_n = pbr_normal_array.SampleGrad(pbr_aniso_sampler, float3(uv, slice), duv_dx, duv_dy).xyz * 2.0 - 1.0;
            orm = pbr_orm_array.SampleGrad(pbr_aniso_sampler, float3(uv, slice), duv_dx, duv_dy);
        } else {
            out_albedo = pbr_albedo_array.SampleLevel(pbr_aniso_sampler, float3(uv, slice), 0.0).rgb;
            ts_n = pbr_normal_array.SampleLevel(pbr_aniso_sampler, float3(uv, slice), 0.0).xyz * 2.0 - 1.0;
            orm = pbr_orm_array.SampleLevel(pbr_aniso_sampler, float3(uv, slice), 0.0);
        }
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
    uint4 world_lighting_params,
    Texture2D<float4> hero_albedo_tex,
    Texture2D<float4> hero_normal_tex,
    Texture2D<float4> hero_orm_tex,
    SamplerState clamp_sampler) {
    float3 albedo, n, out_geo_n;
    float ao, perceptual_roughness, metallic, clearcoat;
    sample_range_pbr_material(
        world_pos, cam_pos,
        geo_n, geo_t, uv, duv_dx, duv_dy, dndu, dndv,
        material_id, tint_emissive, is_viewmodel, aniso_enabled, scene_time, shader_flags,
        hero_albedo_tex, hero_normal_tex, hero_orm_tex,
        albedo, n, out_geo_n, ao, perceptual_roughness, metallic, clearcoat);

    uint active_light_count = min(max(world_lighting_params.x, 1u), 64u);
    uint active_probe_count = world_lighting_params.y;
    bool is_dust2 = (world_lighting_params.z & 1u) != 0u;
    bool dxr_ready = (world_lighting_params.z & 2u) != 0u;
    uint frame_id = world_lighting_params.w;

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
    float3 env_diff = float3(0.0, 0.0, 0.0);
    if ((shader_flags & 1024u) != 0u) {
#ifdef ENABLE_DXR_INLINE
        if (dxr_ready) {
            env_diff = eval_dxr_rtgi_irradiance(world_pos, n, active_probe_count, is_dust2, frame_id, is_viewmodel);
        } else
#endif
        {
            env_diff = (is_dust2 ? eval_octree_probe_irradiance(world_pos, n, active_probe_count, true) : pbr_env_cubemap.SampleLevel(clamp_sampler, n, 5.0).rgb * 0.42)
                + eval_range_multibounce_gi(world_pos, n, scene_time, shader_flags);
        }
    } else {
        env_diff = is_dust2
            ? eval_octree_probe_irradiance(world_pos, n, active_probe_count, true) * 0.72
            : pbr_env_cubemap.SampleLevel(clamp_sampler, n, 5.0).rgb * 0.42;
    }
    float3 env_spec = pbr_env_cubemap.SampleLevel(clamp_sampler, refl, perceptual_roughness * 4.0).rgb;
    float spec_ao = pbr_specular_ao(nov, ao, perceptual_roughness);
    float3 color = diffuse_color * env_diff * ao + env_spec * (f0 * dfg.x + dfg.y) * energy_comp * spec_ao * (is_dust2 ? 0.48 : 0.42);

    if (clearcoat > 0.005) {
        float2 dfg_cc = pbr_dfg_lut.SampleLevel(clamp_sampler, float2(nov, cc_roughness), 0.0);
        float fc_ibl = (0.04 + 0.96 * pow(saturate(1.0 - nov), 5.0)) * clearcoat;
        float3 env_cc = pbr_env_cubemap.SampleLevel(clamp_sampler, reflect(-view_dir, out_geo_n), cc_roughness * 4.0).rgb;
        color = color * (1.0 - fc_ibl) + env_cc * (0.04 * dfg_cc.x + dfg_cc.y) * clearcoat * spec_ao * 0.55;
    }

    bool dxr_sun_ok = dxr_ready && is_dust2;
    [loop]
    for (uint li = 0u; li < active_light_count; ++li) {
        color += eval_single_gpu_light_full(
            cluster_lights_srv[li], world_pos, n, view_dir,
            diffuse_color, f0, perceptual_roughness, alpha, energy_comp, clearcoat, cc_alpha,
            dxr_sun_ok, is_viewmodel);
    }

    if ((shader_flags & (8u | 32u)) != 0u && !is_viewmodel && !is_dust2) {
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
        float3 fog_col = is_dust2 ? float3(0.74, 0.78, 0.84) : float3(0.08, 0.09, 0.11);
        float fog_scale = is_dust2 ? (1.0 / 220.0) : (1.0 / 95.0);
        color = lerp(color, fog_col, saturate(view_z * fog_scale));
    }

    return pbr_aces_filmic(color * 1.10);
}

float3 eval_range_diffuse_irradiance(
    float3 world_pos,
    float3 n,
    float ao,
    bool is_viewmodel,
    float scene_time,
    uint shader_flags,
    uint4 world_lighting_params,
    SamplerState clamp_sampler) {
    uint active_light_count = min(max(world_lighting_params.x, 1u), 64u);
    uint active_probe_count = world_lighting_params.y;
    bool is_dust2 = (world_lighting_params.z & 1u) != 0u;
    bool dxr_ready = (world_lighting_params.z & 2u) != 0u;
    uint frame_id = world_lighting_params.w;

    float3 irr = float3(0.0, 0.0, 0.0);
    if ((shader_flags & 1024u) != 0u) {
#ifdef ENABLE_DXR_INLINE
        if (dxr_ready) {
            irr = eval_dxr_rtgi_irradiance(world_pos, n, active_probe_count, is_dust2, frame_id, is_viewmodel) * ao;
        } else
#endif
        {
            irr = (is_dust2 ? eval_octree_probe_irradiance(world_pos, n, active_probe_count, true) : pbr_env_cubemap.SampleLevel(clamp_sampler, n, 5.0).rgb * 0.42) * ao
                + eval_range_multibounce_gi(world_pos, n, scene_time, shader_flags) * ao;
        }
    } else {
        irr = is_dust2
            ? eval_octree_probe_irradiance(world_pos, n, active_probe_count, true) * (0.72 * ao)
            : pbr_env_cubemap.SampleLevel(clamp_sampler, n, 5.0).rgb * (0.42 * ao);
    }

    bool dxr_sun_ok = dxr_ready && is_dust2;
    [loop]
    for (uint li = 0u; li < active_light_count; ++li) {
        irr += eval_single_gpu_light_diffuse(cluster_lights_srv[li], world_pos, n, dxr_sun_ok, is_viewmodel);
    }

    if ((shader_flags & (8u | 32u)) != 0u && !is_viewmodel && !is_dust2) {
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

    return irr;
}

float3 eval_range_direct_specular_only(
    float3 world_pos,
    float3 n,
    float3 view_dir,
    float nov,
    float3 refl,
    float3 f0,
    float perceptual_roughness,
    float alpha,
    float3 energy_comp,
    float metallic,
    float clearcoat,
    float cc_alpha,
    bool is_viewmodel,
    float scene_time,
    uint shader_flags,
    uint2 tile_light_mask,
    uint4 world_lighting_params,
    bool roughness_cull_enabled,
    float4 muzzle_light_pos_intensity) {
    float3 direct_spec = float3(0.0, 0.0, 0.0);
    bool is_dust2 = (world_lighting_params.z & 1u) != 0u;
    bool skip_minor_spec = roughness_cull_enabled && !is_viewmodel && (perceptual_roughness >= 0.94) && (metallic < 0.01) && (clearcoat < 0.01);

    uint m_lo = tile_light_mask.x;
    uint m_hi = skip_minor_spec ? 0u : tile_light_mask.y;
    if (skip_minor_spec) {
        m_lo &= is_dust2 ? 1u : 31u;
    }

    [loop]
    while (m_lo != 0u) {
        uint bit = firstbitlow(m_lo);
        m_lo &= (m_lo - 1u);
        direct_spec += eval_single_gpu_light_specular(
            cluster_lights_srv[bit], world_pos, n, view_dir, nov, refl, f0, alpha, energy_comp, clearcoat, cc_alpha);
    }
    [loop]
    while (m_hi != 0u) {
        uint bit = firstbitlow(m_hi);
        m_hi &= (m_hi - 1u);
        direct_spec += eval_single_gpu_light_specular(
            cluster_lights_srv[32u + bit], world_pos, n, view_dir, nov, refl, f0, alpha, energy_comp, clearcoat, cc_alpha);
    }

    if ((shader_flags & (8u | 32u)) != 0u && !is_viewmodel && !is_dust2) {
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
            direct_spec += (d_ggx * vis) * f * energy_comp * float3(34.0, 26.0, 12.0) * (nol * dyn_vis / (1.0 + 0.14 * dyn_d2));
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
            direct_spec += (d_ggx * vis) * f * energy_comp * float3(20.0, 12.0, 4.2) * (nol * muzzle_light_pos_intensity.w / (1.0 + 1.3 * md2));
        }
    }
    return direct_spec;
}

float3 eval_range_composite_final(
    float3 world_pos,
    float3 cached_diffuse_irr,
    float3 direct_spec,
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
    float3 view_dir,
    float nov,
    float3 refl,
    float3 f0,
    float2 dfg,
    float3 energy_comp,
    float4 muzzle_light_pos_intensity,
    bool is_dust2,
    SamplerState clamp_sampler) {
    float cc_roughness = 0.12;
    float3 diffuse_color = albedo * (1.0 - metallic);
    float3 total_diff_irr = cached_diffuse_irr;
    if (muzzle_light_pos_intensity.w > 0.01) {
        float3 to_m = muzzle_light_pos_intensity.xyz - world_pos;
        float md2 = max(dot(to_m, to_m), 0.08);
        float nol_m = saturate(dot(n, to_m * rsqrt(md2)));
        total_diff_irr += float3(20.0, 12.0, 4.2) * (k_pbr_inv_pi * nol_m * muzzle_light_pos_intensity.w / (1.0 + 1.3 * md2));
    }
    float3 env_spec = pbr_env_cubemap.SampleLevel(clamp_sampler, refl, perceptual_roughness * 4.0).rgb;
    float spec_ao = pbr_specular_ao(nov, ao, perceptual_roughness);
    float3 color = diffuse_color * total_diff_irr + env_spec * (f0 * dfg.x + dfg.y) * energy_comp * spec_ao * (is_dust2 ? 0.48 : 0.42);

    if (clearcoat > 0.005) {
        float2 dfg_cc = pbr_dfg_lut.SampleLevel(clamp_sampler, float2(nov, cc_roughness), 0.0);
        float fc_ibl = (0.04 + 0.96 * pow(saturate(1.0 - nov), 5.0)) * clearcoat;
        float3 env_cc = pbr_env_cubemap.SampleLevel(clamp_sampler, reflect(-view_dir, geo_n), cc_roughness * 4.0).rgb;
        color = color * (1.0 - fc_ibl) + env_cc * (0.04 * dfg_cc.x + dfg_cc.y) * clearcoat * spec_ao * 0.55;
    }
    color += direct_spec;

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
        float3 fog_col = is_dust2 ? float3(0.74, 0.78, 0.84) : float3(0.08, 0.09, 0.11);
        float fog_scale = is_dust2 ? (1.0 / 220.0) : (1.0 / 95.0);
        color = lerp(color, fog_col, saturate(view_z * fog_scale));
    }

    return pbr_aces_filmic(color * 1.10);
}
