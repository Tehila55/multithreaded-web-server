#include <stdio.h>      // printf, perror
#include <stdlib.h>     // exit, atoi, malloc, free
#include <string.h>     // strcmp, strtok, strlen, strchr
#include <unistd.h>     // read, write, close
#include <errno.h>      // errno
#include <time.h>       // time, strftime, gmtime
#include <sys/types.h>  // types for sockets
#include <sys/socket.h> // socket, bind, listen, accept
#include <netinet/in.h> // sockaddr_in
#include <arpa/inet.h>  // htonl, htons
#include <sys/stat.h>   // stat, S_ISDIR, S_ISREG
#include <dirent.h>     // opendir, readdir, scandir
#include "threadpool.h"
#include <time.h>
#include <stdint.h>
#include <fcntl.h>


#define RFC1123FMT "%a, %d %b %Y %H:%M:%S GMT" /* strftime format */
#define MAX_REQUEST_LINE 4000
#define SERVER_NAME "webserver/1.0"
#define HTTP_VERSION "HTTP/1.0"

/* Thread routine */
int handle_request(void* arg);
void send_file(int fd, const char* path);
void send_directory(int fd, const char* path);
char* get_mime_type(char* name);

/* Response builders */
void send_400(int fd);
void send_403(int fd);
void send_404(int fd);
void send_500(int fd);
void send_501(int fd);
void send_302(int fd, const char* path);

int main(int argc, char* argv[])
{
    int port;
    int pool_size;
    int max_requests;
    int max_queue_size;

    int listen_fd;
    struct sockaddr_in server_addr;

    /*  Parse command-line arguments */

    if (argc != 5) {
        fprintf(stderr,"Usage: %s <port> <pool-size> <max-queue-size> <max-number-of-requests>\n",argv[0]);       
        exit(1);
    }

   port = atoi(argv[1]);
   pool_size = atoi(argv[2]);
   max_queue_size = atoi(argv[3]);
   max_requests = atoi(argv[4]);

    /*Create listening socket*/
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        exit(1);
    }

    /*Bind socket to port */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(port);

    if (bind(listen_fd,(struct sockaddr*)&server_addr,sizeof(server_addr)) < 0) {
        perror("bind");
        close(listen_fd);
        exit(1);
    }

    /*  Start listening*/
    if (listen(listen_fd, 10) < 0) {
        perror("listen");
        close(listen_fd);
        exit(1);
    }

    /* Create thread pool*/
    threadpool* tp = create_threadpool(pool_size, max_queue_size);
    if (tp == NULL) {
        fprintf(stderr, "Failed to create thread pool\n");
        close(listen_fd);
        exit(1);
    }

    /* Accept loop*/
    for (int i = 0; i < max_requests; i++) {
        int client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) {
            perror("accept");
            continue;
        }

        /* Dispatch the request to the thread pool */
        dispatch(tp, handle_request, (void*)(intptr_t)client_fd);
    }

    /* Cleanup and shutdown*/
       
    close(listen_fd);
    destroy_threadpool(tp);

    return 0;
}


int handle_request(void* arg)
{
    /* Convert the generic argument to a socket file descriptor */
    int fd = (int)(intptr_t)arg;

    /* Buffer to store the first request line */
    char line[4096];
    size_t  idx = 0;
    char c;
    int bytes_read;

    /*Read the first request line */
    while (idx < sizeof(line) - 1) {
        bytes_read = read(fd, &c, 1);
        if (bytes_read <= 0) {
            send_400(fd);
            close(fd);
            return 0;
        }

        if (c == '\r') {
            bytes_read = read(fd, &c, 1);
            if (bytes_read <= 0) {
                send_400(fd);
                close(fd);
                return 0;
            }
            if (c == '\n') {
                break; /* End of first line */
            }
        }

        line[idx++] = c;
    }
    line[idx] = '\0';

    /*Parse the request line*/
    char *method;
    char *path;
    char *protocol;

    method = strtok(line, " ");
    path = strtok(NULL, " ");
    protocol = strtok(NULL, " ");

    /* Request line must contain exactly 3 parts */
    if (method == NULL || path == NULL || protocol == NULL) {
        send_400(fd);
        close(fd);
        return 0;
    }

    /* Validate protocol*/
    if (strcmp(protocol, "HTTP/1.0") != 0 &&
        strcmp(protocol, "HTTP/1.1") != 0) {
        send_400(fd);
        close(fd);
        return 0;
    }

    /*  Validate method*/
    if (strcmp(method, "GET") != 0) {
        send_501(fd);
        close(fd);
        return 0;
    }

    /*Build filesystem path */
    char real_path[4096];
    snprintf(real_path, sizeof(real_path), ".%s", path);

    /* Query filesystem (stat) */
    struct stat st;
    if (stat(real_path, &st) < 0) {
        send_404(fd);
        close(fd);
        return 0;
    }

    /*Handle directories*/
    if (S_ISDIR(st.st_mode)) {
        /* Directory requested without trailing slash */
        if (path[strlen(path) - 1] != '/') {
            send_302(fd, path);
            close(fd);
            return 0;
        }

        send_directory(fd, real_path);
        close(fd);
        return 0;
    }

    /*  Handle regular files*/
    if (S_ISREG(st.st_mode)) {
        /* Check read permission for others */
        if (!(st.st_mode & S_IROTH)) {
            send_403(fd);
            close(fd);
            return 0;
        }

        send_file(fd, real_path);
        close(fd);
        return 0;
    }

    /*Anything else is forbidden */
    send_403(fd);
    close(fd);
    return 0;
}
void send_file(int fd, const char* path)
{
    
    int file_fd;     /* File descriptor for the requested file */
    struct stat st;  /* Structure to hold file information (size, last modified time, etc.) */
    char buffer[4096];   /* Buffer used to read file content in chunks */
    ssize_t bytes_read;   /* Number of bytes read from the file */

    /*  Get information about the file*/ 

    /* Ask the operating system for file metadata */
    if (stat(path, &st) < 0) {
        /* If stat fails here, it is an internal server error */
        send_500(fd);
        return;
    }
    /*Open the file for reading */
    file_fd = open(path, O_RDONLY);   /* Open the file in read-only mode */
    /* Check if opening the file failed */
    if (file_fd < 0) {
        /* Failed to open file -> internal server error */
        send_500(fd);
        return;
    }

    /* Prepare HTTP date strings*/
    /* Buffers for HTTP Date and Last-Modified headers */
    char datebuf[128];
    char modbuf[128];

    /* Get the current time */
    time_t now = time(NULL);

    /* Format current time as HTTP Date (RFC1123, GMT) */
    strftime(datebuf, sizeof(datebuf),"%a, %d %b %Y %H:%M:%S GMT",gmtime(&now));

    /* Format file last modification time as HTTP Last-Modified */
    strftime(modbuf, sizeof(modbuf),"%a, %d %b %Y %H:%M:%S GMT",gmtime(&st.st_mtime));

    /* Determine Content-Type*/
    /* Get MIME type based on file extension */
    const char* content_type = get_mime_type((char*)path);

    /* Build and send HTTP headers*/
    /* Buffer for HTTP response headers */
    char header[1024];

    /* Build the HTTP 200 OK response headers */
    if (content_type != NULL) {
         /* Build headers WITH Content-Type */
        snprintf(header, sizeof(header),
        "HTTP/1.0 200 OK\r\n"
        "Server: webserver/1.0\r\n"
        "Date: %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Last-Modified: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        datebuf,
        content_type,
        st.st_size,
        modbuf);
    } else {
    /* Build headers WITHOUT Content-Type */
    snprintf(header, sizeof(header),
        "HTTP/1.0 200 OK\r\n"
        "Server: webserver/1.0\r\n"
        "Date: %s\r\n"
        "Content-Length: %ld\r\n"
        "Last-Modified: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        datebuf,
        st.st_size,
        modbuf);
    }
    write(fd, header, strlen(header)); //Send the HTTP headers to the client 

    /* Send file content*/
    /* Read the file in chunks and send each chunk to the client */
    while ((bytes_read = read(file_fd, buffer, sizeof(buffer))) > 0) {
        /* Write the chunk to the client socket */
        write(fd, buffer, bytes_read);
    }

    /*Cleanup*/

    /* Close the file descriptor */
    close(file_fd);
}

void send_directory(int fd, const char* path)
{
    DIR *dir;
    struct dirent *entry;
    struct stat st;

    /* Buffer to build the path to index.html */
    char index_path[4096];

    /*Check for index.html*/
    /* Build path to index.html inside the directory */
    snprintf(index_path, sizeof(index_path), "%s/index.html", path);

    /* Check if index.html exists and is a regular file */
    if (stat(index_path, &st) == 0 && S_ISREG(st.st_mode)) {
        /* If index.html exists, send it as a regular file */
        send_file(fd, index_path);
        return;
    }

    /*Open the directory*/
    /* Open the directory for reading */
    dir = opendir(path);
    if (dir == NULL) {
        /* Failed to open directory -> internal server error */
        send_500(fd);
        return;
    }

    /*Build directory listing HTML*/
    char html[8192];//Buffer to store the generated HTML
    int len = 0;

    /* Start HTML document */
    len += snprintf(html + len, sizeof(html) - len,"<html><body>\n""<h1>Index of %s</h1>\n",path);

    /* Read directory entries one by one */
    while ((entry = readdir(dir)) != NULL) {

        /* Skip current and parent directory entries */
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        /* Build full path for the entry */
        char entry_path[4096];
        snprintf(entry_path, sizeof(entry_path), "%s/%s", path, entry->d_name);

        /* Get information about the entry */
        if (stat(entry_path, &st) < 0) {
            /* Skip entries we cannot stat */
            continue;
        }

        /* If entry is a directory, add a slash */
        if (S_ISDIR(st.st_mode)) {
            len += snprintf(html + len, sizeof(html) - len,"<a href=\"%s/\">%s/</a><br>\n",entry->d_name, entry->d_name);
        } else {
            /* Entry is a regular file */
            len += snprintf(html + len, sizeof(html) - len,"<a href=\"%s\">%s</a><br>\n",entry->d_name, entry->d_name);
        }
    }

    /* Close the directory */
    closedir(dir);

    /* Finish HTML document */
    len += snprintf(html + len, sizeof(html) - len,"</body></html>\n");

    /*Send HTTP response*/
    /* Prepare date header */
    char datebuf[128];
    time_t now = time(NULL);

    strftime(datebuf, sizeof(datebuf),"%a, %d %b %Y %H:%M:%S GMT",gmtime(&now));

    /* Build HTTP headers for 200 OK */
    char header[1024];
    snprintf(header, sizeof(header),
        "HTTP/1.0 200 OK\r\n"
        "Server: webserver/1.0\r\n"
        "Date: %s\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        datebuf,
        len);

    /* Send headers */
    write(fd, header, strlen(header));

    /* Send HTML body */
    write(fd, html, len);
}

char* get_mime_type(char* name)
{
    /* Pointer to the file extension */
    char *ext;

    /* Find the last '.' in the filename */
    ext = strrchr(name, '.');

    /* If there is no extension, return NULL */
    if (ext == NULL)
        return NULL;

    /* Skip the '.' character */
    ext++;

    /* Compare file extensions and return the matching MIME type */
    if (strcmp(ext, "html") == 0 || strcmp(ext, "htm") == 0)
        return "text/html";

    if (strcmp(ext, "jpg") == 0 || strcmp(ext, "jpeg") == 0)
        return "image/jpeg";

    if (strcmp(ext, "png") == 0)
        return "image/png";

    if (strcmp(ext, "gif") == 0)
        return "image/gif";

    if (strcmp(ext, "css") == 0)
        return "text/css";

    if (strcmp(ext, "js") == 0)
        return "application/javascript";

    if (strcmp(ext, "txt") == 0)
        return "text/plain";

    /* Unknown file type */
    return NULL;
}

void send_400(int fd)
{
    /* HTML body of the response */
    char *body =
        "<html><body>\n"
        "<h1>400 Bad Request</h1>\n"
        "</body></html>\n";

    /* Buffer to store the HTTP response headers */
    char header[1024];

    /* Buffer to store the current time */
    char timebuf[128];
    /* Get current time */
    time_t now = time(NULL);
    /* Convert time to GMT and format it according to RFC1123 */
    strftime(timebuf, sizeof(timebuf),"%a, %d %b %Y %H:%M:%S GMT",gmtime(&now));
   
    /* Build the HTTP response header */
    snprintf(header, sizeof(header),
        "HTTP/1.0 400 Bad Request\r\n"
        "Server: webserver/1.0\r\n"
        "Date: %s\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: %lu\r\n"
        "Connection: close\r\n"
        "\r\n",
        timebuf, strlen(body));
    /* Send the header to the client */
    write(fd, header, strlen(header));
    /* Send the body to the client */
    write(fd, body, strlen(body));
}

void send_403(int fd){
    /* HTML body of the response */
    char *body =
        "<html><body>\n"
        "<h1>403 Forbidden</h1>\n"
        "</body></html>\n";

    /* Buffer to store the HTTP response headers */
    char header[1024];

    /* Buffer to store the current time */
    char timebuf[128];
    /* Get current time */
    time_t now = time(NULL);
    /* Convert time to GMT and format it according to RFC1123 */
    strftime(timebuf, sizeof(timebuf),"%a, %d %b %Y %H:%M:%S GMT",gmtime(&now));
   
    /* Build the HTTP response header */
    snprintf(header, sizeof(header),
        "HTTP/1.0 403 Forbidden\r\n"
        "Server: webserver/1.0\r\n"
        "Date: %s\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        timebuf, strlen(body));
    /* Send the header to the client */
    write(fd, header, strlen(header));
    /* Send the body to the client */
    write(fd, body, strlen(body));
}
void send_404(int fd){
    /* HTML body of the response */
    char *body =
        "<html><body>\n"
        "<h1>404 Not Found</h1>\n"
        "</body></html>\n";

    /* Buffer to store the HTTP response headers */
    char header[1024];

    /* Buffer to store the current time */
    char timebuf[128];
    /* Get current time */
    time_t now = time(NULL);
    /* Convert time to GMT and format it according to RFC1123 */
    strftime(timebuf, sizeof(timebuf),"%a, %d %b %Y %H:%M:%S GMT",gmtime(&now));
   
    /* Build the HTTP response header */
    snprintf(header, sizeof(header),
        "HTTP/1.0 404 Not Found\r\n"
        "Server: webserver/1.0\r\n"
        "Date: %s\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        timebuf, strlen(body));
    /* Send the header to the client */
    write(fd, header, strlen(header));
    /* Send the body to the client */
    write(fd, body, strlen(body));
}
void send_500(int fd){
    /* HTML body of the response */
    char *body =
        "<html><body>\n"
        "<h1>500 Internal Server Error</h1>\n"
        "</body></html>\n";

    /* Buffer to store the HTTP response headers */
    char header[1024];

    /* Buffer to store the current time */
    char timebuf[128];
    /* Get current time */
    time_t now = time(NULL);
    /* Convert time to GMT and format it according to RFC1123 */
    strftime(timebuf, sizeof(timebuf),"%a, %d %b %Y %H:%M:%S GMT",gmtime(&now));
   
    /* Build the HTTP response header */
    snprintf(header, sizeof(header),
        "HTTP/1.0 500 Internal Server Error\r\n"
        "Server: webserver/1.0\r\n"
        "Date: %s\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        timebuf, strlen(body));
    /* Send the header to the client */
    write(fd, header, strlen(header));
    /* Send the body to the client */
    write(fd, body, strlen(body));

}
void send_501(int fd){
    /* HTML body of the response */
    char *body =
        "<html><body>\n"
        "<h1>501 Not Supported</h1>\n"
        "</body></html>\n";

    /* Buffer to store the HTTP response headers */
    char header[1024];

    /* Buffer to store the current time */
    char timebuf[128];
    /* Get current time */
    time_t now = time(NULL);
    /* Convert time to GMT and format it according to RFC1123 */
    strftime(timebuf, sizeof(timebuf),"%a, %d %b %Y %H:%M:%S GMT",gmtime(&now));
   
    /* Build the HTTP response header */
    snprintf(header, sizeof(header),
        "HTTP/1.0 501 Not Supported\r\n"
        "Server: webserver/1.0\r\n"
        "Date: %s\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        timebuf, strlen(body));
    /* Send the header to the client */
    write(fd, header, strlen(header));
    /* Send the body to the client */
    write(fd, body, strlen(body));
}
void send_302(int fd, const char* path)
{
    /* HTML body of the response (not very important for 302, but required) */
    char *body =
        "<html><body>\n"
        "<h1>302 Found</h1>\n"
        "</body></html>\n";
    /* Buffer to store the HTTP response headers */
    char header[2048];
    /* Buffer to store the current time */
    char timebuf[128];
    /* Buffer to store the new redirected path (path + '/') */
    char location[1024];
    /* Build the new location: original path + '/' */
    snprintf(location, sizeof(location), "%s/", path);
    /* Get current time */
    time_t now = time(NULL);
    /* Convert time to GMT and format it according to RFC1123 */
    strftime(timebuf, sizeof(timebuf),
             "%a, %d %b %Y %H:%M:%S GMT",
             gmtime(&now));

    /* Build the HTTP response header */
    snprintf(header, sizeof(header),
        "HTTP/1.0 302 Found\r\n"
        "Server: webserver/1.0\r\n"
        "Date: %s\r\n"
        "Location: %s\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        timebuf, location, strlen(body));
    /* Send the header to the client */
    write(fd, header, strlen(header));
    /* Send the body to the client */
    write(fd, body, strlen(body));
}
