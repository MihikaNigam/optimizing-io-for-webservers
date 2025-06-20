#include "request-handler.h"

#include <pthread.h>
#include <sys/time.h>

#define MAX_PENDING_ACCEPTS 2048

void *handle_client(void *arg)
{
    int client_socket = *(int *)arg;
    free(arg); // Free the allocated memory for the socket descriptor

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(1, &cpuset); // Pin to core 0

    if (pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == -1)
    {
        perror("pthread_setaffinity_np");
        // Handle error
    }
    else
    {
        printf("Thread %lu pinned to core %d.\n", (unsigned long)pthread_self(), 1);
    }

    int file_fd;
    char *req_buffer;
    if (posix_memalign((void **)&req_buffer, MY_BLOCK_SIZE, BUFFER_SIZE) != 0)
    {
        perror("Failed to allocate aligned buffer");
        close(client_socket);
        return NULL;
    }
    memset(req_buffer, 0, BUFFER_SIZE);
    int res = handle_blocking_requests(client_socket, &file_fd, req_buffer);
    if (res == CONN_ERROR)
    {
        send_response(client_socket, "HTTP/1.1 500 Internal Server Error", "text/plain", "Internal Server Error");
    }
    close(file_fd);
    free(req_buffer);
    close(client_socket);
    return NULL;
}

int main()
{
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(1, &cpuset); // Pin to core 0

    if (sched_setaffinity(getpid(), sizeof(cpu_set_t), &cpuset) == -1)
    {
        perror("sched_setaffinity");
        // Handle error
    }
    else
    {
        printf("Process %d pinned to core 1.\n", getpid());
    }

    int server_socket;
    struct sockaddr_in server_addr;

    // CREATE
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0)
    {
        perror("Error creating socket");
        return 1;
    }

    int enable = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEPORT, &enable, sizeof(enable));

    // CONFIGURE & BIND
    memset(&server_addr, 0, sizeof(server_addr)); // Zero out the structure
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY; // Listen on all interfaces
    server_addr.sin_port = htons(SERVER_PORT);
    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        perror("Error binding socket");
        close(server_socket);
        return 1;
    }

    // LISTEN
    if (listen(server_socket, ACCEPT_BACKLOG) < 0)
    {
        perror("Error listening on socket");
        return 1;
    }

    printf("Server listening on port %d\n", SERVER_PORT);

    // ALLOW
    while (1)
    {
        // Phase 1: Accept multiple connections quickly
        int accepted_sockets[MAX_PENDING_ACCEPTS];
        int accept_count = 0;

        while (accept_count < MAX_PENDING_ACCEPTS)
        {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_len);
            if (client_socket < 0)
            {
                if (errno == EWOULDBLOCK || errno == EAGAIN)
                {
                    // No more pending connections
                    break;
                }
                perror("Error accepting connection");
                continue;
            }

            // printf("Connection accepted from %s:%d\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
            accepted_sockets[accept_count++] = client_socket;
        }
        // Phase 2: Handle accepted connections
        for (int i = 0; i < accept_count; i++)
        {
            int *pclient = malloc(sizeof(int));
            if (pclient == NULL)
            {
                perror("Failed to allocate memory for client socket");
                close(client_socket);
                continue;
            }
            *pclient = accepted_sockets[i];

            pthread_t tid;
            if (pthread_create(&tid, NULL, handle_client, pclient) != 0)
            {
                perror("pthread_create");
                free(pclient);
                close(accepted_sockets[i]);
                continue;
            }
            pthread_detach(tid);
        }
    }

    // CLOSE
    close(server_socket);
    printf("Connection closed.\n");
}
