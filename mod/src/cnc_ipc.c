#include "cnc_ipc.h"
#include "cnc_features.h"
#include "cnc_log.h"
#include "cnc_mt.h"
#include "cnc_resolver.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define CNC_PROTO_VERSION 1

static char g_sock_path[128];

const char *cnc_ipc_path(void) { return g_sock_path; }

static void send_line(int fd, const char *s) {
    size_t len = strlen(s);
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, s + off, len - off);
        if (w <= 0) return;
        off += (size_t)w;
    }
    write(fd, "\n", 1);
}

/* 按命令分发。返回 JSON 行（静态缓冲区或 malloc，调用方无需 free 的用静态）。 */
static void handle_line(const char *line, char *out, size_t outsz) {
    /* 命令格式：`cmd [args...]`，参数由各 feature 自行解析 */
    char cmd[64] = {0};
    sscanf(line, "%63s", cmd);

    if (strcmp(cmd, "ping") == 0) {
        snprintf(out, outsz,
                 "{\"ok\":true,\"proto\":%d,\"mod\":\"libcncmod\",\"mod_version\":\"0.3.0\"}",
                 CNC_PROTO_VERSION);
        return;
    }
    if (strcmp(cmd, "state.get") == 0) {
        cnc_features_state_json(out, outsz);
        return;
    }
    /* 交给功能模块（M2 起的 cash/speed 等都挂在这里） */
    if (cnc_features_dispatch(line, out, outsz))
        return;

    snprintf(out, outsz, "{\"ok\":false,\"error\":\"unknown cmd: %s\"}", cmd);
}

typedef struct {
    char req[512];
    char resp[1024];
} ipc_ctx_t;

static void ipc_handle_on_main(void *p) {
    ipc_ctx_t *ctx = p;
    /* 惰性初始化也在主线程做（后台线程碰 IL2CPP 会段错误）。
     * 游戏还在启动时命令返回 booting，面板自动重试。 */
    if (!cnc_features_ensure()) {
        snprintf(ctx->resp, sizeof ctx->resp,
                 "{\"ok\":false,\"error\":\"booting\"}");
        return;
    }
    handle_line(ctx->req, ctx->resp, sizeof ctx->resp);
}

static void *client_thread(void *arg) {
    int fd = (int)(long)arg;
    char buf[512];
    static _Thread_local ipc_ctx_t ctx; /* 主线程 job 用 */

    struct timeval tv = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    /* 单连接单命令（面板每次请求短连），够用且免状态机 */
    ssize_t r = read(fd, buf, sizeof buf - 1);
    if (r > 0) {
        buf[r] = '\0';
        buf[strcspn(buf, "\r\n")] = '\0';
        cnc_log("ipc <- %s", buf);
        snprintf(ctx.req, sizeof ctx.req, "%s", buf);
        ctx.resp[0] = '\0';
        /* 全部游戏侧调用编组到 Unity 主线程（IL2CPP 托管堆非线程安全） */
        if (g_il2.domain || cnc_mt_ensure())
            cnc_mt_run(ipc_handle_on_main, &ctx, 8000);
        else
            snprintf(ctx.resp, sizeof ctx.resp,
                     "{\"ok\":false,\"error\":\"main-thread unavailable\"}");
        if (!ctx.resp[0])
            snprintf(ctx.resp, sizeof ctx.resp, "{\"ok\":false,\"error\":\"main-thread timeout\"}");
        cnc_log("ipc -> %.200s", ctx.resp);
        send_line(fd, ctx.resp);
    }
    close(fd);
    return NULL;
}

void cnc_ipc_serve(void) {
    snprintf(g_sock_path, sizeof g_sock_path, "/tmp/cnc-mod-%d.sock", getuid());
    unlink(g_sock_path);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        cnc_log("ipc: socket() 失败 %s", strerror(errno));
        return;
    }
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, g_sock_path, sizeof addr.sun_path - 1);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        cnc_log("ipc: bind(%s) 失败 %s", g_sock_path, strerror(errno));
        close(fd);
        return;
    }
    chmod(g_sock_path, 0600);
    if (listen(fd, 8) < 0) {
        cnc_log("ipc: listen() 失败 %s", strerror(errno));
        close(fd);
        return;
    }
    cnc_log("ipc: 服务就绪 @ %s", g_sock_path);

    for (;;) {
        int cfd = accept(fd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            cnc_log("ipc: accept() 失败 %s", strerror(errno));
            usleep(200 * 1000);
            continue;
        }
        pthread_t th;
        if (pthread_create(&th, NULL, client_thread, (void *)(long)cfd) == 0)
            pthread_detach(th);
        else
            close(cfd);
    }
}
