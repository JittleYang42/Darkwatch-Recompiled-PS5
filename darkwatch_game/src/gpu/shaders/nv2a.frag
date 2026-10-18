#version 450
layout(location = 0) in vec4 v_color;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in float v_fog;
layout(location = 0) out vec4 o_color;

layout(set = 0, binding = 0) uniform sampler2D u_tex;

layout(push_constant) uniform PC {
    vec4 vp;
    vec4 tex;    // .z > 0.5: textured
    vec4 alpha;  // .x func 0..7 (GL_NEVER..GL_ALWAYS order), 8 = off; .y ref
    vec4 fog;    // fog colour rgb, .a > 0.5: fog on
} pc;

void main()
{
    // Stage 0 with D3D's default colour and alpha ops (MODULATE). Register
    // combiner programs are not passed through the back end yet.
    vec4 c = v_color;
    if (pc.tex.z > 0.5)
        c *= texture(u_tex, v_uv);

    int f = int(pc.alpha.x + 0.5);
    if (f < 8) {
        float a = c.a, r = pc.alpha.y;
        bool pass = f == 0 ? false
                  : f == 1 ? a <  r
                  : f == 2 ? a == r
                  : f == 3 ? a <= r
                  : f == 4 ? a >  r
                  : f == 5 ? a != r
                  : f == 6 ? a >= r
                  : true;
        if (!pass)
            discard;
    }
    // The final combiner's fog, as D3D sets it up: the fog colour mixed in
    // by the interpolated factor, clamped per pixel; alpha untouched.
    if (pc.fog.a > 0.5)
        c.rgb = mix(pc.fog.rgb, c.rgb, clamp(v_fog, 0.0, 1.0));
    o_color = c;
}
