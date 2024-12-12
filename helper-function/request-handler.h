#ifndef REQUEST_HANDLER_H
#define REQUEST_HANDLER_H

void handle_requests(int client_socket);
void handle_get(int client_socket, const char *path);
void handle_put(int client_socket, const char *path);

#endif