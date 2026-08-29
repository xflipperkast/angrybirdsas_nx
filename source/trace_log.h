#ifndef ASNX_TRACE_LOG_H
#define ASNX_TRACE_LOG_H

#include <stdint.h>

int trace_log_init(void);
void trace_log_printf(const char *category, const char *fmt, ...) __attribute__((format(printf,2,3)));
void trace_log_pump(void);
void trace_log_flush(void);
void trace_log_close(void);
uint64_t trace_now_us(void);

#endif
