// request_handler.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include "request-handler.h"

#include <sys/sendfile.h>

#include <malloc.h>   // for memalign
#include <sys/mman.h> // for memory alignment

void *allocate_aligned_buffer(size_t size)
{
    void *ptr;
    if (posix_memalign(&ptr, BLOCK_SIZE, size) != 0)
    {
        return NULL;
    }
    return ptr;
}

off_t get_file_size(int fd)
{
    struct stat st;
    if (fstat(fd, &st) == -1)
    {
        return -1;
    }
    return st.st_size;
}

// round down to block size
size_t align_to_block(size_t size)
{
    return (size / BLOCK_SIZE) * BLOCK_SIZE;
}

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
    int file_fd;
    snprintf(full_path, sizeof(full_path), "%s%s", ROOT, path);

    // Check if the path is just "/"
    if (strcmp(path, "/") == 0)
    {
        snprintf(full_path, sizeof(full_path), "%s/server-index.html", ROOT);
    }

    file_fd = open(full_path, O_RDONLY | O_DIRECT);
    if (file_fd == -1)
    {
        send_response(client_socket, "HTTP/1.1 404 Not Found", "text/plain", "File not found.");
        return;
    }

    // Determine the file size
    // fseek(file, 0, SEEK_END);
    // long file_size = ftell(file);
    // rewind(file);
    off_t file_size = get_file_size(file_fd);
    if (file_size == -1)
    {
        close(file_fd);
        send_response(client_socket, "HTTP/1.1 500 Internal Server Error", "text/plain", "Could not get file size.");
        return;
    }

    // Get the MIME type
    const char *mime_type = get_mime_type(full_path);
    printf("mime_type is: %s\n", mime_type);

    // Construct and send the HTTP header
    char header[BUFFER_SIZE];
    snprintf(header, sizeof(header),
             "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %ld\r\n\r\n",
             mime_type, file_size);
    send(client_socket, header, strlen(header), 0);

    // Allocate aligned buffer
    char *buffer = allocate_aligned_buffer(BUFFER_SIZE);
    if (!buffer)
    {
        close(file_fd);
        send_response(client_socket, "HTTP/1.1 500 Internal Server Error", "text/plain", "Memory allocation failed.");
        return;
    }

    off_t remaining = file_size;
    while (remaining > 0)
    {
        size_t to_read = (remaining > BUFFER_SIZE) ? BUFFER_SIZE : remaining;
        to_read = align_to_block(to_read); // Ensure aligned size
        if (to_read == 0)
            to_read = BLOCK_SIZE; // Handle very small files

        ssize_t bytes_read = read(file_fd, buffer, to_read);
        if (bytes_read == -1)
        {
            perror("Error reading file");
            break;
        }

        ssize_t sent = send(client_socket, buffer, bytes_read, 0);
        if (sent == -1)
        {
            perror("Error sending data");
            break;
        }

        remaining -= bytes_read;
    }

    free(buffer);
    close(file_fd);
}

void handle_put(int client_socket, const char *path, int content_length, const char *initial_body, int initial_body_length)
{
    // char buffer[BUFFER_SIZE];
    char file_path[2048];
    int file_fd;

    // Check if the path starts with "/upload"
    if (strncmp(path, "/upload", 7) != 0)
    {
        send_response(client_socket, "HTTP/1.1 400 Bad Request", "text/plain", "Invalid upload path.");
        return;
    }

    // constructing file path under ROOT/uploads/
    snprintf(file_path, sizeof(file_path), "%s/uploads%s", ROOT, path + 7);
    printf("Trying to create file at: %s\n", file_path);

    // open file for writing
    file_fd = open(file_path, O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT, 0644);
    if (file_fd == -1)
    {
        perror("Error creating file");
        send_response(client_socket, "HTTP/1.1 500 Internal Server Error", "text/plain", "Error creating file.");
        return;
    }

    // allocate aligned buffer
    char *aligned_buf = allocate_aligned_buffer(BUFFER_SIZE);
    if (!aligned_buf)
    {
        close(file_fd);
        send_response(client_socket, "HTTP/1.1 500 Internal Server Error", "text/plain", "Memory allocation failed.");
        return;
    }

    // write initial body data if present
    int total_received = 0;

    if (initial_body && initial_body_length > 0)
    {
        // Copy to aligned buffer in chunks
        const char *src = initial_body;
        size_t remaining = initial_body_length;

        while (remaining > 0)
        {
            size_t chunk = (remaining > BUFFER_SIZE) ? BUFFER_SIZE : remaining;
            size_t copy_size = chunk;

            // For the last chunk that might need padding
            if (chunk < BLOCK_SIZE && remaining == chunk)
            {
                memset(aligned_buf, 0, BLOCK_SIZE); // Zero out the block first
                copy_size = chunk;                  // Only copy actual data
            }

            memcpy(aligned_buf, src, copy_size);

            size_t to_write = align_to_block(chunk);
            if (to_write == 0)
                to_write = BLOCK_SIZE;

            ssize_t written = write(file_fd, aligned_buf, to_write);
            if (written == -1)
            {
                perror("Error writing initial data");
                free(aligned_buf);
                close(file_fd);
                send_response(client_socket, "HTTP/1.1 500 Internal Server Error",
                              "text/plain", "Error writing to file.");
                return;
            }

            total_received += chunk;
            src += chunk;
            remaining -= chunk;
        }
    }

    // Read remaining data
    int remaining = content_length - total_received;
    while (remaining > 0)
    {
        size_t to_read = (remaining > BUFFER_SIZE) ? BUFFER_SIZE : remaining;
        // Don't align reads - we'll handle alignment in the write
        ssize_t bytes_read = read(client_socket, aligned_buf, to_read);
        if (bytes_read <= 0)
        {
            perror("Error reading from client");
            break;
        }

        // Handle partial block at end of transfer
        if (bytes_read < BLOCK_SIZE &&
            (total_received + bytes_read) == content_length)
        {
            memset(aligned_buf + bytes_read, 0, BLOCK_SIZE - bytes_read);
            size_t to_write = BLOCK_SIZE;

            if (write(file_fd, aligned_buf, to_write) == -1)
            {
                perror("Error writing final block");
                break;
            }
        }
        else
        {
            // Normal aligned write
            size_t to_write = align_to_block(bytes_read);
            if (to_write == 0)
                to_write = BLOCK_SIZE;

            if (write(file_fd, aligned_buf, to_write) == -1)
            {
                perror("Error writing to file");
                break;
            }
        }

        total_received += bytes_read;
        remaining -= bytes_read;
    }

    free(aligned_buf);
    close(file_fd);

    // verify if all data was received
    if (total_received == content_length)
    {
        // fsync(file_fd);
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

int verify_alignment(conn_state *conn)
{
    // Verify buffer alignment
    if ((uintptr_t)conn->buffer % BLOCK_SIZE != 0)
    {
        perror("Error: Buffer not aligned to block size ");
        return CONN_CLOSED_OR_ERROR;
    }

    // verify offset alignment
    if (conn->byte_offset % BLOCK_SIZE != 0)
    {
        fprintf(stderr, "Error: Offset %ld not aligned to block size %d\n",
                conn->byte_offset, BLOCK_SIZE);
        return CONN_CLOSED_OR_ERROR;
    }
    return 2;
}

int handle_partial_with_padding(conn_state *conn)
{
    size_t remaining = conn->file_size - conn->byte_offset;
    // printf("File Size= %ld, offset= %ld, remaining= %zu\n", conn->file_size, conn->byte_offset, remaining);

    const size_t padding_needed = BLOCK_SIZE - remaining;

    if (verify_alignment(conn) != 2)
    {
        return CONN_CLOSED_OR_ERROR;
    }

    ssize_t bytes_read = pread(conn->file_fd, conn->buffer, BLOCK_SIZE, conn->byte_offset);
    if (bytes_read == -1)
    {
        printf("Read failed at offset %ld \n", conn->byte_offset);
        perror("PREAD FAILED: ");
        return CONN_CLOSED_OR_ERROR;
    }

    // get the actual data from padded
    size_t real_bytes = BLOCK_SIZE - padding_needed; // bytes_read > remaining ? remaining : bytes_read;

    printf("real_bytes= %zd, bytes_read= %zd, actual_bytes_left_tosend= %zd\n", real_bytes, bytes_read, remaining);

    ssize_t sent = send(conn->fd, conn->buffer, real_bytes, 0);
    if (sent == -1)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            return CONN_ALIVE;
        }
        perror("Error sending data");
        return CONN_CLOSED_OR_ERROR;
    }
    conn->byte_offset += real_bytes;
    if (conn->byte_offset >= conn->file_size)
    {
        close(conn->file_fd);
        printf("PARTIALFILE SENT SUCCESSFULLY\n");
        return CONN_CLOSED_OR_ERROR;
    }
    return CONN_ALIVE;
}

ssize_t write_fully(int fd, const void *buf, size_t count)
{
    ssize_t written = 0;
    while (written < count)
    {
        ssize_t n = write(fd, (char *)buf + written, count - written);
        if (n == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                continue; // Retry on non-blocking
            }
            perror("Error in writing");
            return -1; // Real error
        }
        written += n;
    }
    return written;
}
int handle_requests_event_driven(conn_state *conn)
{

    ssize_t n;
    char method[16], path[1024];
    char full_path[2048];

    if (conn->state == READING_HEADER)
    {
        n = recv(conn->fd, conn->header_buffer + conn->bytes_read, BUFFER_SIZE - conn->bytes_read, 0);

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
        // conn->header_buffer[conn->bytes_read] = '\0';

        // if we have full header request
        if (strstr(conn->header_buffer, "\r\n\r\n"))
        {
            sscanf(conn->header_buffer, "%s %s", method, path);
            if (strcmp(method, "GET") == 0)
            {
                snprintf(full_path, sizeof(full_path), "%s%s", ROOT, path);
                if (strcmp(path, "/") == 0)
                {
                    snprintf(full_path, sizeof(full_path), "%s/server-index.html", ROOT);
                }

                // open w O_DIRECT
                conn->file_fd = open(full_path, O_RDONLY | O_DIRECT);
                if (conn->file_fd == -1)
                {
                    perror("File not found");
                    send_response(conn->fd, "HTTP/1.1 404 Not Found", "text/plain", "File not found");
                    return CONN_CLOSED_OR_ERROR;
                }

                // Get file size
                conn->file_size = get_file_size(conn->file_fd);
                if (conn->file_size == -1)
                {
                    send_response(conn->fd, "HTTP/1.1 500 Internal Server Error", "text/plain", "Could not get file size.");
                    return CONN_CLOSED_OR_ERROR;
                }

                conn->byte_offset = 0;

                const char *mime_type = get_mime_type(full_path);
                printf("mime_type is: %s\n", mime_type);

                char header[BUFFER_SIZE];
                snprintf(header, sizeof(header),
                         "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %ld\r\n\r\n",
                         mime_type, conn->file_size);

                send(conn->fd, header, strlen(header), 0);
                conn->state = HANDLING_GET;
                conn->last_op = OP_READ;
            }
            else if (strcmp(method, "PUT") == 0)
            {
                if (strncmp(path, "/upload", 7) != 0)
                {
                    send_response(conn->fd, "HTTP/1.1 400 Bad Request", "text/plain", "Invalid upload path.");
                    return CONN_CLOSED_OR_ERROR;
                }

                char *content_len_header = strstr(conn->header_buffer, "Content-Length: ");
                if (!content_len_header)
                {
                    send_response(conn->fd, "HTTP/1.1 411 Length Required", "text/plain", "Content-Length required.");
                    close(conn->file_fd);
                    close(conn->fd);
                    return CONN_CLOSED_OR_ERROR;
                }

                char file_path[2048];

                snprintf(file_path, sizeof(file_path), "%s/uploads%s", ROOT, path + 7);
                printf("Trying to create file at: %s\n", file_path);

                conn->file_fd = open(file_path, O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT, 0644);
                if (conn->file_fd == -1)
                {
                    perror("Error creating/opening the file");
                    printf("FD: %d\n", conn->fd);
                    send_response(conn->fd, "HTTP/1.1 500 Internal Server Error", "text/plain", "Error creating file.");
                    close(conn->file_fd);
                    close(conn->fd);
                    return CONN_CLOSED_OR_ERROR;
                }

                sscanf(content_len_header, "Content-Length: %ld", &conn->file_size);
                conn->byte_offset = 0;

                char *body_start = strstr(conn->header_buffer, "\r\n\r\n");
                if (!body_start)
                {
                    send_response(conn->fd, "HTTP/1.1 400 Bad Request", "text/plain", "Malformed headers.");
                    return CONN_CLOSED_OR_ERROR;
                }
                body_start += 4; // Skip past the "\r\n\r\n"
                size_t initial_body_len = conn->bytes_read - (body_start - conn->header_buffer);

                // write all of data we have rn
                size_t bytes_processed = 0;

                while (bytes_processed < initial_body_len)
                {
                    size_t remaining = initial_body_len - bytes_processed;
                    size_t chunk = remaining >= BLOCK_SIZE ? BLOCK_SIZE : remaining;
                    memcpy(conn->buffer, body_start + bytes_processed, chunk);
                    bytes_processed += chunk;

                    if (chunk < BLOCK_SIZE)
                    {
                        memset(conn->buffer + chunk, 0, BLOCK_SIZE - chunk);
                        conn->util_offset = chunk;
                        break;
                    }

                    size_t written = write_fully(conn->file_fd, conn->buffer, BLOCK_SIZE);
                    if (written == -1)
                        return CONN_CLOSED_OR_ERROR;
                    conn->byte_offset += written;
                }
                if (conn->util_offset + conn->byte_offset >= conn->file_size)
                {
                    ssize_t written = write_fully(conn->file_fd, conn->buffer, BLOCK_SIZE);
                    if (written == -1)
                        return CONN_CLOSED_OR_ERROR;
                    conn->byte_offset += written;
                    conn->util_offset = 0;
                }
                if (conn->byte_offset >= conn->file_size)
                {
                    send_response(conn->fd, "HTTP/1.1 201 Created", "text/plain", "File uploaded.");
                    close(conn->file_fd);
                    close(conn->fd);
                    return CONN_CLOSED_OR_ERROR;
                }

                conn->bytes_read = 0;
                // printf("PRE-WRITES: util_offset=%zd, file_offset=%zd\n", conn->util_offset, conn->byte_offset);

                conn->state = HANDLING_POST;
            }
        }
        else
        {
            send_response(conn->fd, "HTTP/1.1 405 Method Not Allowed", "text/plain", "Method Not Allowed.");
            close(conn->fd);
            return CONN_CLOSED_OR_ERROR;
        }
    }

    else if (conn->state == HANDLING_GET)
    {
        size_t remaining = conn->file_size - conn->byte_offset;
        if (remaining == 0)
        {
            // close(conn->file_fd);
            return CONN_CLOSED_OR_ERROR; // Normal EOF
        }
        if (remaining > 0 && remaining < BLOCK_SIZE)
        {
            int rc = handle_partial_with_padding(conn);
            return rc;
        }

        size_t to_read = (remaining > BUFFER_SIZE) ? BUFFER_SIZE : remaining;
        to_read = align_to_block(to_read);

        if (verify_alignment(conn) != 2)
        {
            return CONN_CLOSED_OR_ERROR;
        }
        ssize_t bytes_read = pread(conn->file_fd, conn->buffer, to_read, conn->byte_offset); // read(conn->file_fd, conn->buffer, to_read);

        if (bytes_read == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                return CONN_ALIVE;
            }
            perror("Error reading file");
            return CONN_CLOSED_OR_ERROR;
        }

        while (conn->util_offset < bytes_read)
        {
            ssize_t sent = send(conn->fd, conn->buffer + conn->util_offset,
                                bytes_read - conn->util_offset, 0);

            if (sent == -1)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    // Partial send: exit and wait for EPOLLOUT
                    return CONN_ALIVE;
                }
                perror("Error sending data");
                return CONN_CLOSED_OR_ERROR;
            }
            else
            {
                conn->util_offset += sent;
            }
        }

        // Reset for next block
        conn->byte_offset += bytes_read;
        conn->util_offset = 0;

        if (conn->byte_offset >= conn->file_size)
        {
            printf("FILE SENT SUCCESSFULLY\n");
            return CONN_CLOSED_OR_ERROR;
        }
    }
    else if (conn->state == HANDLING_POST)
    {
        //          while(1){
        n = recv(conn->fd, conn->header_buffer + conn->bytes_read,
                 sizeof(conn->header_buffer) - conn->bytes_read, 0);

        // printf("PRE-WRITES: n=%zd, util_offset=%zd, file_offset=%zd\n", n, conn->util_offset, conn->byte_offset);

        if (n < 0)
        {
            // printf("\n-----------------------N<0--------------------\n");
            // printf("LESS N: soccketconn=%d,  n=%zd, util_offset=%zd, file_offset=%zd, file_size=%zd\n",conn->fd, n, conn->util_offset, conn->byte_offset, conn->file_size);
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                if (conn->util_offset + conn->byte_offset >= conn->file_size)
                {
                    memset(conn->buffer + conn->util_offset, 0, BLOCK_SIZE - conn->util_offset);
                    ssize_t written = write_fully(conn->file_fd, conn->buffer, BLOCK_SIZE);
                    if (written == -1)
                        return CONN_CLOSED_OR_ERROR;
                    conn->byte_offset += conn->util_offset;
                    if (conn->byte_offset >= conn->file_size)
                    {
                        send_response(conn->fd, "HTTP/1.1 201 Created", "text/plain", "File uploaded.");
                        return CONN_CLOSED_OR_ERROR;
                    }
                    else
                    {
                        send_response(conn->fd, "HTTP/1.1 206 Internal Server Error", "text/plain", "File upload incomplete.");
                        return CONN_CLOSED_OR_ERROR;
                    }
                }
                return CONN_ALIVE;
            }
            if (conn->util_offset > 0)
            {
                // Pad & write the final block before returning
                memset(conn->buffer + conn->util_offset, 0, BLOCK_SIZE - conn->util_offset);
                ssize_t written = write_fully(conn->file_fd, conn->buffer, BLOCK_SIZE);
                if (written == -1)
                    return CONN_CLOSED_OR_ERROR;
                conn->byte_offset += conn->util_offset;
                if (conn->byte_offset >= conn->file_size)
                {
                    send_response(conn->fd, "HTTP/1.1 201 Created", "text/plain", "File uploaded.");
                    close(conn->file_fd);
                    close(conn->fd);
                    return CONN_CLOSED_OR_ERROR;
                }
                else
                {
                    send_response(conn->fd, "HTTP/1.1 206 Internal Server Error", "text/plain", "File upload incomplete.");
                    close(conn->file_fd);
                    close(conn->fd);
                    return CONN_CLOSED_OR_ERROR;
                }
            }

            perror("Error in recv()");
            send_response(conn->fd, "HTTP/1.1 500 Internal Server Error", "text/plain", "Error recieving data from client");
            close(conn->file_fd);
            close(conn->fd);
            return CONN_CLOSED_OR_ERROR;
        }
        else if (n == 0)
        {
            printf("\n-----------------------N=0--------------------\n");
            perror("Client closed connection");
            send_response(conn->fd, "HTTP/1.1 500 Internal Server Error", "text/plain", "CLient closed connection. Couldn't complete upload");
            close(conn->file_fd);
            close(conn->fd);
            return CONN_CLOSED_OR_ERROR;
        }

        conn->bytes_read += n;
        size_t bytes_processed = 0;

        // printf("BEF_WRITE-1: n= %zd, bytes_processed= %zd, remaining= %zd\n", n, bytes_processed, n - bytes_processed);
        if (conn->util_offset > 0)
        {
            size_t space_left = BLOCK_SIZE - conn->util_offset;
            size_t append_size = (n >= space_left) ? space_left : n;

            memcpy(conn->buffer + conn->util_offset, conn->header_buffer + bytes_processed, append_size);
            bytes_processed += append_size;

            if (conn->util_offset + append_size == BLOCK_SIZE)
            {
                ssize_t written = write_fully(conn->file_fd, conn->buffer, BLOCK_SIZE);
                if (written == -1)
                    return CONN_CLOSED_OR_ERROR;
                conn->byte_offset += written;
                conn->util_offset = 0;
                // printf("WRITE-1: n= %zd, bytes_processed= %zd, remaining= %zd, UTIL_OFFSET= %zd, OFFSET= %zd\n", n, bytes_processed, n - bytes_processed, conn->util_offset, conn->byte_offset);
            }
            else
            {
                conn->util_offset += append_size;
                return CONN_ALIVE;
                // continue;
            }
        }

        while (bytes_processed < n)
        {
            // printf("BEF_WRITE-2: n= %zd, bytes_processed= %zd, remaining= %zd\n", n, bytes_processed, n - bytes_processed);
            size_t remaining = n - bytes_processed;
            size_t chunk = remaining > BLOCK_SIZE ? BLOCK_SIZE : remaining;
            memcpy(conn->buffer, conn->header_buffer + bytes_processed, chunk);
            bytes_processed += chunk;

            if (chunk < BLOCK_SIZE)
            {
                memset(conn->buffer + chunk, 0, BLOCK_SIZE - chunk);
                conn->util_offset = chunk;
                break;
            }
            size_t written = write_fully(conn->file_fd, conn->buffer, BLOCK_SIZE);
            if (written == -1)
                return CONN_CLOSED_OR_ERROR;
            conn->byte_offset += written;
            // printf("WRITE-2: n= %zd, bytes_processed= %zd, remaining= %zd, UTIL_OFFSET= %zd, OFFSET= %zd\n\n", n, bytes_processed,  n - bytes_processed, conn->util_offset, conn->byte_offset);
            if (verify_alignment(conn) != 2)
            {
                printf("ERROR WRITE:n=%zd, bytes_left= %zd, written= %zd, OFFSET= %zd \n", n, n - bytes_processed, written, conn->byte_offset);
                return CONN_CLOSED_OR_ERROR;
            }
        }
        conn->bytes_read = n - bytes_processed;
        if (conn->util_offset + conn->byte_offset >= conn->file_size)
        {
            ssize_t written = write_fully(conn->file_fd, conn->buffer, BLOCK_SIZE);
            if (written == -1)
                return CONN_CLOSED_OR_ERROR;
            conn->byte_offset += written;
            conn->util_offset = 0;
        }
        // Upload complete
        if (conn->byte_offset >= conn->file_size)
        {
            send_response(conn->fd, "HTTP/1.1 201 Created", "text/plain", "File uploaded.");
            close(conn->file_fd);
            close(conn->fd);
            return CONN_CLOSED_OR_ERROR;
        }
    }

    else
    {
        perror("Error Completing the request");
        send_response(conn->fd, "HTTP/1.1 500 Internal Server Error", "text/plain", "Error completing the request.");
        close(conn->file_fd);
        close(conn->fd);
        return CONN_CLOSED_OR_ERROR;
    }

    return CONN_ALIVE;
}

void submit_close(struct io_uring *ring, conn_state *conn)
{
    if (!conn)
        return;
    if (conn->file_fd != -1)
    {
        close(conn->file_fd);
        conn->file_fd = -1;
    }
    if (conn->fd != -1)
    {
        close(conn->fd);
        conn->fd = -1;
    }
    if (conn->buffer)
    {
        free(conn->buffer);
    }
    free(conn);
}

void io_uring_func(struct io_uring *ring, conn_state *conn, uring_func_enum func)
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe)
    {
        perror("Error getting SQE");
        submit_close(ring, conn);
        return;
    }
    switch (func)
    {
    case READ_REQUEST:
        io_uring_prep_recv(sqe, conn->fd, conn->header_buffer + conn->bytes_read, BUFFER_SIZE - conn->bytes_read, 0);
        break;
    case WRITE_FILE:
        io_uring_prep_write(sqe, conn->file_fd, conn->buffer, BLOCK_SIZE, conn->byte_offset);
        break;
    case READ_FILE:
        io_uring_prep_read(sqe, conn->file_fd, conn->buffer, BLOCK_SIZE, conn->byte_offset);
        break;
    case SEND_FILE:
        size_t unsent = conn->byte_offset - conn->util_offset;

        // if bytes left to send is less than block size
        if (unsent > 0 && unsent < BLOCK_SIZE)
        {
            io_uring_prep_send(sqe, conn->fd, conn->buffer + (BLOCK_SIZE - unsent), unsent, 0);
        }
        else
        { // else send the whole buffer
            io_uring_prep_send(sqe, conn->fd, conn->buffer, BLOCK_SIZE, 0);
        }
        break;
    default:
        perror("Invalid uring call");

        break;
    }
    io_uring_sqe_set_data(sqe, conn);
    io_uring_submit(ring);
}

int handle_requests_uring(struct io_uring *ring, struct io_uring_cqe *cqe)
{
    conn_state *conn = (conn_state *)io_uring_cqe_get_data(cqe);
    int res = cqe->res;
    char method[16], path[1024];
    char full_path[2048];

    switch (conn->state)
    {
    case READING_HEADER:
        if (res < 0)
        {
            if (res == -EAGAIN || res == -EWOULDBLOCK)
            {
                io_uring_func(ring, conn, READ_REQUEST);
                return CONN_ALIVE;
            }

            return CONN_CLOSED_OR_ERROR;
        }
        else if (res == 0)
        {
            return CONN_CLOSED_OR_ERROR;
        }

        conn->bytes_read += res;
        if (strstr(conn->header_buffer, "\r\n\r\n"))
        {
            sscanf(conn->header_buffer, "%s %s", method, path);
            if (strcmp(method, "GET") == 0)
            {
                snprintf(full_path, sizeof(full_path), "%s%s", ROOT, path);
                if (strcmp(path, "/") == 0)
                {
                    snprintf(full_path, sizeof(full_path), "%s/server-index.html", ROOT);
                }

                // open w O_DIRECT
                conn->file_fd = open(full_path, O_RDONLY | O_DIRECT);
                if (conn->file_fd == -1)
                {
                    perror("File not found");
                    send_response(conn->fd, "HTTP/1.1 404 Not Found", "text/plain", "File not found");
                    return CONN_CLOSED_OR_ERROR;
                }

                // Get file size
                conn->file_size = get_file_size(conn->file_fd);
                if (conn->file_size == -1)
                {
                    send_response(conn->fd, "HTTP/1.1 500 Internal Server Error", "text/plain", "Could not get file size.");
                    return CONN_CLOSED_OR_ERROR;
                }

                const char *mime_type = get_mime_type(full_path);
                printf("mime_type is: %s\n", mime_type);

                char header[BUFFER_SIZE];
                snprintf(header, sizeof(header),
                         "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %ld\r\n\r\n",
                         mime_type, conn->file_size);
                send(conn->fd, header, strlen(header), 0);

                conn->byte_offset = 0;
                conn->bytes_read = 0;
                conn->util_offset = 0;
                conn->state = HANDLING_GET;
                io_uring_func(ring, conn, READ_FILE);
                conn->last_op = OP_READ;
            }
            else if (strcmp(method, "PUT") == 0)
            {
                if (strncmp(path, "/upload", 7) != 0)
                {
                    send_response(conn->fd, "HTTP/1.1 400 Bad Request", "text/plain", "Invalid upload path.");
                    return CONN_CLOSED_OR_ERROR;
                }

                char *content_len_header = strstr(conn->header_buffer, "Content-Length: ");
                if (!content_len_header)
                {
                    send_response(conn->fd, "HTTP/1.1 411 Length Required", "text/plain", "Content-Length required.");
                    return CONN_CLOSED_OR_ERROR;
                }
                char file_path[2048];

                snprintf(file_path, sizeof(file_path), "%s/uploads%s", ROOT, path + 7);
                printf("Trying to create file at: %s\n", file_path);

                conn->file_fd = open(file_path, O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT, 0644);
                if (conn->file_fd == -1)
                {
                    perror("Error creating/opening the file");
                    printf("FD: %d\n", conn->fd);
                    send_response(conn->fd, "HTTP/1.1 500 Internal Server Error", "text/plain", "Error creating file.");
                    return CONN_CLOSED_OR_ERROR;
                }
                sscanf(content_len_header, "Content-Length: %ld", &conn->file_size);
                conn->byte_offset = 0;

                char *body_start = strstr(conn->header_buffer, "\r\n\r\n");
                if (!body_start)
                {
                    send_response(conn->fd, "HTTP/1.1 400 Bad Request", "text/plain", "Malformed headers.");
                    return CONN_CLOSED_OR_ERROR;
                }
                body_start += 4; // Skip past the "\r\n\r\n"

                size_t initial_body_len = conn->bytes_read - (body_start - conn->header_buffer);

                // writing initial data
                if (initial_body_len > 0)
                {
                    conn->header_buffer_processed = body_start - conn->header_buffer; // so we only process after headers

                    size_t remaining = conn->bytes_read - conn->header_buffer_processed;
                    size_t chunk = remaining > BLOCK_SIZE ? BLOCK_SIZE : remaining;

                    memcpy(conn->buffer, conn->header_buffer + conn->header_buffer_processed, chunk);
                    conn->header_buffer_processed += chunk;
                    conn->state = HANDLING_POST;
                    io_uring_func(ring, conn, WRITE_FILE);
                    conn->last_op = OP_WRITE;
                }
                else
                {
                    conn->bytes_read = 0;
                    conn->state = HANDLING_POST;
                    io_uring_func(ring, conn, READ_REQUEST);
                    conn->last_op = OP_READ;
                }
            }
            else
            {
                send_response(conn->fd, "HTTP/1.1 405 Method Not Allowed", "text/plain", "Method Not Allowed.");
                return CONN_CLOSED_OR_ERROR;
            }
        }
        break;
    case HANDLING_GET:
        if (conn->byte_offset >= conn->file_size && conn->util_offset == conn->byte_offset)
        {
            printf("FILE SENT SUCCESSFULLY\n");
            return CONN_CLOSED_OR_ERROR;
        }
        if (conn->last_op == OP_READ)
        {
            // res is from the read req
            if (res == -1)
            {
                perror("PREAD FAILED: ");
                return CONN_CLOSED_OR_ERROR;
            }
            if (res == 0)
            {
                if (conn->util_offset < conn->byte_offset)
                {
                    // send last buffer
                    io_uring_func(ring, conn, SEND_FILE);
                    conn->last_op = OP_WRITE;
                    return CONN_ALIVE;
                }
                return CONN_CLOSED_OR_ERROR;
            }
            conn->byte_offset += res; // to track what we've read

            // send what we just read
            io_uring_func(ring, conn, SEND_FILE);
            conn->last_op = OP_WRITE;
        }
        else
        {
            // res is from the send req
            if (res == -1)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    io_uring_func(ring, conn, READ_FILE);
                    return CONN_ALIVE;
                }
                perror("Error sending data");
                return CONN_CLOSED_OR_ERROR;
            }
            conn->util_offset += res; // to track what we've sent
            if (conn->byte_offset >= conn->file_size && conn->util_offset == conn->byte_offset)
            {
                // last offset was read and sent.
                printf("FILE SENT SUCCESSFULLY\n");
                return CONN_CLOSED_OR_ERROR;
            }

            io_uring_func(ring, conn, READ_FILE);
            conn->last_op = OP_READ;
        }
        break;
    case HANDLING_POST:

        if (conn->last_op == OP_READ)
        {
            printf("READ: n=%d, util_offset=%zd, file_offset=%zd, file_size=%zd, las_op=%d\n", res, conn->util_offset, conn->byte_offset, conn->file_size, conn->last_op);
            // res is from the req reads
            if (res < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    if (conn->byte_offset >= conn->file_size)
                    {
                        send_response(conn->fd, "HTTP/1.1 201 Created", "text/plain", "File uploaded.");
                        return CONN_CLOSED_OR_ERROR;
                    }
                    if (conn->util_offset > 0 && conn->util_offset + conn->byte_offset >= conn->file_size)
                    {
                        memset(conn->buffer + conn->util_offset, 0, BLOCK_SIZE - conn->util_offset);
                        io_uring_func(ring, conn, WRITE_FILE);
                        conn->last_op = OP_WRITE;
                        conn->util_offset = 0;
                    }
                    return CONN_ALIVE;
                }
                // write final chunk if any
                if (conn->util_offset > 0 && conn->util_offset + conn->byte_offset >= conn->file_size)
                {
                    memset(conn->buffer + conn->util_offset, 0, BLOCK_SIZE - conn->util_offset);
                    io_uring_func(ring, conn, WRITE_FILE);
                    conn->last_op = OP_WRITE;
                    conn->util_offset = 0;
                    return CONN_ALIVE;
                }
                printf("error in recv");
                return CONN_CLOSED_OR_ERROR;
            }
            if (res == 0)
            {
                if (conn->util_offset > 0 && conn->util_offset + conn->byte_offset >= conn->file_size)
                {
                    memset(conn->buffer + conn->util_offset, 0, BLOCK_SIZE - conn->util_offset);
                    io_uring_func(ring, conn, WRITE_FILE);
                    conn->last_op = OP_WRITE;
                    conn->util_offset = 0;
                    return CONN_ALIVE;
                }
                if (conn->byte_offset >= conn->file_size)
                {
                    send_response(conn->fd, "HTTP/1.1 201 Created", "text/plain", "File uploaded.");
                    return CONN_CLOSED_OR_ERROR;
                }
                return CONN_CLOSED_OR_ERROR;
            }
            conn->bytes_read += res;

            size_t remaining = conn->bytes_read - conn->header_buffer_processed;
            size_t chunk = remaining > BLOCK_SIZE ? BLOCK_SIZE : remaining;

            if (conn->util_offset > 0)
            {
                size_t space_left = BLOCK_SIZE - conn->util_offset;
                size_t append_size = (chunk >= space_left) ? space_left : chunk;
                memcpy(conn->buffer + conn->util_offset, conn->header_buffer, append_size);
                conn->header_buffer_processed += append_size;
                if (conn->util_offset + append_size == BLOCK_SIZE)
                {
                    io_uring_func(ring, conn, WRITE_FILE);
                    conn->last_op = OP_WRITE;
                    conn->util_offset = 0;
                }
                else
                {
                    conn->util_offset += append_size;
                    io_uring_func(ring, conn, READ_REQUEST);
                    conn->last_op = OP_READ;
                }
                return CONN_ALIVE;
            }
            memcpy(conn->buffer, conn->header_buffer + conn->header_buffer_processed, chunk);
            conn->header_buffer_processed += chunk;
            io_uring_func(ring, conn, WRITE_FILE);
            conn->last_op = OP_WRITE;
        }
        else
        {
            printf("WRITTEN: n=%d, util_offset=%zd, file_offset=%zd, file_size=%zd, las_op=%d, header_buffer_processed=%zd, bytes_read=%zd \n", res, conn->util_offset, conn->byte_offset, conn->file_size, conn->last_op, conn->header_buffer_processed, conn->bytes_read);
            // res is from the writes
            if (res < 0)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                {
                    if (conn->byte_offset >= conn->file_size)
                    {
                        send_response(conn->fd, "HTTP/1.1 201 Created", "text/plain", "File uploaded.");
                        return CONN_CLOSED_OR_ERROR;
                    }
                    if (conn->util_offset > 0 && conn->util_offset + conn->byte_offset >= conn->file_size)
                    {
                        memset(conn->buffer + conn->util_offset, 0, BLOCK_SIZE - conn->util_offset);
                        io_uring_func(ring, conn, WRITE_FILE);
                        conn->last_op = OP_WRITE;
                        conn->util_offset = 0;
                    }
                    return CONN_ALIVE;
                }
                perror("Write Failed");
                return CONN_CLOSED_OR_ERROR;
            }

            if (res == 0)
            {
                perror("BUFFER is 0");
                // keep reading
                conn->bytes_read -= conn->header_buffer_processed;
                io_uring_func(ring, conn, READ_REQUEST);
                conn->last_op = OP_READ;
                return CONN_ALIVE;
            }
            conn->byte_offset += res; // bytes written to file

            if (conn->byte_offset >= conn->file_size)
            {
                send_response(conn->fd, "HTTP/1.1 201 Created", "text/plain", "File uploaded.");
                return CONN_CLOSED_OR_ERROR;
            }

            size_t remaining = conn->bytes_read - conn->header_buffer_processed;
            size_t chunk = remaining > BLOCK_SIZE ? BLOCK_SIZE : remaining; // writing only this much

            memcpy(conn->buffer, conn->header_buffer + conn->header_buffer_processed, chunk);
            conn->header_buffer_processed += chunk;

            if (chunk == BLOCK_SIZE)
            {
                io_uring_func(ring, conn, WRITE_FILE);
                conn->last_op = OP_WRITE;
                return CONN_ALIVE;
            }
            else
            {
                memset(conn->buffer + chunk, 0, BLOCK_SIZE - chunk);
                conn->util_offset = chunk; // bytes used in buffer
                if (conn->util_offset + conn->byte_offset >= conn->file_size)
                {
                    memset(conn->buffer + conn->util_offset, 0, BLOCK_SIZE - conn->util_offset);
                    io_uring_func(ring, conn, WRITE_FILE);
                    conn->last_op = OP_WRITE;
                    conn->util_offset = 0;
                    return CONN_ALIVE;
                }
            }
            if (conn->bytes_read != 0 && conn->header_buffer_processed == conn->bytes_read)
            {
                // all bytes from header buffer processed
                conn->header_buffer_processed = 0;
                conn->bytes_read = 0;
                io_uring_func(ring, conn, READ_REQUEST);
                conn->last_op = OP_READ;
                return CONN_ALIVE;
            }

            // keep reading
            conn->bytes_read -= conn->header_buffer_processed;
            io_uring_func(ring, conn, READ_REQUEST);
            conn->last_op = OP_READ;
        }
        return CONN_ALIVE;
        break;
    default:
        perror("Error Completing the request");
        send_response(conn->fd, "HTTP/1.1 500 Internal Server Error", "text/plain", "Error completing the request.");
        return CONN_CLOSED_OR_ERROR;
    }
    return CONN_ALIVE;
}