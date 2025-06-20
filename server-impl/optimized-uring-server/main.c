#include "request-handler.h"

#include <fcntl.h>

#define QUEUE_DEPTH 8192
#define WAIT_TIMEOUT_MS 100

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
        if (io_uring_sq_space_left(ring) == 0)
        {
            io_uring_submit(ring);
            sqe = io_uring_get_sqe(ring);
            if (!sqe)
            {
                fprintf(stderr, "Failed to get SQE in add_accept_request: %s\n", strerror(errno));
                return CONN_ERROR;
            }
            reset_req_counter();
        }
    }

    // initializing connection state for client
    conn_state *conn = malloc(sizeof(conn_state));
    memset(conn, 0, sizeof(conn_state));
    if (posix_memalign((void **)&conn->req_buffer, MY_BLOCK_SIZE, BUFFER_SIZE) != 0)
    {
        perror("Failed to allocate aligned buffer");
        free(conn);
        return CONN_ERROR;
    }
    memset(conn->req_buffer, 0, BUFFER_SIZE);
    conn->file_fd = -1;
    conn->state = ACCEPTING_CONNECTION;
    io_uring_prep_accept(sqe, server_socket, (struct sockaddr *)client_addr,
                         client_len, SOCK_NONBLOCK);
    io_uring_sqe_set_data(sqe, conn);
    return CONN_ALIVE;
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

void print_sq_poll_kernel_thread_status()
{

    if (system("ps --ppid 2 | grep io_uring-sq") == 0)
        printf("Kernel thread io_uring-sq found running...\n");
    else
        printf("Kernel thread io_uring-sq is not running.\n");
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
    struct io_uring ring;
    struct io_uring_params params;

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

    // init io_uring
    memset(&params, 0, sizeof(params));
    // params.flags |= IORING_SETUP_SQPOLL;
    // params.sq_thread_idle = 2000;
    if (io_uring_queue_init_params(QUEUE_DEPTH, &ring, &params) < 0)
    {
        perror("Error initializing io_uring");
        close(server_socket);
        exit(1);
        return 1;
    }

    // check if IORING_FEAT_FAST_POLL is supported
    if (!(params.features & IORING_FEAT_FAST_POLL))
    {
        printf("IORING_FEAT_FAST_POLL not available in the kernel, quiting...\n");
        exit(0);
    }

    printf("Server listening on PORT %d\n", SERVER_PORT);

    // ALLOW
    // 1st connection req
    struct sockaddr_in client_addr1;
    socklen_t client_len1 = sizeof(client_addr1);
    if (add_accept_request(server_socket, &ring, &client_addr1, &client_len1) < 0)
    {
        perror("Error accepting 1st connection");
        close(server_socket);
        exit(1);
        return 1;
    }
    io_uring_submit(&ring);

    // Pre-seed with multiple accept requests
    for (int i = 0; i < 10; i++)
    {
        if (add_accept_request(server_socket, &ring, &client_addr, &client_len) < 0)
        {
            fprintf(stderr, "Failed to add initial accept request\n");
            close(server_socket);
            return 1;
        }
    }

    printf("SQPOLL thread active (thread ID: %d)\n", print_sq_poll_kernel_thread_status());

    // struct io_uring_cqe *cqe;
    // struct __kernel_timespec ts = {
    //     .tv_sec = 0,
    //     .tv_nsec = WAIT_TIMEOUT_MS * 1000000,
    // };

    while (1)
    {
        /*
        unsigned ret = io_uring_wait_cqes(&ring, &cqe, 1, &ts, NULL);
        if (ret == -ETIME)
        {
            // Timeout occurred - submit any pending SQEs
            if (get_req_counter() > 0)
            {
                io_uring_submit(&ring);
                reset_req_counter();
            }
            continue;
        }
        else if (ret < 0)
        {
            // Handle other errors
            if (errno == -EINTR)
                continue;
            perror("io_uring_wait_cqes");
            break;
        }*/
        io_uring_submit_and_wait(&ring, 1);
        struct io_uring_cqe *cqe;
        unsigned cqe_count = 0;
        unsigned head;
        io_uring_for_each_cqe(&ring, head, cqe)
        {
            conn_state *conn = (conn_state *)io_uring_cqe_get_data(cqe);
            ssize_t res = cqe->res;

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

            // printf("Connection accepted from %s:%d\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

            if (conn->state == ACCEPTING_CONNECTION)
            {
                conn->fd = res;
                conn->state = READING_HEADER;
                conn->last_op = OP_READ;
                if (io_uring_func(&ring, conn, RECV_REQUEST) < 0)
                {
                    close_conn(conn);
                    continue;
                }
                struct sockaddr_in client_addr;
                socklen_t client_len = sizeof(client_addr);

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
            cqe_count++;
        }

        io_uring_cq_advance(&ring, cqe_count);

        /*
        // Submit any prepared SQEs if we have a batch
        if (get_req_counter() > 0)
        {
            io_uring_submit(&ring);
            reset_req_counter();
        }
*/
        // if (io_uring_sq_ready(&ring) > QUEUE_DEPTH / 2)
        //     io_uring_submit(&ring); // Manual kick
    }

    io_uring_queue_exit(&ring);
    close(server_socket);
    printf("Connection closed.\n");
}
