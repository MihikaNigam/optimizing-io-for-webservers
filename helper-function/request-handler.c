// request_handler.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include "request-handler.h"
#include <sys/fcntl.h>
#include <sys/sendfile.h>

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
        // block other reads
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

int handle_requests_event_driven(conn_state *conn)
{

    ssize_t n;
    char method[16], path[1024];
    char full_path[2048];

    if (conn->state == READING_HEADER)
    {
        n = recv(conn->fd, conn->buffer + conn->bytes_read,
                 sizeof(conn->buffer) - conn->bytes_read, 0);

        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return CONN_ALIVE;
            return CONN_CLOSED_OR_ERROR;
        }
        else if (n == 0)
        {
            return CONN_CLOSED_OR_ERROR;
        }
        conn->bytes_read += n;

        // if we have full header request
        if (strstr(conn->buffer, "\r\n\r\n"))
        {
            sscanf(conn->buffer, "%s %s", method, path);
            if (strcmp(method, "GET") == 0)
            {
                snprintf(full_path, sizeof(full_path), "%s%s", ROOT, path);
                if (strcmp(path, "/") == 0)
                {
                    snprintf(full_path, sizeof(full_path), "%s/server-index.html", ROOT);
                }
                conn->file_fd = open(full_path, O_RDONLY);
                if (conn->file_fd == -1)
                {
                    perror("File not found");
                    send_response(conn->fd, "HTTP/1.1 404 Not Found", "text/plain", "File not found");
                    return CONN_CLOSED_OR_ERROR;
                }

                lseek(conn->file_fd, 0, SEEK_END);
                long file_size = lseek(conn->file_fd, 0, SEEK_CUR);
                lseek(conn->file_fd, 0, SEEK_SET);
                conn->file_size = file_size;
                conn->byte_offset = 0;

                const char *mime_type = get_mime_type(full_path);
                printf("mime_type is: %s\n", mime_type);

                char header[BUFFER_SIZE];
                snprintf(header, sizeof(header),
                         "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %ld\r\n\r\n",
                         mime_type, file_size);

                send(conn->fd, header, strlen(header), 0);
                conn->state = HANDLING_GET;
            }
            else if (strcmp(method, "PUT") == 0)
            {
                if (strncmp(path, "/upload", 7) != 0)
                {
                    send_response(conn->fd, "HTTP/1.1 400 Bad Request", "text/plain", "Invalid upload path.");
                    return CONN_CLOSED_OR_ERROR;
                }

                char *content_len_header = strstr(conn->buffer, "Content-Length: ");
                if (!content_len_header)
                {
                    send_response(conn->fd, "HTTP/1.1 411 Length Required", "text/plain", "Content-Length required.");
                    return CONN_CLOSED_OR_ERROR;
                }

                char file_path[2048];

                snprintf(file_path, sizeof(file_path), "%s/uploads%s", ROOT, path + 7);
                printf("Trying to create file at: %s\n", file_path);

                conn->file_fd = open(file_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (conn->file_fd == -1)
                {
                    perror("Error creating/opening the file");
                    send_response(conn->fd, "HTTP/1.1 500 Internal Server Error", "text/plain", "Error creating file.");
                    return CONN_CLOSED_OR_ERROR;
                }

                sscanf(content_len_header, "Content-Length: %ld", &conn->file_size);
                conn->byte_offset = 0;

                char *body_start = strstr(conn->buffer, "\r\n\r\n");
                if (!body_start)
                {
                    send_response(conn->fd, "HTTP/1.1 400 Bad Request", "text/plain", "Malformed headers.");
                    return CONN_CLOSED_OR_ERROR;
                }
                body_start += 4;                                                      // Skip past the "\r\n\r\n"
                size_t ini_body_len = conn->bytes_read - (body_start - conn->buffer); //(conn->buffer + conn->bytes_read) - body_start;
                if (ini_body_len > 0)
                {
                    ssize_t written = write(conn->file_fd, body_start, ini_body_len);
                    if (written == -1)
                    {
                        perror("Error writing initial content to file");
                        return CONN_CLOSED_OR_ERROR;
                    }
                    conn->byte_offset += written;

                    if (written < conn->bytes_read)
                    {
                        memmove(conn->buffer, conn->buffer + written, conn->bytes_read - written);
                    }
                    conn->bytes_read -= written;
                    printf("Bytes read: %zu, Bytes written: %zd, File offset: %ld\n", conn->bytes_read, written, conn->byte_offset);
                }

                conn->state = HANDLING_POST;
            }
            else
            {
                send_response(conn->fd, "HTTP/1.1 405 Method Not Allowed", "text/plain", "Method Not Allowed.");
            }
        }
    }

    else if (conn->state == HANDLING_GET)
    {
        ssize_t sent = sendfile(conn->fd, conn->file_fd, &conn->byte_offset, conn->file_size);

        if (sent == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                // Wait for next EPOLLOUT event
                return CONN_ALIVE;
            }
            return CONN_CLOSED_OR_ERROR;
        }

        if (conn->byte_offset >= conn->file_size)
        {
            close(conn->file_fd);
            return CONN_CLOSED_OR_ERROR;
        }
    }
    else if (conn->state == HANDLING_POST)
    {
        n = recv(conn->fd, conn->buffer + conn->bytes_read,
                 sizeof(conn->buffer) - conn->bytes_read, 0);

        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return CONN_ALIVE;
            return CONN_CLOSED_OR_ERROR;
        }

        conn->bytes_read += n;
        ssize_t remaining = conn->bytes_read;
        ssize_t written = 0;
        while (remaining > 0)
        {
            written = write(conn->file_fd, conn->buffer, remaining);
            if (written == -1)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    // Wait for the next EPOLLOUT event
                    return CONN_ALIVE;
                }
                perror("error writing during data reception");
                send_response(conn->fd, "HTTP/1.1 500 Internal Server Error", "text/plain", "Error writing to file.");
                return CONN_CLOSED_OR_ERROR;
            }
            printf("Bytes read: %zu, Bytes written: %zd, File offset: %ld\n", conn->bytes_read, written, conn->byte_offset);
            remaining -= written;
            memmove(conn->buffer, conn->buffer + written, remaining);
        }
        conn->bytes_read = remaining;
        conn->byte_offset += written;

        if (conn->byte_offset >= conn->file_size)
        {
            send_response(conn->fd, "HTTP/1.1 201 Created", "text/plain", "File uploaded");
            close(conn->file_fd);
            return CONN_CLOSED_OR_ERROR;
        }
    }
    else
    {
        perror("Error Completing the request");
        send_response(conn->fd, "HTTP/1.1 500 Internal Server Error", "text/plain", "Error completing the request.");
        return CONN_CLOSED_OR_ERROR;
    }

    return CONN_ALIVE;
}