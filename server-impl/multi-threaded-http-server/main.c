#include "request-handler.h"

#include <pthread.h>

void *handle_client(void *arg)
{
    int client_socket = *(int *)arg;
    free(arg); // Free the allocated memory for the socket descriptor

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
    int server_socket, client_socket;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);

    // CREATE
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0)
    {
        perror("Error creating socket");
        return 1;
    }

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
        client_socket = accept(server_socket,
                               (struct sockaddr *)&client_addr,
                               &client_len);
        if (client_socket < 0)
        {
            perror("Error accepting connection");
            continue; // Continue to accept next connection
        }

        // printf("Connection accepted from %s:%d\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

        // Allocate memory for the client socket to pass to the thread
        int *pclient = malloc(sizeof(int));
        if (pclient == NULL)
        {
            perror("Failed to allocate memory for client socket");
            close(client_socket);
            continue;
        }
        *pclient = client_socket;

        // THREAD
        pthread_t tid;
        // Create a new thread to handle the client connection
        if (pthread_create(&tid, NULL, handle_client, pclient) != 0)
        {
            perror("Failed to create thread");
            free(pclient);
            close(client_socket);
            continue;
        }

        // Detach the thread so that it cleans up after itself
        pthread_detach(tid);
    }

    // CLOSE
    close(server_socket);
    printf("Connection closed.\n");
}
