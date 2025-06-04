#include "request-handler.h"

#include <fcntl.h>
#include <sys/epoll.h>

#define MAX_EVENTS 8192 
static atomic_int req_counter = 0;
static struct iocb *pending_aio_iocbs[BATCH_SIZE]; // Array to hold pending AIO requests

void reset_req_counter() { atomic_store(&req_counter, 0); }
int get_req_counter() { return atomic_load(&req_counter); }
void increment_req_counter() { atomic_fetch_add(&req_counter, 1); }
void add_to_iocbs(struct iocb *aio_iocb)
{
    pending_aio_iocbs[get_req_counter()] = aio_iocb;
    increment_req_counter();
}
void submit_iocbs(struct io_context_t ctx)
{
    int ret = io_submit(ctx, get_req_counter(), pending_aio_iocbs);
    if (ret < 0)
    {
        perror("io_submit failed");
    }
    reset_req_counter();
}

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

void cleanup_connection(int epoll_fd, conn_state *conn)
{
    if (!conn)
        return;
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, conn->fd, NULL);
    if (conn->file_fd != -1)
        close(conn->file_fd);
    if (conn->fd != -1)
        close(conn->fd);
    if (conn->req_buffer)
        free(conn->req_buffer);
    free(conn);
}

int init_aio_context(io_context_t *ctx, int *event_fd, unsigned int max_events)
{
    memset(ctx, 0, sizeof(*ctx));
    if (io_setup(max_events, ctx) < 0)
    {
        perror("io_setup failed");
        return -1;
    }
    *event_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (*event_fd < 0)
    {
        perror("eventfd failed");
        io_destroy(*ctx);
        *ctx = 0;
        return -1;
    }
    printf("AIO context initialized with eventfd: %d\n", *event_fd);
    return 0;
}

int main()
{
    int server_socket, client_socket, epoll_fd;
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);
    struct epoll_event event, events[MAX_EVENTS];

    io_context_t global_aio_ctx = 0; // output context
    int global_aio_event_fd = -1;    // global aio event fd

    // init the iocb queue
    memset(pending_aio_iocbs, 0, sizeof(pending_aio_iocbs));

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

    // init io_context_t
    if (init_aio_context(&global_aio_ctx, &global_aio_event_fd, MAX_EVENTS) < 0)
    {
        perror("Error initializing AIO context");
        close(server_socket);
        close(epoll_fd);
        return 1;
    }

    // Add global_aio_event_fd to epoll
    event.events = EPOLLIN | EPOLLET;
    event.data.fd = global_aio_event_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, global_aio_event_fd, &event) == -1)
    {
        perror("Error adding global_aio_event_fd socket to epoll");
        close(server_socket);
        close(epoll_fd);
        io_destroy(global_aio_ctx);
        close(global_aio_event_fd);
        return 1;
    }

    // adding server sockets to epoll
    event.events = EPOLLIN;
    event.data.fd = server_socket;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_socket, &event) == -1)
    {
        perror("Error adding server socket to epoll");
        close(server_socket);
        close(epoll_fd);
        io_destroy(global_aio_ctx);
        close(global_aio_event_fd);
        return 1;
    }

    printf("Server listening on port %d\n", SERVER_PORT);

    // ALLOW
    while (1)
    {
        if (get_req_counter() > 0)
        {
            int ret = io_submit(global_aio_ctx, get_req_counter(), pending_aio_iocbs);
            if (ret < 0)
            {
                perror("io_submit batch failed");
            }
            reset_req_counter();
        }

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
            if (events[i].data.fd == server_socket)
            {
                // drain accept events from listen queue
                while (1)
                {
                    client_socket = accept4(server_socket,
                                            (struct sockaddr *)&client_addr,
                                            &client_len, SOCK_NONBLOCK);

                    if (client_socket < 0)
                    {
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                            break;
                        perror("Error accepting connection");
                        continue;
                    }
                    // printf("Connection accepted from %s:%d\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

                    // initializing connection state for client
                    conn_state *conn = malloc(sizeof(conn_state));
                    if (!conn)
                    {
                        perror("Failed to allocate conn_state");
                        close(client_socket);
                        continue;
                    }
                    memset(conn, 0, sizeof(conn_state));
                    if (posix_memalign((void **)&conn->req_buffer, MY_BLOCK_SIZE, BUFFER_SIZE) != 0)
                    {
                        perror("Failed to allocate aligned buffer");
                        free(conn);
                        close(client_socket);
                        continue;
                    }
                    memset(conn->req_buffer, 0, BUFFER_SIZE);
                    conn->fd = client_socket;
                    conn->file_fd = -1;
                    conn->state = READING_HEADER;

                    // adding client socket to epoll to monitor io events
                    uint32_t events_to_set = EPOLLIN | EPOLLET | EPOLLRDHUP;
                    struct epoll_event client_event = {.events = events_to_set, .data.ptr = conn, .data.fd = client_socket};
                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_socket, &client_event) == -1)
                    {
                        perror("ERROR: epoll_ctl ADD after client socket accept");
                        cleanup_connection(epoll_fd, conn); // Clean up on error
                    }
                }
            }
            else if (events[i].data.fd == global_aio_event_fd)
            {
                uint64_t completed_aio_ops;

                // reads resets the eventfd's open fd to 0 (MUST CONSUME EVENTS)
                if (read(global_aio_event_fd, &completed_aio_ops, sizeof(completed_aio_ops)) != sizeof(completed_aio_ops))
                {
                    perror("read aio_event_fd failed");
                    continue;
                }
                struct io_event aio_events[completed_aio_ops];

                // get completed aio from libaio cintext
                int num_completed = io_getevents(global_aio_ctx, 0, completed_aio_ops, aio_events, NULL);
                if (num_completed < 0)
                {
                    perror("io_getevents failed");
                    continue;
                }

                for (int j = 0; j < num_completed; ++j)
                {
                    conn_state *conn = (conn_state *)aio_events[j].data;

                    if (!conn)
                    {
                        printf("AIO completion with NULL conn!\n");
                        continue;
                    }
                    ssize_t res = aio_events[j].res;
                    int res2 = aio_events[j].res2;
                    conn->last_aio_res = res;

                    if (res < 0)
                    {
                        fprintf(stderr, "Async request failed: %s for state: %d\n",
                                strerror(-res2), conn->state);
                        send_response(conn->fd, "HTTP/1.1 500 Internal Server Error", "text/plain", "File I/O Error.");
                        cleanup_connection(epoll_fd, conn);
                        continue;
                    }
                    else if (res == 0)
                    {
                        if (conn->state == WAITING_FOR_AIO_READ)
                            send_response(conn->fd, "HTTP/1.1 200 OK", "text/plain", "File sent.");
                        else if (conn->state == WAITING_FOR_AIO_WRITE)
                        {
                            if (conn->byte_offset >= conn->file_size)
                            {
                                send_response(conn->fd, "HTTP/1.1 201 Created", "text/plain", "File uploaded.");
                                return CONN_CLOSED;
                            }
                        }
                        send_response(conn->fd, "HTTP/1.1 500 Internal Server Error", "text/plain", "File write error (0 bytes written).");
                        fprintf(stderr, "AIO operation returned 0 bytes for FD %d, state %d. Possible EOF/Error.\n",
                                conn->file_fd, conn->state);

                        cleanup_connection(epoll_fd, conn);
                        continue;
                    }

                    int status = handle_requests_event_driven(&global_aio_ctx, &global_aio_event_fd, conn);

                    uint32_t events_to_set = 0;
                    if (status == CONN_ALIVE)
                    {
                        if (conn->state == HANDLING_GET_IO || conn->state == READING_HEADER)
                            events_to_set = EPOLLIN; // Keep reading PUT body
                        else if (conn->state == HANDLING_POST_IO)
                            events_to_set = EPOLLOUT;
                        else if (conn->state == WAITING_FOR_AIO_READ || conn->state == WAITING_FOR_AIO_WRITE)
                            events_to_set = 0;
                        else
                            events_to_set = EPOLLIN;
                        events_to_set |= EPOLLET | EPOLLRDHUP;
                        struct epoll_event client_event = {.events = events_to_set, .data.ptr = conn};
                        if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn->fd, &client_event) == -1)
                            perror("ERROR: epoll_ctl MOD after AIO completion");
                    }

                    else if (status == CONN_CLOSED || status == CONN_ERROR)
                    {
                        if (status == CONN_ERROR)
                            send_response(conn->fd, "HTTP/1.1 500 Internal Server Error", "text/plain", "Internal Server Error");
                        cleanup_connection(epoll_fd, conn);
                    }
                }
            }
            else
            {
                conn_state *conn = (conn_state *)events[i].data.ptr;
                int status = handle_requests_event_driven(&global_aio_ctx, &global_aio_event_fd, conn);

                if (status == CONN_ALIVE)
                {
                    uint32_t events_to_set = 0;
                    if (conn->state == READING_HEADER || conn->state == HANDLING_GET_IO)
                        events_to_set = EPOLLIN; // Keep reading PUT body
                    else if (conn->state == HANDLING_POST_IO)
                        events_to_set = EPOLLOUT;
                    else if (conn->state == WAITING_FOR_AIO_READ || conn->state == WAITING_FOR_AIO_WRITE)
                        events_to_set = 0;
                    else
                        events_to_set = EPOLLIN;
                    events_to_set |= EPOLLET | EPOLLRDHUP;
                    struct epoll_event client_event = {.events = events_to_set, .data.ptr = conn};
                    if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, conn->fd, &client_event) == -1)
                        perror("ERROR: epoll_ctl MOD after network event processing");
                }
                else if (status == CONN_CLOSED || status == CONN_ERROR)
                {
                    if (status == CONN_ERROR)
                        send_response(conn->fd, "HTTP/1.1 500 Internal Server Error", "text/plain", "Internal Server Error");
                    cleanup_connection(epoll_fd, conn);
                }
            }
        }
    }

    // CLOSE
    close(server_socket);
    io_destroy(global_aio_ctx);
    close(global_aio_event_fd);
    close(epoll_fd);
    printf("Connection closed.\n");
    return 0;
}