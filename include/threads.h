#ifndef THREADS_H
#define THREADS_H

#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>  // For _Atomic bool (std thread-safety vs volatile)
#include "ring_buffer.h"  // For shared sensor data buffer (CPU/RAM)
#include "ipc.h"  // For Unix domain socket IPC server + JSON broadcast

// Shared synchronization primitives for multithreaded design
// Producers (CPU/RAM) + Consumer use these to coordinate ring buffer w/o busy-waiting
extern pthread_mutex_t data_mutex;   // Protects shared data & ring buffer
extern pthread_cond_t data_cond;     // For main loop/other signaling
extern pthread_cond_t buffer_not_empty;  // Consumer waits here if buffer empty
extern pthread_cond_t buffer_not_full;   // Producers wait here if buffer full
extern _Atomic bool running;         // Atomic flag for graceful shutdown (std C11; ensures visibility across threads/signals vs volatile)

// Thread function templates (placeholders for monitor threads)
// These demonstrate the synchronized multithread design from the start.
// Now extended with producer/consumer pattern using ring buffer.

// CPU monitor thread (producer: generates dummy CPU % and pushes to ring buffer)
void* cpu_monitor_thread(void* arg);

// RAM monitor thread (producer: generates dummy RAM % and pushes to ring buffer)
void* ram_monitor_thread(void* arg);

// Consumer thread (reads from ring buffer and logs via logger module)
void* consumer_thread(void* arg);

// Helper to initialize synchronization primitives (incl. buffer conds)
int threads_init(void);

// Helper to cleanup synchronization primitives (incl. buffer conds)
void threads_cleanup(void);

#endif // THREADS_H

