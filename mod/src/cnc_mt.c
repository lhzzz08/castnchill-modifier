#include "cnc_mt.h"
#include "cnc_log.h"

#include <dispatch/dispatch.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>

/* 主线程调度器 v3：GCD 主队列。
 * Unity 的 PlayerLoop/脚本运行在 App 主线程（NSApplicationMain run loop），
 * 而 GCD main queue 派发的闭包同样在该线程执行 —— 因此用原生
 * dispatch_async_f 即可把托管调用编组到主线程，零委托、零 hook。
 * （v1 的 SynchronizationContext.Post + 原生指针委托实测崩溃：
 *  IL2CPP 的 Delegate.ctor(object, IntPtr) 不接受原生函数指针。）
 *
 * v3：job 一律堆分配 + 引用计数（等待方 1 + 主线程 1）。
 * v2 的 job 在等待方栈上，超时返回后主线程才执行 = 写已释放的栈（加载期卡顿可触发）。 */

typedef struct job {
    void (*fn)(void *);
    void *arg;
    atomic_int refs;
    pthread_mutex_t mu;
    pthread_cond_t done;
    bool finished;
} cnc_job_t;

static atomic_bool g_ready = false;

static cnc_job_t *job_new(void (*fn)(void *), void *arg, int refs) {
    cnc_job_t *job = calloc(1, sizeof *job);
    if (!job) return NULL;
    job->fn = fn;
    job->arg = arg;
    atomic_init(&job->refs, refs);
    pthread_mutex_init(&job->mu, NULL);
    pthread_cond_init(&job->done, NULL);
    return job;
}

static void job_release(cnc_job_t *job) {
    if (atomic_fetch_sub(&job->refs, 1) != 1) return;
    pthread_mutex_destroy(&job->mu);
    pthread_cond_destroy(&job->done);
    free(job);
}

/* ---- 在 App 主线程（= Unity 主线程）上执行 ---- */

static void run_on_main_f(void *p) {
    cnc_job_t *job = p;
    job->fn(job->arg);
    pthread_mutex_lock(&job->mu);
    job->finished = true;
    pthread_cond_broadcast(&job->done);
    pthread_mutex_unlock(&job->mu);
    job_release(job);
}

/* ---- 以下在任意线程 ---- */

/* 探针标志用静态变量：超时后迟到执行的探针也不会写到失效内存 */
static atomic_int g_probe_hit;
static void probe_set(void *unused) {
    (void)unused;
    atomic_store(&g_probe_hit, 1);
}

bool cnc_mt_init(void) {
    atomic_store(&g_probe_hit, 0);
    if (cnc_mt_run(probe_set, NULL, 5000) != CNC_MT_OK || !atomic_load(&g_probe_hit)) {
        cnc_log("mt: 主线程探针失败（下次命令重试）");
        return false;
    }
    atomic_store(&g_ready, true);
    cnc_log("mt: 主线程调度器就绪（GCD 主队列）");
    return true;
}

static pthread_mutex_t g_init_mu = PTHREAD_MUTEX_INITIALIZER;

bool cnc_mt_ensure(void) {
    if (atomic_load(&g_ready)) return true;
    pthread_mutex_lock(&g_init_mu);
    if (!atomic_load(&g_ready)) cnc_mt_init();
    pthread_mutex_unlock(&g_init_mu);
    return atomic_load(&g_ready);
}

cnc_mt_status_t cnc_mt_run(void (*fn)(void *), void *arg, int timeout_ms) {
    cnc_job_t *job = job_new(fn, arg, 2);
    if (!job) return CNC_MT_FAILED;
    dispatch_async_f(dispatch_get_main_queue(), job, run_on_main_f);

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }

    pthread_mutex_lock(&job->mu);
    while (!job->finished) {
        if (pthread_cond_timedwait(&job->done, &job->mu, &ts) == ETIMEDOUT) break;
    }
    bool ok = job->finished;
    pthread_mutex_unlock(&job->mu);
    job_release(job);
    return ok ? CNC_MT_OK : CNC_MT_TIMEOUT;
}

bool cnc_mt_post(void (*fn)(void *), void *arg) {
    cnc_job_t *job = job_new(fn, arg, 1);
    if (!job) return false;
    dispatch_async_f(dispatch_get_main_queue(), job, run_on_main_f);
    return true;
}
