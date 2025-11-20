#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

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

    /* Enable TCP quick ACK */
#ifdef TCP_QUICKACK
    setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &opt, sizeof(opt));
#endif
}

int main(int argc, char *argv[]) {
    int server_fd, epoll_fd;
    struct sockaddr_in address;
    struct epoll_event ev, events[MAX_EVENTS];
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

    /* Create epoll instance */
    epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        perror("epoll_create1");
        exit(EXIT_FAILURE);
    }

    /* Add server socket to epoll */
    ev.events = EPOLLIN | EPOLLET; /* Edge-triggered for performance */
    ev.data.fd = server_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev) == -1) {
        perror("epoll_ctl: server_fd");
        exit(EXIT_FAILURE);
    }

    printf("High-performance web server listening on port %d\n", port);
    printf("Using epoll with edge-triggered mode\n");
    printf("Press Ctrl+C to stop\n");

    /* Event loop */
    char buffer[BUFFER_SIZE];
    while (1) {
        int nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (nfds == -1) {
            perror("epoll_wait");
            exit(EXIT_FAILURE);
        }

        for (int i = 0; i < nfds; i++) {
            if (events[i].data.fd == server_fd) {
                /* Accept new connections */
                while (1) {
                    struct sockaddr_in client_addr;
                    socklen_t client_len = sizeof(client_addr);
                    int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);

                    if (client_fd == -1) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            /* All connections processed */
                            break;
                        } else {
                            perror("accept");
                            break;
                        }
                    }

                    /* Configure client socket */
                    set_nonblocking(client_fd);
                    configure_socket(client_fd);

                    /* Add to epoll */
                    ev.events = EPOLLIN | EPOLLET;
                    ev.data.fd = client_fd;
                    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) == -1) {
                        perror("epoll_ctl: client_fd");
                        close(client_fd);
                    }
                }
            } else {
                /* Handle client data */
                int client_fd = events[i].data.fd;
                int done = 0;

                while (1) {
                    ssize_t count = recv(client_fd, buffer, sizeof(buffer), 0);

                    if (count == -1) {
                        if (errno != EAGAIN && errno != EWOULDBLOCK) {
                            perror("recv");
                            done = 1;
                        }
                        break;
                    } else if (count == 0) {
                        /* Client closed connection */
                        done = 1;
                        break;
                    }

                    /* Check if we have a complete HTTP request (look for \r\n\r\n) */
                    if (count >= 4) {
                        /* Send response */
                        ssize_t sent = 0;
                        while (sent < response_len) {
                            ssize_t n = send(client_fd, response + sent, response_len - sent, MSG_NOSIGNAL);
                            if (n == -1) {
                                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                                    done = 1;
                                }
                                break;
                            }
                            sent += n;
                        }
                        break; /* Request handled */
                    }
                }

                if (done) {
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
                    close(client_fd);
                }
            }
        }
    }

    close(server_fd);
    return 0;
}
