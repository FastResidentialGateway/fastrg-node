/*\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\
  LIGHTHTTP.C

  Minimal dependency-free HTTP/1.1 server. See lighthttp.h.
/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "lighthttp.h"

#define LIGHTHTTP_BACKLOG 16
#define LIGHTHTTP_REQ_BUF 2048
#define LIGHTHTTP_BUF_MIN 4096

/* ----------------------------------------------------------------------- */
/* Growable buffer                                                          */
/* ----------------------------------------------------------------------- */

void lighthttp_buf_append(lighthttp_buf_t *b, const char *data, size_t len)
{
    if (b->oom || len == 0)
        return;
    if (b->len + len + 1 > b->cap) {
        size_t newcap = b->cap ? b->cap : LIGHTHTTP_BUF_MIN;
        while (newcap < b->len + len + 1)
            newcap *= 2;
        char *p = realloc(b->data, newcap);
        if (p == NULL) {
            b->oom = 1;
            return;
        }
        b->data = p;
        b->cap = newcap;
    }
    memcpy(b->data + b->len, data, len);
    b->len += len;
    b->data[b->len] = '\0';
}

void lighthttp_buf_appendf(lighthttp_buf_t *b, const char *fmt, ...)
{
    if (b->oom)
        return;

    char stackbuf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(stackbuf, sizeof(stackbuf), fmt, ap);
    va_end(ap);
    if (n < 0)
        return;

    if ((size_t)n < sizeof(stackbuf)) {
        lighthttp_buf_append(b, stackbuf, (size_t)n);
        return;
    }

    /* Rare large line: format into a heap buffer of the exact size. */
    char *big = malloc((size_t)n + 1);
    if (big == NULL) {
        b->oom = 1;
        return;
    }
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    lighthttp_buf_append(b, big, (size_t)n);
    free(big);
}

int lighthttp_buf_oom(const lighthttp_buf_t *b)
{
    return b->oom;
}

void lighthttp_buf_reset(lighthttp_buf_t *b)
{
    b->len = 0;
    b->oom = 0;
    if (b->data)
        b->data[0] = '\0';
}

void lighthttp_buf_free(lighthttp_buf_t *b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
    b->oom = 0;
}

/* ----------------------------------------------------------------------- */
/* Pure parsers / matcher                                                   */
/* ----------------------------------------------------------------------- */

int lighthttp_parse_addr(const char *addr, char *host, size_t hostsz, int *port)
{
    if (addr == NULL || host == NULL || port == NULL || hostsz == 0)
        return -1;

    const char *colon = strrchr(addr, ':');
    const char *port_str;
    if (colon == NULL) {
        /* bare port */
        snprintf(host, hostsz, "0.0.0.0");
        port_str = addr;
    } else {
        size_t hlen = (size_t)(colon - addr);
        if (hlen == 0) {
            snprintf(host, hostsz, "0.0.0.0");
        } else {
            if (hlen >= hostsz)
                return -1;
            memcpy(host, addr, hlen);
            host[hlen] = '\0';
        }
        port_str = colon + 1;
    }

    if (*port_str == '\0')
        return -1;
    for(const char *p=port_str; *p; p++) {
        if (*p < '0' || *p > '9')
            return -1;
    }
    long pn = strtol(port_str, NULL, 10);
    if (pn <= 0 || pn > 65535)
        return -1;
    *port = (int)pn;
    return 0;
}

int lighthttp_parse_request_line(const char *req, char *method, size_t msz,
                                 char *path, size_t psz)
{
    if (req == NULL || method == NULL || path == NULL || msz == 0 || psz == 0)
        return -1;

    /* method = up to first space */
    const char *sp = strchr(req, ' ');
    if (sp == NULL || sp == req)
        return -1;
    size_t mlen = (size_t)(sp - req);
    if (mlen >= msz)
        return -1;
    memcpy(method, req, mlen);
    method[mlen] = '\0';

    /* target = up to next space; strip query string */
    const char *target = sp + 1;
    const char *end = target;
    while (*end && *end != ' ' && *end != '\r' && *end != '\n' && *end != '?')
        end++;
    size_t plen = (size_t)(end - target);
    if (plen == 0 || plen >= psz)
        return -1;
    memcpy(path, target, plen);
    path[plen] = '\0';
    return 0;
}

const lighthttp_route_t *lighthttp_match(const lighthttp_server_t *s,
                                         const char *method, const char *path)
{
    if (s == NULL || method == NULL || path == NULL)
        return NULL;
    for(int i=0; i<s->n_routes; i++) {
        if (strcmp(s->routes[i].method, method) == 0 &&
            strcmp(s->routes[i].path, path) == 0)
            return &s->routes[i];
    }
    return NULL;
}

/* ----------------------------------------------------------------------- */
/* Server                                                                   */
/* ----------------------------------------------------------------------- */

int lighthttp_add_route(lighthttp_server_t *s, const char *method, const char *path,
                        lighthttp_handler_t handler, void *ctx)
{
    if (s->n_routes >= LIGHTHTTP_MAX_ROUTES)
        return -1;
    s->routes[s->n_routes].method = method;
    s->routes[s->n_routes].path = path;
    s->routes[s->n_routes].handler = handler;
    s->routes[s->n_routes].ctx = ctx;
    s->n_routes++;
    return 0;
}

int lighthttp_init(lighthttp_server_t *s, const char *addr)
{
    memset(s, 0, sizeof(*s));
    s->listen_fd = -1;

    if (lighthttp_parse_addr(addr, s->host, sizeof(s->host), &s->port) != 0) {
        fprintf(stderr, "lighthttp: invalid listen addr '%s'\n", addr ? addr : "(null)");
        return -1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "lighthttp: socket() failed: %s\n", strerror(errno));
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)s->port);
    if (strcmp(s->host, "0.0.0.0") == 0) {
        sa.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (inet_pton(AF_INET, s->host, &sa.sin_addr) != 1) {
        fprintf(stderr, "lighthttp: bad listen host '%s'\n", s->host);
        close(fd);
        return -1;
    }

    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        fprintf(stderr, "lighthttp: bind(%s:%d) failed: %s\n", s->host, s->port, strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, LIGHTHTTP_BACKLOG) < 0) {
        fprintf(stderr, "lighthttp: listen() failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    s->listen_fd = fd;
    return 0;
}

static int write_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

static void send_simple(int fd, int status, const char *reason, const char *body)
{
    char hdr[256];
    size_t blen = body ? strlen(body) : 0;
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, reason, blen);
    if (n <= 0)
        return;
    if (write_all(fd, hdr, (size_t)n) != 0)
        return;
    if (blen)
        write_all(fd, body, blen);
}

static void handle_conn(lighthttp_server_t *s, int fd)
{
    char req[LIGHTHTTP_REQ_BUF];
    ssize_t n = read(fd, req, sizeof(req) - 1);
    if (n <= 0)
        return;
    req[n] = '\0';

    char method[16], path[256];
    if (lighthttp_parse_request_line(req, method, sizeof(method), path, sizeof(path)) != 0) {
        send_simple(fd, 400, "Bad Request", "Bad Request\n");
        return;
    }

    const lighthttp_route_t *route = lighthttp_match(s, method, path);
    if (route == NULL) {
        send_simple(fd, 404, "Not Found", "Not Found\n");
        return;
    }

    /* Build the body via the handler (no socket I/O inside), then send it. */
    lighthttp_buf_t body = {0};
    const char *content_type = "text/plain";
    int status = route->handler(&body, &content_type, route->ctx);

    if (lighthttp_buf_oom(&body)) {
        lighthttp_buf_free(&body);
        send_simple(fd, 500, "Internal Server Error", "out of memory\n");
        return;
    }

    char hdr[256];
    int hn = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, content_type, body.len);
    if (hn > 0 && write_all(fd, hdr, (size_t)hn) == 0)
        write_all(fd, body.data ? body.data : "", body.len);

    lighthttp_buf_free(&body);
}

void lighthttp_serve(lighthttp_server_t *s)
{
    for(;;) {
        int conn = accept(s->listen_fd, NULL, NULL);
        if (conn < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        handle_conn(s, conn);
        close(conn);
    }
    close(s->listen_fd);
    s->listen_fd = -1;
}
