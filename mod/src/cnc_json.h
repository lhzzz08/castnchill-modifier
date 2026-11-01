#ifndef CNC_JSON_H
#define CNC_JSON_H

#include <stddef.h>
#include <stdio.h>

/* 把任意字符串转义成 JSON 字符串内容（不含两侧引号）。
 * 输出总以 '\0' 结尾；空间不够时截断，但不会截断在转义序列中间。 */
void cnc_json_escape(const char *in, char *out, size_t outsz);

/* 直接写一个带引号的 JSON 字符串到文件 */
void cnc_json_fputs(FILE *fp, const char *s);

#endif
