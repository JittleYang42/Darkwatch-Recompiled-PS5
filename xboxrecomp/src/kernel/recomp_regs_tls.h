/*
 * recomp_regs_tls.h - the guest registers as one thread-local block.
 *
 * Force-included (-include) into every unit of a build that defines
 * RECOMP_REGS_STRUCT: the PS5, whose titles have emulated TLS only, so each
 * access to a __thread variable is a call to __emutls_get_address. The guest
 * registers are touched by nearly every lifted statement, so there they are
 * one TLS block whose address a generated function takes once, at its entry
 * (RECOMP_FN_ENTRY, inserted after each function's opening brace when the
 * PS5 build copies the generated sources), and every g_* name below is a
 * member of it. Outside generated code the names go through the block's
 * address directly, one call per access, as before.
 */
#ifndef RECOMP_REGS_TLS_H
#define RECOMP_REGS_TLS_H
#if defined(RECOMP_REGS_STRUCT)

/* No #include here: this header is force-included ahead of each unit's own
 * _GNU_SOURCE, which a system header seen first would lock out. */

typedef struct recomp_regs {
    unsigned int r_eax, r_ecx, r_edx, r_esp, r_ebx, r_esi, r_edi, r_ebp;
    unsigned int r_fs_base, r_seh_ebp, r_mxcsr;
    int r_df, r_fp_top, r_fp_cmp;
    unsigned short r_fp_control_word, r_fp_cc;
    double r_fp_stack[8];
    /* RecompXmm (16 bytes) and RecompMmx (8 bytes), as recomp_types.h
     * defines them; stored here as bytes so this header needs neither. */
    _Alignas(16) unsigned char r_xmm[8][16];
    _Alignas(8) unsigned char r_mm[8][8];
} recomp_regs_t;

extern __thread recomp_regs_t g_recomp_regs;

#if defined(RECOMP_FN_LOCAL_REGS)
/* Generated code: each function's own pointer to the block. */
#  define RECOMP_RR _rr
#  define RECOMP_FN_ENTRY recomp_regs_t *const _rr = &g_recomp_regs;
#else
#  define RECOMP_RR (&g_recomp_regs)
#  define RECOMP_FN_ENTRY
#endif

#define g_eax             (RECOMP_RR->r_eax)
#define g_ecx             (RECOMP_RR->r_ecx)
#define g_edx             (RECOMP_RR->r_edx)
#define g_esp             (RECOMP_RR->r_esp)
#define g_ebx             (RECOMP_RR->r_ebx)
#define g_esi             (RECOMP_RR->r_esi)
#define g_edi             (RECOMP_RR->r_edi)
#define g_ebp             (RECOMP_RR->r_ebp)
#define g_fs_base         (RECOMP_RR->r_fs_base)
#define g_seh_ebp         (RECOMP_RR->r_seh_ebp)
#define g_mxcsr           (RECOMP_RR->r_mxcsr)
#define g_df              (RECOMP_RR->r_df)
#define g_fp_top          (RECOMP_RR->r_fp_top)
#define g_fp_cmp          (RECOMP_RR->r_fp_cmp)
#define g_fp_control_word (RECOMP_RR->r_fp_control_word)
#define g_fp_cc           (RECOMP_RR->r_fp_cc)
#define g_fp_stack        (RECOMP_RR->r_fp_stack)
#define g_xmm0 (*(RecompXmm *)(void *)RECOMP_RR->r_xmm[0])
#define g_xmm1 (*(RecompXmm *)(void *)RECOMP_RR->r_xmm[1])
#define g_xmm2 (*(RecompXmm *)(void *)RECOMP_RR->r_xmm[2])
#define g_xmm3 (*(RecompXmm *)(void *)RECOMP_RR->r_xmm[3])
#define g_xmm4 (*(RecompXmm *)(void *)RECOMP_RR->r_xmm[4])
#define g_xmm5 (*(RecompXmm *)(void *)RECOMP_RR->r_xmm[5])
#define g_xmm6 (*(RecompXmm *)(void *)RECOMP_RR->r_xmm[6])
#define g_xmm7 (*(RecompXmm *)(void *)RECOMP_RR->r_xmm[7])
#define g_mm0 (*(RecompMmx *)(void *)RECOMP_RR->r_mm[0])
#define g_mm1 (*(RecompMmx *)(void *)RECOMP_RR->r_mm[1])
#define g_mm2 (*(RecompMmx *)(void *)RECOMP_RR->r_mm[2])
#define g_mm3 (*(RecompMmx *)(void *)RECOMP_RR->r_mm[3])
#define g_mm4 (*(RecompMmx *)(void *)RECOMP_RR->r_mm[4])
#define g_mm5 (*(RecompMmx *)(void *)RECOMP_RR->r_mm[5])
#define g_mm6 (*(RecompMmx *)(void *)RECOMP_RR->r_mm[6])
#define g_mm7 (*(RecompMmx *)(void *)RECOMP_RR->r_mm[7])

#else
#  define RECOMP_FN_ENTRY
#endif /* RECOMP_REGS_STRUCT */
#endif /* RECOMP_REGS_TLS_H */
