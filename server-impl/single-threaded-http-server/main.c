#include "request-handler.h"

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
            perror("Error accepting client connection");
            continue; // Continue to accept next connection
        }

        // printf("Connection accepted from %s:%d\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

        handle_blocking_requests(client_socket);

        close(client_socket);
    }

    // CLOSE
    close(server_socket);
    printf("Connection closed.\n");
}
