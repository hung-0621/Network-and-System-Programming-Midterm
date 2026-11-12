#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <string.h>

static LogLevel g_current_log_level = LOG_LVL_INFO;

void log_set_level(LogLevel level) {
    if (level < LOG_LVL_ERROR) level = LOG_LVL_ERROR;
    if (level > LOG_LVL_DEBUG) level = LOG_LVL_DEBUG;
    g_current_log_level = level;
}

LogLevel log_get_level(void) {
    return g_current_log_level;
}

void log_message(LogLevel level, const char *file, int line, const char *fmt, ...) {
    if (level > g_current_log_level) {
        return;
    }

    const char *color_prefix = "";
    const char *color_suffix = "\033[0m";
    const char *level_str = "INFO";

    switch (level) {
        case LOG_LVL_ERROR:
            color_prefix = "\033[1;31m"; // Bold Red
            level_str = "ERROR";
            break;
        case LOG_LVL_WARN:
            color_prefix = "\033[1;33m"; // Bold Yellow
            level_str = "WARN ";
            break;
        case LOG_LVL_INFO:
            color_prefix = "\033[1;32m"; // Bold Green
            level_str = "INFO ";
            break;
        case LOG_LVL_DEBUG:
            color_prefix = "\033[1;36m"; // Bold Cyan
            level_str = "DEBUG";
            break;
    }

    // Get timestamp
    time_t now = time(NULL);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    char time_str[32];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &tm_buf);

    // Basename of file
    const char *base_file = strrchr(file, '/');
    if (base_file) {
        base_file++;
    } else {
        base_file = file;
    }

    // Format output
    FILE *out = (level == LOG_LVL_ERROR) ? stderr : stdout;

    fprintf(out, "%s[%s] [%s] [PID:%d] (%s:%d) ",
            color_prefix, time_str, level_str, (int)getpid(), base_file, line);

    va_list args;
    va_start(args, fmt);
    vfprintf(out, fmt, args);
    va_end(args);

    fprintf(out, "%s\n", color_suffix);
    fflush(out);
}
