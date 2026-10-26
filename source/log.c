/**
 * @file log.c
 * @brief Thread-safe logging subsystem implementation.
 */
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/rtc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"

static SceUID log_fd = -1;

void log_open(const char *path) {
    if (log_fd >= 0)
        sceIoClose(log_fd);
    log_fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (log_fd < 0)
        printf("[-] Failed to open log file at %s\n", path);
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
    printf("%s", line);
    if (log_fd >= 0) {
        sceIoWrite(log_fd, line, n);
        // Una caída dura (apagado) pierde la caché del FS: sincronizar cada línea (log_003 quedó truncado).
        // ponytail: sync por línea es lento; si un log por-frame tira los FPS, sincronizar solo WARN+.
        sceIoSyncByFd(log_fd, 0);
    }
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
