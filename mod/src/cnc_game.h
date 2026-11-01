#ifndef CNC_GAME_H
#define CNC_GAME_H

#include <stdbool.h>
#include <stddef.h>

/* 功能层：金钱 / 加速 / 卖鱼 / 钓鱼增强 / 时间等。全部函数只在 Unity 主线程调用。 */

/* 解析并缓存全部游戏侧类型/方法（IL2CPP 就绪后调用一次）；
 * 部分缺失返回 false，对应命令回 unsupported，其余功能照常可用 */
bool cnc_game_init(void);

/* 命令分发：cash/speed/sell/storage/time/ids + M4 钓鱼增强 */
bool cnc_game_dispatch(const char *line, char *out, size_t outsz);

/* 供 state.get 汇总：输出不带花括号的 JSON 字段片段，
 * 如 "cash":123,"speed":1.00,"features":[...],"toggles":{...},"mults":{...} */
void cnc_game_state_json(char *out, size_t outsz);

/* 当前金钱，-1 = 存档未加载或不支持 */
int cnc_game_cash(void);
float cnc_game_speed(void);

#endif
