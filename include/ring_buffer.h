#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <time.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>  // For _Atomic bool param (thread-safety)

// Sensor data item for CPU/RAM monitoring
typedef struct {
    time_t timestamp;   // When data was captured
    char type[8];       // "CPU" or "RAM"
    double value;       // Percentage value (0-100 dummy for now)
} sensor_data_t;

// Pre-sized ring buffer (circular queue) config - optimized size to reduce full/empty waits/context switches
#define RING_BUFFER_SIZE 64  // Increased for better throughput; still fixed/small mem; power-of-2 friendly

// Ring buffer structure (thread-safe with mutex/cond)
typedef struct {
    sensor_data_t buffer[RING_BUFFER_SIZE];  // Circular array
    int head;                                // Write index
    int tail;                                // Read index
    int count;                               // Current items (for full/empty check)
    pthread_mutex_t *mutex;                  // Shared/reused from threads
    pthread_cond_t *not_empty;               // Wait for data to consume
    pthread_cond_t *not_full;                // Wait for space to produce
    _Atomic bool *running;                   // Shared atomic shutdown flag (C11 std)
} ring_buffer_t;

// Global ring buffer instance (for simplicity in framework)
extern ring_buffer_t g_ring_buffer;

// Function declarations
// running param as _Atomic bool* for std-compliant shutdown signaling across threads
int ring_buffer_init(ring_buffer_t *rb, pthread_mutex_t *mutex, pthread_cond_t *not_empty, pthread_cond_t *not_full, _Atomic bool *running);
void ring_buffer_cleanup(ring_buffer_t *rb);
bool ring_buffer_push(ring_buffer_t *rb, const sensor_data_t *data);  // Producer: blocks if full (no busy-wait)
bool ring_buffer_pop(ring_buffer_t *rb, sensor_data_t *data);         // Consumer: blocks if empty (no busy-wait)
int ring_buffer_size(ring_buffer_t *rb);  // Current count (for debug)

#endif // RING_BUFFER_H
