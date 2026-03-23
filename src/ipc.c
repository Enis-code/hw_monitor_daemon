#define _XOPEN_SOURCE 500  // Enable usleep (POSIX) with -std=c11 strict
#include "ipc.h"
#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>  // For non-blocking
#include <errno.h>
#include <sys/select.h>  // For accept/select in thread

// Global IPC server instance
ipc_server_t g_ipc_server = {0};

// Init Unix domain socket server: create, bind, listen
// Non-blocking for daemon integration; path /tmp/hw_monitor.sock
int ipc_init(void) {
    // Cleanup any stale socket file
    unlink(SOCKET_PATH);

    g_ipc_server.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_ipc_server.server_fd < 0) {
        LOG_ERROR("Socket creation failed");
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    if (bind(g_ipc_server.server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("Socket bind failed");
        close(g_ipc_server.server_fd);
        return -1;
    }

    if (listen(g_ipc_server.server_fd, 5) < 0) {
        LOG_ERROR("Socket listen failed");
        close(g_ipc_server.server_fd);
        return -1;
    }

    // Non-blocking for accept thread
    fcntl(g_ipc_server.server_fd, F_SETFL, O_NONBLOCK);

    // Init client list + mutex
    memset(g_ipc_server.client_fds, -1, sizeof(g_ipc_server.client_fds));
    if (pthread_mutex_init(&g_ipc_server.clients_mutex, NULL) != 0) {
        LOG_ERROR("Clients mutex init failed");
        close(g_ipc_server.server_fd);
        return -1;
    }

    LOG_INFO("Unix domain socket server initialized at %s", SOCKET_PATH);
    return 0;
}

// Cleanup: close fds, unlink socket file (secure on SIGTERM)
void ipc_cleanup(void) {
    pthread_mutex_lock(&g_ipc_server.clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_ipc_server.client_fds[i] != -1) {
            close(g_ipc_server.client_fds[i]);
            g_ipc_server.client_fds[i] = -1;
        }
    }
    pthread_mutex_unlock(&g_ipc_server.clients_mutex);
    pthread_mutex_destroy(&g_ipc_server.clients_mutex);

    if (g_ipc_server.server_fd != -1) {
        close(g_ipc_server.server_fd);
        g_ipc_server.server_fd = -1;
    }

    // Remove socket file to avoid stale on restart/shutdown
    if (unlink(SOCKET_PATH) == 0) {
        LOG_INFO("Socket file %s cleaned up", SOCKET_PATH);
    } else {
        LOG_WARNING("Socket cleanup unlink failed (may not exist)");
    }
}

// Add client fd to pool (thread-safe)
int ipc_add_client(int client_fd) {
    pthread_mutex_lock(&g_ipc_server.clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_ipc_server.client_fds[i] == -1) {
            g_ipc_server.client_fds[i] = client_fd;
            fcntl(client_fd, F_SETFL, O_NONBLOCK);  // Non-block for broadcast
            pthread_mutex_unlock(&g_ipc_server.clients_mutex);
            LOG_INFO("Client connected (fd=%d)", client_fd);
            return 0;
        }
    }
    pthread_mutex_unlock(&g_ipc_server.clients_mutex);
    LOG_WARNING("Max clients reached, rejecting fd=%d", client_fd);
    close(client_fd);
    return -1;
}

// Broadcast JSON msg to all connected clients (thread-safe, remove dead)
void ipc_broadcast(const char* json_msg) {
    if (!json_msg) return;

    pthread_mutex_lock(&g_ipc_server.clients_mutex);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        int fd = g_ipc_server.client_fds[i];
        if (fd != -1) {
            ssize_t sent = send(fd, json_msg, strlen(json_msg), 0);
            if (sent < 0 && (errno == EPIPE || errno == ECONNRESET)) {
                // Client gone; clean
                close(fd);
                g_ipc_server.client_fds[i] = -1;
                LOG_INFO("Client disconnected (fd=%d)", fd);
            }
        }
    }
    pthread_mutex_unlock(&g_ipc_server.clients_mutex);
}

// Manual JSON formatting (no external deps: simple sprintf for per-sensor item)
// Format: {"type":"CPU","value":12.34,"ts":1234567890}\n
// Called by consumer; keeps lightweight for IPC
void ipc_broadcast_sensor(const sensor_data_t* data) {
    if (!data) return;

    char json_buf[128];  // Fixed buf sufficient for simple JSON
    snprintf(json_buf, sizeof(json_buf),
             "{\"type\":\"%s\",\"value\":%.2f,\"ts\":%ld}\n",
             data->type, data->value, (long)data->timestamp);

    // Broadcast the JSON line to clients
    ipc_broadcast(json_buf);
    LOG_DEBUG("Broadcasted JSON: %s", json_buf);  // Debug only
}

// Accept thread: polls server_fd for new clients (non-block, integrates with daemon shutdown)
// Uses atomic running for loop
void* ipc_accept_thread(void* arg) {
    (void)arg;
    LOG_INFO("IPC accept thread started");

    while (atomic_load(&running)) {
        struct sockaddr_un client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(g_ipc_server.server_fd, (struct sockaddr*)&client_addr, &addr_len);
        if (client_fd >= 0) {
            ipc_add_client(client_fd);
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            // Real error (ignore non-block)
            LOG_DEBUG("Accept error: %s", strerror(errno));
        }
        // Short sleep/poll to avoid busy (simple; could use select/epoll for prod)
        usleep(100000);  // 0.1s
    }

    LOG_INFO("IPC accept thread shutting down");
    return NULL;
}