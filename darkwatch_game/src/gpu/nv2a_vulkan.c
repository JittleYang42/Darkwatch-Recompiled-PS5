/*
 * nv2a_vulkan.c -- a Vulkan render back end for the xboxrecomp pushbuffer
 * executor (src/kernel/nv2a_backend.h).
 *
 * The executor decodes what the title asks the NV2A to do and hands it over as
 * screen-space triangles with a render state and a stage-0 texture. This file
 * rasterises them with Vulkan instead of the executor's CPU rasteriser.
 *
 * Coherence model: guest memory stays the truth. Every surface (colour buffer
 * or render target) is a Vulkan image that is loaded from guest memory the
 * first time it is drawn to in a frame and written back to guest memory at
 * flip. So everything that reads guest memory keeps working unchanged: the
 * toolkit's framebuffer window and RECOMP_FB_DUMP, render-to-texture (a
 * texture that overlaps a surface drawn this frame forces a write-back
 * before it is decoded), and CPU writes the title makes into a surface.
 * ponytail: one readback per surface per frame. Cheap at 640x480; a later
 * version keeps surfaces on the GPU and presents directly.
 *
 * Vulkan 1.3 core only (dynamic rendering, extended dynamic state), so it
 * runs on RADV and on SwiftShader alike. Function pointers come from vkGetInstanceProcAddr,
 * which Linux gets from libvulkan.so.1 and a PS5 build from the RADV archive
 * it links -- see dwvk_register / dwvk_register_with.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#if defined(_WIN32)
#include <windows.h>
#elif !defined(__PROSPERO__)
#include <dlfcn.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <pthread.h>
#include <unistd.h>

#include "nv2a_backend.h"
#include "shaders/nv2a_vert.spv.h"
#include "shaders/nv2a_frag.spv.h"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "../../third_party/stb/stb_image_write.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

extern ptrdiff_t xbox_GetMemoryOffset(void);

/* ── function table ─────────────────────────────────────────────── */

#define DW_VK_INSTANCE_FNS(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) \
    X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFormatProperties) \
    X(vkGetPhysicalDeviceFeatures2) X(vkEnumerateDeviceExtensionProperties) \
    X(vkCreateDevice) X(vkGetDeviceProcAddr)

#define DW_VK_DEVICE_FNS(X) \
    X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkCreateCommandPool) \
    X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
    X(vkResetCommandBuffer) X(vkQueueSubmit) X(vkCreateFence) X(vkWaitForFences) \
    X(vkResetFences) X(vkCreateBuffer) X(vkDestroyBuffer) \
    X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) \
    X(vkBindBufferMemory) X(vkMapMemory) X(vkCreateImage) X(vkDestroyImage) \
    X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateImageView) \
    X(vkDestroyImageView) X(vkCreateSampler) X(vkCreateShaderModule) \
    X(vkCreatePipelineLayout) X(vkCreateDescriptorSetLayout) \
    X(vkCreateGraphicsPipelines) \
    X(vkCmdBindPipeline) X(vkCmdBindVertexBuffers) X(vkCmdDraw) \
    X(vkCmdBindIndexBuffer) X(vkCmdDrawIndexed) \
    X(vkCmdPushConstants) X(vkCmdBindDescriptorSets) X(vkCmdSetViewport) \
    X(vkCmdSetScissor) \
    X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) \
    X(vkCmdSetStencilReference) \
    X(vkCmdSetBlendConstants) X(vkCmdClearAttachments) X(vkCmdPipelineBarrier) \
    X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdBlitImage) \
    X(vkCreateDescriptorPool) X(vkResetDescriptorPool) \
    X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets)

#define DW_DECL(f) static PFN_##f f;
DW_VK_INSTANCE_FNS(DW_DECL)
DW_VK_DEVICE_FNS(DW_DECL)
static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr_;
/* Core in 1.3, VK_KHR_dynamic_rendering on 1.2 (WSL's D3D12-backed dzn). */
static PFN_vkCmdBeginRendering vkCmdBeginRendering;
static PFN_vkCmdEndRendering vkCmdEndRendering;
static PFN_vkCreateInstance vkCreateInstance;

/* Presenting to a display of our own (the PS5: VK_KHR_display), loaded only
 * when a surface provider is set (dwvk_set_present). */
#define DW_VK_PRESENT_INSTANCE_FNS(X) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR)
#define DW_VK_PRESENT_DEVICE_FNS(X) \
    X(vkCreateSwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) \
    X(vkQueuePresentKHR) X(vkCreateSemaphore) X(vkCmdClearColorImage) \
    X(vkDestroySwapchainKHR) X(vkQueueWaitIdle)
DW_VK_PRESENT_INSTANCE_FNS(DW_DECL)
DW_VK_PRESENT_DEVICE_FNS(DW_DECL)

typedef VkSurfaceKHR (*dwvk_surface_fn)(VkInstance inst, PFN_vkGetInstanceProcAddr gipa,
                                        VkPhysicalDevice phys, VkExtent2D *extent);
static struct {
    const char *const *inst_exts;
    uint32_t n_inst_exts;
    dwvk_surface_fn make_surface;
} s_present_cfg;

/* ── state ──────────────────────────────────────────────────────── */

#define MAX_TARGETS   32
#define MAX_TEXTURES  512
#define MAX_PIPELINES 512
#define STAGING_BYTES (48u << 20)
#define VERTEX_BYTES  (16u << 20)
#define INDEX_BYTES   (8u << 20)
#define DW_DESC_SETS  8192

typedef struct {
    VkImage image; VkDeviceMemory mem; VkImageView view;
    VkImageLayout layout;
} Img;

typedef struct {
    int used;
    uint32_t va, w, h, pitch, bpp;      /* the guest surface */
    uint32_t scale;                     /* img is w*scale x h*scale */
    Img img;
    Img img1x;                          /* guest-sized: loads in, write-backs out */
    uint32_t frame;                     /* last frame drawn to */
    int loaded;                         /* loaded from guest this frame */
    /* With scale > 1: a fingerprint of the console-sized picture last
     * written back. While guest memory still holds exactly that, img is the
     * same picture at full detail and a reload (which could only bring back
     * a blurred upscale of it) is skipped. */
    uint64_t wb_sum;
    int wb_valid;
} Target;

typedef struct {
    int used;
    uint32_t zeta, w, h;                /* the guest surface */
    uint32_t scale;
    Img img;
    uint32_t frame;
} Depth;

typedef struct {
    int used;
    uint32_t offset, color, w, h, pitch;
    uint64_t sum;
    Img img;
    uint32_t frame;
    uint32_t sum_walk, sum_flush;       /* when sum was last taken */
} Tex;

typedef struct {
    uint32_t key[12];                   /* blend, mask, format, depth/stencil */
    VkPipeline pipe;
} Pipe;

typedef struct {
    VkBuffer buf; VkDeviceMemory mem; uint8_t *ptr; uint32_t size, used;
    uint32_t reserved;                  /* promised to write-backs at flush */
} Ring;

static struct {
    int tried, ok;
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue queue;
    uint32_t qfam;
    VkPhysicalDeviceMemoryProperties memprops;
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;
    VkFormat depth_fmt;
    VkDescriptorSetLayout dsl;
    VkDescriptorPool dpool;
    uint32_t dsets;                     /* allocated since the last reset */
    VkImageView bound_view;
    VkSampler bound_sampler;
    VkPipelineLayout layout;
    VkShaderModule vs, fs;
    VkSampler samplers[64];
    Ring staging, verts, idx;           /* idx: indexed batches' triangle corners */
    Img white;

    Target targets[MAX_TARGETS];
    Depth depths[MAX_TARGETS];
    Tex textures[MAX_TEXTURES];
    Pipe pipes[MAX_PIPELINES];
    int npipes;

    int recording;
    Target *rt;                         /* rendering into */
    Depth *rd;
    int rendering;
    VkPipeline bound;
    uint32_t frame;

    /* write-backs queued for the current submission */
    struct { Target *t; uint32_t off; } wb[MAX_TARGETS];
    int nwb;

    uint32_t *decode_buf;
    size_t decode_cap;
    unsigned stats_draws, stats_tris, stats_uploads, stats_flushes, stats_sum_skipped;
    unsigned stats_fogged;
    unsigned stats_kept, stats_reloads; /* scaled targets: reload skipped / done */
    Ring dump;                          /* DW_HIRES_DUMP read-backs */
    /* Where the executor thread's time goes, in ns, since the last report. */
    uint64_t t_decode, t_sum, t_flush, t_draw, t_clear, t_frame0;
    uint64_t t_fl_submit, t_fl_wait, t_fl_wb;    /* inside t_flush: submit, GPU wait, copy-back */
    /* draw-record split (TSC ticks, flushes taken out): textures, surface,
     * clip+copy, pipeline, descriptors, the rest; batches seen. */
    uint64_t k_tex, k_surf, k_clip, k_pipe, k_desc, k_cmd, k_cal_tsc, k_cal_ns;
    double k_tpns;
    unsigned n_batches;
    unsigned frames_since;

    /* Our own display (dwvk_set_present). */
    int present;
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkExtent2D sc_extent;
    VkFormat sc_format;
    VkImage sc_images[8];
    uint32_t sc_count;
    VkSemaphore sem_acquire, sem_done;
    VkCommandBuffer pcmd;
    VkFence pfence;
    Img src[2];                         /* the frame from guest memory: 16 / 32 bpp */
    unsigned presents;
    int headless;                       /* DW_HEADLESS_PRESENT: flips present from here */
    VkPresentModeKHR sc_mode;
    VkCompositeAlphaFlagBitsKHR sc_alpha;
    uint32_t sc_min_images;
} V;

/* Display options a desktop host sets (dwvk_set_display_options): V-Sync,
 * a frame-rate cap for the interpolating presenter (0 = none), and how the
 * 4:3 picture fills the window. A window resize marks the swapchain stale
 * (dwvk_window_resized); whichever thread presents rebuilds it. */
static volatile int s_vsync = 1;
static volatile int s_fps_cap;
static volatile int s_aspect;          /* 0: 4:3 with bars, 1: stretch to fill */
static volatile int s_sc_dirty;
static volatile unsigned s_display_frames;     /* presents to the display */
/* The host's overlay and the lock over every acquire..present (see
 * dwvk_overlay_present). */
static pthread_mutex_t s_present_mu = PTHREAD_MUTEX_INITIALIZER;
static struct {
    volatile int on;
    pthread_mutex_t mu;
    uint32_t *px;
    int w, h, cap;
    VkBuffer buf; VkDeviceMemory mem; uint8_t *ptr; uint32_t size;
    Img img; int iw, ih;
    VkCommandPool pool; VkCommandBuffer cmd; VkFence fence;
} O = { .mu = PTHREAD_MUTEX_INITIALIZER };
static int sc_rebuild(void);

/* Frame interpolation's state (see "frame interpolation" below). */
typedef struct {
    uint8_t kind;                       /* 0 draw, 1 clear */
    uint8_t nclear;
    Target *t;                          /* the surface it went to */
    VkClearAttachment at[2];
    VkPipeline pipe;
    VkImageView view;
    VkSampler smp;
    float pc[16];                       /* push constants: vp, tex, alpha, fog */
    uint32_t smask, wmask, sref;
    float bc[4];
    uint32_t key;                       /* the executor's batch key */
    uint32_t tex_va, tex_bytes;
    uint32_t first, count;              /* homogeneous vertices in the list's store */
    /* An indexed draw: icount triangle corners at ifirst in the list's index
     * store, each indexing the draw's own count vertices. 0: the vertices
     * are a triangle list. */
    uint32_t ifirst, icount;
    int32_t match;                      /* the same draw in the previous frame, or -1 */
} Rec;

typedef struct {
    Rec *rec;
    uint32_t nrec, caprec;
    Nv2aVertex *vtx;
    uint32_t nvtx, capvtx;
    uint16_t *idx;
    uint32_t nidx, capidx;
} RecList;

#define DW_SLOTS 4

typedef struct {
    RecList list;
    Img real;                           /* the frame as drawn */
    uint32_t rw, rh, bpp;               /* real's size; replays render at it too */
    uint32_t frame;                     /* V.frame it was drawn in */
    uint32_t gen;                       /* bumped at each publish into the slot */
    uint64_t t_ns;                      /* when it was published */
    int prev;                           /* slot published just before it, or -1 */
    int replayable;                     /* records reproduce real */
    unsigned matched, draws;
} Slot;

static struct {
    int on;                             /* -1: not decided yet */
    const char *test;                   /* DW_INTERP_TEST=<prefix>: offscreen, dumps */
    RecList build, done;                /* executor: this frame's records, the last one's */
    uint32_t done_frame;
    int done_ok;                        /* done is a finished frame not published yet */
    Slot slot[DW_SLOTS];
    pthread_mutex_t mu;                 /* the fields below to `thread` */
    pthread_cond_t cv;
    int latest;                         /* newest published slot, -1 none */
    int pin[2];                         /* slots the presenter is reading */
    int busy;                           /* presenter replaying: it uses texture views */
    uint32_t no_interp_before;          /* older frames are shown as drawn */
    uint64_t interval_ns;               /* smoothed time between published frames */
    pthread_t thread;
    int started, inited;
    pthread_mutex_t qmu;                /* vkQueueSubmit / vkQueuePresentKHR */
    /* executor side */
    VkCommandBuffer xcmd;
    VkFence xfence;
    int xpending;                       /* xcmd submitted, not yet waited for */
    /* presenter side */
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;
    VkDescriptorPool dpool;
    Ring verts, idx, rb;
    Img img, depth;                     /* what replays draw into */
    uint32_t iw, ih, ibpp;
    uint8_t *mode;                      /* per record of the current pair (pair_prepare) */
    int32_t *mi;                        /* its partner in the previous frame, or -1 */
    uint32_t capok;
    uint32_t ok_gen_cur, ok_gen_prev;
    int pair_valid;                     /* mode/mi describe (ok_gen_prev, ok_gen_cur) */
    uint32_t gen_next;                  /* publish counter: a slot's gen is unique */
    int ok_cut;                         /* the pair is a cut: no blending at all */
    double M[16];                       /* camera motion: previous = M * newest (normalised) */
    int mvalid;
    uint32_t *ht_key, *ht_cnt;          /* pair_prepare's tables */
    int32_t *ht_idx, *next;
    uint32_t cap_ht, cap_next;
    /* stats, presenter side */
    unsigned st_frames, st_replayed, st_blended, st_real, st_cuts;
    unsigned st_pairs_draws, st_pairs_ok, st_reproj, st_fits, st_fit_tries, st_world;
    unsigned st_bad_pair, st_bad_reproj;
    double st_fid;
    unsigned st_fid_n;
    uint64_t st_t0, st_present_ns, st_present_max, st_cpu_ns;
} P = { .on = -1, .latest = -1, .pin = { -1, -1 } };

static int interp_on(void);
static uint32_t interp_keep_frame(void);
static void interp_quiesce(void);

/* DW_RES_SCALE: draw the 3D at this multiple of the console's resolution.
 *
 * Darkwatch renders 640x480, which a 1080p display has to stretch 2.25x. The
 * executor hands this back end triangles already in window space, and the
 * shader turns those into NDC with the *guest* surface size, so a colour and
 * depth image N times larger with a viewport to match rasterises the same
 * geometry at N times the sample rate: no guest state changes, and nothing
 * the title can read is different. Guest memory keeps the console-sized
 * picture (write-backs scale back down), so render-to-texture, CPU reads and
 * the title's own blits see exactly what they saw before; only the frame on
 * screen, which is presented from the large image, is sharper.
 *
 * 1 is the console's own resolution and the default. Memory and fill cost
 * grow with the square, so 4 is the ceiling.
 */
static uint32_t res_scale(void)
{
    static uint32_t s;
    if (!s) {
        const char *e = getenv("DW_RES_SCALE");
        long v = e ? strtol(e, NULL, 10) : 1;
        s = (uint32_t)(v < 1 ? 1 : v > 4 ? 4 : v);
        if (s > 1)
            fprintf(stderr, "[VK] internal resolution: %ux\n", s);
    }
    return s;
}

/* A fingerprint of h rows of row bytes, stride apart: four independent
 * multiply-xor lanes over 8-byte words (about 0.2 ms for a 640x480x4
 * surface). Only ever compared with itself, so any decent mix will do. */
static uint64_t rows_sum(const uint8_t *p, uint32_t row, uint32_t h, uint32_t stride)
{
    uint64_t a = 0x9E3779B97F4A7C15ull, b = 0xC2B2AE3D27D4EB4Full,
             c = 0x165667B19E3779F9ull, d = 0x27D4EB2F165667C5ull;
    const uint64_t K = 0xFF51AFD7ED558CCDull;
    uint32_t y, i;

    for (y = 0; y < h; y++, p += stride) {
        const uint8_t *q = p;
        uint32_t n = row;
        for (; n >= 32; n -= 32, q += 32) {
            uint64_t w[4];
            memcpy(w, q, 32);
            a = (a ^ w[0]) * K; b = (b ^ w[1]) * K;
            c = (c ^ w[2]) * K; d = (d ^ w[3]) * K;
        }
        for (i = 0; i < n; i++)
            a = (a ^ q[i]) * K;
        a ^= a >> 29; b ^= b >> 31; c ^= c >> 27; d ^= d >> 33;
    }
    return a ^ (b * 3) ^ (c * 5) ^ (d * 7);
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static inline uint64_t tsc_now(void)
{
#if defined(__x86_64__) || defined(__i386__)
    return __builtin_ia32_rdtsc();
#else
    return now_ns();
#endif
}

/* A sleep of about us microseconds (the runtime's high-resolution nap). */
extern void xbox_HostNapUs(unsigned us);
static void dw_nap_us(unsigned us)
{
    xbox_HostNapUs(us);
}

static void vk_log(const char *fmt, const char *a)
{
    fprintf(stderr, "[VK] ");
    fprintf(stderr, fmt, a);
    fprintf(stderr, "\n");
    fflush(stderr);
}

/* ── memory and resources ───────────────────────────────────────── */

static int mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < V.memprops.memoryTypeCount; i++)
        if ((bits & (1u << i))
            && (V.memprops.memoryTypes[i].propertyFlags & want) == want)
            return (int)i;
    return -1;
}

static int ring_init(Ring *r, uint32_t size, VkBufferUsageFlags usage)
{
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryRequirements mr;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    int t;

    bi.size = size;
    bi.usage = usage;
    if (vkCreateBuffer(V.dev, &bi, NULL, &r->buf) != VK_SUCCESS)
        return 0;
    vkGetBufferMemoryRequirements(V.dev, r->buf, &mr);
    /* A ring the GPU writes into is read back by the CPU (surface
     * write-backs): CPU-cached memory, where there is any. Write-combined
     * memory reads at a fraction of the speed. */
    t = (usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT)
        ? mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                      | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                      | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
        : -1;
    if (t < 0)
        t = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                        | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (t < 0)
        return 0;
    fprintf(stderr, "[VK] ring of %u KB (usage %X): memory type %d, flags %X\n", size >> 10,
            (unsigned)usage, t, (unsigned)V.memprops.memoryTypes[t].propertyFlags);
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = (uint32_t)t;
    if (vkAllocateMemory(V.dev, &ai, NULL, &r->mem) != VK_SUCCESS
        || vkBindBufferMemory(V.dev, r->buf, r->mem, 0) != VK_SUCCESS
        || vkMapMemory(V.dev, r->mem, 0, VK_WHOLE_SIZE, 0, (void **)&r->ptr) != VK_SUCCESS)
        return 0;
    r->size = size;
    r->used = 0;
    return 1;
}

/* Space in a ring for this submission, or UINT32_MAX when it is full. What is
 * reserved stays free for the write-backs that flush() allocates. */
static uint32_t ring_alloc(Ring *r, uint32_t bytes, uint32_t align)
{
    uint32_t off = (r->used + align - 1) & ~(align - 1);
    if ((uint64_t)off + bytes + r->reserved > r->size)
        return UINT32_MAX;
    r->used = off + bytes;
    return off;
}

static int img_create(Img *im, uint32_t w, uint32_t h, VkFormat fmt,
                      VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkMemoryRequirements mr;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    int t;

    memset(im, 0, sizeof *im);
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent.width = w;
    ci.extent.height = h;
    ci.extent.depth = 1;
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(V.dev, &ci, NULL, &im->image) != VK_SUCCESS)
        return 0;
    vkGetImageMemoryRequirements(V.dev, im->image, &mr);
    t = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (t < 0)
        t = mem_type(mr.memoryTypeBits, 0);
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = (uint32_t)t;
    if (vkAllocateMemory(V.dev, &ai, NULL, &im->mem) != VK_SUCCESS
        || vkBindImageMemory(V.dev, im->image, im->mem, 0) != VK_SUCCESS)
        return 0;
    vi.image = im->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange.aspectMask = aspect;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    if (vkCreateImageView(V.dev, &vi, NULL, &im->view) != VK_SUCCESS)
        return 0;
    im->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return 1;
}

static void img_destroy(Img *im)
{
    if (im->view) vkDestroyImageView(V.dev, im->view, NULL);
    if (im->image) vkDestroyImage(V.dev, im->image, NULL);
    if (im->mem) vkFreeMemory(V.dev, im->mem, NULL);
    memset(im, 0, sizeof *im);
}

/* One layout transition with a full barrier: correct, not minimal. */
static void img_layout_cmd(VkCommandBuffer cmd, Img *im, VkImageLayout to,
                           VkImageAspectFlags aspect);
static void img_layout(Img *im, VkImageLayout to, VkImageAspectFlags aspect)
{
    img_layout_cmd(V.cmd, im, to, aspect);
}

static void img_layout_cmd(VkCommandBuffer cmd, Img *im, VkImageLayout to,
                           VkImageAspectFlags aspect)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };

    if (im->layout == to)
        return;
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = im->layout;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im->image;
    b.subresourceRange.aspectMask = aspect;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                         0, NULL, 0, NULL, 1, &b);
    im->layout = to;
}

/* A filtered copy between two images of different sizes, in the recording
 * command buffer. Both come back as TRANSFER_SRC/DST, which is where the
 * callers want them. */
static void img_blit(Img *src, uint32_t sw, uint32_t sh,
                     Img *dst, uint32_t dw, uint32_t dh)
{
    VkImageBlit bl;

    img_layout(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    img_layout(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    memset(&bl, 0, sizeof bl);
    bl.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bl.srcSubresource.layerCount = 1;
    bl.srcOffsets[1].x = (int32_t)sw;
    bl.srcOffsets[1].y = (int32_t)sh;
    bl.srcOffsets[1].z = 1;
    bl.dstSubresource = bl.srcSubresource;
    bl.dstOffsets[1].x = (int32_t)dw;
    bl.dstOffsets[1].y = (int32_t)dh;
    bl.dstOffsets[1].z = 1;
    vkCmdBlitImage(V.cmd, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl,
                   VK_FILTER_LINEAR);
}

/* ── frame recording ────────────────────────────────────────────── */

static void begin_recording(void)
{
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };

    if (V.recording)
        return;
    vkResetCommandBuffer(V.cmd, 0);
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(V.cmd, &bi);
    V.recording = 1;
    V.staging.used = 0;
    V.staging.reserved = 0;
    V.verts.used = 0;
    V.idx.used = 0;
    V.nwb = 0;
    V.bound = VK_NULL_HANDLE;
    vkResetDescriptorPool(V.dev, V.dpool, 0);
    V.dsets = 0;
    V.bound_view = VK_NULL_HANDLE;
    V.bound_sampler = VK_NULL_HANDLE;
}

static void end_rendering(void)
{
    if (V.rendering) {
        vkCmdEndRendering(V.cmd);
        V.rendering = 0;
        V.bound = VK_NULL_HANDLE;
    }
}

/* Submit what is recorded, wait, and copy the surfaces drawn to back into
 * guest memory. Used at flip, when a ring is full, and before a texture that
 * overlaps a surface drawn this frame is decoded. */
static void flush_body(void);
static void flush(void)
{
    uint64_t t0 = now_ns();
    flush_body();
    /* Nothing submitted may still be running after a flush: callers free
     * images next. The interpolation copy was submitted before anything
     * flush_body waited for, so this is normally already done. */
    if (P.xpending) {
        vkWaitForFences(V.dev, 1, &P.xfence, VK_TRUE, UINT64_MAX);
        P.xpending = 0;
    }
    V.t_flush += now_ns() - t0;
}

static void flush_body(void)
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    int i;

    if (!V.recording)
        return;
    end_rendering();
    /* Queue the write-backs: every surface drawn since the last flush. */
    for (i = 0; i < MAX_TARGETS; i++) {
        Target *t = &V.targets[i];
        VkBufferImageCopy c;
        uint32_t bytes, off;

        if (!t->used || !t->loaded)
            continue;
        bytes = t->w * t->h * t->bpp;
        V.staging.reserved -= bytes < V.staging.reserved ? bytes : V.staging.reserved;
        off = ring_alloc(&V.staging, bytes, 16);
        if (off == UINT32_MAX)
            continue;                    /* cannot happen: reserved on load */
        if (t->scale > 1)
            img_blit(&t->img, t->w * t->scale, t->h * t->scale,
                     &t->img1x, t->w, t->h);
        {
            Img *from = t->scale > 1 ? &t->img1x : &t->img;
            img_layout(from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_IMAGE_ASPECT_COLOR_BIT);
            memset(&c, 0, sizeof c);
            c.bufferOffset = off;
            c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            c.imageSubresource.layerCount = 1;
            c.imageExtent.width = t->w;
            c.imageExtent.height = t->h;
            c.imageExtent.depth = 1;
            vkCmdCopyImageToBuffer(V.cmd, from->image,
                                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   V.staging.buf, 1, &c);
        }
        V.wb[V.nwb].t = t;
        V.wb[V.nwb].off = off;
        V.nwb++;
    }
    vkEndCommandBuffer(V.cmd);
    si.commandBufferCount = 1;
    si.pCommandBuffers = &V.cmd;
    vkResetFences(V.dev, 1, &V.fence);
    {
        uint64_t a = now_ns(), b, c;
        pthread_mutex_lock(&P.qmu);
        vkQueueSubmit(V.queue, 1, &si, V.fence);
        pthread_mutex_unlock(&P.qmu);
        b = now_ns();
        vkWaitForFences(V.dev, 1, &V.fence, VK_TRUE, UINT64_MAX);
        c = now_ns();
        V.t_fl_submit += b - a;
        V.t_fl_wait += c - b;
    }
    V.recording = 0;

    {
    uint64_t twb = now_ns();
    for (i = 0; i < V.nwb; i++) {
        Target *t = V.wb[i].t;
        const uint8_t *src = V.staging.ptr + V.wb[i].off;
        uint32_t row = t->w * t->bpp, y;
        for (y = 0; y < t->h; y++)
            memcpy(mem + t->va + (size_t)y * t->pitch, src + (size_t)y * row, row);
        if (t->scale > 1 || interp_on()) {
            t->wb_sum = rows_sum(src, row, t->h, row);
            t->wb_valid = 1;
        }
        t->loaded = 0;                  /* reload from guest next time */
    }
    V.t_fl_wb += now_ns() - twb;
    }
    V.nwb = 0;
    V.rt = NULL;
    V.rd = NULL;
    V.stats_flushes++;
}

/* ── surfaces ───────────────────────────────────────────────────── */

/* Can [va, va+bytes) be read and written through the guest mapping? RAM, the
 * contiguous window at 0x80000000 and its tiled alias at 0xF0000000, each
 * 64 MB. A surface or texture outside them is the title pointing somewhere
 * this runtime does not back; the CPU rasteriser refuses those too. */
static int guest_range_ok(uint32_t va, uint32_t bytes)
{
    uint64_t end = (uint64_t)va + bytes;
    return end <= 0x04000000ull
        || (va >= 0x80000000u && end <= 0x84000000ull)
        || (va >= 0xF0000000u && end <= 0xF4000000ull);
}

static VkFormat color_format(uint32_t bpp)
{
    return bpp == 2 ? VK_FORMAT_R5G6B5_UNORM_PACK16 : VK_FORMAT_B8G8R8A8_UNORM;
}

static Target *get_target(const Nv2aSurface *s)
{
    Target *free_slot = NULL, *oldest = NULL;
    uint32_t sc;
    int i;

    for (i = 0; i < MAX_TARGETS; i++) {
        Target *t = &V.targets[i];
        if (t->used && t->va == s->color_va && t->w == s->width
            && t->h == s->height && t->pitch == s->pitch
            && t->bpp == s->bytes_per_pixel)
            return t;
        if (!t->used && !free_slot)
            free_slot = t;
        if (t->used && !t->loaded && (!oldest || t->frame < oldest->frame))
            oldest = t;
    }
    if (!free_slot) {
        if (!oldest)
            return NULL;
        flush();                        /* nothing may still use it */
        img_destroy(&oldest->img);
        img_destroy(&oldest->img1x);
        oldest->used = 0;
        free_slot = oldest;
    }
    memset(free_slot, 0, sizeof *free_slot);
    sc = res_scale();
    if (!img_create(&free_slot->img, s->width * sc, s->height * sc,
                    color_format(s->bytes_per_pixel),
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                    | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                    | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT))
        return NULL;
    /* Guest memory is read and written at the console's size; the large image
     * is only ever reached through a filtered blit against this one. */
    if (sc > 1 && !img_create(&free_slot->img1x, s->width, s->height,
                              color_format(s->bytes_per_pixel),
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                              | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                              VK_IMAGE_ASPECT_COLOR_BIT)) {
        img_destroy(&free_slot->img);
        return NULL;
    }
    free_slot->used = 1;
    free_slot->va = s->color_va;
    free_slot->w = s->width;
    free_slot->h = s->height;
    free_slot->scale = sc;
    free_slot->pitch = s->pitch;
    free_slot->bpp = s->bytes_per_pixel;
    return free_slot;
}

static Depth *get_depth(uint32_t zeta, uint32_t w, uint32_t h)
{
    Depth *free_slot = NULL;
    int i;

    for (i = 0; i < MAX_TARGETS; i++) {
        Depth *d = &V.depths[i];
        if (d->used && d->zeta == zeta && d->w == w && d->h == h)
            return d;
        if (!d->used && !free_slot)
            free_slot = d;
    }
    if (!free_slot) {
        /* Evict the least recently used; flush so nothing still uses it. */
        Depth *oldest = &V.depths[0];
        for (i = 1; i < MAX_TARGETS; i++)
            if (V.depths[i].frame < oldest->frame)
                oldest = &V.depths[i];
        flush();
        img_destroy(&oldest->img);
        oldest->used = 0;
        free_slot = oldest;
    }
    memset(free_slot, 0, sizeof *free_slot);
    /* Depth follows the colour target's size, or the attachments disagree. */
    free_slot->scale = res_scale();
    if (!img_create(&free_slot->img, w * free_slot->scale, h * free_slot->scale,
                    V.depth_fmt,
                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
        return NULL;
    free_slot->used = 1;
    free_slot->zeta = zeta;
    free_slot->w = w;
    free_slot->h = h;
    return free_slot;
}

/* Make `s` the surface being rendered into, loading it from guest memory if
 * this is its first use since the last flush. Returns 0 if it cannot be. */
static int bind_surface(const Nv2aSurface *s, const Nv2aRenderState *rs)
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    Target *t;
    Depth *d;
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };

    if (!s->color_va || !s->width || !s->height || !s->pitch
        || (s->bytes_per_pixel != 2 && s->bytes_per_pixel != 4)
        || s->width * s->bytes_per_pixel > s->pitch)
        return 0;
    if (!guest_range_ok(s->color_va, s->pitch * s->height)) {
        static unsigned n;
        if (n++ < 8)
            fprintf(stderr, "[VK] surface 0x%08X (%ux%u pitch %u) outside guest "
                    "memory; skipped\n", s->color_va, s->width, s->height, s->pitch);
        return 0;
    }
    begin_recording();
    t = get_target(s);
    if (!t)
        return 0;
    /* zeta 0 = no depth surface on the console; keep a scratch one so every
     * pipeline has the same attachments, with depth/stencil tests off. */
    d = get_depth(rs ? rs->zeta_va : 0, s->width, s->height);
    if (!d)
        return 0;
    begin_recording();                  /* get_* may have flushed */
    if (V.rendering && V.rt == t && V.rd == d)
        return 1;
    end_rendering();

    if (!t->loaded) {
        uint32_t row = t->w * t->bpp, bytes = row * t->h, y, off;
        VkBufferImageCopy c;

        /* Room for this load and its write-back at flush. */
        if ((uint64_t)V.staging.used + V.staging.reserved + 2ull * bytes + 64
                > V.staging.size) {
            if (!V.staging.used && !V.staging.reserved)
                return 0;                /* larger than the ring itself */
            flush();
            return bind_surface(s, rs);  /* afresh, on an empty ring */
        }
        /* Scaled, and guest memory still holds what the last flush wrote
         * back: the large image already is this picture, in more detail
         * than an upload could restore. Only the write-back's room is
         * needed. A mid-frame flush (render-to-texture, a full ring) would
         * otherwise blur everything drawn before it. */
        if (t->scale > 1 && t->wb_valid
            && rows_sum(mem + t->va, row, t->h, t->pitch) == t->wb_sum) {
            V.staging.reserved += bytes;
            V.stats_kept++;
            t->loaded = 1;
            goto loaded;
        }
        if (t->scale > 1)
            V.stats_reloads++;
        off = ring_alloc(&V.staging, bytes, 16);
        if (off == UINT32_MAX)
            return 0;                    /* one surface larger than the ring */
        for (y = 0; y < t->h; y++)
            memcpy(V.staging.ptr + off + (size_t)y * row,
                   mem + t->va + (size_t)y * t->pitch, row);
        V.staging.reserved += bytes;    /* its write-back at flush */
        {
            Img *dst = t->scale > 1 ? &t->img1x : &t->img;
            img_layout(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_ASPECT_COLOR_BIT);
            memset(&c, 0, sizeof c);
            c.bufferOffset = off;
            c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            c.imageSubresource.layerCount = 1;
            c.imageExtent.width = t->w;
            c.imageExtent.height = t->h;
            c.imageExtent.depth = 1;
            vkCmdCopyBufferToImage(V.cmd, V.staging.buf, dst->image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            if (t->scale > 1)
                img_blit(&t->img1x, t->w, t->h, &t->img, t->w * t->scale,
                         t->h * t->scale);
        }
        t->loaded = 1;
    }
loaded:
    t->frame = V.frame;
    d->frame = V.frame;
    img_layout(&t->img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_ASPECT_COLOR_BIT);
    img_layout(&d->img, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
               VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);

    ca.imageView = t->img.view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    da.imageView = d->img.view;
    da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ri.renderArea.extent.width = t->w * t->scale;
    ri.renderArea.extent.height = t->h * t->scale;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    ri.pDepthAttachment = &da;
    ri.pStencilAttachment = &da;
    vkCmdBeginRendering(V.cmd, &ri);
    V.rendering = 1;
    V.rt = t;
    V.rd = d;
    V.bound = VK_NULL_HANDLE;
    {
        /* The vertices stay in console window space -- the shader turns
         * them into NDC with the guest size (pc.vp) -- and the viewport
         * spreads that NDC across the whole large image. */
        VkViewport vp = { 0, 0, (float)(t->w * t->scale), (float)(t->h * t->scale),
                          0.0f, 1.0f };
        VkRect2D sc = { { 0, 0 }, { t->w * t->scale, t->h * t->scale } };
        vkCmdSetViewport(V.cmd, 0, 1, &vp);
        vkCmdSetScissor(V.cmd, 0, 1, &sc);
    }
    return 1;
}

/* ── textures ───────────────────────────────────────────────────── */

static uint32_t tex_bytes(const Nv2aTexture *t)
{
    uint32_t w = t->width, h = t->height;
    switch (t->color) {
    case 0x0C: return ((w + 3) / 4) * ((h + 3) / 4) * 8;
    case 0x0E: case 0x0F: return ((w + 3) / 4) * ((h + 3) / 4) * 16;
    }
    if (t->pitch && t->levels <= 1 && t->pitch >= w)
        return t->pitch * h;             /* linear */
    switch (t->color) {
    case 0x00: case 0x01: case 0x0B: case 0x13: case 0x19: case 0x1F:
        return w * h;
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x10: case 0x11:
    case 0x1C: case 0x1D:
        return w * h * 2;
    default:
        return w * h * 4;
    }
}

/* FNV-1a over the texture's bytes: all of them up to 256 KB, otherwise 8192
 * words spread across it plus both ends. A movie frame rewrites the whole
 * texture, so a spread sample sees every change that matters. */
static uint64_t tex_sum(const uint8_t *p, uint32_t bytes)
{
    uint64_t h = 1469598103934665603ull;
    uint32_t i, n = bytes / 4, step;

    if (bytes <= (256u << 10)) {
        /* Four independent lanes over 8-byte words: a quarter of the
         * multiplies of a word-at-a-time FNV and no chain between them (this
         * ran for every textured draw: 3-4 ms a frame in a busy scene). */
        uint64_t a = h, b = 0x9E3779B97F4A7C15ull, c = 0xC2B2AE3D27D4EB4Full,
                 d = 0x165667B19E3779F9ull;
        const uint64_t K = 0xFF51AFD7ED558CCDull;
        uint32_t q = bytes & ~31u;
        for (i = 0; i < q; i += 32) {
            uint64_t w[4];
            memcpy(w, p + i, 32);
            a = (a ^ w[0]) * K; b = (b ^ w[1]) * K;
            c = (c ^ w[2]) * K; d = (d ^ w[3]) * K;
        }
        for (; i + 4 <= bytes; i += 4) {
            uint32_t w;
            memcpy(&w, p + i, 4);
            a = (a ^ w) * K;
        }
        a ^= a >> 29; b ^= b >> 31; c ^= c >> 27; d ^= d >> 33;
        return a ^ (b * 3) ^ (c * 5) ^ (d * 7) ^ bytes;
    }
    step = n / 8192;
    for (i = 0; i < n; i += step)
        h = (h ^ ((const uint32_t *)p)[i]) * 1099511628211ull;
    for (i = 0; i < 256; i++)
        h = (h ^ ((const uint32_t *)p)[n - 1 - i]) * 1099511628211ull;
    return h;
}

/* A texture drawn into this frame (render-to-texture) must reach guest
 * memory before it is decoded from there. */
static void sync_if_rendered(uint32_t offset, uint32_t bytes)
{
    int i;
    for (i = 0; i < MAX_TARGETS; i++) {
        Target *t = &V.targets[i];
        uint32_t end;
        if (!t->used || !t->loaded)
            continue;
        end = t->va + t->pitch * t->h;
        if (offset < end && t->va < offset + bytes) {
            flush();
            return;
        }
    }
}

/* Where each texture was last found, by its guest identity: the search of
 * all MAX_TEXTURES entries ran for every draw. A stale slot is caught by the
 * comparison and the search runs as before. */
/* DW_TEXSUM_ALWAYS=1: checksum every bind, as before (A/B). */
static int getenv_flag_texsum_always(void)
{
    static int v = -1;
    if (v < 0)
        v = getenv("DW_TEXSUM_ALWAYS") != NULL;
    return v;
}

#define TEX_HASH 2048
static int16_t s_tex_hash[TEX_HASH];

static uint32_t tex_hash_slot(const Nv2aTexture *nt)
{
    uint32_t h = nt->offset * 2654435761u ^ (nt->color << 24) ^ (nt->width << 12) ^ nt->height;
    return (h ^ (h >> 15)) & (TEX_HASH - 1);
}

/* The pushbuffer walk's count (nv2a_pb_scan.c). While one walk runs, the
 * title's main thread waits in its kick, so a texture the CPU fills does not
 * change between two draws of one walk; and anything the GPU renders into it
 * reaches guest memory only at a flush. A texture drawn again in the same
 * walk with no flush between needs no new checksum. */
extern uint32_t g_nv2a_walk_seq;

static Tex *get_texture(const Nv2aTexture *nt)
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t bytes = tex_bytes(nt);
    Tex *e = NULL, *free_slot = NULL, *oldest = NULL;
    uint64_t sum;
    size_t texels = (size_t)nt->width * nt->height;
    uint32_t off, y, hs;
    int i;
    VkBufferImageCopy c;

    if (!nt->offset || !nt->width || !nt->height
        || nt->width > 4096 || nt->height > 4096
        || !guest_range_ok(nt->offset, tex_bytes(nt)))
        return NULL;
    sync_if_rendered(nt->offset, bytes);

    hs = tex_hash_slot(nt);
    {
        int k = s_tex_hash[hs] - 1;
        if (k >= 0) {
            Tex *t = &V.textures[k];
            if (t->used && t->offset == nt->offset && t->color == nt->color
                && t->w == nt->width && t->h == nt->height && t->pitch == nt->pitch)
                e = t;
        }
    }
    if (!e)
        for (i = 0; i < MAX_TEXTURES; i++) {
            Tex *t = &V.textures[i];
            if (t->used && t->offset == nt->offset && t->color == nt->color
                && t->w == nt->width && t->h == nt->height && t->pitch == nt->pitch) {
                e = t;
                s_tex_hash[hs] = (int16_t)(i + 1);
                break;
            }
            if (!t->used && !free_slot)
                free_slot = t;
            if (t->used && t->frame != V.frame && (!oldest || t->frame < oldest->frame))
                oldest = t;
        }
    if (e && e->sum_walk == g_nv2a_walk_seq && e->sum_flush == V.stats_flushes
        && !getenv_flag_texsum_always()) {
        e->frame = V.frame;
        V.stats_sum_skipped++;
        return e;
    }
    {
        uint64_t t0 = now_ns();
        sum = tex_sum(mem + nt->offset, bytes);
        V.t_sum += now_ns() - t0;
    }
    /* A P8 texture's colours live in its palette: a new palette is a new
     * texture even when the indices are unchanged. */
    if (nt->color == 0x0B) {
        if (!nt->palette || !guest_range_ok(nt->palette, 1024))
            return NULL;
        sum = sum * 31 + tex_sum(mem + nt->palette, 1024) + nt->palette;
    }
    if (e && e->sum == sum) {
        e->frame = V.frame;
        e->sum_walk = g_nv2a_walk_seq;
        e->sum_flush = V.stats_flushes;
        return e;
    }
    if (!e && !free_slot && !oldest)
        for (i = 0; i < MAX_TEXTURES; i++) {         /* the hash found e before: look now */
            Tex *t = &V.textures[i];
            if (!t->used && !free_slot)
                free_slot = t;
            if (t->used && t->frame != V.frame && (!oldest || t->frame < oldest->frame))
                oldest = t;
        }
    if (!e) {
        if (!free_slot) {
            if (!oldest)
                return NULL;
            flush();
            /* The presenter may still replay a frame that sampled it. */
            if (interp_on() && oldest->frame >= interp_keep_frame())
                interp_quiesce();
            img_destroy(&oldest->img);
            oldest->used = 0;
            free_slot = oldest;
        }
        e = free_slot;
        memset(e, 0, sizeof *e);
        if (!img_create(&e->img, nt->width, nt->height, VK_FORMAT_B8G8R8A8_UNORM,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT))
            return NULL;
        e->used = 1;
        e->offset = nt->offset;
        e->color = nt->color;
        e->w = nt->width;
        e->h = nt->height;
        e->pitch = nt->pitch;
        s_tex_hash[hs] = (int16_t)(e - V.textures + 1);
    }

    if (V.decode_cap < texels) {
        free(V.decode_buf);
        V.decode_buf = malloc(texels * 4);
        V.decode_cap = V.decode_buf ? texels : 0;
        if (!V.decode_buf)
            return NULL;
    }
    {
        uint64_t t0 = now_ns();
        int ok = nv2a_backend_decode_texture(nt, V.decode_buf);
        V.t_decode += now_ns() - t0;
        if (!ok)
            return NULL;                 /* a format the decoder lacks */
    }

    /* Upload: outside a render pass. */
    begin_recording();
    if (V.staging.used + texels * 4 + 64 > V.staging.size) {
        flush();
        begin_recording();
    }
    end_rendering();
    off = ring_alloc(&V.staging, (uint32_t)(texels * 4), 16);
    if (off == UINT32_MAX)
        return NULL;
    /* 0xAARRGGBB in a little-endian word is B, G, R, A in memory. */
    memcpy(V.staging.ptr + off, V.decode_buf, texels * 4);
    img_layout(&e->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    memset(&c, 0, sizeof c);
    c.bufferOffset = off;
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = nt->width;
    c.imageExtent.height = nt->height;
    c.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(V.cmd, V.staging.buf, e->img.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    img_layout(&e->img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    (void)y;
    e->sum = sum;
    e->frame = V.frame;
    /* Taken after any flush the upload caused. */
    e->sum_walk = g_nv2a_walk_seq;
    e->sum_flush = V.stats_flushes;
    V.stats_uploads++;
    return e;
}

static VkSamplerAddressMode addr_mode(uint32_t m)
{
    switch (m) {
    case 2:  return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 3: case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case 4:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

/* NV097 SET_TEXTURE_FILTER: MIN in bits 16-23, MAG in 24-27. 1 is the box
 * (nearest) filter of level 0; the others sample linearly. */
static VkSampler get_sampler(const Nv2aTexture *t)
{
    uint32_t u = t ? (t->addr_u & 7) : 1, v = t ? (t->addr_v & 7) : 1;
    uint32_t mag = t ? (t->filter >> 24) & 0xF : 2;
    uint32_t nearest = (mag == 1);
    uint32_t key = (u & 3) | ((v & 3) << 2) | (nearest << 4);
    VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };

    if (V.samplers[key & 63])
        return V.samplers[key & 63];
    si.magFilter = si.minFilter = nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = addr_mode(u);
    si.addressModeV = addr_mode(v);
    si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.0f;
    si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    vkCreateSampler(V.dev, &si, NULL, &V.samplers[key & 63]);
    return V.samplers[key & 63];
}

/* ── pipelines ──────────────────────────────────────────────────── */

static VkBlendFactor blend_factor(uint32_t f)
{
    switch (f) {
    case 0:      return VK_BLEND_FACTOR_ZERO;
    case 1:      return VK_BLEND_FACTOR_ONE;
    case 0x300:  return VK_BLEND_FACTOR_SRC_COLOR;
    case 0x301:  return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x302:  return VK_BLEND_FACTOR_SRC_ALPHA;
    case 0x303:  return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x304:  return VK_BLEND_FACTOR_DST_ALPHA;
    case 0x305:  return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 0x306:  return VK_BLEND_FACTOR_DST_COLOR;
    case 0x307:  return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0x308:  return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x8001: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x8002: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x8003: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default:     return VK_BLEND_FACTOR_ONE;
    }
}

static VkBlendOp blend_op(uint32_t e)
{
    switch (e) {
    case 0x8007: return VK_BLEND_OP_MIN;
    case 0x8008: return VK_BLEND_OP_MAX;
    case 0x800A: return VK_BLEND_OP_SUBTRACT;
    case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
    default:     return VK_BLEND_OP_ADD;
    }
}

static VkStencilOp stencil_op(uint32_t o);
static VkCompareOp compare_op(uint32_t f);

/* Depth, stencil and cull are part of the pipeline rather than dynamic state:
 * extended dynamic state is not on every 1.2 device this runs on. has_depth is
 * 0 when the title has no depth surface (the scratch one is attached anyway). */
static VkPipeline get_pipeline(const Nv2aRenderState *rs, uint32_t bpp, int has_depth)
{
    uint32_t key[12];
    VkGraphicsPipelineCreateInfo pi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkPipelineShaderStageCreateInfo st[2];
    VkVertexInputBindingDescription vb = { 0, sizeof(Nv2aVertex), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[4] = {
        { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Nv2aVertex, x) },
        { 1, 0, VK_FORMAT_B8G8R8A8_UNORM,      offsetof(Nv2aVertex, diffuse) },
        { 2, 0, VK_FORMAT_R32G32_SFLOAT,       offsetof(Nv2aVertex, u) },
        { 3, 0, VK_FORMAT_R32_SFLOAT,          offsetof(Nv2aVertex, fog) },
    };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo rz = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState ba;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkDynamicState dyn[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
    };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkPipelineRenderingCreateInfo rinfo = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    VkFormat cf = color_format(bpp);
    uint32_t m = rs->color_mask;
    int i;

    key[0] = rs->blend_enable ? 1 : 0;
    key[1] = rs->blend_enable ? rs->blend_src : 0;
    key[2] = rs->blend_enable ? rs->blend_dst : 0;
    key[3] = rs->blend_enable ? rs->blend_eq : 0;
    key[4] = m;
    key[5] = bpp;
    key[6] = (has_depth && rs->depth_test_enable) ? 1 + (rs->depth_func & 0xF) : 0;
    key[7] = has_depth && rs->depth_write;
    key[8] = (has_depth && rs->stencil_enable) ? 1 + (rs->stencil_func & 0xF) : 0;
    key[9] = key[8] ? rs->stencil_fail : 0;
    key[10] = key[8] ? rs->stencil_zfail : 0;
    key[11] = key[8] ? rs->stencil_zpass : 0;
    for (i = 0; i < V.npipes; i++)
        if (!memcmp(V.pipes[i].key, key, sizeof key))
            return V.pipes[i].pipe;
    if (V.npipes == MAX_PIPELINES)
        return V.pipes[0].pipe;          /* ponytail: no eviction yet */

    memset(st, 0, sizeof st);
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = V.vs;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = V.fs;
    st[1].pName = "main";
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = 4;
    vi.pVertexAttributeDescriptions = va;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    vps.viewportCount = 1;
    vps.scissorCount = 1;
    rz.polygonMode = VK_POLYGON_MODE_FILL;
    rz.lineWidth = 1.0f;
    /* ponytail: no culling. The executor hands over screen-space triangles
     * whose winding is in window space with y down, the reverse of the
     * NV2A's convention; drawing both faces is never wrong for a title that
     * culls for speed, only for one that culls to select a face. */
    rz.cullMode = VK_CULL_MODE_NONE;
    rz.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    ds.depthTestEnable = key[6] ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = key[7] ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = compare_op(rs->depth_func);
    ds.stencilTestEnable = key[8] ? VK_TRUE : VK_FALSE;
    ds.front.failOp = stencil_op(rs->stencil_fail);
    ds.front.passOp = stencil_op(rs->stencil_zpass);
    ds.front.depthFailOp = stencil_op(rs->stencil_zfail);
    ds.front.compareOp = compare_op(rs->stencil_func);
    ds.back = ds.front;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    memset(&ba, 0, sizeof ba);
    ba.blendEnable = rs->blend_enable ? VK_TRUE : VK_FALSE;
    ba.srcColorBlendFactor = ba.srcAlphaBlendFactor = blend_factor(rs->blend_src);
    ba.dstColorBlendFactor = ba.dstAlphaBlendFactor = blend_factor(rs->blend_dst);
    ba.colorBlendOp = ba.alphaBlendOp = blend_op(rs->blend_eq);
    ba.colorWriteMask = ((m & 0x00010000u) ? VK_COLOR_COMPONENT_R_BIT : 0)
                      | ((m & 0x00000100u) ? VK_COLOR_COMPONENT_G_BIT : 0)
                      | ((m & 0x00000001u) ? VK_COLOR_COMPONENT_B_BIT : 0)
                      | ((m & 0x01000000u) ? VK_COLOR_COMPONENT_A_BIT : 0);
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;
    dy.dynamicStateCount = (uint32_t)(sizeof dyn / sizeof dyn[0]);
    dy.pDynamicStates = dyn;
    rinfo.colorAttachmentCount = 1;
    rinfo.pColorAttachmentFormats = &cf;
    rinfo.depthAttachmentFormat = V.depth_fmt;
    rinfo.stencilAttachmentFormat = V.depth_fmt;
    pi.pNext = &rinfo;
    pi.stageCount = 2;
    pi.pStages = st;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vps;
    pi.pRasterizationState = &rz;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cb;
    pi.pDynamicState = &dy;
    pi.layout = V.layout;
    if (vkCreateGraphicsPipelines(V.dev, VK_NULL_HANDLE, 1, &pi, NULL,
                                  &V.pipes[V.npipes].pipe) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    memcpy(V.pipes[V.npipes].key, key, sizeof key);
    return V.pipes[V.npipes++].pipe;
}

static VkStencilOp stencil_op(uint32_t o)
{
    switch (o) {
    case 0:      return VK_STENCIL_OP_ZERO;
    case 0x1E01: return VK_STENCIL_OP_REPLACE;
    case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 0x150A: return VK_STENCIL_OP_INVERT;
    case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default:     return VK_STENCIL_OP_KEEP;
    }
}

static VkCompareOp compare_op(uint32_t f)
{
    return (f >= 0x200 && f <= 0x207) ? (VkCompareOp)(f - 0x200) : VK_COMPARE_OP_ALWAYS;
}

/* ── frame interpolation: what a frame drew ─────────────────────── */

/* DW_INTERP: smooth motion above the game's own frame rate, the way the N64
 * recompilations do it. The game keeps running at its own pace; every clear
 * and draw it makes is also recorded (pipeline, texture, push constants and
 * the homogeneous vertices), and a presenter thread running at the
 * display's refresh replays the newest frame with each draw's vertices
 * blended toward their place in the frame before -- so a 30 fps game shows
 * 60 or 120 distinct pictures a second. Draws are paired across frames by
 * the executor's batch key (which vertex stream, which indices), then by
 * texture/pipeline/size in order; a pair whose vertices jump too far, a
 * frame that does not start with a clear, or one that samples its own
 * surface is shown as drawn instead. Costs one frame of latency. */


static int interp_on(void)
{
    if (P.on < 0) {
        const char *e = getenv("DW_INTERP");
        P.test = getenv("DW_INTERP_TEST");
        P.on = e ? atoi(e) != 0 : V.present;
        if (P.test)
            P.on = 1;
        if (P.on)
            fprintf(stderr, "[VK] frame interpolation on%s\n",
                    P.test ? " (offscreen test presenter)" : "");
    }
    return P.on;
}

static Rec *rec_push(RecList *l)
{
    if (l->nrec == l->caprec) {
        uint32_t c = l->caprec ? l->caprec * 2 : 1024;
        Rec *r = realloc(l->rec, (size_t)c * sizeof *r);
        if (!r)
            return NULL;
        l->rec = r;
        l->caprec = c;
    }
    return &l->rec[l->nrec++];
}

static Nv2aVertex *vtx_push(RecList *l, uint32_t n)
{
    Nv2aVertex *p;
    if (l->nvtx + n > l->capvtx) {
        uint32_t c = l->capvtx ? l->capvtx : 65536;
        Nv2aVertex *v;
        while (c < l->nvtx + n)
            c *= 2;
        v = realloc(l->vtx, (size_t)c * sizeof *v);
        if (!v)
            return NULL;
        l->vtx = v;
        l->capvtx = c;
    }
    p = l->vtx + l->nvtx;
    l->nvtx += n;
    return p;
}

static uint16_t *idx_push(RecList *l, uint32_t n)
{
    uint16_t *p;
    if (l->nidx + n > l->capidx) {
        uint32_t c = l->capidx ? l->capidx : 131072;
        uint16_t *x;
        while (c < l->nidx + n)
            c *= 2;
        x = realloc(l->idx, (size_t)c * sizeof *x);
        if (!x)
            return NULL;
        l->idx = x;
        l->capidx = c;
    }
    p = l->idx + l->nidx;
    l->nidx += n;
    return p;
}

static uint32_t tex_bytes(const Nv2aTexture *t);

static void rec_draw(const Nv2aBatch *b, VkPipeline pipe, VkImageView view, VkSampler smp,
                     const float *pc, const Nv2aRenderState *rs, const Nv2aTexture *tex)
{
    Nv2aVertex *v;
    uint16_t *ix = NULL;
    uint32_t ni = b->indices ? b->nindices : 0;
    Rec *r;

    if (!V.rt || !b->count || (b->indices && !ni))
        return;
    v = vtx_push(&P.build, b->count);
    if (!v)
        return;
    if (ni && !(ix = idx_push(&P.build, ni))) {
        P.build.nvtx -= b->count;
        return;
    }
    r = rec_push(&P.build);
    if (!r) {
        P.build.nvtx -= b->count;
        P.build.nidx -= ni;
        return;
    }
    memcpy(v, b->vertices, (size_t)b->count * sizeof *v);
    if (ni)
        memcpy(ix, b->indices, (size_t)ni * sizeof *ix);
    memset(r, 0, sizeof *r);
    r->kind = 0;
    r->t = V.rt;
    r->pipe = pipe;
    r->view = view;
    r->smp = smp;
    memcpy(r->pc, pc, sizeof r->pc);
    r->smask = rs->stencil_func_mask & 0xFF;
    r->wmask = rs->stencil_write_mask & 0xFF;
    r->sref = rs->stencil_ref & 0xFF;
    r->bc[0] = ((rs->blend_color >> 16) & 0xFF) / 255.0f;
    r->bc[1] = ((rs->blend_color >>  8) & 0xFF) / 255.0f;
    r->bc[2] = ( rs->blend_color        & 0xFF) / 255.0f;
    r->bc[3] = ((rs->blend_color >> 24) & 0xFF) / 255.0f;
    r->key = b->key;
    r->tex_va = tex ? tex->offset : 0;
    r->tex_bytes = tex ? tex_bytes(tex) : 0;
    r->first = (uint32_t)(v - P.build.vtx);
    r->count = b->count;
    r->ifirst = ni ? (uint32_t)(ix - P.build.idx) : 0;
    r->icount = ni;
    r->match = -1;
}

static void rec_clear(const VkClearAttachment *at, uint32_t n)
{
    Rec *r;
    if (!V.rt || !(r = rec_push(&P.build)))
        return;
    memset(r, 0, sizeof *r);
    r->kind = 1;
    r->t = V.rt;
    r->nclear = (uint8_t)n;
    memcpy(r->at, at, n * sizeof *at);
    r->match = -1;
}

/* The oldest frame whose records the presenter may still replay: textures
 * used since then must stay alive. */
static uint32_t interp_keep_frame(void)
{
    uint32_t f = V.frame;
    int i, s[4];

    if (!P.started)
        return f;
    pthread_mutex_lock(&P.mu);
    s[0] = P.latest;
    s[1] = P.latest >= 0 ? P.slot[P.latest].prev : -1;
    s[2] = P.pin[0];
    s[3] = P.pin[1];
    for (i = 0; i < 4; i++)
        if (s[i] >= 0 && P.slot[s[i]].frame < f)
            f = P.slot[s[i]].frame;
    pthread_mutex_unlock(&P.mu);
    return f;
}

/* About to destroy a texture the presenter might be replaying with: stop
 * replays of everything published so far and wait out the one running. */
static void interp_quiesce(void)
{
    if (!P.started)
        return;
    pthread_mutex_lock(&P.mu);
    P.no_interp_before = V.frame + 1;
    while (P.busy)
        pthread_cond_wait(&P.cv, &P.mu);
    pthread_mutex_unlock(&P.mu);
}

/* ── homogeneous vertices ───────────────────────────────────────── */

/* The executor hands over every triangle unclipped, in homogeneous surface
 * space (NV2A_BACKEND_HOMOGENEOUS: x, y, z times w, and w in the rhw field),
 * so the same draw has the same vertices every frame -- what frame
 * interpolation needs to pair them up. The vertex shader takes them as they
 * are and the GPU clips and divides (it did the near-plane clip and the
 * divide on the CPU before, three vertices per triangle). DW_CLIP_W is where
 * frame interpolation counts a vertex as in front of the eye. */
#define DW_CLIP_W 1e-3f

/* DW_FOG=0: draw without the title's fog (as before it was passed on). */
static int s_no_fog = -1;

/* ── the back end ───────────────────────────────────────────────── */

static void be_clear(const Nv2aSurface *s, const Nv2aRenderState *rs,
                     uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    VkClearAttachment at[2];
    VkClearRect rect;
    uint32_t n = 0;

    if (!V.ok || !bind_surface(s, rs))
        return;
    memset(at, 0, sizeof at);
    if (flags & 0xF0u) {
        at[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        at[n].clearValue.color.float32[0] = ((argb >> 16) & 0xFF) / 255.0f;
        at[n].clearValue.color.float32[1] = ((argb >>  8) & 0xFF) / 255.0f;
        at[n].clearValue.color.float32[2] = ( argb        & 0xFF) / 255.0f;
        at[n].clearValue.color.float32[3] = ((argb >> 24) & 0xFF) / 255.0f;
        n++;
    }
    if (flags & 0x3u) {
        /* Z24S8 when the clip range says 24 bits, Z16 otherwise. */
        int z24 = rs->depth_max == 0.0f || rs->depth_max > 65535.5f;
        at[n].aspectMask = ((flags & 1u) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0)
                         | ((flags & 2u) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        at[n].clearValue.depthStencil.depth = z24
            ? (float)(zstencil >> 8) / 16777215.0f
            : (float)(zstencil & 0xFFFF) / 65535.0f;
        at[n].clearValue.depthStencil.stencil = z24 ? (zstencil & 0xFF) : 0;
        n++;
    }
    if (!n)
        return;
    memset(&rect, 0, sizeof rect);
    rect.rect.extent.width = V.rt->w * V.rt->scale;
    rect.rect.extent.height = V.rt->h * V.rt->scale;
    rect.layerCount = 1;
    vkCmdClearAttachments(V.cmd, n, at, 1, &rect);
    if (interp_on())
        rec_clear(at, n);
}

static void be_draw_body(const Nv2aSurface *s, const Nv2aBatch *b);
static void be_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    uint64_t t0 = now_ns(), f0 = V.t_flush;
    be_draw_body(s, b);
    V.t_draw += (now_ns() - t0) - (V.t_flush - f0);
}

/* Ticks since *k0, less any flush in between (V.t_flush is in ns), into *acc;
 * *k0 moves on. */
static void ksplit(uint64_t *acc, uint64_t *k0, uint64_t *f0)
{
    uint64_t k = tsc_now(), f = V.t_flush - *f0;
    uint64_t d = k - *k0, fl = (uint64_t)((double)f * (V.k_tpns > 0.0 ? V.k_tpns : 1.0));
    *acc += d > fl ? d - fl : 0;
    *k0 = k;
    *f0 = V.t_flush;
}

static void be_draw_body(const Nv2aSurface *s, const Nv2aBatch *b)
{
    const Nv2aRenderState *rs = b->state;
    uint64_t k0 = tsc_now(), kf = V.t_flush;
    Tex *tex = NULL;
    VkImageView view;
    VkPipeline pipe;
    uint32_t bytes, off, ioff = 0, func, ni = b->indices ? b->nindices : 0;
    struct { float vp[4], tex[4], alpha[4], fog[4]; } pc;
    VkDescriptorImageInfo ii;
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    VkDeviceSize vo;
    int has_depth = rs->zeta_va != 0;

    if (!V.ok || !b->count || (b->indices && ni < 3))
        return;
    V.n_batches++;
    /* The texture first: decoding it can flush, which ends the rendering. */
    if (V.recording && V.dsets == DW_DESC_SETS)
        flush();
    if (b->texture)
        tex = get_texture(b->texture);
    ksplit(&V.k_tex, &k0, &kf);
    if (!bind_surface(s, rs))
        return;
    ksplit(&V.k_surf, &k0, &kf);
    /* The vertices as they came, and an indexed batch's triangle corners:
     * the GPU clips. Both rings are write-only from here (mapped memory
     * that can be uncached). */
    bytes = b->count * (uint32_t)sizeof(Nv2aVertex);
    off = ring_alloc(&V.verts, bytes, 16);
    if (off != UINT32_MAX && ni)
        ioff = ring_alloc(&V.idx, ni * (uint32_t)sizeof(uint16_t), 16);
    if (off == UINT32_MAX || ioff == UINT32_MAX) {
        flush();
        if (b->texture)
            tex = get_texture(b->texture);
        if (!bind_surface(s, rs))
            return;
        off = ring_alloc(&V.verts, bytes, 16);
        ioff = 0;
        if (off != UINT32_MAX && ni)
            ioff = ring_alloc(&V.idx, ni * (uint32_t)sizeof(uint16_t), 16);
        if (off == UINT32_MAX || ioff == UINT32_MAX)
            return;                      /* larger than the whole ring */
    }
    memcpy(V.verts.ptr + off, b->vertices, bytes);
    if (ni)
        memcpy(V.idx.ptr + ioff, b->indices, ni * sizeof(uint16_t));
    ksplit(&V.k_clip, &k0, &kf);

    pipe = get_pipeline(rs, V.rt->bpp, has_depth);
    if (!pipe)
        return;
    if (pipe != V.bound) {
        vkCmdBindPipeline(V.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        V.bound = pipe;
    }
    ksplit(&V.k_pipe, &k0, &kf);
    vkCmdSetStencilCompareMask(V.cmd, VK_STENCIL_FACE_FRONT_AND_BACK, rs->stencil_func_mask & 0xFF);
    vkCmdSetStencilWriteMask(V.cmd, VK_STENCIL_FACE_FRONT_AND_BACK, rs->stencil_write_mask & 0xFF);
    vkCmdSetStencilReference(V.cmd, VK_STENCIL_FACE_FRONT_AND_BACK, rs->stencil_ref & 0xFF);
    {
        float bc[4] = { ((rs->blend_color >> 16) & 0xFF) / 255.0f,
                        ((rs->blend_color >>  8) & 0xFF) / 255.0f,
                        ( rs->blend_color        & 0xFF) / 255.0f,
                        ((rs->blend_color >> 24) & 0xFF) / 255.0f };
        vkCmdSetBlendConstants(V.cmd, bc);
    }

    view = tex ? tex->img.view : V.white.view;
    {
        VkSampler smp = get_sampler(tex ? b->texture : NULL);
        if (view != V.bound_view || smp != V.bound_sampler) {
            VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            VkDescriptorSet set;
            if (V.dsets == DW_DESC_SETS)
                return;                  /* ponytail: flush instead */
            dai.descriptorPool = V.dpool;
            dai.descriptorSetCount = 1;
            dai.pSetLayouts = &V.dsl;
            if (vkAllocateDescriptorSets(V.dev, &dai, &set) != VK_SUCCESS)
                return;
            V.dsets++;
            memset(&ii, 0, sizeof ii);
            ii.sampler = smp;
            ii.imageView = view;
            ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            w.dstSet = set;
            w.dstBinding = 0;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.pImageInfo = &ii;
            vkUpdateDescriptorSets(V.dev, 1, &w, 0, NULL);
            vkCmdBindDescriptorSets(V.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, V.layout,
                                    0, 1, &set, 0, NULL);
            V.bound_view = view;
            V.bound_sampler = smp;
        }
    }
    ksplit(&V.k_desc, &k0, &kf);

    func = rs->alpha_test_enable ? rs->alpha_func : 0;
    pc.vp[0] = 2.0f / (float)V.rt->w;
    pc.vp[1] = 2.0f / (float)V.rt->h;
    pc.vp[2] = 1.0f / ((rs->depth_max > 0.0f) ? rs->depth_max : 16777215.0f);
    pc.vp[3] = rs->zclamp ? 1.0f : 0.0f;   /* 0: beyond near/far is clipped */
    pc.tex[0] = tex ? 1.0f / (float)b->texture->width : 0.0f;
    pc.tex[1] = tex ? 1.0f / (float)b->texture->height : 0.0f;
    pc.tex[2] = tex ? 1.0f : 0.0f;
    pc.tex[3] = 0.0f;
    pc.alpha[0] = (rs->alpha_test_enable && func >= 0x200 && func <= 0x207)
                  ? (float)(func - 0x200) : 8.0f;
    pc.alpha[1] = (float)(rs->alpha_ref & 0xFF) / 255.0f;
    pc.alpha[2] = pc.alpha[3] = 0.0f;
    pc.fog[0] = (float)((rs->fog_color >> 16) & 0xFF) / 255.0f;
    pc.fog[1] = (float)((rs->fog_color >> 8) & 0xFF) / 255.0f;
    pc.fog[2] = (float)(rs->fog_color & 0xFF) / 255.0f;
    if (s_no_fog < 0) {
        const char *e = getenv("DW_FOG");
        s_no_fog = e && atoi(e) == 0;
        vk_log("fog %s", s_no_fog ? "off (DW_FOG=0)" : "on");
    }
    pc.fog[3] = (rs->fog_enable && !s_no_fog) ? 1.0f : 0.0f;
    vkCmdPushConstants(V.cmd, V.layout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof pc, &pc);
    if (interp_on())
        rec_draw(b, pipe, view, get_sampler(tex ? b->texture : NULL), (const float *)&pc, rs,
                 tex ? b->texture : NULL);
    vo = off;
    vkCmdBindVertexBuffers(V.cmd, 0, 1, &V.verts.buf, &vo);
    if (ni) {
        vkCmdBindIndexBuffer(V.cmd, V.idx.buf, ioff, VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(V.cmd, ni, 1, 0, 0, 0);
        V.stats_tris += ni / 3;
    } else {
        vkCmdDraw(V.cmd, b->count, 1, 0, 0);
        V.stats_tris += b->count / 3;
    }
    V.stats_draws++;
    if (pc.fog[3] > 0.5f)
        V.stats_fogged++;
    ksplit(&V.k_cmd, &k0, &kf);
}

/* A BGRA8 / RGB565 picture as a 24-bit bottom-up BMP. */
static void write_bmp(const char *path, const uint8_t *p, uint32_t w, uint32_t h,
                      uint32_t stride, uint32_t bpp)
{
    uint32_t row = w * 3, pad = (4 - (row & 3)) & 3, filesz = 54 + (row + pad) * h, x, y;
    uint8_t hdr[54], *line;
    FILE *f = fopen(path, "wb");

    if (!f)
        return;
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &filesz, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, sizeof hdr, f);
    line = calloc(1, row + pad);
    for (y = h; line && y-- > 0; ) {
        const uint8_t *s = p + (size_t)y * stride;
        for (x = 0; x < w; x++) {
            if (bpp == 4) {
                line[x * 3] = s[x * 4];
                line[x * 3 + 1] = s[x * 4 + 1];
                line[x * 3 + 2] = s[x * 4 + 2];
            } else {
                uint16_t v = (uint16_t)(s[x * 2] | s[x * 2 + 1] << 8);
                line[x * 3] = (uint8_t)((v & 0x1F) << 3);
                line[x * 3 + 1] = (uint8_t)(((v >> 5) & 0x3F) << 2);
                line[x * 3 + 2] = (uint8_t)(((v >> 11) & 0x1F) << 3);
            }
        }
        fwrite(line, 1, row + pad, f);
    }
    free(line);
    fclose(f);
}

/* DW_HIRES_DUMP=<prefix>: every DW_HIRES_DUMP_EVERY frames (default 150),
 * the 640x480 surface drawn this frame twice -- <prefix>hNNNNN.bmp from the
 * large image (what the console build presents) and <prefix>gNNNNN.bmp from
 * guest memory (the console-sized write-back) -- so a headless run can show
 * what DW_RES_SCALE does. Runs right after the flip's flush. */
static void hires_dump(void)
{
    static const char *prefix = (const char *)-1;
    static long every;
    static unsigned seq;
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    Target *t = NULL;
    VkBufferImageCopy c;
    uint32_t sw, sh;
    char path[512];
    int i;

    if (prefix == (const char *)-1) {
        const char *e = getenv("DW_HIRES_DUMP_EVERY");
        prefix = getenv("DW_HIRES_DUMP");
        every = e ? strtol(e, NULL, 10) : 150;
        if (every < 1)
            every = 1;
    }
    if (!prefix || V.frame % (uint32_t)every)
        return;
    for (i = 0; i < MAX_TARGETS; i++) {
        Target *x = &V.targets[i];
        if (x->used && x->frame == V.frame && x->w == 640 && x->h == 480
            && x->scale > 1 && x->wb_valid)
            t = x;
    }
    if (!t)
        return;
    sw = t->w * t->scale;
    sh = t->h * t->scale;
    if (!V.dump.buf && !ring_init(&V.dump, 640u * 480u * 4u * 16u,
                                  VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
        prefix = NULL;
        return;
    }
    begin_recording();
    img_layout(&t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    memset(&c, 0, sizeof c);
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = sw;
    c.imageExtent.height = sh;
    c.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(V.cmd, t->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           V.dump.buf, 1, &c);
    flush();                            /* nothing is loaded: just the copy */
    snprintf(path, sizeof path, "%sh%05u.bmp", prefix, seq);
    write_bmp(path, V.dump.ptr, sw, sh, sw * t->bpp, t->bpp);
    snprintf(path, sizeof path, "%sg%05u.bmp", prefix, seq);
    write_bmp(path, mem + t->va, t->w, t->h, t->pitch, t->bpp);
    seq++;
}

/* nv2a_capture_request (the host's screenshot key): the frame that this flip
 * ends, as drawn (internal resolution), to <prefix>.png; the executor has
 * written what drew it to <prefix>.txt. */
static void capture_save(const char *prefix)
{
    Target *t = NULL;
    VkBufferImageCopy c;
    uint32_t sw, sh, x, y, i;
    uint8_t *rgb;
    char path[600];

    for (i = 0; i < MAX_TARGETS; i++) {
        Target *x2 = &V.targets[i];
        if (x2->used && x2->frame == V.frame && x2->w == 640 && x2->h == 480)
            t = x2;
    }
    if (!t) {
        fprintf(stderr, "[VK] capture: no 640x480 surface drawn this frame\n");
        return;
    }
    sw = t->w * t->scale;
    sh = t->h * t->scale;
    if (!V.dump.buf && !ring_init(&V.dump, 640u * 480u * 4u * 16u,
                                  VK_BUFFER_USAGE_TRANSFER_DST_BIT))
        return;
    begin_recording();
    img_layout(&t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    memset(&c, 0, sizeof c);
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = sw;
    c.imageExtent.height = sh;
    c.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(V.cmd, t->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           V.dump.buf, 1, &c);
    flush();
    rgb = (uint8_t *)malloc((size_t)sw * sh * 3);
    if (!rgb)
        return;
    for (y = 0; y < sh; y++)
        for (x = 0; x < sw; x++) {
            const uint8_t *p = V.dump.ptr + ((size_t)y * sw + x) * t->bpp;
            uint8_t *o = rgb + ((size_t)y * sw + x) * 3;
            if (t->bpp == 4) {
                o[0] = p[2]; o[1] = p[1]; o[2] = p[0];
            } else {
                uint16_t v = (uint16_t)(p[0] | p[1] << 8);
                o[0] = (uint8_t)(((v >> 11) & 0x1F) << 3);
                o[1] = (uint8_t)(((v >> 5) & 0x3F) << 2);
                o[2] = (uint8_t)((v & 0x1F) << 3);
            }
        }
    snprintf(path, sizeof path, "%s.png", prefix);
    if (stbi_write_png(path, (int)sw, (int)sh, 3, rgb, (int)sw * 3))
        fprintf(stderr, "[VK] capture: %s (%ux%u)\n", path, sw, sh);
    else
        fprintf(stderr, "[VK] capture: could not write %s\n", path);
    free(rgb);
}

static void interp_publish_test(void);
static void present_drawn_surface(void);

static void be_flip(void)
{
    if (!V.ok)
        return;
    flush();
    hires_dump();
    {
        const char *cap = nv2a_capture_ending();
        if (cap)
            capture_save(cap);
    }
    if (interp_on()) {
        /* This frame's records become the finished frame's; the flip that
         * follows (dwvk_present) publishes them. */
        RecList t = P.done;
        P.done = P.build;
        P.build = t;
        P.build.nrec = P.build.nvtx = P.build.nidx = 0;
        P.done_frame = V.frame;
        P.done_ok = 1;
        if (P.test && !V.headless)
            interp_publish_test();
    }
    if (V.headless && V.present)
        present_drawn_surface();
    V.frame++;
    if (!V.t_frame0)
        V.t_frame0 = now_ns();
    if (++V.frames_since == 60) {
        uint64_t wall = now_ns() - V.t_frame0;
        fprintf(stderr, "[VK] last 60 frames: %.1f ms/frame wall; per frame "
                "draw-record %.2f ms, texture decode %.2f ms, checksum %.2f ms "
                "(%u binds reused one), submit+wait+writeback %.2f ms (%u draws total)\n",
                wall / 60e6, V.t_draw / 60e6, V.t_decode / 60e6, V.t_sum / 60e6,
                V.stats_sum_skipped / 60, V.t_flush / 60e6, V.stats_draws);
        V.stats_sum_skipped = 0;
        fprintf(stderr, "[VK]   flush split per frame: submit %.2f ms, GPU wait %.2f ms, "
                "copy back to guest memory %.2f ms\n", V.t_fl_submit / 60e6, V.t_fl_wait / 60e6,
                V.t_fl_wb / 60e6);
        V.t_fl_submit = V.t_fl_wait = V.t_fl_wb = 0;
        {
            uint64_t kn = tsc_now(), nn = now_ns();
            if (V.k_cal_ns && nn > V.k_cal_ns) {
                double tpns = (double)(kn - V.k_cal_tsc) / (double)(nn - V.k_cal_ns);
                double f = 1.0 / (tpns * 60e6);
                if (V.k_tpns > 0.0)
                    fprintf(stderr, "[VK]   draw-record split per frame: textures %.2f ms, surface "
                            "%.2f, vertex copy %.2f, pipeline %.2f, descriptors %.2f, commands %.2f; "
                            "%u batches a frame\n", V.k_tex * f, V.k_surf * f, V.k_clip * f,
                            V.k_pipe * f, V.k_desc * f, V.k_cmd * f, V.n_batches / 60);
                V.k_tpns = tpns;
            }
            V.k_cal_tsc = kn;
            V.k_cal_ns = nn;
            V.k_tex = V.k_surf = V.k_clip = V.k_pipe = V.k_desc = V.k_cmd = 0;
            V.n_batches = 0;
        }
        fflush(stderr);
        V.t_draw = V.t_decode = V.t_sum = V.t_flush = 0;
        V.frames_since = 0;
        V.t_frame0 = now_ns();
    }
    if (V.frame == 1 || V.frame % 300 == 0) {
        char buf[224];
        int n = snprintf(buf, sizeof buf, "frame %u: %u draws (%u fogged), %u triangles, %u texture "
                         "uploads, %u submissions so far", V.frame, V.stats_draws, V.stats_fogged,
                         V.stats_tris, V.stats_uploads, V.stats_flushes);
        if (res_scale() > 1 && n > 0 && (size_t)n < sizeof buf)
            snprintf(buf + n, sizeof buf - (size_t)n, "; %ux surfaces: %u reloads "
                     "skipped, %u done", res_scale(), V.stats_kept, V.stats_reloads);
        vk_log("%s", buf);
    }
}

static const Nv2aBackend s_backend = { be_clear, be_draw, be_flip,
                                       NV2A_BACKEND_HOMOGENEOUS | NV2A_BACKEND_INDEXED };

/* ── presenting to our own display ──────────────────────────────── */

/* The surface, a FIFO swapchain on it, and what a present records. Returns 1
 * when frames can be shown. */
/* The swapchain at the surface's current size. Returns 0 while the surface
 * has no area (a minimised window): nothing can be shown until it does. */
static int sc_create(void)
{
    VkSurfaceCapabilitiesKHR caps;
    VkSwapchainCreateInfoKHR sci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    VkPresentModeKHR modes[16];
    uint32_t nm = 16, i;
    VkSwapchainKHR old = V.swapchain;
    VkExtent2D ext;

    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(V.phys, V.surface, &caps) != VK_SUCCESS)
        return 0;
    ext = caps.currentExtent;
    if (ext.width == UINT32_MAX) {
        ext = V.sc_extent.width ? V.sc_extent : (VkExtent2D){ 1920, 1080 };
        if (ext.width < caps.minImageExtent.width) ext.width = caps.minImageExtent.width;
        if (ext.height < caps.minImageExtent.height) ext.height = caps.minImageExtent.height;
    }
    if (!ext.width || !ext.height)
        return 0;
    /* FIFO is V-Sync and always there. Without V-Sync: MAILBOX (no tearing,
     * newest frame wins), else IMMEDIATE. */
    V.sc_mode = VK_PRESENT_MODE_FIFO_KHR;
    if (!s_vsync && vkGetPhysicalDeviceSurfacePresentModesKHR(V.phys, V.surface, &nm, modes) >= 0) {
        for (i = 0; i < nm; i++)
            if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR)
                V.sc_mode = modes[i];
        if (V.sc_mode == VK_PRESENT_MODE_FIFO_KHR)
            for (i = 0; i < nm; i++)
                if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR)
                    V.sc_mode = modes[i];
    }
    sci.surface = V.surface;
    sci.minImageCount = caps.minImageCount < 3 ? 3 : caps.minImageCount;
    if (caps.maxImageCount && sci.minImageCount > caps.maxImageCount)
        sci.minImageCount = caps.maxImageCount;
    sci.imageFormat = V.sc_format;
    sci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    sci.imageExtent = ext;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                       ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
    sci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
        ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
        : (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha & -caps.supportedCompositeAlpha);
    sci.presentMode = V.sc_mode;
    sci.clipped = VK_TRUE;
    sci.oldSwapchain = old;
    if (vkCreateSwapchainKHR(V.dev, &sci, NULL, &V.swapchain) != VK_SUCCESS) {
        V.swapchain = old;
        vk_log("%s", "present: vkCreateSwapchainKHR failed");
        return 0;
    }
    if (old) {
        /* Nothing may still be using the old one's images. */
        pthread_mutex_lock(&P.qmu);
        vkQueueWaitIdle(V.queue);
        pthread_mutex_unlock(&P.qmu);
        vkDestroySwapchainKHR(V.dev, old, NULL);
    }
    V.sc_extent = ext;
    V.sc_count = 8;
    vkGetSwapchainImagesKHR(V.dev, V.swapchain, &V.sc_count, V.sc_images);
    {
        char buf[128];
        snprintf(buf, sizeof buf, "%ux%u, %u images, format %d, %s", V.sc_extent.width,
                 V.sc_extent.height, V.sc_count, (int)V.sc_format,
                 V.sc_mode == VK_PRESENT_MODE_FIFO_KHR ? "V-Sync (FIFO)"
                 : V.sc_mode == VK_PRESENT_MODE_MAILBOX_KHR ? "no V-Sync (MAILBOX)"
                 : "no V-Sync (IMMEDIATE)");
        vk_log("present: swapchain %s", buf);
    }
    return 1;
}

/* Before an acquire, on the presenting thread: rebuild a stale swapchain.
 * 0: nothing can be presented right now (minimised); try again later. */
static int sc_rebuild(void)
{
    if (!V.swapchain || s_sc_dirty) {
        s_sc_dirty = 0;
        if (!sc_create()) {
            s_sc_dirty = 1;
            return 0;
        }
    }
    return 1;
}

/* What an acquire or present said about the swapchain. */
static void sc_note(VkResult r)
{
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR || r == VK_ERROR_SURFACE_LOST_KHR)
        s_sc_dirty = 1;
}

/* The surface, a swapchain on it, and what a present records. Returns 1
 * when frames can be shown. */
static int present_init(void)
{
    VkSurfaceFormatKHR fmts[32];
    uint32_t nf = 32, i;
    VkBool32 ok = VK_FALSE;
    VkExtent2D ext = { 0, 0 };
    VkSemaphoreCreateInfo semi = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };

    V.surface = s_present_cfg.make_surface(V.inst, vkGetInstanceProcAddr_, V.phys, &ext);
    if (!V.surface) {
        vk_log("%s", "present: no surface");
        return 0;
    }
    vkGetPhysicalDeviceSurfaceSupportKHR(V.phys, V.qfam, V.surface, &ok);
    if (!ok)
        vk_log("%s", "present: the queue reports no present support; trying anyway");
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(V.phys, V.surface, &nf, fmts) < 0 || !nf) {
        vk_log("%s", "present: no surface formats");
        return 0;
    }
    V.sc_format = fmts[0].format;
    for (i = 0; i < nf; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) {
            V.sc_format = fmts[i].format;
            break;
        }
    V.sc_extent = ext;
    if (!sc_create()) {
        /* A minimised window at start: built at the first present. */
        s_sc_dirty = 1;
        vk_log("%s", "present: the surface has no area yet; the swapchain waits");
    }
    vkCreateSemaphore(V.dev, &semi, NULL, &V.sem_acquire);
    vkCreateSemaphore(V.dev, &semi, NULL, &V.sem_done);
    cai.commandPool = V.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    vkAllocateCommandBuffers(V.dev, &cai, &V.pcmd);
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    vkCreateFence(V.dev, &fci, NULL, &V.pfence);
    return 1;
}

/* Host side: the window changed size (or mode); V-Sync, the frame cap and
 * the aspect policy. Safe from any thread. */
void dwvk_window_resized(void)
{
    s_sc_dirty = 1;
}

void dwvk_set_display_options(int vsync, int fps_cap, int aspect)
{
    if (vsync != s_vsync)
        s_sc_dirty = 1;
    s_vsync = vsync != 0;
    s_fps_cap = fps_cap < 0 ? 0 : fps_cap;
    s_aspect = aspect;
}

/* Whether a Vulkan display of our own is up (else the host draws the
 * guest framebuffer itself). */
unsigned dwvk_display_frames(void)
{
    return s_display_frames;
}

int dwvk_presenting(void)
{
    return V.ok && V.present;
}

/* Where the 4:3 picture goes in a w x h swapchain image. */
static void sc_rect(uint32_t *dx, uint32_t *dy, uint32_t *dw, uint32_t *dh)
{
    uint32_t W = V.sc_extent.width, H = V.sc_extent.height;
    if (s_aspect == 1) {
        *dx = *dy = 0;
        *dw = W;
        *dh = H;
        return;
    }
    *dh = H;
    *dw = H * 4 / 3;
    if (*dw > W) {
        *dw = W;
        *dh = W * 3 / 4;
    }
    *dx = (W - *dw) / 2;
    *dy = (H - *dh) / 2;
}

/* Show the guest surface at fb_va (640 wide, pitch bytes a row): copied out of
 * guest memory, which the flip has just made current, and scaled into a 4:3
 * rectangle in the middle of the display. Called on the executor's thread,
 * right after the flip. */
/* ── frame interpolation: publishing frames, and the presenter ─────── */

static uint64_t handle_bits(const void *h, size_t n)
{
    uint64_t v = 0;
    memcpy(&v, h, n < sizeof v ? n : sizeof v);
    return v;
}

/* Pass 0 pairs by the executor's batch key; pass 1, for what is left (inline
 * vertex data, dynamic buffers), by size, pipeline and texture alone. */
static uint32_t rec_hash(const Rec *r, int pass)
{
    uint64_t p = handle_bits(&r->pipe, sizeof r->pipe);
    uint32_t h = 2166136261u;
    h = (h ^ r->count) * 16777619u;
    h = (h ^ r->tex_va) * 16777619u;
    h = (h ^ (uint32_t)p) * 16777619u;
    h = (h ^ (uint32_t)(p >> 32)) * 16777619u;
    if (!pass)
        h = (h ^ r->key) * 16777619u;
    return h;
}

static int rec_same(const Rec *a, const Rec *b, int pass)
{
    return a->count == b->count && a->tex_va == b->tex_va && a->pipe == b->pipe
        && (pass || a->key == b->key);
}

/* The same texture coordinates, vertex for vertex: for a screen-locked draw
 * (menu text, HUD) that is "the same picture", and only that is moved
 * between frames. Two strings of one length drawn inline look alike to
 * rec_same (same count, texture and key), and sliding one into the other's
 * place made the profile screen's words flash. */
static int uv_same(const RecList *la, const Rec *ra, const RecList *lb, const Rec *rb)
{
    const Nv2aVertex *a = la->vtx + ra->first, *b = lb->vtx + rb->first;
    uint32_t k;
    if (ra->count != rb->count)
        return 0;
    for (k = 0; k < ra->count; k++)
        if (a[k].u != b[k].u || a[k].v != b[k].v)
            return 0;
    return 1;
}

static int interp_init(void)
{
    VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096 };
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };

    if (P.inited)
        return P.inited > 0;
    P.inited = -1;
    pthread_mutex_init(&P.mu, NULL);
    pthread_cond_init(&P.cv, NULL);
    /* The executor's copies out of its surfaces: its own pool. */
    cai.commandPool = V.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(V.dev, &cai, &P.xcmd) != VK_SUCCESS
        || vkCreateFence(V.dev, &fci, NULL, &P.xfence) != VK_SUCCESS)
        return 0;
    /* The presenter records on another thread: a pool of its own. */
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = V.qfam;
    if (vkCreateCommandPool(V.dev, &cpi, NULL, &P.pool) != VK_SUCCESS)
        return 0;
    cai.commandPool = P.pool;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkAllocateCommandBuffers(V.dev, &cai, &P.cmd) != VK_SUCCESS
        || vkCreateFence(V.dev, &fci, NULL, &P.fence) != VK_SUCCESS)
        return 0;
    dpi.maxSets = 4096;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    if (vkCreateDescriptorPool(V.dev, &dpi, NULL, &P.dpool) != VK_SUCCESS)
        return 0;
    if (!ring_init(&P.verts, VERTEX_BYTES, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)
        || !ring_init(&P.idx, INDEX_BYTES, VK_BUFFER_USAGE_INDEX_BUFFER_BIT))
        return 0;
    if (P.test && !ring_init(&P.rb, 3u * 2560u * 1920u * 4u, VK_BUFFER_USAGE_TRANSFER_DST_BIT))
        return 0;
    P.inited = 1;
    return 1;
}

static void *presenter_main(void *arg);

static void submit_locked(VkCommandBuffer cmd, VkFence fence, const VkSemaphore *wait,
                          const VkSemaphore *signal)
{
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT;

    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (wait) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = wait;
        si.pWaitDstStageMask = &ws;
    }
    if (signal) {
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = signal;
    }
    pthread_mutex_lock(&P.qmu);
    vkQueueSubmit(V.queue, 1, &si, fence);
    pthread_mutex_unlock(&P.qmu);
}

/* The flip just presented the 640 x h surface at fb_va: hand the frame to
 * the presenter. Executor thread, after the flip's flush. */
static void interp_publish(uint32_t fb_va, uint32_t pitch, uint32_t h)
{
    const uint32_t w = 640;
    uint32_t bpp = pitch / w, i, sw, sh;
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    Target *T = NULL;
    Slot *S;
    int s, prev;
    uint64_t now;

    if ((bpp != 2 && bpp != 4) || !guest_range_ok(fb_va, pitch * h) || !interp_init())
        return;
    for (i = 0; i < MAX_TARGETS; i++) {
        Target *t = &V.targets[i];
        if (t->used && t->va == fb_va && t->w == w && t->h == h && t->bpp == bpp
            && t->wb_valid && !t->loaded
            && rows_sum(mem + fb_va, w * bpp, h, pitch) == t->wb_sum) {
            T = t;
            break;
        }
    }

    pthread_mutex_lock(&P.mu);
    for (s = 0; s < DW_SLOTS; s++)
        if (s != P.latest && s != P.pin[0] && s != P.pin[1])
            break;
    prev = P.latest;
    /* The newest frame may still name this slot as its predecessor. */
    if (s < DW_SLOTS && prev >= 0 && P.slot[prev].prev == s)
        P.slot[prev].prev = -1;
    pthread_mutex_unlock(&P.mu);
    if (s == DW_SLOTS)
        return;
    S = &P.slot[s];

    /* Its records, those of the presented surface only. The finished frame's
     * lists become the slot's (and the slot's old ones the next spare): a
     * busy frame's vertices are megabytes, and copying them here was the
     * executor's time. Records of other surfaces are dropped in place; the
     * vertices stay where they are, each record still pointing at its own. */
    S->list.nrec = S->list.nvtx = S->list.nidx = 0;
    S->replayable = 0;
    S->matched = S->draws = 0;
    if (T && P.done_ok) {
        int first = 1, base = 0, feedback = 0;
        uint32_t end = T->va + T->pitch * T->h, j = 0;
        RecList t = S->list;
        S->list = P.done;
        P.done = t;
        P.done_ok = 0;
        for (i = 0; i < S->list.nrec; i++) {
            const Rec *r = &S->list.rec[i];
            if (r->t != T)
                continue;
            if (first) {
                base = r->kind == 1 && r->nclear
                       && (r->at[0].aspectMask & VK_IMAGE_ASPECT_COLOR_BIT);
                first = 0;
            }
            if (r->kind == 0 && r->tex_bytes && r->tex_va < end
                && T->va < r->tex_va + r->tex_bytes)
                feedback = 1;
            if (r->kind == 0)
                S->draws++;
            if (j != i)
                S->list.rec[j] = *r;
            S->list.rec[j].match = -1;
            j++;
        }
        S->list.nrec = j;
        S->replayable = base && !feedback;
    }

    /* The picture itself: the surface's own image (at DW_RES_SCALE), or,
     * when guest memory no longer holds what was drawn there (a frame the
     * CPU wrote), guest memory. */
    sw = T ? T->w * T->scale : w;
    sh = T ? T->h * T->scale : h;
    /* The last publish's copy: only its command buffer is reused here. It
     * was not waited for when submitted -- the presenter's commands come
     * after it in the queue and its closing barrier orders them -- so the
     * executor no longer stalls on everything the GPU has queued (the
     * presenter's replays included) once per frame. */
    if (P.xpending) {
        vkWaitForFences(V.dev, 1, &P.xfence, VK_TRUE, UINT64_MAX);
        P.xpending = 0;
    }
    if (!S->real.image || S->rw != sw || S->rh != sh || S->bpp != bpp) {
        if (S->real.image)
            img_destroy(&S->real);
        if (!img_create(&S->real, sw, sh, color_format(bpp),
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT))
            return;
        S->rw = sw;
        S->rh = sh;
        S->bpp = bpp;
    }
    vkResetFences(V.dev, 1, &P.xfence);
    vkResetCommandBuffer(P.xcmd, 0);
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(P.xcmd, &bi);
    img_layout_cmd(P.xcmd, &S->real, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    if (T) {
        VkImageBlit bl;
        img_layout_cmd(P.xcmd, &T->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_IMAGE_ASPECT_COLOR_BIT);
        memset(&bl, 0, sizeof bl);
        bl.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bl.srcSubresource.layerCount = 1;
        bl.srcOffsets[1].x = (int32_t)sw;
        bl.srcOffsets[1].y = (int32_t)sh;
        bl.srcOffsets[1].z = 1;
        bl.dstSubresource = bl.srcSubresource;
        bl.dstOffsets[1] = bl.srcOffsets[1];
        vkCmdBlitImage(P.xcmd, T->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       S->real.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl,
                       VK_FILTER_NEAREST);
    } else {
        VkBufferImageCopy c;
        uint32_t off = (V.staging.size - w * h * bpp) & ~15u, y;
        for (y = 0; y < h; y++)
            memcpy(V.staging.ptr + off + (size_t)y * w * bpp, mem + fb_va + (size_t)y * pitch,
                   (size_t)w * bpp);
        memset(&c, 0, sizeof c);
        c.bufferOffset = off;
        c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        c.imageSubresource.layerCount = 1;
        c.imageExtent.width = w;
        c.imageExtent.height = h;
        c.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(P.xcmd, V.staging.buf, S->real.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    }
    img_layout_cmd(P.xcmd, &S->real, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    vkEndCommandBuffer(P.xcmd);
    submit_locked(P.xcmd, P.xfence, NULL, NULL);
    P.xpending = 1;
    if (!T) {
        /* From the staging ring, which the next frame's uploads reuse. */
        vkWaitForFences(V.dev, 1, &P.xfence, VK_TRUE, UINT64_MAX);
        P.xpending = 0;
    }

    now = now_ns();
    pthread_mutex_lock(&P.mu);
    if (prev >= 0) {
        uint64_t dt = now - P.slot[prev].t_ns;
        if (dt >= 4000000ull && dt <= 200000000ull)
            P.interval_ns = P.interval_ns ? (P.interval_ns * 7 + dt) / 8 : dt;
    }
    if (!P.interval_ns)
        P.interval_ns = 33333333ull;
    S->frame = P.done_frame;
    /* Unique across slots: pair_prepare's cache is keyed on the two gens,
     * and per-slot counters let two different pairs share a key (stale
     * partner indices then pointed past the previous frame's records). */
    S->gen = ++P.gen_next;
    S->t_ns = now;
    S->prev = prev;
    P.latest = s;
    pthread_cond_broadcast(&P.cv);
    pthread_mutex_unlock(&P.mu);

    if (!P.started) {
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setstacksize(&at, 1u << 20);
        if (pthread_create(&P.thread, &at, presenter_main, NULL) == 0)
            P.started = 1;
        pthread_attr_destroy(&at);
        vk_log("%s", P.started ? "interpolation: presenter running"
                               : "interpolation: no presenter thread");
    }
}

/* How each draw of the newest frame gets its position in an in-between
 * picture.
 *
 * Pairing draws by order went wrong two ways on the console. A draw the
 * previous frame did not have in the same form (index lists rebuilt per
 * frame, a mesh whose visible part changed) was drawn where the newest frame
 * has it, a frame ahead of everything around it -- seams and "tearing" all
 * over the screen as the camera moved. And repeated objects (fence posts,
 * crates: one mesh drawn many times) paired with a neighbour whenever one of
 * them came into or left view, so each one slid toward the next.
 *
 * Now:
 *  1. a draw whose key, size, pipeline and texture occur exactly once in
 *     both frames is paired outright;
 *  2. from those pairs' vertices, the camera's motion is fitted: for anything
 *     that stood still, the previous frame's clip-space position is a fixed
 *     4x4 matrix times the new one, so a least-squares fit with the movers
 *     (characters, the gun) trimmed as outliers gives that matrix;
 *  3. the other draws pair with the candidate (same size/pipeline/texture)
 *     nearest to where the camera motion says they were;
 *  4. a draw that still has no partner moves with the camera (it is most
 *     likely scenery), a screen-space one (HUD) stays put, and a pair whose
 *     own motion and camera-predicted motion are both over a quarter screen
 *     is a teleport and snaps.
 * Modes per draw: 0 as drawn, 1 lerp from its partner, 2 lerp from where the
 * camera motion puts it. */

typedef struct {
    double c[4];                        /* homogeneous centroid (raw units) */
    float x, y;                         /* projected, guest pixels */
    int valid;                          /* some vertex in front of the eye */
    int flat;                           /* one w for every vertex: screen-locked */
    int front;                          /* every vertex in front of the eye */
    uint32_t h1, h2;                    /* match hashes: with / without key */
} DrawInfo;

typedef struct { double c[4], p[4]; float sx, sy; } FitPt;

static void draw_info(const RecList *l, DrawInfo **out, uint32_t *cap)
{
    uint32_t i, k;
    if (l->nrec > *cap) {
        DrawInfo *d = realloc(*out, (size_t)l->nrec * sizeof *d);
        if (!d)
            return;
        *out = d;
        *cap = l->nrec;
    }
    for (i = 0; i < l->nrec; i++) {
        const Rec *r = &l->rec[i];
        const Nv2aVertex *v = l->vtx + r->first;
        DrawInfo *d = &(*out)[i];
        double s[4] = { 0, 0, 0, 0 };
        uint32_t n = 0;
        memset(d, 0, sizeof *d);
        if (r->kind != 0)
            continue;
        {
            /* Screen-locked (HUD, overlays): every vertex at one w -- an
             * orthographic or pre-transformed draw. (A camera-facing quad in
             * the world can be too; it is then treated as screen-locked when
             * it has no partner.) */
            float wmin = 1e30f, wmax = -1e30f;
            for (k = 0; k < r->count; k++) {
                if (v[k].rhw < wmin) wmin = v[k].rhw;
                if (v[k].rhw > wmax) wmax = v[k].rhw;
            }
            d->flat = r->count && wmin > 0.0f && wmax - wmin <= wmax * 1e-5f;
        }
        d->front = 1;
        for (k = 0; k < r->count; k++) {
            if (!(v[k].rhw > DW_CLIP_W)) {
                d->front = 0;
                continue;
            }
            s[0] += v[k].x; s[1] += v[k].y; s[2] += v[k].z; s[3] += v[k].rhw;
            n++;
        }
        if (n) {
            for (k = 0; k < 4; k++)
                d->c[k] = s[k] / n;
            d->x = (float)(d->c[0] / d->c[3]);
            d->y = (float)(d->c[1] / d->c[3]);
            d->valid = 1;
        }
        d->h2 = rec_hash(r, 1);
        d->h1 = rec_hash(r, 0);
    }
}

/* Per draw: raw homogeneous -> normalised (x, y over the surface, z over the
 * depth range), the units the camera motion is fitted in. */
static void rec_scale(const Rec *r, double s[4])
{
    s[0] = r->pc[0] * 0.5;
    s[1] = r->pc[1] * 0.5;
    s[2] = r->pc[2] > 0.0f ? r->pc[2] : 1.0 / 16777215.0;
    s[3] = 1.0;
}

static int solve4(double A[16], double B[16], double X[16])
{
    int c, r, k, piv;
    for (c = 0; c < 4; c++) {
        double best = 0.0;
        piv = -1;
        for (r = c; r < 4; r++)
            if (fabs(A[r * 4 + c]) > best) {
                best = fabs(A[r * 4 + c]);
                piv = r;
            }
        if (piv < 0 || best < 1e-300)
            return 0;
        if (piv != c)
            for (k = 0; k < 4; k++) {
                double t = A[c * 4 + k]; A[c * 4 + k] = A[piv * 4 + k]; A[piv * 4 + k] = t;
                t = B[c * 4 + k]; B[c * 4 + k] = B[piv * 4 + k]; B[piv * 4 + k] = t;
            }
        for (r = 0; r < 4; r++) {
            double f;
            if (r == c)
                continue;
            f = A[r * 4 + c] / A[c * 4 + c];
            for (k = 0; k < 4; k++) {
                A[r * 4 + k] -= f * A[c * 4 + k];
                B[r * 4 + k] -= f * B[c * 4 + k];
            }
        }
    }
    for (r = 0; r < 4; r++)
        for (k = 0; k < 4; k++)
            X[r * 4 + k] = B[r * 4 + k] / A[r * 4 + r];
    return 1;
}

static int cmp_float(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

static float fit_residual(const double M[16], const FitPt *q)
{
    double p[4];
    int k;
    for (k = 0; k < 4; k++)
        p[k] = M[k * 4] * q->c[0] + M[k * 4 + 1] * q->c[1] + M[k * 4 + 2] * q->c[2]
             + M[k * 4 + 3] * q->c[3];
    if (!(p[3] > 1e-9))
        return 1e9f;
    {
        double dx = fabs(p[0] / p[3] - q->p[0] / q->p[3]) * q->sx;
        double dy = fabs(p[1] / p[3] - q->p[1] / q->p[3]) * q->sy;
        return (float)(dx > dy ? dx : dy);
    }
}

/* Least squares p = M c over the points, trimmed twice. M row-major in
 * normalised units. Returns the number of inliers (0: no fit). */
static int fit_motion(const FitPt *pts, int n, double M[16], float *median)
{
    static uint8_t *use;
    static float *res, *tmp;
    static int cap;
    int it, i, a, b, inl = 0;

    if (n < 12)
        return 0;
    if (n > cap) {
        uint8_t *u = realloc(use, (size_t)n);
        float *r = realloc(res, (size_t)n * sizeof *r);
        float *t = realloc(tmp, (size_t)n * sizeof *t);
        if (u) use = u;
        if (r) res = r;
        if (t) tmp = t;
        if (!u || !r || !t)
            return 0;
        cap = n;
    }
    memset(use, 1, (size_t)n);
    for (it = 0; it < 3; it++) {
        double G[16] = { 0 }, R[16] = { 0 }, X[16], tr, lam;
        int m = 0;
        for (i = 0; i < n; i++) {
            if (!use[i])
                continue;
            for (a = 0; a < 4; a++)
                for (b = 0; b < 4; b++) {
                    G[a * 4 + b] += pts[i].c[a] * pts[i].c[b];
                    R[a * 4 + b] += pts[i].c[a] * pts[i].p[b];
                }
            m++;
        }
        if (m < 12)
            return 0;
        /* Pulled gently toward "no motion", so a frame of one flat wall
         * (a plane leaves the fit underdetermined) still solves. */
        tr = G[0] + G[5] + G[10] + G[15];
        lam = tr * 1e-7 + 1e-30;
        for (a = 0; a < 4; a++) {
            G[a * 5] += lam;
            R[a * 5] += lam;
        }
        if (!solve4(G, R, X))
            return 0;
        for (a = 0; a < 4; a++)
            for (b = 0; b < 4; b++)
                M[b * 4 + a] = X[a * 4 + b];
        for (i = 0; i < n; i++)
            res[i] = fit_residual(M, &pts[i]);
        memcpy(tmp, res, (size_t)n * sizeof *tmp);
        qsort(tmp, (size_t)n, sizeof *tmp, cmp_float);
        {
            /* Keep what is near the bulk: the trim follows the better half. */
            float thr = tmp[n / 2] * 3.0f;
            if (thr < 1.0f)
                thr = 1.0f;
            inl = 0;
            for (i = 0; i < n; i++) {
                use[i] = res[i] < thr;
                inl += use[i];
            }
        }
    }
    {
        int j = 0;
        for (i = 0; i < n; i++)
            if (use[i])
                tmp[j++] = res[i];
        qsort(tmp, (size_t)j, sizeof *tmp, cmp_float);
        *median = j ? tmp[j / 2] : 1e9f;
    }
    return inl;
}

/* Where the camera motion puts a raw homogeneous point in the previous
 * frame, for a draw with scale s. */
static void motion_apply(const double M[16], const double s[4], const double in[4],
                         double out[4])
{
    double n[4];
    int k;
    for (k = 0; k < 4; k++)
        n[k] = in[k] * s[k];
    for (k = 0; k < 4; k++)
        out[k] = (M[k * 4] * n[0] + M[k * 4 + 1] * n[1] + M[k * 4 + 2] * n[2]
                  + M[k * 4 + 3] * n[3]) / s[k];
}

/* Vertex by vertex, does a pairing hold? A pair is chosen from a few
 * numbers per draw (size, texture, pipeline, centroid), so two different
 * draws that agree on those (a left and a right hand on one mesh, a body
 * part drawn twice) can still be paired, and lerping one into the other
 * pulls its triangles across the screen. And a draw moved "with the camera"
 * that is really near the eye (the arms and gun are) can land behind it in
 * the previous frame, which stretches it to infinity. Up to 64 vertices
 * spread over the draw are looked at; one that disagrees by more than vlim
 * guest pixels (or whose depth flips or jumps) refuses the pairing, and the
 * draw is shown as drawn. */
#define DW_SANE_SAMPLES 64

/* The surface's size in guest pixels, from the draw's push constants. */
static void rec_surface(const Rec *r, float *w, float *h)
{
    *w = r->pc[0] > 0.0f ? 2.0f / r->pc[0] : 640.0f;
    *h = r->pc[1] > 0.0f ? 2.0f / r->pc[1] : 480.0f;
}

static int sane_proj(const double h[4], float sw, float sh, float *x, float *y)
{
    if (!(h[3] > DW_CLIP_W))
        return 0;
    *x = (float)(h[0] / h[3]);
    *y = (float)(h[1] / h[3]);
    /* Only what lands on the surface (with a quarter's margin) is judged: a
     * vertex near the eye plane projects far away and swings with the
     * smallest motion, and the clip takes care of it anyway. */
    return *x > -0.25f * sw && *x < 1.25f * sw && *y > -0.25f * sh && *y < 1.25f * sh;
}

/* mode 1: ra/la is the partner in the previous frame of rb/lb. cam: the
 * camera motion is valid (a vertex fixed in the world may move with it). */
static int pair_sane(const RecList *la, const Rec *ra, const RecList *lb, const Rec *rb,
                     int cam, float vlim)
{
    const Nv2aVertex *va = la->vtx + ra->first, *vb = lb->vtx + rb->first;
    uint32_t n = rb->count, step = n > DW_SANE_SAMPLES ? n / DW_SANE_SAMPLES : 1, k;
    double s[4];
    float sw, sh;
    if (ra->count != n)
        return 0;
    rec_surface(rb, &sw, &sh);
    vlim *= sw / 640.0f;                    /* the limits are in 640-wide pixels */
    if (cam)
        rec_scale(rb, s);
    for (k = 0; k < n; k += step) {
        double hb[4] = { vb[k].x, vb[k].y, vb[k].z, vb[k].rhw };
        double ha[4] = { va[k].x, va[k].y, va[k].z, va[k].rhw };
        float bx, by, ax, ay, d;
        if (!sane_proj(hb, sw, sh, &bx, &by) || !sane_proj(ha, sw, sh, &ax, &ay))
            continue;
        d = fmaxf(fabsf(ax - bx), fabsf(ay - by));
        if (d > vlim && cam) {
            double pr[4];
            float px, py;
            motion_apply(P.M, s, hb, pr);
            if (sane_proj(pr, sw, sh, &px, &py)) {
                float e = fmaxf(fabsf(ax - px), fabsf(ay - py));
                if (e < d)
                    d = e;
            }
        }
        if (d > vlim)
            return 0;
    }
    return 1;
}

/* mode 2: rb moved by the camera motion. */
static int reproj_sane(const RecList *lb, const Rec *rb, float lim)
{
    const Nv2aVertex *vb = lb->vtx + rb->first;
    uint32_t n = rb->count, step = n > DW_SANE_SAMPLES ? n / DW_SANE_SAMPLES : 1, k;
    double s[4];
    float sw, sh;
    rec_scale(rb, s);
    rec_surface(rb, &sw, &sh);
    lim *= sw / 640.0f;
    for (k = 0; k < n; k += step) {
        double hb[4] = { vb[k].x, vb[k].y, vb[k].z, vb[k].rhw }, pr[4], ratio;
        float bx, by, px, py;
        if (!(hb[3] > DW_CLIP_W))
            continue;
        motion_apply(P.M, s, hb, pr);
        if (!(pr[3] > DW_CLIP_W))
            return 0;                       /* behind the eye a frame ago */
        ratio = pr[3] / hb[3];
        if (ratio < 0.33 || ratio > 3.0)
            return 0;                       /* depth tripled in one frame: near the eye */
        if (sane_proj(hb, sw, sh, &bx, &by) && sane_proj(pr, sw, sh, &px, &py)
            && fmaxf(fabsf(px - bx), fabsf(py - by)) > lim)
            return 0;
    }
    return 1;
}

static int pair_prepare(const Slot *A, const Slot *B)
{
    static DrawInfo *ia, *ib;
    static uint32_t capa, capb;
    static uint8_t *used;
    static uint32_t capused;
    static FitPt *pts;
    static int cappts;
    static float lim = -1.0f, vlim = -1.0f;
    uint32_t i, j, na = A->list.nrec, nb = B->list.nrec, tv = 0, stride;
    unsigned paired = 0, reproj = 0, asdrawn = 0, teleports = 0, draws = 0;
    int np = 0, inl = 0;
    float med = 1e9f;

    if (P.ok_gen_cur == B->gen && P.ok_gen_prev == A->gen && P.mode)
        return P.pair_valid;
    /* Until this pair is done, mode/mi belong to nothing. */
    P.pair_valid = 0;
    P.ok_gen_cur = B->gen;
    P.ok_gen_prev = A->gen;
    if (lim < 0.0f) {
        const char *e = getenv("DW_INTERP_MAXMOVE");
        lim = e ? (float)atof(e) : 320.0f;
        e = getenv("DW_INTERP_MAXVERTEX");
        vlim = e ? (float)atof(e) : 128.0f;
    }
    if (nb > P.capok) {
        uint8_t *m = realloc(P.mode, nb);
        int32_t *x = realloc(P.mi, (size_t)nb * sizeof *x);
        if (m) P.mode = m;
        if (x) P.mi = x;
        if (!m || !x)
            return 0;
        P.capok = nb;
    }
    if (na > capused) {
        uint8_t *u = realloc(used, na ? na : 1);
        if (!u)
            return 0;
        used = u;
        capused = na;
    }
    draw_info(&A->list, &ia, &capa);
    draw_info(&B->list, &ib, &capb);
    if ((na && capa < na) || (nb && capb < nb))
        return 0;
    memset(used, 0, na);
    for (i = 0; i < nb; i++) {
        P.mode[i] = 0;
        P.mi[i] = -1;
    }

    /* 1: pairs that are unique on both sides: a table of h1 -> count in
     * each frame (and the one A index), then a lookup per B draw. */
    {
        uint32_t hs = 64;
        while (hs < 2 * (na + nb))
            hs <<= 1;
        if (hs > P.cap_ht) {
            uint32_t *k = realloc(P.ht_key, hs * sizeof *k);
            uint32_t *c = realloc(P.ht_cnt, hs * sizeof *c);
            int32_t *x = realloc(P.ht_idx, hs * sizeof *x);
            if (k) P.ht_key = k;
            if (c) P.ht_cnt = c;
            if (x) P.ht_idx = x;
            if (!k || !c || !x)
                return 0;
            P.cap_ht = hs;
        }
        memset(P.ht_cnt, 0, hs * sizeof *P.ht_cnt);
        /* ht_cnt: low 16 bits frames-A count, high 16 bits frame-B count */
        for (j = 0; j < na + nb; j++) {
            int isb = j >= na;
            uint32_t idx = isb ? j - na : j;
            const Rec *r = isb ? &B->list.rec[idx] : &A->list.rec[idx];
            uint32_t h = isb ? ib[idx].h1 : ia[idx].h1, slot = (h * 2654435761u) & (hs - 1);
            if (r->kind != 0)
                continue;
            while (P.ht_cnt[slot] && P.ht_key[slot] != h)
                slot = (slot + 1) & (hs - 1);
            P.ht_key[slot] = h;
            if (isb) {
                if ((P.ht_cnt[slot] >> 16) < 0xFFFF)
                    P.ht_cnt[slot] += 0x10000u;
            } else {
                if ((P.ht_cnt[slot] & 0xFFFF) < 0xFFFF)
                    P.ht_cnt[slot] += 1;
                P.ht_idx[slot] = (int32_t)idx;
            }
        }
        for (i = 0; i < nb; i++) {
            uint32_t h = ib[i].h1, slot = (h * 2654435761u) & (hs - 1);
            if (B->list.rec[i].kind != 0)
                continue;
            while (P.ht_cnt[slot] && P.ht_key[slot] != h)
                slot = (slot + 1) & (hs - 1);
            if (P.ht_cnt[slot] == 0x10001u) {
                int32_t a = P.ht_idx[slot];
                if (rec_same(&A->list.rec[a], &B->list.rec[i], 0)
                    && (!ib[i].flat || uv_same(&A->list, &A->list.rec[a], &B->list, &B->list.rec[i]))) {
                    P.mi[i] = a;
                    used[a] = 1;
                }
            }
        }
    }

    /* 2: the camera's motion, from those pairs' vertices (screen-space draws
     * left out). Up to ~6000 points, spread over all of them. */
    for (i = 0; i < nb; i++)
        if (P.mi[i] >= 0 && !ib[i].flat)
            tv += B->list.rec[i].count;
    stride = tv > 6000 ? tv / 6000 + 1 : 1;
    if (tv && cappts < (int)(tv / stride + 16)) {
        FitPt *p = realloc(pts, (tv / stride + 16) * sizeof *p);
        if (p) {
            pts = p;
            cappts = (int)(tv / stride + 16);
        }
    }
    P.mvalid = 0;
    if (pts && tv) {
        uint32_t phase = 0;
        for (i = 0; i < nb; i++) {
            const Rec *rb, *ra;
            const Nv2aVertex *vb, *va;
            double s[4];
            uint32_t k;
            if (P.mi[i] < 0 || ib[i].flat)
                continue;
            rb = &B->list.rec[i];
            ra = &A->list.rec[P.mi[i]];
            vb = B->list.vtx + rb->first;
            va = A->list.vtx + ra->first;
            rec_scale(rb, s);
            for (k = phase; k < rb->count && np < cappts; k += stride) {
                FitPt *q;
                int c;
                if (!(vb[k].rhw > DW_CLIP_W) || !(va[k].rhw > DW_CLIP_W))
                    continue;
                q = &pts[np++];
                q->c[0] = vb[k].x * s[0]; q->c[1] = vb[k].y * s[1];
                q->c[2] = vb[k].z * s[2]; q->c[3] = vb[k].rhw;
                q->p[0] = va[k].x * s[0]; q->p[1] = va[k].y * s[1];
                q->p[2] = va[k].z * s[2]; q->p[3] = va[k].rhw;
                q->sx = (float)(1.0 / s[0]);
                q->sy = (float)(1.0 / s[1]);
                for (c = 0; c < 4; c++)
                    if (!isfinite(q->c[c]) || !isfinite(q->p[c])) {
                        np--;
                        break;
                    }
            }
            /* Carry the stride on into the next draw. */
            phase = (uint32_t)((((int64_t)phase - (int64_t)rb->count) % (int64_t)stride
                                + (int64_t)stride) % (int64_t)stride);
        }
        inl = fit_motion(pts, np, P.M, &med);
        P.mvalid = inl >= 24 && inl * 4 >= np && med <= 1.5f;
    }
    if (!P.mvalid) {
        memset(P.M, 0, sizeof P.M);
        P.M[0] = P.M[5] = P.M[10] = P.M[15] = 1.0;
    }

    /* 3: everything else, by where the camera motion says it was: chains
     * of A draws per h2, in a table the size of the one above. */
    {
        uint32_t hs = P.cap_ht, hmask;
        while (hs > 64 && hs / 2 >= 2 * (na + nb))
            hs >>= 1;
        hmask = hs - 1;
        if (na > P.cap_next) {
            int32_t *x = realloc(P.next, (size_t)na * sizeof *x);
            if (!x)
                return 0;
            P.next = x;
            P.cap_next = na;
        }
        for (j = 0; j < hs; j++)
            P.ht_idx[j] = -1;
        for (j = na; j-- > 0; ) {
            uint32_t slot;
            if (A->list.rec[j].kind != 0 || used[j] || !ia[j].valid)
                continue;
            slot = (ia[j].h2 * 2654435761u) & hmask;
            P.next[j] = P.ht_idx[slot];
            P.ht_idx[slot] = (int32_t)j;
        }
        /* 3a: draws fixed in the world first, by the camera alone. A field
         * of identical ground tiles scrolling past (riding fast) puts the
         * tile beside each one where that one is now; matched by "where it
         * is now" it would take its neighbour's place and slide against the
         * tiles around it -- the tearing on the ground. With the camera's
         * motion known, a tile's own previous self is where the motion puts
         * it, within a few pixels; those claims are settled before any
         * nearest-anything guess can take their partners. */
        if (P.mvalid)
            for (i = 0; i < nb; i++) {
                const Rec *rb = &B->list.rec[i];
                double pr[4], s4[4];
                float cx, cy, best = 4.0f;
                int32_t bj = -1, jj;
                if (rb->kind != 0 || P.mi[i] >= 0 || !ib[i].valid || ib[i].flat)
                    continue;
                rec_scale(rb, s4);
                motion_apply(P.M, s4, ib[i].c, pr);
                if (!(pr[3] > DW_CLIP_W))
                    continue;
                cx = (float)(pr[0] / pr[3]);
                cy = (float)(pr[1] / pr[3]);
                for (jj = P.ht_idx[(ib[i].h2 * 2654435761u) & hmask]; jj >= 0; jj = P.next[jj]) {
                    float e;
                    if (used[jj] || ia[jj].h2 != ib[i].h2 || !rec_same(&A->list.rec[jj], rb, 1))
                        continue;
                    e = fmaxf(fabsf(ia[jj].x - cx), fabsf(ia[jj].y - cy));
                    if (e < best) {
                        best = e;
                        bj = jj;
                    }
                }
                if (bj >= 0) {
                    P.mi[i] = bj;
                    used[bj] = 1;
                    P.st_world++;
                }
            }
        for (i = 0; i < nb; i++) {
            const Rec *rb = &B->list.rec[i];
            double pr[4], s4[4];
            /* Two guesses at where it was: where it is now (it is fixed on
             * the screen, or still), and where the camera motion puts it
             * (it is fixed in the world). The nearer candidate under either
             * wins. */
            float px = ib[i].x, py = ib[i].y, cx = 0.0f, cy = 0.0f, best = 48.0f;
            int32_t bj = -1, jj, cam = 0;
            if (rb->kind != 0 || P.mi[i] >= 0 || !ib[i].valid)
                continue;
            if (P.mvalid) {
                rec_scale(rb, s4);
                motion_apply(P.M, s4, ib[i].c, pr);
                if (pr[3] > DW_CLIP_W) {
                    cx = (float)(pr[0] / pr[3]);
                    cy = (float)(pr[1] / pr[3]);
                    cam = 1;
                }
            }
            for (jj = P.ht_idx[(ib[i].h2 * 2654435761u) & hmask]; jj >= 0; jj = P.next[jj]) {
                float d, e;
                if (used[jj] || ia[jj].h2 != ib[i].h2 || !rec_same(&A->list.rec[jj], rb, 1))
                    continue;
                if (ib[i].flat && !uv_same(&A->list, &A->list.rec[jj], &B->list, rb))
                    continue;
                d = fmaxf(fabsf(ia[jj].x - px), fabsf(ia[jj].y - py));
                if (cam) {
                    e = fmaxf(fabsf(ia[jj].x - cx), fabsf(ia[jj].y - cy));
                    if (e < d)
                        d = e;
                }
                if (d < best) {
                    best = d;
                    bj = jj;
                }
            }
            if (bj >= 0) {
                P.mi[i] = bj;
                used[bj] = 1;
            }
        }
    }

    /* 4: a mode for each. */
    for (i = 0; i < nb; i++) {
        const Rec *rb = &B->list.rec[i];
        if (rb->kind != 0)
            continue;
        draws++;
        if (P.mi[i] >= 0) {
            const DrawInfo *a = &ia[P.mi[i]];
            float own = 0.0f, dev = 0.0f;
            if (a->valid && ib[i].valid) {
                double s[4], pr[4];
                own = fmaxf(fabsf(a->x - ib[i].x), fabsf(a->y - ib[i].y));
                dev = own;
                if (P.mvalid && !ib[i].flat) {
                    rec_scale(rb, s);
                    motion_apply(P.M, s, ib[i].c, pr);
                    if (pr[3] > DW_CLIP_W)
                        dev = fmaxf(fabsf(a->x - (float)(pr[0] / pr[3])),
                                    fabsf(a->y - (float)(pr[1] / pr[3])));
                }
            }
            /* A centroid with vertices behind the eye can swing anywhere,
             * so only a draw wholly in front can be judged a teleport. */
            if (!pair_sane(&A->list, &A->list.rec[P.mi[i]], &B->list, rb,
                           P.mvalid && !ib[i].flat, vlim)) {
                P.mode[i] = 0;              /* the vertices say it is not the same draw */
                asdrawn++;
                P.st_bad_pair++;
            } else if (own <= lim || dev <= lim || !a->front || !ib[i].front) {
                P.mode[i] = 1;
                paired++;
            } else {
                P.mode[i] = 0;
                teleports++;
            }
        } else if (P.mvalid && !ib[i].flat) {
            if (reproj_sane(&B->list, rb, lim)) {
                P.mode[i] = 2;
                reproj++;
            } else {
                asdrawn++;
                P.st_bad_reproj++;
            }
        } else {
            asdrawn++;
        }
    }
    /* A cut: most pairs jumped. Show the new frame as drawn. */
    P.ok_cut = paired + teleports >= 8 && teleports * 2 > paired + teleports;
    P.pair_valid = 1;
    P.st_pairs_draws += draws;
    P.st_pairs_ok += P.ok_cut ? 0 : paired;
    P.st_reproj += P.ok_cut ? 0 : reproj;
    P.st_fits += P.mvalid;
    P.st_fit_tries++;
    if (P.ok_cut)
        P.st_cuts++;
    {
        static int verbose = -1;
        static unsigned shown;
        if (verbose < 0)
            verbose = getenv("DW_INTERP_VERBOSE") != NULL;
        if (verbose && shown++ % 30 == 0) {
            fprintf(stderr, "[VK] interp pair: %u draws: %u paired, %u camera, %u as drawn, "
                    "%u teleports; fit %d/%d points, median %.2f px%s\n", draws, paired,
                    reproj, asdrawn, teleports, inl, np, med, P.mvalid ? "" : " (not used)");
            if (verbose > 1 || getenv("DW_INTERP_VERBOSE")[0] == '2')
                for (i = 0; i < nb; i++) {
                    const Rec *rb = &B->list.rec[i];
                    const Nv2aVertex *v = B->list.vtx + rb->first;
                    float wmin = 1e30f, wmax = -1e30f;
                    uint32_t k;
                    if (rb->kind != 0 || rb->count > 24)
                        continue;
                    for (k = 0; k < rb->count; k++) {
                        if (v[k].rhw < wmin) wmin = v[k].rhw;
                        if (v[k].rhw > wmax) wmax = v[k].rhw;
                    }
                    fprintf(stderr, "[VK]   draw %u: mode %u partner %d n %u tex %08X key %08X "
                            "w %g..%g flat %d at %.1f,%.1f", i, P.mode[i], P.mi[i], rb->count,
                            rb->tex_va, rb->key, wmin, wmax, ib[i].flat, ib[i].x, ib[i].y);
                    if (P.mi[i] >= 0)
                        fprintf(stderr, " was %.1f,%.1f", ia[P.mi[i]].x, ia[P.mi[i]].y);
                    fprintf(stderr, "\n");
                }
        }
    }
    return 1;
}

static int replay_targets(uint32_t w, uint32_t h, uint32_t bpp)
{
    if (P.img.image && P.iw == w && P.ih == h && P.ibpp == bpp)
        return 1;
    if (P.img.image) {
        img_destroy(&P.img);
        img_destroy(&P.depth);
    }
    if (!img_create(&P.img, w, h, color_format(bpp),
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT)
        || !img_create(&P.depth, w, h, V.depth_fmt,
                       VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                       VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
        img_destroy(&P.img);
        img_destroy(&P.depth);
        return 0;
    }
    P.iw = w;
    P.ih = h;
    P.ibpp = bpp;
    return 1;
}

/* Draw B's records into P.img, each paired draw's vertices `alpha` of the
 * way from where A had them. Recorded into P.cmd; leaves P.img ready to be
 * copied from. Returns 0 if it could not finish (the image is then not
 * shown). */
static int replay(const Slot *A, const Slot *B, float alpha)
{
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    VkPipeline bound = VK_NULL_HANDLE;
    VkImageView bview = VK_NULL_HANDLE;
    VkSampler bsmp = VK_NULL_HANDLE;
    VkClearRect rect;
    uint32_t i, W = B->rw, H = B->rh;
    int ok = 1, blend = A && !P.ok_cut && alpha < 0.999f && P.mode && P.pair_valid
                        && P.ok_gen_cur == B->gen && P.ok_gen_prev == A->gen
                        && P.capok >= B->list.nrec;

    if (!replay_targets(W, H, B->bpp))
        return 0;
    vkResetDescriptorPool(V.dev, P.dpool, 0);
    P.verts.used = 0;
    P.idx.used = 0;
    img_layout_cmd(P.cmd, &P.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_ASPECT_COLOR_BIT);
    img_layout_cmd(P.cmd, &P.depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                   VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    ca.imageView = P.img.view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ca.clearValue.color.float32[3] = 1.0f;
    da.imageView = P.depth.view;
    da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    da.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    da.clearValue.depthStencil.depth = 1.0f;
    ri.renderArea.extent.width = W;
    ri.renderArea.extent.height = H;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    ri.pDepthAttachment = &da;
    ri.pStencilAttachment = &da;
    vkCmdBeginRendering(P.cmd, &ri);
    {
        VkViewport vp = { 0, 0, (float)W, (float)H, 0.0f, 1.0f };
        VkRect2D sc = { { 0, 0 }, { W, H } };
        vkCmdSetViewport(P.cmd, 0, 1, &vp);
        vkCmdSetScissor(P.cmd, 0, 1, &sc);
    }
    memset(&rect, 0, sizeof rect);
    rect.rect.extent.width = W;
    rect.rect.extent.height = H;
    rect.layerCount = 1;

    for (i = 0; i < B->list.nrec && ok; i++) {
        const Rec *r = &B->list.rec[i];
        const Nv2aVertex *src;
        Nv2aVertex *dst;
        uint32_t n, off, ioff = 0, k;
        VkDeviceSize vo;

        if (r->kind == 1) {
            vkCmdClearAttachments(P.cmd, r->nclear, r->at, 1, &rect);
            continue;
        }
        n = r->count;
        src = B->list.vtx + r->first;
        if (blend && P.mode[i] == 1
            && ((uint32_t)P.mi[i] >= A->list.nrec || A->list.rec[P.mi[i]].count != n))
            P.mode[i] = 0;                  /* never: a partner that is not one */
        /* Straight into the vertex ring (write-only: it can be uncached
         * memory), whole vertices at a time; the GPU clips. */
        off = ring_alloc(&P.verts, n * (uint32_t)sizeof(Nv2aVertex), 16);
        if (off != UINT32_MAX && r->icount)
            ioff = ring_alloc(&P.idx, r->icount * (uint32_t)sizeof(uint16_t), 16);
        if (off == UINT32_MAX || ioff == UINT32_MAX) {
            ok = 0;
            break;
        }
        dst = (Nv2aVertex *)(P.verts.ptr + off);
        if (blend && P.mode[i] == 1) {
            /* From its partner in the previous frame. */
            const Nv2aVertex *pa = A->list.vtx + A->list.rec[P.mi[i]].first;
            for (k = 0; k < n; k++) {
                Nv2aVertex t = src[k];
                t.x = pa[k].x + (src[k].x - pa[k].x) * alpha;
                t.y = pa[k].y + (src[k].y - pa[k].y) * alpha;
                t.z = pa[k].z + (src[k].z - pa[k].z) * alpha;
                t.rhw = pa[k].rhw + (src[k].rhw - pa[k].rhw) * alpha;
                dst[k] = t;
            }
        } else if (blend && P.mode[i] == 2) {
            /* From where the camera motion puts it: M in this draw's
             * raw units is S^-1 M S. */
            double sc[4];
            float m[16], bb = 1.0f - alpha;
            int u, v;
            rec_scale(r, sc);
            for (u = 0; u < 4; u++)
                for (v = 0; v < 4; v++)
                    m[u * 4 + v] = (float)(P.M[u * 4 + v] * sc[v] / sc[u]);
            for (k = 0; k < n; k++) {
                const Nv2aVertex *c = &src[k];
                float px = m[0] * c->x + m[1] * c->y + m[2] * c->z + m[3] * c->rhw;
                float py = m[4] * c->x + m[5] * c->y + m[6] * c->z + m[7] * c->rhw;
                float pz = m[8] * c->x + m[9] * c->y + m[10] * c->z + m[11] * c->rhw;
                float pw = m[12] * c->x + m[13] * c->y + m[14] * c->z + m[15] * c->rhw;
                Nv2aVertex t = *c;
                t.x = px * bb + c->x * alpha;
                t.y = py * bb + c->y * alpha;
                t.z = pz * bb + c->z * alpha;
                t.rhw = pw * bb + c->rhw * alpha;
                dst[k] = t;
            }
        } else {
            memcpy(dst, src, (size_t)n * sizeof *dst);
        }
        if (r->icount)
            memcpy(P.idx.ptr + ioff, B->list.idx + r->ifirst, (size_t)r->icount * sizeof(uint16_t));
        if (r->pipe != bound) {
            vkCmdBindPipeline(P.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->pipe);
            bound = r->pipe;
        }
        if (r->view != bview || r->smp != bsmp) {
            VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
            VkDescriptorImageInfo ii;
            VkWriteDescriptorSet wr = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
            VkDescriptorSet set;
            dai.descriptorPool = P.dpool;
            dai.descriptorSetCount = 1;
            dai.pSetLayouts = &V.dsl;
            if (vkAllocateDescriptorSets(V.dev, &dai, &set) != VK_SUCCESS) {
                ok = 0;
                break;
            }
            memset(&ii, 0, sizeof ii);
            ii.sampler = r->smp;
            ii.imageView = r->view;
            ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            wr.dstSet = set;
            wr.descriptorCount = 1;
            wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            wr.pImageInfo = &ii;
            vkUpdateDescriptorSets(V.dev, 1, &wr, 0, NULL);
            vkCmdBindDescriptorSets(P.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, V.layout,
                                    0, 1, &set, 0, NULL);
            bview = r->view;
            bsmp = r->smp;
        }
        vkCmdSetStencilCompareMask(P.cmd, VK_STENCIL_FACE_FRONT_AND_BACK, r->smask);
        vkCmdSetStencilWriteMask(P.cmd, VK_STENCIL_FACE_FRONT_AND_BACK, r->wmask);
        vkCmdSetStencilReference(P.cmd, VK_STENCIL_FACE_FRONT_AND_BACK, r->sref);
        vkCmdSetBlendConstants(P.cmd, r->bc);
        vkCmdPushConstants(P.cmd, V.layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof r->pc, r->pc);
        vo = off;
        vkCmdBindVertexBuffers(P.cmd, 0, 1, &P.verts.buf, &vo);
        if (r->icount) {
            vkCmdBindIndexBuffer(P.cmd, P.idx.buf, ioff, VK_INDEX_TYPE_UINT16);
            vkCmdDrawIndexed(P.cmd, r->icount, 1, 0, 0, 0);
        } else {
            vkCmdDraw(P.cmd, n, 1, 0, 0);
        }
    }
    vkCmdEndRendering(P.cmd);
    img_layout_cmd(P.cmd, &P.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    return ok;
}

/* src (TRANSFER_SRC, sw x sh) into swapchain image idx: 4:3 in the middle,
 * black around it, then ready to present. */
static void sc_blit(VkCommandBuffer cmd, VkImage src, uint32_t sw, uint32_t sh, uint32_t idx)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    VkClearColorValue black = { { 0.0f, 0.0f, 0.0f, 1.0f } };
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageBlit blit;
    uint32_t dw, dh, dx, dy;

    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.subresourceRange = range;
    b.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.image = V.sc_images[idx];
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    vkCmdClearColorImage(cmd, V.sc_images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &black, 1, &range);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    sc_rect(&dx, &dy, &dw, &dh);
    memset(&blit, 0, sizeof blit);
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.layerCount = 1;
    blit.srcOffsets[1].x = (int32_t)sw;
    blit.srcOffsets[1].y = (int32_t)sh;
    blit.srcOffsets[1].z = 1;
    blit.dstSubresource = blit.srcSubresource;
    blit.dstOffsets[0].x = (int32_t)dx;
    blit.dstOffsets[0].y = (int32_t)dy;
    blit.dstOffsets[1].x = (int32_t)(dx + dw);
    blit.dstOffsets[1].y = (int32_t)(dy + dh);
    blit.dstOffsets[1].z = 1;
    vkCmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, V.sc_images[idx],
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

/* DW_INTERP_TEST: copy an image (TRANSFER_SRC) into the read-back buffer at
 * byte offset off. */
static void rb_copy(VkImage img, uint32_t w, uint32_t h, uint32_t off)
{
    VkBufferImageCopy c;
    memset(&c, 0, sizeof c);
    c.bufferOffset = off;
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = w;
    c.imageExtent.height = h;
    c.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(P.cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, P.rb.buf, 1, &c);
}

static double img_diff(const uint8_t *a, const uint8_t *b, size_t bytes)
{
    uint64_t sum = 0;
    size_t i;
    for (i = 0; i < bytes; i++)
        sum += (uint64_t)(a[i] > b[i] ? a[i] - b[i] : b[i] - a[i]);
    return bytes ? (double)sum / (double)bytes : 0.0;
}

static void *presenter_main(void *arg)
{
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    uint64_t tick = now_ns();
    unsigned n = 0, dumps = 0;
    long every = 90;
    uint64_t dump_after = 0;

    (void)arg;
    if (P.test) {
        const char *e = getenv("DW_INTERP_TEST_EVERY");
        const char *a = getenv("DW_INTERP_TEST_AFTER");   /* seconds from now */
        if (e && atol(e) > 0)
            every = atol(e);
        if (a)
            dump_after = now_ns() + (uint64_t)(atof(a) * 1e9);
    }
    P.st_t0 = now_ns();
    for (;;) {
        int cur, prev, replay_ok, blend_ok, shown_replay = 0, fid = 0, dump = 0;
        Slot *A = NULL, *B;
        float alpha;
        uint32_t idx = 0;
        uint64_t now;

        if (!V.present) {
            /* No display: pace at 60 Hz. */
            tick += 16666667ull;
            now = now_ns();
            if (tick > now)
                usleep((useconds_t)((tick - now) / 1000));
            else
                tick = now;
        } else {
            VkResult r;
            if (O.on) {
                usleep(4000);                   /* the host's overlay owns the display */
                continue;
            }
            pthread_mutex_lock(&s_present_mu);
            if (O.on || !sc_rebuild()) {
                pthread_mutex_unlock(&s_present_mu);
                usleep(O.on ? 4000 : 16000);    /* overlay, or minimised */
                continue;
            }
            r = vkAcquireNextImageKHR(V.dev, V.swapchain, UINT64_MAX, V.sem_acquire,
                                      VK_NULL_HANDLE, &idx);
            sc_note(r);
            if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
                pthread_mutex_unlock(&s_present_mu);
                usleep(2000);
                continue;
            }
        }
        n++;
        fid = P.test && n % 30 == 15;
        dump = P.test && n % (unsigned)every == 0 && now_ns() >= dump_after;

        pthread_mutex_lock(&P.mu);
        while (P.latest < 0)
            pthread_cond_wait(&P.cv, &P.mu);
        cur = P.latest;
        prev = P.slot[cur].prev;
        B = &P.slot[cur];
        A = prev >= 0 ? &P.slot[prev] : NULL;
        replay_ok = B->replayable && B->frame >= P.no_interp_before;
        blend_ok = replay_ok && A && A->replayable && A->frame >= P.no_interp_before
                   && A->rw == B->rw && A->rh == B->rh;
        now = now_ns();
        alpha = P.interval_ns ? (float)((double)(now - B->t_ns) / (double)P.interval_ns) : 1.0f;
        if (alpha < 0.0f) alpha = 0.0f;
        if (alpha > 1.0f) alpha = 1.0f;
        if (fid)
            alpha = 1.0f;
        P.pin[0] = cur;
        P.pin[1] = prev;
        P.busy = replay_ok;
        pthread_mutex_unlock(&P.mu);

        vkWaitForFences(V.dev, 1, &P.fence, VK_TRUE, UINT64_MAX);
        vkResetFences(V.dev, 1, &P.fence);
        vkResetCommandBuffer(P.cmd, 0);
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(P.cmd, &bi);
        /* A replay that blends nothing re-renders the newest frame exactly
         * as drawn: show the drawn picture instead. That is the case once
         * the next game frame is late (alpha reaches 1) -- in a busy scene,
         * where re-rendering the whole frame at every display refresh took
         * GPU time from the late frame itself. */
        if (replay_ok && !P.test && !(blend_ok && alpha < 0.999f))
            replay_ok = 0;
        if (replay_ok) {
            uint64_t t0 = now_ns();
            if (blend_ok && !pair_prepare(A, B))
                blend_ok = 0;
            shown_replay = replay(blend_ok && !fid ? A : NULL, B, alpha);
            P.st_cpu_ns += now_ns() - t0;
        }
        if (V.present) {
            if (shown_replay)
                sc_blit(P.cmd, P.img.image, P.iw, P.ih, idx);
            else
                sc_blit(P.cmd, B->real.image, B->rw, B->rh, idx);
        }
        if (P.test && (fid || dump) && shown_replay) {
            uint32_t sz = B->rw * B->rh * B->bpp;
            rb_copy(P.img.image, B->rw, B->rh, 0);
            rb_copy(B->real.image, B->rw, B->rh, sz);
            if (A && A->rw == B->rw && A->rh == B->rh)
                rb_copy(A->real.image, A->rw, A->rh, 2 * sz);
        }
        vkEndCommandBuffer(P.cmd);
        if (V.present) {
            VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
            submit_locked(P.cmd, P.fence, &V.sem_acquire, &V.sem_done);
            /* The picture is finished before presenting: the queue lock is
             * held across vkQueuePresentKHR, and a present that had to wait
             * for rendering would hold the executor's submissions up. */
            vkWaitForFences(V.dev, 1, &P.fence, VK_TRUE, UINT64_MAX);
            pi.waitSemaphoreCount = 1;
            pi.pWaitSemaphores = &V.sem_done;
            pi.swapchainCount = 1;
            pi.pSwapchains = &V.swapchain;
            pi.pImageIndices = &idx;
            {
                uint64_t t0 = now_ns(), dt;
                VkResult pr;
                pthread_mutex_lock(&P.qmu);
                pr = vkQueuePresentKHR(V.queue, &pi);
                pthread_mutex_unlock(&P.qmu);
                sc_note(pr);
                s_display_frames++;
                pthread_mutex_unlock(&s_present_mu);
                dt = now_ns() - t0;
                P.st_present_ns += dt;
                if (dt > P.st_present_max)
                    P.st_present_max = dt;
            }
        } else {
            submit_locked(P.cmd, P.fence, NULL, NULL);
        }
        vkWaitForFences(V.dev, 1, &P.fence, VK_TRUE, UINT64_MAX);

        /* The frame-rate cap: the next present no sooner than 1/cap after
         * this one. A timer sleep to within ~1 ms, then a short spin. */
        if (V.present && s_fps_cap > 0) {
            static uint64_t next_ns;
            uint64_t period = 1000000000ull / (uint64_t)s_fps_cap, t = now_ns();
            if (!next_ns || t > next_ns + period)
                next_ns = t;
            next_ns += period;
            while ((t = now_ns()) < next_ns) {
                uint64_t left = next_ns - t;
                if (left > 1500000ull)
                    dw_nap_us((unsigned)((left - 1000000ull) / 1000ull));
                else
                    __builtin_ia32_pause();
            }
        }

        if (P.test && (fid || dump) && shown_replay) {
            uint32_t sz = B->rw * B->rh * B->bpp;
            if (fid) {
                P.st_fid += img_diff(P.rb.ptr, P.rb.ptr + sz, sz);
                P.st_fid_n++;
            } else {
                char path[512];
                snprintf(path, sizeof path, "%s%04u_i%03d.bmp", P.test, dumps,
                         (int)(alpha * 100.0f + 0.5f));
                write_bmp(path, P.rb.ptr, B->rw, B->rh, B->rw * B->bpp, B->bpp);
                snprintf(path, sizeof path, "%s%04u_cur.bmp", P.test, dumps);
                write_bmp(path, P.rb.ptr + sz, B->rw, B->rh, B->rw * B->bpp, B->bpp);
                if (A && A->rw == B->rw && A->rh == B->rh) {
                    snprintf(path, sizeof path, "%s%04u_prev.bmp", P.test, dumps);
                    write_bmp(path, P.rb.ptr + 2 * sz, B->rw, B->rh, B->rw * B->bpp, B->bpp);
                }
                dumps++;
            }
        }

        pthread_mutex_lock(&P.mu);
        P.busy = 0;
        P.pin[0] = P.pin[1] = -1;
        pthread_cond_broadcast(&P.cv);
        pthread_mutex_unlock(&P.mu);

        if (!fid) {
            P.st_frames++;
            if (shown_replay) {
                P.st_replayed++;
                if (blend_ok && alpha < 0.999f && !P.ok_cut)
                    P.st_blended++;
            } else {
                P.st_real++;
            }
        }
        now = now_ns();
        if (now - P.st_t0 >= 10000000000ull) {
            fprintf(stderr, "[VK] interp 10s: %u shown (%u replayed, %u of them blended, "
                    "%u as drawn); frame interval %.1f ms; draws paired %u (%u by the camera), moved with "
                    "the camera %u, of %u; camera fitted in %u of %u frames; "
                    "%u cuts; refused %u pairs and %u camera moves by their vertices; "
                    "replay vs drawn %.3f/255 (%u checks); recording %.2f ms "
                    "a frame; present call %.2f ms avg, %.2f max\n",
                    P.st_frames, P.st_replayed, P.st_blended, P.st_real,
                    P.interval_ns / 1e6, P.st_pairs_ok, P.st_world, P.st_reproj, P.st_pairs_draws,
                    P.st_fits, P.st_fit_tries, P.st_cuts, P.st_bad_pair, P.st_bad_reproj,
                    P.st_fid_n ? P.st_fid / P.st_fid_n : 0.0, P.st_fid_n,
                    P.st_frames ? P.st_cpu_ns / 1e6 / P.st_frames : 0.0,
                    P.st_frames ? P.st_present_ns / 1e6 / P.st_frames : 0.0,
                    P.st_present_max / 1e6);
            fflush(stderr);
            P.st_present_ns = P.st_present_max = P.st_cpu_ns = 0;
            P.st_frames = P.st_replayed = P.st_blended = P.st_real = 0;
            P.st_pairs_ok = P.st_pairs_draws = P.st_cuts = 0;
            P.st_reproj = P.st_fits = P.st_fit_tries = P.st_world = 0;
            P.st_bad_pair = P.st_bad_reproj = 0;
            P.st_fid = 0.0;
            P.st_fid_n = 0;
            P.st_t0 = now;
        }
    }
    return NULL;
}

/* ── an overlay from the host (the in-game settings) ─────────────── */

/* While the host shows a picture of its own over the game, the presenter and
 * the flip stop presenting and the host's thread presents that picture
 * (dwvk_overlay_present). One lock covers every acquire..present. */


/* px NULL: off. The picture is copied. */
void dwvk_overlay_set(const uint32_t *px, int w, int h)
{
    if (!px) {
        O.on = 0;
        return;
    }
    pthread_mutex_lock(&O.mu);
    if (w * h > O.cap) {
        free(O.px);
        O.px = (uint32_t *)malloc((size_t)w * h * 4);
        O.cap = O.px ? w * h : 0;
    }
    if (O.px) {
        memcpy(O.px, px, (size_t)w * h * 4);
        O.w = w;
        O.h = h;
        O.on = 1;
    }
    pthread_mutex_unlock(&O.mu);
}

int dwvk_overlay_present(void)
{
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    VkBufferImageCopy c;
    VkImageBlit blit;
    uint32_t idx, need;
    int w, h;
    VkResult r;

    if (!V.ok || !V.present || !O.on)
        return 0;
    pthread_mutex_lock(&s_present_mu);
    if (!O.pool) {
        VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cpi.queueFamilyIndex = V.qfam;
        if (vkCreateCommandPool(V.dev, &cpi, NULL, &O.pool) != VK_SUCCESS)
            goto fail;
        cai.commandPool = O.pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        vkAllocateCommandBuffers(V.dev, &cai, &O.cmd);
        vkCreateFence(V.dev, &fci, NULL, &O.fence);
    }
    if (!sc_rebuild())
        goto fail;
    pthread_mutex_lock(&O.mu);
    w = O.w;
    h = O.h;
    need = (uint32_t)(w * h * 4);
    if (need > O.size) {
        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        VkMemoryRequirements mr;
        VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        int t;
        if (O.buf) {
            vkDestroyBuffer(V.dev, O.buf, NULL);
            vkFreeMemory(V.dev, O.mem, NULL);
            O.buf = VK_NULL_HANDLE;
            O.size = 0;
        }
        bci.size = need;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        if (vkCreateBuffer(V.dev, &bci, NULL, &O.buf) == VK_SUCCESS) {
            vkGetBufferMemoryRequirements(V.dev, O.buf, &mr);
            t = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                            | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            ai.allocationSize = mr.size;
            ai.memoryTypeIndex = (uint32_t)(t < 0 ? 0 : t);
            if (t >= 0 && vkAllocateMemory(V.dev, &ai, NULL, &O.mem) == VK_SUCCESS
                && vkBindBufferMemory(V.dev, O.buf, O.mem, 0) == VK_SUCCESS
                && vkMapMemory(V.dev, O.mem, 0, VK_WHOLE_SIZE, 0, (void **)&O.ptr) == VK_SUCCESS)
                O.size = need;
        }
    }
    if (O.size >= need)
        memcpy(O.ptr, O.px, need);
    pthread_mutex_unlock(&O.mu);
    if (O.size < need)
        goto fail;
    if (O.iw != w || O.ih != h || !O.img.image) {
        if (O.img.image) {
            pthread_mutex_lock(&P.qmu);
            vkQueueWaitIdle(V.queue);
            pthread_mutex_unlock(&P.qmu);
            img_destroy(&O.img);
        }
        if (!img_create(&O.img, (uint32_t)w, (uint32_t)h, VK_FORMAT_B8G8R8A8_UNORM,
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT))
            goto fail;
        O.iw = w;
        O.ih = h;
    }
    r = vkAcquireNextImageKHR(V.dev, V.swapchain, UINT64_MAX, V.sem_acquire, VK_NULL_HANDLE, &idx);
    sc_note(r);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
        goto fail;
    vkResetFences(V.dev, 1, &O.fence);
    vkResetCommandBuffer(O.cmd, 0);
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(O.cmd, &bi);
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.subresourceRange = range;
    b.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.image = O.img.image;
    vkCmdPipelineBarrier(O.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &b);
    b.image = V.sc_images[idx];
    vkCmdPipelineBarrier(O.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &b);
    memset(&c, 0, sizeof c);
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = (uint32_t)w;
    c.imageExtent.height = (uint32_t)h;
    c.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(O.cmd, O.buf, O.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.image = O.img.image;
    vkCmdPipelineBarrier(O.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &b);
    memset(&blit, 0, sizeof blit);
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.layerCount = 1;
    blit.srcOffsets[1].x = w;
    blit.srcOffsets[1].y = h;
    blit.srcOffsets[1].z = 1;
    blit.dstSubresource = blit.srcSubresource;
    blit.dstOffsets[1].x = (int32_t)V.sc_extent.width;
    blit.dstOffsets[1].y = (int32_t)V.sc_extent.height;
    blit.dstOffsets[1].z = 1;
    vkCmdBlitImage(O.cmd, O.img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, V.sc_images[idx],
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.image = V.sc_images[idx];
    vkCmdPipelineBarrier(O.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, NULL, 0, NULL, 1, &b);
    vkEndCommandBuffer(O.cmd);
    submit_locked(O.cmd, O.fence, &V.sem_acquire, &V.sem_done);
    vkWaitForFences(V.dev, 1, &O.fence, VK_TRUE, UINT64_MAX);
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &V.sem_done;
    pi.swapchainCount = 1;
    pi.pSwapchains = &V.swapchain;
    pi.pImageIndices = &idx;
    pthread_mutex_lock(&P.qmu);
    r = vkQueuePresentKHR(V.queue, &pi);
    pthread_mutex_unlock(&P.qmu);
    sc_note(r);
    pthread_mutex_unlock(&s_present_mu);
    return 1;
fail:
    pthread_mutex_unlock(&s_present_mu);
    return 0;
}

int dwvk_present(uint32_t fb_va, uint32_t pitch, uint32_t height);

/* DW_HEADLESS_PRESENT: the PC has no flip hook into this file, so the
 * 640x480 surface drawn this frame is presented as the console's flip
 * would. */
static void present_drawn_surface(void)
{
    int i;
    for (i = 0; i < MAX_TARGETS; i++) {
        Target *t = &V.targets[i];
        if (t->used && t->frame == V.frame && t->w == 640 && t->h == 480) {
            dwvk_present(t->va, t->pitch, t->h);
            return;
        }
    }
}

/* DW_INTERP_TEST: no display; the 640x480 surface drawn this frame stands
 * for the one the flip shows. */
static void interp_publish_test(void)
{
    int i;
    for (i = 0; i < MAX_TARGETS; i++) {
        Target *t = &V.targets[i];
        if (t->used && t->frame == V.frame && t->w == 640 && t->h == 480 && t->wb_valid) {
            interp_publish(t->va, t->pitch, t->h);
            return;
        }
    }
}

int dwvk_present(uint32_t fb_va, uint32_t pitch, uint32_t height)
{
    const uint32_t w = 640;
    uint32_t bpp = pitch / w, h = height ? height : 480, idx, off, y;
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    VkClearColorValue black = { { 0.0f, 0.0f, 0.0f, 1.0f } };
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkBufferImageCopy c;
    VkImageBlit blit;
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    Img *src;
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t dw, dh, dx, dy, sw = 0, sh = 0;
    VkImage srcimg = VK_NULL_HANDLE;
    Target *hi = NULL;
    VkResult r;

    if (!V.ok || !V.present || (bpp != 2 && bpp != 4) || !guest_range_ok(fb_va, pitch * h))
        return 0;
    /* Interpolating: the presenter thread shows frames at the display's
     * rate; this only hands the finished one over. */
    if (interp_on()) {
        interp_publish(fb_va, pitch, h);
        if (++V.presents == 1)
            vk_log("%s", "present: first frame handed to the presenter");
        return 1;
    }
    /* With DW_RES_SCALE the console-sized copy in guest memory is a
     * downscale of what was actually drawn; the large image this surface was
     * rendered into is still here, so show that instead. */
    if (res_scale() > 1) {
        int i;
        for (i = 0; i < MAX_TARGETS; i++) {
            Target *t = &V.targets[i];
            /* Only while guest memory still holds the downscale of it: a
             * frame the CPU wrote (or one drawn some other way) is shown
             * from guest memory as before. */
            if (t->used && t->scale > 1 && t->va == fb_va && t->w == w
                && t->h == h && t->bpp == bpp && t->wb_valid && !t->loaded
                && rows_sum(mem + fb_va, w * bpp, h, pitch) == t->wb_sum) {
                hi = t;
                break;
            }
        }
    }
    if (O.on)
        return 1;                               /* the host's overlay is up */
    src = &V.src[bpp == 4];
    if (!src->image && !img_create(src, w, h, color_format(bpp),
                                   VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                   VK_IMAGE_ASPECT_COLOR_BIT))
        return 0;

    pthread_mutex_lock(&s_present_mu);
    vkWaitForFences(V.dev, 1, &V.pfence, VK_TRUE, UINT64_MAX);
    if (!sc_rebuild()) {
        pthread_mutex_unlock(&s_present_mu);
        return 0;                               /* minimised */
    }
    r = vkAcquireNextImageKHR(V.dev, V.swapchain, UINT64_MAX, V.sem_acquire, VK_NULL_HANDLE, &idx);
    sc_note(r);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        pthread_mutex_unlock(&s_present_mu);
        return 0;
    }
    vkResetFences(V.dev, 1, &V.pfence);

    vkResetCommandBuffer(V.pcmd, 0);
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(V.pcmd, &bi);
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.subresourceRange = range;
    if (hi) {
        /* The large image, straight to the display. Rendering finished at the
         * flush before this flip (flush waits on its fence), so no barrier
         * against the renderer's queue submission is needed beyond the layout
         * change this records. */
        srcimg = hi->img.image;
        sw = hi->w * hi->scale;
        sh = hi->h * hi->scale;
        b.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.oldLayout = hi->img.layout;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.image = srcimg;
        vkCmdPipelineBarrier(V.pcmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        hi->img.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.image = V.sc_images[idx];
        vkCmdPipelineBarrier(V.pcmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        vkCmdClearColorImage(V.pcmd, V.sc_images[idx],
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
        goto blit;
    }
    /* The frame into the staging ring's tail (the renderer is idle between
     * its flush and the next draw, and the ring restarts at its next
     * recording). */
    off = (V.staging.size - w * h * bpp) & ~15u;
    for (y = 0; y < h; y++)
        memcpy(V.staging.ptr + off + (size_t)y * w * bpp, mem + fb_va + (size_t)y * pitch,
               (size_t)w * bpp);

    b.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.image = src->image;
    vkCmdPipelineBarrier(V.pcmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    b.image = V.sc_images[idx];
    vkCmdPipelineBarrier(V.pcmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);

    memset(&c, 0, sizeof c);
    c.bufferOffset = off;
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = w;
    c.imageExtent.height = h;
    c.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(V.pcmd, V.staging.buf, src->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    vkCmdClearColorImage(V.pcmd, V.sc_images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &black, 1, &range);

    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.image = src->image;
    vkCmdPipelineBarrier(V.pcmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.image = V.sc_images[idx];
    vkCmdPipelineBarrier(V.pcmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);

    srcimg = src->image;
    sw = w;
    sh = h;

blit:
    /* 4:3 in the middle of the display (or stretched). */
    sc_rect(&dx, &dy, &dw, &dh);
    memset(&blit, 0, sizeof blit);
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.layerCount = 1;
    blit.srcOffsets[1].x = (int32_t)sw;
    blit.srcOffsets[1].y = (int32_t)sh;
    blit.srcOffsets[1].z = 1;
    blit.dstSubresource = blit.srcSubresource;
    blit.dstOffsets[0].x = (int32_t)dx;
    blit.dstOffsets[0].y = (int32_t)dy;
    blit.dstOffsets[1].x = (int32_t)(dx + dw);
    blit.dstOffsets[1].y = (int32_t)(dy + dh);
    blit.dstOffsets[1].z = 1;
    vkCmdBlitImage(V.pcmd, srcimg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   V.sc_images[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                   VK_FILTER_LINEAR);

    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(V.pcmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    vkEndCommandBuffer(V.pcmd);

    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &V.sem_acquire;
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &V.pcmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &V.sem_done;
    pthread_mutex_lock(&P.qmu);
    vkQueueSubmit(V.queue, 1, &si, V.pfence);
    pthread_mutex_unlock(&P.qmu);
    /* The staging tail is reused by the next recording: wait for the copy. */
    vkWaitForFences(V.dev, 1, &V.pfence, VK_TRUE, UINT64_MAX);

    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &V.sem_done;
    pi.swapchainCount = 1;
    pi.pSwapchains = &V.swapchain;
    pi.pImageIndices = &idx;
    pthread_mutex_lock(&P.qmu);
    r = vkQueuePresentKHR(V.queue, &pi);
    pthread_mutex_unlock(&P.qmu);
    sc_note(r);
    s_display_frames++;
    pthread_mutex_unlock(&s_present_mu);
    if (++V.presents == 1)
        vk_log("%s", "present: first frame shown");
    return 1;
}

/* A surface provider and the instance extensions it needs, before
 * dwvk_register_with. */
void dwvk_set_present(const char *const *inst_exts, uint32_t n, dwvk_surface_fn make_surface)
{
    s_present_cfg.inst_exts = inst_exts;
    s_present_cfg.n_inst_exts = n;
    s_present_cfg.make_surface = make_surface;
}

/* ── initialisation ─────────────────────────────────────────────── */

static int has_ext(VkPhysicalDevice pd, const char *name)
{
    uint32_t n = 0, i;
    VkExtensionProperties *e;
    int found = 0;

    vkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL);
    e = calloc(n ? n : 1, sizeof *e);
    vkEnumerateDeviceExtensionProperties(pd, NULL, &n, e);
    for (i = 0; i < n; i++)
        if (!strcmp(e[i].extensionName, name))
            found = 1;
    free(e);
    return found;
}

/* What a rejected device offers, so the next fix can be aimed at it. */
static void log_exts(VkPhysicalDevice pd)
{
    uint32_t n = 0, i;
    VkExtensionProperties *e;

    vkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL);
    e = calloc(n ? n : 1, sizeof *e);
    vkEnumerateDeviceExtensionProperties(pd, NULL, &n, e);
    fprintf(stderr, "[VK] extensions:");
    for (i = 0; i < n; i++)
        fprintf(stderr, " %s", e[i].extensionName);
    fprintf(stderr, "\n");
    free(e);
}

static int init_vulkan(PFN_vkGetInstanceProcAddr gipa)
{
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    VkPhysicalDevice pds[8];
    uint32_t npd = 8, i, nq;
    VkPhysicalDeviceProperties props;
    VkQueueFamilyProperties qf[16];
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceDynamicRenderingFeaturesKHR drf = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR };
    const char *dr_ext[] = { VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME };
    int core13;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkShaderModuleCreateInfo smi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    VkDescriptorSetLayoutBinding bind = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                          VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64 };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    const VkFormat depth_cands[] = { VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT };

    vkGetInstanceProcAddr_ = gipa;
    vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    if (!vkCreateInstance)
        return 0;
    app.pApplicationName = "Darkwatch";
    app.apiVersion = VK_API_VERSION_1_3;
    ici.pApplicationInfo = &app;
    if (s_present_cfg.make_surface) {
        ici.enabledExtensionCount = s_present_cfg.n_inst_exts;
        ici.ppEnabledExtensionNames = s_present_cfg.inst_exts;
    }
    if (vkCreateInstance(&ici, NULL, &V.inst) != VK_SUCCESS) {
        vk_log("%s", "vkCreateInstance failed (no Vulkan 1.3 driver?)");
        return 0;
    }
#define DW_LOAD_I(f) f = (PFN_##f)gipa(V.inst, #f); if (!f) { vk_log("missing %s", #f); return 0; }
    DW_VK_INSTANCE_FNS(DW_LOAD_I)
    if (s_present_cfg.make_surface) {
        DW_VK_PRESENT_INSTANCE_FNS(DW_LOAD_I)
    }

    if (vkEnumeratePhysicalDevices(V.inst, &npd, pds) < 0 || !npd) {
        vk_log("%s", "no Vulkan device");
        return 0;
    }
    /* A real GPU over a CPU driver: discrete, integrated, virtual (WSL's
     * D3D12-backed "dzn" reports itself as one of these), then CPU (llvmpipe,
     * SwiftShader). DW_VK_DEVICE=<index> picks one by hand. */
    {
        static const int rank[] = {
            [VK_PHYSICAL_DEVICE_TYPE_OTHER] = 1,
            [VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU] = 3,
            [VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU] = 4,
            [VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU] = 2,
            [VK_PHYSICAL_DEVICE_TYPE_CPU] = 0,
        };
        const char *pick = getenv("DW_VK_DEVICE");
        int best = -1, best_rank = -1;
        for (i = 0; i < npd; i++) {
            vkGetPhysicalDeviceProperties(pds[i], &props);
            fprintf(stderr, "[VK] device %u: %s (type %d, Vulkan %u.%u)\n", i,
                    props.deviceName, (int)props.deviceType,
                    VK_API_VERSION_MAJOR(props.apiVersion),
                    VK_API_VERSION_MINOR(props.apiVersion));
            if (VK_API_VERSION_MINOR(props.apiVersion) < 2
                || (VK_API_VERSION_MINOR(props.apiVersion) < 3
                    && !has_ext(pds[i], VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME)))
                continue;
            if (props.deviceType <= VK_PHYSICAL_DEVICE_TYPE_CPU
                && rank[props.deviceType] > best_rank) {
                best_rank = rank[props.deviceType];
                best = (int)i;
            }
        }
        if (pick && (uint32_t)atoi(pick) < npd)
            best = atoi(pick);
        if (best < 0) {
            vk_log("%s", "no device with Vulkan 1.3, or 1.2 with VK_KHR_dynamic_rendering");
            for (i = 0; i < npd; i++)
                log_exts(pds[i]);
            return 0;
        }
        V.phys = pds[best];
    }
    vkGetPhysicalDeviceProperties(V.phys, &props);
    vk_log("device: %s", props.deviceName);
    core13 = VK_API_VERSION_MINOR(props.apiVersion) >= 3;
    nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(V.phys, &nq, qf);
    V.qfam = UINT32_MAX;
    for (i = 0; i < nq; i++)
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            V.qfam = i;
            break;
        }
    if (V.qfam == UINT32_MAX)
        return 0;
    vkGetPhysicalDeviceMemoryProperties(V.phys, &V.memprops);

    V.depth_fmt = VK_FORMAT_UNDEFINED;
    for (i = 0; i < 2; i++) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(V.phys, depth_cands[i], &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            V.depth_fmt = depth_cands[i];
            break;
        }
    }
    if (V.depth_fmt == VK_FORMAT_UNDEFINED)
        return 0;

    qci.queueFamilyIndex = V.qfam;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char *dev_exts[2];
    uint32_t n_dev_exts = 0;
    if (core13) {
        f13.dynamicRendering = VK_TRUE;
        dci.pNext = &f13;
    } else {
        drf.dynamicRendering = VK_TRUE;
        dci.pNext = &drf;
        dev_exts[n_dev_exts++] = dr_ext[0];
        vk_log("%s", "Vulkan 1.2 device: dynamic rendering through VK_KHR_dynamic_rendering");
    }
    if (s_present_cfg.make_surface)
        dev_exts[n_dev_exts++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    dci.enabledExtensionCount = n_dev_exts;
    dci.ppEnabledExtensionNames = n_dev_exts ? dev_exts : NULL;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    if (vkCreateDevice(V.phys, &dci, NULL, &V.dev) != VK_SUCCESS) {
        vk_log("%s", "vkCreateDevice failed");
        return 0;
    }
#define DW_LOAD_D(f) f = (PFN_##f)vkGetDeviceProcAddr(V.dev, #f); if (!f) { vk_log("missing %s", #f); return 0; }
    DW_VK_DEVICE_FNS(DW_LOAD_D)
    vkCmdBeginRendering = (PFN_vkCmdBeginRendering)vkGetDeviceProcAddr(
        V.dev, core13 ? "vkCmdBeginRendering" : "vkCmdBeginRenderingKHR");
    vkCmdEndRendering = (PFN_vkCmdEndRendering)vkGetDeviceProcAddr(
        V.dev, core13 ? "vkCmdEndRendering" : "vkCmdEndRenderingKHR");
    if (!vkCmdBeginRendering || !vkCmdEndRendering) {
        vk_log("%s", "dynamic rendering entry points missing");
        return 0;
    }
    vkGetDeviceQueue(V.dev, V.qfam, 0, &V.queue);
    pthread_mutex_init(&P.qmu, NULL);

    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = V.qfam;
    vkCreateCommandPool(V.dev, &cpi, NULL, &V.pool);
    cai.commandPool = V.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    vkAllocateCommandBuffers(V.dev, &cai, &V.cmd);
    vkCreateFence(V.dev, &fci, NULL, &V.fence);

    smi.codeSize = sizeof nv2a_vert_spv;
    smi.pCode = nv2a_vert_spv;
    vkCreateShaderModule(V.dev, &smi, NULL, &V.vs);
    smi.codeSize = sizeof nv2a_frag_spv;
    smi.pCode = nv2a_frag_spv;
    vkCreateShaderModule(V.dev, &smi, NULL, &V.fs);
    dli.bindingCount = 1;
    dli.pBindings = &bind;
    vkCreateDescriptorSetLayout(V.dev, &dli, NULL, &V.dsl);
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &V.dsl;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    vkCreatePipelineLayout(V.dev, &pli, NULL, &V.layout);
    {
        VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, DW_DESC_SETS };
        VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        dpi.maxSets = DW_DESC_SETS;
        dpi.poolSizeCount = 1;
        dpi.pPoolSizes = &ps;
        if (vkCreateDescriptorPool(V.dev, &dpi, NULL, &V.dpool) != VK_SUCCESS)
            return 0;
    }

    if (!ring_init(&V.staging, STAGING_BYTES,
                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)
        || !ring_init(&V.verts, VERTEX_BYTES, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)
        || !ring_init(&V.idx, INDEX_BYTES, VK_BUFFER_USAGE_INDEX_BUFFER_BIT))
        return 0;

    /* A 1x1 white texture for untextured batches. */
    if (!img_create(&V.white, 1, 1, VK_FORMAT_B8G8R8A8_UNORM,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT))
        return 0;
    begin_recording();
    {
        VkBufferImageCopy c;
        uint32_t off = ring_alloc(&V.staging, 4, 16);
        memset(V.staging.ptr + off, 0xFF, 4);
        img_layout(&V.white, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
        memset(&c, 0, sizeof c);
        c.bufferOffset = off;
        c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        c.imageSubresource.layerCount = 1;
        c.imageExtent.width = c.imageExtent.height = c.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(V.cmd, V.staging.buf, V.white.image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        img_layout(&V.white, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    }
    flush();
    V.stats_flushes = 0;
    if (s_present_cfg.make_surface) {
        DW_VK_PRESENT_DEVICE_FNS(DW_LOAD_D)
        V.present = present_init();
    }
    vk_log("%s", "renderer ready");
    return 1;
}

/* Register with a vkGetInstanceProcAddr the caller provides (a PS5 build that
 * links RADV statically). Returns 1 when the back end is live. */
int dwvk_register_with(PFN_vkGetInstanceProcAddr gipa)
{
    if (V.tried)
        return V.ok;
    V.tried = 1;
    V.ok = gipa && init_vulkan(gipa);
    if (V.ok)
        nv2a_backend_register(&s_backend);
    else
        vk_log("%s", "unavailable; the CPU rasteriser stays in use");
    return V.ok;
}

/* Linux: the system loader, or DW_VK_LIB -- a driver opened directly, with no
 * loader in between (SwiftShader exports vkGetInstanceProcAddr itself, which
 * is how this runs on a machine with no GPU and no Vulkan installed). */
#if !defined(__PROSPERO__)
/* DW_HEADLESS_PRESENT=1: a VK_EXT_headless_surface swapchain stands in for
 * the console's display, so the console's present path (and the
 * interpolation presenter) runs on a PC with nothing to show it on. */
static VkSurfaceKHR make_headless_surface(VkInstance inst, PFN_vkGetInstanceProcAddr gipa,
                                          VkPhysicalDevice phys, VkExtent2D *extent)
{
    PFN_vkCreateHeadlessSurfaceEXT f =
        (PFN_vkCreateHeadlessSurfaceEXT)gipa(inst, "vkCreateHeadlessSurfaceEXT");
    VkHeadlessSurfaceCreateInfoEXT ci = { VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT };
    VkSurfaceKHR surf = VK_NULL_HANDLE;

    (void)phys;
    if (!f || f(inst, &ci, NULL, &surf) != VK_SUCCESS) {
        vk_log("%s", "headless present: no VK_EXT_headless_surface");
        return VK_NULL_HANDLE;
    }
    extent->width = 1920;
    extent->height = 1080;
    V.headless = 1;
    return surf;
}

static PFN_vkGetInstanceProcAddr load_loader(void)
{
#if defined(_WIN32)
    const char *name = getenv("DW_VK_LIB") ? getenv("DW_VK_LIB") : "vulkan-1.dll";
    HMODULE lib = LoadLibraryA(name);
    if (!lib) {
        vk_log("cannot load %s (no Vulkan driver installed?)", name);
        return NULL;
    }
    return (PFN_vkGetInstanceProcAddr)(uintptr_t)GetProcAddress(lib, "vkGetInstanceProcAddr");
#else
    const char *name = getenv("DW_VK_LIB") ? getenv("DW_VK_LIB") : "libvulkan.so.1";
    void *lib = dlopen(name, RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        vk_log("cannot open %s", name);
        return NULL;
    }
    return (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
#endif
}

int dwvk_register(void)
{
    PFN_vkGetInstanceProcAddr gipa;
    if (getenv("DW_HEADLESS_PRESENT")) {
        static const char *const exts[] = { VK_KHR_SURFACE_EXTENSION_NAME,
                                            VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME };
        dwvk_set_present(exts, 2, make_headless_surface);
    }
    gipa = load_loader();
    if (!gipa) {
        vk_log("%s", "no Vulkan loader; the CPU rasteriser stays in use");
        return 0;
    }
    return dwvk_register_with(gipa);
}

/* The GPUs a settings screen can offer: names, in the order DW_VK_DEVICE
 * numbers them. Uses a throwaway instance. Returns how many. */
int dwvk_list_devices(char names[][256], int max)
{
    PFN_vkGetInstanceProcAddr gipa = load_loader();
    PFN_vkCreateInstance ci;
    PFN_vkDestroyInstance di;
    PFN_vkEnumeratePhysicalDevices ep;
    PFN_vkGetPhysicalDeviceProperties gp;
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    VkInstance inst;
    VkPhysicalDevice pds[8];
    uint32_t n = 8, i;

    if (!gipa || !(ci = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance")))
        return 0;
    app.pApplicationName = "Darkwatch";
    app.apiVersion = VK_API_VERSION_1_3;
    ici.pApplicationInfo = &app;
    if (ci(&ici, NULL, &inst) != VK_SUCCESS)
        return 0;
    di = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");
    ep = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    gp = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
    if (!ep || !gp || ep(inst, &n, pds) < 0)
        n = 0;
    for (i = 0; i < n && (int)i < max; i++) {
        VkPhysicalDeviceProperties pr;
        gp(pds[i], &pr);
        snprintf(names[i], 256, "%s", pr.deviceName);
    }
    if (di)
        di(inst, NULL);
    return (int)(n < (uint32_t)max ? n : (uint32_t)max);
}
#endif /* !__PROSPERO__ */
