// request_handler.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include "request-handler.h"

#define ROOT "/var/www/html"
// #define BUFFER_SIZE 4096

void send_response(int client_socket, const char *status, const char *content_type, const char *body)
{
    char response[BUFFER_SIZE];
    snprintf(response, sizeof(response),
             "%s\r\nContent-Type: %s\r\n\r\n%s", status, content_type, body);

    if (body)
    {
        int content_length = strlen(body);
        snprintf(response, sizeof(response),
                 "%s\r\nContent-Type: %s\r\nContent-Length: %d\r\n\r\n%s",
                 status, content_type, content_length, body);
        send(client_socket, response, strlen(response), 0);
    }
    else
    {
        // When there's no body, don't include Content-Length
        snprintf(response, sizeof(response),
                 "%s\r\nContent-Type: %s\r\n\r\n",
                 status, content_type);
        send(client_socket, response, strlen(response), 0);
    }
}

const char *get_mime_type(const char *path)
{
    if (strstr(path, ".jpg") || strstr(path, ".jpeg"))
        return "image/jpeg";
    else if (strstr(path, ".pdf"))
        return "application/pdf";
    else if (strstr(path, ".zip"))
        return "application/zip";
    else if (strstr(path, ".txt"))
        return "text/plain";
    else if (strstr(path, ".html"))
        return "text/html";
    else
        return "application/octet-stream"; // Default for unknown types
}

void handle_get(int client_socket, const char *path)
{
    char full_path[1024];
    FILE *file;
    snprintf(full_path, sizeof(full_path), "%s%s", ROOT, path);

    // Check if the path is just "/"
    if (strcmp(path, "/") == 0)
    {
        snprintf(full_path, sizeof(full_path), "%s/server-index.html", ROOT);
    }

    file = fopen(full_path, "rb");
    if (file == NULL)
    {
        send_response(client_socket, "HTTP/1.1 404 Not Found", "text/plain", "File not found.");
        return;
    }

    // Determine the file size
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    rewind(file);

    // Get the MIME type
    const char *mime_type = get_mime_type(full_path);
    printf("mime_type is: %s\n", mime_type);

    // Construct and send the HTTP header
    char header[BUFFER_SIZE];
    snprintf(header, sizeof(header),
             "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %ld\r\n\r\n",
             mime_type, file_size);
    send(client_socket, header, strlen(header), 0);

    // Send the file content
    char buffer[BUFFER_SIZE];
    size_t bytes_read;
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0)
    {
        // It's better to handle partial sends in a loop, but for simplicity:
        send(client_socket, buffer, bytes_read, 0);
    }
    fclose(file);
}

void handle_put(int client_socket, const char *path, int content_length, const char *initial_body, int initial_body_length)
{
    char buffer[BUFFER_SIZE];
    FILE *file;

    // Check if the path starts with "/upload"
    if (strncmp(path, "/upload", 7) != 0)
    {
        send_response(client_socket, "HTTP/1.1 400 Bad Request", "text/plain", "Invalid upload path.");
        return;
    }

    // constructing file path under ROOT/uploads/
    char file_path[2048];
    snprintf(file_path, sizeof(file_path), "%s/uploads%s", ROOT, path + 7);
    printf("Trying to create file at: %s\n", file_path);

    // open file for writing
    file = fopen(file_path, "wb");
    if (file == NULL)
    {
        perror("Error creating file");
        send_response(client_socket, "HTTP/1.1 500 Internal Server Error", "text/plain", "Error creating file.");
        return;
    }

    // write initial body data if present
    int total_received = 0;
    if (initial_body && initial_body_length > 0)
    {
        fwrite(initial_body, 1, initial_body_length, file);
        total_received = initial_body_length;
    }

    // read remaining data based on Content-Length
    int remaining = content_length - total_received;
    while (remaining > 0)
    {
        int bytes_to_read = remaining < BUFFER_SIZE ? remaining : BUFFER_SIZE;
        int bytes_read = read(client_socket, buffer, bytes_to_read);
        if (bytes_read <= 0)
        {
            perror("Error reading from client or connection closed prematurely");
            break;
        }
        fwrite(buffer, 1, bytes_read, file);
        total_received += bytes_read;
        remaining -= bytes_read;
    }

    fclose(file);

    // verify if all data was received
    if (total_received == content_length)
    {
        send_response(client_socket, "HTTP/1.1 200 OK", "text/plain", "File uploaded successfully.");
    }
    else
    {
        send_response(client_socket, "HTTP/1.1 500 Internal Server Error", "text/plain", "File upload incomplete.");
    }
}

void handle_requests(int client_socket)
{
    char req_buffer[BUFFER_SIZE];
    int bytes_read;

    // Read incoming request
    bytes_read = read(client_socket, req_buffer, sizeof(req_buffer) - 1);
    if (bytes_read < 0)
    {
        perror("Error reading request");
        return;
    }

    req_buffer[bytes_read] = '\0'; // Null-terminate the request string
    // printf("Received request:\n%s\n", req_buffer);

    // Extract method and path from the request string
    char method[8];
    char path[1024];
    sscanf(req_buffer, "%s %s", method, path);
    printf("Received request:\n%s %s\n", method, path);

    // Handle GET method
    if (strcmp(method, "GET") == 0)
    {
        handle_get(client_socket, path);
    }
    // Handle PUT method
    else if (strcmp(method, "PUT") == 0)
    {
        char *cl_header = strstr(req_buffer, "Content-Length: ");
        if (!cl_header)
        {
            send_response(client_socket, "HTTP/1.1 411 Length Required", "text/plain", "Content-Length required.");
            return;
        }
        int content_length = atoi(cl_header + strlen("Content-Length: "));
        char *body_start = strstr(req_buffer, "\r\n\r\n");
        if (!body_start)
        {
            send_response(client_socket, "HTTP/1.1 400 Bad Request", "text/plain", "Malformed headers.");
            return;
        }
        body_start += 4; // Skip CRLFCRLF
        int initial_body_length = bytes_read - (body_start - req_buffer);
        if (initial_body_length < 0)
            initial_body_length = 0;
        if (initial_body_length > content_length)
            initial_body_length = content_length;

        handle_put(client_socket, path, content_length, body_start, initial_body_length);
        // handle_put(client_socket, path);
    }
    else
    {
        // Unsupported method
        send_response(client_socket, "HTTP/1.1 405 Method Not Allowed", "text/plain", "Method Not Allowed.");
    }
}

/*
int handle_requests_event_driven(connection_t *conn)
{
    int bytes_read; // data from single recv call

    // non-blocking read loop
    while ((bytes_read = recv(conn->fd, conn->buffer + conn->buf_len, sizeof(conn->buffer) - conn->buf_len - 1, MSG_DONTWAIT)) > 0)
    {

        conn->buf_len += bytes_read;
        conn->buffer[conn->buf_len] = '\0';
        // printf("Received request:\n%s\n", conn->buffer);

        // check if we have full req
        char *headers_end = strstr(conn->buffer, "\r\n\r\n");

        if (headers_end)
        {
            char method[8];
            char path[1024];
            sscanf(conn->buffer, "%s %s", method, path);

            if (strcmp(method, "GET") == 0)
            {
                // save current flags
                int flags = fcntl(conn->fd, F_GETFL, 0);
                // disable non-blocking mode temporarily
                fcntl(conn->fd, F_SETFL, flags & ~O_NONBLOCK);

                handle_get(conn->fd, path);

                // restore original flags
                fcntl(conn->fd, F_SETFL, flags);
            }
            else if (strcmp(method, "PUT") == 0)
            {
                int flags = fcntl(conn->fd, F_GETFL, 0);
                // handle_put(conn->fd, path);
                char *cl_header = strstr(conn->buffer, "Content-Length: ");
                fcntl(conn->fd, F_SETFL, flags & ~O_NONBLOCK);

                if (!cl_header)
                {
                    send_response(conn->fd, "HTTP/1.1 411 Length Required", "text/plain", "Content-Length required.");
                    return CONN_CLOSED_OR_ERROR;
                }

                int content_length = atoi(cl_header + strlen("Content-Length: "));
                if (content_length <= 0)
                {
                    send_response(conn->fd, "HTTP/1.1 400 Bad Request", "text/plain", "Invalid Content-Length.");
                    return CONN_CLOSED_OR_ERROR;
                }

                char *body_start = headers_end + 4;
                int initial_body_length = conn->buf_len - (body_start - conn->buffer);

                if (initial_body_length < 0)
                    initial_body_length = 0;
                if (initial_body_length > content_length)
                    initial_body_length = content_length;

                handle_put(conn->fd, path, content_length, body_start, initial_body_length);
                fcntl(conn->fd, F_SETFL, flags);
            }
            else
            {
                // Unsupported method
                send_response(conn->fd, "HTTP/1.1 405 Method Not Allowed", "text/plain", "Method Not Allowed.");
            }

            return CONN_CLOSED_OR_ERROR;
        }
    }

    // if recv() returned 0 => client closed cleanly
    if (bytes_read == 0)
    {
        return CONN_CLOSED_OR_ERROR;
    }
    // if recv() < 0 and errno is EAGAIN => no more data right now
    if (bytes_read < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
    {
        return CONN_ALIVE; // wait for more data
    }
    // else => real error
    return CONN_CLOSED_OR_ERROR;
}
*/

int handle_requests_event_driven(connection_t *conn)
{
    int bytes_read;

    while ((bytes_read = recv(conn->fd, conn->buffer + conn->buf_len,
                              sizeof(conn->buffer) - conn->buf_len - 1, MSG_DONTWAIT)) > 0)
    {
        conn->buf_len += bytes_read;
        conn->buffer[conn->buf_len] = '\0';

        if (conn->state == 0)
        { // Reading headers
            char *headers_end = strstr(conn->buffer, "\r\n\r\n");

            if (headers_end)
            {
                char method[8], path[1024];
                sscanf(conn->buffer, "%s %s", method, path);

                if (strcmp(method, "GET") == 0)
                {
                    conn->state = 2; // Move to response phase
                    send_file_nonblocking(conn, path);
                    return CONN_ALIVE;
                }
                else if (strcmp(method, "PUT") == 0)
                {
                    char *cl_header = strstr(conn->buffer, "Content-Length: ");
                    if (!cl_header)
                    {
                        send_response(conn->fd, "HTTP/1.1 411 Length Required", "text/plain",
                                      "Content-Length required.");
                        return CONN_CLOSED_OR_ERROR;
                    }

                    conn->expected_body_length = atoi(cl_header + strlen("Content-Length: "));
                    conn->received_body_length = 0;
                    conn->state = 1; // Move to body reading phase

                    char file_path[1024];
                    snprintf(file_path, sizeof(file_path), "%s/uploads%s", ROOT, path + 7);
                    conn->file = fopen(file_path, "wb");

                    if (!conn->file)
                    {
                        send_response(conn->fd, "HTTP/1.1 500 Internal Server Error", "text/plain",
                                      "File creation failed.");
                        return CONN_CLOSED_OR_ERROR;
                    }

                    char *body_start = headers_end + 4;
                    int initial_body_length = conn->buf_len - (body_start - conn->buffer);
                    fwrite(body_start, 1, initial_body_length, conn->file);
                    conn->received_body_length += initial_body_length;
                }
            }
        }

        if (conn->state == 1)
        { // Reading body for PUT
            while (conn->received_body_length < conn->expected_body_length)
            {
                bytes_read = recv(conn->fd, conn->buffer, BUFFER_SIZE, MSG_DONTWAIT);
                if (bytes_read <= 0)
                    break;

                fwrite(conn->buffer, 1, bytes_read, conn->file);
                conn->received_body_length += bytes_read;
            }

            if (conn->received_body_length >= conn->expected_body_length)
            {
                fclose(conn->file);
                send_response(conn->fd, "HTTP/1.1 200 OK", "text/plain", "Upload complete.");
                return CONN_CLOSED_OR_ERROR;
            }
        }
    }

    if (bytes_read == 0)
        return CONN_CLOSED_OR_ERROR;
    if (bytes_read < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return CONN_ALIVE;

    return CONN_CLOSED_OR_ERROR;
}

void send_file_nonblocking(connection_t *conn, const char *path)
{
    char full_path[1024];
    snprintf(full_path, sizeof(full_path), "%s%s", ROOT, path);

    FILE *file = fopen(full_path, "rb");
    if (!file)
    {
        send_response(conn->fd, "HTTP/1.1 404 Not Found", "text/plain", "File not found.");
        return;
    }

    // Get file size
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    rewind(file);

    // Send headers
    char header[BUFFER_SIZE];
    snprintf(header, sizeof(header),
             "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\n\r\n", file_size);
    send(conn->fd, header, strlen(header), MSG_DONTWAIT);

    // Read and send in chunks
    char buffer[BUFFER_SIZE];
    size_t bytes_read;
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0)
    {
        send(conn->fd, buffer, bytes_read, MSG_DONTWAIT);
    }
    fclose(file);
}