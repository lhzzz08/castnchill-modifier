#include "cnc_log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

/* 日志跨会话追加（崩溃取证），超过上限轮转为 .1，最多占 2×上限 */
#define CNC_LOG_MAX_BYTES (4L * 1024 * 1024)

static FILE *g_log;
static char g_path[256];
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

void cnc_log_open(const char *path) {
    snprintf(g_path, sizeof g_path, "%s", path);
    FILE *f = fopen(path, "a");
    if (f) {
        setvbuf(f, NULL, _IOLBF, 0);
        g_log = f;
    }
}

void cnc_log_close(void) {
    pthread_mutex_lock(&g_mu);
    if (g_log) fclose(g_log);
    g_log = NULL;
    pthread_mutex_unlock(&g_mu);
}

/* 持锁调用 */
static void rotate_if_needed(void) {
    if (ftell(g_log) < CNC_LOG_MAX_BYTES) return;
    char old[sizeof g_path + 4];
    snprintf(old, sizeof old, "%s.1", g_path);
    fclose(g_log);
    rename(g_path, old);
    g_log = fopen(g_path, "a");
    if (g_log) setvbuf(g_log, NULL, _IOLBF, 0);
}

void cnc_log(const char *fmt, ...) {
    char stamp[32];
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(stamp, sizeof stamp, "%m-%d %H:%M:%S", &tmv);

    pthread_mutex_lock(&g_mu);
    if (g_log) {
        fprintf(g_log, "[%s] ", stamp);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fputc('\n', g_log);
        rotate_if_needed();
    }
    pthread_mutex_unlock(&g_mu);
}
