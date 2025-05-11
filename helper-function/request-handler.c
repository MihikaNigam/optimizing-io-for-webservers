// request_handler.c
#include "request-handler.h"

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
    return ((size - 1) / MY_BLOCK_SIZE + 1) * MY_BLOCK_SIZE;
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

int handle_get_header(int client_socket, char *path, int *file_fd,
                      off_t *file_size)
{
    char full_path[2048];

    // check if the path is just "/"
    snprintf(full_path, sizeof(full_path), "%s%s", ROOT, path);
    if (strcmp(path, "/") == 0)
    {
        snprintf(full_path, sizeof(full_path), "%s/server-index.html", ROOT);
    }

    // open w O_DIRECT
    *file_fd = open(full_path, O_RDONLY | O_DIRECT);
    if (*file_fd == -1)
    {
        send_response(client_socket, "HTTP/1.1 404 Not Found", "text/plain", "File not found");
        return CONN_CLOSED;
    }

    // get file size
    *file_size = get_file_size(*file_fd);
    if (*file_size == -1)
    {
        perror("Couldnt get file size");
        send_response(client_socket, "HTTP/1.1 500 Internal Server Error", "text/plain", "Could not get file size.");
        return CONN_ERROR;
    }

    // get the MIME type
    const char *mime_type = get_mime_type(full_path);
    printf("mime_type is: %s\n", mime_type);

    // construct and send the HTTP header
    char header[BUFFER_SIZE];
    snprintf(header, sizeof(header),
             "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %ld\r\n\r\n",
             mime_type, *file_size);
    send(client_socket, header, strlen(header), 0);
    return CONN_ALIVE;
}

int handle_put_header(int client_socket, char *path, int *file_fd,
                      off_t *file_size, char *req_buffer)
{
    char file_path[2048];

    // Check if the path starts with "/upload"
    if (strncmp(path, "/upload", 7) != 0)
    {
        send_response(client_socket, "HTTP/1.1 400 Bad Request", "text/plain", "Invalid upload path.");
        return CONN_CLOSED;
    }

    // get content length
    char *cl_header = strstr(req_buffer, "Content-Length: ");
    if (!cl_header)
    {
        send_response(client_socket, "HTTP/1.1 411 Length Required", "text/plain", "Content-Length required.");
        return CONN_CLOSED;
    }
    sscanf(cl_header, "Content-Length: %ld", file_size);

    // constructing file path under ROOT/uploads/
    snprintf(file_path, sizeof(file_path), "%s/uploads%s", ROOT, path + 7);
    printf("Trying to create file at: %s\n", file_path);

    // open file for writing
    *file_fd = open(file_path, O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT, 0644);
    if (*file_fd == -1)
    {
        perror("Error creating file");
        return CONN_ERROR;
    }

    return CONN_ALIVE;
}

ssize_t send_fully(int fd, const void *buf, size_t to_send, server_type s_type)
{
    ssize_t sent = 0;
    while (sent < to_send)
    {
        ssize_t n = send(fd, (char *)buf + sent, to_send - sent, 0);
        if (n == -1)
        {
            if (errno == EINTR)
            {
                continue; // Retry
            }
            // for eagain and ewouldblock
            if (s_type == NON_BLOCKING && (errno == EAGAIN || errno == EWOULDBLOCK))
                return sent;
            return -1; // Real error
        }
        sent += n;
    }
    return sent;
}

ssize_t write_fully(int fd, const void *buf, size_t to_write, server_type s_type)
{
    ssize_t written = 0;
    to_write = align_to_block(to_write);
    while (written < to_write)
    {
        ssize_t n = write(fd, (char *)buf + written, to_write - written);
        if (n == -1)
        {
            if (errno == EINTR)
            {
                continue; // Retry
            }
            // for eagain and ewouldblock
            if (s_type == NON_BLOCKING && (errno == EAGAIN || errno == EWOULDBLOCK))
                return written;
            perror("Couldnt write: \n");
            return -1; // Real error
        }
        written += n;
    }
    return written;
}

int handle_blocking_requests(int client_socket, int *file_fd, char *req_buffer)
{
    off_t file_size;
    char method[8], path[1024];
    ssize_t n;

    // read incoming request
    n = recv(client_socket, req_buffer, BUFFER_SIZE, 0);
    if (n < 0)
    {
        perror("Client stopped sending");
        send_response(client_socket, "HTTP/1.1 400 Bad Request", "text/plain", "Malformed Request.");
        return CONN_CLOSED;
    }
    if (n == 0)
    {
        perror("Client disconnected");
        send_response(client_socket, "HTTP/1.1 400 Bad Request", "text/plain", "Client Disconnected");
        return CONN_CLOSED;
    }

    // if we have full header request
    if (strstr(req_buffer, "\r\n\r\n"))
    {

        // extract method and path from the request string
        sscanf(req_buffer, "%s %s", method, path);
        printf("Received request:\n%s %s\n", method, path);

        // Handle GET method
        if (strcmp(method, "GET") == 0)
        {
            int res = handle_get_header(client_socket, path, &file_fd,
                                        &file_size);
            if (res == CONN_ERROR)
            {
                perror("error completing get_header");
                return CONN_ERROR;
            }
            else if (res == CONN_CLOSED)
            {
                perror("issue in client's req");
                return CONN_CLOSED;
            }

            memset(req_buffer, 0, BUFFER_SIZE); // use the req_buffer as send buffer
            off_t byte_offset = 0;
            while (byte_offset < file_size)
            {
                size_t remaining = file_size - byte_offset;
                ssize_t bytes_read = pread(*file_fd, req_buffer, BUFFER_SIZE, byte_offset);
                if (bytes_read == -1)
                {
                    perror("PREAD FAILED in GET ");
                    return CONN_ERROR;
                }

                ssize_t sent = send_fully(client_socket, req_buffer, bytes_read, BLOCKING);
                if (sent == -1)
                {
                    perror("Error sending blocking data");
                    return CONN_ERROR;
                }
                printf("bytes_read=%zd, sent=%zd, file_size=%zd \n", bytes_read, sent, file_size);
                byte_offset += sent;
            }
            return CONN_CLOSED;
        }
        // Handle PUT method
        else if (strcmp(method, "PUT") == 0)
        {
            char *body_start = strstr(req_buffer, "\r\n\r\n");
            if (!body_start)
            {
                send_response(client_socket, "HTTP/1.1 400 Bad Request", "text/plain", "Malformed headers.");
                return CONN_CLOSED;
            }
            int res = handle_put_header(client_socket, path, file_fd, &file_size, req_buffer);
            if (res == CONN_ERROR)
            {
                perror("error completing put_header");
                return CONN_ERROR;
            }
            else if (res == CONN_CLOSED)
            {
                perror("issue in client's req");
                return CONN_CLOSED;
            }
            body_start += 4; // Skip past the "\r\n\r\n"
            size_t initial_body_len = n - (body_start - req_buffer);
            if (initial_body_len < 0)
                initial_body_len = 0;
            off_t byte_offset = 0;
            if (initial_body_len > 0)
            {
                // move the content to the start
                memmove(req_buffer, body_start, initial_body_len);

                if (initial_body_len >= file_size)
                {
                    ssize_t written = write_fully(*file_fd, req_buffer, BUFFER_SIZE, BLOCKING);
                    if (written == -1)
                        return CONN_ERROR;
                    byte_offset += written;

                    if (byte_offset >= file_size)
                    {
                        send_response(client_socket, "HTTP/1.1 201 Created", "text/plain", "File uploaded.");
                        return CONN_CLOSED;
                    }
                    return CONN_ERROR;
                }
                else
                {
                    // set rest values as 0
                    memset(req_buffer + initial_body_len, 0, BUFFER_SIZE - initial_body_len);
                }
            }
            size_t bytes_read = initial_body_len;
            printf("bytes_read=%zd, byte_offset=%zd, file_size=%zd \n", bytes_read, byte_offset, file_size);
            while (byte_offset < file_size)
            {
                size_t bytes_recvd = recv(client_socket, req_buffer + bytes_read, BUFFER_SIZE - bytes_read, 0);
                bytes_read += bytes_recvd;
                if (bytes_recvd < 0)
                {
                    perror("Client stopped sending");
                    send_response(client_socket, "HTTP/1.1 400 Bad Request", "text/plain", "Malformed Request.");
                    return CONN_CLOSED;
                }
                if (bytes_recvd == 0)
                {
                    perror("Client disconnected");
                    send_response(client_socket, "HTTP/1.1 400 Bad Request", "text/plain", "Client Disconnected");
                    return CONN_CLOSED;
                }
                if (bytes_read < BUFFER_SIZE)
                {
                    memset(req_buffer + bytes_read, 0, BUFFER_SIZE - bytes_read);
                    if (byte_offset + bytes_read < file_size)
                    {
                        continue;
                    }
                }

                ssize_t written = write_fully(*file_fd, req_buffer, BUFFER_SIZE, BLOCKING);
                if (written == -1)
                    return CONN_ERROR;
                byte_offset += written;
                printf("bytes_recvd=%zd, bytes_read=%zd, byte_offset=%zd, file_size=%zd \n", bytes_recvd, bytes_read, byte_offset, file_size);
                bytes_read = 0;
            }
            if (byte_offset >= file_size)
            {
                send_response(client_socket, "HTTP/1.1 201 Created", "text/plain", "File uploaded.");
                return CONN_CLOSED;
            }
            return CONN_ERROR;
        }

        else
        {
            // Unsupported method
            send_response(client_socket, "HTTP/1.1 405 Method Not Allowed", "text/plain", "Method Not Allowed.");
            return CONN_CLOSED;
        }
    }
    return CONN_CLOSED;
}

int verify_alignment(conn_state *conn)
{
    // Verify buffer alignment
    if ((uintptr_t)conn->buffer % BLOCK_SIZE != 0)
    {
        perror("Error: Buffer not aligned to block size ");
        return CONN_ERROR;
    }

    // verify offset alignment
    if (conn->byte_offset % BLOCK_SIZE != 0)
    {
        fprintf(stderr, "Error: Offset %ld not aligned to block size %d\n",
                conn->byte_offset, BLOCK_SIZE);
        return CONN_ERROR;
    }
    return 2;
}

int handle_requests_event_driven(conn_state *conn)
{
    char method[16], path[1024];
    ssize_t n;

    if (conn->state == READING_HEADER)
    {
        n = recv(conn->fd, conn->req_buffer + conn->bytes_read, BUFFER_SIZE - conn->bytes_read, 0);

        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return CONN_ALIVE;
            perror("Client stopped sending");
            send_response(conn->fd, "HTTP/1.1 400 Bad Request", "text/plain", "Malformed Request.");
            return CONN_CLOSED;
        }
        else if (n == 0)
        {
            perror("Client disconnected");
            send_response(conn->fd, "HTTP/1.1 400 Bad Request", "text/plain", "Client Disconnected");
            return CONN_CLOSED;
        }
        conn->bytes_read += n;

        // if we have full header request
        if (strstr(conn->req_buffer, "\r\n\r\n"))
        {
            sscanf(conn->req_buffer, "%s %s", method, path);
            printf("Received request:\n%s %s\n", method, path);

            // Handle GET method
            if (strcmp(method, "GET") == 0)
            {
                int res = handle_get_header(conn->fd, path, &conn->file_fd,
                                            &conn->file_size);
                if (res == CONN_ERROR)
                {
                    perror("error completing get_header");
                    return CONN_ERROR;
                }
                else if (res == CONN_CLOSED)
                {
                    perror("issue in client's req");
                    return CONN_CLOSED;
                }

                memset(conn->req_buffer, 0, BUFFER_SIZE); // use the req_buffer as send buffer
                conn->byte_offset = 0;
                conn->bytes_read = 0;
                conn->state = HANDLING_GET;
            }
            // Handle PUT method
            else if (strcmp(method, "PUT") == 0)
            {
                char *body_start = strstr(conn->req_buffer, "\r\n\r\n");
                if (!body_start)
                {
                    send_response(conn->fd, "HTTP/1.1 400 Bad Request", "text/plain", "Malformed headers.");
                    return CONN_CLOSED;
                }
                int res = handle_put_header(conn->fd, path, &conn->file_fd, &conn->file_size, conn->req_buffer);
                if (res == CONN_ERROR)
                {
                    perror("error completing put_header");
                    return CONN_ERROR;
                }
                else if (res == CONN_CLOSED)
                {
                    perror("issue in client's req");
                    return CONN_CLOSED;
                }
                body_start += 4; // Skip past the "\r\n\r\n"
                size_t initial_body_len = n - (body_start - conn->req_buffer);
                if (initial_body_len < 0)
                    initial_body_len = 0;
                if (initial_body_len > 0)
                {
                    // move the content to the start
                    memmove(conn->req_buffer, body_start, initial_body_len);
                    // set rest values as 0
                    memset(conn->req_buffer + initial_body_len, 0, BUFFER_SIZE - initial_body_len);

                    if (initial_body_len >= conn->file_size)
                    {
                        ssize_t written = write_fully(conn->file_fd, conn->req_buffer, BUFFER_SIZE, NON_BLOCKING);
                        if (written == -1)
                            return CONN_ERROR;
                        conn->byte_offset += written;

                        if (conn->byte_offset >= conn->file_size)
                        {
                            send_response(conn->fd, "HTTP/1.1 201 Created", "text/plain", "File uploaded.");
                            return CONN_CLOSED;
                        }
                        return CONN_ERROR;
                    }
                }

                conn->bytes_read = initial_body_len;
                conn->byte_offset = 0;
                conn->state = HANDLING_POST;
            }
            else
            {
                send_response(conn->fd, "HTTP/1.1 405 Method Not Allowed", "text/plain", "Method Not Allowed.");
                return CONN_CLOSED;
            }
        }
    }

    else if (conn->state == HANDLING_GET)
    {
        // bytes_read hold value for what we read last into buffer
        // util offset holds value for what we have send to client from what we last read. 0 means we've sent everything
        if (conn->util_offset == 0)
        {
            ssize_t bytes_read = pread(conn->file_fd, conn->req_buffer, BUFFER_SIZE, conn->byte_offset);
            if (bytes_read == -1)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    return CONN_ALIVE;

                perror("Error reading file");
                return CONN_ERROR;
            }
            conn->bytes_read = bytes_read;
            conn->byte_offset += bytes_read;
        }

        ssize_t sent = send_fully(conn->fd, conn->req_buffer + conn->util_offset, conn->bytes_read - conn->util_offset, NON_BLOCKING);
        if (sent == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return CONN_ALIVE;
            perror("Error sending blocking data");
            return CONN_ERROR;
        }
        conn->util_offset += sent;
        if (conn->util_offset < conn->bytes_read)
        {
            return CONN_ALIVE;
        }
        if (conn->byte_offset >= conn->file_size)
        {
            printf("FILE SENT SUCCESSFULLY\n");
            return CONN_CLOSED;
        }
        conn->util_offset = 0;
    }
    else if (conn->state == HANDLING_POST)
    {
        size_t n = recv(conn->fd, conn->req_buffer + conn->bytes_read, BUFFER_SIZE - conn->bytes_read, 0);
        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return CONN_ALIVE;
            perror("Client stopped sending");
            send_response(conn->fd, "HTTP/1.1 400 Bad Request", "text/plain", "Malformed Request.");
            return CONN_CLOSED;
        }
        if (n == 0)
        {
            perror("Client disconnected");
            send_response(conn->fd, "HTTP/1.1 400 Bad Request", "text/plain", "Client Disconnected");
            return CONN_CLOSED;
        }
        conn->bytes_read += n;

        if (conn->bytes_read < BUFFER_SIZE)
        {
            memset(conn->req_buffer + conn->bytes_read, 0, BUFFER_SIZE - conn->bytes_read);
            if (conn->byte_offset + conn->bytes_read < conn->file_size)
            {
                return CONN_ALIVE;
            }
        }

        ssize_t written = write_fully(conn->file_fd, conn->req_buffer, BUFFER_SIZE, NON_BLOCKING);
        if (written == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return CONN_ALIVE;
            return CONN_ERROR;
        }
        conn->byte_offset += written;
        conn->bytes_read = 0;

        if (conn->byte_offset >= conn->file_size)
        {
            send_response(conn->fd, "HTTP/1.1 201 Created", "text/plain", "File uploaded.");
            return CONN_CLOSED;
        }
    }
    else
    {
        perror("Incorect state to complete request");
        send_response(conn->fd, "HTTP/1.1 500 Internal Server Error", "text/plain", "Error completing the request.");
        return CONN_CLOSED;
    }
    return CONN_ALIVE;
}

int io_uring_func(struct io_uring *ring, conn_state *conn, uring_func_enum func)
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    if (!sqe)
    {
        perror("Error getting SQE");
        return CONN_ERROR;
    }
    switch (func)
    {
    case RECV_REQUEST:
        io_uring_prep_recv(sqe, conn->fd, conn->req_buffer + conn->bytes_read, BUFFER_SIZE - conn->bytes_read, 0);
        break;
    case WRITE_FILE:
        io_uring_prep_write(sqe, conn->file_fd, conn->req_buffer, BUFFER_SIZE, conn->byte_offset);
        break;
    case READ_FILE:
        io_uring_prep_read(sqe, conn->file_fd, conn->req_buffer, BUFFER_SIZE, conn->byte_offset);
        break;
    case SEND_FILE:
        io_uring_prep_send(sqe, conn->fd, conn->req_buffer + conn->util_offset, BUFFER_SIZE - conn->util_offset, 0);
        break;
    default:
        perror("Invalid uring call");
        return CONN_ERROR;
    }
    io_uring_sqe_set_data(sqe, conn);
    io_uring_submit(ring);
    return CONN_ALIVE;
}

int handle_requests_uring(struct io_uring *ring, conn_state *conn, ssize_t res)
{
    char method[16], path[1024];

    if (conn->state == READING_HEADER)
    {
        conn->bytes_read += res;
        // if we have full header request
        if (strstr(conn->req_buffer, "\r\n\r\n"))
        {
            sscanf(conn->req_buffer, "%s %s", method, path);
            printf("Received request:\n%s %s\n", method, path);

            if (strcmp(method, "GET") == 0)
            {
                int ret = handle_get_header(conn->fd, path, &conn->file_fd,
                                            &conn->file_size);
                if (ret == CONN_ERROR)
                {
                    perror("error completing get_header");
                    return CONN_ERROR;
                }
                else if (ret == CONN_CLOSED)
                {
                    perror("issue in client's req");
                    return CONN_CLOSED;
                }

                memset(conn->req_buffer, 0, BUFFER_SIZE); // use the req_buffer as send buffer
                conn->byte_offset = 0;
                conn->bytes_read = 0;
                conn->state = HANDLING_GET;
                conn->last_op = OP_READ;
                io_uring_func(ring, conn, READ_FILE);
            }
            else if (strcmp(method, "PUT") == 0)
            {
                char *body_start = strstr(conn->req_buffer, "\r\n\r\n");
                if (!body_start)
                {
                    send_response(conn->fd, "HTTP/1.1 400 Bad Request", "text/plain", "Malformed headers.");
                    return CONN_CLOSED;
                }
                int ret = handle_put_header(conn->fd, path, &conn->file_fd, &conn->file_size, conn->req_buffer);
                if (ret == CONN_ERROR)
                {
                    perror("error completing put_header");
                    return CONN_ERROR;
                }
                else if (ret == CONN_CLOSED)
                {
                    perror("issue in client's req");
                    return CONN_CLOSED;
                }
                body_start += 4; // Skip past the "\r\n\r\n"
                size_t initial_body_len = res - (body_start - conn->req_buffer);
                if (initial_body_len < 0)
                    initial_body_len = 0;
                if (initial_body_len > 0)
                {
                    memmove(conn->req_buffer, body_start, initial_body_len);
                    memset(conn->req_buffer + initial_body_len, 0, BUFFER_SIZE - initial_body_len);
                    if (initial_body_len >= conn->file_size)
                    {
                        // write what we have
                        conn->state = HANDLING_POST;
                        conn->last_op = OP_WRITE;
                        io_uring_func(ring, conn, WRITE_FILE);
                        return CONN_ALIVE;
                    }
                }
                conn->bytes_read = initial_body_len;
                conn->byte_offset = 0;
                conn->state = HANDLING_POST;
                conn->last_op = OP_READ; // from server
                io_uring_func(ring, conn, RECV_REQUEST);
            }
            else
            {
                send_response(conn->fd, "HTTP/1.1 405 Method Not Allowed", "text/plain", "Method Not Allowed.");
                return CONN_CLOSED;
            }
        }
        return CONN_ALIVE;
    }
    else if (conn->state == HANDLING_GET)
    {
        if (conn->last_op == OP_READ)
        {
            if (conn->util_offset == 0)
            {
                conn->bytes_read = res;
                conn->byte_offset += res;
            }
            conn->last_op = OP_WRITE;
            io_uring_func(ring, conn, SEND_FILE);
            return CONN_ALIVE;
        }
        else if (conn->last_op == OP_WRITE)
        {
            conn->util_offset += res;

            if (conn->util_offset < conn->bytes_read)
            {
                io_uring_func(ring, conn, SEND_FILE);
                return CONN_ALIVE;
            }
            if (conn->byte_offset >= conn->file_size)
            {
                printf("FILE SENT SUCCESSFULLY\n");
                return CONN_CLOSED;
            }
            conn->util_offset = 0;
            conn->last_op = OP_READ;
            io_uring_func(ring, conn, READ_FILE);
        }
        else
        {
            perror("incorrect last_op");
            return CONN_ERROR;
        }
    }
    else if (conn->state == HANDLING_POST)
    {
        if (conn->last_op == OP_READ)
        {
            conn->bytes_read += res;
            if (conn->bytes_read < BUFFER_SIZE)
            {
                // if we can read more into buffer and its still less than file size, keep reading to fill buffer
                memset(conn->req_buffer + conn->bytes_read, 0, BUFFER_SIZE - conn->bytes_read);
                if (conn->byte_offset + conn->bytes_read < conn->file_size)
                {
                    conn->last_op = OP_READ;
                    io_uring_func(ring, conn, RECV_REQUEST);
                    return CONN_ALIVE;
                }
            }
            // write what we just recvd from req
            conn->last_op = OP_WRITE;
            io_uring_func(ring, conn, WRITE_FILE);
            return CONN_ALIVE;
        }
        else if (conn->last_op == OP_WRITE)
        {
            conn->byte_offset += res;
            conn->bytes_read = 0; // to indicate read block you've written everything from buffer
            if (conn->byte_offset >= conn->file_size)
            {
                send_response(conn->fd, "HTTP/1.1 201 Created", "text/plain", "File uploaded.");
                return CONN_CLOSED;
            }

            // keep recving if bytes not complete
            conn->last_op = OP_READ;
            io_uring_func(ring, conn, RECV_REQUEST);
        }
        else
        {
            perror("incorrect last_op");
            return CONN_ERROR;
        }
    }
    else
    {
        perror("Incorect state to complete request");
        send_response(conn->fd, "HTTP/1.1 500 Internal Server Error", "text/plain", "Error completing the request.");
        return CONN_CLOSED;
    }

    return CONN_ALIVE;
}