#include "config.h"
#include "crash_log.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

static FILE *g_crash_log;
static int g_crash_header_written;

static FILE *open_crash_log(void) {
    if (!g_crash_log) g_crash_log = fopen(LOG_PATH, "w");
    if (g_crash_log && !g_crash_header_written) {
        time_t t = time(NULL);
        fprintf(g_crash_log, "---- angrybirdas_nx crash %lld ----\n", (long long)t);
        g_crash_header_written = 1;
    }
    return g_crash_log;
}

void crash_log_fatal_printf(const char *fmt, ...) {
    FILE *f = open_crash_log();
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fflush(f);
}

void crash_log_flush(void) {
    if (g_crash_log) fflush(g_crash_log);
}

void crash_log_close(void) {
    if (!g_crash_log) return;
    fflush(g_crash_log);
    fclose(g_crash_log);
    g_crash_log = NULL;
    g_crash_header_written = 0;
}
