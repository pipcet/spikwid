/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#define VECS_PER_SPECIFIC_BRUSH 2

#include shared,prim_shared,brush,gradient_shared

flat varying vec2 v_center;
flat varying float v_start_offset;
flat varying float v_offset_scale;
flat varying float v_angle;

#define PI                  3.141592653589793

#ifdef WR_VERTEX_SHADER

struct ConicGradient {
    vec2 center_point;
    vec2 start_end_offset;
    float angle;
    int extend_mode;
    vec2 stretch_size;
};

ConicGradient fetch_gradient(int address) {
    vec4 data[2] = fetch_from_gpu_cache_2(address);
    return ConicGradient(
        data[0].xy,
        data[0].zw,
        float(data[1].x),
        int(data[1].y),
        data[1].zw
    );
}

void brush_vs(
    VertexInfo vi,
    int prim_address,
    RectWithSize local_rect,
    RectWithSize segment_rect,
    ivec4 prim_user_data,
    int specific_resource_address,
    mat4 transform,
    PictureTask pic_task,
    int brush_flags,
    vec4 texel_rect
) {
    ConicGradient gradient = fetch_gradient(prim_address);

    write_gradient_vertex(
        vi,
        local_rect,
        segment_rect,
        prim_user_data,
        brush_flags,
        texel_rect,
        gradient.extend_mode,
        gradient.stretch_size
    );

    v_center = gradient.center_point;
    v_angle = PI / 2.0 - gradient.angle;

    // Store 1/scale where scale = end_offset - start_offset
    // If scale = 0, we can't get its reciprocal. Instead, just use a zero scale.
    v_offset_scale =
        gradient.start_end_offset.x != gradient.start_end_offset.y
            ? 1.0 / (gradient.start_end_offset.y - gradient.start_end_offset.x)
            : 0.0;
    v_start_offset = gradient.start_end_offset.x * v_offset_scale;
}
#endif

#ifdef WR_FRAGMENT_SHADER
float get_gradient_offset(vec2 pos) {
    // Rescale UV to actual repetition size. This can't be done in the vertex
    // shader due to the use of atan() below.
    pos *= v_repeated_size;

    // Use inverse trig to find the angle offset from the relative position.
    vec2 current_dir = pos - v_center;
    float current_angle = atan(current_dir.y, current_dir.x) + v_angle;
    return fract(current_angle / (2.0 * PI)) * v_offset_scale - v_start_offset;
}

Fragment brush_fs() {
    vec4 color = sample_gradient(get_gradient_offset(compute_repeated_pos()));

#ifdef WR_FEATURE_ALPHA_PASS
    color *= antialias_brush();
#endif

    return Fragment(color);
}
#endif
