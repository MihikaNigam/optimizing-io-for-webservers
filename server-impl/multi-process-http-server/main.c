#include "request-handler.h"

#include <signal.h>
#include <sys/time.h>
#define ACCEPT_TIMEOUT_MS 100
#define MAX_PENDING_ACCEPTS 2048

void make_non_blocking(int socket_fd)
{
    int flags = fcntl(socket_fd, F_GETFL, 0);
    if (flags == -1)
    {
        perror("fcntl F_GETFL");
        exit(EXIT_FAILURE);
    }
    if (fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) == -1)
    {
        perror("fcntl F_SETFL");
        exit(EXIT_FAILURE);
    }
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

    // Avoid child from becoming a zombie process
    signal(SIGCHLD, SIG_IGN);

    // CREATE
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0)
    {
        perror("Error creating socket");
        return 1;
    }

    // make the server socket non-blocking
    make_non_blocking(server_socket);

    int enable = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEPORT, &enable, sizeof(enable));

    // BIND
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
                    break;
                }
                perror("Error accepting connection");
                continue;
            }
            accepted_sockets[accept_count++] = client_socket;
        }
        for (int i = 0; i < accept_count; i++)
        {

            // printf("Connection accepted from %s:%d\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

            // FORK
            pid_t pid = fork();
            if (pid < 0)
            {
                perror("Failed to fork");
                close(accepted_sockets[i]);
                continue;
            }
            else if (pid == 0)
            {
                int file_fd;
                char *req_buffer;
                if (posix_memalign((void **)&req_buffer, MY_BLOCK_SIZE, BUFFER_SIZE) != 0)
                {
                    perror("Failed to allocate aligned buffer");
                    close(client_socket);
                    continue;
                }
                memset(req_buffer, 0, BUFFER_SIZE);
                int res = handle_blocking_requests(accepted_sockets[i], &file_fd, req_buffer);
                if (res == CONN_ERROR)
                {
                    send_response(client_socket, "HTTP/1.1 500 Internal Server Error", "text/plain", "Internal Server Error");
                }
                close(file_fd);
                free(req_buffer);
                close(accepted_sockets[i]);
                exit(0);
            }
            else
            {
                close(accepted_sockets[i]);
            }
        }
    }

    // CLOSE
    close(server_socket);
    printf("Connection closed.\n");
}