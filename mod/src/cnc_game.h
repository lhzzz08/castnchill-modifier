#ifndef CNC_GAME_H
#define CNC_GAME_H

#include <stdbool.h>
#include <stddef.h>

/* M2 P0 功能层：F01 金钱 / F02 加速 / F03 一键卖鱼 / F06 成就拦截 */

/* 解析并缓存全部游戏侧类型/方法（IL2CPP 就绪后调用一次） */
bool cnc_game_init(void);

/* 命令分发：cash/speed/sell/storage/time/ids + M4 钓鱼增强 */
bool cnc_game_dispatch(const char *line, char *out, size_t outsz);

/* 供 state.get 汇总 */
int cnc_game_cash(void);
float cnc_game_speed(void);
bool cnc_game_achieve_blocked(void);
int cnc_game_achievements_cleared(void);

#endif
