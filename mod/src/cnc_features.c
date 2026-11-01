#include "cnc_features.h"
#include "cnc_game.h"
#include "cnc_log.h"
#include "cnc_resolver.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* 功能层：状态汇总 + 命令分发（M2 起各作弊功能挂在 cnc_game 层）。 */

static char g_game_version[32] = "";
static pthread_mutex_t g_ensure_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_ensure_done = 0; /* 0/1 = 未完成（未试或游戏未就绪），2 = 完成 */

/* 在 Unity 主线程上执行：绑定 IL2CPP、等域、解析全部游戏类型。
 * 游戏尚未启动完（域为空）时返回 false，下一个命令会再试。 */
/* v3.1：ensure 拆两级。
 * 轻探针（每次轮询）：bind + 域检查 + Journal.Instance 检查 —— 全是单次轻调用；
 * 重活（全量解析）只在 存档已加载（Instance 非空）后做一次。
 * 背景：标题/加载窗口期在主线程做全量枚举会卡死主线程 → macOS 看门狗强杀
 * （2026-10-30 实测：面板 2s 轮询 + 加载窗口重活 = 游戏 10-60s 内被 SIGKILL）。 */
bool cnc_features_ensure(void) {
    if (g_ensure_done == 2) return true;
    pthread_mutex_lock(&g_ensure_mu);
    if (g_ensure_done != 2) {
        if (!g_il2.ga) cnc_il2cpp_bind();
        if (g_il2.ga && g_il2.domain_get) {
            cnc_il2cpp_attach();
            if (!g_il2.domain) g_il2.domain = g_il2.domain_get();
            if (g_il2.domain) {
                /* 轻探针：Journal.Instance 非空 = 存档已加载 = 游戏可安全承受重活 */
                void *jk = cnc_find_class("Game", "Journal");
                void *inst = NULL;
                if (jk) {
                    void *m = g_il2.class_get_method_from_name(jk, "get_Instance", 0);
                    if (m) inst = g_il2.runtime_invoke(m, NULL, NULL, NULL);
                }
                if (inst) {
                    size_t n = 0;
                    if (g_il2.domain_get_assemblies) {
                        g_il2.domain_get_assemblies(g_il2.domain, &n);
                        g_il2.assembly_count = n;
                    }
                    if (!cnc_features_init())
                        cnc_log("features: 初始化不完整（部分功能可能不可用）");
                    g_ensure_done = 2;
                    cnc_log("features: 主线程惰性初始化完成（存档已加载）");
                } else {
                    static int logged = 0;
                    if (!logged) { cnc_log("features: 标题画面中，轻探针待命"); logged = 1; }
                }
            }
        }
    }
    pthread_mutex_unlock(&g_ensure_mu);
    return g_ensure_done == 2;
}

bool cnc_features_init(void) {
    /* 运行时版本探测（NFR3：启动期与 version.txt 双校验） */
    void *klass = cnc_find_class("UnityEngine", "Application");
    if (!klass) {
        cnc_log("features: UnityEngine.Application 未找到");
        return false;
    }
    void *m = g_il2.class_get_method_from_name(klass, "get_version", 0);
    if (!m) {
        cnc_log("features: Application.get_version 方法未找到");
        return false;
    }
    void *ret = g_il2.runtime_invoke(m, NULL, NULL, NULL);
    if (!ret) {
        cnc_log("features: get_version 调用失败");
        return false;
    }
    char *v = cnc_string_to_utf8(ret);
    if (v) {
        snprintf(g_game_version, sizeof g_game_version, "%s", v);
        free(v);
    }
    cnc_log("features: 游戏运行时版本 = %s", g_game_version);

    if (!cnc_game_init())
        cnc_log("features: 功能层初始化不完整（部分功能可能不可用）");
    return true;
}

void cnc_features_state_json(char *out, size_t outsz) {
    int cash = cnc_game_cash(); /* -1 = 存档未加载 */
    snprintf(out, outsz,
             "{\"ok\":true,\"pid\":%d,\"assemblies\":%zu,"
             "\"game_version\":\"%s\",\"mod_version\":\"0.3.0\","
             "\"cash\":%d,\"speed\":%.2f,"
             "\"features\":[\"cash\",\"speed\",\"sell\"]}",
             getpid(), g_il2.assembly_count, g_game_version,
             cash, cnc_game_speed());
}

bool cnc_features_dispatch(const char *line, char *out, size_t outsz) {
    return cnc_game_dispatch(line, out, outsz);
}
