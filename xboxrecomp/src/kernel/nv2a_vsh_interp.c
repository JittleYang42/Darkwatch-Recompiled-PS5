/**
 * NV2A vertex program interpreter. See nv2a_vsh_interp.h for why it exists.
 *
 * Encoding. An instruction is four dwords; dword 0 is unused. Fields, as
 * (dword, lowest bit, width):
 *
 *   ILU op        1 25 3      MAC op        1 21 4
 *   const index   1 13 8      input index   1  9 4   (shared by A, B, C)
 *   A negate      1  8 1      A swizzle     1  0 8   (x y z w, 2 bits each,
 *   A temp        2 28 4      A mux         2 26 2    x highest)
 *   B negate      2 25 1      B swizzle     2 17 8
 *   B temp        2 13 4      B mux         2 11 2
 *   C negate      2 10 1      C swizzle     2  2 8
 *   C temp        2 0 2 (high) : 3 30 2 (low)          C mux 3 28 2
 *   MAC temp mask 3 24 4      temp dest     3 20 4
 *   ILU temp mask 3 16 4      out mask      3 12 4
 *   out to o[]    3 11 1      out address   3  3 8   (0: to c[])
 *   out from ILU  3  2 1      a0.x relative 3  1 1   final 3 0 1
 *
 * Masks are x=8 y=4 z=2 w=1. A source mux of 1 reads a temp, 2 an input,
 * 3 a constant. Temp 12 is oPos. When an instruction carries both a MAC and
 * an ILU op, the ILU's temp write goes to R1 whatever the temp field says,
 * and both units read their sources before either writes.
 *
 * The layout is checked against real programs rather than taken on trust:
 * RECOMP_VSH_DUMP disassembles each program the title uploads, and an Xbox
 * D3D program is recognisable -- DP4s against the matrix constants, ending in
 * the runtime's screen-space epilogue (RCC of R12.w, then a MAD into oPos).
 */
#include "nv2a_vsh_interp.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t s_program[NV2A_VSH_SLOTS][4];
static float    s_const[NV2A_VSH_CONSTANTS][4];
static uint32_t s_load_slot, s_load_word, s_start_slot;
static uint32_t s_const_load, s_const_word;
static uint32_t s_cxt_write;
static uint32_t s_prog_gen = 1;          /* bumped by every program change  */
static int      s_const_trace = -1;      /* RECOMP_VSH_DUMP: constant traffic */

static int vsh_dump_enabled(void)
{
    if (s_const_trace < 0)
        s_const_trace = getenv("RECOMP_VSH_DUMP") != NULL;
    return s_const_trace;
}

static uint32_t field(const uint32_t *ins, int dw, int lo, int width)
{
    return (ins[dw] >> lo) & ((1u << width) - 1u);
}

/* ---- uploads ------------------------------------------------------------ */

void nv2a_vsh_set_load_slot(uint32_t slot)
{
    s_load_slot = slot;
    s_load_word = 0;
}

void nv2a_vsh_program_word(uint32_t word)
{
    if (s_load_slot < NV2A_VSH_SLOTS
        && s_program[s_load_slot][s_load_word] != word) {
        s_program[s_load_slot][s_load_word] = word;
        s_prog_gen++;
    }
    if (++s_load_word == 4) {
        s_load_word = 0;
        s_load_slot++;
    }
}

void nv2a_vsh_set_start_slot(uint32_t slot)
{
    if (s_start_slot != slot)
        s_prog_gen++;
    s_start_slot = slot;
}
void nv2a_vsh_set_cxt_write(uint32_t enable) { s_cxt_write = enable; }
int nv2a_vsh_writes_constants(void) { return s_cxt_write != 0; }

void nv2a_vsh_set_constant_load(uint32_t index)
{
    s_const_load = index;
    s_const_word = 0;
    if (vsh_dump_enabled())
        fprintf(stderr, "  [VSH] constant load -> c[%u]\n", index);
}

void nv2a_vsh_constant_word(uint32_t word)
{
    if (s_const_load < NV2A_VSH_CONSTANTS)
        memcpy(&s_const[s_const_load][s_const_word], &word, 4);
    else if (vsh_dump_enabled()) {
        static int warned;
        if (warned++ < 8)
            fprintf(stderr, "  [VSH] constant write DROPPED (c[%u] >= %u)\n",
                    s_const_load, NV2A_VSH_CONSTANTS);
    }
    if (vsh_dump_enabled() && s_const_word == 3 && s_const_load >= 64) {
        static int shown;
        if (shown++ < 96)
            fprintf(stderr, "  [VSH] c[%u] = %.6g %.6g %.6g %.6g\n",
                    s_const_load, s_const[s_const_load][0],
                    s_const[s_const_load][1], s_const[s_const_load][2],
                    s_const[s_const_load][3]);
    }
    if (++s_const_word == 4) {
        s_const_word = 0;
        s_const_load++;
    }
}

void nv2a_vsh_set_constant(uint32_t index, const float v[4])
{
    if (index < NV2A_VSH_CONSTANTS)
        memcpy(s_const[index], v, sizeof s_const[index]);
}

void nv2a_vsh_set_instruction(uint32_t slot, const uint32_t words[4])
{
    if (slot < NV2A_VSH_SLOTS) {
        memcpy(s_program[slot], words, sizeof s_program[slot]);
        s_prog_gen++;
    }
}

/* ---- disassembly (RECOMP_VSH_DUMP) --------------------------------------- */

static const char *const k_mac[16] = {
    "nop", "mov", "mul", "add", "mad", "dp3", "dph", "dp4",
    "dst", "min", "max", "slt", "sge", "arl", "mac14", "mac15"
};
static const char *const k_ilu[8] = {
    "nop", "mov", "rcp", "rcc", "rsq", "exp", "log", "lit"
};

static void dis_src(char *b, size_t n, const uint32_t *ins, int which)
{
    static const char sw[] = "xyzw";
    uint32_t neg, swz, mux, reg;

    if (which == 0) {
        neg = field(ins, 1, 8, 1); swz = field(ins, 1, 0, 8);
        mux = field(ins, 2, 26, 2); reg = field(ins, 2, 28, 4);
    } else if (which == 1) {
        neg = field(ins, 2, 25, 1); swz = field(ins, 2, 17, 8);
        mux = field(ins, 2, 11, 2); reg = field(ins, 2, 13, 4);
    } else {
        neg = field(ins, 2, 10, 1); swz = field(ins, 2, 2, 8);
        mux = field(ins, 3, 28, 2);
        reg = (field(ins, 2, 0, 2) << 2) | field(ins, 3, 30, 2);
    }
    if (mux == 1)
        snprintf(b, n, "%sR%u", neg ? "-" : "", reg);
    else if (mux == 2)
        snprintf(b, n, "%sv%u", neg ? "-" : "", field(ins, 1, 9, 4));
    else
        snprintf(b, n, "%sc[%s%u]", neg ? "-" : "",
                 field(ins, 3, 1, 1) ? "a0.x+" : "", field(ins, 1, 13, 8));
    {
        size_t l = strlen(b);
        if (l + 6 < n)
            snprintf(b + l, n - l, ".%c%c%c%c", sw[(swz >> 6) & 3],
                     sw[(swz >> 4) & 3], sw[(swz >> 2) & 3], sw[swz & 3]);
    }
}

static void dump_program(uint32_t start)
{
    uint32_t s;
    fprintf(stderr, "  [VSH] program at slot %u:\n", start);
    for (s = start; s < NV2A_VSH_SLOTS; s++) {
        const uint32_t *ins = s_program[s];
        char a[40], bb[40], c[40];
        dis_src(a, sizeof a, ins, 0);
        dis_src(bb, sizeof bb, ins, 1);
        dis_src(c, sizeof c, ins, 2);
        fprintf(stderr, "  [VSH] %3u %-4s %-4s A=%s B=%s C=%s  R%u mac%X ilu%X"
                " out%s%u m%X%s%s\n", s,
                k_mac[field(ins, 1, 21, 4)], k_ilu[field(ins, 1, 25, 3)],
                a, bb, c, field(ins, 3, 20, 4), field(ins, 3, 24, 4),
                field(ins, 3, 16, 4), field(ins, 3, 11, 1) ? "o" : "c",
                field(ins, 3, 3, 8), field(ins, 3, 12, 4),
                field(ins, 3, 2, 1) ? " ilu" : " mac",
                field(ins, 3, 0, 1) ? " FINAL" : "");
        if (field(ins, 3, 0, 1))
            break;
    }
    fflush(stderr);
}

const float *nv2a_vsh_constant(uint32_t index)
{
    return s_const[index < NV2A_VSH_CONSTANTS ? index : 0];
}

void nv2a_vsh_constant_component(uint32_t index, uint32_t comp, uint32_t word)
{
    if (index < NV2A_VSH_CONSTANTS && comp < 4)
        memcpy(&s_const[index][comp], &word, 4);
}

/* ---- execution ----------------------------------------------------------- */

typedef struct { float v[4]; } vec4;

static vec4 read_src(const uint32_t *ins, int which,
                     const float in[NV2A_VSH_INPUTS][4],
                     const vec4 *temp, const vec4 *opos, int a0)
{
    uint32_t neg, swz, mux, reg;
    const float *s;
    vec4 r;

    if (which == 0) {
        neg = field(ins, 1, 8, 1); swz = field(ins, 1, 0, 8);
        mux = field(ins, 2, 26, 2); reg = field(ins, 2, 28, 4);
    } else if (which == 1) {
        neg = field(ins, 2, 25, 1); swz = field(ins, 2, 17, 8);
        mux = field(ins, 2, 11, 2); reg = field(ins, 2, 13, 4);
    } else {
        neg = field(ins, 2, 10, 1); swz = field(ins, 2, 2, 8);
        mux = field(ins, 3, 28, 2);
        reg = (field(ins, 2, 0, 2) << 2) | field(ins, 3, 30, 2);
    }

    if (mux == 1)
        s = (reg == 12) ? opos->v : (reg < 12 ? temp[reg].v : temp[0].v);
    else if (mux == 2)
        s = in[field(ins, 1, 9, 4)];
    else {
        int ci = (int)field(ins, 1, 13, 8);
        if (field(ins, 3, 1, 1))
            ci += a0;
        if (ci < 0 || ci >= NV2A_VSH_CONSTANTS)
            ci = 0;
        s = s_const[ci];
    }
    r.v[0] = s[(swz >> 6) & 3];
    r.v[1] = s[(swz >> 4) & 3];
    r.v[2] = s[(swz >> 2) & 3];
    r.v[3] = s[swz & 3];
    if (neg) {
        r.v[0] = -r.v[0]; r.v[1] = -r.v[1];
        r.v[2] = -r.v[2]; r.v[3] = -r.v[3];
    }
    return r;
}

static void write_masked(float *dst, const vec4 *src, uint32_t mask)
{
    if (mask & 8) dst[0] = src->v[0];
    if (mask & 4) dst[1] = src->v[1];
    if (mask & 2) dst[2] = src->v[2];
    if (mask & 1) dst[3] = src->v[3];
}

static vec4 splat(float f)
{
    vec4 r;
    r.v[0] = r.v[1] = r.v[2] = r.v[3] = f;
    return r;
}

static vec4 run_mac(uint32_t op, vec4 a, vec4 b, vec4 c, int *a0)
{
    vec4 r = a;
    int i;

    switch (op) {
    case 1: break;                                            /* mov */
    case 2: for (i = 0; i < 4; i++) r.v[i] = a.v[i] * b.v[i]; break;
    case 3: for (i = 0; i < 4; i++) r.v[i] = a.v[i] + c.v[i]; break;
    case 4: for (i = 0; i < 4; i++) r.v[i] = a.v[i] * b.v[i] + c.v[i]; break;
    case 5: r = splat(a.v[0]*b.v[0] + a.v[1]*b.v[1] + a.v[2]*b.v[2]); break;
    case 6: r = splat(a.v[0]*b.v[0] + a.v[1]*b.v[1] + a.v[2]*b.v[2]
                      + b.v[3]); break;                        /* dph */
    case 7: r = splat(a.v[0]*b.v[0] + a.v[1]*b.v[1] + a.v[2]*b.v[2]
                      + a.v[3]*b.v[3]); break;
    case 8: r.v[0] = 1.0f; r.v[1] = a.v[1] * b.v[1];            /* dst */
            r.v[2] = a.v[2]; r.v[3] = b.v[3]; break;
    case 9: for (i = 0; i < 4; i++) r.v[i] = a.v[i] < b.v[i] ? a.v[i] : b.v[i]; break;
    case 10: for (i = 0; i < 4; i++) r.v[i] = a.v[i] >= b.v[i] ? a.v[i] : b.v[i]; break;
    case 11: for (i = 0; i < 4; i++) r.v[i] = a.v[i] < b.v[i] ? 1.0f : 0.0f; break;
    case 12: for (i = 0; i < 4; i++) r.v[i] = a.v[i] >= b.v[i] ? 1.0f : 0.0f; break;
    case 13: *a0 = (int)floorf(a.v[0] + 0.001f); break;         /* arl */
    default: break;
    }
    return r;
}

static vec4 run_ilu(uint32_t op, vec4 c)
{
    float x = c.v[0];
    vec4 r = c;

    switch (op) {
    case 1: break;                                            /* mov */
    case 2: r = splat(x == 0.0f ? INFINITY : 1.0f / x); break;  /* rcp */
    case 3: {                                                  /* rcc */
        float f = 1.0f / x;
        if (fabsf(f) < 5.42101e-20f) f = f < 0 ? -5.42101e-20f : 5.42101e-20f;
        if (fabsf(f) > 1.884467e19f) f = f < 0 ? -1.884467e19f : 1.884467e19f;
        r = splat(f);
        break;
    }
    case 4: r = splat(1.0f / sqrtf(fabsf(x))); break;           /* rsq */
    case 5: {                                                  /* exp */
        float fl = floorf(x);
        r.v[0] = exp2f(fl); r.v[1] = x - fl; r.v[2] = exp2f(x); r.v[3] = 1.0f;
        break;
    }
    case 6: {                                                  /* log */
        float ax = fabsf(x);
        if (ax == 0.0f) {
            r.v[0] = r.v[2] = -INFINITY; r.v[1] = 1.0f;
        } else {
            float e = floorf(log2f(ax));
            r.v[0] = e; r.v[1] = ax / exp2f(e); r.v[2] = log2f(ax);
        }
        r.v[3] = 1.0f;
        break;
    }
    case 7: {                                                  /* lit */
        float nl = c.v[0] > 0.0f ? c.v[0] : 0.0f;
        float nh = c.v[1] > 0.0f ? c.v[1] : 0.0f;
        float p = c.v[3] < -127.9961f ? -127.9961f
                : (c.v[3] > 127.9961f ? 127.9961f : c.v[3]);
        r.v[0] = 1.0f; r.v[1] = nl;
        r.v[2] = c.v[0] > 0.0f ? powf(nh, p) : 0.0f; r.v[3] = 1.0f;
        break;
    }
    default: break;
    }
    return r;
}

/* RECOMP_VSH_DUMP: disassemble each distinct program once. */
static void maybe_dump(void)
{
    static int dump = -1;
    static uint32_t dumped[16];
    static int ndumped;

    if (dump < 0)
        dump = getenv("RECOMP_VSH_DUMP") != NULL;
    if (dump && ndumped < 16) {
        /* By content: titles reload different programs into the same slot. */
        uint32_t h = 2166136261u, t;
        int i, seen = 0;
        for (t = s_start_slot; t < NV2A_VSH_SLOTS; t++) {
            for (i = 0; i < 4; i++)
                h = (h ^ s_program[t][i]) * 16777619u;
            if (field(s_program[t], 3, 0, 1))
                break;
        }
        for (i = 0; i < ndumped; i++)
            if (dumped[i] == h) seen = 1;
        if (!seen) {
            dumped[ndumped++] = h;
            dump_program(s_start_slot);
        }
    }
}

int nv2a_vsh_run(const float in[NV2A_VSH_INPUTS][4], Nv2aVshOutput *out)
{
    vec4 temp[12], opos;
    float outregs[13][4];
    int a0 = 0;
    uint32_t s;

    maybe_dump();

    memset(temp, 0, sizeof temp);
    memset(&opos, 0, sizeof opos);
    memset(outregs, 0, sizeof outregs);
    outregs[3][3] = outregs[4][3] = 1.0f;      /* colours default opaque */
    /* Unwritten texture coordinates are (0,0,0,1), as xemu initialises them:
     * a projective stage divides by q, and q = 0 would put every texel of an
     * unwritten stage at infinity. */
    outregs[9][3] = outregs[10][3] = outregs[11][3] = outregs[12][3] = 1.0f;

    for (s = s_start_slot; s < NV2A_VSH_SLOTS; s++) {
        const uint32_t *ins = s_program[s];
        uint32_t mac = field(ins, 1, 21, 4), ilu = field(ins, 1, 25, 3);
        uint32_t mac_mask = field(ins, 3, 24, 4), ilu_mask = field(ins, 3, 16, 4);
        uint32_t tdst = field(ins, 3, 20, 4);
        uint32_t omask = field(ins, 3, 12, 4), oaddr = field(ins, 3, 3, 8);
        vec4 a = read_src(ins, 0, in, temp, &opos, a0);
        vec4 b = read_src(ins, 1, in, temp, &opos, a0);
        vec4 c = read_src(ins, 2, in, temp, &opos, a0);
        vec4 mres = {{0, 0, 0, 0}}, ires = {{0, 0, 0, 0}};

        if (mac)
            mres = run_mac(mac, a, b, c, &a0);
        if (ilu)
            ires = run_ilu(ilu, c);

        if (mac && mac != 13 && mac_mask) {
            float *d = tdst == 12 ? opos.v : (tdst < 12 ? temp[tdst].v : NULL);
            if (d) write_masked(d, &mres, mac_mask);
        }
        if (ilu && ilu_mask) {
            /* Paired with any MAC op, the ILU can only write R1 -- even when
             * the MAC half writes no temp at all. The D3D epilogue is exactly
             * that: `mul o[0].xyz` beside `rcc R1.x`, then `mad` with R1.x.
             * Sending the rcc to the temp field (R7 there) left R1.x at 0 and
             * put every vertex of every 3D batch at c[59]. */
            uint32_t it = mac ? 1u : tdst;
            float *d = it == 12 ? opos.v : (it < 12 ? temp[it].v : NULL);
            if (d) write_masked(d, &ires, ilu_mask);
        }
        if (omask) {
            const vec4 *src = field(ins, 3, 2, 1) ? &ires : &mres;
            if (!field(ins, 3, 11, 1)) {                  /* to c[] */
                if (s_cxt_write && oaddr < NV2A_VSH_CONSTANTS)
                    write_masked(s_const[oaddr], src, omask);
            } else if (oaddr == 0) {
                write_masked(opos.v, src, omask);
            } else if (oaddr < 13) {
                write_masked(outregs[oaddr], src, omask);
            }
        }
        if (field(ins, 3, 0, 1)) {
            memcpy(out->pos, opos.v, sizeof out->pos);
            memcpy(out->d0, outregs[3], sizeof out->d0);
            memcpy(out->d1, outregs[4], sizeof out->d1);
            memcpy(out->fog, outregs[5], sizeof out->fog);
            memcpy(out->tex[0], outregs[9], sizeof out->tex[0]);
            memcpy(out->tex[1], outregs[10], sizeof out->tex[1]);
            memcpy(out->tex[2], outregs[11], sizeof out->tex[2]);
            memcpy(out->tex[3], outregs[12], sizeof out->tex[3]);
            return 1;
        }
    }
    return 0;
}

/* ---- eight vertices at a time --------------------------------------------
 *
 * nv2a_vsh_run decodes every field of every instruction for every vertex,
 * and a skinned character's program is dozens of instructions: in a crowd
 * that was most of the executor's time, and the title waits on the
 * executor at each kick. The program has no flow control, so every vertex
 * runs the same instruction stream: decode it once per upload, and run it
 * on eight vertices together with each register held as four 8-lane
 * vectors (x of all eight lanes, then y, ...). A swizzle is then only a
 * choice of vector, and the arithmetic is the scalar code's, lane by lane
 * (same expressions, same order). Relative constant reads and the ILU's
 * scalar functions go lane by lane. Programs that write constants
 * (_CXT_WRITE_EN) stay on nv2a_vsh_run: their vertices are not
 * independent. */

typedef float   vsh_vf __attribute__((vector_size(32)));
typedef int32_t vsh_vi __attribute__((vector_size(32)));
typedef struct { vsh_vf c[4]; } vsh_v4;

typedef struct {
    uint8_t mux, reg, neg, rel;
    uint8_t swz[4];
    uint16_t ci;
} VshSrc;

typedef struct {
    uint8_t mac, ilu, mac_mask, ilu_mask;
    uint8_t mac_dst, ilu_dst;            /* 0..11 temp, 12 oPos, 0xFF none */
    uint8_t omask, oaddr, o_ilu, o_dst;  /* o_dst: 0xFF none, 0 oPos, else o[] */
    uint8_t use;                         /* bit 0 A, 1 B, 2 C */
    uint8_t final;
    VshSrc s[3];
} VshOp;

struct Nv2aVshProg {
    uint32_t gen;
    int n;
    uint32_t inputs;
    VshOp op[NV2A_VSH_SLOTS];
};

static struct Nv2aVshProg s_prog;

static void decode_src(VshSrc *d, const uint32_t *ins, int which)
{
    uint32_t neg, swz, mux, reg;

    if (which == 0) {
        neg = field(ins, 1, 8, 1); swz = field(ins, 1, 0, 8);
        mux = field(ins, 2, 26, 2); reg = field(ins, 2, 28, 4);
    } else if (which == 1) {
        neg = field(ins, 2, 25, 1); swz = field(ins, 2, 17, 8);
        mux = field(ins, 2, 11, 2); reg = field(ins, 2, 13, 4);
    } else {
        neg = field(ins, 2, 10, 1); swz = field(ins, 2, 2, 8);
        mux = field(ins, 3, 28, 2);
        reg = (field(ins, 2, 0, 2) << 2) | field(ins, 3, 30, 2);
    }
    d->mux = (uint8_t)(mux == 1 || mux == 2 ? mux : 3);
    d->neg = (uint8_t)neg;
    d->swz[0] = (uint8_t)((swz >> 6) & 3);
    d->swz[1] = (uint8_t)((swz >> 4) & 3);
    d->swz[2] = (uint8_t)((swz >> 2) & 3);
    d->swz[3] = (uint8_t)(swz & 3);
    d->rel = 0;
    d->ci = 0;
    if (d->mux == 1)
        d->reg = (uint8_t)(reg == 12 ? 12 : (reg < 12 ? reg : 0));
    else if (d->mux == 2)
        d->reg = (uint8_t)field(ins, 1, 9, 4);
    else {
        d->reg = 0;
        d->ci = (uint16_t)field(ins, 1, 13, 8);
        d->rel = (uint8_t)field(ins, 3, 1, 1);
        if (!d->rel && d->ci >= NV2A_VSH_CONSTANTS)
            d->ci = 0;
    }
}

const struct Nv2aVshProg *nv2a_vsh_prepare(void)
{
    struct Nv2aVshProg *p = &s_prog;
    uint32_t s;

    if (s_cxt_write)
        return NULL;
    if (p->gen == s_prog_gen)
        return p->n ? p : NULL;
    maybe_dump();
    p->gen = s_prog_gen;
    p->n = 0;
    p->inputs = 0;
    for (s = s_start_slot; s < NV2A_VSH_SLOTS; s++) {
        const uint32_t *ins = s_program[s];
        VshOp *o = &p->op[p->n++];
        uint32_t mac = field(ins, 1, 21, 4), ilu = field(ins, 1, 25, 3);
        uint32_t tdst = field(ins, 3, 20, 4);
        int k;

        memset(o, 0, sizeof *o);
        o->mac = (uint8_t)mac;
        o->ilu = (uint8_t)ilu;
        o->mac_mask = (uint8_t)field(ins, 3, 24, 4);
        o->ilu_mask = (uint8_t)field(ins, 3, 16, 4);
        o->mac_dst = (uint8_t)(mac && mac != 13 && o->mac_mask && tdst <= 12 ? tdst : 0xFF);
        {
            uint32_t it = mac ? 1u : tdst;
            o->ilu_dst = (uint8_t)(ilu && o->ilu_mask && it <= 12 ? it : 0xFF);
        }
        o->omask = (uint8_t)field(ins, 3, 12, 4);
        o->oaddr = (uint8_t)field(ins, 3, 3, 8);
        o->o_ilu = (uint8_t)field(ins, 3, 2, 1);
        o->o_dst = 0xFF;
        if (o->omask && field(ins, 3, 11, 1) && o->oaddr < 13)
            o->o_dst = o->oaddr;            /* 0 is oPos; to c[] is dropped (no CXT write) */
        o->final = (uint8_t)field(ins, 3, 0, 1);
        switch (mac) {
        case 0: break;
        case 1: case 13: case 14: case 15: o->use |= 1; break;
        case 3: o->use |= 1 | 4; break;
        case 4: o->use |= 1 | 2 | 4; break;
        default: o->use |= 1 | 2; break;
        }
        if (ilu)
            o->use |= 4;
        for (k = 0; k < 3; k++) {
            decode_src(&o->s[k], ins, k);
            if ((o->use >> k & 1) && o->s[k].mux == 2)
                p->inputs |= 1u << o->s[k].reg;
        }
        if (o->final)
            return p;
    }
    p->n = 0;                               /* no FINAL: nothing to run */
    return NULL;
}

uint32_t nv2a_vsh_prog_inputs(const struct Nv2aVshProg *p) { return p->inputs; }

static inline __attribute__((always_inline)) vsh_vf vsh_splat(float f)
{
    vsh_vf r = { f, f, f, f, f, f, f, f };
    return r;
}

static inline __attribute__((always_inline)) vsh_vf vsh_sel(vsh_vi m, vsh_vf a, vsh_vf b)
{
    return (vsh_vf)((m & (vsh_vi)a) | (~m & (vsh_vi)b));
}

static inline __attribute__((always_inline)) void vsh_read(vsh_v4 *r, const VshSrc *s, const vsh_v4 *in,
                     const vsh_v4 *temp, const vsh_v4 *opos, const vsh_vi a0)
{
    int k, l;

    if (s->mux == 1 || s->mux == 2) {
        const vsh_v4 *src = s->mux == 2 ? &in[s->reg]
                          : (s->reg == 12 ? opos : &temp[s->reg]);
        for (k = 0; k < 4; k++)
            r->c[k] = src->c[s->swz[k]];
    } else if (!s->rel) {
        const float *c = s_const[s->ci];
        for (k = 0; k < 4; k++)
            r->c[k] = vsh_splat(c[s->swz[k]]);
    } else {
        for (l = 0; l < 8; l++) {
            int ci = (int)s->ci + a0[l];
            const float *c;
            if (ci < 0 || ci >= NV2A_VSH_CONSTANTS)
                ci = 0;
            c = s_const[ci];
            for (k = 0; k < 4; k++)
                r->c[k][l] = c[s->swz[k]];
        }
    }
    if (s->neg)
        for (k = 0; k < 4; k++)
            r->c[k] = -r->c[k];
}

static inline __attribute__((always_inline)) void vsh_write(vsh_v4 *d, const vsh_v4 *s, uint32_t mask)
{
    if (mask & 8) d->c[0] = s->c[0];
    if (mask & 4) d->c[1] = s->c[1];
    if (mask & 2) d->c[2] = s->c[2];
    if (mask & 1) d->c[3] = s->c[3];
}

int nv2a_vsh_run8(const struct Nv2aVshProg *p,
                  const float in[NV2A_VSH_INPUTS][4][8], int n,
                  Nv2aVshOutput *const out[8])
{
    vsh_v4 vin[NV2A_VSH_INPUTS], temp[12], opos, outregs[13];
    vsh_vi a0 = { 0, 0, 0, 0, 0, 0, 0, 0 };
    const vsh_vf zero = vsh_splat(0.0f), one = vsh_splat(1.0f);
    uint32_t inputs = p->inputs;
    int i, k, l;

    for (i = 0; i < NV2A_VSH_INPUTS; i++)
        if (inputs >> i & 1)
            for (k = 0; k < 4; k++)
                memcpy(&vin[i].c[k], in[i][k], sizeof vin[i].c[k]);
    for (i = 0; i < 12; i++)
        for (k = 0; k < 4; k++)
            temp[i].c[k] = zero;
    for (k = 0; k < 4; k++)
        opos.c[k] = zero;
    for (i = 0; i < 13; i++)
        for (k = 0; k < 4; k++)
            outregs[i].c[k] = zero;
    outregs[3].c[3] = outregs[4].c[3] = one;
    outregs[9].c[3] = outregs[10].c[3] = outregs[11].c[3] = outregs[12].c[3] = one;

    for (i = 0; i < p->n; i++) {
        const VshOp *o = &p->op[i];
        vsh_v4 a, b, c, mres, ires;

        if (o->use & 1) vsh_read(&a, &o->s[0], vin, temp, &opos, a0);
        if (o->use & 2) vsh_read(&b, &o->s[1], vin, temp, &opos, a0);
        if (o->use & 4) vsh_read(&c, &o->s[2], vin, temp, &opos, a0);
        for (k = 0; k < 4; k++)
            mres.c[k] = ires.c[k] = zero;

        switch (o->mac) {
        case 0: break;
        case 1: mres = a; break;
        case 2: for (k = 0; k < 4; k++) mres.c[k] = a.c[k] * b.c[k]; break;
        case 3: for (k = 0; k < 4; k++) mres.c[k] = a.c[k] + c.c[k]; break;
        case 4: for (k = 0; k < 4; k++) mres.c[k] = a.c[k] * b.c[k] + c.c[k]; break;
        case 5: {
            vsh_vf d = a.c[0]*b.c[0] + a.c[1]*b.c[1] + a.c[2]*b.c[2];
            mres.c[0] = mres.c[1] = mres.c[2] = mres.c[3] = d;
            break;
        }
        case 6: {
            vsh_vf d = a.c[0]*b.c[0] + a.c[1]*b.c[1] + a.c[2]*b.c[2]
                     + b.c[3];
            mres.c[0] = mres.c[1] = mres.c[2] = mres.c[3] = d;
            break;
        }
        case 7: {
            vsh_vf d = a.c[0]*b.c[0] + a.c[1]*b.c[1] + a.c[2]*b.c[2]
                     + a.c[3]*b.c[3];
            mres.c[0] = mres.c[1] = mres.c[2] = mres.c[3] = d;
            break;
        }
        case 8:
            mres.c[0] = one; mres.c[1] = a.c[1] * b.c[1];
            mres.c[2] = a.c[2]; mres.c[3] = b.c[3];
            break;
        case 9:  for (k = 0; k < 4; k++) mres.c[k] = vsh_sel(a.c[k] <  b.c[k], a.c[k], b.c[k]); break;
        case 10: for (k = 0; k < 4; k++) mres.c[k] = vsh_sel(a.c[k] >= b.c[k], a.c[k], b.c[k]); break;
        case 11: for (k = 0; k < 4; k++) mres.c[k] = vsh_sel(a.c[k] <  b.c[k], one, zero); break;
        case 12: for (k = 0; k < 4; k++) mres.c[k] = vsh_sel(a.c[k] >= b.c[k], one, zero); break;
        case 13:
            mres = a;
            for (l = 0; l < 8; l++)
                a0[l] = (int)floorf(a.c[0][l] + 0.001f);
            break;
        default: mres = a; break;
        }

        if (o->ilu) {
            for (l = 0; l < 8; l++) {
                vec4 cl, r;
                for (k = 0; k < 4; k++)
                    cl.v[k] = c.c[k][l];
                r = run_ilu(o->ilu, cl);
                for (k = 0; k < 4; k++)
                    ires.c[k][l] = r.v[k];
            }
        }

        if (o->mac_dst != 0xFF)
            vsh_write(o->mac_dst == 12 ? &opos : &temp[o->mac_dst], &mres, o->mac_mask);
        if (o->ilu_dst != 0xFF)
            vsh_write(o->ilu_dst == 12 ? &opos : &temp[o->ilu_dst], &ires, o->ilu_mask);
        if (o->o_dst != 0xFF)
            vsh_write(o->o_dst == 0 ? &opos : &outregs[o->o_dst],
                      o->o_ilu ? &ires : &mres, o->omask);
    }

    for (l = 0; l < n && l < 8; l++) {
        Nv2aVshOutput *d = out[l];
        for (k = 0; k < 4; k++) {
            d->pos[k] = opos.c[k][l];
            d->d0[k] = outregs[3].c[k][l];
            d->d1[k] = outregs[4].c[k][l];
            d->fog[k] = outregs[5].c[k][l];
            d->tex[0][k] = outregs[9].c[k][l];
            d->tex[1][k] = outregs[10].c[k][l];
            d->tex[2][k] = outregs[11].c[k][l];
            d->tex[3][k] = outregs[12].c[k][l];
        }
    }
    return 1;
}
