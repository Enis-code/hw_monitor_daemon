#ifndef LOGGER_H
#define LOGGER_H

#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>

// Log levels
typedef enum {
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARNING,
    LOG_ERROR,
    LOG_FATAL
} log_level_t;

// Logger configuration
typedef struct {
    FILE* log_file;
    log_level_t min_level;
    pthread_mutex_t mutex;  // For thread safety
} logger_t;

// Global logger instance
extern logger_t g_logger;

// Function declarations
int logger_init(const char* filename, log_level_t min_level);
void logger_cleanup(void);
void logger_log(log_level_t level, const char* format, ...);

// Convenience macros
#define LOG_DEBUG(fmt, ...) logger_log(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  logger_log(LOG_INFO, fmt, ##__VA_ARGS__)
#define LOG_WARNING(fmt, ...) logger_log(LOG_WARNING, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) logger_log(LOG_ERROR, fmt, ##__VA_ARGS__)
#define LOG_FATAL(fmt, ...) logger_log(LOG_FATAL, fmt, ##__VA_ARGS__)

// Convenience for sensor data (used by consumer)
#define LOG_SENSOR(type, value, ts) LOG_INFO("Sensor data: %s=%.2f%% (ts=%ld)", type, value, (long)ts)

#endif // LOGGER_H

