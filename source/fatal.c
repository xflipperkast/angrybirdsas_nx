#include "fatal.h"
#include "crash_log.h"
#include <switch.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

void fatal_error(const char *fmt, ...) {
    char msg[2048];
    va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof(msg), fmt, ap); va_end(ap);
    crash_log_printf("FATAL: %s\n", msg);
    crash_log_flush();
    consoleInit(NULL);
    consoleClear();
    printf("angrybirdas_nx\n\n%s\n\nPress + to exit.\n", msg);
    consoleUpdate(NULL);
    PadState pad; padConfigureInput(1, HidNpadStyleSet_NpadStandard); padInitializeDefault(&pad);
    while (appletMainLoop()) { padUpdate(&pad); if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break; svcSleepThread(16000000); }
    consoleExit(NULL);
    crash_log_close();
    exit(1);
}
