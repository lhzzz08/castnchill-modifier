#include "cnc_features.h"
#include "cnc_game.h"
#include "cnc_json.h"
#include "cnc_log.h"
#include "cnc_resolver.h"
#include "cnc_version.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* 功能层：状态汇总 + 命令分发（M2 起各作弊功能挂在 cnc_game 层）。 */

static char g_game_version[32] = "";
static pthread_mutex_t g_ensure_mu = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool g_ensure_done = false;

/* v3.1：ensure 拆两级。
 * 轻探针（每次轮询）：bind + 域检查 + Journal.Instance 检查 —— 全是单次轻调用；
 * 重活（全量解析）只在 存档已加载（Instance 非空）后做一次。
 * 背景：标题/加载窗口期在主线程做全量枚举会卡死主线程 → macOS 看门狗强杀
 * （2026-10-30 实测：面板 2s 轮询 + 加载窗口重活 = 游戏 10-60s 内被 SIGKILL）。
 * 必须在 Unity 主线程调用。 */
bool cnc_features_ensure(void) {
    if (atomic_load(&g_ensure_done)) return true;
    pthread_mutex_lock(&g_ensure_mu);
    if (!atomic_load(&g_ensure_done)) {
        if (!g_il2.ga) cnc_il2cpp_bind();
        if (g_il2.ga && !g_il2.domain) {
            g_il2.domain = g_il2.domain_get();
            if (g_il2.domain) cnc_il2cpp_attach();
        }
        if (g_il2.domain) {
            /* 轻探针：Journal.Instance 非空 = 存档已加载 = 游戏可安全承受重活 */
            static void *m_probe;
            if (!m_probe) {
                void *jk = cnc_find_class("Game", "Journal");
                if (jk) m_probe = g_il2.class_get_method_from_name(jk, "get_Instance", 0);
            }
            void *inst = NULL;
            if (m_probe) {
                void *exc = NULL; /* 必须传 exc：传 NULL 时托管异常会直接让进程崩溃 */
                inst = g_il2.runtime_invoke(m_probe, NULL, NULL, &exc);
                if (exc) inst = NULL;
            } else {
                static bool warned;
                if (!warned) {
                    cnc_log("features: Game.Journal.get_Instance 未找到（游戏更新？），跳过存档探针");
                    warned = true;
                }
            }
            if (inst || !m_probe) {
                size_t n = 0;
                g_il2.domain_get_assemblies(g_il2.domain, &n);
                g_il2.assembly_count = n;
                if (!cnc_features_init())
                    cnc_log("features: 初始化不完整（缺失的功能会返回 unsupported）");
                atomic_store(&g_ensure_done, true);
                cnc_log("features: 主线程惰性初始化完成");
            } else {
                static bool logged;
                if (!logged) { cnc_log("features: 标题画面中，轻探针待命"); logged = true; }
            }
        }
    }
    pthread_mutex_unlock(&g_ensure_mu);
    return atomic_load(&g_ensure_done);
}

/* 运行时版本探测（NFR3：启动期与 version.txt 双校验）；失败只影响显示 */
static void probe_version(void) {
    void *klass = cnc_find_class("UnityEngine", "Application");
    void *m = klass ? g_il2.class_get_method_from_name(klass, "get_version", 0) : NULL;
    if (!m) {
        cnc_log("features: Application.get_version 未找到");
        return;
    }
    void *exc = NULL;
    void *ret = g_il2.runtime_invoke(m, NULL, NULL, &exc);
    char *v = exc ? NULL : cnc_string_to_utf8(ret);
    if (!v) {
        cnc_log("features: get_version 调用失败");
        return;
    }
    snprintf(g_game_version, sizeof g_game_version, "%s", v);
    free(v);
    cnc_log("features: 游戏运行时版本 = %s", g_game_version);
}

bool cnc_features_init(void) {
    probe_version();
    return cnc_game_init();
}

void cnc_features_state_json(char *out, size_t outsz) {
    char ver[80], game[1024];
    cnc_json_escape(g_game_version, ver, sizeof ver);
    cnc_game_state_json(game, sizeof game);
    snprintf(out, outsz,
             "{\"ok\":true,\"pid\":%d,\"assemblies\":%zu,"
             "\"game_version\":\"%s\",\"mod_version\":\"%s\",%s}",
             getpid(), g_il2.assembly_count, ver, CNC_MOD_VERSION, game);
}

bool cnc_features_dispatch(const char *line, char *out, size_t outsz) {
    return cnc_game_dispatch(line, out, outsz);
}
