#version 450
// [UIT 0171] The in-game overlay (src/vk/overlay_vk.c): one instance per quad (UitOvQuad, src/host/uit_overlay.h),
// four vertices as a triangle strip. Pixel coordinates (top left origin) to clip space with the push constant.
layout(location = 0) in vec4 i_rect;     // x0 y0 x1 y1 (pixels)
layout(location = 1) in vec4 i_uv;       // u0 v0 u1 v1 (atlas); the ring's fraction in u0
layout(location = 2) in vec4 i_color;    // R8G8B8A8_UNORM
layout(location = 3) in vec3 i_shape;    // radius, kind, stroke
layout(push_constant) uniform Push { vec2 inv_size; } pc;   // 2 / size
layout(location = 0) out vec2 o_local;   // pixels from the quad's centre
layout(location = 1) out vec2 o_uv;
layout(location = 2) out vec4 o_color;
layout(location = 3) flat out vec4 o_shape;   // half width, half height, radius, stroke
layout(location = 4) flat out vec2 o_kind;    // kind, ring fraction

void main() {
    vec2 t = vec2(gl_VertexIndex & 1, gl_VertexIndex >> 1);
    vec2 p = mix(i_rect.xy, i_rect.zw, t);
    gl_Position = vec4(p * pc.inv_size - 1.0, 0.0, 1.0);
    vec2 half_size = (i_rect.zw - i_rect.xy) * 0.5;
    o_local = (t - 0.5) * 2.0 * half_size;
    o_uv = mix(i_uv.xy, i_uv.zw, t);
    o_color = i_color;
    o_shape = vec4(half_size, i_shape.x, i_shape.z);
    o_kind = vec2(i_shape.y, i_uv.x);
}
