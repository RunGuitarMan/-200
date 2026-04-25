#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#ifdef __linux__
    #include <sys/epoll.h>
    #define SEND_FLAGS MSG_NOSIGNAL
#elif defined(__APPLE__) || defined(__FreeBSD__)
    #include <sys/event.h>
    #define SEND_FLAGS 0
#else
    #error "Unsupported platform"
#endif

#define DEFAULT_PORT 8080
#define BACKLOG 2048
#define MAX_EVENTS 1024
#define BUFFER_SIZE 4096
#define CONN_BUF_SIZE 512
#define MAX_FDS 65536
#define MAX_BODY_SIZE (1024 * 1024)

/* --- Per-connection partial-read buffer --- */

struct connection {
    char buf[CONN_BUF_SIZE];
    int len;
};

static struct connection conns[MAX_FDS];

/* --- Globals --- */

static volatile sig_atomic_t running = 1;
static long request_count;
static long last_count;
static char *response_data;
static int response_len;
static int date_offset;
static const char *pid_file = "server.pid";

/* --- Signal handler --- */

static void handle_shutdown(int sig) {
    (void)sig;
    running = 0;
}

/* --- Build pre-computed response --- */

static void build_response(int body_size) {
    char header[512];
    time_t now = time(NULL);
    struct tm tm;
    char date[64];

    gmtime_r(&now, &tm);
    strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", &tm);

    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: %d\r\n"
        "Connection: keep-alive\r\n"
        "Keep-Alive: timeout=60, max=1000\r\n"
        "Date: %s\r\n"
        "\r\n", body_size, date);

    response_len = hlen + body_size;
    response_data = malloc((size_t)response_len);
    if (!response_data) { perror("malloc"); exit(1); }
    memcpy(response_data, header, hlen);

    char *dp = strstr(response_data, "Date: ");
    if (dp) date_offset = (int)(dp - response_data) + 6;

    if (body_size == 2)
        memcpy(response_data + hlen, "OK", 2);
    else
        memset(response_data + hlen, 'A', body_size);
}

/* --- Socket helpers --- */

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void configure_socket(int fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
#ifdef TCP_QUICKACK
    setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &one, sizeof(one));
#endif
}

/* --- Event loop abstraction --- */

#ifdef __linux__
typedef struct epoll_event ev_t;
#else
typedef struct kevent ev_t;
#endif

static inline int ev_init(void) {
#ifdef __linux__
    return epoll_create1(0);
#else
    return kqueue();
#endif
}

static inline int ev_add(int efd, int fd) {
#ifdef __linux__
    struct epoll_event ev = { .events = EPOLLIN | EPOLLET, .data.fd = fd };
    return epoll_ctl(efd, EPOLL_CTL_ADD, fd, &ev);
#else
    struct kevent kev;
    EV_SET(&kev, fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, NULL);
    return kevent(efd, &kev, 1, NULL, 0, NULL);
#endif
}

static inline int ev_del(int efd, int fd) {
#ifdef __linux__
    return epoll_ctl(efd, EPOLL_CTL_DEL, fd, NULL);
#else
    struct kevent kev;
    EV_SET(&kev, fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
    return kevent(efd, &kev, 1, NULL, 0, NULL);
#endif
}

static inline int ev_wait(int efd, ev_t *events, int max) {
#ifdef __linux__
    return epoll_wait(efd, events, max, 1000);
#else
    struct timespec ts = {1, 0};
    return kevent(efd, NULL, 0, events, max, &ts);
#endif
}

static inline int ev_fd(ev_t *e) {
#ifdef __linux__
    return e->data.fd;
#else
    return (int)e->ident;
#endif
}

/* --- Accept connections --- */

static void accept_connections(int server_fd, int efd) {
    while (1) {
        struct sockaddr_in addr;
        socklen_t addrlen = sizeof(addr);
        int fd;

#ifdef __linux__
        fd = accept4(server_fd, (struct sockaddr *)&addr, &addrlen, SOCK_NONBLOCK);
#else
        fd = accept(server_fd, (struct sockaddr *)&addr, &addrlen);
#endif
        if (fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            perror("accept");
            break;
        }

#ifndef __linux__
        set_nonblocking(fd);
#endif
        configure_socket(fd);
        if (fd >= MAX_FDS) { close(fd); continue; }
        conns[fd].len = 0;
        if (ev_add(efd, fd) == -1) close(fd);
    }
}

/* --- Handle client data --- */

static void handle_client(int fd, int efd) {
    if (fd >= MAX_FDS) { ev_del(efd, fd); close(fd); return; }

    struct connection *c = &conns[fd];
    int close_conn = 0;
    char recv_buf[BUFFER_SIZE];

    while (1) {
        ssize_t n = recv(fd, recv_buf, sizeof(recv_buf), 0);
        if (n == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            close_conn = 1; break;
        }
        if (n == 0) { close_conn = 1; break; }

        /* Combine with leftover from previous recv */
        char *ptr;
        int total;
        char combined[BUFFER_SIZE + CONN_BUF_SIZE];

        if (c->len > 0) {
            memcpy(combined, c->buf, c->len);
            memcpy(combined + c->len, recv_buf, n);
            ptr = combined;
            total = c->len + (int)n;
            c->len = 0;
        } else {
            ptr = recv_buf;
            total = (int)n;
        }

        /* Process complete HTTP requests */
        int rem = total;
        while (rem >= 4) {
            char *end = memmem(ptr, (size_t)rem, "\r\n\r\n", 4);
            if (!end) break;
            end += 4;

            /* Send response; close on partial send to prevent stream corruption */
            ssize_t sent = 0;
            while (sent < response_len) {
                ssize_t w = send(fd, response_data + sent,
                                (size_t)(response_len - sent), SEND_FLAGS);
                if (w == -1) { close_conn = 1; break; }
                sent += w;
            }
            if (sent < response_len) { close_conn = 1; break; }

            request_count++;
            rem -= (int)(end - ptr);
            ptr = end;
        }
        if (close_conn) break;

        /* Save leftover partial header data */
        if (rem > 0) {
            if (rem <= CONN_BUF_SIZE) {
                memcpy(c->buf, ptr, rem);
                c->len = rem;
            } else {
                close_conn = 1; break;
            }
        }
    }

    if (close_conn) {
        ev_del(efd, fd);
        close(fd);
        c->len = 0;
    }
}

/* --- Worker event loop --- */

static void run_worker(int server_fd) {
    int efd = ev_init();
    if (efd == -1) { perror("ev_init"); exit(1); }
    if (ev_add(efd, server_fd) == -1) { perror("ev_add"); exit(1); }

    ev_t events[MAX_EVENTS];
    time_t last_time = time(NULL);

    while (running) {
        /* Periodic RPS report */
        time_t now = time(NULL);
        if (now != last_time) {
            /* Refresh Date header in pre-built response */
            struct tm tm;
            char date[30];
            gmtime_r(&now, &tm);
            strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S GMT", &tm);
            memcpy(response_data + date_offset, date, 29);

            long cur = request_count;
            if (cur > last_count)
                fprintf(stderr, "[pid %d] RPS: %ld  total: %ld\n",
                        getpid(), cur - last_count, cur);
            last_count = cur;
            last_time = now;
        }

        int nfds = ev_wait(efd, events, MAX_EVENTS);
        if (nfds == -1) {
            if (errno == EINTR) continue;
            perror("ev_wait");
            break;
        }

        for (int i = 0; i < nfds; i++) {
            int fd = ev_fd(&events[i]);
            if (fd == server_fd)
                accept_connections(server_fd, efd);
            else
                handle_client(fd, efd);
        }
    }

    close(efd);
}

/* --- PID file --- */

static void write_pid(void) {
    FILE *f = fopen(pid_file, "w");
    if (f) { fprintf(f, "%d\n", getpid()); fclose(f); }
}

static void remove_pid(void) { unlink(pid_file); }

/* --- Usage --- */

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [-p PORT] [-w WORKERS] [-s BODY_SIZE] [PORT]\n"
        "  -p PORT       Listen port (default: %d)\n"
        "  -w WORKERS    Number of worker processes (default: 1)\n"
        "  -s SIZE       Response body size in bytes (default: 2)\n",
        prog, DEFAULT_PORT);
    exit(1);
}

/* --- Main --- */

int main(int argc, char *argv[]) {
    int port = DEFAULT_PORT, workers = 1, body_size = 2;
    int ch;

    while ((ch = getopt(argc, argv, "p:w:s:h")) != -1) {
        char *endp;
        long val;
        switch (ch) {
        case 'p':
            val = strtol(optarg, &endp, 10);
            if (*endp || val <= 0 || val > 65535)
                { fprintf(stderr, "Bad port: %s\n", optarg); return 1; }
            port = (int)val;
            break;
        case 'w':
            val = strtol(optarg, &endp, 10);
            if (*endp || val <= 0 || val > 1024)
                { fprintf(stderr, "Bad workers: %s\n", optarg); return 1; }
            workers = (int)val;
            break;
        case 's':
            val = strtol(optarg, &endp, 10);
            if (*endp || val <= 0 || val > MAX_BODY_SIZE)
                { fprintf(stderr, "Bad size: %s\n", optarg); return 1; }
            body_size = (int)val;
            break;
        default: usage(argv[0]);
        }
    }

    /* Legacy positional port argument */
    if (optind < argc && port == DEFAULT_PORT) {
        char *endp;
        long val = strtol(argv[optind], &endp, 10);
        if (*endp == '\0' && val > 0 && val <= 65535)
            port = (int)val;
    }

    build_response(body_size);

    /* Create server socket */
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif
#ifdef TCP_FASTOPEN
    int qlen = 5;
    setsockopt(server_fd, IPPROTO_TCP, TCP_FASTOPEN, &qlen, sizeof(qlen));
#endif

    set_nonblocking(server_fd);

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(port)
    };
    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        { perror("bind"); return 1; }
    if (listen(server_fd, BACKLOG) < 0)
        { perror("listen"); return 1; }

    printf("HTTP server on port %d", port);
#ifdef __linux__
    printf(" [epoll]");
#else
    printf(" [kqueue]");
#endif
    printf("  workers=%d  body=%dB\n", workers, body_size);

    /* Signals */
    struct sigaction sa = { .sa_handler = handle_shutdown };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    if (workers > 1) {
        pid_t pids[1024];
        write_pid();

        for (int i = 0; i < workers; i++) {
            pid_t pid = fork();
            if (pid < 0) {
                perror("fork");
                for (int j = 0; j < i; j++) kill(pids[j], SIGTERM);
                for (int j = 0; j < i; j++) waitpid(pids[j], NULL, 0);
                remove_pid();
                close(server_fd);
                free(response_data);
                return 1;
            }
            if (pid == 0) {
                run_worker(server_fd);
                close(server_fd);
                free(response_data);
                _exit(0);
            }
            pids[i] = pid;
            printf("  worker %d: pid %d\n", i + 1, pid);
        }

        while (running) sleep(1);
        printf("\nShutting down %d workers...\n", workers);
        for (int i = 0; i < workers; i++) kill(pids[i], SIGTERM);
        for (int i = 0; i < workers; i++) waitpid(pids[i], NULL, 0);
        remove_pid();
    } else {
        write_pid();
        run_worker(server_fd);
        remove_pid();
    }

    close(server_fd);
    free(response_data);
    printf("Server stopped.\n");
    return 0;
}
