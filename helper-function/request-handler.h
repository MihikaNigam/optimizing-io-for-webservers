#ifndef REQUEST_HANDLER_H
#define REQUEST_HANDLER_H

#include <stdio.h>  // for io
#include <stdlib.h> // for std lib
#include <string.h> // for str manipulation

#include <unistd.h>    // for sys calls
#include <errno.h>     // for erros
#include <sys/types.h> // for sys  call data types
#include <stddef.h>    // for standard data types
#include <stdint.h>    // for fix width int types

#include <sys/socket.h> // for socket api func
#include <netinet/in.h> // for internet protocol def
#include <arpa/inet.h>  // for ip manipulation

#include <fcntl.h>    // for file control options
#include <sys/stat.h> // to get file stats
#include <sys/mman.h> // for memory alignment
#include <liburing.h> // uring

#define SERVER_PORT 8083
#define ACCEPT_BACKLOG 4096
#define BUFFER_SIZE 64 * 1024 // 64kb or 16 blocks on (hardware)
#define BLOCK_SIZE 4096
#define ROOT "/var/www/html"

#define CONN_CLOSED 1
#define CONN_ERROR -1
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