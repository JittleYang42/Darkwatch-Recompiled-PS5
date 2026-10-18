/*
 * Read-only survey of the pushbuffer a title submits.
 *
 * The title builds NV2A commands in guest RAM and advances DMA_PUT; nothing
 * here executes them, so the framebuffer stays black however far the game
 * gets. Before any of that can be made to draw, the question is what it
 * actually asks for -- which methods, on which object classes, how many of
 * them -- because that is the difference between "the existing PGRAPH
 * translator nearly covers this" and "this needs a real one".
 *
 * Purely a reader: it walks the buffer and counts, and never writes to guest
 * memory or to the GPU state. Enabled with RECOMP_PB_SCAN.
 *
 * Pushbuffer encoding (NV20/NV2A), one dword per command header:
 *   (w & 0xE0030003) == 0x00000000  increasing methods
 *   (w & 0xE0030003) == 0x40000000  non-increasing (same method, count params)
 *   (w & 0x00000003) == 0x00000001  jump
 *   (w & 0x00000003) == 0x00000002  call
 *   (w & 0xFFFF0003) == 0x00020000  return
 * For a method header: count = (w >> 18) & 0x7FF, subchannel = (w >> 13) & 7,
 * method = w & 0x1FFC.
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE   /* REG_RIP, dladdr (RECOMP_EXEC_PROF) */
#endif
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>   /* ptrdiff_t */
#include <stdlib.h>
#include <string.h>

extern ptrdiff_t xbox_GetMemoryOffset(void);

#define PB_MAX_METHODS 4096

static struct { uint32_t method, subch, count; } s_seen[PB_MAX_METHODS];
static int s_seen_count;

/* Parse health. An inventory is only worth reading if the walk stayed in step
 * with the command stream: a decoder that desynchronises produces plausible
 * looking method numbers out of parameter data, and the counts then describe
 * nothing. Unrecognised words are the tell. */
static uint32_t s_tot_words, s_tot_unknown, s_tot_jumps, s_tot_segments;
static uint32_t s_tot_calls;

/* Executing is opt-in separately from surveying: a survey is read-only, while
 * the executor writes to guest memory. */
extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
extern void nv2a_pb_exec_report(void);
static int s_exec_enabled = -1;
static int s_scan_enabled = -1;

/* Clears the NV2A request bits (xbox_memory_layout.c); called every 256
 * words while executing, so kickoffs are answered mid-walk. */
extern void xbox_Nv2aAckBusyBits(void);
static uint32_t s_ack_tick;

/* Either switch, read once. The answer "not set" has to be cached as well as
 * "set": this runs on every pass of the NV2A poll loop, and on the default
 * path -- neither switch set -- a cache that only remembered "set" called
 * getenv on every pass, which walks the whole environment. */
static int pb_walk_enabled(void)
{
    if (s_exec_enabled < 0)
        s_exec_enabled = getenv("RECOMP_PB_EXEC") != NULL;
    if (s_scan_enabled < 0)
        s_scan_enabled = getenv("RECOMP_PB_SCAN") != NULL;
    return s_exec_enabled || s_scan_enabled;
}

/* Slot + 1 of each (subchannel, method) in s_seen. note() runs for every
 * pushbuffer word, so a linear search here was the executor's largest cost
 * (about 40% of its thread in X-Men Legends' levels). */
static uint16_t s_seen_slot[8][0x2000 / 4];

static void note(uint32_t subch, uint32_t method)
{
    uint16_t *slot = &s_seen_slot[subch & 7][(method & 0x1FFC) / 4];

    if (*slot) {
        s_seen[*slot - 1].count++;
        return;
    }
    if (s_seen_count >= PB_MAX_METHODS) {
        /* Silently dropping past the cap is how a truncated inventory reads as
         * "the title never does that" -- exactly the wrong conclusion when the
         * inventory is being used to decide what to implement. */
        static int warned;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "[PB] method table full at %d -- inventory is"
                            " truncated\n", PB_MAX_METHODS);
        }
    }
    if (s_seen_count < PB_MAX_METHODS) {
        s_seen[s_seen_count].method = method;
        s_seen[s_seen_count].subch  = subch;
        s_seen[s_seen_count].count  = 1;
        s_seen_count++;
        *slot = (uint16_t)s_seen_count;
    }
}

/* NV097 (Kelvin 3D class) methods worth naming. The point of the survey is to
 * decide what a translator has to implement, and a bare method number does not
 * answer that -- "0x1808 x412" only means something once it reads
 * INLINE_ARRAY. Unnamed ones still get counted. */
static const struct { uint32_t m; const char *name; } NV097_NAMES[] = {
    { 0x0000, "SET_OBJECT" },
    { 0x0100, "NO_OPERATION" },
    { 0x0104, "SET_WARNING_ENABLE" },
    { 0x0130, "SET_FLIP_READ" },
    { 0x0200, "SET_SURFACE_CLIP_HORIZONTAL" },
    { 0x0204, "SET_SURFACE_CLIP_VERTICAL" },
    { 0x0208, "SET_SURFACE_FORMAT" },
    { 0x020C, "SET_SURFACE_PITCH" },
    { 0x0210, "SET_SURFACE_COLOR_OFFSET" },
    { 0x0214, "SET_SURFACE_ZETA_OFFSET" },
    { 0x0300, "SET_ALPHA_TEST_ENABLE" },
    { 0x0304, "SET_BLEND_ENABLE" },
    { 0x030C, "SET_DEPTH_TEST_ENABLE" },
    { 0x0310, "SET_DITHER_ENABLE" },
    { 0x0314, "SET_LIGHTING_ENABLE" },
    { 0x033C, "SET_CULL_FACE_ENABLE" },
    { 0x0340, "SET_DEPTH_MASK" },
    { 0x0350, "SET_CLEAR_DEPTH_VALUE" },
    { 0x1D8C, "SET_CLEAR_DEPTH" },
    { 0x1D90, "SET_COLOR_CLEAR_VALUE" },
    { 0x1D94, "CLEAR_SURFACE" },
    { 0x1D6C, "SET_ZSTENCIL_CLEAR" },
    { 0x0B80, "SET_TRANSFORM_PROGRAM" },
    { 0x0B00, "SET_TRANSFORM_CONSTANT" },
    { 0x1720, "SET_VERTEX_DATA_ARRAY_OFFSET" },
    { 0x1760, "SET_VERTEX_DATA_ARRAY_FORMAT" },
    { 0x17FC, "SET_BEGIN_END" },
    { 0x1800, "ARRAY_ELEMENT16" },
    { 0x1808, "INLINE_ARRAY" },
    { 0x1810, "DRAW_ARRAYS" },
    { 0x1B00, "SET_TEXTURE_OFFSET" },
    { 0x1B04, "SET_TEXTURE_FORMAT" },
    { 0x1B08, "SET_TEXTURE_ADDRESS" },
    { 0x1B0C, "SET_TEXTURE_CONTROL0" },
    { 0x1B14, "SET_TEXTURE_IMAGE_RECT" },
    { 0x0FD8, "SET_COMBINER_*" },
    { 0x0000, NULL },
};

static const char *nv097_name(uint32_t m)
{
    int i;
    for (i = 0; NV097_NAMES[i].name; i++)
        if (NV097_NAMES[i].m == m)
            return NV097_NAMES[i].name;
    return "";
}

/* For the executor report: the methods with the most words since the last
 * call, per frame -- where the walk's per-word time goes. */
void nv2a_pb_scan_top_methods(unsigned frames)
{
    static uint32_t prev[PB_MAX_METHODS];
    uint32_t best[6] = { 0 }, bi[6] = { 0 }, total = 0;
    char buf[480];
    int i, k, len = 0;

    if (!frames)
        frames = 1;
    for (i = 0; i < s_seen_count; i++) {
        uint32_t d = s_seen[i].count - prev[i];
        prev[i] = s_seen[i].count;
        total += d;
        for (k = 0; k < 6; k++)
            if (d > best[k]) {
                int j;
                for (j = 5; j > k; j--) {
                    best[j] = best[j - 1];
                    bi[j] = bi[j - 1];
                }
                best[k] = d;
                bi[k] = (uint32_t)i;
                break;
            }
    }
    for (k = 0; k < 6 && best[k]; k++)
        len += snprintf(buf + len, sizeof buf - (size_t)len, "%s%u:%04X %s %u", k ? ", " : "",
                        s_seen[bi[k]].subch, s_seen[bi[k]].method,
                        nv097_name(s_seen[bi[k]].method), best[k] / frames);
    fprintf(stderr, "[GPU]   pushbuffer words a frame: %u; most: %s\n", total / frames,
            len ? buf : "-");
}

void nv2a_pb_scan_report(void)
{
    int i;

    if (s_exec_enabled > 0) {
        nv2a_pb_exec_report();
        fprintf(stderr, "[PB] %u pushbuffer calls followed\n", s_tot_calls);
    }
    if (!s_seen_count || !getenv("RECOMP_PB_SCAN"))
        return;
    fprintf(stderr, "[PB] %u segments, %u words, %u jumps, %u calls,"
                    " %u unrecognised -- %d distinct (subchannel, method)"
                    " pairs\n",
            s_tot_segments, s_tot_words, s_tot_jumps, s_tot_calls,
            s_tot_unknown, s_seen_count);
    for (i = 0; i < s_seen_count; i++)
        fprintf(stderr, "  [PB]   subch %u  method 0x%04X  x%-6u %s\n",
                s_seen[i].subch, s_seen[i].method, s_seen[i].count,
                nv097_name(s_seen[i].method));
    fflush(stderr);
}

/* Walk the pushbuffer the way the GPU's DMA engine does, from our own read
 * position up to the title's DMA_PUT.
 *
 * This used to scan only the bytes between the previous PUT and this one,
 * stop at the first jump, skip subroutine calls, and skip the whole segment
 * whenever PUT wrapped back to the start of the ring. Each of those drops
 * commands: XDK D3D sends state and vertex-program uploads through CALLs into
 * pre-built pushbuffers, and every wrap lost a segment. What survived drew
 * with stale state -- untransformed meshes run through the pre-transformed
 * pass-through program, garbled text.
 *
 * So: keep a GET of our own (physical, like the register), follow JUMP and
 * CALL/RETURN (NV2A has one level of subroutine), and stop when GET reaches
 * PUT. Addresses are physical; the contiguous window at XBOX_CONTIG_BASE is
 * the physical view.
 *
 * ponytail: if the walk desynchronises (a run of words that decode as
 * nothing), GET snaps to PUT rather than wandering through guest RAM. */
#define PB_PHYS_MASK 0x0FFFFFFCu
#define PB_RAM_BYTES (64u * 1024u * 1024u)   /* XBOX_CONTIG_SIZE */

/* A jump or call outside RAM means the walk is reading data as commands. */
/* The last command headers decoded, so a desync can be traced back to the
 * command that caused it. */
#define PB_HIST 8
static struct { uint32_t at, w; } s_hist[PB_HIST];
static unsigned s_hist_n;
/* The last jumps/calls/returns taken, and resyncs (w = 0xFFFFFFFF). */
static struct { uint32_t at, w, to; } s_flow[PB_HIST];
static unsigned s_flow_n;
static void pb_flow(uint32_t at, uint32_t w, uint32_t to)
{
    s_flow[s_flow_n % PB_HIST].at = at;
    s_flow[s_flow_n % PB_HIST].w = w;
    s_flow[s_flow_n % PB_HIST].to = to;
    s_flow_n++;
}

extern uint32_t xbox_Nv2aFenceWordVa(void);
extern volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset);
#define NV2A_USER_DMA_PUT 0x800040u
#define NV2A_USER_DMA_GET 0x800044u
static int pb_in_ring(uint32_t a);
static uint32_t s_put_lo, s_put_hi, s_walk_start, s_tot_segments;
static int pb_bad_target(uint32_t w, uint32_t at, uint32_t target, uint32_t put)
{
    static unsigned total;
    unsigned k;
    if (target < PB_RAM_BYTES && ((w & 3u) == 2u || !pb_in_ring(put) || pb_in_ring(target)))
        return 0;
    if (++total <= 16 || total % 100 == 0) {
        fprintf(stderr, "  [PB] bad target #%u 0x%08X from word 0x%08X at 0x%08X"
                        " (PUT 0x%08X); resync\n", total, target, w, at, put);
        if (total <= 4)
            for (k = 0; k < PB_HIST && k < s_hist_n; k++) {
                unsigned i = (s_hist_n - 1 - k) % PB_HIST;
                fprintf(stderr, "  [PB]   earlier header 0x%08X at 0x%08X\n",
                        s_hist[i].w, s_hist[i].at);
            }
        if (total <= 4) {
            const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
            uint32_t a0 = (at >= 0x60u) ? at - 0x60u : 0;
            for (uint32_t a = a0; a < at + 0x20u && a + 4 <= PB_RAM_BYTES; a += 16)
                fprintf(stderr, "  [PB]   %08X: %08X %08X %08X %08X\n", a,
                        *(const uint32_t *)(mem + (0x80000000u | a)),
                        *(const uint32_t *)(mem + (0x80000000u | (a + 4))),
                        *(const uint32_t *)(mem + (0x80000000u | (a + 8))),
                        *(const uint32_t *)(mem + (0x80000000u | (a + 12))));
            fprintf(stderr, "  [PB]   this walk began at %08X, ring %08X..%08X, %u walks so far\n",
                    s_walk_start, s_put_lo, s_put_hi, s_tot_segments);
            {
                extern uint32_t nv2a_pb_title_write_ptr(void);
                volatile uint32_t *pr = xbox_Nv2aRegPtr(NV2A_USER_DMA_PUT);
                volatile uint32_t *gr = xbox_Nv2aRegPtr(NV2A_USER_DMA_GET);
                uint32_t fva = xbox_Nv2aFenceWordVa();
                fprintf(stderr, "  [PB]   now: PUT reg %08X, GET reg %08X, title write ptr %08X, fence word %08X\n",
                        pr ? *pr : 0, gr ? *gr : 0, nv2a_pb_title_write_ptr(),
                        fva ? *(const uint32_t *)(mem + fva) : 0);
            }
        }
        if (total <= 4)
            for (k = 0; k < PB_HIST && k < s_flow_n; k++) {
                unsigned i = (s_flow_n - 1 - k) % PB_HIST;
                fprintf(stderr, "  [PB]   earlier flow 0x%08X at 0x%08X -> 0x%08X\n",
                        s_flow[i].w, s_flow[i].at, s_flow[i].to);
            }
    }
    return 1;
}
static unsigned s_salvaged;
/* The extent of the ring, learned from every PUT seen: the salvage of a
 * stalled fence scans this. */
static uint32_t s_put_lo = 0xFFFFFFFFu, s_put_hi;
/* The last position the walk held inside the ring itself (not in a CALLed
 * segment). When the walk loses its place, everything from here to PUT is
 * what it dropped, and that is the span the salvage must read -- the address
 * the walk had wandered off to is in some vertex buffer and holds nothing. */
static uint32_t s_ring_pos = 0xFFFFFFFFu;
static uint32_t s_walked_put = 0xFFFFFFFFu;   /* the PUT the last finished walk went to */
static uint32_t s_walk_start;
#define PB_RING_SLACK 0x40000u
static int pb_in_ring(uint32_t a)
{
    return s_put_lo != 0xFFFFFFFFu && s_put_hi > s_put_lo
        && a + PB_RING_SLACK >= s_put_lo && a <= s_put_hi + PB_RING_SLACK;
}
static uint32_t s_get = 0xFFFFFFFFu, s_ret;
static int      s_in_call;

/* A method whose parameters the walk has not all reached: the header said
 * `count` words and PUT came first. The DMA engine stalls at PUT and carries
 * on with the same method when PUT moves; reading the next word as a new
 * header instead would decode vertex data as commands. (D3D streams large
 * inline arrays across several kicks, so this is routine, not a desync.) */
static uint32_t s_pend_left, s_pend_i, s_pend_method, s_pend_subch;
static int      s_pend_noninc;

/* DMA_GET is the GPU's fetch pointer: nothing at or past it has been read, and
 * XDK D3D relies on exactly that. Its fence wait reads GET and, when the fence
 * is far enough on, patches a "wake me" software method into the pushbuffer at
 * the fence and sleeps on an event. If GET lagged the executor -- it used to
 * move only when the walk reached PUT -- D3D patched commands the executor had
 * already passed and slept for good. So under RECOMP_PB_EXEC the walk publishes
 * its position before every command (s_pub is the last value it wrote); a GET
 * that is not what it last wrote means the title moved it, and the walk
 * follows. */
static uint32_t s_pub = 0xFFFFFFFFu;

/* The newest fence release anywhere in the ring that is ahead of the counter.
 *
 * For a title asleep in a poll loop on the fence word while the GPU is idle:
 * everything it submitted has run, so the counter should stand at the value
 * of its latest release. A desync that never reached a give-up -- one that
 * swallowed a release as somebody else's parameter -- leaves nothing for the
 * span salvage to see, but the release is still in the ring, and the title's
 * values only ever go up, so the largest plausible one there is the latest.
 * Returns 0 when there is nothing ahead. */
/* Does this word head a run of increasing methods on subchannel 0 that
 * includes BACK_END_WRITE_SEMAPHORE_RELEASE (0x1D70)? D3D writes the release
 * either alone (header 0x00041D70) or together with SET_SEMAPHORE_OFFSET
 * (0x1D6C) in one two-method run (0x00081D6C), and a general decode covers
 * whichever it chose. Returns the index of the release's parameter after the
 * header, or 0. */
static uint32_t pb_release_index(uint32_t w)
{
    uint32_t count, method;
    if ((w & 0xE0030003u) != 0u || ((w >> 13) & 7u) != 0u)
        return 0;
    count  = (w >> 18) & 0x7FFu;
    method =  w & 0x1FFCu;
    if (!count || count > 16 || method > 0x1D70u || method + (count - 1) * 4 < 0x1D70u)
        return 0;
    return (0x1D70u - method) / 4 + 1;
}

uint32_t nv2a_pb_latest_release(uint32_t cur)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t a, best = 0, seen = 0, maxv = 0;
    static unsigned reported;

    if (s_put_lo == 0xFFFFFFFFu || s_put_hi <= s_put_lo || !mem)
        return 0;
    for (a = s_put_lo & ~3u; a + 8 <= s_put_hi + 4 && a + 8 <= PB_RAM_BYTES; a += 4) {
        uint32_t w = *(const uint32_t *)(mem + (0x80000000u | a)), v, k;
        k = pb_release_index(w);
        if (!k)
            continue;
        v = *(const uint32_t *)(mem + (0x80000000u | (a + 4 * k)));
        seen++;
        if (v > maxv) maxv = v;
        if (v > cur && (uint32_t)(v - cur) < 0x10000u && v > best)
            best = v;
    }
    if (reported++ < 3)
        fprintf(stderr, "  [PB] fence search: ring %08X..%08X, %u release(s) found, "
                        "newest %08X, counter %08X -> %s%08X\n",
                s_put_lo, s_put_hi, seen, maxv, cur, best ? "catch up to " : "nothing ahead ", best);
    return best;
}

/* ── salvaging a lost span ───────────────────────────────────────
 *
 * When the walk cannot make sense of a word it gives up on the span between
 * here and PUT and restarts at PUT. That keeps it out of guest RAM, but the
 * span it drops is real work, and two of the methods in it are ones the title
 * *waits* for rather than merely draws with:
 *
 *   0x1D70  BACK_END_WRITE_SEMAPHORE_RELEASE -- writes the fence word D3D
 *           polls. A dropped release leaves the counter short for ever, and
 *           the title sleeps in a poll loop waiting for a value that cannot
 *           arrive. (Console capture: fence word 0x1B93, title waiting for
 *           0x1B9F -- twelve releases lost.)
 *   0x0100  NO_OPERATION with argument 5 -- the wake-up trap D3D sleeps on
 *           in its fence wait.
 *
 * Both are single-parameter methods on subchannel 0, so each appears in the
 * stream as one exact header word followed by its parameter. Scanning the
 * dropped span for those two constants recovers the waits without pretending
 * to understand the geometry around them: a false positive needs a data word
 * equal to the header *and* a plausible successor, and for the release the
 * value is checked against the counter it would be advancing.
 */
static uint32_t pb_release_index(uint32_t w);
#define PB_HDR_SEM_RELEASE 0x00041D70u   /* count 1, subch 0, method 0x1D70 */
#define PB_HDR_NOP         0x00040100u   /* count 1, subch 0, method 0x0100 */

extern uint32_t xbox_Nv2aFenceWordVa(void);

static void pb_salvage(const uint8_t *mem, uint32_t from, uint32_t put)
{
    uint32_t fence_va = xbox_Nv2aFenceWordVa();
    uint32_t cur = 0, best = 0, a, span = 0;
    int woke = 0;

    if (fence_va)
        cur = *(const volatile uint32_t *)(mem + fence_va);

    /* Forward from where the walk lost its place. The span ends at PUT when
     * PUT is ahead; when the ring has wrapped, the rest of it is what was
     * dropped, so the scan is bounded instead. */
    for (a = from; span < (4u << 20); a = (a + 4) & PB_PHYS_MASK, span += 4) {
        uint32_t w, v;

        if (a == put || a >= PB_RAM_BYTES)
            break;
        if (pb_in_ring(from) && a > s_put_hi + 4) {
            /* Past the end of the ring: the title wrapped. */
            a = s_put_lo & ~3u;
            if (a >= put) break;
        }
        uint32_t k;
        w = *(const uint32_t *)(mem + (0x80000000u | a));
        if (w == PB_HDR_NOP) {
            v = *(const uint32_t *)(mem + (0x80000000u | ((a + 4) & PB_PHYS_MASK)));
            if (v == 5)
                woke = 1;                 /* one wake-up is enough */
            continue;
        }
        k = pb_release_index(w);
        if (!k)
            continue;
        v = *(const uint32_t *)(mem + (0x80000000u | ((a + 4 * k) & PB_PHYS_MASK)));
        /* A release that moves the counter forward by a sane amount. */
        if (fence_va && v > cur && (uint32_t)(v - cur) < 0x10000u
                && (!best || v > best))
            best = v;
    }

    if (best) {
        nv2a_pb_exec_method(0, 0x1D70, best);
        if (s_salvaged++ < 8 || (s_salvaged % 64) == 0)
            fprintf(stderr, "  [PB] salvaged fence release %08X (was %08X) from the "
                            "dropped span (#%u)\n", best, cur, s_salvaged);
    }
    if (woke) {
        nv2a_pb_exec_method(0, 0x0100, 5);
        fprintf(stderr, "  [PB] salvaged a D3D wake-up from the dropped span\n");
    }
    if (best || woke)
        fflush(stderr);
}



/* The title moved GET itself: XDK D3D resets the ring by writing DMA_PUT and
 * DMA_GET together (CDevice::KickOff's restart path). Walking on from the old
 * position would read whatever lies between as commands. */
void nv2a_pb_resync(uint32_t get_phys)
{
    if (!pb_walk_enabled())
        return;
    pb_flow(s_get, 0xFFFFFFFFu, get_phys & PB_PHYS_MASK);
    s_get = get_phys & PB_PHYS_MASK;
    s_in_call = 0;
    s_pend_left = 0;
    s_pub = s_get;
    s_ring_pos = 0xFFFFFFFFu;
}

/* Feed the parameters of the method in progress to the survey and the
 * executor, up to PUT. */
extern void nv2a_pb_exec_indices16(const uint32_t *w, uint32_t n);

static void pb_consume(const uint8_t *mem, uint32_t put, uint32_t *words)
{
    while (s_pend_left && s_get < PB_RAM_BYTES && s_get != put) {
        /* ARRAY_ELEMENT16 -- two indices a word, about two thirds of a busy
         * frame's words -- goes to the executor a run at a time instead of
         * a call per word. A method's parameters are contiguous (the ring
         * wraps only by a JUMP), up to PUT when PUT is ahead. */
        if (s_exec_enabled && s_pend_noninc && s_pend_subch == 0
            && s_pend_method == 0x1800u && s_pend_left > 1) {
            uint32_t lim = put > s_get ? (put - s_get) / 4 : (PB_RAM_BYTES - s_get) / 4;
            uint32_t n = s_pend_left < lim ? s_pend_left : lim;
            if (n > 1) {
                uint16_t *slot = &s_seen_slot[0][0x1800 / 4];
                note(0, 0x1800);
                if (*slot)
                    s_seen[*slot - 1].count += n - 1;
                nv2a_pb_exec_indices16((const uint32_t *)(mem + (0x80000000u | s_get)), n);
                s_get = (s_get + 4 * n) & PB_PHYS_MASK;
                s_pend_i += n;
                s_pend_left -= n;
                *words += n;
                continue;
            }
        }
        uint32_t m = s_pend_noninc ? s_pend_method : s_pend_method + s_pend_i * 4;
        uint32_t param = *(const uint32_t *)(mem + (0x80000000u | s_get));

        note(s_pend_subch, m);
        /* Same walk, two consumers: the survey counts, the executor
         * acts. Keeping them on one decode means they can never
         * disagree about what the stream said. */
        if (s_exec_enabled)
            nv2a_pb_exec_method(s_pend_subch, m, param);
        s_get = (s_get + 4) & PB_PHYS_MASK;
        s_pend_i++;
        s_pend_left--;
        (*words)++;
    }
}

/* RECOMP_EXEC_PROF=<file> (Linux, x86-64): a sampling profile of the
 * executor while it walks -- every 0.5 ms of wall time the interrupted
 * instruction address is noted (waits included: they land in libc/the
 * driver), and every 10 s the samples are appended to <file> as
 * "module offset" lines, to be named offline (nm -n on that module). For
 * finding where the executor's time goes; off by default. */
#if defined(__linux__) && defined(__x86_64__)
#include <signal.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/syscall.h>
#define XP_RING 131072u
static uint64_t s_xp_ring[XP_RING];
static volatile uint32_t s_xp_n, s_xp_read;
static volatile int s_xp_in;
static int s_xp_state = -1;             /* -1 unknown, 0 off, 1 on */
static const char *s_xp_path;
static struct timespec s_xp_last;

static void xp_handler(int sig, siginfo_t *si, void *ucv)
{
    ucontext_t *uc = (ucontext_t *)ucv;
    uint32_t n;
    (void)sig; (void)si;
    if (!s_xp_in)
        return;
    n = s_xp_n;
    s_xp_ring[n & (XP_RING - 1u)] = (uint64_t)uc->uc_mcontext.gregs[REG_RIP];
    s_xp_n = n + 1u;
}

static void xp_flush(void)
{
    uint32_t a = s_xp_read, b = s_xp_n, i;
    FILE *f;
    if (b - a > XP_RING)
        a = b - XP_RING;
    s_xp_read = b;
    if (a == b || !(f = fopen(s_xp_path, "a")))
        return;
    for (i = a; i != b; i++) {
        uint64_t rip = s_xp_ring[i & (XP_RING - 1u)];
        Dl_info di;
        if (dladdr((void *)rip, &di) && di.dli_fname)
            fprintf(f, "%s %llx\n", di.dli_fname,
                    (unsigned long long)(rip - (uint64_t)di.dli_fbase));
        else
            fprintf(f, "? %llx\n", (unsigned long long)rip);
    }
    fclose(f);
}

static void xp_tick(void)
{
    struct timespec now;
    if (s_xp_state == -1) {
        struct sigaction sa;
        struct sigevent sev;
        struct itimerspec its;
        timer_t t;
        s_xp_path = getenv("RECOMP_EXEC_PROF");
        s_xp_state = 0;
        if (!s_xp_path || !*s_xp_path)
            return;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = xp_handler;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGPROF, &sa, NULL);
        memset(&sev, 0, sizeof sev);
        sev.sigev_notify = SIGEV_THREAD_ID;
        sev.sigev_signo = SIGPROF;
        sev._sigev_un._tid = (pid_t)syscall(SYS_gettid);
        if (timer_create(CLOCK_MONOTONIC, &sev, &t) != 0) {
            fprintf(stderr, "[XPROF] timer_create failed\n");
            return;
        }
        memset(&its, 0, sizeof its);
        its.it_interval.tv_nsec = its.it_value.tv_nsec = 500000;
        timer_settime(t, 0, &its, NULL);
        clock_gettime(CLOCK_MONOTONIC, &s_xp_last);
        s_xp_state = 1;
        fprintf(stderr, "[XPROF] sampling the executor every 0.5 ms into %s\n", s_xp_path);
    }
    if (s_xp_state != 1)
        return;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec - s_xp_last.tv_sec >= 10) {
        s_xp_last = now;
        xp_flush();
    }
}
#define XP_BEGIN() do { xp_tick(); s_xp_in = 1; } while (0)
#define XP_END() (s_xp_in = 0)
#else
#define XP_BEGIN() ((void)0)
#define XP_END() ((void)0)
#endif

/* Time the walk spent executing (TSC ticks), for the executor report; and
 * a count of walks (a back end can tell "same walk" by it). */
uint64_t g_nv2a_walk_tsc;
uint32_t g_nv2a_walk_seq;

/* Kicks, counted by the title project's KickOff hook after the title has
 * written PUT, and the last count the walk is known to have executed
 * through (the poll thread reads the count before PUT, so walking to that
 * PUT covers it). Lets a kick wait for an earlier kick rather than its own:
 * the GPU then runs behind the CPU, as it does on the console. */
volatile uint32_t g_nv2a_kick_seq, g_nv2a_walked_seq;

void nv2a_pb_scan(uint32_t put_phys)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t put = put_phys & PB_PHYS_MASK;
    volatile uint32_t *get_reg = NULL;
    uint32_t words = 0, jumps = 0, unknown = 0;
    uint64_t t0 = 0;

    if (!pb_walk_enabled())
        return;
    if (put < s_put_lo) s_put_lo = put;
    if (put > s_put_hi) s_put_hi = put;
    if (s_get == 0xFFFFFFFFu) {               /* never resynced: start at PUT */
        s_get = put, s_pend_left = 0;
        s_walked_put = put;
        return;
    }
    if (s_exec_enabled)
        get_reg = xbox_Nv2aRegPtr(NV2A_USER_DMA_GET);

    s_walk_start = s_get;
    g_nv2a_walk_seq++;
    XP_BEGIN();
#if defined(__x86_64__) || defined(__i386__)
    t0 = __builtin_ia32_rdtsc();
#endif
    pb_consume(mem, put, &words);             /* the method PUT stalled inside */

    while (s_get != put) {
        uint32_t at, target;
        uint32_t w;

        if (get_reg) {
            uint32_t g = *get_reg;

            if (g != s_pub) {                 /* the title moved GET mid-walk */
                pb_flow(s_get, 0xFFFFFFFFu, g & PB_PHYS_MASK);
                s_get = g & PB_PHYS_MASK;
                s_in_call = 0;
                s_pend_left = 0;
                s_pub = s_get;
                continue;
            }
            /* What the title reads as the GPU's position. Inside a CALLed
             * segment the hardware GET is the segment address; XDK D3D then
             * cannot place it in the ring and falls back to its fence-record
             * table to decide how much of the ring is free, and with a few
             * dozen fences outstanding (our walk runs well behind the title)
             * that table has recycled the record it needs and says the ring
             * is free. The title then overwrote commands this walk had not
             * yet read -- every desync traced so far was index data on top
             * of a half-read kick. The CALL word itself, in the ring, is a
             * position D3D can use and is never past anything unexecuted. */
            *get_reg = s_in_call ? s_ret - 4 : s_get;
            s_pub = *get_reg;
        }
        at = s_get;
        if (!s_in_call && pb_in_ring(at))
            s_ring_pos = at;

        /* Keep answering kickoffs while executing; see xbox_Nv2aAckBusyBits. */
        if (s_exec_enabled && (++s_ack_tick & 0xFFu) == 0)
            xbox_Nv2aAckBusyBits();

        if (s_get >= PB_RAM_BYTES) {
            s_get = put, s_pend_left = 0;      /* nothing readable to salvage */
            break;
        }
        w = *(const uint32_t *)(mem + (0x80000000u | s_get));
        s_hist[s_hist_n % PB_HIST].at = at;
        s_hist[s_hist_n % PB_HIST].w = w;
        s_hist_n++;

        if (++words > 0x400000u || unknown > 64) {
            pb_salvage(mem, s_ring_pos != 0xFFFFFFFFu ? s_ring_pos : s_get, put);
            s_get = put, s_pend_left = 0;                      /* lost sync, or runaway */
            break;
        }
        s_get = (s_get + 4) & PB_PHYS_MASK;

        if ((w & 0xE0000003u) == 0x20000000u) {         /* old-style jump */
            target = w & 0x1FFFFFFCu;
            if (pb_bad_target(w, at, target, put)) { pb_salvage(mem, s_ring_pos != 0xFFFFFFFFu ? s_ring_pos : s_get, put); s_get = put, s_pend_left = 0; break; }
            pb_flow(at, w, target);
            s_get = target;
            jumps++;
            if (at + 4 == put) {
                /* PUT sits right after the JUMP: the title has committed
                 * nothing past it yet. Walking on from the target would read
                 * the previous lap's words as this lap's commands. */
                static unsigned n;
                if (n++ < 8)
                    fprintf(stderr, "  [PB] PUT %08X sits right after the JUMP at %08X; "
                                    "stopping at %08X until the title writes more\n", put, at, target);
                break;
            }
            continue;
        }
        if ((w & 3u) == 1u) {                            /* jump */
            target = w & 0xFFFFFFFCu;
            if (pb_bad_target(w, at, target, put)) { pb_salvage(mem, s_ring_pos != 0xFFFFFFFFu ? s_ring_pos : s_get, put); s_get = put, s_pend_left = 0; break; }
            pb_flow(at, w, target);
            s_get = target;
            jumps++;
            if (at + 4 == put) {
                /* PUT sits right after the JUMP: the title has committed
                 * nothing past it yet. Walking on from the target would read
                 * the previous lap's words as this lap's commands. */
                static unsigned n;
                if (n++ < 8)
                    fprintf(stderr, "  [PB] PUT %08X sits right after the JUMP at %08X; "
                                    "stopping at %08X until the title writes more\n", put, at, target);
                break;
            }
            continue;
        }
        if ((w & 3u) == 2u) {                            /* call */
            target = w & 0xFFFFFFFCu;
            if (pb_bad_target(w, at, target, put)) { pb_salvage(mem, s_ring_pos != 0xFFFFFFFFu ? s_ring_pos : s_get, put); s_get = put, s_pend_left = 0; break; }
            if (s_in_call) {
                /* One level only: the hardware raises a CALL error and stops
                 * the pusher. Taking it would overwrite the return address
                 * and send the first RETURN to the wrong place. */
                s_in_call = 0;
                pb_salvage(mem, s_ring_pos != 0xFFFFFFFFu ? s_ring_pos : s_get, put);
                s_get = put, s_pend_left = 0;
                break;
            }
            s_ret = s_get;
            s_in_call = 1;
            s_tot_calls++;
            pb_flow(at, w, target);
            s_get = target;
            continue;
        }
        if ((w & 0xFFFF0003u) == 0x00020000u) {          /* return */
            if (s_in_call) {
                pb_flow(at, w, s_ret);
                s_get = s_ret;
                s_in_call = 0;
            }
            continue;
        }
        if ((w & 0x00030003u) == 0u) {
            uint32_t count  = (w >> 18) & 0x7FFu;
            uint32_t subch  = (w >> 13) & 7u;
            uint32_t method =  w & 0x1FFCu;
            int noninc = (w & 0xE0000000u) == 0x40000000u;

            /* PUT bounds the parameters too. A count that runs past it must
             * not carry s_get beyond PUT (the outer loop would then replay a
             * lap of stale ring contents), nor be forgotten (the leftover
             * parameters would be read as headers): the method stays pending
             * and the next walk finishes it. */
            s_pend_left = count;
            s_pend_i = 0;
            s_pend_method = method;
            s_pend_subch = subch;
            s_pend_noninc = noninc;
            pb_consume(mem, put, &words);
            unknown = 0;
            continue;
        }
        unknown++;
    }

    if (get_reg) {                            /* what the poll thread writes too */
        *get_reg = s_in_call ? s_ret - 4 : s_get;
        s_pub = *get_reg;
    }
    s_tot_words += words;
    s_tot_unknown += unknown;
    s_tot_jumps += jumps;
    s_tot_segments++;
#if defined(__x86_64__) || defined(__i386__)
    g_nv2a_walk_tsc += __builtin_ia32_rdtsc() - t0;
#endif
    XP_END();
    __atomic_store_n(&s_walked_put, put, __ATOMIC_RELEASE);
}

/* RECOMP_PB_WALK=dma selects the DMA-engine walk above. The default is the
 * segment scan below, which is what the executor did before the walk existed.
 *
 * The walk is the more faithful of the two: it follows JUMPs and ring wraps,
 * keeps its own GET, publishes it before every command, and answers kickoffs
 * mid-walk. That fidelity changes what XDK D3D does. Told exactly how far the
 * GPU has got, D3D's fence code takes its "GPU far behind" path: it patches a
 * software method (NO_OPERATION with a value) into the pushbuffer and sleeps
 * on an event that the GPU's interrupt for that method signals. A runtime that
 * does not deliver that interrupt leaves the title asleep for good -- a title
 * that ran under the segment scan froze on the first screen whose pushbuffer
 * made the walk long enough. So the walk is opt-in, for projects that deliver
 * the GPU's software-method interrupt. */
int nv2a_pb_dma_walk(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_PB_WALK");
        on = e && !strcmp(e, "dma");
    }
    return on;
}

/* The segment scan: walk commands from va to end_va. Returns the words
 * consumed.
 *
 * A CALL runs a subroutine -- a pushbuffer somewhere else in memory -- up to
 * its RETURN, then carries on after the CALL. RenderWare on the Xbox compiles
 * static geometry into pushbuffers and draws it with a CALL, so skipping CALLs
 * loses whole meshes. The NV2A has one level of subroutine, so a CALL inside a
 * call is not followed. A JUMP outside a call ends the segment: the ring's
 * wrap is handled by the caller, which scans the two pieces separately.
 * Addresses are physical, like PUT's, and reach guest memory through the
 * contiguous window. */
static uint32_t pb_walk_segment(uint32_t va, uint32_t end_va, int in_call,
                                uint32_t *jumps, uint32_t *unknown)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t words = 0;

    while (va < end_va && words < 0x100000u) {
        uint32_t w = *(const uint32_t *)(mem + va);
        va += 4;
        words++;

        if ((w & 3u) == 1u || (w & 0xE0000003u) == 0x20000000u) {
            uint32_t target = (w & 3u) == 1u ? (w & 0xFFFFFFFCu)
                                             : (w & 0x1FFFFFFCu);
            (*jumps)++;
            if (!in_call)
                break;                        /* the ring: a jump ends it */
            va = 0x80000000u | (target & 0x0FFFFFFFu);
            end_va = va + 0x400000u;
            continue;
        }
        if ((w & 0xFFFF0003u) == 0x00020000u) {  /* RETURN */
            if (in_call)
                break;
            continue;
        }
        if ((w & 3u) == 2u) {                    /* CALL */
            if (!in_call) {
                uint32_t sub = 0x80000000u | ((w & 0xFFFFFFFCu) & 0x0FFFFFFFu);
                s_tot_calls++;
                words += pb_walk_segment(sub, sub + 0x400000u, 1, jumps, unknown);
            }
            continue;
        }
        if ((w & 0x00030003u) == 0u) {
            uint32_t count  = (w >> 18) & 0x7FFu;
            uint32_t subch  = (w >> 13) & 7u;
            uint32_t method =  w & 0x1FFCu;
            int noninc = (w & 0xE0000000u) == 0x40000000u;

            for (uint32_t i = 0; i < count && va < end_va; i++) {
                uint32_t m = noninc ? method : method + i * 4;
                note(subch, m);
                if (s_exec_enabled)
                    nv2a_pb_exec_method(subch, m,
                                        *(const uint32_t *)(mem + va));
                va += 4;
                words++;
            }
            continue;
        }
        (*unknown)++;
    }
    return words;
}

void nv2a_pb_scan_segment(uint32_t start_va, uint32_t end_va)
{
    uint32_t words, jumps = 0, unknown = 0;

    if (!pb_walk_enabled() || end_va <= start_va)
        return;
    if (end_va - start_va > 0x400000u)        /* a sane single-frame bound */
        end_va = start_va + 0x400000u;

    g_nv2a_walk_seq++;
    words = pb_walk_segment(start_va, end_va, 0, &jumps, &unknown);

    s_tot_words += words;
    s_tot_unknown += unknown;
    s_tot_jumps += jumps;
    s_tot_segments++;
}


/* Called from the poll thread every tick. After a walk the fence word holds
 * the newest release the walk executed; a desync that skipped a span leaves
 * it behind every release in that span, and D3D's fence wait (sub_003487B0 in
 * Darkwatch, a spin on the word with no kernel call in it) never ends. So
 * when the title has submitted nothing new for a while and the word has not
 * moved, the newest release still readable in the ring is taken as executed
 * -- every command up to PUT has been walked or dropped by then, so it has
 * been passed either way. */
void nv2a_pb_fence_catchup(uint32_t put, uint32_t now_ms)
{
    static uint32_t last_check;
    static uint32_t last_put, last_fence;
    static unsigned still, caught;
    uint32_t va, cur, latest;
    uint8_t *mem;

    if (!pb_walk_enabled() || now_ms - last_check < 50)
        return;
    last_check = now_ms;
    va = xbox_Nv2aFenceWordVa();
    if (!va)
        return;
    mem = (uint8_t *)xbox_GetMemoryOffset();
    cur = *(volatile uint32_t *)(mem + va);
    if (put != last_put || cur != last_fence) {
        last_put = put, last_fence = cur, still = 0;
        return;
    }
    if (++still < 4)                       /* ~200 ms of nothing moving */
        return;
    still = 0;
    latest = nv2a_pb_latest_release(cur);
    if (latest && latest != cur) {
        *(volatile uint32_t *)(mem + va) = latest;
        if (++caught <= 8 || (caught % 64) == 0)
            fprintf(stderr, "  [PB] fence word stuck at %08X with PUT idle at %08X; "
                            "the ring holds a release up to %08X: caught up (#%u)\n",
                    cur, put, latest, caught);
        fflush(stderr);
    }
}

/* Has the walk consumed everything up to this PUT? The poll thread only
 * reports GET == PUT to the title when it has. */
int nv2a_pb_caught_up(uint32_t put_phys)
{
    if (!pb_walk_enabled())
        return 1;
    return s_get == (put_phys & PB_PHYS_MASK) && !s_in_call && !s_pend_left;
}

/* Where the title's pushbuffer writer is, if the title project knows; a
 * diagnostic for the desync reports. */
__attribute__((weak)) uint32_t nv2a_pb_title_write_ptr(void) { return 0; }

/* Has a walk to this PUT finished? The title's KickOff waits on this (see
 * Darkwatch's dw_patches.c): a title that runs ahead of the walk overwrites
 * commands it has not read. With the walk off there is nothing to wait for. */
int nv2a_pb_walk_done(uint32_t put_phys)
{
    if (!pb_walk_enabled())
        return 1;
    return __atomic_load_n(&s_walked_put, __ATOMIC_ACQUIRE) == (put_phys & PB_PHYS_MASK);
}
