#ifndef ASNX_FATAL_H
#define ASNX_FATAL_H
void fatal_error(const char *fmt, ...) __attribute__((format(printf,1,2),noreturn));
#endif
