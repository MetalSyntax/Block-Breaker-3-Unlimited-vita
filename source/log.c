/**
 * @file log.c
 * @brief Thread-safe logging subsystem implementation.
 */
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/rtc.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"

static SceUID log_fd = -1;

// sceIoWrite on ux0 costs ~10 ms even unsynced and the game printf()s every frame: INFO lines go to a
// RAM buffer drained by a low-priority thread; WARN+ drain it synchronously (crash keeps them).
static char log_buf[64 * 1024];
static int log_len;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER, io_lock = PTHREAD_MUTEX_INITIALIZER; // order: log -> io

static void log_drain(int sync) { // caller holds log_lock
    pthread_mutex_lock(&io_lock); // after the thread's pending (older) chunk: keeps line order
    if (log_fd >= 0 && log_len) {
        sceIoWrite(log_fd, log_buf, log_len);
        if (sync)
            sceIoSyncByFd(log_fd, 0);
    }
    log_len = 0;
    pthread_mutex_unlock(&io_lock);
}

static int log_thread(SceSize args, void *argp) {
    static char out[sizeof(log_buf)];
    for (;;) {
        sceKernelDelayThread(500 * 1000);
        pthread_mutex_lock(&log_lock);
        int n = log_len; // copy out so the game's printf never waits on the SD write
        memcpy(out, log_buf, n);
        log_len = 0;
        pthread_mutex_lock(&io_lock);
        pthread_mutex_unlock(&log_lock);
        if (n && log_fd >= 0)
            sceIoWrite(log_fd, out, n);
        pthread_mutex_unlock(&io_lock);
    }
    return 0;
}

void log_open(const char *path) {
    if (log_fd >= 0)
        sceIoClose(log_fd);
    log_fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (log_fd < 0)
        printf("[-] Failed to open log file at %s\n", path);
    static SceUID th = -1;
    if (th < 0 && (th = sceKernelCreateThread("log", log_thread, 0x10000100 + 60, 0x4000, 0, 0, NULL)) >= 0)
        sceKernelStartThread(th, 0, NULL);
}

static void log_v(const char *level, const char *fmt, va_list ap) {
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, ap);

    SceDateTime t;
    sceRtcGetCurrentClockLocalTime(&t);
    char time_str[32];
    snprintf(time_str, sizeof(time_str), "%02d:%02d:%02d.%03d",
             t.hour, t.minute, t.second, t.microsecond / 1000);

    char line[1100];
    int n = snprintf(line, sizeof(line), "[%s] [%s] %s\n", time_str, level, buf);
    if (n > (int)sizeof(line) - 1)
        n = sizeof(line) - 1;
    pthread_mutex_lock(&log_lock);
    if (log_len + n > (int)sizeof(log_buf))
        log_drain(0); // only if the thread fell behind
    memcpy(log_buf + log_len, line, n);
    log_len += n;
    if (level[0] != 'I')
        log_drain(1);
    pthread_mutex_unlock(&log_lock);
}

void l_debug(const char *fmt, ...) {
#ifdef DEBUG_LOADER
    va_list ap;
    va_start(ap, fmt);
    log_v("DEBUG", fmt, ap);
    va_end(ap);
#else
    (void)fmt;
#endif
}

void l_info(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_v("INFO", fmt, ap);
    va_end(ap);
}

void l_warn(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_v("WARN", fmt, ap);
    va_end(ap);
}

void l_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_v("ERROR", fmt, ap);
    va_end(ap);
}

void fatal_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_v("FATAL", fmt, ap);
    va_end(ap);
    if (log_fd >= 0) {
        sceIoClose(log_fd);
        log_fd = -1;
    }
    sceKernelExitProcess(1);
    while (1) {}
}
