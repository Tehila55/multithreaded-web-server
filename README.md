# Multithreaded Web Server

A multithreaded HTTP web server implementation in C with thread pool management and file system handling.

---

## Features

- HTTP/1.0 request handling
- Multithreaded server using pthreads
- Thread pool implementation
- Request queue management
- File and directory serving
- Automatic directory listing
- MIME type detection
- Error handling for:
  - 400 Bad Request
  - 403 Forbidden
  - 404 Not Found
  - 500 Internal Server Error
  - 501 Not Supported
  - 302 Redirect

---

## Technologies

- C
- Linux sockets API
- TCP/IP
- pthreads
- Thread synchronization
- Mutexes & condition variables
- File system operations

---

## Project Structure

- `server.c` – HTTP server implementation
- `threadpool.c` – thread pool implementation
- `threadpool.h` – thread pool definitions
- `*.txt` – HTTP response templates and examples

---

## Build

```bash
gcc server.c threadpool.c -o server -lpthread
```

---

## Run

```bash
./server <port> <pool-size> <max-queue-size> <max-number-of-requests>
```

Example:

```bash
./server 8080 4 8 100
```

---

## Test

Open in browser:

```text
http://localhost:8080/
```

---

## Author

Tehila Cohen
