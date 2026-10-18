/*
 * cpu_check.c -- built for plain x86-64 (CMakeLists.txt) and run before
 * anything else: the rest of the program uses AVX2, and a CPU without it
 * would fault on the first such instruction with no explanation.
 */
#include <windows.h>
#include <cpuid.h>

int dw_cpu_ok(void)
{
    unsigned a, b, c, d;
    int avx2 = 0, fma = 0, osxsave = 0;
    if (__get_cpuid(1, &a, &b, &c, &d)) {
        fma = (c >> 12) & 1;
        osxsave = (c >> 27) & 1;
    }
    if (__get_cpuid_count(7, 0, &a, &b, &c, &d))
        avx2 = (b >> 5) & 1;
    if (osxsave) {
        unsigned lo, hi;
        __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        if ((lo & 6) != 6)
            avx2 = 0;                   /* the OS does not save the YMM state */
    }
    if (avx2 && fma)
        return 1;
    MessageBoxA(NULL, "This build of Darkwatch needs a processor with AVX2 "
                "(Intel Haswell / AMD Excavator or newer).", "Darkwatch", MB_ICONERROR);
    return 0;
}

/* Before main: main itself is compiled for AVX2 and may use it in its
 * prologue (zeroing locals). */
__attribute__((constructor)) static void dw_cpu_check_early(void)
{
    if (!dw_cpu_ok())
        ExitProcess(1);
}
