#ifndef REQUEST_HANDLER_H
#define REQUEST_HANDLER_H

#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <stddef.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

#include <liburing.h>

#define BUFFER_SIZE 64 * 1024 // 64kb or 16 blocks on (hardware)
#define MY_BLOCK_SIZE 4096
#define ROOT "/var/www/html"

#define CONN_CLOSED_OR_ERROR 1
#define CONN_ALIVE 0

_Static_assert(BUFFER_SIZE % BLOCK_SIZE == 0,
               "BUFFER_SIZE must be multiple of BLOCK_SIZE");

typedef enum
{
    READING_HEADER,
    HANDLING_GET,
    HANDLING_POST
} conn_state_enum;

typedef enum
{
    OP_READ,
    OP_WRITE
} operation_type;

typedef enum
{
    READ_REQUEST,
    WRITE_FILE,
    READ_FILE,
    SEND_FILE
} uring_func_enum;

typedef struct
{
    int fd; // fd of client
    char header_buffer[BUFFER_SIZE];
    char *buffer;      // aligned buffer
    size_t bytes_read; // of the request
    off_t file_size;   // total file size
    int file_fd;       // fd of file to send or of being written
    off_t byte_offset; // offset in file (read or write)
    size_t util_offset;
    size_t header_buffer_processed; // to track request read/sent bytes
    conn_state_enum state;

    struct sockaddr_in client_addr;
    socklen_t client_len;
    operation_type last_op;
} conn_state;

void handle_requests(int client_socket);
int handle_requests_event_driven(conn_state *conn);
int handle_requests_uring(struct io_uring *ring, struct io_uring_cqe *cqe);
void submit_close(struct io_uring *ring, conn_state *conn);
void io_uring_func(struct io_uring *ring, conn_state *conn, uring_func_enum func);

#endif