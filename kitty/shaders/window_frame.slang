#language slang 2026
// Copyright (C) 2026 Kovid Goyal <kovid at kovidgoyal.net>
// Distributed under terms of the GPLv3 license.

static const float2 vertex_positions[4] = { float2(1, 1), float2(1, -1), float2(-1, -1), float2(-1, 1) };

float3
srgb_to_linear(float3 color) {
    float3 low = color / 12.92;
    float3 high = pow((color + 0.055) / 1.055, float3(2.4));
    return select(color <= 0.04045, low, high);
}

[shader("vertex")]
float4
vertex_main(uint vertex_id: SV_VertexID)
    : SV_Position {
    return float4(vertex_positions[vertex_id], 0, 1);
}

[shader("fragment")]
float4
fragment_main(
    uniform float2 window_size, uniform float2 frame_width, uniform float corner_radius, uniform float dark_appearance, float4 frag_coord: SV_Position)
    : SV_Target {
    float2 half_size = window_size * 0.5;
    float2 inner_half_size = max(float2(0), half_size - frame_width);
    float inner_radius = max(0.0, corner_radius - min(frame_width.x, frame_width.y));
    float2 q = abs(frag_coord.xy - half_size) - inner_half_size + inner_radius;
    float distance_to_inner = min(max(q.x, q.y), 0.0) + length(max(q, float2(0))) - inner_radius;
    float coverage = smoothstep(-0.75, 0.75, distance_to_inner);

    // Zen's three-color workspace gradient, evaluated in CSS sRGB space.
    float2 css_pos = float2(frag_coord.x, window_size.y - frag_coord.y);
    float2 centered = css_pos - half_size;
    float angle = radians(-5.0);
    float2 direction = float2(sin(angle), -cos(angle));
    float extent = 0.5 * dot(abs(direction), window_size);
    float linear_t = (dot(centered, direction) + extent) / (2.0 * extent);
    float linear_alpha = 1.0 - saturate((linear_t - 0.10) / 0.70);

    float diagonal = length(window_size);
    float left_t = length(css_pos) / diagonal;
    float left_alpha = 1.0 - saturate((left_t - 0.10) / 0.60);

    float2 right_origin = float2(window_size.x * 0.95, 0.0);
    float right_radius = length(float2(window_size.x * 0.95, window_size.y));
    float right_t = length(css_pos - right_origin) / right_radius;
    float right_alpha = 1.0 - saturate(right_t / 0.75);

    // Zen blends the workspace colors against a mode-specific toolbar base.
    float3 base = lerp(float3(240.0, 240.0, 244.0), float3(23.0, 23.0, 26.0), dark_appearance) / 255.0;
    float3 left = lerp(float3(175.0, 172.0, 182.0), float3(23.0, 20.0, 29.0), dark_appearance) / 255.0;
    float3 right = lerp(float3(179.0, 172.0, 180.0), float3(27.0, 20.0, 28.0), dark_appearance) / 255.0;
    float3 linear = lerp(float3(172.0, 175.0, 182.0), float3(20.0, 23.0, 29.0), dark_appearance) / 255.0;
    float3 color = base;
    color = lerp(color, left, left_alpha);
    color = lerp(color, right, right_alpha);
    color = lerp(color, linear, linear_alpha);
    return float4(srgb_to_linear(color) * coverage, coverage);
}
