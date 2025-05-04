#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <errno.h>

#include "request-handler.h"

#define SERVER_PORT 8083
#define QUEUE_DEPTH 32

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

void submit_accept(struct io_uring *ring, int server_fd)
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe)
    {
        perror("failed to fetch sqe in submit_accept");
        return;
    }

    // initialize new conn
    conn_state *conn = calloc(1, sizeof(conn_state));
    if (posix_memalign((void **)&conn->buffer, MY_BLOCK_SIZE, BUFFER_SIZE) != 0)
    {
        perror("Failed to allocate aligned buffer");
        free(conn);
        return;
    }
    conn->fd = -1;
    conn->file_fd = -1;
    conn->client_len = sizeof(conn->client_addr);
    conn->state = READING_HEADER;

    io_uring_prep_accept(sqe, server_fd, (struct sockaddr *)&conn->client_addr, &conn->client_len, SOCK_NONBLOCK);
    io_uring_sqe_set_data(sqe, conn);
}

int main()
{
    int server_socket;
    struct sockaddr_in server_addr, client_addr;
    struct io_uring ring;

    // CREATE
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0)
    {
        perror("Error creating socket");
        return 1;
    }

    // make the server socket non-blocking
    make_non_blocking(server_socket);

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
    if (listen(server_socket, 1024) < 0)
    {
        perror("Error listening on socket");
        return 1;
    }

    // init io_uring
    if (io_uring_queue_init(QUEUE_DEPTH, &ring, 0) < 0)
    {
        perror("Error initializing io_uring");
        close(server_socket);
        exit(1);
        return 1;
    }
    printf("io_uring initialized\n");

    printf("Server listening on PORT %d\n", SERVER_PORT);

    // ALLOW
    // 1st connection req
    submit_accept(&ring, server_socket);
    io_uring_submit(&ring);

    while (1)
    {
        struct io_uring_cqe *cqe;
        // wait until an event happens
        int ret = io_uring_wait_cqe(&ring, &cqe);
        if (ret < 0)
        {
            if (errno == EINTR)
                continue;
            perror("io_uring_wait_cqe");
            break;
        }
        conn_state *conn = (conn_state *)io_uring_cqe_get_data(cqe);
        if (!conn)
        {
            perror("Null connection state in completion");
            io_uring_cqe_seen(&ring, cqe);
            continue;
        }

        int res = cqe->res;

        if (res < 0)
        {
            // perror("Error accepting connection");
            free(conn->buffer);
            free(conn);
            submit_accept(&ring, server_socket);
            io_uring_submit(&ring);
            // continue;
        }
        else if (conn->fd == -1)
        {
            conn->fd = res;
            // connection succeeded
            printf("Connection accepted from %s:%d\n", inet_ntoa(conn->client_addr.sin_addr), ntohs(conn->client_addr.sin_port));
            io_uring_func(&ring, conn, READ_REQUEST);
            conn->last_op = OP_READ;

            // re-arm
            submit_accept(&ring, server_socket);
        }
        else
        {
            // existing connection
            int status = handle_requests_uring(&ring, cqe);
            if (status == CONN_CLOSED_OR_ERROR)
            {
                submit_close(&ring, conn);
            }
        }

        io_uring_cqe_seen(&ring, cqe);
        io_uring_submit(&ring);
    }

    io_uring_queue_exit(&ring);
    close(server_socket);
    printf("Connection closed.\n");
}
