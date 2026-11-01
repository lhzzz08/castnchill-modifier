#include "cnc_ipc.h"
#include "cnc_features.h"
#include "cnc_json.h"
#include "cnc_log.h"
#include "cnc_mt.h"
#include "cnc_version.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define IPC_REQ_MAX 512
#define IPC_RESP_MAX 2048

static char g_sock_path[128];

const char *cnc_ipc_path(void) { return g_sock_path; }

/* 一次写出 "s\n"；对端已断开时返回而非触发 SIGPIPE（fd 已设 SO_NOSIGPIPE） */
static void send_line(int fd, const char *s) {
    char buf[IPC_RESP_MAX + 2];
    int len = snprintf(buf, sizeof buf, "%s\n", s);
    if (len < 0) return;
    if ((size_t)len >= sizeof buf) {
        len = (int)sizeof buf - 1;
        buf[len - 1] = '\n';
    }
    size_t off = 0;
    while (off < (size_t)len) {
        ssize_t w = write(fd, buf + off, (size_t)len - off);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return;
        off += (size_t)w;
    }
}

/* 读到换行 / 缓冲满 / EOF / 超时为止，返回字节数（已 '\0' 结尾，去掉行尾） */
static ssize_t read_line(int fd, char *buf, size_t bufsz) {
    size_t got = 0;
    while (got < bufsz - 1) {
        ssize_t r = read(fd, buf + got, bufsz - 1 - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        got += (size_t)r;
        if (memchr(buf + got - (size_t)r, '\n', (size_t)r)) break;
    }
    buf[got] = '\0';
    buf[strcspn(buf, "\r\n")] = '\0';
    return (ssize_t)got;
}

/* 按命令分发，结果写入 out。 */
static void handle_line(const char *line, char *out, size_t outsz) {
    /* 命令格式：`cmd [args...]`，参数由各 feature 自行解析 */
    char cmd[64] = {0};
    sscanf(line, "%63s", cmd);

    if (strcmp(cmd, "state.get") == 0) {
        cnc_features_state_json(out, outsz);
        return;
    }
    if (cnc_features_dispatch(line, out, outsz))
        return;

    char esc[160];
    cnc_json_escape(cmd, esc, sizeof esc);
    snprintf(out, outsz, "{\"ok\":false,\"error\":\"unknown cmd: %s\"}", esc);
}

/* 客户端线程与主线程 job 共享的上下文：堆分配 + 引用计数，
 * 任何一方超时/先走都不会让另一方写失效内存 */
typedef struct {
    atomic_int refs;
    char req[IPC_REQ_MAX];
    char resp[IPC_RESP_MAX];
} ipc_ctx_t;

static void ctx_release(ipc_ctx_t *ctx) {
    if (atomic_fetch_sub(&ctx->refs, 1) == 1) free(ctx);
}

static void ipc_handle_on_main(void *p) {
    ipc_ctx_t *ctx = p;
    /* 惰性初始化也在主线程做（后台线程碰 IL2CPP 会段错误）。
     * 游戏还在启动时命令返回 booting，面板自动重试。 */
    if (!cnc_features_ensure())
        snprintf(ctx->resp, sizeof ctx->resp, "{\"ok\":false,\"error\":\"booting\"}");
    else
        handle_line(ctx->req, ctx->resp, sizeof ctx->resp);
    ctx_release(ctx);
}

/* 面板每 2s 轮询的命令不写日志（否则日志每小时数千行） */
static bool is_quiet_cmd(const char *line) {
    return strncmp(line, "state.get", 9) == 0 || strncmp(line, "ping", 4) == 0;
}

static void *client_thread(void *arg) {
    int fd = (int)(long)arg;
    char buf[IPC_REQ_MAX];

    struct timeval tv = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);

    /* 单连接单命令（面板每次请求短连），够用且免状态机 */
    if (read_line(fd, buf, sizeof buf) > 0 && buf[0]) {
        bool quiet = is_quiet_cmd(buf);
        if (!quiet) cnc_log("ipc <- %s", buf);

        if (strcmp(buf, "ping") == 0) {
            /* ping 不碰托管堆，后台线程直接答（游戏加载中也能探活） */
            char resp[160];
            snprintf(resp, sizeof resp,
                     "{\"ok\":true,\"proto\":%d,\"mod\":\"libcncmod\",\"mod_version\":\"%s\"}",
                     CNC_PROTO_VERSION, CNC_MOD_VERSION);
            send_line(fd, resp);
            close(fd);
            return NULL;
        }

        ipc_ctx_t *ctx = calloc(1, sizeof *ctx);
        if (!ctx) {
            send_line(fd, "{\"ok\":false,\"error\":\"out of memory\"}");
            close(fd);
            return NULL;
        }
        atomic_init(&ctx->refs, 2); /* 本线程 1 + 主线程 job 1 */
        snprintf(ctx->req, sizeof ctx->req, "%s", buf);

        /* 全部游戏侧调用编组到 Unity 主线程（IL2CPP 托管堆非线程安全） */
        cnc_mt_status_t st = CNC_MT_FAILED;
        if (cnc_mt_ensure())
            st = cnc_mt_run(ipc_handle_on_main, ctx, 8000);
        const char *resp;
        if (st == CNC_MT_OK)
            resp = ctx->resp[0] ? ctx->resp : "{\"ok\":false,\"error\":\"empty response\"}";
        else if (st == CNC_MT_TIMEOUT)
            resp = "{\"ok\":false,\"error\":\"main-thread timeout\"}";
        else
            resp = "{\"ok\":false,\"error\":\"main-thread unavailable\"}";
        if (!quiet || st != CNC_MT_OK) cnc_log("ipc -> %.200s", resp);
        send_line(fd, resp);

        if (st == CNC_MT_FAILED) ctx_release(ctx); /* job 没派发出去，替它释放 */
        ctx_release(ctx);
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
