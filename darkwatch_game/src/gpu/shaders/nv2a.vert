#version 450
// Vertices as the pushbuffer executor hands them over (nv2a_backend.h,
// NV2A_BACKEND_HOMOGENEOUS): x, y in surface pixels and z as the title
// produced it (0..clip max), all three multiplied by w; the fourth component
// is w itself (1 for pre-transformed vertices). diffuse 0xAARRGGBB, u, v in
// texels, the fog factor (unclamped). Nothing is clipped or divided on the
// CPU: the GPU clips in clip space and divides, as the NV2A does.
layout(location = 0) in vec4 in_pos;     // x*w, y*w, z*w, w
layout(location = 1) in vec4 in_color;   // B8G8R8A8_UNORM: arrives as rgba
layout(location = 2) in vec2 in_uv;      // texels
layout(location = 3) in float in_fog;    // 1 = no fog, 0 = fog colour

layout(push_constant) uniform PC {
    vec4 vp;     // 2/width, 2/height, 1/zmax, .w > 0.5: clamp depth (else clip)
    vec4 tex;    // 1/tex_width, 1/tex_height, textured, unused
    vec4 alpha;  // func (0..7 = GL_NEVER..GL_ALWAYS, 8 = off), ref 0..1
    vec4 fog;    // fog colour rgb, .a > 0.5: fog on
} pc;

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec2 v_uv;
layout(location = 2) out float v_fog;

void main()
{
    // Surface pixels to NDC is x * 2/width - 1; in clip space (times w)
    // that is x*w * 2/width - w. w stays the clip w, so the rasteriser
    // interpolates colour and texture coordinates with perspective, as the
    // NV2A does.
    float w = in_pos.w;
    vec2 xy = vec2(in_pos.x * pc.vp.x - w, in_pos.y * pc.vp.y - w);
    // The NV2A culls what lies beyond the near and far planes unless the
    // title asks for clamping (SET_ZMIN_MAX_CONTROL); unclamped, Vulkan
    // clips at 0 <= z <= w the same way -- which is also the near-plane
    // clip that keeps what is behind the eye off the screen.
    float z = in_pos.z * pc.vp.z;
    if (pc.vp.w > 0.5 && w > 0.0)
        z = clamp(z, 0.0, w);
    gl_Position = vec4(xy, z, w);
    v_color = in_color;
    v_uv = in_uv * pc.tex.xy;
    v_fog = in_fog;
}
