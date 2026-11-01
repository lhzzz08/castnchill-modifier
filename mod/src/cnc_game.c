/* M2 P0 功能实现。
 * 全部按名解析（cnc_game_init 缓存），按名调用；零硬编码偏移。 */
#include "cnc_game.h"
#include "cnc_log.h"
#include "cnc_mt.h"
#include "cnc_resolver.h"

#include <stdatomic.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---------- 调用辅助 ---------- */

static void *invoke0(void *method, void *obj) {
    void *exc = NULL;
    return g_il2.runtime_invoke(method, obj, NULL, &exc);
}

static void *invoke1(void *method, void *obj, void *p0) {
    void *params[1] = {p0};
    void *exc = NULL;
    return g_il2.runtime_invoke(method, obj, params, &exc);
}

static void *invoke2(void *method, void *obj, void *p0, void *p1) {
    void *params[2] = {p0, p1};
    void *exc = NULL;
    return g_il2.runtime_invoke(method, obj, params, &exc);
}

/* runtime_invoke 对值类型返回会装箱：[klass 8][monitor 8][data] */
static int unbox_int(void *boxed) { return boxed ? *(int *)((char *)boxed + 0x10) : 0; }
static float unbox_float(void *boxed) { return boxed ? *(float *)((char *)boxed + 0x10) : 0.f; }

/* runtime_invoke 包装：记录异常供诊断（避免静默失败） */
static void *last_exc = NULL;

static void *inv0(void *method, void *obj) {
    last_exc = NULL;
    return g_il2.runtime_invoke(method, obj, NULL, &last_exc);
}
static void *inv1(void *method, void *obj, void *p0) {
    void *params[1] = {p0};
    last_exc = NULL;
    return g_il2.runtime_invoke(method, obj, params, &last_exc);
}
static void *inv2(void *method, void *obj, void *p0, void *p1) {
    void *params[2] = {p0, p1};
    last_exc = NULL;
    return g_il2.runtime_invoke(method, obj, params, &last_exc);
}

/* ---------- 解析缓存 ---------- */

static void *g_journal, *g_journal_inst;      /* Game.Journal + 单例实例 */
static void *m_get_Instance, *m_get_Cash, *m_set_Cash, *m_SaveEntry;
static void *m_get_CatchesToSellCount, *m_SellNext;
static void *g_tip_entry;                     /* Game.CashTipJournalEntry */
static int g_tip_value_off = -1;
static void *g_time;                          /* UnityEngine.Time */
static void *m_get_timeScale, *m_set_timeScale;
static void *g_gm;                            /* Game.GameManager */
static void *m_get_NormalTimeScale, *m_set_NormalTimeScale;
static void *g_stats;                         /* Steamworks.SteamUserStats */
static void *m_GetAchievementCount, *m_GetAchievementName, *m_GetAchievement;
static void *m_ClearAchievement, *m_StoreStats;
static void *g_jmgr;                          /* Game.JournalManager */
static void *m_jmgr_get_Instance, *m_jmgr_Save;
static void *m_TipRusty;


static bool resolve_all(void) {
    bool ok = true;

    g_journal = cnc_find_class("Game", "Journal");
    if (!g_journal) { cnc_log("game: Game.Journal 未找到"); return false; }
    m_get_Instance = g_il2.class_get_method_from_name(g_journal, "get_Instance", 0);
    m_get_Cash = g_il2.class_get_method_from_name(g_journal, "get_Cash", 0);
    m_set_Cash = g_il2.class_get_method_from_name(g_journal, "set_Cash", 1);
    m_SaveEntry = g_il2.class_get_method_from_name(g_journal, "SaveEntry", 1);
    m_get_CatchesToSellCount =
        g_il2.class_get_method_from_name(g_journal, "get_CatchesToSellCount", 0);
    m_SellNext = g_il2.class_get_method_from_name(g_journal, "SellNext", 0);
    m_TipRusty = g_il2.class_get_method_from_name(g_journal, "TipRusty", 1);
    if (!m_TipRusty) cnc_log("game: TipRusty 未找到（金钱类功能不可用）");
    if (!m_get_Instance || !m_get_Cash || !m_SaveEntry || !m_SellNext) {
        cnc_log("game: Journal 关键方法缺失");
        return false;
    }

    g_tip_entry = cnc_find_class("Game", "CashTipJournalEntry");
    void *fld = g_tip_entry ? g_il2.class_get_field_from_name(g_tip_entry, "value") : NULL;
    if (g_tip_entry && fld) {
        g_tip_value_off = g_il2.field_get_offset(fld);
    } else {
        cnc_log("game: CashTipJournalEntry.value 未找到（金钱将仅改内存不落盘）");
    }

    g_time = cnc_find_class("UnityEngine", "Time");
    if (g_time) {
        m_get_timeScale = g_il2.class_get_method_from_name(g_time, "get_timeScale", 0);
        m_set_timeScale = g_il2.class_get_method_from_name(g_time, "set_timeScale", 1);
    }
    if (!m_set_timeScale) { cnc_log("game: Time.set_timeScale 未找到"); ok = false; }

    g_gm = cnc_find_class("Game", "GameManager");
    if (g_gm) {
        m_get_NormalTimeScale = g_il2.class_get_method_from_name(g_gm, "get_NormalTimeScale", 0);
        m_set_NormalTimeScale = g_il2.class_get_method_from_name(g_gm, "set_NormalTimeScale", 1);
    }
    if (!m_set_NormalTimeScale) cnc_log("game: NormalTimeScale 未找到（加速仍可用，暂停恢复时可能还原）");

    g_stats = cnc_find_class("Steamworks", "SteamUserStats");
    if (g_stats) {
        m_GetAchievementCount = g_il2.class_get_method_from_name(g_stats, "GetAchievementCount", 0);
        m_GetAchievementName = g_il2.class_get_method_from_name(g_stats, "GetAchievementName", 1);
        m_GetAchievement = g_il2.class_get_method_from_name(g_stats, "GetAchievement", 2);
        m_ClearAchievement = g_il2.class_get_method_from_name(g_stats, "ClearAchievement", 1);
        m_StoreStats = g_il2.class_get_method_from_name(g_stats, "StoreStats", 0);
    }
    if (!m_ClearAchievement) cnc_log("game: SteamUserStats 未找到（成就拦截不可用）");

    g_jmgr = cnc_find_class("Game", "JournalManager");
    if (g_jmgr) {
        m_jmgr_get_Instance = g_il2.class_get_method_from_name(g_jmgr, "get_Instance", 0);
        m_jmgr_Save = g_il2.class_get_method_from_name(g_jmgr, "Save", 1);
    }
    if (!m_jmgr_Save) cnc_log("game: JournalManager.Save 未找到（改动将等游戏自动落盘）");
    return ok;
}

bool cnc_game_init(void) {
    if (!resolve_all()) return false;
    g_journal_inst = invoke0(m_get_Instance, NULL);
    if (!g_journal_inst) {
        cnc_log("game: Journal.Instance 为空（还在主菜单/存档未加载，cash 命令暂不可用）");
        /* 不算失败：进存档后 dispatch 时会再取 */
    }
    cnc_log("game: 功能层解析完成");
    return true;
}

/* ---------- F01 金钱 ---------- */

int cnc_game_cash(void) {
    if (!g_journal) return -1;
    void *inst = invoke0(m_get_Instance, NULL);
    if (!inst) return -1;
    g_journal_inst = inst;
    return unbox_int(invoke0(m_get_Cash, inst));
}

/* 改动立即落盘（SaveEntry 只入队，JournalManager.Save 才写文件） */
static void flush_save(void) {
    if (!m_jmgr_get_Instance || !m_jmgr_Save) return;
    void *mgr = invoke0(m_jmgr_get_Instance, NULL);
    if (!mgr) return;
    bool force = true;
    invoke1(m_jmgr_Save, mgr, &force);
}

/* F01 金钱：走游戏原生 TipRusty(int) 通道（现金小费事件）。
 * 实测语义：TipRusty(x) => cash -= x，且事件立即入队落盘。
 * 故 加钱 N = TipRusty(-N)；设值 T = TipRusty(cash - T)。 */

static bool cash_apply(int target) {
    void *inst = inv0(m_get_Instance, NULL);
    if (!inst) return false;
    g_journal_inst = inst;
    if (!m_TipRusty) {
        m_TipRusty = g_il2.class_get_method_from_name(g_journal, "TipRusty", 1);
        if (!m_TipRusty) return false;
    }
    int cur = unbox_int(inv0(m_get_Cash, inst));
    int tip = cur - target;
    if (tip != 0) {
        inv1(m_TipRusty, inst, &tip);
        if (last_exc) {
            cnc_log("game: TipRusty 异常 %p", last_exc);
            return false;
        }
    }
    /* set_Cash 兜底（正常情况 TipRusty 已生效，跳过避免双写） */
    int now = unbox_int(inv0(m_get_Cash, inst));
    if (now != target && m_set_Cash) {
        int v = target;
        inv1(m_set_Cash, inst, &v);
    }
    flush_save();
    return true;
}

/* ---------- F02 加速（D4：≤16×） ---------- */

float cnc_game_speed(void) {
    if (!m_get_timeScale) return 0.f;
    return unbox_float(invoke0(m_get_timeScale, NULL));
}

static void speed_apply(float x) {
    if (x < 0.25f) x = 0.25f;
    if (x > 16.f) x = 16.f;
    if (m_set_NormalTimeScale) invoke1(m_set_NormalTimeScale, NULL, &x);
    invoke1(m_set_timeScale, NULL, &x);
}

/* ---------- F17 船存容量（实例枚举 + 字段直写） ---------- */

static int storage_max(void) {
    void *klass = cnc_find_class("Game", "BoatStorageData");
    if (!klass) return -1;
    void *fld = g_il2.class_get_field_from_name(klass, "capacity");
    if (!fld) return -2;
    int off = g_il2.field_get_offset(fld);
    size_t n = 0;
    void **insts = cnc_find_instances(klass, &n);
    if (!insts) return -3;
    int changed = 0, maxcap = 0;
    for (size_t i = 0; i < n; i++) {
        if (!insts[i]) continue;
        int cap = *(int *)((char *)insts[i] + off);
        if (cap > maxcap) maxcap = cap;
        *(int *)((char *)insts[i] + off) = 9999;
        changed++;
    }
    free(insts);
    cnc_log("game: 船存容量 %d 个实例已拉满（原最大 %d）", changed, maxcap);
    return changed;
}

/* ---------- F07 售价倍率（结算补差：走 TipRusty 通道） ---------- */

static float g_sell_mult = 1.0f;

/* ---------- F08 消费不减（现金锁定轮询） ---------- */


/* ---------- F03 一键卖鱼（含 F07 售价倍率结算补差） ---------- */

static int sell_all(void) {
    void *inst = invoke0(m_get_Instance, NULL);
    if (!inst) return -1;
    int sold = 0;
    for (;;) {
        int n = unbox_int(invoke0(m_get_CatchesToSellCount, inst));
        if (n <= 0 || sold >= 500) break;
        invoke0(m_SellNext, inst);
        sold++;
    }
    return sold;
}


/* ---------- M4 配置资产补丁框架 v3：实例枚举 + 字段直写 + 原值恢复 ---------- */

#define MAX_PATCH_INSTS 320

typedef struct {
    void *inst;
    int off;
    bool is_float;
    float f0;
    int i0;
} prec_t;

typedef struct {
    const char *tag;
    bool on;
    float mult;
    prec_t recs[MAX_PATCH_INSTS];
    int n;
} patch_t;

static patch_t g_patches[24];

static patch_t *patch_find(const char *tag) {
    for (int i = 0; i < 24; i++)
        if (g_patches[i].tag && strcmp(g_patches[i].tag, tag) == 0) return &g_patches[i];
    return NULL;
}

static patch_t *patch_get(const char *tag) {
    patch_t *p = patch_find(tag);
    if (p) return p;
    for (int i = 0; i < 24; i++) {
        if (!g_patches[i].tag) {
            g_patches[i].tag = tag;
            g_patches[i].on = false;
            g_patches[i].mult = 1;
            g_patches[i].n = 0;
            return &g_patches[i];
        }
    }
    return NULL;
}

static void patch_restore(const char *tag) {
    patch_t *p = patch_find(tag);
    if (!p || !p->on) return;
    for (int i = 0; i < p->n; i++) {
        prec_t *r = &p->recs[i];
        if (!r->inst) continue;
        if (r->is_float) *(float *)((char *)r->inst + r->off) = r->f0;
        else *(int *)((char *)r->inst + r->off) = r->i0;
    }
    p->on = false;
    p->mult = 1;
}

/* 记录原值并写新值。kind: 's'=set 'm'=mult（以首次记录的原值为基准，
 * 重应用 tick 反复调用也不会复合放大） */
static void prec_write(patch_t *p, void *inst, int off, char kind, float val) {
    int known_idx = -1;
    for (int j = 0; j < p->n; j++)
        if (p->recs[j].inst == inst && p->recs[j].off == off) { known_idx = j; break; }
    float base;
    if (known_idx >= 0) {
        base = p->recs[known_idx].f0;
    } else {
        base = *(float *)((char *)inst + off);
        if (p->n < MAX_PATCH_INSTS) {
            p->recs[p->n].inst = inst;
            p->recs[p->n].off = off;
            p->recs[p->n].is_float = true;
            p->recs[p->n].f0 = base;
            p->n++;
        }
    }
    *(float *)((char *)inst + off) = kind == 'm' ? base * val : val;
}

static void prec_write_i(patch_t *p, void *inst, int off, int val) {
    int known_idx = -1;
    for (int j = 0; j < p->n; j++)
        if (p->recs[j].inst == inst && p->recs[j].off == off) { known_idx = j; break; }
    int i0;
    if (known_idx >= 0) {
        i0 = p->recs[known_idx].i0;
    } else {
        i0 = *(int *)((char *)inst + off);
        if (p->n < MAX_PATCH_INSTS) {
            p->recs[p->n].inst = inst;
            p->recs[p->n].off = off;
            p->recs[p->n].is_float = false;
            p->recs[p->n].i0 = i0;
            p->n++;
        }
    }
    *(int *)((char *)inst + off) = val;
}

/* 对某资产类的全部实例应用字段补丁（Unity Object 派生类专用） */
static int patch_asset_field(patch_t *p, const char *cls, const char *field, char kind, float val) {
    void *klass = cnc_find_class("Game", cls);
    if (!klass) return -1;
    void *fld = g_il2.class_get_field_from_name(klass, field);
    if (!fld) return -2;
    int off = g_il2.field_get_offset(fld);
    size_t n = 0;
    void **insts = cnc_find_instances(klass, &n);
    if (!insts) return -3;
    int changed = 0;
    for (size_t i = 0; i < n; i++) {
        if (!insts[i]) continue;
        prec_write(p, insts[i], off, kind, val);
        changed++;
    }
    free(insts);
    return changed;
}

static int patch_asset_field_i(patch_t *p, const char *cls, const char *field, int val) {
    void *klass = cnc_find_class("Game", cls);
    if (!klass) return -1;
    void *fld = g_il2.class_get_field_from_name(klass, field);
    if (!fld) return -2;
    int off = g_il2.field_get_offset(fld);
    size_t n = 0;
    void **insts = cnc_find_instances(klass, &n);
    if (!insts) return -3;
    int changed = 0;
    for (size_t i = 0; i < n; i++) {
        if (!insts[i]) continue;
        prec_write_i(p, insts[i], off, val);
        changed++;
    }
    free(insts);
    return changed;
}

/* FloatRange 等引用类型字段的安全写入：
 * 字段存的是对象指针（FloatRange 是 class 不是 struct！直接写指针位置 = 崩溃，
 * 2026-10-29 实锤：duration 被写成 0.05 后游戏解引用 0x3d4ccccd... 段错误）。
 * 正确流程：field_get_type 拿字段类型类 → 解引用父实例里的指针得到对象 →
 * 在对象内部的 min/max 字段写值。 */
static int patch_asset_field2(patch_t *p, const char *cls, const char *field, float val) {
    void *klass = cnc_find_class("Game", cls);
    if (!klass) return -1;
    void *fld = g_il2.class_get_field_from_name(klass, field);
    if (!fld) return -2;
    int off = g_il2.field_get_offset(fld);

    void *ik = NULL;
    if (g_il2.field_get_type && g_il2.class_from_il2cpp_type) {
        void *ft = g_il2.field_get_type(fld);
        if (ft) ik = g_il2.class_from_il2cpp_type(ft);
    }
    if (!ik) return -3;
    void *fmin = g_il2.class_get_field_from_name(ik, "min");
    void *fmax = g_il2.class_get_field_from_name(ik, "max");
    if (!fmin || !fmax) return -4;
    int omin = g_il2.field_get_offset(fmin);
    int omax = g_il2.field_get_offset(fmax);

    size_t n = 0;
    void **insts = cnc_find_instances(klass, &n);
    if (!insts) return -5;
    int changed = 0;
    for (size_t i = 0; i < n; i++) {
        if (!insts[i]) continue;
        void *rangeobj = *(void **)((char *)insts[i] + off); /* 解引用字段指针 */
        if (!rangeobj) continue;
        prec_write(p, rangeobj, omin, 's', val);
        prec_write(p, rangeobj, omax, 's', val);
        changed++;
    }
    free(insts);
    return changed;
}

/* v2 核心：链式嵌套路径（父类 = ScriptableObject 资产/单例，嵌套对象是父实例的字段）。
 * 嵌套类类型用 il2cpp_field_get_type → il2cpp_class_from_il2cpp_type 定位。 */
static int patch_nested(patch_t *p, const char *parent_cls, const char *parent_field,
                        const char *inner_field, char kind, float val) {
    void *pk = cnc_find_class("Game", parent_cls);
    if (!pk) return -1;
    void *pf = g_il2.class_get_field_from_name(pk, parent_field);
    if (!pf) return -2;
    int poff = g_il2.field_get_offset(pf);

    void *ik = NULL;
    if (g_il2.field_get_type && g_il2.class_from_il2cpp_type) {
        void *ft = g_il2.field_get_type(pf);
        if (ft) ik = g_il2.class_from_il2cpp_type(ft);
    }
    if (!ik) return -3;
    void *ifld = g_il2.class_get_field_from_name(ik, inner_field);
    if (!ifld) return -4;
    int ioff = g_il2.field_get_offset(ifld);

    size_t n = 0;
    void **insts = cnc_find_instances(pk, &n);
    if (!insts) return -5;
    int changed = 0;
    for (size_t i = 0; i < n; i++) {
        void *parent = insts[i];
        if (!parent) continue;
        void *inner = *(void **)((char *)parent + poff);
        if (!inner) continue;
        prec_write(p, inner, ioff, kind, val);
        changed++;
    }
    free(insts);
    return changed;
}

static void apply_fish_stamina(void) {
    patch_t *p = patch_get("fish.stamina");
    patch_asset_field_i(p, "FishData", "stamina", 1);
    /* 真靶点：FishFightActionData.duration（FloatRange）注释明示
     * "fight 时长 = duration × 体力倍率" —— 直接把区间归 0.05 秒 */
    patch_asset_field2(p, "FishFightActionData", "duration", 0.05f);
}

static void apply_fish_bite(void) {
    patch_t *p = patch_get("fish.bite");
    patch_asset_field_i(p, "FishChaseActionData", "commonNibbleToHookRatio", 1);
    patch_asset_field_i(p, "FishChaseActionData", "bonusNibbleToHookRatio", 1);
    patch_asset_field(p, "FishChaseActionData", "attract", 's', 100.f);
    /* chaseDuration = 鱼愿意追饵的时长：必须加大（v2 误设 0.1 反而让鱼秒失去兴趣） */
    patch_asset_field(p, "FishChaseActionData", "chaseDuration", 's', 60.f);
}

static void apply_line_safe(void) {
    patch_t *p = patch_get("line.safe");
    /* v3：绝对值 86400 秒 —— 连续满张力一整天才会断，实际不可能；
     * ×10 在大鱼长拉锯下仍可能断 */
    patch_asset_field(p, "FishingLineConfig", "breakDelay", 's', 86400.f);
    patch_asset_field(p, "FishingLineConfig", "tooBigBreakDelay", 's', 86400.f);
}

static void apply_player_stamina(void) {
    patch_t *p = patch_get("player.stamina");
    patch_nested(p, "GameplayConfig", "stamina", "rowDrainRate", 's', 0.f);
    patch_nested(p, "GameplayConfig", "stamina", "walkDrainRate", 's', 0.f);
    patch_nested(p, "GameplayConfig", "stamina", "breakIceCost", 's', 0.f);
    patch_nested(p, "GameplayConfig", "stamina", "recoveryRate", 's', 100.f);
}

static void apply_cast_super(void) {
    patch_t *p = patch_get("cast.super");
    /* v3：castSpeed* 是蓄力条摆动速度（v2 设 40/90 = 力度条瞬摆无法掐时机 = 投不出去），
     * 改温和 ×1.5；飞行距离用 FishingLineConfig 的阻力/速度上限实现 */
    patch_nested(p, "RodData", "casting", "castSpeedMin", 'm', 1.5f);
    patch_nested(p, "RodData", "casting", "castSpeedMax", 'm', 1.5f);
    patch_asset_field(p, "FishingLineConfig", "airDrag", 's', 0.f);
    patch_asset_field(p, "FishingLineConfig", "waterDrag", 'm', 0.5f);
    patch_asset_field(p, "FishingLineConfig", "forwardSpeedMax", 'm', 3.f);
}

static void apply_boat_fast(void) {
    patch_t *p = patch_get("boat.fast");
    float mult = p->mult;
    patch_asset_field(p, "BoatEngineData", "maxSpeed", 'm', mult);
    patch_asset_field(p, "BoatEngineData", "acceleration", 'm', mult);
    patch_nested(p, "GameplayConfig", "angler", "boatMaxIdleSpeed", 'm', mult);
    patch_nested(p, "GameplayConfig", "dockWalking", "walkSpeed", 'm', mult);
}

static atomic_bool g_m4_tick_on = false;

static void apply_fish_stamina(void);
static void apply_fish_bite(void);
static void apply_line_safe(void);
static void apply_player_stamina(void);
static void apply_cast_super(void);
static void apply_boat_fast(void);
static void apply_reel_fast(void);
static void apply_ice_fast(void);

static void m4_apply_all(void *unused) {
    (void)unused;
    if (!atomic_load(&g_m4_tick_on)) return;
    patch_t *p;
    if ((p = patch_find("fish.stamina")) && p->on) apply_fish_stamina();
    if ((p = patch_find("fish.bite")) && p->on) apply_fish_bite();
    if ((p = patch_find("line.safe")) && p->on) apply_line_safe();
    if ((p = patch_find("player.stamina")) && p->on) apply_player_stamina();
    if ((p = patch_find("cast.super")) && p->on) apply_cast_super();
    if ((p = patch_find("boat.fast")) && p->on) apply_boat_fast();
    if ((p = patch_find("reel.fast")) && p->on) apply_reel_fast();
    if ((p = patch_find("ice.fast")) && p->on) apply_ice_fast();
}

static bool m4_any_enabled(void) {
    static const char *tags[] = {"fish.stamina", "fish.bite", "line.safe",
                                 "player.stamina", "cast.super", "boat.fast", "reel.fast", "ice.fast"};
    for (size_t i = 0; i < sizeof tags / sizeof tags[0]; i++) {
        patch_t *p = patch_find(tags[i]);
        if (p && p->on) return true;
    }
    return false;
}

static void *m4_tick_thread(void *unused) {
    (void)unused;
    cnc_il2cpp_attach();
    while (atomic_load(&g_m4_tick_on)) {
        cnc_mt_post(m4_apply_all, NULL);
        usleep(2 * 1000 * 1000);
    }
    return NULL;
}

static void m4_ensure_tick(void) {
    bool expected = false;
    if (atomic_compare_exchange_strong(&g_m4_tick_on, &expected, true)) {
        pthread_t th;
        if (pthread_create(&th, NULL, m4_tick_thread, NULL) == 0)
            pthread_detach(th);
        else
            atomic_store(&g_m4_tick_on, false);
    }
}

static void m4_stop_if_idle(void) {
    if (!m4_any_enabled()) atomic_store(&g_m4_tick_on, false);
}

/* 开关入口：on = 标记 + 立即应用一次 + 确保 tick 存活；off = 恢复原值 + 空闲则停 tick */
static void f_fish_stamina(bool on) {
    patch_t *p = patch_get("fish.stamina");
    if (on) {
        bool was = p->on;
        p->on = true;
        apply_fish_stamina();
        if (!was) {
            int n = p->n;
            cnc_log("game: 秒赢对决 → FishData.stamina + 战斗时长 0.05s（已记录 %d 字段）", n);
        }
        m4_ensure_tick();
    } else {
        patch_restore("fish.stamina");
        m4_stop_if_idle();
    }
}

static void f_fish_bite(bool on) {
    patch_t *p = patch_get("fish.bite");
    if (on) {
        bool was = p->on;
        p->on = true;
        apply_fish_bite();
        if (!was) cnc_log("game: 即时上钩 → 咬钩率 1 + attract 100 + 追饵 60s");
        m4_ensure_tick();
    } else {
        patch_restore("fish.bite");
        m4_stop_if_idle();
    }
}

static void f_line_safe(bool on) {
    patch_t *p = patch_get("line.safe");
    if (on) {
        bool was = p->on;
        p->on = true;
        apply_line_safe();
        if (!was) cnc_log("game: 不断之线 → 断线计时 86400s（实际不可断，张力/震动保留）");
        m4_ensure_tick();
    } else {
        patch_restore("line.safe");
        m4_stop_if_idle();
    }
}

static void f_player_stamina(bool on) {
    patch_t *p = patch_get("player.stamina");
    if (on) {
        bool was = p->on;
        p->on = true;
        apply_player_stamina();
        if (!was) cnc_log("game: 无限体力 → 划船/走路/破冰消耗 0（仅影响玩家体力条）");
        m4_ensure_tick();
    } else {
        patch_restore("player.stamina");
        m4_stop_if_idle();
    }
}

static void f_cast_super(bool on) {
    patch_t *p = patch_get("cast.super");
    if (on) {
        bool was = p->on;
        p->on = true;
        apply_cast_super();
        if (!was) cnc_log("game: 超级投掷 → 甩竿 ×1.5 + 飞行阻力降低 + 速度上限 ×3");
        m4_ensure_tick();
    } else {
        patch_restore("cast.super");
        m4_stop_if_idle();
    }
}

static void f_boat_fast(float mult) {
    patch_t *p = patch_get("boat.fast");
    patch_restore("boat.fast");
    if (mult <= 1.0f) { m4_stop_if_idle(); return; }
    p->on = true;
    p->mult = mult;
    apply_boat_fast();
    cnc_log("game: 船速 ×%.0f → 引擎 + 放置闲逛 + 码头走路", mult);
    m4_ensure_tick();
}

static void apply_reel_fast(void) {
    patch_t *p = patch_get("reel.fast");
    float mult = p->mult;
    patch_asset_field(p, "FishingLineConfig", "hookedReelSpeed", 'm', mult);
    patch_asset_field(p, "FishingLineConfig", "hookedSpoolSpeed", 'm', mult);
    patch_asset_field(p, "FishingLineConfig", "autoReelSpeed", 'm', mult);
    patch_asset_field(p, "FishingLineConfig", "autoReelSpeedMax", 'm', mult);
    patch_asset_field(p, "FishingLineConfig", "spoolSpeed", 'm', mult);
}

static void f_reel_fast(float mult) {
    patch_t *p = patch_get("reel.fast");
    patch_restore("reel.fast");
    if (mult <= 1.0f) { m4_stop_if_idle(); return; }
    p->on = true;
    p->mult = mult;
    apply_reel_fast();
    cnc_log("game: 快速收线 ×%.0f（5 字段）", mult);
    m4_ensure_tick();
}

static void apply_ice_fast(void) {
    patch_t *p = patch_get("ice.fast");
    float mult = p->mult;
    patch_nested(p, "GameplayConfig", "ice", "walkSpeed", 'm', mult);
    patch_nested(p, "GameplayConfig", "ice", "walkSpeedZ", 'm', mult);
}

static void f_ice_fast(float mult) {
    patch_t *p = patch_get("ice.fast");
    patch_restore("ice.fast");
    if (mult <= 1.0f) { m4_stop_if_idle(); return; }
    p->on = true;
    p->mult = mult;
    apply_ice_fast();
    cnc_log("game: 冰面速度 ×%.0f", mult);
    m4_ensure_tick();
}

static void f_fish_trophy(bool on) {
    patch_t *p = patch_get("fish.trophy");
    if (on && !p->on) {
        p->on = true;
        int n = patch_asset_field_i(p, "FishData", "trophySize", 1);
        cnc_log("game: 必出奖杯 → FishData.trophySize=1 (%d 实例)", n);
    } else if (!on) patch_restore("fish.trophy");
}

/* ---------- M5: F21 即时码头销售 + F24 时间控制 ---------- */

static atomic_bool g_sell_instant = false;

static void sell_tick_on_main(void *unused) {
    (void)unused;
    void *klass = cnc_find_class("Game", "Boat");
    if (!klass) return;
    void *fld = g_il2.class_get_field_from_name(klass, "m_nextSellTime");
    if (!fld) return;
    int off = g_il2.field_get_offset(fld);
    size_t n = 0;
    void **insts = cnc_find_instances(klass, &n);
    if (!insts) return;
    for (size_t i = 0; i < n; i++)
        if (insts[i]) *(float *)((char *)insts[i] + off) = 0.f;
    free(insts);
}

static void *sell_tick_thread(void *unused) {
    (void)unused;
    cnc_il2cpp_attach();
    while (atomic_load(&g_sell_instant)) {
        cnc_mt_post(sell_tick_on_main, NULL);
        usleep(200 * 1000);
    }
    return NULL;
}

static void sell_instant_set(bool on) {
    bool was = atomic_exchange(&g_sell_instant, on);
    if (on && !was) {
        pthread_t th;
        if (pthread_create(&th, NULL, sell_tick_thread, NULL) == 0) pthread_detach(th);
    }
}

static bool time_set(float minutes) {
    void *klass = g_journal;
    void *fld = g_il2.class_get_field_from_name(klass, "m_timeOfDay");
    if (!fld) return false;
    int off = g_il2.field_get_offset(fld);
    void *inst = inv0(m_get_Instance, NULL);
    if (!inst) return false;
    g_journal_inst = inst;
    *(float *)((char *)inst + off) = minutes;
    return true;
}

static float time_get(void) {
    void *fld = g_il2.class_get_field_from_name(g_journal, "m_timeOfDay");
    if (!fld) return -1.f;
    int off = g_il2.field_get_offset(fld);
    void *inst = inv0(m_get_Instance, NULL);
    if (!inst) return -1.f;
    return *(float *)((char *)inst + off);
}

/* ---------- 命令分发 ---------- */

static bool parse_long(const char *s, long *out) {
    char *end = NULL;
    *out = strtol(s, &end, 10);
    return end && *end == '\0' && end != s;
}

static bool parse_float(const char *s, float *out) {
    char *end = NULL;
    *out = strtof(s, &end);
    return end && *end == '\0' && end != s;
}

/* TipRusty 原生加钱（delta 可负）：正值扣钱、负值加钱，事件即时落盘 */
static void tip_delta(int delta) {
    void *inst = inv0(m_get_Instance, NULL);
    if (!inst) return;
    if (delta != 0) {
        int tip = -delta;
        inv1(m_TipRusty, inst, &tip);
    }
}

/* F18 素材：枚举全部资产 ID（外观/鱼种等）写 ~/.castnchill-modifier/game-ids.json */
static const char *k_id_classes[] = {
    "FishData", "AnglerSkinData", "AnglerOutfitData", "AnglerHatData",
    "RodData", "LureData", "BoatVariantData", "CompanionData",
    "BoatStorageData", NULL,
};

static void ids_dump(const char *line, char *out, size_t outsz) {
    (void)line;
    const char *home = getenv("HOME");
    char path[512];
    snprintf(path, sizeof path, "%s/.castnchill-modifier/game-ids.json", home ? home : ".");
    FILE *fp = fopen(path, "w");
    if (!fp) {
        snprintf(out, outsz, "{\"ok\":false,\"error\":\"cannot write\"}");
        return;
    }
    fputs("{", fp);
    bool first_cat = true;
    char summary[512] = "";
    for (int ci = 0; k_id_classes[ci]; ci++) {
        void *klass = cnc_find_class("Game", k_id_classes[ci]);
        if (!klass) continue;
        void *fld = g_il2.class_get_field_from_name(klass, "id");
        if (!fld) continue;
        int off = g_il2.field_get_offset(fld);
        size_t n = 0;
        void **insts = cnc_find_instances(klass, &n);
        if (!insts) continue;
        if (!first_cat) fputs(",", fp);
        first_cat = false;
        fprintf(fp, "\"%s\":[", k_id_classes[ci]);
        int written = 0;
        for (size_t i = 0; i < n; i++) {
            if (!insts[i]) continue;
            void *str = *(void **)((char *)insts[i] + off);
            if (!str) continue;
            char *id = cnc_string_to_utf8(str);
            if (!id || !*id) { free(id); continue; }
            if (written) fputc(',', fp);
            fprintf(fp, "\"%s\"", id);
            free(id);
            written++;
        }
        free(insts);
        fputs("]", fp);
        char part[64];
        snprintf(part, sizeof part, "%s:%d ", k_id_classes[ci], written);
        strncat(summary, part, sizeof summary - strlen(summary) - 1);
    }
    fputs("}", fp);
    fclose(fp);
    snprintf(out, outsz, "{\"ok\":true,\"dump\":\"%s\",\"ids\":\"%s\"}", path, summary);
}

bool cnc_game_dispatch(const char *line, char *out, size_t outsz) {
    char cmd[64] = {0}, a1[64] = {0};
    sscanf(line, "%63s %63s", cmd, a1);

    if (strcmp(cmd, "cash.get") == 0) {
        snprintf(out, outsz, "{\"ok\":true,\"cash\":%d}", cnc_game_cash());
        return true;
    }
    if (strcmp(cmd, "cash.add") == 0 || strcmp(cmd, "cash.set") == 0) {
        long n = 0;
        if (!parse_long(a1, &n)) {
            snprintf(out, outsz, "{\"ok\":false,\"error\":\"bad amount\"}");
            return true;
        }
        long target = strcmp(cmd, "cash.add") == 0 ? cnc_game_cash() + n : n;
        if (target < 0) target = 0;
        if (target > 1000000000L) target = 1000000000L; /* D4：单次 ≤10 亿 */
        if (!cash_apply((int)target)) {
            snprintf(out, outsz, "{\"ok\":false,\"error\":\"journal not loaded（先进存档）\"}");
            return true;
        }
        snprintf(out, outsz, "{\"ok\":true,\"cash\":%d}", cnc_game_cash());
        return true;
    }
    if (strcmp(cmd, "speed.get") == 0) {
        snprintf(out, outsz, "{\"ok\":true,\"speed\":%.2f}", cnc_game_speed());
        return true;
    }
    if (strcmp(cmd, "speed.set") == 0) {
        float x = 0;
        if (!parse_float(a1, &x)) {
            snprintf(out, outsz, "{\"ok\":false,\"error\":\"bad speed\"}");
            return true;
        }
        speed_apply(x);
        snprintf(out, outsz, "{\"ok\":true,\"speed\":%.2f}", cnc_game_speed());
        return true;
    }
    if (strcmp(cmd, "sell.mult") == 0) {
        float x = 0;
        if (!parse_float(a1, &x)) {
            snprintf(out, outsz, "{\"ok\":false,\"error\":\"bad mult\"}");
            return true;
        }
        if (x < 1) x = 1;
        if (x > 100) x = 100; /* D4 上限 */
        g_sell_mult = x;
        snprintf(out, outsz, "{\"ok\":true,\"sell_mult\":%.1f}", g_sell_mult);
        return true;
    }
    if (strcmp(cmd, "sell.all") == 0) {
        int before = cnc_game_cash();
        int sold = sell_all();
        if (sold < 0) {
            snprintf(out, outsz, "{\"ok\":false,\"error\":\"journal not loaded\"}");
            return true;
        }
        /* F07 结算补差：总卖价 × (mult-1) 通过原生小费通道补发 */
        int earned = cnc_game_cash() - before;
        long bonus = (long)((float)earned * (g_sell_mult - 1.0f));
        if (bonus > 0 && sold > 0) tip_delta((int)bonus);
        flush_save();
        snprintf(out, outsz,
                 "{\"ok\":true,\"sold\":%d,\"earned\":%d,\"bonus\":%ld,\"cash\":%d,\"mult\":%.1f}",
                 sold, earned, (bonus > 0 && sold > 0) ? bonus : 0, cnc_game_cash(), g_sell_mult);
        return true;
    }
    if (strcmp(cmd, "storage.max") == 0) {
        int changed = storage_max();
        if (changed < 0) {
            const char *why =
                changed == -3 ? "FindObjectsOfTypeAll 失败" : "类/字段未找到";
            snprintf(out, outsz, "{\"ok\":false,\"error\":\"%s\"}", why);
            return true;
        }
        snprintf(out, outsz, "{\"ok\":true,\"instances\":%d,\"capacity\":9999}", changed);
        return true;
    }
    if (strcmp(cmd, "sell.instant") == 0) {
        if (strcmp(a1, "on") == 0) sell_instant_set(true);
        else if (strcmp(a1, "off") == 0) sell_instant_set(false);
        else if (a1[0]) {
            snprintf(out, outsz, "{\"ok\":false,\"error\":\"usage: sell.instant on|off\"}");
            return true;
        }
        snprintf(out, outsz, "{\"ok\":true,\"sell.instant\":\"%s\"}",
                 atomic_load(&g_sell_instant) ? "on" : "off");
        return true;
    }
    if (strcmp(cmd, "time.set") == 0) {
        float m = 0;
        if (!parse_float(a1, &m)) {
            snprintf(out, outsz, "{\"ok\":false,\"error\":\"bad minutes (0-1440)\"}");
            return true;
        }
        if (m < 0) m = 0;
        if (m > 1440) m = 1440;
        if (!time_set(m)) {
            snprintf(out, outsz, "{\"ok\":false,\"error\":\"journal not loaded\"}");
            return true;
        }
        snprintf(out, outsz, "{\"ok\":true,\"time_of_day\":%.0f}", time_get());
        return true;
    }
    if (strcmp(cmd, "time.get") == 0) {
        snprintf(out, outsz, "{\"ok\":true,\"time_of_day\":%.0f}", time_get());
        return true;
    }

    /* ---------- M4 钓鱼增强 ---------- */
    struct { const char *cmd; void (*fn)(bool); } toggles[] = {
        {"fish.stamina", f_fish_stamina},
        {"fish.trophy", f_fish_trophy},
        {"fish.bite", f_fish_bite},
        {"line.safe", f_line_safe},
        {"player.stamina", f_player_stamina},
        {"cast.super", f_cast_super},
    };
    for (size_t i = 0; i < sizeof toggles / sizeof toggles[0]; i++) {
        if (strcmp(cmd, toggles[i].cmd) == 0) {
            bool on = strcmp(a1, "on") == 0;
            if (!on && strcmp(a1, "off") != 0) {
                snprintf(out, outsz, "{\"ok\":false,\"error\":\"usage: %s on|off\"}", cmd);
                return true;
            }
            toggles[i].fn(on);
            patch_t *p = patch_find(toggles[i].cmd);
            snprintf(out, outsz, "{\"ok\":true,\"feature\":\"%s\",\"state\":\"%s\"}",
                     toggles[i].cmd, p && p->on ? "on" : "off");
            return true;
        }
    }
    struct { const char *cmd; void (*fn)(float); float maxv; } mults[] = {
        {"reel.fast", f_reel_fast, 20.f},
        {"boat.fast", f_boat_fast, 10.f},
        {"ice.fast", f_ice_fast, 10.f},
    };
    for (size_t i = 0; i < sizeof mults / sizeof mults[0]; i++) {
        if (strcmp(cmd, mults[i].cmd) == 0) {
            float x = 0;
            if (!parse_float(a1, &x)) {
                snprintf(out, outsz, "{\"ok\":false,\"error\":\"bad mult\"}");
                return true;
            }
            if (x < 1) x = 1;
            if (x > mults[i].maxv) x = mults[i].maxv;
            mults[i].fn(x);
            patch_t *p = patch_find(mults[i].cmd);
            snprintf(out, outsz, "{\"ok\":true,\"%s\":%.1f}", mults[i].cmd, p && p->on ? p->mult : 1.0f);
            return true;
        }
    }

    if (strcmp(cmd, "ach.petreset") == 0) {
        /* 抚摸宠物成就计数器归零（成就被首次误触发消耗后，用于重新测试）。
         * 同时报告当前值，便于确认是否已消耗。 */
        void *klass = cnc_find_class("Game", "PatCompanionAchievementData");
        if (!klass) {
            snprintf(out, outsz, "{\"ok\":false,\"error\":\"class not found\"}");
            return true;
        }
        void *fcnt = g_il2.class_get_field_from_name(klass, "count");
        void *fcur = g_il2.class_get_field_from_name(klass, "m_currentCount");
        if (!fcnt || !fcur) {
            snprintf(out, outsz, "{\"ok\":false,\"error\":\"fields not found\"}");
            return true;
        }
        int ocnt = g_il2.field_get_offset(fcnt);
        int ocur = g_il2.field_get_offset(fcur);
        size_t n = 0;
        void **insts = cnc_find_instances(klass, &n);
        if (!insts) {
            snprintf(out, outsz, "{\"ok\":false,\"error\":\"no instances\"}");
            return true;
        }
        int target = 0, cur = 0, reset = 0;
        for (size_t i = 0; i < n; i++) {
            if (!insts[i]) continue;
            target = *(int *)((char *)insts[i] + ocnt);
            cur = *(int *)((char *)insts[i] + ocur);
            *(int *)((char *)insts[i] + ocur) = 0;
            reset++;
        }
        free(insts);
        snprintf(out, outsz,
                 "{\"ok\":true,\"target\":%d,\"was\":%d,\"reset_instances\":%d,"
                 "\"note\":\"计数器已归零，重新抚摸 %d 次即可再次触发\"}",
                 target, cur, reset, target);
        return true;
    }

    if (strcmp(cmd, "fish.debug") == 0) {
        /* 诊断：现场读 FishData.stamina 与 FishFightActionData.duration 的真实状态 */
        int smin = 2147483647, smax = -2147483648, scnt = 0, sones = 0;
        void *fk = cnc_find_class("Game", "FishData");
        if (fk) {
            void *fld = g_il2.class_get_field_from_name(fk, "stamina");
            if (fld) {
                int off = g_il2.field_get_offset(fld);
                size_t n = 0;
                void **insts = cnc_find_instances(fk, &n);
                if (insts) {
                    for (size_t i = 0; i < n; i++) {
                        if (!insts[i]) continue;
                        int v = *(int *)((char *)insts[i] + off);
                        if (v < smin) smin = v;
                        if (v > smax) smax = v;
                        if (v == 1) sones++;
                        scnt++;
                    }
                    free(insts);
                }
            }
        }
        /* duration FloatRange：读解引用后的 min/max 现值 */
        float dmin = -1, dmax = -1;
        int dcnt = 0;
        void *dk = cnc_find_class("Game", "FishFightActionData");
        if (dk) {
            void *dfld = g_il2.class_get_field_from_name(dk, "duration");
            void *ik = NULL;
            if (dfld && g_il2.field_get_type && g_il2.class_from_il2cpp_type) {
                void *ft = g_il2.field_get_type(dfld);
                if (ft) ik = g_il2.class_from_il2cpp_type(ft);
            }
            if (dfld && ik) {
                void *fmin = g_il2.class_get_field_from_name(ik, "min");
                void *fmax = g_il2.class_get_field_from_name(ik, "max");
                if (fmin && fmax) {
                    int off = g_il2.field_get_offset(dfld);
                    int omin = g_il2.field_get_offset(fmin);
                    int omax = g_il2.field_get_offset(fmax);
                    size_t n = 0;
                    void **insts = cnc_find_instances(dk, &n);
                    if (insts) {
                        for (size_t i = 0; i < n; i++) {
                            if (!insts[i]) continue;
                            void *ro = *(void **)((char *)insts[i] + off);
                            if (!ro) continue;
                            float a = *(float *)((char *)ro + omin);
                            float b = *(float *)((char *)ro + omax);
                            if (dmin < 0 || a < dmin) dmin = a;
                            if (b > dmax) dmax = b;
                            dcnt++;
                        }
                        free(insts);
                    }
                }
            }
        }
        snprintf(out, outsz,
                 "{\"ok\":true,\"fish_stamina\":{\"count\":%d,\"min\":%d,\"max\":%d,\"ones\":%d},"
                 "\"fight_duration\":{\"count\":%d,\"min\":%.2f,\"max\":%.2f}}",
                 scnt, smin == 2147483647 ? 0 : smin, smax, sones, dcnt, dmin, dmax);
        return true;
    }

    if (strcmp(cmd, "enum.test") == 0) {
        /* 诊断：逐步报告某类的枚举链路 */
        char steps[512] = "";
        void *k = cnc_find_class("Game", a1);
        snprintf(steps, sizeof steps, "class=%p", k);
        if (k && g_il2.class_get_type && g_il2.type_get_object) {
            void *t = g_il2.class_get_type(k);
            void *to = t ? g_il2.type_get_object(t) : NULL;
            snprintf(steps + strlen(steps), sizeof steps - strlen(steps),
                     " type=%p typeobj=%p", t, to);
            void *res = cnc_find_class("UnityEngine", "Resources");
            void *m = res ? g_il2.class_get_method_from_name(res, "FindObjectsOfTypeAll", 1) : NULL;
            snprintf(steps + strlen(steps), sizeof steps - strlen(steps),
                     " res=%p m=%p", res, m);
            if (m && to) {
                void *exc = NULL;
                void *params[1] = {to};
                void *arr = g_il2.runtime_invoke(m, NULL, params, &exc);
                snprintf(steps + strlen(steps), sizeof steps - strlen(steps),
                         " arr=%p exc=%p", arr, exc);
                if (arr && !exc) {
                    size_t len = g_il2.array_length(arr);
                    snprintf(steps + strlen(steps), sizeof steps - strlen(steps),
                             " len=%zu", len);
                }
            }
        } else {
            snprintf(steps + strlen(steps), sizeof steps - strlen(steps),
                     " class_get_type=%p type_get_object=%p",
                     (void *)g_il2.class_get_type, (void *)g_il2.type_get_object);
        }
        snprintf(out, outsz, "{\"ok\":true,\"cls\":\"%s\",\"%s\"}", a1, steps);
        return true;
    }

    if (strcmp(cmd, "ids.dump") == 0) {
        ids_dump(line, out, outsz);
        return true;
    }
    if (strcmp(cmd, "save.now") == 0) {
        flush_save();
        snprintf(out, outsz, "{\"ok\":true}");
        return true;
    }
    if (strcmp(cmd, "debug.tiprusty") == 0) {
        /* 探针：调用游戏自身的 TipRusty(int)，观察金钱方向与落盘行为 */
        long n = 0;
        if (!parse_long(a1, &n)) {
            snprintf(out, outsz, "{\"ok\":false,\"error\":\"bad amount\"}");
            return true;
        }
        void *inst = inv0(m_get_Instance, NULL);
        if (!inst) {
            snprintf(out, outsz, "{\"ok\":false,\"error\":\"journal not loaded\"}");
            return true;
        }
        void *m_tip = g_il2.class_get_method_from_name(g_journal, "TipRusty", 1);
        if (!m_tip) {
            snprintf(out, outsz, "{\"ok\":false,\"error\":\"TipRusty not found\"}");
            return true;
        }
        int before = cnc_game_cash();
        int v = (int)n;
        inv1(m_tip, inst, &v);
        if (last_exc) cnc_log("game: TipRusty 异常 %p", last_exc);
        flush_save();
        snprintf(out, outsz, "{\"ok\":true,\"before\":%d,\"after\":%d}", before, cnc_game_cash());
        return true;
    }

    /* 未匹配命令 → 交给上层报 unknown cmd（不可缺：缺了会跌出函数返回垃圾值） */
    return false;
}


