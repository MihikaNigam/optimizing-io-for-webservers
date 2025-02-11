#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "request-handler.h"

#include <pthread.h>

// Configuration constants
#define SERVER_PORT 8083

void *handle_client(void *arg)
{
    int client_socket = *(int *)arg;
    free(arg); // Free the allocated memory for the socket descriptor

    handle_requests(client_socket);

    close(client_socket);
    printf("Connection closed.\n");
    return NULL;
}

int main()
{
    int server_socket, client_socket;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr); // recieved after the connection is established

    // CREATE
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0)
    {
        perror("Error creating socket");
        return 1;
    }

    // CONFIGURE & BIND -> configuring a socket to use IPv4 and bind to all available network interfaces on a specific port.
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
    if (listen(server_socket, 1024) < 0)
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

        printf("Connection accepted from %s:%d\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

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
