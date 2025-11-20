#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

/* Platform-specific event handling */
#ifdef __linux__
    #include <sys/epoll.h>
    #define USE_EPOLL 1
#elif defined(__APPLE__) || defined(__FreeBSD__)
    #include <sys/event.h>
    #define USE_KQUEUE 1
#else
    #error "Unsupported platform - need epoll (Linux) or kqueue (BSD/macOS)"
#endif

#define PORT 8080
#define BACKLOG 2048
#define MAX_EVENTS 1024
#define BUFFER_SIZE 4096

/* Pre-computed HTTP response with Keep-Alive */
static const char response[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Length: 2\r\n"
    "Connection: keep-alive\r\n"
    "Keep-Alive: timeout=60, max=1000\r\n"
    "\r\n"
    "OK";

static const int response_len = sizeof(response) - 1;

/* Set socket to non-blocking mode */
static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* Configure socket for high performance */
static void configure_socket(int fd) {
    int opt = 1;

    /* Disable Nagle's algorithm for lower latency */
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    /* Increase buffer sizes */
    int bufsize = 65536;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));

    /* Prevent SIGPIPE on macOS/BSD (Linux uses MSG_NOSIGNAL) */
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &opt, sizeof(opt));
#endif

    /* Enable TCP quick ACK */
#ifdef TCP_QUICKACK
    setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &opt, sizeof(opt));
#endif
}

int main(int argc, char *argv[]) {
    int server_fd, event_fd;
    struct sockaddr_in address;
    int opt = 1;
    int port = PORT;

    /* Allow custom port via argument */
    if (argc > 1) {
        port = atoi(argv[1]);
        if (port <= 0 || port > 65535) {
            fprintf(stderr, "Invalid port number. Using default: %d\n", PORT);
            port = PORT;
        }
    }

    /* Create server socket */
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    /* Set socket options */
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt SO_REUSEADDR");
        exit(EXIT_FAILURE);
    }

#ifdef SO_REUSEPORT
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt))) {
        perror("setsockopt SO_REUSEPORT");
        exit(EXIT_FAILURE);
    }
#endif

    /* Set non-blocking */
    if (set_nonblocking(server_fd) == -1) {
        perror("set_nonblocking");
        exit(EXIT_FAILURE);
    }

    /* Bind */
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }

    /* Listen with larger backlog */
    if (listen(server_fd, BACKLOG) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }

#ifdef USE_EPOLL
    /* Create epoll instance */
    struct epoll_event ev, events[MAX_EVENTS];
    event_fd = epoll_create1(0);
    if (event_fd == -1) {
        perror("epoll_create1");
        exit(EXIT_FAILURE);
    }

    /* Add server socket to epoll */
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = server_fd;
    if (epoll_ctl(event_fd, EPOLL_CTL_ADD, server_fd, &ev) == -1) {
        perror("epoll_ctl: server_fd");
        exit(EXIT_FAILURE);
    }

    printf("High-performance web server listening on port %d\n", port);
    printf("Using epoll (Linux)\n");

#elif defined(USE_KQUEUE)
    /* Create kqueue instance */
    struct kevent events[MAX_EVENTS];
    struct kevent change;
    event_fd = kqueue();
    if (event_fd == -1) {
        perror("kqueue");
        exit(EXIT_FAILURE);
    }

    /* Add server socket to kqueue */
    EV_SET(&change, server_fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, NULL);
    if (kevent(event_fd, &change, 1, NULL, 0, NULL) == -1) {
        perror("kevent: server_fd");
        exit(EXIT_FAILURE);
    }

    printf("High-performance web server listening on port %d\n", port);
    printf("Using kqueue (macOS/BSD)\n");
#endif

    printf("Press Ctrl+C to stop\n");

    /* Event loop */
    char buffer[BUFFER_SIZE];
    while (1) {
#ifdef USE_EPOLL
        int nfds = epoll_wait(event_fd, events, MAX_EVENTS, -1);
        if (nfds == -1) {
            perror("epoll_wait");
            exit(EXIT_FAILURE);
        }

        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == server_fd) {
#elif defined(USE_KQUEUE)
        int nfds = kevent(event_fd, NULL, 0, events, MAX_EVENTS, NULL);
        if (nfds == -1) {
            perror("kevent");
            exit(EXIT_FAILURE);
        }

        for (int i = 0; i < nfds; i++) {
            int fd = (int)events[i].ident;
            if (fd == server_fd) {
#endif
                /* Accept new connections */
                while (1) {
                    struct sockaddr_in client_addr;
                    socklen_t client_len = sizeof(client_addr);
                    int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);

                    if (client_fd == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break;
                        } else {
                            perror("accept");
                            break;
                        }
                    }

                    /* Configure client socket */
                    set_nonblocking(client_fd);
                    configure_socket(client_fd);

#ifdef USE_EPOLL
                    /* Add to epoll */
                    ev.events = EPOLLIN | EPOLLET;
                    ev.data.fd = client_fd;
                    if (epoll_ctl(event_fd, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
                        perror("epoll_ctl: client_fd");
                        close(client_fd);
                    }
#elif defined(USE_KQUEUE)
                    /* Add to kqueue */
                    EV_SET(&change, client_fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, NULL);
                    if (kevent(event_fd, &change, 1, NULL, 0, NULL) == -1) {
                        perror("kevent: client_fd");
                        close(client_fd);
                    }
#endif
                }
            } else {
                /* Handle client data */
#ifdef USE_EPOLL
                int client_fd = events[i].data.fd;
#elif defined(USE_KQUEUE)
                int client_fd = fd;
#endif
                int close_conn = 0;

                /* Read all available data in edge-triggered mode */
                while (1) {
                    ssize_t count = recv(client_fd, buffer, sizeof(buffer), 0);

                    if (count == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            /* No more data, keep connection alive */
                            break;
                        } else {
                            /* Real error */
                            close_conn = 1;
                            break;
                        }
                    } else if (count == 0) {
                        /* Client closed */
                        close_conn = 1;
                        break;
                    }

                    /* Process all complete requests in this buffer */
                    char *buf_ptr = buffer;
                    ssize_t remaining = count;

                    while (remaining > 0) {
                        /* Look for end of HTTP headers */
                        char *end_marker = NULL;
                        for (ssize_t j = 0; j <= remaining - 4; j++) {
                            if (buf_ptr[j] == '\r' && buf_ptr[j+1] == '\n' &&
                                buf_ptr[j+2] == '\r' && buf_ptr[j+3] == '\n') {
                                end_marker = buf_ptr + j + 4;
                                break;
                            }
                        }

                        if (!end_marker) {
                            /* No complete request in buffer */
                            break;
                        }

                        /* Send response */
                        ssize_t sent = 0;
                        while (sent < response_len) {
#ifdef USE_EPOLL
                            ssize_t n = send(client_fd, response + sent,
                                           response_len - sent, MSG_NOSIGNAL);
#elif defined(USE_KQUEUE)
                            ssize_t n = send(client_fd, response + sent,
                                           response_len - sent, 0);
#endif
                            if (n == -1) {
                                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                                    close_conn = 1;
                                }
                                break;
                            }
                            sent += n;
                        }

                        if (close_conn) break;

                        /* Move to next request in buffer (pipelining) */
                        remaining -= (end_marker - buf_ptr);
                        buf_ptr = end_marker;
                    }

                    if (close_conn) break;
                }

                if (close_conn) {
#ifdef USE_EPOLL
                    epoll_ctl(event_fd, EPOLL_CTL_DEL, client_fd, NULL);
#elif defined(USE_KQUEUE)
                    EV_SET(&change, client_fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
                    kevent(event_fd, &change, 1, NULL, 0, NULL);
#endif
                    close(client_fd);
                }
            }
        }
    }

    close(server_fd);
    return 0;
}
