#language slang 2026
// Copyright (C) 2026 Kovid Goyal <kovid at kovidgoyal.net>
// Distributed under terms of the GPLv3 license.

static const float2 vertex_positions[4] = { float2(1, 1), float2(1, -1), float2(-1, -1), float2(-1, 1) };

[shader("vertex")]
float4
vertex_main(uint vertex_id: SV_VertexID)
    : SV_Position {
    return float4(vertex_positions[vertex_id], 0, 1);
}

[shader("fragment")]
float4
fragment_main(uniform float2 window_size, uniform float corner_radius, float4 frag_coord: SV_Position)
    : SV_Target {
    float2 edge_distance = min(frag_coord.xy, window_size - frag_coord.xy);
    float coverage = 1.0;

    if (edge_distance.x < corner_radius && edge_distance.y < corner_radius) {
        float distance_from_center = length(float2(corner_radius) - edge_distance);
        coverage = 1.0 - smoothstep(corner_radius - 0.75, corner_radius + 0.75, distance_from_center);
    }

    return float4(0, 0, 0, coverage);
}
