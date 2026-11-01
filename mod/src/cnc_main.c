/* libcncmod —— Cast n Chill 修改器注入模块（入口）
 * 注入方式：launcher 以 DYLD_INSERT_LIBRARIES 拉起游戏。
 *
 * 铁律：本文件的代码运行在后台线程，绝不直接调用任何 IL2CPP API
 * （domain_get / class_from_name / runtime_invoke 等）。冷启动期后台直调
 * 实测段错误（多份 .ips 崩溃栈一致）。全部托管交互延迟到首个 IPC 命令、
 * 经 GCD 编组到主线程执行（见 cnc_features_ensure / cnc_mt）。 */
#include "cnc_features.h"
#include "cnc_ipc.h"
#include "cnc_log.h"

#include <pthread.h>
#include <unistd.h>

#define CNC_LOG_PATH "/tmp/cnc-mod.log"

static void *bootstrap(void *unused) {
    (void)unused;
    cnc_log("=== libcncmod 启动 (pid=%d) ===", getpid());
    cnc_ipc_serve(); /* 阻塞，常驻；IL2CPP 全部走主线程惰性初始化 */
    return NULL;
}

__attribute__((constructor))
static void cnc_ctor(void) {
    cnc_log_open(CNC_LOG_PATH);
    pthread_t th;
    if (pthread_create(&th, NULL, bootstrap, NULL) == 0)
        pthread_detach(th);
    else
        cnc_log("ctor: 后台线程创建失败");
}
