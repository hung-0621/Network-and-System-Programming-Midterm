#ifndef LOGGER_H
#define LOGGER_H

#include <stdio.h>
#include <unistd.h>

typedef enum {
    LOG_LVL_ERROR = 0,
    LOG_LVL_WARN  = 1,
    LOG_LVL_INFO  = 2,
    LOG_LVL_DEBUG = 3
} LogLevel;

void log_set_level(LogLevel level);
LogLevel log_get_level(void);
void log_message(LogLevel level, const char *file, int line, const char *fmt, ...);

#define LOG_ERROR(fmt, ...) log_message(LOG_LVL_ERROR, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  log_message(LOG_LVL_WARN,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  log_message(LOG_LVL_INFO,  __FILE__, __LINE__, fmt, ##__VA_ARGS__)

// Compile-time debug log control
#ifdef DEBUG_ENABLED
#define LOG_DEBUG(fmt, ...) log_message(LOG_LVL_DEBUG, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#else
#define LOG_DEBUG(fmt, ...) do {} while (0)
#endif

#endif // LOGGER_H
