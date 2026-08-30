#ifndef ASNX_TRACE_LOG_H
#define ASNX_TRACE_LOG_H

#include <stdint.h>

int trace_log_init(void);
void trace_log_printf(const char *category, const char *fmt, ...) __attribute__((format(printf,2,3)));
void trace_log_pump(void);
void trace_log_flush(void);
void trace_log_close(void);
void trace_main_thread_set(void);
int trace_is_main_thread(void);
void trace_main_block_begin(const char *kind, const void *key, const char *detail);
void trace_main_block_end(void);
void trace_main_block_snapshot(const char *reason);
uint64_t trace_now_us(void);

#endif
