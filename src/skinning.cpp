#include "skinning.h"
#include "range_mesh.h"
#include "gltf_loader.h"

#include <cgltf.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>

namespace frame_warp_lab {

namespace {

inline Mat4x4 mat4_identity() {
    Mat4x4 m{};
    m.m[0] = 1.0f;
    m.m[5] = 1.0f;
    m.m[10] = 1.0f;
    m.m[15] = 1.0f;
    return m;
}

inline Mat4x4 mat4_mul(const Mat4x4& a, const Mat4x4& b) {
    Mat4x4 r{};
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            r.m[row * 4 + col] =
                a.m[row * 4 + 0] * b.m[0 * 4 + col] +
                a.m[row * 4 + 1] * b.m[1 * 4 + col] +
                a.m[row * 4 + 2] * b.m[2 * 4 + col] +
                a.m[row * 4 + 3] * b.m[3 * 4 + col];
        }
    }
    return r;
}

inline Mat4x4 mat4_from_col_major(const float c[16]) {
    Mat4x4 r{};
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            r.m[row * 4 + col] = c[col * 4 + row];
        }
    }
    return r;
}

inline Mat4x4 mat4_trs(Float3 t, Quat4 q, Float3 s) {
    const float xx = q.x * q.x;
    const float yy = q.y * q.y;
    const float zz = q.z * q.z;
    const float xy = q.x * q.y;
    const float xz = q.x * q.z;
    const float yz = q.y * q.z;
    const float wx = q.w * q.x;
    const float wy = q.w * q.y;
    const float wz = q.w * q.z;

    Mat4x4 r{};
    r.m[0] = (1.0f - 2.0f * (yy + zz)) * s.x;
    r.m[1] = (2.0f * (xy - wz)) * s.y;
    r.m[2] = (2.0f * (xz + wy)) * s.z;
    r.m[3] = t.x;

    r.m[4] = (2.0f * (xy + wz)) * s.x;
    r.m[5] = (1.0f - 2.0f * (xx + zz)) * s.y;
    r.m[6] = (2.0f * (yz - wx)) * s.z;
    r.m[7] = t.y;

    r.m[8] = (2.0f * (xz - wy)) * s.x;
    r.m[9] = (2.0f * (yz + wx)) * s.y;
    r.m[10] = (1.0f - 2.0f * (xx + yy)) * s.z;
    r.m[11] = t.z;

    r.m[12] = 0.0f;
    r.m[13] = 0.0f;
    r.m[14] = 0.0f;
    r.m[15] = 1.0f;
    return r;
}

inline Mat4x4 mat4_rot_y(float rad) {
    const float c = std::cos(rad);
    const float s = std::sin(rad);
    Mat4x4 m = mat4_identity();
    m.m[0] = c;
    m.m[2] = s;
    m.m[8] = -s;
    m.m[10] = c;
    return m;
}

inline Quat4 quat_norm(Quat4 q) {
    const float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (len < 1e-6f) return {0.0f, 0.0f, 0.0f, 1.0f};
    const float inv = 1.0f / len;
    return {q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}

inline Quat4 quat_mul(Quat4 a, Quat4 b) {
    return quat_norm({
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    });
}

inline Quat4 quat_from_axis_x(float rad) {
    const float h = rad * 0.5f;
    return {std::sin(h), 0.0f, 0.0f, std::cos(h)};
}

inline Quat4 quat_from_axis_y(float rad) {
    const float h = rad * 0.5f;
    return {0.0f, std::sin(h), 0.0f, std::cos(h)};
}

inline Quat4 quat_from_axis_z(float rad) {
    const float h = rad * 0.5f;
    return {0.0f, 0.0f, std::sin(h), std::cos(h)};
}

inline Mat4x4 mat4_rot_x(float rad) {
    const float c = std::cos(rad);
    const float s = std::sin(rad);
    Mat4x4 r = mat4_identity();
    r.m[5] = c;
    r.m[6] = -s;
    r.m[9] = s;
    r.m[10] = c;
    return r;
}

void compute_skinned_tangents(std::vector<SkinnedSourceVertex>& verts, const std::vector<std::uint32_t>& indices) {
    std::vector<Float3> tan_acc(verts.size(), {0.0f, 0.0f, 0.0f});
    for (std::size_t i = 0; i + 2U < indices.size(); i += 3U) {
        const std::uint32_t i0 = indices[i + 0U];
        const std::uint32_t i1 = indices[i + 1U];
        const std::uint32_t i2 = indices[i + 2U];
        if (i0 >= verts.size() || i1 >= verts.size() || i2 >= verts.size()) continue;
        const auto& v0 = verts[i0];
        const auto& v1 = verts[i1];
        const auto& v2 = verts[i2];
        const float dx1 = v1.pos.x - v0.pos.x;
        const float dy1 = v1.pos.y - v0.pos.y;
        const float dz1 = v1.pos.z - v0.pos.z;
        const float dx2 = v2.pos.x - v0.pos.x;
        const float dy2 = v2.pos.y - v0.pos.y;
        const float dz2 = v2.pos.z - v0.pos.z;
        const float du1 = v1.u - v0.u;
        const float dv1 = v1.v - v0.v;
        const float du2 = v2.u - v0.u;
        const float dv2 = v2.v - v0.v;
        const float det = du1 * dv2 - du2 * dv1;
        if (std::fabs(det) < 1e-8f) continue;
        const float inv = 1.0f / det;
        const Float3 t{
            (dx1 * dv2 - dx2 * dv1) * inv,
            (dy1 * dv2 - dy2 * dv1) * inv,
            (dz1 * dv2 - dz2 * dv1) * inv};
        tan_acc[i0] = {tan_acc[i0].x + t.x, tan_acc[i0].y + t.y, tan_acc[i0].z + t.z};
        tan_acc[i1] = {tan_acc[i1].x + t.x, tan_acc[i1].y + t.y, tan_acc[i1].z + t.z};
        tan_acc[i2] = {tan_acc[i2].x + t.x, tan_acc[i2].y + t.y, tan_acc[i2].z + t.z};
    }
    for (std::size_t i = 0; i < verts.size(); ++i) {
        const Float3 n = verts[i].normal;
        const Float3 t0 = tan_acc[i];
        const float d = t0.x * n.x + t0.y * n.y + t0.z * n.z;
        const Float3 ortho{t0.x - n.x * d, t0.y - n.y * d, t0.z - n.z * d};
        const float len = std::sqrt(ortho.x * ortho.x + ortho.y * ortho.y + ortho.z * ortho.z);
        if (len > 1e-5f) {
            verts[i].tangent = {ortho.x / len, ortho.y / len, ortho.z / len};
        }
    }
}

inline Quat4 quat_conj(Quat4 q) {
    return {-q.x, -q.y, -q.z, q.w};
}

inline Float3 quat_rotate(Quat4 q, Float3 v) {
    const Quat4 qv{v.x, v.y, v.z, 0.0f};
    const Quat4 r = quat_mul(quat_mul(q, qv), quat_conj(q));
    return {r.x, r.y, r.z};
}

void compute_rest_world_rotations(SkinnedCharacterTemplate& tpl) {
    const std::size_t n = tpl.nodes.size();
    tpl.rest_world_rot.resize(n);
    std::vector<std::uint8_t> done(n, 0U);
    const Quat4 root_q = quat_mul(quat_from_axis_y(tpl.yaw_correction), quat_from_axis_x(tpl.pitch_correction));
    auto eval_q = [&](auto&& self, std::size_t i) -> Quat4 {
        if (done[i] != 0U) return tpl.rest_world_rot[i];
        const std::int32_t p = tpl.nodes[i].parent;
        if (p >= 0 && static_cast<std::size_t>(p) < n) {
            tpl.rest_world_rot[i] = quat_mul(self(self, static_cast<std::size_t>(p)), tpl.nodes[i].rest_r);
        } else {
            tpl.rest_world_rot[i] = quat_mul(root_q, tpl.nodes[i].rest_r);
        }
        done[i] = 1U;
        return tpl.rest_world_rot[i];
    };
    for (std::size_t i = 0; i < n; ++i) {
        eval_q(eval_q, i);
    }
}

void apply_valve_biped_procedural_pose(
    const SkinnedCharacterTemplate& tpl,
    std::int32_t clip_index,
    float clip_time,
    std::vector<Float3>& out_t,
    std::vector<Quat4>& out_r) {
    if (tpl.rest_world_rot.size() != tpl.nodes.size()) {
        return;
    }
    const auto& bp = tpl.biped;
    auto rot_world = [&](std::int32_t idx, Quat4 q_world) {
        if (idx < 0 || static_cast<std::size_t>(idx) >= out_r.size()) return;
        const auto u = static_cast<std::size_t>(idx);
        const Quat4 w = tpl.rest_world_rot[u];
        const Quat4 local_delta = quat_mul(quat_mul(quat_conj(w), q_world), w);
        out_r[u] = quat_mul(out_r[u], local_delta);
    };

    const float t = clip_time;
    const float breath = std::sin(t * 2.4f) * 0.025f;
    float stride_freq = 0.0f;
    float stride_amp = 0.0f;
    float crouch = 0.0f;
    float shoot_kick = 0.0f;
    float flinch = 0.0f;
    float death_u = 0.0f;

    if (clip_index == 1) {
        stride_freq = 6.5f;
        stride_amp = 0.36f;
    } else if (clip_index == 2) {
        stride_freq = 9.5f;
        stride_amp = 0.54f;
    } else if (clip_index == 3) {
        stride_freq = 6.2f;
        stride_amp = 0.30f;
        shoot_kick = std::fabs(std::sin(t * 16.0f)) * 0.08f;
    } else if (clip_index == 4) {
        shoot_kick = std::fabs(std::sin(t * 16.0f)) * 0.09f;
    } else if (clip_index == 5) {
        crouch = 1.0f;
    } else if (clip_index == 6) {
        flinch = std::sin(std::clamp(t / 0.30f, 0.0f, 1.0f) * 3.14159265f);
    } else if (clip_index == 7) {
        death_u = std::clamp(t / 0.65f, 0.0f, 1.0f);
        death_u = death_u * death_u * (3.0f - 2.0f * death_u);
    }

    const float ph = t * stride_freq;
    const float leg_l = std::sin(ph) * stride_amp;
    const float leg_r = -leg_l;
    const float knee_l = std::max(0.0f, -std::cos(ph)) * stride_amp * 0.90f + crouch * 0.68f;
    const float knee_r = std::max(0.0f, std::cos(ph)) * stride_amp * 0.90f + crouch * 0.68f;

    const float arm_sway = std::sin(ph) * stride_amp * 0.14f;
    rot_world(bp.arm_upper_l, quat_mul(quat_from_axis_x(-0.58f + arm_sway - shoot_kick * 0.6f), quat_mul(quat_from_axis_y(0.32f), quat_from_axis_z(-1.05f))));
    rot_world(bp.arm_lower_l, quat_mul(quat_from_axis_x(-0.78f), quat_from_axis_y(0.55f)));
    rot_world(bp.arm_upper_r, quat_mul(quat_from_axis_x(-0.52f - arm_sway - shoot_kick * 0.9f), quat_mul(quat_from_axis_y(-0.26f), quat_from_axis_z(1.05f))));
    rot_world(bp.arm_lower_r, quat_mul(quat_from_axis_x(-0.82f - shoot_kick), quat_from_axis_y(-0.48f)));

    rot_world(bp.leg_upper_l, quat_from_axis_x(-leg_l - crouch * 0.55f));
    rot_world(bp.leg_lower_l, quat_from_axis_x(knee_l));
    rot_world(bp.leg_upper_r, quat_from_axis_x(-leg_r - crouch * 0.55f));
    rot_world(bp.leg_lower_r, quat_from_axis_x(knee_r));

    rot_world(bp.spine0, quat_from_axis_x(breath + crouch * 0.22f - flinch * 0.26f));
    rot_world(bp.spine1, quat_from_axis_x(breath * 0.8f - shoot_kick * 0.45f));
    rot_world(bp.head, quat_from_axis_x(-breath * 0.5f + flinch * 0.20f));

    if (bp.pelvis >= 0 && static_cast<std::size_t>(bp.pelvis) < out_t.size()) {
        const auto pi = static_cast<std::size_t>(bp.pelvis);
        const float bob_m = std::fabs(std::sin(ph)) * stride_amp * 0.045f;
        const float drop_m = crouch * 0.34f + death_u * 0.78f - bob_m;
        const float inv_s = tpl.root_scale > 1e-4f ? (1.0f / tpl.root_scale) : 1.0f;
        const std::int32_t parent_idx = tpl.nodes[pi].parent;
        const Quat4 parent_w = (parent_idx >= 0 && static_cast<std::size_t>(parent_idx) < tpl.rest_world_rot.size())
            ? tpl.rest_world_rot[static_cast<std::size_t>(parent_idx)]
            : quat_mul(quat_from_axis_y(tpl.yaw_correction), quat_from_axis_x(tpl.pitch_correction));
        const Float3 local_shift = quat_rotate(quat_conj(parent_w), {0.0f, -drop_m * inv_s, 0.0f});
        out_t[pi].x += local_shift.x;
        out_t[pi].y += local_shift.y;
        out_t[pi].z += local_shift.z;
        if (death_u > 0.001f) {
            rot_world(bp.pelvis, quat_from_axis_x(-death_u * 1.48f));
        }
    }
}

inline Quat4 quat_slerp(Quat4 a, Quat4 b, float t) {
    float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (dot < 0.0f) {
        dot = -dot;
        b = {-b.x, -b.y, -b.z, -b.w};
    }
    if (dot > 0.9995f) {
        return quat_norm({
            a.x + (b.x - a.x) * t,
            a.y + (b.y - a.y) * t,
            a.z + (b.z - a.z) * t,
            a.w + (b.w - a.w) * t});
    }
    const float theta0 = std::acos(std::clamp(dot, -1.0f, 1.0f));
    const float sin_theta0 = std::sin(theta0);
    const float theta = theta0 * t;
    const float s0 = std::cos(theta) - dot * std::sin(theta) / sin_theta0;
    const float s1 = std::sin(theta) / sin_theta0;
    return quat_norm({
        a.x * s0 + b.x * s1,
        a.y * s0 + b.y * s1,
        a.z * s0 + b.z * s1,
        a.w * s0 + b.w * s1});
}

inline Float3 mat4_point(const Mat4x4& m, Float3 p) {
    return {
        m.m[0] * p.x + m.m[1] * p.y + m.m[2] * p.z + m.m[3],
        m.m[4] * p.x + m.m[5] * p.y + m.m[6] * p.z + m.m[7],
        m.m[8] * p.x + m.m[9] * p.y + m.m[10] * p.z + m.m[11]};
}

inline Float3 mat4_dir(const Mat4x4& m, Float3 d) {
    return {
        m.m[0] * d.x + m.m[1] * d.y + m.m[2] * d.z,
        m.m[4] * d.x + m.m[5] * d.y + m.m[6] * d.z,
        m.m[8] * d.x + m.m[9] * d.y + m.m[10] * d.z};
}

inline Float3 vec3_norm(Float3 v, Float3 fb = {0.0f, 1.0f, 0.0f}) {
    const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len < 1e-6f) return fb;
    const float inv = 1.0f / len;
    return {v.x * inv, v.y * inv, v.z * inv};
}

std::uint32_t classify_character_material(const cgltf_material* mat, bool is_ct_fbi) {
    float r = 0.35f;
    float g = 0.35f;
    float b = 0.35f;
    float rough = 0.65f;
    float metal = 0.05f;
    std::string mname;
    if (mat != nullptr) {
        if (mat->name != nullptr) mname = mat->name;
        if (mat->has_pbr_metallic_roughness) {
            r = mat->pbr_metallic_roughness.base_color_factor[0];
            g = mat->pbr_metallic_roughness.base_color_factor[1];
            b = mat->pbr_metallic_roughness.base_color_factor[2];
            rough = mat->pbr_metallic_roughness.roughness_factor;
            metal = mat->pbr_metallic_roughness.metallic_factor;
        }
    }
    if (is_ct_fbi) {
        if (mname.find("Character_Main") != std::string::npos) {
            r = 0.045f; g = 0.085f; b = 0.20f; rough = 0.72f; metal = 0.06f;
        } else if (mname.find("Pants") != std::string::npos) {
            r = 0.032f; g = 0.048f; b = 0.095f; rough = 0.78f; metal = 0.04f;
        } else if (mname.find("DarkGrey") != std::string::npos) {
            r = 0.045f; g = 0.055f; b = 0.072f; rough = 0.50f; metal = 0.25f;
        } else if (mname.find("Grey") != std::string::npos) {
            r = 0.14f; g = 0.17f; b = 0.22f; rough = 0.42f; metal = 0.55f;
        } else if (mname.find("Black") != std::string::npos) {
            r = 0.025f; g = 0.028f; b = 0.035f; rough = 0.36f; metal = 0.35f;
        } else if (mname.find("Skin") != std::string::npos) {
            r = 0.52f; g = 0.33f; b = 0.20f; rough = 0.62f; metal = 0.0f;
        }
    } else {
        if (mname.find("Enemy_Red") != std::string::npos) {
            r = 0.46f; g = 0.11f; b = 0.06f; rough = 0.78f; metal = 0.02f;
        } else if (mname.find("Grey2") != std::string::npos) {
            r = 0.36f; g = 0.28f; b = 0.17f; rough = 0.80f; metal = 0.02f;
        } else if (mname.find("Grey") != std::string::npos) {
            r = 0.24f; g = 0.21f; b = 0.16f; rough = 0.72f; metal = 0.15f;
        } else if (mname.find("Wood") != std::string::npos) {
            r = 0.34f; g = 0.16f; b = 0.055f; rough = 0.55f; metal = 0.02f;
        } else if (mname.find("DarkGrey") != std::string::npos) {
            r = 0.07f; g = 0.065f; b = 0.06f; rough = 0.45f; metal = 0.55f;
        } else if (mname.find("Black") != std::string::npos) {
            r = 0.035f; g = 0.032f; b = 0.030f; rough = 0.65f; metal = 0.15f;
        } else if (mname.find("Skin") != std::string::npos) {
            r = 0.48f; g = 0.29f; b = 0.16f; rough = 0.62f; metal = 0.0f;
        }
    }
    return pack_inline_pbr_material(r, g, b, rough, metal);
}

void sample_clip_trs(
    const AnimClip& clip,
    float time_sec,
    bool loop,
    std::vector<Float3>& out_t,
    std::vector<Quat4>& out_r,
    std::vector<Float3>& out_s) {
    float t = time_sec;
    if (clip.duration > 1e-4f) {
        if (loop) {
            t = std::fmod(t, clip.duration);
            if (t < 0.0f) t += clip.duration;
        } else {
            t = std::clamp(t, 0.0f, clip.duration);
        }
    } else {
        t = 0.0f;
    }

    for (const auto& ch : clip.channels) {
        if (ch.node_index < 0 || static_cast<std::size_t>(ch.node_index) >= out_t.size() || ch.times.empty()) {
            continue;
        }
        const std::size_t n = ch.times.size();
        std::size_t idx0 = 0;
        std::size_t idx1 = 0;
        float alpha = 0.0f;
        if (n > 1U) {
            if (t <= ch.times.front()) {
                idx0 = idx1 = 0;
            } else if (t >= ch.times.back()) {
                idx0 = idx1 = n - 1U;
            } else {
                auto it = std::upper_bound(ch.times.begin(), ch.times.end(), t);
                idx1 = static_cast<std::size_t>(it - ch.times.begin());
                idx0 = idx1 > 0 ? idx1 - 1U : 0U;
                const float span = ch.times[idx1] - ch.times[idx0];
                alpha = span > 1e-5f ? std::clamp((t - ch.times[idx0]) / span, 0.0f, 1.0f) : 0.0f;
            }
        }
        const auto ni = static_cast<std::size_t>(ch.node_index);
        if (ch.path == AnimPathKind::translation && ch.values.size() >= (idx1 + 1U) * 3U) {
            const float* a = &ch.values[idx0 * 3U];
            const float* b = &ch.values[idx1 * 3U];
            out_t[ni] = {
                a[0] + (b[0] - a[0]) * alpha,
                a[1] + (b[1] - a[1]) * alpha,
                a[2] + (b[2] - a[2]) * alpha};
        } else if (ch.path == AnimPathKind::scale && ch.values.size() >= (idx1 + 1U) * 3U) {
            const float* a = &ch.values[idx0 * 3U];
            const float* b = &ch.values[idx1 * 3U];
            out_s[ni] = {
                a[0] + (b[0] - a[0]) * alpha,
                a[1] + (b[1] - a[1]) * alpha,
                a[2] + (b[2] - a[2]) * alpha};
        } else if (ch.path == AnimPathKind::rotation && ch.values.size() >= (idx1 + 1U) * 4U) {
            const float* a = &ch.values[idx0 * 4U];
            const float* b = &ch.values[idx1 * 4U];
            out_r[ni] = quat_slerp(
                quat_norm({a[0], a[1], a[2], a[3]}),
                quat_norm({b[0], b[1], b[2], b[3]}),
                alpha);
        }
    }
}

void compute_world_matrices(
    const std::vector<RigNode>& nodes,
    const std::vector<Float3>& local_t,
    const std::vector<Quat4>& local_r,
    const std::vector<Float3>& local_s,
    std::vector<Mat4x4>& out_world) {
    const std::size_t n = nodes.size();
    out_world.resize(n);
    std::vector<Mat4x4> local_m(n);
    std::vector<std::uint8_t> done(n, 0U);
    for (std::size_t i = 0; i < n; ++i) {
        local_m[i] = mat4_trs(local_t[i], local_r[i], local_s[i]);
    }
    auto eval_node = [&](auto&& self, std::size_t i) -> const Mat4x4& {
        if (done[i] != 0U) return out_world[i];
        const std::int32_t p = nodes[i].parent;
        if (p >= 0 && static_cast<std::size_t>(p) < n) {
            out_world[i] = mat4_mul(self(self, static_cast<std::size_t>(p)), local_m[i]);
        } else {
            out_world[i] = local_m[i];
        }
        done[i] = 1U;
        return out_world[i];
    };
    for (std::size_t i = 0; i < n; ++i) {
        eval_node(eval_node, i);
    }
}

}

std::int32_t SkinnedCharacterTemplate::find_clip(const std::string& clip_name) const {
    for (std::size_t i = 0; i < clips.size(); ++i) {
        if (clips[i].name == clip_name) {
            return static_cast<std::int32_t>(i);
        }
    }
    return clips.empty() ? -1 : 0;
}

SkinnedCharacterTemplate load_skinned_character_glb(
    const std::filesystem::path& relative_glb_path,
    bool is_ct_fbi,
    std::vector<ArsenalPbrMaterialDesc>& out_materials) {
    SkinnedCharacterTemplate tpl{};
    tpl.is_ct_fbi = is_ct_fbi;
    const std::filesystem::path full_path = resolve_asset_path(relative_glb_path);
    std::error_code ec;
    if (!std::filesystem::exists(full_path, ec)) {
        return tpl;
    }

    cgltf_options opts{};
    cgltf_data* data = nullptr;
    const std::string utf8 = full_path.string();
    if (cgltf_parse_file(&opts, utf8.c_str(), &data) != cgltf_result_success || data == nullptr) {
        return tpl;
    }
    if (cgltf_load_buffers(&opts, data, utf8.c_str()) != cgltf_result_success) {
        cgltf_free(data);
        return tpl;
    }

    auto read_img = [](const cgltf_texture_view& tv) -> std::vector<std::uint8_t> {
        std::vector<std::uint8_t> bytes;
        if (tv.texture == nullptr || tv.texture->image == nullptr) return bytes;
        const cgltf_image* img = tv.texture->image;
        if (img->buffer_view != nullptr && img->buffer_view->buffer != nullptr && img->buffer_view->buffer->data != nullptr) {
            const auto* src = static_cast<const std::uint8_t*>(img->buffer_view->buffer->data) + img->buffer_view->offset;
            bytes.assign(src, src + img->buffer_view->size);
        }
        return bytes;
    };

    std::unordered_map<const cgltf_material*, std::uint32_t> char_mat_map;
    auto resolve_char_material = [&](const cgltf_material* mat) -> std::uint32_t {
        if (mat != nullptr) {
            auto it = char_mat_map.find(mat);
            if (it != char_mat_map.end()) return it->second;
            ArsenalPbrMaterialDesc desc{};
            desc.character_fabric_orm = true;
            if (mat->has_pbr_metallic_roughness) {
                const auto& mr = mat->pbr_metallic_roughness;
                desc.fallback_r = mr.base_color_factor[0];
                desc.fallback_g = mr.base_color_factor[1];
                desc.fallback_b = mr.base_color_factor[2];
                desc.roughness_factor = mr.roughness_factor;
                desc.metallic_factor = mr.metallic_factor;
                desc.albedo_encoded = read_img(mr.base_color_texture);
                desc.orm_encoded = read_img(mr.metallic_roughness_texture);
            }
            desc.normal_encoded = read_img(mat->normal_texture);
            if (!desc.albedo_encoded.empty() || !desc.normal_encoded.empty()) {
                const auto slice_idx = static_cast<std::uint32_t>(out_materials.size());
                if (slice_idx < 64U) {
                    out_materials.push_back(std::move(desc));
                    const std::uint32_t mat_id = k_arsenal_pbr_material_flag | slice_idx;
                    char_mat_map[mat] = mat_id;
                    return mat_id;
                }
            }
        }
        return classify_character_material(mat, is_ct_fbi);
    };

    tpl.source_path = utf8;
    std::unordered_map<const cgltf_node*, std::int32_t> node_to_idx;
    tpl.nodes.resize(data->nodes_count);
    for (cgltf_size i = 0; i < data->nodes_count; ++i) {
        node_to_idx[&data->nodes[i]] = static_cast<std::int32_t>(i);
    }
    for (cgltf_size i = 0; i < data->nodes_count; ++i) {
        const cgltf_node& cn = data->nodes[i];
        RigNode& rn = tpl.nodes[i];
        rn.name = cn.name != nullptr ? cn.name : "";
        if (cn.parent != nullptr) {
            auto it = node_to_idx.find(cn.parent);
            rn.parent = (it != node_to_idx.end()) ? it->second : -1;
        }
        if (cn.has_translation) rn.rest_t = {cn.translation[0], cn.translation[1], cn.translation[2]};
        if (cn.has_rotation) rn.rest_r = quat_norm({cn.rotation[0], cn.rotation[1], cn.rotation[2], cn.rotation[3]});
        if (cn.has_scale) rn.rest_s = {cn.scale[0], cn.scale[1], cn.scale[2]};
        const auto ii = static_cast<std::int32_t>(i);
        if ((rn.name == "Head" || rn.name.rfind("head_0_", 0) == 0) && tpl.head_node_index < 0) tpl.head_node_index = ii;
        if ((rn.name == "Torso" || rn.name == "Chest" || rn.name == "UpperBody" || rn.name == "Spine2" || rn.name.rfind("spine_2_", 0) == 0) && tpl.torso_node_index < 0) {
            tpl.torso_node_index = ii;
        }
        if (rn.name.rfind("weapon_hand_R_", 0) == 0 && tpl.weapon_node_index < 0) tpl.weapon_node_index = ii;
        if (rn.name.rfind("pelvis_", 0) == 0 && tpl.biped.pelvis < 0) tpl.biped.pelvis = ii;
        if (rn.name.rfind("spine_0_", 0) == 0 && tpl.biped.spine0 < 0) tpl.biped.spine0 = ii;
        if (rn.name.rfind("spine_1_", 0) == 0 && tpl.biped.spine1 < 0) tpl.biped.spine1 = ii;
        if (rn.name.rfind("spine_2_", 0) == 0 && tpl.biped.spine2 < 0) tpl.biped.spine2 = ii;
        if (rn.name.rfind("neck_0_", 0) == 0 && tpl.biped.neck < 0) tpl.biped.neck = ii;
        if (rn.name.rfind("head_0_", 0) == 0 && tpl.biped.head < 0) tpl.biped.head = ii;
        if (rn.name.rfind("arm_upper_L_", 0) == 0 && rn.name.find("TWIST") == std::string::npos && tpl.biped.arm_upper_l < 0) tpl.biped.arm_upper_l = ii;
        if (rn.name.rfind("arm_lower_L_", 0) == 0 && rn.name.find("TWIST") == std::string::npos && tpl.biped.arm_lower_l < 0) tpl.biped.arm_lower_l = ii;
        if (rn.name.rfind("arm_upper_R_", 0) == 0 && rn.name.find("TWIST") == std::string::npos && tpl.biped.arm_upper_r < 0) tpl.biped.arm_upper_r = ii;
        if (rn.name.rfind("arm_lower_R_", 0) == 0 && rn.name.find("TWIST") == std::string::npos && tpl.biped.arm_lower_r < 0) tpl.biped.arm_lower_r = ii;
        if (rn.name.rfind("leg_upper_L_", 0) == 0 && rn.name.find("TWIST") == std::string::npos && tpl.biped.leg_upper_l < 0) tpl.biped.leg_upper_l = ii;
        if (rn.name.rfind("leg_lower_L_", 0) == 0 && rn.name.find("TWIST") == std::string::npos && tpl.biped.leg_lower_l < 0) tpl.biped.leg_lower_l = ii;
        if (rn.name.rfind("leg_upper_R_", 0) == 0 && rn.name.find("TWIST") == std::string::npos && tpl.biped.leg_upper_r < 0) tpl.biped.leg_upper_r = ii;
        if (rn.name.rfind("leg_lower_R_", 0) == 0 && rn.name.find("TWIST") == std::string::npos && tpl.biped.leg_lower_r < 0) tpl.biped.leg_lower_r = ii;
    }

    if (data->skins_count > 0U) {
        const cgltf_skin& sk = data->skins[0];
        tpl.joint_nodes.resize(sk.joints_count, -1);
        tpl.inverse_bind_matrices.resize(sk.joints_count, mat4_identity());
        for (cgltf_size ji = 0; ji < sk.joints_count; ++ji) {
            auto it = node_to_idx.find(sk.joints[ji]);
            tpl.joint_nodes[ji] = (it != node_to_idx.end()) ? it->second : -1;
            if (sk.inverse_bind_matrices != nullptr && ji < sk.inverse_bind_matrices->count) {
                float col_m[16]{};
                cgltf_accessor_read_float(sk.inverse_bind_matrices, ji, col_m, 16);
                tpl.inverse_bind_matrices[ji] = mat4_from_col_major(col_m);
            }
        }
    }

    tpl.clips.reserve(data->animations_count);
    for (cgltf_size ai = 0; ai < data->animations_count; ++ai) {
        const cgltf_animation& ca = data->animations[ai];
        AnimClip clip{};
        clip.name = ca.name != nullptr ? ca.name : ("Clip_" + std::to_string(ai));
        clip.duration = 0.0f;
        for (cgltf_size ci = 0; ci < ca.channels_count; ++ci) {
            const cgltf_animation_channel& cc = ca.channels[ci];
            if (cc.target_node == nullptr || cc.sampler == nullptr || cc.sampler->input == nullptr || cc.sampler->output == nullptr) {
                continue;
            }
            auto it = node_to_idx.find(cc.target_node);
            if (it == node_to_idx.end()) continue;
            AnimChannel ch{};
            ch.node_index = it->second;
            int comps = 0;
            if (cc.target_path == cgltf_animation_path_type_translation) {
                ch.path = AnimPathKind::translation;
                comps = 3;
            } else if (cc.target_path == cgltf_animation_path_type_rotation) {
                ch.path = AnimPathKind::rotation;
                comps = 4;
            } else if (cc.target_path == cgltf_animation_path_type_scale) {
                ch.path = AnimPathKind::scale;
                comps = 3;
            } else {
                continue;
            }
            const cgltf_size kcount = cc.sampler->input->count;
            ch.times.resize(kcount);
            ch.values.resize(kcount * static_cast<cgltf_size>(comps));
            for (cgltf_size ki = 0; ki < kcount; ++ki) {
                float tv = 0.0f;
                cgltf_accessor_read_float(cc.sampler->input, ki, &tv, 1);
                ch.times[ki] = tv;
                if (tv > clip.duration) clip.duration = tv;
                cgltf_accessor_read_float(cc.sampler->output, ki, &ch.values[ki * static_cast<cgltf_size>(comps)], static_cast<cgltf_size>(comps));
            }
            clip.channels.push_back(std::move(ch));
        }
        tpl.clips.push_back(std::move(clip));
    }

    const std::string keep_weapon = is_ct_fbi ? "SMG" : "AK";
    for (cgltf_size ni = 0; ni < data->nodes_count; ++ni) {
        const cgltf_node& cn = data->nodes[ni];
        if (cn.mesh == nullptr) continue;
        const std::string nname = cn.name != nullptr ? cn.name : "";
        if ((nname == "AK" || nname == "Pistol" || nname == "SMG" || nname == "Sniper") && nname != keep_weapon) {
            continue;
        }
        if (nname == keep_weapon) {
            tpl.weapon_node_index = static_cast<std::int32_t>(ni);
        }
        const bool is_skinned = (cn.skin != nullptr);
        const std::int32_t rigid_node = is_skinned ? -1 : static_cast<std::int32_t>(ni);

        for (cgltf_size pi = 0; pi < cn.mesh->primitives_count; ++pi) {
            const cgltf_primitive& prim = cn.mesh->primitives[pi];
            if (prim.type != cgltf_primitive_type_triangles) continue;
            const cgltf_accessor* pos_acc = nullptr;
            const cgltf_accessor* norm_acc = nullptr;
            const cgltf_accessor* uv_acc = nullptr;
            const cgltf_accessor* j_acc = nullptr;
            const cgltf_accessor* w_acc = nullptr;
            for (cgltf_size a = 0; a < prim.attributes_count; ++a) {
                const cgltf_attribute& attr = prim.attributes[a];
                if (attr.type == cgltf_attribute_type_position) pos_acc = attr.data;
                else if (attr.type == cgltf_attribute_type_normal) norm_acc = attr.data;
                else if (attr.type == cgltf_attribute_type_texcoord && attr.index == 0) uv_acc = attr.data;
                else if (attr.type == cgltf_attribute_type_joints && attr.index == 0) j_acc = attr.data;
                else if (attr.type == cgltf_attribute_type_weights && attr.index == 0) w_acc = attr.data;
            }
            if (pos_acc == nullptr || pos_acc->count == 0U) continue;
            const std::uint32_t mat_id = resolve_char_material(prim.material);
            const auto base_v = static_cast<std::uint32_t>(tpl.source_vertices.size());
            tpl.source_vertices.resize(base_v + static_cast<std::uint32_t>(pos_acc->count));
            for (cgltf_size vi = 0; vi < pos_acc->count; ++vi) {
                float p[3] = {0.0f, 0.0f, 0.0f};
                float n[3] = {0.0f, 1.0f, 0.0f};
                float uv[2] = {0.0f, 0.0f};
                cgltf_uint jv[4] = {0, 0, 0, 0};
                float wv[4] = {1.0f, 0.0f, 0.0f, 0.0f};
                cgltf_accessor_read_float(pos_acc, vi, p, 3);
                if (norm_acc != nullptr) cgltf_accessor_read_float(norm_acc, vi, n, 3);
                if (uv_acc != nullptr) cgltf_accessor_read_float(uv_acc, vi, uv, 2);
                if (is_skinned && j_acc != nullptr && w_acc != nullptr) {
                    cgltf_accessor_read_uint(j_acc, vi, jv, 4);
                    cgltf_accessor_read_float(w_acc, vi, wv, 4);
                    const float wsum = wv[0] + wv[1] + wv[2] + wv[3];
                    if (wsum > 1e-5f) {
                        const float inv = 1.0f / wsum;
                        wv[0] *= inv; wv[1] *= inv; wv[2] *= inv; wv[3] *= inv;
                    } else {
                        wv[0] = 1.0f; wv[1] = wv[2] = wv[3] = 0.0f;
                    }
                }
                const Float3 nn = vec3_norm({n[0], n[1], n[2]});
                const Float3 up = std::fabs(nn.y) < 0.95f ? Float3{0.0f, 1.0f, 0.0f} : Float3{1.0f, 0.0f, 0.0f};
                const Float3 tt = vec3_norm({
                    up.y * nn.z - up.z * nn.y,
                    up.z * nn.x - up.x * nn.z,
                    up.x * nn.y - up.y * nn.x}, {1.0f, 0.0f, 0.0f});
                SkinnedSourceVertex sv{};
                sv.pos = {p[0], p[1], p[2]};
                sv.normal = nn;
                sv.tangent = tt;
                sv.u = uv[0];
                sv.v = uv[1];
                sv.material_id = mat_id;
                sv.emissive = 0.0f;
                sv.joints[0] = static_cast<std::uint16_t>(jv[0]);
                sv.joints[1] = static_cast<std::uint16_t>(jv[1]);
                sv.joints[2] = static_cast<std::uint16_t>(jv[2]);
                sv.joints[3] = static_cast<std::uint16_t>(jv[3]);
                sv.weights[0] = wv[0];
                sv.weights[1] = wv[1];
                sv.weights[2] = wv[2];
                sv.weights[3] = wv[3];
                sv.rigid_node_index = rigid_node;
                tpl.source_vertices[base_v + static_cast<std::uint32_t>(vi)] = sv;
            }
            if (prim.indices != nullptr) {
                for (cgltf_size ii = 0; ii + 2U < prim.indices->count; ii += 3U) {
                    const auto i0 = base_v + static_cast<std::uint32_t>(cgltf_accessor_read_index(prim.indices, ii + 0U));
                    const auto i1 = base_v + static_cast<std::uint32_t>(cgltf_accessor_read_index(prim.indices, ii + 1U));
                    const auto i2 = base_v + static_cast<std::uint32_t>(cgltf_accessor_read_index(prim.indices, ii + 2U));
                    if (i0 != i1 && i1 != i2 && i2 != i0) {
                        tpl.indices.push_back(i0);
                        tpl.indices.push_back(i1);
                        tpl.indices.push_back(i2);
                    }
                }
            } else {
                for (cgltf_size ii = 0; ii + 2U < pos_acc->count; ii += 3U) {
                    tpl.indices.push_back(base_v + static_cast<std::uint32_t>(ii + 0U));
                    tpl.indices.push_back(base_v + static_cast<std::uint32_t>(ii + 1U));
                    tpl.indices.push_back(base_v + static_cast<std::uint32_t>(ii + 2U));
                }
            }
        }
    }
    cgltf_free(data);

    if (tpl.source_vertices.empty() || tpl.indices.empty()) {
        return tpl;
    }

    compute_skinned_tangents(tpl.source_vertices, tpl.indices);

    if (tpl.clips.empty() && tpl.biped.pelvis >= 0) {
        tpl.procedural_valve_biped = true;
        const std::array<const char*, 8> pnames = {{
            "Idle", "Walk", "Run_Gun", "Walk_Shoot", "Idle_Shoot", "Duck", "HitReact", "Death"}};
        for (const char* pn : pnames) {
            AnimClip c{};
            c.name = pn;
            c.duration = 1.0f;
            tpl.clips.push_back(std::move(c));
        }
    }

    tpl.root_scale = 1.0f;
    tpl.y_offset = 0.0f;
    tpl.pitch_correction = 0.0f;
    tpl.yaw_correction = 0.0f;
    std::vector<RangeVertex> probe(tpl.source_vertices.size());
    SkinnedPoseParams idle_pose{};
    idle_pose.clip_index = tpl.find_clip("Idle");
    Float3 head_l{}, chest_l{}, muz_l{};
    evaluate_skinned_character(tpl, idle_pose, probe.data(), head_l, chest_l, muz_l);
    float min_y = std::numeric_limits<float>::max();
    float max_y = -std::numeric_limits<float>::max();
    float min_z = std::numeric_limits<float>::max();
    float max_z = -std::numeric_limits<float>::max();
    for (const auto& rv : probe) {
        if (rv.py < min_y) min_y = rv.py;
        if (rv.py > max_y) max_y = rv.py;
        if (rv.pz < min_z) min_z = rv.pz;
        if (rv.pz > max_z) max_z = rv.pz;
    }
    if ((max_z - min_z) > (max_y - min_y) * 1.4f) {
        tpl.pitch_correction = (min_z < -max_z) ? 1.57079633f : -1.57079633f;
        evaluate_skinned_character(tpl, idle_pose, probe.data(), head_l, chest_l, muz_l);
        min_y = std::numeric_limits<float>::max();
        max_y = -std::numeric_limits<float>::max();
        for (const auto& rv : probe) {
            if (rv.py < min_y) min_y = rv.py;
            if (rv.py > max_y) max_y = rv.py;
        }
    }
    const float raw_h = std::max(max_y - min_y, 0.2f);
    tpl.root_scale = 1.78f / raw_h;
    tpl.y_offset = -min_y * tpl.root_scale;
    compute_rest_world_rotations(tpl);
    tpl.loaded = true;
    return tpl;
}

void evaluate_skinned_character(
    const SkinnedCharacterTemplate& tpl,
    const SkinnedPoseParams& pose,
    RangeVertex* out_vertices,
    Float3& out_head_local,
    Float3& out_chest_local,
    Float3& out_muzzle_local) {
    const std::size_t node_count = tpl.nodes.size();
    std::vector<Float3> t_cur(node_count);
    std::vector<Quat4> r_cur(node_count);
    std::vector<Float3> s_cur(node_count);
    for (std::size_t i = 0; i < node_count; ++i) {
        t_cur[i] = tpl.nodes[i].rest_t;
        r_cur[i] = tpl.nodes[i].rest_r;
        s_cur[i] = tpl.nodes[i].rest_s;
    }
    if (tpl.procedural_valve_biped) {
        apply_valve_biped_procedural_pose(tpl, pose.clip_index, pose.clip_time, t_cur, r_cur);
    } else if (pose.clip_index >= 0 && static_cast<std::size_t>(pose.clip_index) < tpl.clips.size()) {
        sample_clip_trs(tpl.clips[static_cast<std::size_t>(pose.clip_index)], pose.clip_time, pose.loop_clip, t_cur, r_cur, s_cur);
    }
    if (!tpl.procedural_valve_biped
        && pose.prev_clip_index >= 0
        && static_cast<std::size_t>(pose.prev_clip_index) < tpl.clips.size()
        && pose.blend_alpha < 0.995f) {
        std::vector<Float3> t_prev(node_count);
        std::vector<Quat4> r_prev(node_count);
        std::vector<Float3> s_prev(node_count);
        for (std::size_t i = 0; i < node_count; ++i) {
            t_prev[i] = tpl.nodes[i].rest_t;
            r_prev[i] = tpl.nodes[i].rest_r;
            s_prev[i] = tpl.nodes[i].rest_s;
        }
        sample_clip_trs(tpl.clips[static_cast<std::size_t>(pose.prev_clip_index)], pose.prev_clip_time, true, t_prev, r_prev, s_prev);
        const float a = std::clamp(pose.blend_alpha, 0.0f, 1.0f);
        for (std::size_t i = 0; i < node_count; ++i) {
            t_cur[i] = {
                t_prev[i].x + (t_cur[i].x - t_prev[i].x) * a,
                t_prev[i].y + (t_cur[i].y - t_prev[i].y) * a,
                t_prev[i].z + (t_cur[i].z - t_prev[i].z) * a};
            r_cur[i] = quat_slerp(r_prev[i], r_cur[i], a);
            s_cur[i] = {
                s_prev[i].x + (s_cur[i].x - s_prev[i].x) * a,
                s_prev[i].y + (s_cur[i].y - s_prev[i].y) * a,
                s_prev[i].z + (s_cur[i].z - s_prev[i].z) * a};
        }
    }

    if (tpl.torso_node_index >= 0 && std::fabs(pose.aim_pitch_rad) > 1e-4f) {
        const auto ti = static_cast<std::size_t>(tpl.torso_node_index);
        const Quat4 qp = quat_from_axis_x(std::clamp(pose.aim_pitch_rad, -0.55f, 0.55f));
        if (tpl.procedural_valve_biped && ti < tpl.rest_world_rot.size()) {
            const Quat4 w = tpl.rest_world_rot[ti];
            r_cur[ti] = quat_mul(r_cur[ti], quat_mul(quat_mul(quat_conj(w), qp), w));
        } else {
            r_cur[ti] = quat_mul(r_cur[ti], qp);
        }
    }

    std::vector<Mat4x4> node_world;
    compute_world_matrices(tpl.nodes, t_cur, r_cur, s_cur, node_world);

    Mat4x4 root_xform = mat4_mul(mat4_rot_y(tpl.yaw_correction), mat4_rot_x(tpl.pitch_correction));
    root_xform.m[0] *= tpl.root_scale;
    root_xform.m[1] *= tpl.root_scale;
    root_xform.m[2] *= tpl.root_scale;
    root_xform.m[4] *= tpl.root_scale;
    root_xform.m[5] *= tpl.root_scale;
    root_xform.m[6] *= tpl.root_scale;
    root_xform.m[8] *= tpl.root_scale;
    root_xform.m[9] *= tpl.root_scale;
    root_xform.m[10] *= tpl.root_scale;
    root_xform.m[7] += tpl.y_offset;

    for (auto& nw : node_world) {
        nw = mat4_mul(root_xform, nw);
    }

    const std::size_t jcount = tpl.joint_nodes.size();
    std::vector<Mat4x4> skin_mats(jcount, mat4_identity());
    for (std::size_t ji = 0; ji < jcount; ++ji) {
        const std::int32_t ni = tpl.joint_nodes[ji];
        if (ni >= 0 && static_cast<std::size_t>(ni) < node_world.size()) {
            skin_mats[ji] = mat4_mul(node_world[static_cast<std::size_t>(ni)], tpl.inverse_bind_matrices[ji]);
        }
    }

    const std::size_t vcount = tpl.source_vertices.size();
    for (std::size_t vi = 0; vi < vcount; ++vi) {
        const SkinnedSourceVertex& sv = tpl.source_vertices[vi];
        Float3 p{0.0f, 0.0f, 0.0f};
        Float3 n{0.0f, 0.0f, 0.0f};
        Float3 t{0.0f, 0.0f, 0.0f};
        if (sv.rigid_node_index >= 0 && static_cast<std::size_t>(sv.rigid_node_index) < node_world.size()) {
            const Mat4x4& nm = node_world[static_cast<std::size_t>(sv.rigid_node_index)];
            p = mat4_point(nm, sv.pos);
            n = mat4_dir(nm, sv.normal);
            t = mat4_dir(nm, sv.tangent);
        } else if (!skin_mats.empty()) {
            for (int k = 0; k < 4; ++k) {
                const float w = sv.weights[k];
                if (w <= 1e-5f) continue;
                const std::size_t ji = sv.joints[k] < skin_mats.size() ? sv.joints[k] : 0U;
                const Mat4x4& sm = skin_mats[ji];
                const Float3 sp = mat4_point(sm, sv.pos);
                const Float3 sn = mat4_dir(sm, sv.normal);
                const Float3 st = mat4_dir(sm, sv.tangent);
                p.x += sp.x * w; p.y += sp.y * w; p.z += sp.z * w;
                n.x += sn.x * w; n.y += sn.y * w; n.z += sn.z * w;
                t.x += st.x * w; t.y += st.y * w; t.z += st.z * w;
            }
        } else {
            p = mat4_point(root_xform, sv.pos);
            n = mat4_dir(root_xform, sv.normal);
            t = mat4_dir(root_xform, sv.tangent);
        }
        const Float3 nn = vec3_norm(n);
        const Float3 tt = vec3_norm(t, {1.0f, 0.0f, 0.0f});
        out_vertices[vi] = {
            p.x, p.y, p.z,
            nn.x, nn.y, nn.z,
            tt.x, tt.y, tt.z,
            sv.u, sv.v,
            sv.material_id,
            sv.emissive};
    }

    out_head_local = {0.0f, 1.56f, 0.0f};
    if (tpl.head_node_index >= 0 && static_cast<std::size_t>(tpl.head_node_index) < node_world.size()) {
        const Mat4x4& hm = node_world[static_cast<std::size_t>(tpl.head_node_index)];
        out_head_local = {hm.m[3], hm.m[7] + 0.10f, hm.m[11]};
    }
    out_chest_local = {out_head_local.x, std::max(0.55f, out_head_local.y - 0.42f), out_head_local.z};
    out_muzzle_local = {0.16f, std::max(0.85f, out_head_local.y - 0.30f), 0.52f};
    if (tpl.weapon_node_index >= 0 && static_cast<std::size_t>(tpl.weapon_node_index) < node_world.size()) {
        const Mat4x4& wm = node_world[static_cast<std::size_t>(tpl.weapon_node_index)];
        out_muzzle_local = {wm.m[3], wm.m[7] + 0.04f, wm.m[11] + 0.25f};
    }
}

}
