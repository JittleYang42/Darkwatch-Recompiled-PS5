/*
 * MCPX APU DSP (GP/EP) - Stub implementation
 *
 * The DSP Global Processor (GP) and Encode Processor (EP) handle effects
 * processing (reverb, chorus, etc.) and final output encoding. The full
 * DSP is ~3000 lines of DSP56300 emulation code.
 *
 * For initial audio, we bypass the DSP entirely:
 * - VP mixbins are passed directly to the EP output
 * - GP effects processing is skipped
 * - The EP just copies mixbin 0/1 (front L/R) to the monitor buffer
 *
 * This gives us basic voice playback without effects. The DSP can be
 * connected later for reverb, EQ, and other processing.
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "apu_state.h"
#include "fpconv.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ── DSP command doorbell acknowledgement ────────────────────────────────
 *
 * DirectSound does not stop at creating the device. It hands the audio DSP a
 * command block in guest RAM, writes a command word, and spins until the DSP
 * writes zero back. On real hardware the GP runs a DSP56300 program that does
 * that. Here the DSP is a passthrough stub, so the word never changes and the
 * title hangs inside DirectSound initialisation -- which on Wreckless gates the
 * entire engine, not just audio.
 *
 * RECOMP_APU_DSP_ACK=<addr>[,<addr>...] clears those guest dwords once per APU
 * frame, which is what "the command completed" looks like to the title.
 *
 * RECOMP_APU_DSP_ACK=auto, the default when the variable is unset, finds the
 * doorbell from what the title programs; "0" or "off" turns the ack off.
 * The XDK DSOUND GP program takes its commands through the first page of
 * the GP's scratch space: a 2 KB command FIFO, then a header whose dword at
 * +0x810 is the doorbell the host writes and the DSP program clears (the
 * DSOUND submit of every XDK title seen spins on <block>+0x810). The
 * scratch pages are what NV_PAPU_GPSADDR's scatter-gather table maps -- 8
 * bytes an entry, the physical page in the first dword, as xemu's apu.c
 * scratch_rw walks it -- so the doorbell is SGE[0] + 0x810 wherever the
 * contiguous allocator put the block. A fixed address moves with allocation
 * order (reserving physical page 0 moved one title's by a page); this does
 * not. Wreckless fits too: its doorbell 0x014F8810 is the first page of the
 * scratch its GPSADDR table (0x01504000) maps, not the table itself, which
 * is where the earlier note here looked.
 *
 * ponytail: this is a handshake acknowledgement, not a DSP. It says every
 * command succeeded instantly and computes nothing, so anything whose *result*
 * the title reads back will still be wrong. The real fix is DSP56300 emulation
 * in the GP/EP; this exists so audio init stops blocking everything behind it.
 */
#define APU_DSP_ACK_MAX 8
#define GP_DOORBELL_OFFSET 0x810u
static uint32_t s_dsp_ack[APU_DSP_ACK_MAX];
static int s_dsp_ack_count = -1;
static int s_dsp_ack_auto;                /* =auto: derive from GPSADDR */
static uint32_t s_dsp_ack_auto_page;      /* the page last reported */
static volatile int s_gp_in_reset;        /* last GPRST write held the GP */

static void dsp_ack_init(void)
{
    const char *spec = getenv("RECOMP_APU_DSP_ACK");
    char buf[256], *p, *end;

    s_dsp_ack_count = 0;
    if (!spec || !*spec)
        spec = "auto";                    /* default: derive it */
    if (!strcmp(spec, "0") || !strcmp(spec, "off"))
        return;
    if (!strcmp(spec, "auto")) {
        s_dsp_ack_auto = 1;
        fprintf(stderr, "[APU] DSP doorbell ack: auto (GP scratch page 0"
                        " + 0x%X, from GPSADDR)\n", GP_DOORBELL_OFFSET);
        return;
    }
    strncpy(buf, spec, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    for (p = buf; *p && s_dsp_ack_count < APU_DSP_ACK_MAX; ) {
        unsigned long v = strtoul(p, &end, 0);
        if (end == p)
            break;
        if (v)
            s_dsp_ack[s_dsp_ack_count++] = (uint32_t)v;
        p = (*end == ',') ? end + 1 : end;
    }
    if (s_dsp_ack_count)
        fprintf(stderr, "[APU] DSP doorbell ack: %d address(es), first 0x%08X\n",
                s_dsp_ack_count, s_dsp_ack[0]);
}

/* SUM EVERY MIXBIN THE GUEST ROUTED TO, NOT JUST THE FIRST TWO.
 *
 * Default ON. RECOMP_APU_MIXDOWN_ALL=0 restores the previous two-bin read,
 * because this changes audible output for every title and an escape hatch
 * costs one branch. */
/* Per-bin energy and clipping, for apu_core.c's 10 s health line. */
double g_bin_energy[NUM_MIXBINS];
unsigned g_bin_clip[NUM_MIXBINS], g_out_clip;

/* RECOMP_APU_MIXDOWN: speakers (default), all (sum every bin, the old
 * default; RECOMP_APU_MIXDOWN_ALL=1 also selects it), or front (bins 0/1). */
enum { MIXDOWN_FRONT, MIXDOWN_ALL, MIXDOWN_SPEAKERS };
static int mcpx_apu_mixdown_mode(void)
{
    static int mode = -1;
    if (mode < 0) {
        const char *e = getenv("RECOMP_APU_MIXDOWN");
        const char *a = getenv("RECOMP_APU_MIXDOWN_ALL");
        mode = MIXDOWN_SPEAKERS;
        if (a && *a) mode = atoi(a) ? MIXDOWN_ALL : MIXDOWN_FRONT;
        if (e && !strcmp(e, "all")) mode = MIXDOWN_ALL;
        if (e && !strcmp(e, "front")) mode = MIXDOWN_FRONT;
        if (e && !strcmp(e, "speakers")) mode = MIXDOWN_SPEAKERS;
    }
    return mode;
}

/* A small reverb for the I3DL2 send (bin 10): four combs and two allpasses
 * per side (Freeverb's tunings at 48 kHz, the right side offset for width),
 * returned at about -9 dB. Not the GP's I3DL2 -- the title's room
 * parameters are not read -- but it keeps the ambience the send carries
 * without playing the dry sound a second time. */
#define RV_COMBS 4
#define RV_APS 2
static const int rv_comb_len[RV_COMBS] = { 1617, 1557, 1491, 1422 };
static const int rv_ap_len[RV_APS] = { 556, 441 };
#define RV_SPREAD 23
static float rv_comb[2][RV_COMBS][1617 + RV_SPREAD];
static float rv_comb_lp[2][RV_COMBS];
static int   rv_comb_pos[2][RV_COMBS];
static float rv_ap[2][RV_APS][556 + RV_SPREAD];
static int   rv_ap_pos[2][RV_APS];
static float rv_out_r;
static float rv_side(int ch, float in)
{
    const float fb = 0.80f, damp = 0.25f;
    float out = 0.0f;
    for (int c = 0; c < RV_COMBS; c++) {
        int len = rv_comb_len[c] + ch * RV_SPREAD;
        float y = rv_comb[ch][c][rv_comb_pos[ch][c]];
        rv_comb_lp[ch][c] = y * (1.0f - damp) + rv_comb_lp[ch][c] * damp;
        rv_comb[ch][c][rv_comb_pos[ch][c]] = in + rv_comb_lp[ch][c] * fb;
        if (++rv_comb_pos[ch][c] >= len) rv_comb_pos[ch][c] = 0;
        out += y;
    }
    for (int a = 0; a < RV_APS; a++) {
        int len = rv_ap_len[a] + ch * RV_SPREAD;
        float b = rv_ap[ch][a][rv_ap_pos[ch][a]];
        rv_ap[ch][a][rv_ap_pos[ch][a]] = out + b * 0.5f;
        out = b - out;
        if (++rv_ap_pos[ch][a] >= len) rv_ap_pos[ch][a] = 0;
    }
    return out;
}
/* Left sample now; the right one for the same input is kept for
 * dsp_reverb_r(), called right after. */
static float dsp_reverb(float in)
{
    const float g = 0.35f / RV_COMBS;     /* about -9 dB overall */
    float x = in * 0.5f;
    float l = rv_side(0, x) * g;
    rv_out_r = rv_side(1, x) * g;
    return l;
}
static float dsp_reverb_r(void) { return rv_out_r; }

/* RECOMP_APU_REVERB: 0 (default) leaves the I3DL2 send out; 1..100 plays it
 * through the reverb above at that percentage of its -9 dB return. Its 30 ms
 * combs read as a second copy of every voice on the console, and the title's
 * room parameters are not read anyway. */
static float reverb_level(void)
{
    static float lv = -1.0f;
    if (lv < 0.0f) {
        const char *e = getenv("RECOMP_APU_REVERB");
        long v = e && *e ? atol(e) : 0;
        if (v == 1)
            v = 100;
        lv = (float)(v < 0 ? 0 : v > 100 ? 100 : v) / 100.0f;
    }
    return lv;
}

/* A peak limiter in place of the clamp: loud fights summed past full scale
 * (hundreds of clipped samples per 10 s) and a hard clip on that many is
 * what crackles. Gain drops at once to keep the peak at 0.95 and recovers
 * over ~60 ms. */
static float s_lim_gain = 1.0f;
unsigned g_limited;
static void limit(float *l, float *r)
{
    float pk = fabsf(*l) > fabsf(*r) ? fabsf(*l) : fabsf(*r);
    float want = pk > 0.95f ? 0.95f / pk : 1.0f;
    if (want < s_lim_gain) {
        s_lim_gain = want;
        g_limited++;
    } else {
        s_lim_gain += (want - s_lim_gain) * 0.00035f;
    }
    *l *= s_lim_gain;
    *r *= s_lim_gain;
}

/* Exact repeats in the output: a block of 256 sound samples identical to one
 * played within the last ~2.7 s is a stream buffer played a second time
 * (stale data) -- what "voices playing double" would be if it is not the mix.
 * Counted per lag for the 10 s report. */
#define RPT_BLOCK 256
#define RPT_HIST 512
static uint64_t s_rpt_hash[RPT_HIST];
static unsigned s_rpt_pos, s_rpt_fill, s_rpt_n;
static uint64_t s_rpt_acc;
static double s_rpt_energy;
unsigned g_rpt_blocks, g_rpt_repeats, g_rpt_lag[8];   /* lags in blocks, top 8 kinds */
unsigned g_rpt_lag_n[8];
/* Clicks in the output: a sample far off the straight line through the two
 * before it, against the recent roughness of the signal. Split by where it
 * falls in the 32-sample VP frame: a pile-up at the frame start is a
 * frame-level fault (see jump_check in apu_vp.c), not the music. */
unsigned g_click_start, g_click_other;
static float s_ck1, s_ck2, s_ck_rough = 100.0f;
static unsigned s_ck_phase;
static void click_feed(int16_t l, int16_t r)
{
    float x = 0.5f * ((float)l + (float)r);
    float e = fabsf(x - 2.0f * s_ck1 + s_ck2);
    if (e > 12.0f * s_ck_rough + 600.0f) {
        if (s_ck_phase <= 1)
            g_click_start++;
        else
            g_click_other++;
    }
    s_ck_rough += (e - s_ck_rough) * (1.0f / 64.0f);
    s_ck2 = s_ck1;
    s_ck1 = x;
    s_ck_phase = (s_ck_phase + 1) & 31;
}

static void repeat_feed(int16_t l, int16_t r)
{
    click_feed(l, r);
    uint32_t v = (uint16_t)l | (uint32_t)(uint16_t)r << 16;
    s_rpt_acc = (s_rpt_acc ^ v) * 0x100000001B3ull;
    s_rpt_energy += (double)l * l + (double)r * r;
    if (++s_rpt_n < RPT_BLOCK)
        return;
    if (s_rpt_energy > (double)RPT_BLOCK * 2.0 * 300.0 * 300.0) {    /* not near silence */
        unsigned k, back;
        g_rpt_blocks++;
        for (back = 1; back <= s_rpt_fill; back++) {
            k = (s_rpt_pos + RPT_HIST - back) % RPT_HIST;
            if (s_rpt_hash[k] == s_rpt_acc) {
                int j, slot = -1;
                g_rpt_repeats++;
                for (j = 0; j < 8; j++)
                    if (g_rpt_lag_n[j] && g_rpt_lag[j] == back) { slot = j; break; }
                if (slot < 0)
                    for (j = 0; j < 8; j++)
                        if (!g_rpt_lag_n[j]) { slot = j; g_rpt_lag[j] = back; break; }
                if (slot >= 0)
                    g_rpt_lag_n[slot]++;
                break;
            }
        }
    }
    s_rpt_hash[s_rpt_pos] = s_rpt_acc;
    s_rpt_pos = (s_rpt_pos + 1) % RPT_HIST;
    if (s_rpt_fill < RPT_HIST - 1)
        s_rpt_fill++;
    s_rpt_acc = 0xCBF29CE484222325ull;
    s_rpt_energy = 0.0;
    s_rpt_n = 0;
}

/* GPRST (GP region + 0xFFFC): the GP runs only with both reset bits set, as
 * in xemu's gp_write. Nothing else models the GP, so this is kept for the
 * =auto ack alone. Before the first write the GP counts as running: a title
 * that never touches GPRST keeps its ack. */
void mcpx_apu_dsp_gprst_write(uint32_t val)
{
    const uint32_t run = NV_PAPU_GPRST_GPRST | NV_PAPU_GPRST_GPDSPRST;
    int held = (val & run) != run;

    if (held != s_gp_in_reset && s_dsp_ack_auto)
        fprintf(stderr, "[APU] GPRST 0x%X: GP %s; DSP doorbell ack %s\n",
                val, held ? "held in reset" : "running",
                held ? "paused" : "resumed");
    s_gp_in_reset = held;
}

/* The doorbell for =auto: SGE[0] of the GP scratch table + 0x810, re-read on
 * every poll, so the slot always lies in the page the table names now. NULL
 * while GPSADDR is unset, the table's first entry names no page, or the GP
 * is held in reset: a title that tears DirectSound down frees the scratch,
 * and a dword cleared there after that would land in whatever reuses the
 * page. */
static uint32_t *dsp_ack_auto_slot(MCPXAPUState *d)
{
    uint32_t sge = qatomic_read(&d->regs[NV_PAPU_GPSADDR]), page;

    if (!sge || s_gp_in_reset)
        return NULL;
    page = *(const uint32_t *)mcpx_apu_phys(sge);
    if (!page || (page & 0xFFF))
        return NULL;                       /* table not filled in yet */
    if (page != s_dsp_ack_auto_page) {
        s_dsp_ack_auto_page = page;
        fprintf(stderr, "[APU] DSP doorbell: GP scratch SGE table 0x%08X maps"
                        " page 0x%08X (%s); doorbell at physical 0x%08X\n",
                sge, page, mcpx_apu_phys_class(page),
                page + GP_DOORBELL_OFFSET);
    }
    return (uint32_t *)(mcpx_apu_phys(page) + GP_DOORBELL_OFFSET);
}

void mcpx_apu_dsp_ack_poll(MCPXAPUState *d)
{
    int i;

    if (s_dsp_ack_count < 0)
        dsp_ack_init();
    if (!d->ram_ptr)
        return;
    if (s_dsp_ack_auto) {
        uint32_t *slot = dsp_ack_auto_slot(d);
        if (slot && *slot) {
            static int shown;
            if (shown++ < 3)
                fprintf(stderr, "[APU] DSP doorbell (auto): command 0x%08X"
                                " acknowledged\n", *slot);
            *slot = 0;
        }
    }
    for (i = 0; i < s_dsp_ack_count; i++) {
        uint32_t *slot = (uint32_t *)(d->ram_ptr + s_dsp_ack[i]);
        if (*slot) {
            static int shown[APU_DSP_ACK_MAX];
            if (shown[i]++ < 3)
                fprintf(stderr, "[APU] DSP doorbell 0x%08X: command 0x%08X"
                                " acknowledged\n", s_dsp_ack[i], *slot);
            *slot = 0;
        }
    }
}

void mcpx_apu_dsp_init(MCPXAPUState *d)
{
    /* The doorbell ack's state is file-static, so it outlives an instance:
     * a GP the last instance held in reset would leave this one's ack
     * paused, and the RECOMP_APU_DSP_ACK setting would never be read again.
     * Start from what a fresh process has. */
    s_dsp_ack_count = -1;                  /* re-read on the first poll */
    s_dsp_ack_auto = 0;
    s_dsp_ack_auto_page = 0;
    s_gp_in_reset = 0;

    /* Allocate minimal DSP state for GP and EP.
     * We need these to exist so reset doesn't crash,
     * but they won't actually run DSP programs. */
    d->gp.dsp = (DSPState *)calloc(1, sizeof(DSPState));
    d->ep.dsp = (DSPState *)calloc(1, sizeof(DSPState));

    if (d->gp.dsp) d->gp.dsp->is_gp = true;
    if (d->ep.dsp) d->ep.dsp->is_gp = false;

    d->gp.realtime = false;
    d->ep.realtime = false;

    fprintf(stderr, "[APU] DSP GP/EP initialized (STUBBED - passthrough mode)\n");
}

/* Called after the frame thread is joined. Only mcpx_apu_reset_locked
 * (currently uncalled) reads this state, and it would run on that thread.
 * Safe to call twice. */
void mcpx_apu_dsp_finalize(MCPXAPUState *d)
{
    free(d->gp.dsp);
    free(d->ep.dsp);
    d->gp.dsp = NULL;
    d->ep.dsp = NULL;
}

void mcpx_apu_update_dsp_preference(MCPXAPUState *d)
{
    /* In the real xemu, this reads settings to decide whether
     * GP/EP should run in realtime or cached mode. We ignore it. */
    (void)d;
}

void mcpx_apu_dsp_frame(MCPXAPUState *d,
                         float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME])
{
    /* Bypass DSP: take mixbin 0 (front-left) and mixbin 1 (front-right)
     * and write them directly to the monitor frame buffer as the final
     * EP output.
     *
     * The Xbox DirectSound typically routes:
     *   Mixbin 0 = Front Left
     *   Mixbin 1 = Front Right
     *   Mixbin 2 = Center (often unused in stereo)
     *   Mixbin 3 = LFE
     *   Mixbin 4-5 = Rear L/R
     *
     * For stereo output, bins 0 and 1 are what we want.
     */

    int off = (d->ep_frame_div % 8) * NUM_SAMPLES_PER_FRAME;

    if (d->monitor.point != MCPX_APU_DEBUG_MON_VP) {
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            /* Bins 2..31 used to be computed and then dropped on the floor.
             * On hardware the GP and EP mix the submixes down; here they are
             * stubs, so thirty of thirty-two bins were discarded every frame
             * with no counter anywhere to say so.
             *
             * Measured on Jet Set Radio Future, one 200 s gameplay run, with
             * a positive control moving beside it:
             *
             *     [APU-BIN] 2D heard=557466 lost=0
             *               3D heard=0      lost=377768
             *               lost by bin: 6,7,8,9,10
             *
             * 557,466 music voice-frames heard and none lost; 377,768 effect
             * voice-frames produced correctly and thrown away. The title's 3D
             * positional voices -- its sound effects -- are routed to bins 6
             * to 10 by the guest's own V0BIN..V3BIN, and music on 2D voices
             * lands in bins 0 and 1, which is why the music was always
             * audible and no effect ever was. Every instrument upstream of
             * this line read healthy.
             *
             * Gating the HRTF submix override was tried first and did not fix
             * it, so the defect is the width of this mixdown and nothing else.
             *
             * Even bins left, odd bins right, which preserves the stereo
             * pairing the guest set up -- bins 6/7 and 8/9 arrive with matched
             * counts. This is not what a real EP does; it is the cheapest
             * mixdown that stops discarding audio. */
            float left, right;
            for (int b = 0; b < NUM_MIXBINS; ++b) {
                float x = mixbins[b][i];
                g_bin_energy[b] += (double)x * x;
                if (x > 1.0f || x < -1.0f) g_bin_clip[b]++;
            }
            int mode = mcpx_apu_mixdown_mode();
            if (mode == MIXDOWN_SPEAKERS) {
                /* What the EP's stereo output would carry, by the XDK's own
                 * DSMIXBIN assignments: 0/1 front L/R, 2 centre, 3 LFE, 4/5
                 * rear L/R, 6..9 the 3D (crosstalk) front L/R and rear L/R,
                 * 10 the I3DL2 reverb SEND, 11..30 effect sends. The sends
                 * are inputs to the GP's effects and never reach a speaker
                 * raw. Summing every bin (the mode below) played each 3D
                 * sound twice -- dry on 6/7 and again, unprocessed, from the
                 * reverb send on 10, and only in the left ear because 10 is
                 * even; centre (2) was left-only too. Darkwatch in play: bins
                 * 6/7 ~25% of the energy each, bin 10 ~35%. The reverb send
                 * goes through a small reverb instead, at return level. */
                const float k = 0.7071f;
                float c = mixbins[2][i] * k;
                float rl = reverb_level(), wet = 0.0f, wet_r = 0.0f;
                if (rl > 0.0f) {
                    wet = dsp_reverb(mixbins[10][i]) * rl;
                    wet_r = dsp_reverb_r() * rl;
                }
                left  = mixbins[0][i] + c + k * mixbins[4][i]
                      + mixbins[6][i] + k * mixbins[8][i] + wet;
                right = mixbins[1][i] + c + k * mixbins[5][i]
                      + mixbins[7][i] + k * mixbins[9][i] + wet_r;
            } else if (mode == MIXDOWN_ALL) {
                left = 0.0f;
                right = 0.0f;
                for (int b = 0; b < NUM_MIXBINS; ++b) {
                    if (b & 1) right += mixbins[b][i];
                    else       left  += mixbins[b][i];
                }
            } else {
                left = mixbins[0][i];
                right = mixbins[1][i];
            }
            if (left > 1.0f || left < -1.0f || right > 1.0f || right < -1.0f)
                g_out_clip++;
            limit(&left, &right);
            /* Clamp to [-1, 1] range */
            if (left > 1.0f) left = 1.0f;
            if (left < -1.0f) left = -1.0f;
            if (right > 1.0f) right = 1.0f;
            if (right < -1.0f) right = -1.0f;

            /* Convert to 16-bit and write (not accumulate) into frame buffer.
             * Each of the 8 sub-frames writes its own 32-sample slice. */
            d->monitor.frame_buf[off + i][0] = (int16_t)(left * 32767.0f);
            d->monitor.frame_buf[off + i][1] = (int16_t)(right * 32767.0f);
            repeat_feed(d->monitor.frame_buf[off + i][0], d->monitor.frame_buf[off + i][1]);
        }
    }

    g_dbg.gp.cycles = 0;
    g_dbg.ep.cycles = 0;
}
