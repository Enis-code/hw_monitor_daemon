#ifndef IPC_H
#define IPC_H

#include <sys/socket.h>
#include <sys/un.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>  // For atomic running flag compat
#include "ring_buffer.h"  // For sensor_data_t in broadcast

// Extern from threads.h for atomic running flag (avoid include cycle)
extern _Atomic bool running;

// Unix Domain Socket server for IPC
// Listens on /tmp/hw_monitor.sock ; broadcasts JSON to clients
#define SOCKET_PATH "/tmp/hw_monitor.sock"
#define MAX_CLIENTS 10  // Simple fixed for framework

// IPC state (thread-safe basics)
typedef struct {
    int server_fd;               // Listen socket
    int client_fds[MAX_CLIENTS]; // Connected clients
    pthread_mutex_t clients_mutex;  // Protects client list
} ipc_server_t;

extern ipc_server_t g_ipc_server;

// Function declarations
int ipc_init(void);  // Create/bind/listen UDS server
void ipc_cleanup(void);  // Close fds + unlink socket file for secure shutdown
void ipc_broadcast(const char* json_msg);  // Send JSON to all connected clients
void ipc_broadcast_sensor(const sensor_data_t* data);  // Format sensor to JSON + broadcast
void* ipc_accept_thread(void* arg);  // Thread to accept new clients (non-blocking simple)

// Helper to add client fd (internal)
int ipc_add_client(int client_fd);

#endif // IPC_H