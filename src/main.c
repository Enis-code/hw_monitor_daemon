#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <time.h>  // For clock_gettime
#include <pthread.h>  // For thread management in daemon
#include <stdbool.h>  // For bool type
#include <stdatomic.h>  // For _Atomic bool (running flag)

#include "logger.h"
#include "threads.h"

// Global variables for daemon control (running, sync primitives declared in threads.c)
// Extended for producer-consumer + UDS IPC server: 4 threads + ring buffer + JSON socket broadcast
pthread_t cpu_thread;       // Producer: real CPU % from /proc/stat
pthread_t ram_thread;       // Producer: real RAM % from /proc/meminfo
pthread_t consumer_tid;     // Consumer: pops, logs, formats JSON + broadcasts to socket clients
pthread_t ipc_tid;          // IPC accept thread for Unix domain socket server

// Signal handler for graceful shutdown
// Sets flag + broadcasts ALL conds (data + buffer not_empty/full) to wake producers/consumers
// Prevents deadlock on waits; ensures graceful (no busy-wait even during stop)
// atomic_store ensures memory visibility to waiting threads
static void signal_handler(int sig) {
    (void)sig;  // Unused
    LOG_INFO("Received shutdown signal, initiating graceful shutdown...");
    atomic_store(&running, false);

    // Wake up any waiting threads across the producer-consumer pipeline
    pthread_cond_broadcast(&data_cond);
    pthread_cond_broadcast(&buffer_not_empty);  // Wake consumer
    pthread_cond_broadcast(&buffer_not_full);   // Wake producers
}

// Daemonize the process: fork, setsid, chdir, umask, close fds
static int daemonize(void) {
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid > 0) {
        // Parent exits
        exit(EXIT_SUCCESS);
    }

    // Create new session
    if (setsid() < 0) {
        return -1;
    }

    // Fork again to ensure not session leader
    pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid > 0) {
        exit(EXIT_SUCCESS);
    }

    // Change to root directory
    if (chdir("/") < 0) {
        return -1;
    }

    // Set umask
    umask(0);

    // Close standard file descriptors
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    return 0;
}

// Setup signal handlers
static void setup_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);  // For potential reload

    // Ignore SIGPIPE
    signal(SIGPIPE, SIG_IGN);
}

int main(void) {
    // Daemonize FIRST to ensure no terminal output duplication from fork processes
    // This is standard production practice for daemons to avoid multi-process artifacts
    if (daemonize() != 0) {
        fprintf(stderr, "Failed to daemonize\n");
        return EXIT_FAILURE;
    }

    // All initializations now happen ONLY in the final detached daemon child process
    // Thread sync first (pure memory ops) -- this now also inits ring buffer internally
    if (threads_init() != 0) {
        // Can't log yet; stderr closed by daemonize, use syslog fallback if needed
        // For framework simplicity, exit (in practice, would openlog/syslog)
        return EXIT_FAILURE;
    }

    // Initialize logger (absolute path safe post-chdir('/'))
    // Set to DEBUG level to capture buffer wait/push/pop details (no busy-wait visible)
    // Basic logging module for saving system events - thread-safe
    // Ring buffer data (CPU/RAM %) will be logged here via consumer
    if (logger_init("/tmp/hw_monitor.log", LOG_DEBUG) != 0) {
        // Fallback not implemented to keep deps minimal
        threads_cleanup();
        return EXIT_FAILURE;
    }
    LOG_INFO("Logger initialized (DEBUG level for buffer ops). Hardware monitor daemon started. PID: %d", getpid());

    // Setup signals AFTER daemonization for proper handling in background
    setup_signals();
    LOG_INFO("Signal handlers setup for graceful shutdown");

    // Start the monitor threads: demonstrates synchronized multithread design
    // from the start (producers push real /proc data to ring buffer;
    // consumer_thread pops, logs, JSON-formats + broadcasts to UDS clients;
    // all use condvars/mutex/atomic + logs outside locks)
    // (Thread-internal logging avoids duplication with launcher)
    if (pthread_create(&cpu_thread, NULL, cpu_monitor_thread, NULL) != 0) {
        LOG_FATAL("Failed to create CPU monitor thread");
        threads_cleanup();
        logger_cleanup();
        return EXIT_FAILURE;
    }

    if (pthread_create(&ram_thread, NULL, ram_monitor_thread, NULL) != 0) {
        LOG_FATAL("Failed to create RAM monitor thread");
        atomic_store(&running, false);  // Use atomic for visibility
        pthread_join(cpu_thread, NULL);
        threads_cleanup();
        logger_cleanup();
        return EXIT_FAILURE;
    }

    // Start consumer thread (completes prod-cons pipeline for ring buffer + JSON IPC)
    // Note: consumer_tid var avoids name collision with consumer_thread func
    // (start log is internal to thread func to avoid dupes)
    if (pthread_create(&consumer_tid, NULL, consumer_thread, NULL) != 0) {
        LOG_FATAL("Failed to create consumer thread");
        atomic_store(&running, false);  // Use atomic for visibility
        pthread_join(cpu_thread, NULL);
        pthread_join(ram_thread, NULL);
        threads_cleanup();
        logger_cleanup();
        return EXIT_FAILURE;
    }

    // Start IPC accept thread for Unix domain socket server (handles client connections)
    // Enables live JSON broadcast; non-block UDS for IPC
    if (pthread_create(&ipc_tid, NULL, ipc_accept_thread, NULL) != 0) {
        LOG_FATAL("Failed to create IPC accept thread");
        atomic_store(&running, false);
        pthread_join(cpu_thread, NULL);
        pthread_join(ram_thread, NULL);
        pthread_join(consumer_tid, NULL);
        threads_cleanup();
        logger_cleanup();
        return EXIT_FAILURE;
    }
    LOG_INFO("IPC socket server thread started");

    // Main daemon loop: essential for keeping daemon alive
    // Uses cond_timedwait for responsive signal handling & low CPU
    // (Buffer prod/cons run independently in their threads)
    // while uses atomic_load for _Atomic running flag
    LOG_INFO("Entering main daemon loop");
    while (atomic_load(&running)) {
        // TODO: Add periodic system health checks or event processing here
        // Framework placeholder for advanced monitoring orchestration
        pthread_mutex_lock(&data_mutex);
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;  // Check every 1 second
        pthread_cond_timedwait(&data_cond, &data_mutex, &ts);
        pthread_mutex_unlock(&data_mutex);
    }
    LOG_INFO("Exiting main daemon loop");

    // Graceful shutdown sequence for producer-consumer + IPC socket server:
    // 1. Signal handler sets atomic running=false & broadcasts ALL conds (data + buffer_*)
    // 2. Producers/consumers/accept wake, exit loops (no deadlock on waits)
    // 3. Join all threads for sync completion
    // 4. Cleanup resources (incl. ring buffer, IPC: close fds + unlink socket) - no leaks/stale files
    LOG_INFO("Waiting for monitor, consumer, and IPC threads to finish...");
    pthread_join(cpu_thread, NULL);
    pthread_join(ram_thread, NULL);
    pthread_join(consumer_tid, NULL);
    pthread_join(ipc_tid, NULL);  // IPC accept thread
    LOG_INFO("All threads joined");

    // Cleanup: logger LAST so final shutdown message is logged
    threads_cleanup();  // Includes ipc_cleanup (unlink socket)
    LOG_INFO("Daemon shutdown complete. Resources cleaned up.");
    logger_cleanup();  // Close log file after last write

    return EXIT_SUCCESS;
}
