#ifndef CNC_IPC_H
#define CNC_IPC_H

#include <stdbool.h>

/* 启动 Unix domain socket 服务（阻塞在 accept 循环，应在专用线程调用）。
 * socket 路径：/tmp/cnc-mod-<uid>.sock，权限 0600，仅本用户可连。 */
void cnc_ipc_serve(void);

/* 取 socket 路径（供日志/诊断）。 */
const char *cnc_ipc_path(void);

#endif
