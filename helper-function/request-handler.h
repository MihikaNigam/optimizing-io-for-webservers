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

#define BUFFER_SIZE 64 * 1024 // 64kb or 16 blocks on (hardware)
#define BLOCK_SIZE 4096
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

typedef struct
{
    int fd;                          // fd of client
    char header_buffer[BUFFER_SIZE]; 
    char *buffer; // aligned buffer
    size_t bytes_read; // of the request
    off_t file_size;   // total file size
    int file_fd;       // fd of file to send or of being written
    off_t byte_offset; // offset in file (read or write)
    size_t util_offset;
    conn_state_enum state;

    struct sockaddr_in client_addr;
    socklen_t client_len;
    operation_type last_op;
} conn_state;

void handle_requests(int client_socket);
int handle_requests_event_driven(conn_state *conn);
void handle_get(int client_socket, const char *path);
void handle_put(int client_socket, const char *path, int content_length, const char *initial_body, int initial_body_length);
void send_response(int client_socket, const char *status, const char *content_type, const char *body);
const char *get_mime_type(const char *path);

#endif