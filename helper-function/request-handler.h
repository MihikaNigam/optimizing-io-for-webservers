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

#include <libaio.h>      // for libaio
#include <sys/eventfd.h> // For eventfd
#include <liburing.h>    // for uring

#define SERVER_PORT 8083
#define ACCEPT_BACKLOG 4096
#define MAX_PENDING_ACCEPTS 2048
#define BATCH_SIZE 1024

#define BUFFER_SIZE 64 * 1024 // 64kb or 16 blocks on (hardware)
#define MY_BLOCK_SIZE 4096
#define ROOT "/var/www/html"

#define CONN_CLOSED 1
#define CONN_ERROR -1
#define CONN_ALIVE 0

_Static_assert(BUFFER_SIZE % MY_BLOCK_SIZE == 0,
               "BUFFER_SIZE must be multiple of BLOCK_SIZE");

typedef enum
{
    BLOCKING,
    NON_BLOCKING
} server_type;

// typedef enum
// {
//     ACCEPTING_CONNECTION,
//     READING_HEADER,
//     HANDLING_GET,
//     HANDLING_POST
// } conn_state_enum;

typedef enum
{
    READING_HEADER,

    // network IO
    HANDLING_GET_IO,  // waiting for more data from client (RECEIVING_PUT_DATA)
    HANDLING_POST_IO, // data ready in buffer, sending to client (SENDING_GET_DATA)

    // file IO
    WAITING_FOR_AIO_READ,  // AIO read submitted, waiting for completion (WAITING_FOR_AIO_READ)
    WAITING_FOR_AIO_WRITE, // AIO write submitted, waiting for completion (WAITING_FOR_AIO_WRITE)
} conn_state_enum;

typedef enum
{
    OP_READ,
    OP_WRITE
} operation_type;

typedef enum
{
    RECV_REQUEST,
    WRITE_FILE,
    READ_FILE,
    SEND_FILE
} async_func_enum;

typedef struct
{
    int fd;                         // fd of client
    char *req_buffer;               // aligned buffer
    size_t bytes_read;              // of the request
    off_t file_size;                // total file size
    int file_fd;                    // fd of file to send or of being written
    off_t byte_offset;              // offset in file (read or write)
    size_t util_offset;             // for network offset
    size_t header_buffer_processed; // to track request read/sent bytes
    conn_state_enum state;

    ssize_t last_aio_res; // last aio result
    struct iocb aio_iocb; // Single iocb for the current async op
    // struct iocb *aio_iocbs; // Array of iocb pointers for io_submit
} conn_state;

void send_response(int client_socket, const char *status, const char *content_type, const char *body);
int handle_blocking_requests(int client_socket, int *file_fd, char *req_buffer);
int handle_requests_event_driven(io_context_t *global_aio_ctx, int *global_aio_event_fd, conn_state *conn);
int handle_requests_uring(struct io_uring *ring, conn_state *conn, ssize_t res);
void submit_close(struct io_uring *ring, conn_state *conn);
int io_uring_func(struct io_uring *ring, conn_state *conn, async_func_enum func);

void reset_req_counter();
int get_req_counter();
void increment_req_counter();
void add_to_iocbs(struct iocb *aio_iocb);
void submit_iocbs(struct io_context_t ctx)

#endif