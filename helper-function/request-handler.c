// request_handler.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "request-handler.h"

#define ROOT "/var/www/html"
#define ROOT2 "/var/www"
#define BUFFER_SIZE 4096

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

void handle_put(int client_socket, const char *path)
{
    char buffer[BUFFER_SIZE];
    FILE *file;

    // Check if the path starts with "/uploads"
    if (strncmp(path, "/upload", 9) != 0)
    {
        send_response(client_socket, "HTTP/1.1 400 Bad Request", "text/plain", "Invalid upload path.");
        return;
    }

    // save the file under ROOT2/uploads/
    snprintf(buffer, sizeof(buffer), "%s%s/s", ROOT2, path);
    printf("Trying to create file at: %s\n", buffer);
    file = fopen(buffer, "wb");

    if (file == NULL)
    {
        perror("Error creating file");
        send_response(client_socket, "HTTP/1.1 500 Internal Server Error", "text/plain", "Error creating file.");
        return;
    }

    // Read the body of the PUT request and write it to the file
    int bytes_read;
    while ((bytes_read = read(client_socket, buffer, sizeof(buffer))) > 0)
    {
        fwrite(buffer, 1, bytes_read, file);
    }

    fclose(file);
    send_response(client_socket, "HTTP/1.1 200 OK", "text/plain", "File uploaded successfully.");
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
    printf("Received request:\n%s\n", req_buffer);

    // Extract method and path from the request string
    char method[8];
    char path[1024];
    sscanf(req_buffer, "%s %s", method, path);

    // Handle GET method
    if (strcmp(method, "GET") == 0)
    {
        handle_get(client_socket, path);
    }
    // Handle PUT method
    else if (strcmp(method, "PUT") == 0)
    {
        handle_put(client_socket, path);
    }
    else
    {
        // Unsupported method
        send_response(client_socket, "HTTP/1.1 405 Method Not Allowed", "text/plain", "Method Not Allowed.");
    }
}
