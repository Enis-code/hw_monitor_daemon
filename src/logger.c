#include "logger.h"
#include <stdlib.h>
#include <string.h>

// Global logger instance
logger_t g_logger = {0};

static const char* level_strings[] = {
    "DEBUG",
    "INFO",
    "WARNING",
    "ERROR",
    "FATAL"
};

int logger_init(const char* filename, log_level_t min_level) {
    if (filename == NULL) {
        return -1;
    }

    // Initialize mutex for thread safety
    if (pthread_mutex_init(&g_logger.mutex, NULL) != 0) {
        return -1;
    }

    g_logger.log_file = fopen(filename, "a");
    if (g_logger.log_file == NULL) {
        pthread_mutex_destroy(&g_logger.mutex);
        return -1;
    }

    // Perf opt: line-buffered IO (reduces syscalls vs unbuf; fflush only on \n)
    // Better than full fflush every log for daemon high-volume events
    // Keeps durability for logs while cutting IO overhead
    // Note: use fprintf for warning to avoid early-log recursion
    if (setvbuf(g_logger.log_file, NULL, _IOLBF, 0) != 0) {
        // Fallback ok, non-fatal
        fprintf(g_logger.log_file, "[WARNING] setvbuf failed, using default buffering\n");
    }

    g_logger.min_level = min_level;
    return 0;
}

void logger_cleanup(void) {
    if (g_logger.log_file != NULL) {
        // Explicit flush on shutdown for log durability (pairs with line-buf opt)
        fflush(g_logger.log_file);
        fclose(g_logger.log_file);
        g_logger.log_file = NULL;
    }
    pthread_mutex_destroy(&g_logger.mutex);
}

void logger_log(log_level_t level, const char* format, ...) {
    if (level < g_logger.min_level || g_logger.log_file == NULL) {
        return;
    }

    pthread_mutex_lock(&g_logger.mutex);

    // Get current time
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    char time_buf[20];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);

    // Log header
    fprintf(g_logger.log_file, "[%s] [%s] ", time_buf, level_strings[level]);

    // Log message
    va_list args;
    va_start(args, format);
    vfprintf(g_logger.log_file, format, args);
    va_end(args);

    fprintf(g_logger.log_file, "\n");
    // No fflush per log (perf opt: rely on _IOLBF from init for line-buf)
    // Flushes handled by libc on \n or explicit in cleanup/shutdown
    // Reduces syscall overhead significantly for frequent events

    pthread_mutex_unlock(&g_logger.mutex);
}
