#include "request-handler.h"

#include <sys/time.h>

#define ACCEPT_TIMEOUT_MS 100 // Timeout after 100ms of no new connections

int main()
{
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
        int accepted_sockets[MAX_PENDING_ACCEPTS];
        int accept_count = 0;

        while (accept_count < MAX_PENDING_ACCEPTS)
        {

            int client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_len);
            if (client_socket < 0)
            {
                if (errno == EWOULDBLOCK || errno == EAGAIN)
                {
                    // usleep(5000); //to prevent busy waiting
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

            int file_fd;
            char *req_buffer;
            if (posix_memalign((void **)&req_buffer, MY_BLOCK_SIZE, BUFFER_SIZE) != 0)
            {
                perror("Failed to allocate aligned buffer");
                close(accepted_sockets[i]);
                continue;
            }
            memset(req_buffer, 0, BUFFER_SIZE);
            int res = handle_blocking_requests(accepted_sockets[i], &file_fd, req_buffer);
            if (res == CONN_ERROR)
            {
                send_response(accepted_sockets[i], "HTTP/1.1 500 Internal Server Error", "text/plain", "Internal Server Error");
            }
            close(file_fd);
            free(req_buffer);
            close(accepted_sockets[i]);
        }
    }
    // CLOSE
    close(server_socket);
    printf("Connection closed.\n");
}
