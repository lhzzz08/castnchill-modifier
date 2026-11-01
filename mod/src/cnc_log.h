#ifndef CNC_LOG_H
#define CNC_LOG_H

#include <stdio.h>

void cnc_log_open(const char *path);
void cnc_log_close(void);
void cnc_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
