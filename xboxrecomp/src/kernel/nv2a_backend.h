/*
 * NV2A render back-end interface.
 *
 * The pushbuffer executor (nv2a_pb_exec.c) decodes what a title asks the GPU
 * to do. By default it carries that out itself, on the CPU, into guest memory.
 * A game project can instead register a back end -- a D3D11 renderer, say --
 * and the executor hands it the work in already-decoded form:
 *
 *   surface state + clears, triangles (transformed to surface pixels, with
 *   per-vertex colour and texel-space UVs), the texture each batch samples,
 *   and the flip that ends a frame.
 *
 * Everything is called on the NV2A poll thread, one call at a time, so a back
 * end may create its window and device lazily on the first call and pump its
 * window messages from flip().
 *
 * Fixed-function, pre-transformed and vertex-program batches all arrive the
 * same way: the executor transforms fixed-function vertices itself, runs
 * vertex programs on the CPU (nv2a_vsh_interp.c) and clips at the near plane,
 * so a back end only ever sees surface pixels. Blend, depth, alpha, cull and
 * stencil state arrive in Nv2aRenderState.
 *
 * ponytail: register-combiner state and texture stages 1-3 are not passed on:
 * a back end sees stage 0, the diffuse colour and the fog. Extend Nv2aBatch
 * when that matters rather than adding callbacks.
 */
#ifndef NV2A_BACKEND_H
#define NV2A_BACKEND_H

#include <stdint.h>

/* The colour surface being drawn into, in real (anti-aliased) pixels.
 * aa_sx/aa_sy give the anti-aliasing factor, so width/aa_sx is the logical
 * size the title thinks it renders at (e.g. 640x480). */
typedef struct {
    uint32_t color_va;          /* guest address of pixel (0,0) */
    uint32_t width, height;     /* clip rectangle size, real pixels */
    uint32_t pitch;             /* bytes per row */
    uint32_t bytes_per_pixel;   /* 2 or 4 */
    uint32_t aa_sx, aa_sy;      /* 1 or 2 each */
} Nv2aSurface;

/* A texture as the title programmed it. uv in Nv2aVertex are in texels. */
typedef struct {
    uint32_t offset;            /* guest address of texel (0,0) */
    uint32_t width, height;
    uint32_t pitch;             /* linear formats only */
    uint32_t color;             /* NV097 colour-format code */
    uint32_t addr_u, addr_v;    /* NV097 wrap mode per axis (1 wrap, 3 clamp) */
    /* Mip levels stored after level 0 (the format word's MIPMAP_LEVELS; 1 for
     * a linear texture) and the raw SET_TEXTURE_FILTER word (MIN in bits
     * 16-23). A back end that ignores them samples level 0. */
    uint32_t levels, filter;
    /* Guest address of the 256-entry 0xAARRGGBB palette of a P8 (0x0B)
     * texture, from SET_TEXTURE_PALETTE; 0 for every other format. */
    uint32_t palette;
} Nv2aTexture;

typedef struct {
    float    x, y, z;           /* surface pixels; z as the title produced it */
    float    rhw;               /* 1/w, 1 for pre-transformed batches */
    uint32_t diffuse;           /* 0xAARRGGBB */
    float    u, v;              /* texels */
    /* The fog unit's factor for this vertex, NOT clamped (the NV2A clamps
     * after interpolating): 1 = no fog, 0 = all fog colour. 1 when fog is
     * off. Applied only when Nv2aRenderState.fog_enable says so. */
    float    fog;
} Nv2aVertex;

/* Render state as the title set it, in NV2A's own (OpenGL) enum values:
 * blend factors GL_ZERO/GL_ONE/0x300..0x308/0x8001..0x8004, equations
 * GL_FUNC_ADD 0x8006 etc., compare functions GL_NEVER 0x200 .. GL_ALWAYS 0x207.
 * color_mask uses NV2A's bytes: A 0x01000000, R 0x00010000, G 0x100, B 0x1. */
typedef struct {
    uint32_t blend_enable, blend_src, blend_dst, blend_eq, blend_color;
    uint32_t alpha_test_enable, alpha_func, alpha_ref;   /* ref 0..255 */
    uint32_t depth_test_enable, depth_func, depth_write;
    uint32_t color_mask;
    uint32_t cull_enable, cull_face, front_face;
    uint32_t zeta_va;                                    /* 0: no depth surface */
    float    depth_min, depth_max;                       /* SET_CLIP_MIN/MAX */
    /* SET_STENCIL_*: compare function as above; ops GL_KEEP 0x1E00, GL_ZERO 0,
     * GL_REPLACE 0x1E01, GL_INCR 0x1E02, GL_DECR 0x1E03, GL_INVERT 0x150A,
     * GL_INCR_WRAP 0x8507, GL_DECR_WRAP 0x8508. Reset values GL_ALWAYS, masks
     * 0xFF, GL_KEEP. A title can confine a pass to the pixels an earlier pass
     * marked (a full-screen quad that only marked pixels pass); a back end
     * that ignores these draws that pass over the whole surface. */
    uint32_t stencil_enable, stencil_func, stencil_ref, stencil_func_mask;
    uint32_t stencil_write_mask, stencil_fail, stencil_zfail, stencil_zpass;
    /* Fog: on when SET_FOG_ENABLE is set and the final combiner reads the
     * fog register -- D3D's fog: colour = mix(fog_color, colour, factor),
     * the factor clamped to 0..1 per pixel; alpha untouched. fog_color is
     * 0xAARRGGBB (the method's word has R in the low byte). */
    uint32_t fog_enable, fog_color;
    /* SET_ZMIN_MAX_CONTROL: 1 = depth outside the clip range is clamped into
     * it; 0 = what lies beyond the near/far planes is culled (D3D's setting:
     * CULL_NEAR_FAR_EN with ZCLAMP_EN = CULL), as GL/Vulkan clip it. */
    uint32_t zclamp;
} Nv2aRenderState;

typedef struct {
    const Nv2aVertex  *vertices;    /* triangle list: count is a multiple of 3 */
    uint32_t           count;
    const Nv2aTexture *texture;     /* NULL: untextured */
    const Nv2aRenderState *state;
    /* Where the batch came from -- a hash of its position stream's address
     * and stride, the primitive and its first/last index and count -- so a
     * back end can find the same draw in the next frame (frame
     * interpolation). Never 0. Inline vertex data hashes as one address, so
     * those batches only differ by count and index. */
    uint32_t           key;
    /* Back ends with NV2A_BACKEND_INDEXED only: when not NULL, the batch is
     * the triangle list indices[0..nindices) (a multiple of 3) into
     * vertices[0..count), count <= 65536 -- each vertex once, however many
     * triangles share it. NULL: vertices is the triangle list itself. */
    const uint16_t    *indices;
    uint32_t           nindices;
} Nv2aBatch;

/* Nv2aBackend.flags */
/* Vertices arrive in homogeneous surface space: x, y, z are the surface
 * pixel position and depth multiplied by w, and the rhw field holds w
 * itself (1 for pre-transformed batches). Nothing is clipped or dropped:
 * triangles behind the eye, degenerate ones and those crossing the near
 * plane all arrive, so a draw has the same vertex count every frame and
 * positions are linear in clip space. The back end clips (w > 0). */
#define NV2A_BACKEND_HOMOGENEOUS 0x1u
/* With HOMOGENEOUS: vertex-program and fixed-function batches may arrive
 * indexed (Nv2aBatch.indices). A vertex's position in the batch is the same
 * every frame, so a draw keeps its vertex count and order. */
#define NV2A_BACKEND_INDEXED 0x2u

typedef struct {
    /* flags: CLEAR_SURFACE bits (Z 0x1, stencil 0x2, colour 0xF0).
     * zstencil: SET_ZSTENCIL_CLEAR_VALUE (depth in the top 24 bits for Z24S8). */
    void (*clear)(const Nv2aSurface *s, const Nv2aRenderState *rs,
                  uint32_t flags, uint32_t argb, uint32_t zstencil);
    void (*draw)(const Nv2aSurface *s, const Nv2aBatch *b);
    void (*flip)(void);
    uint32_t flags;                 /* NV2A_BACKEND_* */
} Nv2aBackend;

/* Register (or, with NULL, remove) the back end. Call before the title starts
 * submitting work; the executor must also be enabled (RECOMP_PB_EXEC). */
void nv2a_backend_register(const Nv2aBackend *backend);

/* Decode a whole texture to 0xAARRGGBB, row-major, width*height entries.
 * Handles every format the executor can sample (swizzled, linear, DXT).
 * Returns 0 if the format is not supported. */
int nv2a_backend_decode_texture(const Nv2aTexture *tex, uint32_t *argb_out);

/* Where BACK_END_WRITE_SEMAPHORE_RELEASE (0x1D70) values land: the guest VA
 * of the semaphore the title reads GPU progress from (for XDK D3D, the
 * pointer at device+0x30). The executor writes each release there once it
 * has executed everything before it -- which is
 * what lets D3D's fence waits and ring-space checks see real progress.
 *
 * ponytail: the title supplies the address instead of the executor resolving
 * the semaphore context DMA object through RAMIN. */
void nv2a_pb_set_semaphore_target(uint32_t guest_va);

/* Frame capture (a host's screenshot key): from the next flip to the one
 * after it, every clear and batch is written to <prefix>.txt with the state
 * it was drawn with (texture stages, combiners, fog, blend, depth, where it
 * landed). At the flip that ends it, nv2a_capture_ending() returns the
 * prefix, so the back end can save the picture as <prefix>.png. Any thread. */
void nv2a_capture_request(const char *prefix);
const char *nv2a_capture_ending(void);

#endif /* NV2A_BACKEND_H */
