#ifndef REQUEST_HANDLER_H
#define REQUEST_HANDLER_H

#include <sys/types.h>
#include <stddef.h>

#define BUFFER_SIZE 65536 //64kb
#define CONN_CLOSED_OR_ERROR 1
#define CONN_ALIVE 0

typedef enum
{
    READING_HEADER,
    HANDLING_GET,
    HANDLING_POST
} conn_state_enum;

typedef struct
{
    int fd; // fd of client
    char buffer[BUFFER_SIZE];
    size_t bytes_read; // of the request
    off_t file_size;   // total file size
    int file_fd;       // fd of file to send or of being written
    off_t byte_offset; // offset in file (read or write)
    conn_state_enum state;
} conn_state;

void handle_requests(int client_socket);
int handle_requests_event_driven(conn_state *conn);
void handle_get(int client_socket, const char *path);
void handle_put(int client_socket, const char *path, int content_length, const char *initial_body, int initial_body_length);

#endif