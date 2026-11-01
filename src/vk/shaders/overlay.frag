#version 450
// [UIT 0171] The in-game overlay's quads: rounded rectangles, outlines and the hold ring as signed distances (crisp at
// any size, one pixel of anti-aliasing), text from the R8 font atlas (made at the drawn size: texel = pixel).
layout(location = 0) in vec2 i_local;
layout(location = 1) in vec2 i_uv;
layout(location = 2) in vec4 i_color;
layout(location = 3) flat in vec4 i_shape;   // half width, half height, radius, stroke
layout(location = 4) flat in vec2 i_kind;    // 0 rect, 1 text, 2 outline, 3 ring; ring fraction
layout(set = 0, binding = 0) uniform sampler2D atlas;
layout(location = 0) out vec4 o_color;

float round_box(vec2 p, vec2 b, float r) {
    r = min(r, min(b.x, b.y));
    vec2 q = abs(p) - b + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

void main() {
    int kind = int(i_kind.x + 0.5);
    float a;
    vec4 c = i_color;
    if (kind == 1) {
        a = texture(atlas, i_uv).r;
    } else if (kind == 3) {
        // a ring of radius half width, stroke thick; filled clockwise from the top up to the fraction, a faint track after
        float r = i_shape.x - i_shape.w * 0.5 - 0.5, d = abs(length(i_local) - r) - i_shape.w * 0.5;
        a = clamp(0.5 - d, 0.0, 1.0);
        float ang = atan(i_local.x, -i_local.y) / 6.2831853;
        if (ang < 0.0) ang += 1.0;
        if (ang > i_kind.y) c = vec4(c.rgb * 0.35 + 0.12, c.a * 0.55);
    } else {
        float d = round_box(i_local, i_shape.xy, i_shape.z);
        a = clamp(0.5 - d, 0.0, 1.0);
        if (kind == 2) a *= clamp(d + i_shape.w + 0.5, 0.0, 1.0);   // the band stroke wide inside the edge
    }
    o_color = vec4(c.rgb, c.a * a);
}
