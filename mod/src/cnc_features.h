#ifndef CNC_FEATURES_H
#define CNC_FEATURES_H

#include <stdbool.h>
#include <stddef.h>

/* 功能模块层：状态汇总 + 命令分发。M2 起各作弊功能在此注册。 */

/* 主线程惰性初始化（绑定 IL2CPP → 查域 → 解析游戏类）。
 * 必须在 Unity 主线程上调用（由 IPC 分发路径保证）；
 * 游戏还在启动时返回 false，命令方应返回 booting 类错误。 */
bool cnc_features_ensure(void);

/* 等 IL2CPP 就绪后调用一次：解析游戏侧类/方法并缓存。 */
bool cnc_features_init(void);

/* state.get 的 JSON 输出 */
void cnc_features_state_json(char *out, size_t outsz);

/* 命令分发：识别返回 true 并填写 out。 */
bool cnc_features_dispatch(const char *line, char *out, size_t outsz);

#endif
