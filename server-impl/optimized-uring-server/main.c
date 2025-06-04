#include "request-handler.h"

#include <fcntl.h>

#define QUEUE_DEPTH 1024

static atomic_int req_counter = 0;

void reset_req_counter() { atomic_store(&req_counter, 0); }
int get_req_counter() { return atomic_load(&req_counter); }
void increment_req_counter() { atomic_fetch_add(&req_counter, 1); }

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

int add_accept_request(int server_socket, struct io_uring *ring, struct sockaddr_in *client_addr,
                       socklen_t *client_len)
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe)
    {
        fprintf(stderr, "failed to fetch sqe in submit_accept: %s\n", strerror(errno));
        return -1;
    }
    io_uring_prep_accept(sqe, server_socket, (struct sockaddr *)client_addr,
                         client_len, SOCK_NONBLOCK);

    // initializing connection state for client
    conn_state *conn = malloc(sizeof(conn_state));
    memset(conn, 0, sizeof(conn_state));
    if (posix_memalign((void **)&conn->req_buffer, MY_BLOCK_SIZE, BUFFER_SIZE) != 0)
    {
        fprintf(stderr, "Failed to allocate aligned buffer: %s\n", strerror(errno));
        free(conn);
        return -1;
    }
    memset(conn->req_buffer, 0, BUFFER_SIZE);
    conn->file_fd = -1;
    conn->state = ACCEPTING_CONNECTION;

    io_uring_sqe_set_data(sqe, conn);
    io_uring_submit(ring);
    return 0;
}

void close_conn(conn_state *conn)
{
    if (!conn)
        return;
    if (conn->file_fd != -1)
        close(conn->file_fd);
    if (conn->fd != -1)
        close(conn->fd);
    if (conn->req_buffer)
        free(conn->req_buffer);
    free(conn);
}

int main()
{
    int server_socket;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    struct io_uring ring;
    struct io_uring_params params;
    struct io_uring_cqe *cqe;

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
    if (listen(server_socket, ACCEPT_BACKLOG) < 0)
    {
        perror("Error listening on socket");
        return 1;
    }

    // init io_uring
    memset(&params, 0, sizeof(params));
    params.flags |= IORING_SETUP_SQPOLL;
    params.sq_thread_idle = 2000;
    if (io_uring_queue_init_params(QUEUE_DEPTH, &ring, &params) < 0)
    {
        perror("Error initializing io_uring");
        close(server_socket);
        exit(1);
        return 1;
    }

    printf("Server listening on PORT %d\n", SERVER_PORT);

    // ALLOW
    // 1st connection req
    if (add_accept_request(server_socket, &ring, &client_addr, &client_len) < 0)
    {
        perror("Error accepting 1st connection");
        close(server_socket);
        exit(1);
        return 1;
    }

    while (1)
    {
        int ret = io_uring_wait_cqe(&ring, &cqe);
        if (ret < 0)
        {
            if (errno == -EINTR)
                continue;
            fprintf(stderr, "Error in uring_wait_cqe: %s\n", strerror(errno));
            break;
        }
        conn_state *conn = (conn_state *)io_uring_cqe_get_data(cqe);
        ssize_t res = cqe->res;
        io_uring_cqe_seen(&ring, cqe);

        if (!conn)
            continue;

        if (res < 0)
        {
            if (res == -EAGAIN || res == -EWOULDBLOCK)
                continue;

            fprintf(stderr, "Async request failed: %s for state: %d\n",
                    strerror(-cqe->res), conn->state);
            send_response(conn->fd, "HTTP/1.1 400 Bad Request", "text/plain", "Malformed Request.");
            close_conn(conn);
        }
        else if (res == 0)
        {
            fprintf(stderr, "Client disconnected: %s for state: %d\n",
                    strerror(-cqe->res), conn->state);
            send_response(conn->fd, "HTTP/1.1 400 Bad Request", "text/plain", "Client Disconnected");
            close_conn(conn);
        }
        else
        {
            // printf("Connection accepted from %s:%d\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

            if (conn->state == ACCEPTING_CONNECTION)
            {
                conn->fd = res;
                conn->state = READING_HEADER;
                conn->last_op = OP_READ;
                io_uring_func(&ring, conn, RECV_REQUEST);

                if (add_accept_request(server_socket, &ring, &client_addr, &client_len) < 0)
                    continue;
            }
            else
            {
                // existing connection
                int status = handle_requests_uring(&ring, conn, res);
                if (status == CONN_CLOSED || status == CONN_ERROR)
                {
                    if (status == CONN_ERROR)
                    {
                        send_response(conn->fd, "HTTP/1.1 500 Internal Server Error", "text/plain", "Internal Server Error");
                    }
                    close_conn(conn);
                }
            }
        }
    }
    printf("LASST loop_counter: %d\n", loop_counter);

    io_uring_queue_exit(&ring);
    close(server_socket);
    printf("Connection closed.\n");
}
