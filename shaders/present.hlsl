cbuffer PresentConstants : register(b0) {
    float4 viewport_and_flags;
    float4 warp_flags_and_aspect;
    float4 source_camera_position_tan_y;
    float4 source_camera_right_rcp_width;
    float4 source_camera_up_rcp_height;
    float4 source_camera_forward_tan_x;
    float4 target_camera_position_tan_y;
    float4 target_camera_right_rcp_tan_x;
    float4 target_camera_up_rcp_tan_y;
    float4 target_camera_forward_tan_x;
};

cbuffer HudConstants : register(b1) {
    float4 hud_panel_rect;
    float4 hud_graph_rect;
    float4 hud_graph_scale;
    uint4 hud_text_meta;
    uint4 hud_text_packed[16];
    float4 hud_graph_samples[128];
};

Texture2D<float4> scene_color_texture : register(t0);
Texture2D<float> scene_depth_texture : register(t1);
Texture2D<float2> scene_motion_texture : register(t2);
RWByteAddressBuffer reprojection_stats : register(u1);
SamplerState linear_clamp_sampler : register(s0);
SamplerState point_clamp_sampler : register(s1);

struct VertexOutput {
    float4 position : SV_Position;
};

struct ReprojectionSample {
    float2 source_uv;
    bool valid;
};

VertexOutput vertex_main(uint vertex_id : SV_VertexID) {
    VertexOutput output;
    float2 corner = float2((vertex_id << 1) & 2, vertex_id & 2);
    output.position = float4(corner * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

bool project_world_to_camera_uv(
    float3 world_position,
    float3 camera_position,
    float3 camera_right,
    float3 camera_up,
    float3 camera_forward,
    float rcp_tan_half_x,
    float rcp_tan_half_y,
    out float2 out_uv,
    out float out_view_z) {
    float3 relative = world_position - camera_position;
    float view_z = dot(relative, camera_forward);
    out_view_z = view_z;
    if (view_z <= 0.01) {
        out_uv = float2(-1.0, -1.0);
        return false;
    }
    float inv_z = rcp(view_z);
    float ndc_x = dot(relative, camera_right) * inv_z * rcp_tan_half_x;
    float ndc_y = dot(relative, camera_up) * inv_z * rcp_tan_half_y;
    out_uv = float2(ndc_x * 0.5 + 0.5, 0.5 - ndc_y * 0.5);
    return out_uv.x >= 0.0 && out_uv.x <= 1.0 && out_uv.y >= 0.0 && out_uv.y <= 1.0;
}

float3 reconstruct_source_world_position(float2 source_uv, float linear_view_depth) {
    float2 ndc = float2(source_uv.x * 2.0 - 1.0, 1.0 - source_uv.y * 2.0);
    return source_camera_position_tan_y.xyz
        + source_camera_right_rcp_width.xyz * (ndc.x * source_camera_forward_tan_x.w * linear_view_depth)
        + source_camera_up_rcp_height.xyz * (ndc.y * source_camera_position_tan_y.w * linear_view_depth)
        + source_camera_forward_tan_x.xyz * linear_view_depth;
}

ReprojectionSample evaluate_reprojection(float2 target_uv, float2 viewport_size, float2 texel_size) {
    ReprojectionSample result;
    result.source_uv = target_uv;
    result.valid = false;

    float2 target_ndc = float2(target_uv.x * 2.0 - 1.0, 1.0 - target_uv.y * 2.0);
    float3 target_origin = target_camera_position_tan_y.xyz;
    float3 target_ray = normalize(
        target_camera_right_rcp_tan_x.xyz * (target_ndc.x * target_camera_forward_tan_x.w)
        + target_camera_up_rcp_tan_y.xyz * (target_ndc.y * target_camera_position_tan_y.w)
        + target_camera_forward_tan_x.xyz);

    float source_ray_z = dot(target_ray, source_camera_forward_tan_x.xyz);
    if (source_ray_z <= 0.05) {
        return result;
    }

    float rcp_source_tan_x = rcp(source_camera_forward_tan_x.w);
    float rcp_source_tan_y = rcp(source_camera_position_tan_y.w);
    float inv_source_ray_z = rcp(source_ray_z);
    float2 initial_ndc = float2(
        dot(target_ray, source_camera_right_rcp_width.xyz) * inv_source_ray_z * rcp_source_tan_x,
        dot(target_ray, source_camera_up_rcp_height.xyz) * inv_source_ray_z * rcp_source_tan_y);
    float2 candidate_uv = float2(initial_ndc.x * 0.5 + 0.5, 0.5 - initial_ndc.y * 0.5);
    if (candidate_uv.x < 0.0 || candidate_uv.x > 1.0 || candidate_uv.y < 0.0 || candidate_uv.y > 1.0) {
        return result;
    }

    float sampled_depth = scene_depth_texture.SampleLevel(point_clamp_sampler, candidate_uv, 0);
    float ray_distance = sampled_depth * rcp(max(source_ray_z, 0.1));
    float2 projected_uv = candidate_uv;
    float projected_source_z = sampled_depth;

    [unroll]
    for (int iteration = 0; iteration < 4; ++iteration) {
        float3 candidate_world = target_origin + target_ray * ray_distance;
        if (!project_world_to_camera_uv(
                candidate_world,
                source_camera_position_tan_y.xyz,
                source_camera_right_rcp_width.xyz,
                source_camera_up_rcp_height.xyz,
                source_camera_forward_tan_x.xyz,
                rcp_source_tan_x,
                rcp_source_tan_y,
                projected_uv,
                projected_source_z)) {
            return result;
        }
        sampled_depth = scene_depth_texture.SampleLevel(point_clamp_sampler, projected_uv, 0);
        float3 surface_world = reconstruct_source_world_position(projected_uv, sampled_depth);
        ray_distance = max(dot(surface_world - target_origin, target_ray), 0.05);
    }

    float3 matched_world = reconstruct_source_world_position(projected_uv, sampled_depth);
    float2 roundtrip_uv;
    float roundtrip_target_z;
    if (!project_world_to_camera_uv(
            matched_world,
            target_origin,
            target_camera_right_rcp_tan_x.xyz,
            target_camera_up_rcp_tan_y.xyz,
            target_camera_forward_tan_x.xyz,
            target_camera_right_rcp_tan_x.w,
            target_camera_up_rcp_tan_y.w,
            roundtrip_uv,
            roundtrip_target_z)) {
        return result;
    }

    float inv_safe_depth = rcp(max(sampled_depth, 0.25));
    float pixel_error = length((roundtrip_uv - target_uv) * viewport_size);
    float relative_depth_error = abs(projected_source_z - sampled_depth) * inv_safe_depth;

    float depth_left = scene_depth_texture.SampleLevel(point_clamp_sampler, projected_uv - float2(texel_size.x, 0.0), 0);
    float depth_right = scene_depth_texture.SampleLevel(point_clamp_sampler, projected_uv + float2(texel_size.x, 0.0), 0);
    float depth_up = scene_depth_texture.SampleLevel(point_clamp_sampler, projected_uv - float2(0.0, texel_size.y), 0);
    float depth_down = scene_depth_texture.SampleLevel(point_clamp_sampler, projected_uv + float2(0.0, texel_size.y), 0);
    float max_neighbor_jump = max(
        max(abs(depth_left - sampled_depth), abs(depth_right - sampled_depth)),
        max(abs(depth_up - sampled_depth), abs(depth_down - sampled_depth))) * inv_safe_depth;

    bool consistent = (pixel_error <= 1.75) && (relative_depth_error <= 0.045) && !(max_neighbor_jump > 0.18 && pixel_error > 0.65);
    result.source_uv = projected_uv;
    result.valid = consistent;
    return result;
}

uint pack_rows_0_to_3(uint r0, uint r1, uint r2, uint r3) {
    return (r0 & 31u) | ((r1 & 31u) << 5u) | ((r2 & 31u) << 10u) | ((r3 & 31u) << 15u);
}

uint pack_rows_4_to_6(uint r4, uint r5, uint r6) {
    return (r4 & 31u) | ((r5 & 31u) << 5u) | ((r6 & 31u) << 10u);
}

uint2 glyph_bitmap_5x7(uint ascii_code) {
    if (ascii_code == 48u) return uint2(pack_rows_0_to_3(14, 17, 19, 21), pack_rows_4_to_6(25, 17, 14));
    if (ascii_code == 49u) return uint2(pack_rows_0_to_3(4, 12, 4, 4), pack_rows_4_to_6(4, 4, 14));
    if (ascii_code == 50u) return uint2(pack_rows_0_to_3(14, 17, 1, 2), pack_rows_4_to_6(4, 8, 31));
    if (ascii_code == 51u) return uint2(pack_rows_0_to_3(30, 1, 1, 14), pack_rows_4_to_6(1, 1, 30));
    if (ascii_code == 52u) return uint2(pack_rows_0_to_3(2, 6, 10, 18), pack_rows_4_to_6(31, 2, 2));
    if (ascii_code == 53u) return uint2(pack_rows_0_to_3(31, 16, 30, 1), pack_rows_4_to_6(1, 17, 14));
    if (ascii_code == 54u) return uint2(pack_rows_0_to_3(14, 16, 30, 17), pack_rows_4_to_6(17, 17, 14));
    if (ascii_code == 55u) return uint2(pack_rows_0_to_3(31, 1, 2, 4), pack_rows_4_to_6(8, 8, 8));
    if (ascii_code == 56u) return uint2(pack_rows_0_to_3(14, 17, 17, 14), pack_rows_4_to_6(17, 17, 14));
    if (ascii_code == 57u) return uint2(pack_rows_0_to_3(14, 17, 17, 15), pack_rows_4_to_6(1, 2, 12));

    if (ascii_code == 65u) return uint2(pack_rows_0_to_3(14, 17, 17, 31), pack_rows_4_to_6(17, 17, 17));
    if (ascii_code == 66u) return uint2(pack_rows_0_to_3(30, 17, 17, 30), pack_rows_4_to_6(17, 17, 30));
    if (ascii_code == 67u) return uint2(pack_rows_0_to_3(14, 17, 16, 16), pack_rows_4_to_6(16, 17, 14));
    if (ascii_code == 68u) return uint2(pack_rows_0_to_3(30, 17, 17, 17), pack_rows_4_to_6(17, 17, 30));
    if (ascii_code == 69u) return uint2(pack_rows_0_to_3(31, 16, 16, 30), pack_rows_4_to_6(16, 16, 31));
    if (ascii_code == 70u) return uint2(pack_rows_0_to_3(31, 16, 16, 30), pack_rows_4_to_6(16, 16, 16));
    if (ascii_code == 71u) return uint2(pack_rows_0_to_3(14, 17, 16, 23), pack_rows_4_to_6(17, 17, 14));
    if (ascii_code == 72u) return uint2(pack_rows_0_to_3(17, 17, 17, 31), pack_rows_4_to_6(17, 17, 17));
    if (ascii_code == 73u) return uint2(pack_rows_0_to_3(14, 4, 4, 4), pack_rows_4_to_6(4, 4, 14));
    if (ascii_code == 74u) return uint2(pack_rows_0_to_3(7, 2, 2, 2), pack_rows_4_to_6(18, 18, 12));
    if (ascii_code == 75u) return uint2(pack_rows_0_to_3(17, 18, 20, 24), pack_rows_4_to_6(20, 18, 17));
    if (ascii_code == 76u) return uint2(pack_rows_0_to_3(16, 16, 16, 16), pack_rows_4_to_6(16, 16, 31));
    if (ascii_code == 77u) return uint2(pack_rows_0_to_3(17, 27, 21, 21), pack_rows_4_to_6(17, 17, 17));
    if (ascii_code == 78u) return uint2(pack_rows_0_to_3(17, 25, 21, 19), pack_rows_4_to_6(17, 17, 17));
    if (ascii_code == 79u) return uint2(pack_rows_0_to_3(14, 17, 17, 17), pack_rows_4_to_6(17, 17, 14));
    if (ascii_code == 80u) return uint2(pack_rows_0_to_3(30, 17, 17, 30), pack_rows_4_to_6(16, 16, 16));
    if (ascii_code == 81u) return uint2(pack_rows_0_to_3(14, 17, 17, 17), pack_rows_4_to_6(21, 18, 13));
    if (ascii_code == 82u) return uint2(pack_rows_0_to_3(30, 17, 17, 30), pack_rows_4_to_6(20, 18, 17));
    if (ascii_code == 83u) return uint2(pack_rows_0_to_3(15, 16, 16, 14), pack_rows_4_to_6(1, 1, 30));
    if (ascii_code == 84u) return uint2(pack_rows_0_to_3(31, 4, 4, 4), pack_rows_4_to_6(4, 4, 4));
    if (ascii_code == 85u) return uint2(pack_rows_0_to_3(17, 17, 17, 17), pack_rows_4_to_6(17, 17, 14));
    if (ascii_code == 86u) return uint2(pack_rows_0_to_3(17, 17, 17, 17), pack_rows_4_to_6(17, 10, 4));
    if (ascii_code == 87u) return uint2(pack_rows_0_to_3(17, 17, 17, 21), pack_rows_4_to_6(21, 27, 17));
    if (ascii_code == 88u) return uint2(pack_rows_0_to_3(17, 17, 10, 4), pack_rows_4_to_6(10, 17, 17));
    if (ascii_code == 89u) return uint2(pack_rows_0_to_3(17, 17, 10, 4), pack_rows_4_to_6(4, 4, 4));
    if (ascii_code == 90u) return uint2(pack_rows_0_to_3(31, 1, 2, 4), pack_rows_4_to_6(8, 16, 31));

    if (ascii_code == 46u) return uint2(pack_rows_0_to_3(0, 0, 0, 0), pack_rows_4_to_6(0, 12, 12));
    if (ascii_code == 58u) return uint2(pack_rows_0_to_3(0, 12, 12, 0), pack_rows_4_to_6(12, 12, 0));
    if (ascii_code == 47u) return uint2(pack_rows_0_to_3(1, 1, 2, 4), pack_rows_4_to_6(8, 16, 16));
    if (ascii_code == 37u) return uint2(pack_rows_0_to_3(25, 25, 2, 4), pack_rows_4_to_6(8, 19, 19));
    if (ascii_code == 45u) return uint2(pack_rows_0_to_3(0, 0, 0, 31), pack_rows_4_to_6(0, 0, 0));
    if (ascii_code == 43u) return uint2(pack_rows_0_to_3(0, 4, 4, 31), pack_rows_4_to_6(4, 4, 0));
    if (ascii_code == 91u) return uint2(pack_rows_0_to_3(14, 8, 8, 8), pack_rows_4_to_6(8, 8, 14));
    if (ascii_code == 93u) return uint2(pack_rows_0_to_3(14, 2, 2, 2), pack_rows_4_to_6(2, 2, 14));
    if (ascii_code == 95u) return uint2(pack_rows_0_to_3(0, 0, 0, 0), pack_rows_4_to_6(0, 0, 31));
    return uint2(0u, 0u);
}

bool sample_glyph_pixel(uint ascii_code, uint local_x, uint local_y) {
    if (ascii_code <= 32u || local_x >= 5u || local_y >= 7u) return false;
    uint2 bits = glyph_bitmap_5x7(ascii_code);
    uint row_bits = (local_y < 4u)
        ? ((bits.x >> (local_y * 5u)) & 31u)
        : ((bits.y >> ((local_y - 4u) * 5u)) & 31u);
    uint column_mask = 1u << (4u - local_x);
    return (row_bits & column_mask) != 0u;
}

uint read_hud_char(uint line_index, uint col_index) {
    uint char_index = line_index * 64u + col_index;
    uint word_index = char_index >> 2u;
    uint vec_index = word_index >> 2u;
    uint comp_index = word_index & 3u;
    uint byte_shift = (char_index & 3u) * 8u;
    uint4 vector_word = hud_text_packed[vec_index];
    uint word = (comp_index == 0u) ? vector_word.x : (comp_index == 1u) ? vector_word.y : (comp_index == 2u) ? vector_word.z : vector_word.w;
    return (word >> byte_shift) & 255u;
}

float3 frame_kind_color(float frame_kind) {
    if (frame_kind < 0.5) return float3(0.28, 0.84, 0.96);
    if (frame_kind < 1.5) return float3(0.96, 0.74, 0.24);
    if (frame_kind < 2.5) return float3(0.26, 0.92, 0.54);
    return float3(0.98, 0.26, 0.68);
}

float3 composite_hud_layer(float2 pixel_pos, float3 base_color) {
    float2 panel_min = hud_panel_rect.xy;
    float2 panel_size = hud_panel_rect.zw;
    float2 panel_max = panel_min + panel_size;
    if (panel_size.x <= 1.0 || panel_size.y <= 1.0) return base_color;
    if (pixel_pos.x < panel_min.x || pixel_pos.x >= panel_max.x || pixel_pos.y < panel_min.y || pixel_pos.y >= panel_max.y) {
        return base_color;
    }

    float3 result = lerp(base_color, float3(0.05, 0.08, 0.11), 0.80);
    bool on_panel_border = pixel_pos.x < panel_min.x + 1.5 || pixel_pos.x >= panel_max.x - 1.5
        || pixel_pos.y < panel_min.y + 1.5 || pixel_pos.y >= panel_max.y - 1.5;
    if (on_panel_border) {
        result = lerp(result, float3(0.32, 0.48, 0.58), 0.75);
    }

    uint line_count = hud_text_meta.x;
    uint chars_per_line = hud_text_meta.y;
    uint char_scale = max(hud_text_meta.z, 1u);
    float2 text_origin = panel_min + float2(10.0, 8.0);
    float char_advance_x = 6.0 * (float)char_scale;
    float line_advance_y = 9.0 * (float)char_scale;
    float2 text_local = pixel_pos - text_origin;
    if (text_local.x >= 0.0 && text_local.y >= 0.0) {
        uint line_index = (uint)(text_local.y / line_advance_y);
        uint col_index = (uint)(text_local.x / char_advance_x);
        if (line_index < line_count && col_index < chars_per_line) {
            uint glyph_x = (uint)((text_local.x - (float)col_index * char_advance_x) / (float)char_scale);
            uint glyph_y = (uint)((text_local.y - (float)line_index * line_advance_y) / (float)char_scale);
            uint ascii_code = read_hud_char(line_index, col_index);
            if (sample_glyph_pixel(ascii_code, glyph_x, glyph_y)) {
                float3 text_color = (line_index == 0u) ? float3(0.96, 0.99, 1.0)
                    : (line_index == 1u) ? float3(0.76, 0.90, 0.96)
                    : (line_index == 2u) ? float3(0.84, 0.88, 0.80)
                    : float3(0.62, 0.72, 0.78);
                return text_color;
            }
        }
    }

    float2 graph_min = hud_graph_rect.xy;
    float2 graph_size = hud_graph_rect.zw;
    float2 graph_max = graph_min + graph_size;
    if (pixel_pos.x >= graph_min.x && pixel_pos.x < graph_max.x && pixel_pos.y >= graph_min.y && pixel_pos.y < graph_max.y) {
        result = lerp(base_color, float3(0.03, 0.05, 0.07), 0.90);
        float max_ms = max(hud_graph_scale.x, 1.0);
        float target_ms = hud_graph_scale.y;
        float norm_x = saturate((pixel_pos.x - graph_min.x) / graph_size.x);
        float pixel_ms = saturate((graph_max.y - pixel_pos.y) / graph_size.y) * max_ms;

        float ref_120_y = graph_max.y - saturate(target_ms / max_ms) * graph_size.y;
        float ref_60_y = graph_max.y - saturate(16.667 / max_ms) * graph_size.y;
        if (abs(pixel_pos.y - ref_120_y) < 0.75) {
            result = lerp(result, float3(0.24, 0.65, 0.42), 0.70);
        }
        if (abs(pixel_pos.y - ref_60_y) < 0.75) {
            result = lerp(result, float3(0.70, 0.50, 0.22), 0.60);
        }

        uint sample_slot = min((uint)(norm_x * 128.0), 127u);
        uint write_head = (uint)(hud_graph_scale.w + 0.5);
        uint history_index = (write_head + sample_slot) & 127u;
        float4 sample_data = hud_graph_samples[history_index];
        float present_ms = sample_data.x;
        float gpu_ms = sample_data.y;
        float3 bar_color = frame_kind_color(sample_data.z);

        if (present_ms > 0.0) {
            float sample_y = graph_max.y - saturate(present_ms / max_ms) * graph_size.y;
            if (pixel_pos.y >= sample_y) {
                result = lerp(result, bar_color, 0.32);
            }
            if (abs(pixel_pos.y - sample_y) < 1.35) {
                result = bar_color;
            }
        }
        if (gpu_ms > 0.0 && pixel_ms <= gpu_ms) {
            result = lerp(result, float3(0.98, 0.94, 0.48), 0.75);
        }

        bool on_graph_border = pixel_pos.x < graph_min.x + 1.0 || pixel_pos.x >= graph_max.x - 1.0
            || pixel_pos.y < graph_min.y + 1.0 || pixel_pos.y >= graph_max.y - 1.0;
        if (on_graph_border) {
            result = float3(0.26, 0.38, 0.46);
        }
    }

    return result;
}

float4 pixel_main(VertexOutput input) : SV_Target {
    float2 viewport_size = viewport_and_flags.xy;
    float2 texel_size = float2(source_camera_right_rcp_width.w, source_camera_up_rcp_height.w);
    int diagnostic_view = (int)(viewport_and_flags.z + 0.5);
    bool overlay_enabled = viewport_and_flags.w > 0.5;
    bool warp_active = warp_flags_and_aspect.x > 0.5;
    float2 uv = input.position.xy * texel_size;

    float2 sample_uv = uv;
    bool pixel_valid = true;
    if (warp_active) {
        ReprojectionSample reprojection = evaluate_reprojection(uv, viewport_size, texel_size);
        sample_uv = saturate(reprojection.source_uv);
        pixel_valid = reprojection.valid;
        if (!pixel_valid) {
            uint previous_count;
            reprojection_stats.InterlockedAdd(0, 1, previous_count);
        }
    }

    float3 output_color = float3(0.0, 0.0, 0.0);
    if (diagnostic_view == 1) {
        float linear_depth = scene_depth_texture.SampleLevel(point_clamp_sampler, sample_uv, 0);
        float normalized_depth = saturate(1.0 - linear_depth / 32.0);
        output_color = float3(normalized_depth, normalized_depth * 0.9, normalized_depth * 0.75);
    } else if (diagnostic_view == 2) {
        float2 motion = scene_motion_texture.SampleLevel(point_clamp_sampler, sample_uv, 0);
        float2 scaled = clamp(motion * 24.0, -1.0, 1.0);
        output_color = float3(saturate(scaled.x * 0.5 + 0.5), saturate(scaled.y * 0.5 + 0.5), saturate(length(scaled)));
    } else {
        float3 linear_color = scene_color_texture.SampleLevel(linear_clamp_sampler, sample_uv, 0).rgb;
        output_color = pow(saturate(linear_color), 1.0 / 2.2);
        if (warp_active && !pixel_valid) {
            if (diagnostic_view == 3) {
                output_color = float3(1.0, 0.12, 0.78);
            } else {
                float3 fallback_color = pow(saturate(scene_color_texture.SampleLevel(linear_clamp_sampler, uv, 0).rgb), 1.0 / 2.2);
                output_color = lerp(output_color, fallback_color, 0.65);
            }
        }
    }

    if (overlay_enabled) {
        float2 center_offset = abs(input.position.xy - viewport_size * 0.5);
        bool horizontal_arm = center_offset.y < 1.25 && center_offset.x > 4.0 && center_offset.x < 14.0;
        bool vertical_arm = center_offset.x < 1.25 && center_offset.y > 4.0 && center_offset.y < 14.0;
        bool center_dot = center_offset.x < 1.25 && center_offset.y < 1.25;
        if (horizontal_arm || vertical_arm || center_dot) {
            output_color = float3(0.95, 0.98, 1.0);
        }
        output_color = composite_hud_layer(input.position.xy, output_color);
    }

    bool flash_enabled = warp_flags_and_aspect.z > 0.5;
    if (flash_enabled) {
        bool in_flash_box = input.position.x >= 0.0 && input.position.x < 72.0
            && abs(input.position.y - viewport_size.y * 0.5) < 48.0;
        if (in_flash_box) {
            bool flash_lit = warp_flags_and_aspect.w > 0.5;
            output_color = flash_lit ? float3(1.0, 1.0, 1.0) : float3(0.02, 0.02, 0.02);
        }
    }

    return float4(output_color, 1.0);
}
