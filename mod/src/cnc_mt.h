#ifndef CNC_MT_H
#define CNC_MT_H

#include <stdbool.h>
#include <stdint.h>

/* 主线程调度器：所有触碰托管堆的调用必须编组到 Unity 主线程执行
 * （IL2CPP 托管对象操作非线程安全，后台线程直调会竞态崩溃）。
 *
 * 实现：SynchronizationContext.Post(SendOrPostCallback, state)
 * —— Unity 主线程每帧泵送该上下文；委托用原生函数指针构造
 * （Delegate::.ctor(object, IntPtr)）。 */

bool cnc_mt_init(void);

/* 惰性初始化（线程安全，只试一次）；命令路径用它 */
bool cnc_mt_ensure(void);

/* 在主线程执行 fn(arg)，等待完成（timeout_ms 毫秒超时返回 false）。 */
bool cnc_mt_run(void (*fn)(void *), void *arg, int timeout_ms);

/* 在主线程执行 fn(arg)，不等待（fire-and-forget）。 */
bool cnc_mt_post(void (*fn)(void *), void *arg);

#endif
