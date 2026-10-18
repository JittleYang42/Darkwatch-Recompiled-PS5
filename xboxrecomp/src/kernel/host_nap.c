/*
 * host_nap.c -- a short sleep that really is short, on every host.
 *
 * The runtime's helper threads (the NV2A poll, the busy-bit ack) nap ~100 us
 * between turns when there is nothing to do. POSIX gets that from nanosleep.
 * Windows has no sub-millisecond Sleep: mingw's usleep(100) is Sleep(0), a
 * bare yield that keeps the core spinning, and Sleep(1) is a whole timer
 * period (15.6 ms unless someone raised the resolution). A high-resolution
 * waitable timer (Windows 10 1803+) sleeps for about the time asked; where
 * it is missing, Sleep(0) keeps the old behaviour.
 */
#include <stdint.h>
#include "xbox_memory_layout.h"

#if defined(_WIN32)
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

void xbox_HostNapUs(unsigned us)
{
    static __thread HANDLE timer;
    static __thread int tried;
    LARGE_INTEGER due;

    if (!us) {
        SwitchToThread();
        return;
    }
    if (!tried) {
        tried = 1;
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                       TIMER_ALL_ACCESS);
    }
    if (!timer) {
        if (us >= 1000)
            Sleep(us / 1000);
        else
            Sleep(0);
        return;
    }
    due.QuadPart = -(LONGLONG)us * 10;          /* relative, 100 ns units */
    if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
        WaitForSingleObject(timer, INFINITE);
    else
        Sleep(0);
}
#else
#include <time.h>

void xbox_HostNapUs(unsigned us)
{
    struct timespec ts;
    ts.tv_sec = us / 1000000u;
    ts.tv_nsec = (long)(us % 1000000u) * 1000L;
    nanosleep(&ts, NULL);
}
#endif
