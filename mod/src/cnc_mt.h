#ifndef CNC_MT_H
#define CNC_MT_H

#include <stdbool.h>
#include <stdint.h>

/* 主线程调度器：所有触碰托管堆的调用必须编组到 Unity 主线程执行
 * （IL2CPP 托管对象操作非线程安全，后台线程直调会竞态崩溃）。
 *
 * 实现：GCD 主队列 dispatch_async_f（见 cnc_mt.c）。 */

typedef enum {
    CNC_MT_OK = 0,   /* fn 已在主线程执行完 */
    CNC_MT_TIMEOUT,  /* 等待超时：fn 之后仍可能执行，arg 必须保持有效 */
    CNC_MT_FAILED,   /* 未能派发：fn 永远不会执行 */
} cnc_mt_status_t;

/* 探测主线程是否在泵送主队列；失败可再调（下次命令重试） */
bool cnc_mt_init(void);

/* 惰性初始化（线程安全；未就绪时每次调用都会重试探针） */
bool cnc_mt_ensure(void);

/* 在主线程执行 fn(arg)，等待完成（timeout_ms 毫秒超时）。 */
cnc_mt_status_t cnc_mt_run(void (*fn)(void *), void *arg, int timeout_ms);

/* 在主线程执行 fn(arg)，不等待（fire-and-forget）。 */
bool cnc_mt_post(void (*fn)(void *), void *arg);

#endif
