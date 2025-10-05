#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define BACKLOG 16
#define REQ_BUF 4096
#define RESP_BUF 1024
#define QUEUE_CAPACITY 64
#define DEFAULT_WORKERS 4
#define MAX_WORKERS 32

struct mime_map {
    const char *ext;
    const char *mime;
};

static const struct mime_map mime_types[] = {
    {".html", "text/html"},
    {".htm", "text/html"},
    {".css", "text/css"},
    {".js", "application/javascript"},
    {".json", "application/json"},
    {".png", "image/png"},
    {".jpg", "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".gif", "image/gif"},
    {".svg", "image/svg+xml"},
    {".txt", "text/plain"},
    {".ico", "image/x-icon"},
    {".pdf", "application/pdf"},
    {NULL, NULL}
};

struct work_queue {
    int fds[QUEUE_CAPACITY];
    int head;
    int tail;
    int count;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
};

struct worker_args {
    struct work_queue *queue;
    const char *root;
};

static void queue_init(struct work_queue *q) {
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
}

static void queue_push(struct work_queue *q, int fd) {
    pthread_mutex_lock(&q->mutex);
    while (q->count == QUEUE_CAPACITY)
        pthread_cond_wait(&q->not_full, &q->mutex);
    q->fds[q->tail] = fd;
    q->tail = (q->tail + 1) % QUEUE_CAPACITY;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);
}

static int queue_pop(struct work_queue *q) {
    pthread_mutex_lock(&q->mutex);
    while (q->count == 0)
        pthread_cond_wait(&q->not_empty, &q->mutex);
    int fd = q->fds[q->head];
    q->head = (q->head + 1) % QUEUE_CAPACITY;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mutex);
    return fd;
}

static void trim_crlf(char *s) {
    size_t len = strlen(s);
    while (len && (s[len - 1] == '\r' || s[len - 1] == '\n')) {
        s[--len] = '\0';
    }
}

static void url_decode(char *dst, const char *src) {
    while (*src) {
        if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            char hex[3] = {src[1], src[2], '\0'};
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

static const char *get_mime_type(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot)
        return "application/octet-stream";
    for (int i = 0; mime_types[i].ext; ++i) {
        if (strcasecmp(dot, mime_types[i].ext) == 0)
            return mime_types[i].mime;
    }
    return "application/octet-stream";
}

static int send_all(int fd, const char *buf, size_t len) {
    while (len) {
        ssize_t n = send(fd, buf, len, 0);
        if (n <= 0)
            return -1;
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

static int send_headers(int client, int status, const char *reason, const char *mime, size_t len) {
    char header[RESP_BUF];
    int wrote = snprintf(header, sizeof(header),
                         "HTTP/1.1 %d %s\r\n"
                         "Server: tiny-c-http\r\n"
                         "Content-Length: %zu\r\n"
                         "Connection: close\r\n"
                         "Content-Type: %s\r\n\r\n",
                         status, reason, len, mime);
    if (wrote < 0 || (size_t)wrote >= sizeof(header))
        return -1;
    return send_all(client, header, (size_t)wrote);
}

static void send_error(int client, int status, const char *reason, const char *detail) {
    char body[RESP_BUF];
    int wrote = snprintf(body, sizeof(body),
                         "<!doctype html><html><head><title>%d %s</title></head>"
                         "<body><h1>%d %s</h1><p>%s</p></body></html>",
                         status, reason, status, reason, detail);
    if (wrote < 0)
        wrote = 0;
    if (send_headers(client, status, reason, "text/html", (size_t)wrote) == 0)
        send_all(client, body, (size_t)wrote);
}

static int read_request(int client, char *buffer, size_t size) {
    ssize_t n = recv(client, buffer, size - 1, 0);
    if (n <= 0)
        return -1;
    buffer[n] = '\0';
    char *line_end = strstr(buffer, "\r\n");
    if (line_end)
        line_end[2] = '\0';
    return 0;
}

static int is_safe_path(const char *path) {
    if (strstr(path, ".."))
        return 0;
    return 1;
}

static void handle_client(int client, const char *root) {
    char request[REQ_BUF];
    if (read_request(client, request, sizeof(request)) < 0) {
        send_error(client, 400, "Bad Request", "Failed to read request.");
        return;
    }

    char method[8], url[1024], version[16];
    if (sscanf(request, "%7s %1023s %15s", method, url, version) != 3) {
        send_error(client, 400, "Bad Request", "Malformed request line.");
        return;
    }
    trim_crlf(version);

    if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0) {
        send_error(client, 405, "Method Not Allowed", "Only GET and HEAD are supported.");
        return;
    }

    char path_decoded[1024];
    url_decode(path_decoded, url);

    if (path_decoded[0] != '/' || !is_safe_path(path_decoded)) {
        send_error(client, 403, "Forbidden", "Invalid path.");
        return;
    }

    char full_path[PATH_MAX];
    size_t needed = strlen(root) + strlen(path_decoded) + 1;
    if (needed > sizeof(full_path)) {
        send_error(client, 414, "URI Too Long", "Request path is too long.");
        return;
    }
    snprintf(full_path, sizeof(full_path), "%s%s", root, path_decoded);

    if (full_path[strlen(full_path) - 1] == '/') {
        if (strlen(full_path) + strlen("index.html") >= sizeof(full_path)) {
            send_error(client, 500, "Internal Server Error", "Path too long.");
            return;
        }
        strcat(full_path, "index.html");
    }

    struct stat st;
    if (stat(full_path, &st) < 0) {
        if (errno == ENOENT)
            send_error(client, 404, "Not Found", "File not found.");
        else if (errno == EACCES)
            send_error(client, 403, "Forbidden", "Access denied.");
        else
            send_error(client, 500, "Internal Server Error", "Stat failed.");
        return;
    }

    if (!S_ISREG(st.st_mode)) {
        send_error(client, 403, "Forbidden", "Not a regular file.");
        return;
    }

    int fd = open(full_path, O_RDONLY);
    if (fd < 0) {
        if (errno == EACCES)
            send_error(client, 403, "Forbidden", "Access denied.");
        else
            send_error(client, 500, "Internal Server Error", "Failed to open file.");
        return;
    }

    const char *mime = get_mime_type(full_path);
    if (send_headers(client, 200, "OK", mime, (size_t)st.st_size) < 0) {
        close(fd);
        return;
    }

    if (strcmp(method, "HEAD") == 0) {
        close(fd);
        return;
    }

    char buf[REQ_BUF];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        if (send_all(client, buf, (size_t)n) < 0)
            break;
    }
    close(fd);
    if (n < 0)
        send_error(client, 500, "Internal Server Error", "Failed while reading file.");
}

static void *worker_main(void *arg) {
    struct worker_args *ctx = (struct worker_args *)arg;
    for (;;) {
        int client = queue_pop(ctx->queue);
        if (client < 0)
            continue;
        handle_client(client, ctx->root);
        close(client);
    }
    return NULL;
}

static int create_server_socket(int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return -1;
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((uint16_t)port);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }
    if (listen(sock, BACKLOG) < 0) {
        close(sock);
        return -1;
    }
    return sock;
}

int main(int argc, char **argv) {
    const char *root_arg = argc > 1 ? argv[1] : ".";
    int port = argc > 2 ? atoi(argv[2]) : 8080;
    int worker_count = argc > 3 ? atoi(argv[3]) : DEFAULT_WORKERS;
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Invalid port: %d\n", port);
        return EXIT_FAILURE;
    }

    if (worker_count <= 0)
        worker_count = DEFAULT_WORKERS;
    if (worker_count > MAX_WORKERS)
        worker_count = MAX_WORKERS;

    char root[PATH_MAX];
    if (!realpath(root_arg, root)) {
        perror("realpath");
        return EXIT_FAILURE;
    }

    signal(SIGPIPE, SIG_IGN);

    int server = create_server_socket(port);
    if (server < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }

    struct work_queue queue;
    queue_init(&queue);

    pthread_t threads[MAX_WORKERS];
    struct worker_args worker_ctx[MAX_WORKERS];
    for (int i = 0; i < worker_count; ++i) {
        worker_ctx[i].queue = &queue;
        worker_ctx[i].root = root;
        if (pthread_create(&threads[i], NULL, worker_main, &worker_ctx[i]) != 0) {
            perror("pthread_create");
            return EXIT_FAILURE;
        }
    }

    printf("Serving %s on port %d with %d workers\n", root, port, worker_count);

    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t len = sizeof(client_addr);
        int client = accept(server, (struct sockaddr *)&client_addr, &len);
        if (client < 0) {
            perror("accept");
            continue;
        }
        queue_push(&queue, client);
    }

    close(server);
    return EXIT_SUCCESS;
}
