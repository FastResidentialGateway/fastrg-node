/*\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\
  LIGHTHTTP.H

  Minimal, dependency-free HTTP/1.1 server for serving small read-only
  endpoints (e.g. Prometheus /metrics). Pure C, transport only — it has no
  knowledge of DPDK or FastRG. Handlers are registered per (method, path) and
  build a response body into a growable buffer; lighthttp owns the socket I/O.

  The handler/buffer split is deliberate: a handler gathers + formats its body
  into the buffer and returns, then lighthttp performs the (potentially
  blocking) socket write. Handlers that read RCU-protected data therefore never
  hold an RCU read lock across a blocking client write.
/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\/\*/

#ifndef _LIGHTHTTP_H_
#define _LIGHTHTTP_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Growable byte buffer that handlers fill with the response body. On allocation
 * failure it enters an out-of-memory state (oom=1) and further appends are
 * no-ops; check lighthttp_buf_oom() before relying on it. */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
    int    oom;
} lighthttp_buf_t;

/**
 * @fn lighthttp_buf_append
 *
 * @brief Append raw bytes to the buffer, growing it as needed
 * @param b
 *      Buffer to append to
 * @param data
 *      Bytes to append
 * @param len
 *      Number of bytes to append
 * @return
 *      void
 */
void lighthttp_buf_append(lighthttp_buf_t *b, const char *data, size_t len);

/**
 * @fn lighthttp_buf_appendf
 *
 * @brief Append a printf-formatted string to the buffer, growing it as needed
 * @param b
 *      Buffer to append to
 * @param fmt
 *      printf-style format string, followed by its arguments
 * @return
 *      void
 */
void lighthttp_buf_appendf(lighthttp_buf_t *b, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/**
 * @fn lighthttp_buf_oom
 *
 * @brief Report whether the buffer hit an allocation failure
 * @param b
 *      Buffer to query
 * @return
 *      1 if the buffer is in an out-of-memory state, 0 otherwise
 */
int lighthttp_buf_oom(const lighthttp_buf_t *b);

/**
 * @fn lighthttp_buf_reset
 *
 * @brief Reset the buffer length to zero, retaining its capacity
 * @param b
 *      Buffer to reset
 * @return
 *      void
 */
void lighthttp_buf_reset(lighthttp_buf_t *b);

/**
 * @fn lighthttp_buf_free
 *
 * @brief Free the buffer's backing storage and clear the struct
 * @param b
 *      Buffer to free
 * @return
 *      void
 */
void lighthttp_buf_free(lighthttp_buf_t *b);

/* Response handler. Fill @out with the body, set *@content_type (defaults to
 * "text/plain"), return the HTTP status code (e.g. 200). @ctx is the per-route
 * user pointer. Must not do blocking I/O on the client socket. */
typedef int (*lighthttp_handler_t)(lighthttp_buf_t *out, const char **content_type, void *ctx);

#define LIGHTHTTP_MAX_ROUTES 8

typedef struct {
    const char         *method;
    const char         *path;
    lighthttp_handler_t handler;
    void               *ctx;
} lighthttp_route_t;

typedef struct {
    int               listen_fd;
    char              host[64];
    int               port;
    lighthttp_route_t routes[LIGHTHTTP_MAX_ROUTES];
    int               n_routes;
} lighthttp_server_t;

/**
 * @fn lighthttp_parse_addr
 *
 * @brief Parse a listen address "host:port" or a bare "port"
 * @param addr
 *      Address string (e.g. "0.0.0.0:9091" or "9091")
 * @param host
 *      Buffer to receive the host (defaults to "0.0.0.0" when none is given)
 * @param hostsz
 *      Size of the host buffer
 * @param port
 *      Pointer to receive the parsed port
 * @return
 *      0 on success, -1 on malformed input
 */
int lighthttp_parse_addr(const char *addr, char *host, size_t hostsz, int *port);

/**
 * @fn lighthttp_parse_request_line
 *
 * @brief Parse an HTTP request line ("GET /metrics?x=1 HTTP/1.1")
 * @param req
 *      Raw request bytes beginning at the request line
 * @param method
 *      Buffer to receive the method (e.g. "GET")
 * @param msz
 *      Size of the method buffer
 * @param path
 *      Buffer to receive the request target with any query string stripped
 * @param psz
 *      Size of the path buffer
 * @return
 *      0 on success, -1 if it does not look like a request line
 */
int lighthttp_parse_request_line(const char *req, char *method, size_t msz,
                                 char *path, size_t psz);

/**
 * @fn lighthttp_match
 *
 * @brief Find a registered route by method and path (exact match)
 * @param s
 *      Server holding the route table
 * @param method
 *      HTTP method to match
 * @param path
 *      Request path to match
 * @return
 *      Pointer to the matching route, or NULL if none matches
 */
const lighthttp_route_t *lighthttp_match(const lighthttp_server_t *s,
                                         const char *method, const char *path);

/**
 * @fn lighthttp_init
 *
 * @brief Initialize a server and bind+listen on the given address
 * @param s
 *      Server to initialize
 * @param addr
 *      Listen address ("host:port" or "port")
 * @return
 *      0 on success, -1 on failure
 */
int lighthttp_init(lighthttp_server_t *s, const char *addr);

/**
 * @fn lighthttp_add_route
 *
 * @brief Register a (method, path) route and its handler
 * @param s
 *      Server to register the route on
 * @param method
 *      HTTP method (e.g. "GET")
 * @param path
 *      Request path (e.g. "/metrics")
 * @param handler
 *      Handler invoked when the route matches
 * @param ctx
 *      Opaque pointer passed back to the handler
 * @return
 *      0 on success, -1 if the route table is full
 */
int lighthttp_add_route(lighthttp_server_t *s, const char *method, const char *path,
                        lighthttp_handler_t handler, void *ctx);

/**
 * @fn lighthttp_serve
 *
 * @brief Run the blocking accept loop, serving requests until the socket fails
 * @param s
 *      Initialized server (see lighthttp_init)
 * @return
 *      void
 */
void lighthttp_serve(lighthttp_server_t *s);

#ifdef __cplusplus
}
#endif

#endif /* _LIGHTHTTP_H_ */
