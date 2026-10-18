/*
 * glibc_compat.c - let a binary built against glibc 2.39 run on glibc 2.35.
 *
 * Built on Ubuntu 24.04, run on 22.04 (the Cowork/WSL VM). glibc 2.38 added
 * C23 strtol variants, the BSD strlcpy family and new fmod/fmodf versions;
 * headers route calls to them, so the binary would refuse to load on 2.35.
 * The executable defines the new names itself (the linker prefers these over
 * libc.so), and fmod/fmodf are wrapped (-Wl,--wrap) onto their old versions.
 * Only linked into the DW_PORTABLE build.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

long __isoc23_strtol(const char *s, char **e, int b) { return strtol(s, e, b); }
unsigned long __isoc23_strtoul(const char *s, char **e, int b) { return strtoul(s, e, b); }
long long __isoc23_strtoll(const char *s, char **e, int b) { return strtoll(s, e, b); }
unsigned long long __isoc23_strtoull(const char *s, char **e, int b) { return strtoull(s, e, b); }

int __isoc23_vsscanf(const char *s, const char *fmt, va_list ap) { return vsscanf(s, fmt, ap); }
int __isoc23_sscanf(const char *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsscanf(s, fmt, ap);
    va_end(ap);
    return r;
}

size_t strlcpy(char *d, const char *s, size_t n)
{
    size_t len = strlen(s);
    if (n) {
        size_t c = len < n - 1 ? len : n - 1;
        memcpy(d, s, c);
        d[c] = 0;
    }
    return len;
}

size_t strlcat(char *d, const char *s, size_t n)
{
    size_t dl = strnlen(d, n);
    if (dl == n)
        return n + strlen(s);
    return dl + strlcpy(d + dl, s, n - dl);
}

size_t wcslcpy(wchar_t *d, const wchar_t *s, size_t n)
{
    size_t len = wcslen(s);
    if (n) {
        size_t c = len < n - 1 ? len : n - 1;
        wmemcpy(d, s, c);
        d[c] = 0;
    }
    return len;
}

size_t wcslcat(wchar_t *d, const wchar_t *s, size_t n)
{
    size_t dl = wcsnlen(d, n);
    if (dl == n)
        return n + wcslen(s);
    return dl + wcslcpy(d + dl, s, n - dl);
}

/* fmod@GLIBC_2.38 is the same function with a new error-handling version. */
double old_fmod(double, double);
float old_fmodf(float, float);
__asm__(".symver old_fmod,fmod@GLIBC_2.2.5");
__asm__(".symver old_fmodf,fmodf@GLIBC_2.2.5");
double __wrap_fmod(double a, double b) { return old_fmod(a, b); }
float __wrap_fmodf(float a, float b) { return old_fmodf(a, b); }
