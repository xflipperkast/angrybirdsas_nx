#pragma once
#include <stdarg.h>

void crash_log_printf(const char *fmt, ...) __attribute__((format(printf,1,2)));
void crash_log_vprintf(const char *fmt, va_list ap);
void crash_log_flush(void);
void crash_log_close(void);
