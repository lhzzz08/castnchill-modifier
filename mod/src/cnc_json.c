#include "cnc_json.h"

#include <string.h>

/* 单个字符的转义形式写进 esc，返回长度 */
static size_t escape_char(unsigned char c, char esc[8]) {
    switch (c) {
    case '"': memcpy(esc, "\\\"", 2); return 2;
    case '\\': memcpy(esc, "\\\\", 2); return 2;
    case '\n': memcpy(esc, "\\n", 2); return 2;
    case '\r': memcpy(esc, "\\r", 2); return 2;
    case '\t': memcpy(esc, "\\t", 2); return 2;
    default:
        if (c < 0x20) return (size_t)snprintf(esc, 8, "\\u%04x", c);
        esc[0] = (char)c;
        return 1;
    }
}

void cnc_json_escape(const char *in, char *out, size_t outsz) {
    if (!outsz) return;
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)(in ? in : ""); *p; p++) {
        char esc[8];
        size_t n = escape_char(*p, esc);
        if (o + n >= outsz) break;
        memcpy(out + o, esc, n);
        o += n;
    }
    out[o] = '\0';
}

void cnc_json_fputs(FILE *fp, const char *s) {
    fputc('"', fp);
    for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
        char esc[8];
        size_t n = escape_char(*p, esc);
        fwrite(esc, 1, n, fp);
    }
    fputc('"', fp);
}
