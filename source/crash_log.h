#pragma once

/*
 * v0.1.75 crash-only logger.
 *
 * Routine diagnostics are intentionally compiled out.  This keeps normal
 * sessions from creating/writing angrybirdas_crash.log and removes SD-card
 * logging from hot paths.  Only fatal_error() opens the crash log.
 */
void crash_log_fatal_printf(const char *fmt, ...) __attribute__((format(printf,1,2)));
void crash_log_flush(void);
void crash_log_close(void);

#define crash_log_printf(...)  ((void)0)
#define crash_log_vprintf(...) ((void)0)
