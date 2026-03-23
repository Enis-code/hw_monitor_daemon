#include "threads.h"
#include "logger.h"
#include "ring_buffer.h"
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>     // For timestamps
#include <stdint.h>   // For uint64_t in /proc parsing

// Shared synchronization primitives
// data_mutex protects ring buffer; data_cond for main loop; buffer_* for prod/cons (no busy-wait)
// running as _Atomic for std-compliant visibility in threads/signals
pthread_mutex_t data_mutex;
pthread_cond_t data_cond;
pthread_cond_t buffer_not_empty;  // Signals consumer when data available
pthread_cond_t buffer_not_full;   // Signals producers when space available
_Atomic bool running = true;

// No rand/seed needed anymore (real /proc data replaces dummy)

// Initialize synchronization primitives (incl. buffer conds + ring buffer + IPC UDS server)
// running atomic init happens at global def (true); no change needed
int threads_init(void) {
    if (pthread_mutex_init(&data_mutex, NULL) != 0) {
        return -1;
    }
    if (pthread_cond_init(&data_cond, NULL) != 0) {
        pthread_mutex_destroy(&data_mutex);
        return -1;
    }
    if (pthread_cond_init(&buffer_not_empty, NULL) != 0) {
        pthread_cond_destroy(&data_cond);
        pthread_mutex_destroy(&data_mutex);
        return -1;
    }
    if (pthread_cond_init(&buffer_not_full, NULL) != 0) {
        pthread_cond_destroy(&buffer_not_empty);
        pthread_cond_destroy(&data_cond);
        pthread_mutex_destroy(&data_mutex);
        return -1;
    }

    // Init ring buffer with shared sync primitives (for prod/cons coordination)
    // Pass &running (atomic compatible)
    if (ring_buffer_init(&g_ring_buffer, &data_mutex, &buffer_not_empty, &buffer_not_full, &running) != 0) {
        pthread_cond_destroy(&buffer_not_full);
        pthread_cond_destroy(&buffer_not_empty);
        pthread_cond_destroy(&data_cond);
        pthread_mutex_destroy(&data_mutex);
        return -1;
    }

    // Init IPC Unix domain socket server (for JSON broadcast to clients)
    if (ipc_init() != 0) {
        // Cleanup prior
        ring_buffer_cleanup(&g_ring_buffer);
        pthread_cond_destroy(&buffer_not_full);
        pthread_cond_destroy(&buffer_not_empty);
        pthread_cond_destroy(&data_cond);
        pthread_mutex_destroy(&data_mutex);
        return -1;
    }

    return 0;
}

// Cleanup synchronization primitives (incl. buffer conds + ring buffer + IPC socket)
void threads_cleanup(void) {
    ring_buffer_cleanup(&g_ring_buffer);
    ipc_cleanup();  // Closes clients, unlinks socket file
    pthread_mutex_destroy(&data_mutex);
    pthread_cond_destroy(&data_cond);
    pthread_cond_destroy(&buffer_not_empty);
    pthread_cond_destroy(&buffer_not_full);
}

// Helper: read Linux /proc/stat for real CPU utilization %
// Uses delta between calls (jiffies: user+nice+sys / total) for accurate %
// Static prev for interval; handles errors gracefully (0% fallback)
static double get_cpu_percent(void) {
    static uint64_t prev_total = 0, prev_idle = 0;
    uint64_t user, nice, sys, idle, iowait, irq, softirq, steal;
    FILE* f = fopen("/proc/stat", "r");
    if (!f) {
        LOG_WARNING("Failed to open /proc/stat");
        return 0.0;
    }
    // Parse cpu line (first): cpu user nice system idle iowait irq softirq steal ...
    // Ignore guest fields for simplicity
    if (fscanf(f, "cpu %lu %lu %lu %lu %lu %lu %lu %lu", &user, &nice, &sys, &idle, &iowait, &irq, &softirq, &steal) != 8) {
        fclose(f);
        LOG_WARNING("Failed to parse /proc/stat");
        return 0.0;
    }
    fclose(f);

    // Idle includes iowait; total all
    uint64_t idle_all = idle + iowait;
    uint64_t total = user + nice + sys + idle_all + irq + softirq + steal;

    // Delta from prev measurement
    uint64_t dtotal = total - prev_total;
    uint64_t didle = idle_all - prev_idle;

    // Update prev for next
    prev_total = total;
    prev_idle = idle_all;

    if (dtotal == 0) return 0.0;  // Avoid div0 first call
    // % util = 100 * (non-idle delta / total delta)
    return 100.0 * (dtotal - didle) / dtotal;
}

// CPU monitor thread (producer): real CPU % from /proc/stat deltas, push to ring buffer
// Uses condvar wait on full (no busy-wait); atomic_load for running; logging outside lock
// Real hw integration replaces dummy; ~every 2s delta for accurate utilization
void* cpu_monitor_thread(void* arg) {
    (void)arg;  // Unused
    static int iter = 0;  // Per-thread iteration for log throttling

    LOG_INFO("CPU monitor thread started (producer - real /proc/stat)");

    while (atomic_load(&running)) {
        double cpu_pct = get_cpu_percent();
        sensor_data_t data = {
            .timestamp = time(NULL),
            .value = cpu_pct
        };
        strncpy(data.type, "CPU", sizeof(data.type) - 1);
        data.type[sizeof(data.type) - 1] = '\0';

        // Push to ring buffer (blocks if full via not_full cond; thread-safe)
        // Crit section minimal (log outside per refactor); larger size=64 helps throughput
        if (ring_buffer_push(&g_ring_buffer, &data)) {
            if (iter % 5 == 0) {  // Throttle INFO logs (DEBUG waits still available)
                LOG_INFO("CPU producer: generated %.2f%% (real) at timestamp %ld", data.value, (long)data.timestamp);
            }
            iter++;
        } else if (!atomic_load(&running)) {
            break;  // Shutdown during wait
        }

        // Interval for sim/measure (can tune; cond ensures sync)
        sleep(2);  // Balanced with RAM/consumer + buffer size
    }

    LOG_INFO("CPU monitor thread shutting down");
    return NULL;
}

// Helper: parse Linux /proc/meminfo for real RAM utilization %
// Uses MemTotal - MemAvailable (kB) / MemTotal * 100; common accurate metric
// Handles errors gracefully (0% fallback); no dummy
static double get_ram_percent(void) {
    uint64_t total = 0, avail = 0;
    FILE* f = fopen("/proc/meminfo", "r");
    if (!f) {
        LOG_WARNING("Failed to open /proc/meminfo");
        return 0.0;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "MemTotal: %lu", &total) == 1) continue;
        if (sscanf(line, "MemAvailable: %lu", &avail) == 1) break;
    }
    fclose(f);

    if (total == 0) {
        LOG_WARNING("Failed to parse MemTotal/MemAvailable");
        return 0.0;
    }
    // % used = 100 * (total - avail) / total
    return 100.0 * (total - avail) / total;
}

// RAM monitor thread (producer): real RAM % from /proc/meminfo, push to ring buffer
// Demonstrates synchronized prod/cons with same buffer/conds
// Opt: throttling as CPU; atomic_load for _Atomic running
// Real hw: MemUsed% replaces dummy; interval for varied prod
void* ram_monitor_thread(void* arg) {
    (void)arg;  // Unused
    static int iter = 0;  // Per-thread for log throttling

    LOG_INFO("RAM monitor thread started (producer - real /proc/meminfo)");

    while (atomic_load(&running)) {
        double ram_pct = get_ram_percent();
        sensor_data_t data = {
            .timestamp = time(NULL),
            .value = ram_pct
        };
        strncpy(data.type, "RAM", sizeof(data.type) - 1);
        data.type[sizeof(data.type) - 1] = '\0';

        // Push to ring buffer (blocks if full; no busy-wait)
        if (ring_buffer_push(&g_ring_buffer, &data)) {
            if (iter % 5 == 0) {  // Throttle to minimize logger mutex contention
                LOG_INFO("RAM producer: generated %.2f%% (real) at timestamp %ld", data.value, (long)data.timestamp);
            }
            iter++;
        } else if (!atomic_load(&running)) {
            break;
        }

        sleep(3);  // Slightly different interval for varied prod
    }

    LOG_INFO("RAM monitor thread shutting down");
    return NULL;
}

// Consumer thread: pops from ring buffer and logs sensor data
// Blocks on empty via not_empty condvar (avoids busy-wait); uses logger for events
// atomic_load for _Atomic running
void* consumer_thread(void* arg) {
    (void)arg;  // Unused

    LOG_INFO("Consumer thread started (logs ring buffer data)");

    while (atomic_load(&running)) {
        sensor_data_t data;

        // Pop from buffer (blocks if empty; thread-safe w/ cond)
        if (ring_buffer_pop(&g_ring_buffer, &data)) {
            // Save to logger (fulfills req; uses convenience macro from logger.h)
            LOG_SENSOR(data.type, data.value, data.timestamp);

            // Format to JSON + broadcast to UDS clients (IPC)
            // Manual JSON no deps; sends live data
            ipc_broadcast_sensor(&data);
        } else if (!atomic_load(&running)) {
            break;  // Shutdown signal
        }
    }

    LOG_INFO("Consumer thread shutting down");
    return NULL;
}

