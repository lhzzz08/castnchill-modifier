/* M2 P0 功能实现。
 * 全部按名解析（cnc_game_init 缓存），按名调用；零硬编码偏移。 */
#include "cnc_game.h"
#include "cnc_json.h"
#include "cnc_log.h"
#include "cnc_mt.h"
#include "cnc_resolver.h"

#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---------- 调用辅助 ---------- */

/* runtime_invoke 包装：必须传 exc 出参（传 NULL 时托管异常会直接让进程崩溃），
 * 最近一次异常记在 last_exc 供诊断。method 为空（游戏更新后没解析到）时不调用。 */
static void *last_exc = NULL;

static void *inv0(void *method, void *obj) {
    last_exc = NULL;
    if (!method) return NULL;
    return g_il2.runtime_invoke(method, obj, NULL, &last_exc);
}
static void *inv1(void *method, void *obj, void *p0) {
    void *params[1] = {p0};
    last_exc = NULL;
    if (!method) return NULL;
    return g_il2.runtime_invoke(method, obj, params, &last_exc);
}

/* runtime_invoke 对值类型返回会装箱：[klass 8][monitor 8][data]。
 * 调用失败（异常/空返回）返回 false —— 不能把失败当成 0 参与计算 */
static bool inv_int(void *method, void *obj, int *out) {
    void *boxed = inv0(method, obj);
    if (last_exc || !boxed) return false;
    *out = *(int *)((char *)boxed + 0x10);
    return true;
}
static bool inv_float(void *method, void *obj, float *out) {
    void *boxed = inv0(method, obj);
    if (last_exc || !boxed) return false;
    *out = *(float *)((char *)boxed + 0x10);
    return true;
}

/* 托管对象头第一个字段是类指针 */
static void *obj_klass(void *obj) { return *(void **)obj; }

/* ---------- 解析缓存 ---------- */

static void *g_journal;                       /* Game.Journal */
static void *m_get_Instance, *m_get_Cash, *m_set_Cash;
static void *m_get_CatchesToSellCount, *m_SellNext;
static void *m_TipRusty;
static int g_time_of_day_off = -1;            /* Journal.m_timeOfDay */
static void *m_get_timeScale, *m_set_timeScale;
static void *m_set_NormalTimeScale;           /* Game.GameManager */
static void *m_jmgr_get_Instance, *m_jmgr_Save;

static void *method(void *klass, const char *name, int argc) {
    return klass ? g_il2.class_get_method_from_name(klass, name, argc) : NULL;
}

/* 各项缺失只关掉对应功能（命令回 unsupported），不影响其余功能 */
static bool resolve_all(void) {
    bool ok = true;

    g_journal = cnc_find_class("Game", "Journal");
    if (!g_journal) cnc_log("game: Game.Journal 未找到");
    m_get_Instance = method(g_journal, "get_Instance", 0);
    m_get_Cash = method(g_journal, "get_Cash", 0);
    m_set_Cash = method(g_journal, "set_Cash", 1);
    m_get_CatchesToSellCount = method(g_journal, "get_CatchesToSellCount", 0);
    m_SellNext = method(g_journal, "SellNext", 0);
    m_TipRusty = method(g_journal, "TipRusty", 1);
    void *tod = g_journal ? g_il2.class_get_field_from_name(g_journal, "m_timeOfDay") : NULL;
    if (tod) g_time_of_day_off = g_il2.field_get_offset(tod);
    if (!m_get_Instance || !m_get_Cash) {
        cnc_log("game: Journal.Instance/Cash 缺失（金钱/卖鱼/时间不可用）");
        ok = false;
    }
    if (!m_TipRusty) { cnc_log("game: TipRusty 未找到（金钱类功能不可用）"); ok = false; }
    if (!m_get_CatchesToSellCount || !m_SellNext) {
        cnc_log("game: SellNext 未找到（一键卖鱼不可用）");
        ok = false;
    }

    void *tk = cnc_find_class("UnityEngine", "Time");
    m_get_timeScale = method(tk, "get_timeScale", 0);
    m_set_timeScale = method(tk, "set_timeScale", 1);
    if (!m_set_timeScale) { cnc_log("game: Time.set_timeScale 未找到"); ok = false; }

    m_set_NormalTimeScale = method(cnc_find_class("Game", "GameManager"), "set_NormalTimeScale", 1);
    if (!m_set_NormalTimeScale) cnc_log("game: NormalTimeScale 未找到（加速仍可用，暂停恢复时可能还原）");

    void *jmgr = cnc_find_class("Game", "JournalManager");
    m_jmgr_get_Instance = method(jmgr, "get_Instance", 0);
    m_jmgr_Save = method(jmgr, "Save", 1);
    if (!m_jmgr_Save) cnc_log("game: JournalManager.Save 未找到（改动将等游戏自动落盘）");
    return ok;
}

bool cnc_game_init(void) {
    bool ok = resolve_all();
    cnc_log("game: 功能层解析完成%s", ok ? "" : "（部分缺失）");
    return ok;
}

/* ---------- F01 金钱 ---------- */

static bool cash_supported(void) { return m_get_Instance && m_get_Cash && m_TipRusty; }

static void *journal(void) {
    void *inst = inv0(m_get_Instance, NULL);
    return last_exc ? NULL : inst;
}

int cnc_game_cash(void) {
    void *inst = journal();
    int cash;
    if (!inst || !inv_int(m_get_Cash, inst, &cash)) return -1;
    return cash;
}

/* 改动立即落盘（SaveEntry 只入队，JournalManager.Save 才写文件） */
static void flush_save(void) {
    void *mgr = inv0(m_jmgr_get_Instance, NULL);
    if (!mgr || !m_jmgr_Save) return;
    bool force = true;
    inv1(m_jmgr_Save, mgr, &force);
}

/* TipRusty 原生改钱：delta 为正加钱、为负扣钱，事件即时入队落盘。
 * 实测语义：TipRusty(x) => cash -= x，故传 -delta。 */
static bool tip_delta(void *inst, long delta) {
    if (!m_TipRusty) return false;
    if (delta == 0) return true;
    if (delta > INT_MAX) delta = INT_MAX;
    if (delta < -(long)INT_MAX) delta = -(long)INT_MAX;
    int tip = (int)-delta;
    inv1(m_TipRusty, inst, &tip);
    if (last_exc) {
        cnc_log("game: TipRusty 异常 %p", last_exc);
        return false;
    }
    return true;
}

/* F01 金钱：设值 T = TipRusty(cash - T)；set_Cash 只作兜底 */
static bool cash_apply(long target) {
    if (!cash_supported()) return false;
    void *inst = journal();
    int cur;
    if (!inst || !inv_int(m_get_Cash, inst, &cur)) return false;
    if (target < 0) target = 0;
    if (target > INT_MAX) target = INT_MAX;
    if (!tip_delta(inst, target - cur)) return false;
    /* set_Cash 兜底（正常情况 TipRusty 已生效，跳过避免双写） */
    int now;
    if (m_set_Cash && inv_int(m_get_Cash, inst, &now) && now != target) {
        int v = (int)target;
        inv1(m_set_Cash, inst, &v);
    }
    flush_save();
    return true;
}

/* ---------- F02 加速（D4：≤16×） ---------- */

float cnc_game_speed(void) {
    float x = 0.f;
    if (!inv_float(m_get_timeScale, NULL, &x)) return 0.f;
    return x;
}

static void speed_apply(float x) {
    if (x < 0.25f) x = 0.25f;
    if (x > 16.f) x = 16.f;
    if (m_set_NormalTimeScale) inv1(m_set_NormalTimeScale, NULL, &x);
    inv1(m_set_timeScale, NULL, &x);
}

/* ---------- 单 tick / 单命令内的实例枚举缓存 ----------
 * FindObjectsOfTypeAll 扫全部托管对象，代价大；同一次主线程调用里同一个类只枚举一次。
 * 数组由缓存持有，icache_end() 统一释放（cnc_game_dispatch 与 tick 结束时调用）。 */

#define ICACHE_MAX 16

static struct {
    void *klass;
    void **insts;
    size_t n;
} g_icache[ICACHE_MAX];
static int g_icache_n;

static void icache_end(void) {
    for (int i = 0; i < g_icache_n; i++) free(g_icache[i].insts);
    g_icache_n = 0;
}

static void **instances(void *klass, size_t *n) {
    for (int i = 0; i < g_icache_n; i++)
        if (g_icache[i].klass == klass) { *n = g_icache[i].n; return g_icache[i].insts; }
    void **insts = cnc_find_instances(klass, n);
    if (!insts) return NULL;
    if (g_icache_n == ICACHE_MAX) icache_end(); /* 调用方都不跨类持有数组，满了整体清空即可 */
    g_icache[g_icache_n].klass = klass;
    g_icache[g_icache_n].insts = insts;
    g_icache[g_icache_n].n = *n;
    g_icache_n++;
    return insts;
}

/* ---------- F17 船存容量（实例枚举 + 字段直写） ---------- */

static int storage_max(void) {
    void *klass = cnc_find_class("Game", "BoatStorageData");
    if (!klass) return -1;
    void *fld = g_il2.class_get_field_from_name(klass, "capacity");
    if (!fld) return -2;
    int off = g_il2.field_get_offset(fld);
    size_t n = 0;
    void **insts = instances(klass, &n);
    if (!insts) return -3;
    int changed = 0, maxcap = 0;
    for (size_t i = 0; i < n; i++) {
        if (!insts[i]) continue;
        int cap = *(int *)((char *)insts[i] + off);
        if (cap > maxcap) maxcap = cap;
        *(int *)((char *)insts[i] + off) = 9999;
        changed++;
    }
    cnc_log("game: 船存容量 %d 个实例已拉满（原最大 %d）", changed, maxcap);
    return changed;
}

/* ---------- F07 售价倍率（结算补差：走 TipRusty 通道） ---------- */

static float g_sell_mult = 1.0f;

/* ---------- F03 一键卖鱼（含 F07 售价倍率结算补差） ---------- */

static bool sell_supported(void) {
    return m_get_Instance && m_get_CatchesToSellCount && m_SellNext;
}

/* 返回卖出条数；-1 = 存档未加载 */
static int sell_all(void) {
    void *inst = journal();
    if (!inst) return -1;
    int sold = 0, left = 0;
    while (sold < 500 && inv_int(m_get_CatchesToSellCount, inst, &left) && left > 0) {
        inv0(m_SellNext, inst);
        if (last_exc) {
            cnc_log("game: SellNext 异常 %p（已卖 %d 条）", last_exc, sold);
            break;
        }
        sold++;
    }
    return sold;
}

/* ---------- M4 配置资产补丁框架 v4：实例枚举 + 字段直写 + 原值恢复 ----------
 * v4：记录表改为动态数组（v3 定长 320 条，满了之后照写不记 → 关不回去，
 * 'm' 倍率还会每 tick 复合放大）；每条记录存类指针，恢复/复写前校验，
 * 对象被卸载、内存被别的对象复用时不再写入。 */

typedef struct {
    void *inst;
    void *klass; /* 记录时 inst 的类指针 */
    int off;
    bool is_float;
    float f0;
    int i0;
} prec_t;

typedef struct {
    const char *tag;
    bool on;
    float mult;
    prec_t *recs;
    int n, cap;
} patch_t;

#define MAX_PATCHES 24

static patch_t g_patches[MAX_PATCHES];

static patch_t *patch_find(const char *tag) {
    for (int i = 0; i < MAX_PATCHES; i++)
        if (g_patches[i].tag && strcmp(g_patches[i].tag, tag) == 0) return &g_patches[i];
    return NULL;
}

static patch_t *patch_get(const char *tag) {
    patch_t *p = patch_find(tag);
    if (p) return p;
    for (int i = 0; i < MAX_PATCHES; i++) {
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

/* 恢复全部原值并清空记录（下次开启重新记录，不残留已卸载对象的指针） */
static void patch_restore(const char *tag) {
    patch_t *p = patch_find(tag);
    if (!p) return;
    if (p->on) {
        for (int i = 0; i < p->n; i++) {
            prec_t *r = &p->recs[i];
            if (obj_klass(r->inst) != r->klass) continue; /* 对象已不在 */
            if (r->is_float) *(float *)((char *)r->inst + r->off) = r->f0;
            else *(int *)((char *)r->inst + r->off) = r->i0;
        }
    }
    p->n = 0;
    p->on = false;
    p->mult = 1;
}

/* 找到或新建 (inst, off) 的原值记录；内存不够返回 NULL（调用方不写，保证可恢复） */
static prec_t *prec_slot(patch_t *p, void *inst, int off, bool is_float) {
    void *k = obj_klass(inst);
    for (int j = 0; j < p->n; j++) {
        prec_t *r = &p->recs[j];
        if (r->inst != inst || r->off != off) continue;
        if (r->klass == k) return r;
        *r = p->recs[--p->n]; /* 旧对象没了、地址被复用：作废旧记录，重新记录 */
        break;
    }
    if (p->n == p->cap) {
        int ncap = p->cap ? p->cap * 2 : 64;
        prec_t *nr = realloc(p->recs, (size_t)ncap * sizeof *nr);
        if (!nr) return NULL;
        p->recs = nr;
        p->cap = ncap;
    }
    prec_t *r = &p->recs[p->n++];
    r->inst = inst;
    r->klass = k;
    r->off = off;
    r->is_float = is_float;
    if (is_float) r->f0 = *(float *)((char *)inst + off);
    else r->i0 = *(int *)((char *)inst + off);
    return r;
}

/* 记录原值并写新值。kind: 's'=set 'm'=mult（以首次记录的原值为基准，
 * 重应用 tick 反复调用也不会复合放大） */
static void prec_write(patch_t *p, void *inst, int off, char kind, float val) {
    prec_t *r = prec_slot(p, inst, off, true);
    if (!r) return;
    *(float *)((char *)inst + off) = kind == 'm' ? r->f0 * val : val;
}

static void prec_write_i(patch_t *p, void *inst, int off, int val) {
    if (!prec_slot(p, inst, off, false)) return;
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
    void **insts = instances(klass, &n);
    if (!insts) return -3;
    int changed = 0;
    for (size_t i = 0; i < n; i++) {
        if (!insts[i]) continue;
        prec_write(p, insts[i], off, kind, val);
        changed++;
    }
    return changed;
}

static int patch_asset_field_i(patch_t *p, const char *cls, const char *field, int val) {
    void *klass = cnc_find_class("Game", cls);
    if (!klass) return -1;
    void *fld = g_il2.class_get_field_from_name(klass, field);
    if (!fld) return -2;
    int off = g_il2.field_get_offset(fld);
    size_t n = 0;
    void **insts = instances(klass, &n);
    if (!insts) return -3;
    int changed = 0;
    for (size_t i = 0; i < n; i++) {
        if (!insts[i]) continue;
        prec_write_i(p, insts[i], off, val);
        changed++;
    }
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
    void **insts = instances(klass, &n);
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
    void **insts = instances(pk, &n);
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

/* tick 线程按"代"运行：关闭或重开都会换代，旧线程醒来发现代号变了就退出，
 * 2s 内先关后开也不会出现两个 tick 线程。pending 防止主线程卡顿时 tick 堆积。
 * tick 线程只负责 post，不碰托管堆，所以不 attach il2cpp。 */
static atomic_bool g_m4_tick_on = false;
static atomic_uint g_m4_gen;
static atomic_bool g_m4_pending;

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
    atomic_store(&g_m4_pending, false);
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
    icache_end();
}

/* 需要 tick 重应用的补丁（fish.trophy 是一次性写入，不在此列） */
static const char *const k_tick_tags[] = {"fish.stamina", "fish.bite", "line.safe", "player.stamina",
                                          "cast.super", "boat.fast", "reel.fast", "ice.fast"};

static bool m4_any_enabled(void) {
    for (size_t i = 0; i < sizeof k_tick_tags / sizeof k_tick_tags[0]; i++) {
        patch_t *p = patch_find(k_tick_tags[i]);
        if (p && p->on) return true;
    }
    return false;
}

static void *m4_tick_thread(void *arg) {
    unsigned gen = (unsigned)(uintptr_t)arg;
    while (atomic_load(&g_m4_gen) == gen) {
        if (!atomic_exchange(&g_m4_pending, true))
            if (!cnc_mt_post(m4_apply_all, NULL)) atomic_store(&g_m4_pending, false);
        usleep(2 * 1000 * 1000);
    }
    return NULL;
}

static void m4_ensure_tick(void) {
    if (atomic_load(&g_m4_tick_on)) return;
    unsigned gen = atomic_fetch_add(&g_m4_gen, 1) + 1;
    atomic_store(&g_m4_tick_on, true);
    pthread_t th;
    if (pthread_create(&th, NULL, m4_tick_thread, (void *)(uintptr_t)gen) == 0)
        pthread_detach(th);
    else
        atomic_store(&g_m4_tick_on, false);
}

static void m4_stop_if_idle(void) {
    if (m4_any_enabled()) return;
    atomic_store(&g_m4_tick_on, false);
    atomic_fetch_add(&g_m4_gen, 1);
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

/* 与 M4 tick 同样按"代"运行，防止快速开关出现两个线程 */
static atomic_bool g_sell_instant = false;
static atomic_uint g_sell_gen;
static atomic_bool g_sell_pending;

/* Boat 实例缓存（只在主线程访问）：每 200ms 全量 FindObjectsOfTypeAll 太重，
 * 每 10 tick（≈2s）重新枚举一次，其间按记录的类指针校验后直写 */
static void **g_boats, **g_boat_klass;
static size_t g_boat_n;
static int g_boat_off = -1, g_boat_age;

static void sell_tick_on_main(void *unused) {
    (void)unused;
    atomic_store(&g_sell_pending, false);
    if (!atomic_load(&g_sell_instant)) return;
    if (g_boat_off < 0) {
        void *klass = cnc_find_class("Game", "Boat");
        void *fld = klass ? g_il2.class_get_field_from_name(klass, "m_nextSellTime") : NULL;
        if (!fld) return;
        g_boat_off = g_il2.field_get_offset(fld);
    }
    if (!g_boats || ++g_boat_age >= 10) {
        size_t n = 0;
        void **insts = cnc_find_instances(cnc_find_class("Game", "Boat"), &n);
        void **ks = insts ? malloc((n ? n : 1) * sizeof *ks) : NULL;
        if (!ks) { free(insts); return; }
        for (size_t i = 0; i < n; i++) ks[i] = insts[i] ? obj_klass(insts[i]) : NULL;
        free(g_boats);
        free(g_boat_klass);
        g_boats = insts;
        g_boat_klass = ks;
        g_boat_n = n;
        g_boat_age = 0;
    }
    for (size_t i = 0; i < g_boat_n; i++)
        if (g_boats[i] && obj_klass(g_boats[i]) == g_boat_klass[i])
            *(float *)((char *)g_boats[i] + g_boat_off) = 0.f;
}

static void *sell_tick_thread(void *arg) {
    unsigned gen = (unsigned)(uintptr_t)arg;
    while (atomic_load(&g_sell_gen) == gen) {
        if (!atomic_exchange(&g_sell_pending, true))
            if (!cnc_mt_post(sell_tick_on_main, NULL)) atomic_store(&g_sell_pending, false);
        usleep(200 * 1000);
    }
    return NULL;
}

static void sell_instant_set(bool on) {
    bool was = atomic_exchange(&g_sell_instant, on);
    if (on == was) return;
    unsigned gen = atomic_fetch_add(&g_sell_gen, 1) + 1;
    if (!on) return;
    g_boat_age = 10; /* 重开时强制重新枚举 */
    pthread_t th;
    if (pthread_create(&th, NULL, sell_tick_thread, (void *)(uintptr_t)gen) == 0)
        pthread_detach(th);
    else
        atomic_store(&g_sell_instant, false);
}

static bool time_set(float minutes) {
    if (g_time_of_day_off < 0) return false;
    void *inst = journal();
    if (!inst) return false;
    *(float *)((char *)inst + g_time_of_day_off) = minutes;
    return true;
}

static float time_get(void) {
    if (g_time_of_day_off < 0) return -1.f;
    void *inst = journal();
    if (!inst) return -1.f;
    return *(float *)((char *)inst + g_time_of_day_off);
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
        void **insts = instances(klass, &n);
        if (!insts) continue;
        if (!first_cat) fputs(",", fp);
        first_cat = false;
        cnc_json_fputs(fp, k_id_classes[ci]);
        fputs(":[", fp);
        int written = 0;
        for (size_t i = 0; i < n; i++) {
            if (!insts[i]) continue;
            void *str = *(void **)((char *)insts[i] + off);
            if (!str) continue;
            char *id = cnc_string_to_utf8(str);
            if (!id || !*id) { free(id); continue; }
            if (written) fputc(',', fp);
            cnc_json_fputs(fp, id);
            free(id);
            written++;
        }
        fputs("]", fp);
        char part[64];
        snprintf(part, sizeof part, "%s:%d ", k_id_classes[ci], written);
        strncat(summary, part, sizeof summary - strlen(summary) - 1);
    }
    fputs("}", fp);
    fclose(fp);
    char epath[600], esum[600];
    cnc_json_escape(path, epath, sizeof epath);
    cnc_json_escape(summary, esum, sizeof esum);
    snprintf(out, outsz, "{\"ok\":true,\"dump\":\"%s\",\"ids\":\"%s\"}", epath, esum);
}

/* ---------- 命令表（dispatch 与 state.get 共用） ---------- */

static const struct { const char *cmd; void (*fn)(bool); } k_toggles[] = {
    {"fish.stamina", f_fish_stamina},
    {"fish.trophy", f_fish_trophy},
    {"fish.bite", f_fish_bite},
    {"line.safe", f_line_safe},
    {"player.stamina", f_player_stamina},
    {"cast.super", f_cast_super},
};

static const struct { const char *cmd; void (*fn)(float); float maxv; } k_mults[] = {
    {"reel.fast", f_reel_fast, 20.f},
    {"boat.fast", f_boat_fast, 10.f},
    {"ice.fast", f_ice_fast, 10.f},
};

#define D4_CASH_STEP 1000000000L /* D4：单次金钱改动 ≤10 亿 */

static bool unsupported(char *out, size_t outsz) {
    snprintf(out, outsz, "{\"ok\":false,\"error\":\"unsupported（游戏版本不兼容此功能）\"}");
    return true;
}

static bool dispatch(const char *line, char *out, size_t outsz) {
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
        if (!cash_supported()) return unsupported(out, outsz);
        long cur = cnc_game_cash();
        if (cur < 0) {
            snprintf(out, outsz, "{\"ok\":false,\"error\":\"journal not loaded（先进存档）\"}");
            return true;
        }
        long target;
        if (strcmp(cmd, "cash.add") == 0) {
            /* 限制的是本次增量，而不是结果：已有 >10 亿时加钱不会被压回 10 亿 */
            if (n > D4_CASH_STEP) n = D4_CASH_STEP;
            if (n < -D4_CASH_STEP) n = -D4_CASH_STEP;
            target = cur + n;
        } else {
            target = n > D4_CASH_STEP ? D4_CASH_STEP : n;
        }
        if (!cash_apply(target)) {
            snprintf(out, outsz, "{\"ok\":false,\"error\":\"改钱失败（见 /tmp/cnc-mod.log）\"}");
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
        if (!m_set_timeScale) return unsupported(out, outsz);
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
        if (!sell_supported()) return unsupported(out, outsz);
        long before = cnc_game_cash();
        int sold = sell_all();
        if (sold < 0 || before < 0) {
            snprintf(out, outsz, "{\"ok\":false,\"error\":\"journal not loaded\"}");
            return true;
        }
        /* F07 结算补差：总卖价 × (mult-1) 通过原生小费通道补发，
         * 封顶到 int 上限，避免溢出成负数反而扣钱 */
        long after = cnc_game_cash();
        long earned = after > before ? after - before : 0;
        long bonus = 0;
        if (sold > 0 && earned > 0 && g_sell_mult > 1.0f && m_TipRusty) {
            double b = (double)earned * (double)(g_sell_mult - 1.0f);
            long room = (long)INT_MAX - after;
            bonus = b > (double)room ? room : (long)b;
            void *inst = journal();
            if (bonus <= 0 || !inst || !tip_delta(inst, bonus)) bonus = 0;
        }
        flush_save();
        snprintf(out, outsz,
                 "{\"ok\":true,\"sold\":%d,\"earned\":%ld,\"bonus\":%ld,\"cash\":%d,\"mult\":%.1f}",
                 sold, earned, bonus, cnc_game_cash(), g_sell_mult);
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
        if (g_time_of_day_off < 0) return unsupported(out, outsz);
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
    for (size_t i = 0; i < sizeof k_toggles / sizeof k_toggles[0]; i++) {
        if (strcmp(cmd, k_toggles[i].cmd) == 0) {
            bool on = strcmp(a1, "on") == 0;
            if (!on && strcmp(a1, "off") != 0) {
                snprintf(out, outsz, "{\"ok\":false,\"error\":\"usage: %s on|off\"}", cmd);
                return true;
            }
            k_toggles[i].fn(on);
            patch_t *p = patch_find(k_toggles[i].cmd);
            snprintf(out, outsz, "{\"ok\":true,\"feature\":\"%s\",\"state\":\"%s\"}",
                     k_toggles[i].cmd, p && p->on ? "on" : "off");
            return true;
        }
    }
    for (size_t i = 0; i < sizeof k_mults / sizeof k_mults[0]; i++) {
        if (strcmp(cmd, k_mults[i].cmd) == 0) {
            float x = 0;
            if (!parse_float(a1, &x)) {
                snprintf(out, outsz, "{\"ok\":false,\"error\":\"bad mult\"}");
                return true;
            }
            if (x < 1) x = 1;
            if (x > k_mults[i].maxv) x = k_mults[i].maxv;
            k_mults[i].fn(x);
            patch_t *p = patch_find(k_mults[i].cmd);
            snprintf(out, outsz, "{\"ok\":true,\"%s\":%.1f}", k_mults[i].cmd, p && p->on ? p->mult : 1.0f);
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
        void **insts = instances(klass, &n);
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
                void **insts = instances(fk, &n);
                if (insts) {
                    for (size_t i = 0; i < n; i++) {
                        if (!insts[i]) continue;
                        int v = *(int *)((char *)insts[i] + off);
                        if (v < smin) smin = v;
                        if (v > smax) smax = v;
                        if (v == 1) sones++;
                        scnt++;
                    }
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
                    void **insts = instances(dk, &n);
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
                    }
                }
            }
        }
        snprintf(out, outsz,
                 "{\"ok\":true,\"fish_stamina\":{\"count\":%d,\"min\":%d,\"max\":%d,\"ones\":%d},"
                 "\"fight_duration\":{\"count\":%d,\"min\":%.2f,\"max\":%.2f}}",
                 scnt, scnt ? smin : 0, scnt ? smax : 0, sones, dcnt, dmin, dmax);
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
                if (arr && !exc && g_il2.array_length) {
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
        char ecls[160];
        cnc_json_escape(a1, ecls, sizeof ecls);
        snprintf(out, outsz, "{\"ok\":true,\"cls\":\"%s\",\"steps\":\"%s\"}", ecls, steps);
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
        if (!m_TipRusty) return unsupported(out, outsz);
        void *inst = journal();
        if (!inst) {
            snprintf(out, outsz, "{\"ok\":false,\"error\":\"journal not loaded\"}");
            return true;
        }
        int before = cnc_game_cash();
        int v = n > INT_MAX ? INT_MAX : n < -(long)INT_MAX ? -INT_MAX : (int)n;
        inv1(m_TipRusty, inst, &v);
        if (last_exc) cnc_log("game: TipRusty 异常 %p", last_exc);
        flush_save();
        snprintf(out, outsz, "{\"ok\":true,\"before\":%d,\"after\":%d}", before, cnc_game_cash());
        return true;
    }

    /* 未匹配命令 → 交给上层报 unknown cmd（不可缺：缺了会跌出函数返回垃圾值） */
    return false;
}

bool cnc_game_dispatch(const char *line, char *out, size_t outsz) {
    bool handled = dispatch(line, out, outsz);
    icache_end(); /* 本次命令枚举的实例数组不跨调用保留（对象可能随场景卸载） */
    return handled;
}

void cnc_game_state_json(char *out, size_t outsz) {
    int len = snprintf(out, outsz, "\"cash\":%d,\"speed\":%.2f,\"sell_mult\":%.1f,"
                       "\"sell_instant\":%s,\"features\":[",
                       cnc_game_cash(), cnc_game_speed(), g_sell_mult,
                       atomic_load(&g_sell_instant) ? "true" : "false");
    const char *feats[] = {
        cash_supported() ? "cash" : NULL,
        m_set_timeScale ? "speed" : NULL,
        sell_supported() ? "sell" : NULL,
        g_time_of_day_off >= 0 ? "time" : NULL,
    };
    bool first = true;
    for (size_t i = 0; i < sizeof feats / sizeof feats[0]; i++) {
        if (!feats[i] || len < 0 || (size_t)len >= outsz) continue;
        len += snprintf(out + len, outsz - (size_t)len, "%s\"%s\"", first ? "" : ",", feats[i]);
        first = false;
    }
    if (len >= 0 && (size_t)len < outsz)
        len += snprintf(out + len, outsz - (size_t)len, "],\"toggles\":{");
    for (size_t i = 0; i < sizeof k_toggles / sizeof k_toggles[0]; i++) {
        if (len < 0 || (size_t)len >= outsz) break;
        patch_t *p = patch_find(k_toggles[i].cmd);
        len += snprintf(out + len, outsz - (size_t)len, "%s\"%s\":%s", i ? "," : "",
                        k_toggles[i].cmd, p && p->on ? "true" : "false");
    }
    if (len >= 0 && (size_t)len < outsz)
        len += snprintf(out + len, outsz - (size_t)len, "},\"mults\":{");
    for (size_t i = 0; i < sizeof k_mults / sizeof k_mults[0]; i++) {
        if (len < 0 || (size_t)len >= outsz) break;
        patch_t *p = patch_find(k_mults[i].cmd);
        len += snprintf(out + len, outsz - (size_t)len, "%s\"%s\":%.1f", i ? "," : "",
                        k_mults[i].cmd, p && p->on ? p->mult : 1.0f);
    }
    if (len >= 0 && (size_t)len < outsz)
        snprintf(out + len, outsz - (size_t)len, "}");
}


