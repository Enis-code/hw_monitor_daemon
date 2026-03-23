#include "ring_buffer.h"
#include "logger.h"
#include <string.h>
#include <stdio.h>

// Global ring buffer instance
ring_buffer_t g_ring_buffer = {0};

// Initialize ring buffer (sets up indices; uses injected sync primitives)
// running injected as _Atomic for std thread-safety
int ring_buffer_init(ring_buffer_t *rb, pthread_mutex_t *mutex, pthread_cond_t *not_empty, pthread_cond_t *not_full, _Atomic bool *running) {
    if (rb == NULL || mutex == NULL || not_empty == NULL || not_full == NULL || running == NULL) {
        return -1;
    }

    memset(rb->buffer, 0, sizeof(rb->buffer));
    rb->head = 0;
    rb->tail = 0;
    rb->count = 0;
    rb->mutex = mutex;
    rb->not_empty = not_empty;
    rb->not_full = not_full;
    rb->running = running;

    LOG_INFO("Ring buffer initialized (size: %d)", RING_BUFFER_SIZE);
    return 0;
}

// Cleanup (no dynamic alloc, just log for framework)
void ring_buffer_cleanup(ring_buffer_t *rb) {
    (void)rb;  // Unused; placeholder for future dynamic mem
    LOG_INFO("Ring buffer cleaned up");
}

// Push data (producer: CPU/RAM threads) - blocks on full using condvar (no busy-wait)
// Logging refactored OUTSIDE mutex (copy data, unlock, then log) to reduce contention/IO under lock
// Critical section minimal; atomic_load for running flag (std C11 safety)
bool ring_buffer_push(ring_buffer_t *rb, const sensor_data_t *data) {
    if (rb == NULL || data == NULL) {
        return false;
    }

    bool success = false;
    sensor_data_t copy = {0};  // Copy for logging outside lock

    pthread_mutex_lock(rb->mutex);

    // Wait while buffer full AND still running (avoids busy-wait, responsive to shutdown)
    // atomic_load ensures visibility/ordering from signal handler
    while (rb->count == RING_BUFFER_SIZE && atomic_load(rb->running)) {
        LOG_DEBUG("Buffer full, producer waiting...");
        pthread_cond_wait(rb->not_full, rb->mutex);
    }

    // Check shutdown during wait (prevents deadlock on stop)
    if (!atomic_load(rb->running)) {
        pthread_mutex_unlock(rb->mutex);
        return false;
    }

    // Insert at head (circular) - minimal crit section
    rb->buffer[rb->head] = *data;
    rb->head = (rb->head + 1) % RING_BUFFER_SIZE;
    rb->count++;
    copy = *data;  // Copy for post-unlock logging
    int count_copy = rb->count;  // Copy count to local (fix data race; read inside lock)
    success = true;

    // Signal consumer that buffer not empty
    pthread_cond_signal(rb->not_empty);
    pthread_mutex_unlock(rb->mutex);

    // Logging OUTSIDE lock (fulfills struct improvement; avoids blocking producers/consumers on IO/mutex)
    // Use local count_copy to prevent race on rb->count
    if (success) {
        LOG_DEBUG("Pushed to buffer: type=%s, value=%.2f%% (count=%d)", copy.type, copy.value, count_copy);
    }

    return true;
}

// Pop data (consumer thread) - blocks on empty using condvar (no busy-wait)
// Logging refactored OUTSIDE mutex (copy data, unlock, then log) to reduce contention/IO under lock
// Critical section minimal; atomic_load for running flag (C11 std)
bool ring_buffer_pop(ring_buffer_t *rb, sensor_data_t *data) {
    if (rb == NULL || data == NULL) {
        return false;
    }

    bool success = false;
    sensor_data_t copy = {0};  // Copy for logging outside lock

    pthread_mutex_lock(rb->mutex);

    // Wait while buffer empty AND still running (avoids busy-wait)
    // atomic_load ensures visibility from signal handler across threads
    while (rb->count == 0 && atomic_load(rb->running)) {
        LOG_DEBUG("Buffer empty, consumer waiting...");
        pthread_cond_wait(rb->not_empty, rb->mutex);
    }

    // Check shutdown during wait
    if (!atomic_load(rb->running)) {
        pthread_mutex_unlock(rb->mutex);
        return false;
    }

    // Extract from tail (circular) - minimal crit section
    *data = rb->buffer[rb->tail];
    rb->tail = (rb->tail + 1) % RING_BUFFER_SIZE;
    rb->count--;
    copy = *data;  // Copy for post-unlock
    int count_copy = rb->count;  // Copy count to local (fix data race; read inside lock)
    success = true;

    // Signal producers that buffer not full
    pthread_cond_signal(rb->not_full);
    pthread_mutex_unlock(rb->mutex);

    // Logging OUTSIDE lock (fulfills struct improvement; avoids blocking on IO/mutex)
    // (Note: consumer thread's main sensor log is already outside this)
    // Use local count_copy to prevent race on rb->count
    if (success) {
        LOG_DEBUG("Popped from buffer: type=%s, value=%.2f%% (count=%d)", copy.type, copy.value, count_copy);
    }

    return true;
}

// Get current size (thread-safe)
int ring_buffer_size(ring_buffer_t *rb) {
    if (rb == NULL) {
        return 0;
    }
    pthread_mutex_lock(rb->mutex);
    int size = rb->count;
    pthread_mutex_unlock(rb->mutex);
    return size;
}
