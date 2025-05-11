#include "request-handler.h"

#include <fcntl.h>
#include <sys/epoll.h>

#define MAX_EVENTS 40

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
    int server_socket, client_socket, epoll_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    struct epoll_event event, events[MAX_EVENTS];

    // CREATE
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0)
    {
        perror("Error creating socket");
        return 1;
    }
    // make the server socket non-blocking
    make_non_blocking(server_socket);

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

    // epoll instance
    epoll_fd = epoll_create1(0);
    if (epoll_fd == -1)
    {
        perror("Error creating epoll instance");
        close(server_socket);
        return 1;
    }

    // adding sockets to epoll
    event.events = EPOLLIN;
    event.data.fd = server_socket;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_socket, &event) == -1)
    {
        perror("Error adding server socket to epoll");
        close(server_socket);
        close(epoll_fd);
        return 1;
    }

    printf("Server listening on port %d\n", SERVER_PORT);

    // ALLOW
    while (1)
    {
        int ready_events = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (ready_events < 0)
        {
            // If epoll_wait was interrupted by a signal, continue
            if (errno == EINTR)
                continue;
            perror("Error in epoll_wait");
            break;
        }

        for (int i = 0; i < ready_events; i++)
        {
            int fd = events[i].data.fd;

            // if current event is for server socket, accept connection
            if (fd == server_socket)
            {
                client_socket = accept4(server_socket,
                                        (struct sockaddr *)&client_addr,
                                        &client_len, SOCK_NONBLOCK);

                if (client_socket < 0)
                {
                    perror("Error accepting connection");
                    continue;
                }
                // printf("Connection accepted from %s:%d\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

                // initializing connection state for client
                conn_state *conn = malloc(sizeof(conn_state));
                memset(conn, 0, sizeof(conn_state));
                if (posix_memalign((void **)&conn->req_buffer, MY_BLOCK_SIZE, BUFFER_SIZE) != 0)
                {
                    perror("Failed to allocate aligned buffer");
                    free(conn);
                    close(client_socket);
                    continue;
                }
                conn->fd = client_socket;
                conn->file_fd = -1;
                conn->state = READING_HEADER;

                // adding client socket to epoll to monitor read or write events
                struct epoll_event client_event;
                client_event.events = EPOLLIN | EPOLLET;
                client_event.data.ptr = conn;
                epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_socket, &client_event);
            }
            else
            {
                conn_state *conn = (conn_state *)events[i].data.ptr;
                int res = handle_requests_event_driven(conn);

                uint32_t events = 0;
                if (res == CONN_ALIVE)
                {
                    if (conn->state == HANDLING_GET)
                        events = EPOLLOUT;
                    else if (conn->state == HANDLING_POST)
                        events = EPOLLIN; // Keep reading PUT body
                    else
                        events = EPOLLIN;
                    events |= EPOLLET;
                }
                struct epoll_event ev = {.events = events, .data.ptr = conn};
                epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn->fd, &ev);
                if (res == CONN_CLOSED || res == CONN_ERROR)
                {
                    if (res == CONN_ERROR)
                    {
                        send_response(conn->fd, "HTTP/1.1 500 Internal Server Error", "text/plain", "Internal Server Error");
                    }

                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
                    close(conn->file_fd);
                    close(conn->fd);
                    free(conn->req_buffer);
                    free(conn);
                }
            }
        }
    }

    // CLOSE
    close(server_socket);
    close(epoll_fd);
    printf("Connection closed.\n");
}