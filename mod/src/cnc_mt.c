#include "cnc_mt.h"
#include "cnc_log.h"
#include "cnc_resolver.h"

#include <dispatch/dispatch.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

/* 主线程调度器 v2：GCD 主队列。
 * Unity 的 PlayerLoop/脚本运行在 App 主线程（NSApplicationMain run loop），
 * 而 GCD main queue 派发的闭包同样在该线程执行 —— 因此用原生
 * dispatch_async_f 即可把托管调用编组到主线程，零委托、零 hook。
 * （v1 的 SynchronizationContext.Post + 原生指针委托实测崩溃：
 *  IL2CPP 的 Delegate.ctor(object, IntPtr) 不接受原生函数指针。） */

typedef struct job {
    void (*fn)(void *);
    void *arg;
    bool sync;
    pthread_mutex_t mu;
    pthread_cond_t done;
    bool finished;
} cnc_job_t;

static pthread_mutex_t g_qmu = PTHREAD_MUTEX_INITIALIZER;
static cnc_job_t *g_queue[64];
static int g_qcount = 0;
static bool g_ready = false;

/* ---- 以下函数在 App 主线程（= Unity 主线程）上执行 ---- */

static void wake_on_main_f(void *unused) {
    (void)unused;
    for (;;) {
        cnc_job_t *job = NULL;
        pthread_mutex_lock(&g_qmu);
        if (g_qcount > 0) {
            job = g_queue[0];
            for (int i = 1; i < g_qcount; i++) g_queue[i - 1] = g_queue[i];
            g_qcount--;
        }
        pthread_mutex_unlock(&g_qmu);
        if (!job) break;
        job->fn(job->arg);
        if (job->sync) {
            pthread_mutex_lock(&job->mu);
            job->finished = true;
            pthread_cond_broadcast(&job->done);
            pthread_mutex_unlock(&job->mu);
        } else {
            free(job);
        }
    }
}

/* ---- 以下在任意线程 ---- */

bool cnc_mt_init(void) {
    /* 探针：验证主队列派发与执行 */
    g_ready = true;
    volatile int hit = 0;
    extern void cnc_mt_probe_set(void *);
    if (!cnc_mt_run(cnc_mt_probe_set, (void *)&hit, 5000) || !hit) {
        cnc_log("mt: 主线程探针失败");
        g_ready = false;
        return false;
    }
    cnc_log("mt: 主线程调度器就绪（GCD 主队列）");
    return true;
}

void cnc_mt_probe_set(void *p) { *(volatile int *)p = 1; }

static pthread_mutex_t g_init_mu = PTHREAD_MUTEX_INITIALIZER;
static bool g_init_tried = false;

bool cnc_mt_ensure(void) {
    pthread_mutex_lock(&g_init_mu);
    if (!g_ready && !g_init_tried) {
        g_init_tried = true;
        cnc_mt_init();
    }
    pthread_mutex_unlock(&g_init_mu);
    return g_ready;
}

static bool enqueue(cnc_job_t *job) {
    bool ok = false;
    pthread_mutex_lock(&g_qmu);
    if (g_qcount < 64) {
        g_queue[g_qcount++] = job;
        ok = true;
    }
    pthread_mutex_unlock(&g_qmu);
    if (!ok) return false;
    dispatch_async_f(dispatch_get_main_queue(), NULL, wake_on_main_f);
    return true;
}

bool cnc_mt_run(void (*fn)(void *), void *arg, int timeout_ms) {
    if (!g_ready) { /* 兜底：内联执行（初始化期） */
        fn(arg);
        return true;
    }
    cnc_job_t job = {.fn = fn, .arg = arg, .sync = true, .finished = false};
    pthread_mutex_init(&job.mu, NULL);
    pthread_cond_init(&job.done, NULL);
    if (!enqueue(&job)) {
        pthread_mutex_destroy(&job.mu);
        pthread_cond_destroy(&job.done);
        return false;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }

    pthread_mutex_lock(&job.mu);
    while (!job.finished) {
        if (pthread_cond_timedwait(&job.done, &job.mu, &ts) == ETIMEDOUT) break;
    }
    bool ok = job.finished;
    pthread_mutex_unlock(&job.mu);
    pthread_mutex_destroy(&job.mu);
    pthread_cond_destroy(&job.done);
    return ok;
}

bool cnc_mt_post(void (*fn)(void *), void *arg) {
    if (!g_ready) { fn(arg); return true; }
    cnc_job_t *job = malloc(sizeof *job);
    if (!job) return false;
    job->fn = fn; job->arg = arg; job->sync = false;
    if (!enqueue(job)) { free(job); return false; }
    return true;
}
