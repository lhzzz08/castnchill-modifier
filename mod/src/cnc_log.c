#include "cnc_log.h"

#include <pthread.h>
#include <stdarg.h>
#include <time.h>

static FILE *g_log;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

void cnc_log_open(const char *path) {
    FILE *f = fopen(path, "a");
    if (f) {
        setvbuf(f, NULL, _IOLBF, 0);
        g_log = f;
    }
}

void cnc_log_close(void) {
    if (g_log) fclose(g_log);
}

void cnc_log(const char *fmt, ...) {
    FILE *f = g_log;
    if (!f) return;
    char stamp[32];
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(stamp, sizeof stamp, "%m-%d %H:%M:%S", &tmv);

    pthread_mutex_lock(&g_mu);
    fprintf(f, "[%s] ", stamp);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    pthread_mutex_unlock(&g_mu);
}
