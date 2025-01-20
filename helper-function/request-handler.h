#ifndef REQUEST_HANDLER_H
#define REQUEST_HANDLER_H

#define BUFFER_SIZE 4096
#define CONN_CLOSED_OR_ERROR 1
#define CONN_ALIVE 0

typedef struct {
    int fd;
    char buffer[BUFFER_SIZE];
    int buf_len;
} connection_t;

void handle_requests(int client_socket);
int handle_requests_event_driven(connection_t *conn);
void handle_get(int client_socket, const char *path);
void handle_put(int client_socket, const char *path);

#endif