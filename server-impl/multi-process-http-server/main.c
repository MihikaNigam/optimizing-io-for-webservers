#include "request-handler.h"

#include <signal.h>

int main()
{
    int server_socket, client_socket;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);

    // Avoid child from becoming a zombie process
    signal(SIGCHLD, SIG_IGN);

    // CREATE
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0)
    {
        perror("Error creating socket");
        return 1;
    }

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
        client_socket = accept(server_socket,
                               (struct sockaddr *)&client_addr,
                               &client_len);
        if (client_socket < 0)
        {
            perror("Error accepting connection");
            continue;
        }

        // printf("Connection accepted from %s:%d\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

        // FORK
        pid_t pid = fork();
        if (pid < 0)
        {
            perror("Failed to fork");
            close(client_socket);
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
            int res = handle_blocking_requests(client_socket, &file_fd, req_buffer);
            if (res == CONN_ERROR)
            {
                send_response(client_socket, "HTTP/1.1 500 Internal Server Error", "text/plain", "Internal Server Error");
            }
            close(file_fd);
            free(req_buffer);
            close(client_socket);
            exit(0);
        }
        else
        {
            close(client_socket);
        }
    }

    // CLOSE
    close(server_socket);
    printf("Connection closed.\n");
}