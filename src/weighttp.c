/*
 * weighttp - lightweight and simple webserver benchmarking tool
 *
 * Copyright (c) 2016,2025 Glue Logic LLC. All rights reserved. code()gluelogic.com
 *
 * This rewrite is based on weighttp by Thomas Porzelt
 *     Copyright (c) 2009-2011 Thomas Porzelt
 *   git://git.lighttpd.net/weighttp
 *   https://github.com/lighttpd/weighttp/
 *
 * License:
 *     MIT, see COPYING file
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <sys/types.h>
#include <sys/socket.h>/* socket() connect() SOCK_NONBLOCK sockaddr_storage */
                       /* recv() send() sendmmsg() */
#include <sys/stat.h>  /* fstat() */
#include <sys/time.h>  /* gettimeofday() */
#include <errno.h>     /* errno EINTR EAGAIN EWOULDBLOCK EINPROGRESS EALREADY */
#include <fcntl.h>     /* open() fcntl() pipe2() F_SETFL (O_* flags) */
#include <inttypes.h>  /* PRIu64 PRId64 */
#include <limits.h>    /* USHRT_MAX */
#include <locale.h>    /* setlocale() */
#include <math.h>      /* sqrt() */
#include <netdb.h>     /* getaddrinfo() freeaddrinfo() */
#include <poll.h>      /* poll() POLLIN POLLOUT POLLERR POLLHUP POLLRDHUP */
#include <pthread.h>   /* pthread_create() pthread_join() */
#include <stdarg.h>    /* va_start() va_end() vfprintf() */
#include <stdio.h>
#include <stdlib.h>    /* calloc() free() exit() strtoul() strtoull() */
#include <stdint.h>    /* INT32_MAX UINT32_MAX UINT64_MAX */
#include <signal.h>    /* signal() */
#include <string.h>
#include <strings.h>   /* strcasecmp() strncasecmp() */
#include <unistd.h>    /* read() write() close() getopt() optarg optind optopt*/

#ifndef POLLRDHUP
#define POLLRDHUP 0
#endif

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <sys/un.h>
#endif

#ifdef WEIGHTTP_SPLICE /* not defined by default; might benefit w/ pipelining */
#ifdef SPLICE_F_NONBLOCK
#include <sys/uio.h>   /* vmsplice() (struct iovec) */
/*(Using tee(), splice() does not appear to have measurable benefit over write()
 * for small requests, which is the typical use case.  Not implemented, but the
 * same probably holds for small responses.  For large responses, might be a
 * benefit to splice() to pipe() and then from pipe() to fd open to /dev/null,
 * which can be done when Content-Length is supplied (instead of chunked)) */
#endif
#endif

#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#ifndef SOCK_NONBLOCK
#define SOCK_NONBLOCK 0
#endif

#ifndef PACKAGE_VERSION
#define PACKAGE_VERSION ""
#endif


#ifndef __has_attribute
#define __has_attribute(x) 0
#endif

/*(oversimplified; these attributes are supported by some other compilers)*/
#if defined(__GNUC__) || defined(__clang__)
#ifndef __attribute_cold__
#define __attribute_cold__       __attribute__((__cold__))
#endif
#ifndef __attribute_hot__
#define __attribute_hot__        __attribute__((__hot__))
#endif
#ifndef __attribute_noinline__
#define __attribute_noinline__   __attribute__((__noinline__))
#endif
#ifndef __attribute_nonnull__
#define __attribute_nonnull__(params)  __attribute__((__nonnull__ params))
#endif
#ifndef __attribute_noreturn__
#define __attribute_noreturn__   __attribute__((__noreturn__))
#endif
#ifndef __attribute_pure__
#define __attribute_pure__       __attribute__((__pure__))
#endif
#ifndef __attribute_format__
#define __attribute_format__(x)  __attribute__((__format__ x))
#endif
#else
#ifndef __builtin_expect
#define __builtin_expect(x, y) (x)
#endif
#ifndef __attribute_cold__
#define __attribute_cold__
#endif
#ifndef __attribute_hot__
#define __attribute_hot__
#endif
#ifndef __attribute_noinline__
#define __attribute_noinline__
#endif
#ifndef __attribute_nonnull__
#define __attribute_nonnull__(params)
#endif
#ifndef __attribute_noreturn__
#define __attribute_noreturn__
#endif
#ifndef __attribute_pure__
#define __attribute_pure__
#endif
#ifndef __attribute_format__
#define __attribute_format__(x)
#endif
#endif

#ifndef __attribute_fallthrough__
#if __has_attribute(fallthrough)
 /*|| __GNUC_PREREQ(7,0)*/
#define __attribute_fallthrough__  __attribute__((__fallthrough__));
#else
#define __attribute_fallthrough__  /* fall through */
#endif
#endif


__attribute_cold__
__attribute_noinline__
static void
show_version (void)
{
    puts("\nweighttp " PACKAGE_VERSION
         " - lightweight and simple webserver benchmarking tool\n");
}


__attribute_cold__
__attribute_noinline__
static void
show_help (void)
{
    puts(
      "weighttp <options> <URI>\n"
      "  -n num     number of requests      (mandatory)\n"
      "  -t num     thread count            (default: 1)\n"
      "  -c num     concurrent clients      (default: 1)\n"
      "  -k         keep alive              (default: no)\n"
      "  -K num     num pipelined requests  (default: 1)\n"
      "  -6         use ipv6                (default: no)\n"
      "  -i         use HTTP HEAD method    (default: GET)\n"
      "  -m method  use custom HTTP method  (default: GET)\n"
      "  -H str     add header to request (\"label: value\"); repeatable\n"
      "  -b size    socket buffer sizes (SO_SNDBUF, SO_RCVBUF)\n"
      "  -B addr    local address to bind to when making outgoing connections\n"
      "  -C cookie  add cookie to request (\"cookie-name=value\"); repeatable\n"
      "  -F         use TCP Fast Open (RFC 7413)\n"
      "  -T type    Content-Type header to use for POST/PUT data,\n"
      "             e.g. application/x-www-form-urlencoded\n"
      "                                     (default: text/plain)\n"
      "  -A string  add Basic WWW Authorization   (str is username:password)\n"
      "  -P string  add Basic Proxy-Authorization (str is username:password)\n"
      "  -X proxy   proxy:port or unix domain socket path beginning w/ '/'\n"
      "  -p file    make HTTP POST request using file contents for body\n"
      "  -u file    make HTTP PUT request using file contents for body\n"
      "  -d         (ignored; compatibility with Apache Bench (ab))\n"
      "  -l         (ignored; compatibility with Apache Bench (ab))\n"
      "  -r         (ignored; compatibility with Apache Bench (ab))\n"
      "  -q         quiet: do not show version header or progress\n"
      "  -h         show help and exit\n"
      "  -V         show version and exit\n\n"
      "example: \n"
      "  weighttpd -n 500000 -c 100 -t 2 -K 64 http://localhost/index.html\n");
}

/* Notes regarding pipelining
 * Enabling pipelining (-p x where x > 1) results in extra requests being sent
 * beyond the precise number requested on the command line.  Subsequently,
 * extra bytes might be read and reported in stats at the end of the test run.
 * Additionally, the extra requests are dropped once the req_todo amount is
 * reached, and so the target web server(s) might report errors that client
 * dropped connection (client disconnect) for those final requests.
 *
 * The benefits of pipelining include reduced latency between request/response,
 * as well as potentially fewer socket read()s for data if multiple requests or
 * multiple responses are available to be read by server or client, respectively
 */

#define CLIENT_BUFFER_SIZE 32 * 1024


struct Times;
typedef struct Times Times;
struct Stats;
typedef struct Stats Stats;
struct Client;
typedef struct Client Client;
struct Worker;
typedef struct Worker Worker;
struct Config;
typedef struct Config Config;
struct Worker_Config;
typedef struct Worker_Config Worker_Config;


struct Times {
    uint32_t connect;    /* connect time (us) */
    uint32_t ttfb;       /* time to first byte (us) */
    uint64_t t;          /* response time (us) */ /* 64-bit align; overloaded */
};

struct Stats {
    uint64_t req_todo;      /* total num of requests to do */
    uint64_t req_started;   /* total num of requests started */
    uint64_t req_done;      /* total num of requests done */
    uint64_t req_success;   /* total num of successful requests */
    uint64_t req_failed;    /* total num of failed requests */
    uint64_t req_error;     /* total num of errored requests */
    uint64_t bytes_total;   /* total num of bytes received (headers+body) */
    uint64_t bytes_headers; /* total num of bytes received (headers) */
    uint64_t req_2xx;
    uint64_t req_3xx;
    uint64_t req_4xx;
    uint64_t req_5xx;
};

struct Client {
    int revents;
    enum {
        PARSER_CONNECT,
        PARSER_START,
        PARSER_HEADER,
        PARSER_BODY
    } parser_state;

    uint32_t buffer_offset;  /* pos in buffer  (size of data in buffer) */
    uint32_t parser_offset;  /* pos in parsing (behind buffer_offset) */
    uint32_t request_offset; /* pos in sending request */
    int chunked;
    int64_t content_length;
    int64_t chunk_size;
    int64_t chunk_received;
    int http_status_success;
    int config_keepalive;
    int keepalive;
    int keptalive;
    int pipelined;
    int pipeline_max;
    int tcp_fastopen;
    int http_head;
    int so_bufsz;
    int req_redo;

  #ifdef WEIGHTTP_SPLICE
    int pipefds[3];
  #endif
    uint32_t request_size;
    const char *request;
    struct pollfd *pfd;
    Stats *stats;
    Times *times;
    Times *wtimes;
    const struct addrinfo *raddr;
    const struct addrinfo *laddr;
    char buffer[CLIENT_BUFFER_SIZE+1];
};

struct Worker {
  #ifdef WEIGHTTP_SPLICE
    int pipefds[2];
  #endif
    struct pollfd *pfds;
    Client *clients;
    Stats stats;
    struct addrinfo raddr;
    struct addrinfo laddr;
    struct sockaddr_storage raddr_storage;
    struct sockaddr_storage laddr_storage;
    Times *wtimes;
};

struct Worker_Config {
    const Config *config;
    int id;
    int num_clients;
    uint64_t num_requests;
    Stats stats;
    Times *wtimes;
    /* pad struct Worker_Config for cache line separation between threads.
     * Round up to 256 to avoid chance of false sharing between threads.
     * Alternatively, could memalign the allocation of struct Worker_Config
     * list to cache line size (e.g. 128 bytes) */
    uint64_t padding[(256 - (1*sizeof(void *))
                          - (2*sizeof(int))
                          - (1*sizeof(uint64_t))
                          - sizeof(Stats))
                      / sizeof(uint64_t)];
};

struct Config {
    Worker_Config *wconfs;
    char *proxy;
    struct timeval ts_start;
    struct timeval ts_end;

    uint64_t req_count;
    int thread_count;
    int keep_alive;
    int concur_count;
    int pipeline_max;
    int tcp_fastopen;
    int http_head;
    int so_bufsz;

    int quiet;
  #ifdef WEIGHTTP_SPLICE
    int reqpipe;
  #endif
    uint32_t request_size;
    char *request;
    char buf[16384]; /*(used for simple 8k memaligned request buffer on stack)*/
    struct addrinfo raddr;
    struct addrinfo laddr;
    struct sockaddr_storage raddr_storage;
    struct sockaddr_storage laddr_storage;
    struct laddrs {
        struct addrinfo **addrs;
        int num;
    } laddrs;
};


__attribute_cold__
__attribute_nonnull__()
static void
client_init (Worker * const restrict worker,
             const Config * const restrict config,
             const int i)
{
    Client * const restrict client = worker->clients+i;
    client->pfd = worker->pfds+i;
    client->pfd->fd = -1;
    client->parser_state = PARSER_CONNECT;

    client->stats = &worker->stats;
    client->times = client->wtimes = worker->wtimes;
    client->raddr = &worker->raddr;
    client->laddr = config->laddrs.num > 0
                  ? config->laddrs.addrs[(i % config->laddrs.num)]
                  : (0 != worker->laddr.ai_addrlen) ? &worker->laddr : NULL;
    client->config_keepalive = config->keep_alive;
    client->pipeline_max     = config->pipeline_max;
    client->tcp_fastopen     = config->tcp_fastopen;
    client->http_head        = config->http_head;
    client->so_bufsz         = config->so_bufsz;
    client->request_size     = config->request_size;
    client->request          = config->request;
    /* future: might copy config->request to new allocation in Worker
     * so that all memory accesses during benchmark execution are to
     * independent, per-thread allocations */

  #ifdef WEIGHTTP_SPLICE
    client->pipefds[0]       = worker->pipefds[0];
    client->pipefds[1]       = worker->pipefds[1];
    client->pipefds[2]       = config->reqpipe;
  #endif
}


__attribute_cold__
__attribute_nonnull__()
static void
client_delete (const Client * const restrict client)
{
    if (-1 != client->pfd->fd)
      #ifdef _WIN32
        closesocket(client->pfd->fd);
      #else
        close(client->pfd->fd);
      #endif
}


__attribute_cold__
__attribute_nonnull__()
__attribute_noinline__
static void
worker_init (Worker * const restrict worker,
             Worker_Config * const restrict wconf)
{
    const Config * const restrict config = wconf->config;
    memset(worker, 0, sizeof(Worker));
    memcpy(&worker->laddr, &config->laddr, sizeof(config->laddr));
    memcpy(&worker->raddr, &config->raddr, sizeof(config->raddr));
    if (config->laddr.ai_addrlen)
        worker->laddr.ai_addr = (struct sockaddr *)
          memcpy(&worker->laddr_storage,
                 &config->laddr_storage, config->laddr.ai_addrlen);
    worker->raddr.ai_addr = (struct sockaddr *)
      memcpy(&worker->raddr_storage,
             &config->raddr_storage, config->raddr.ai_addrlen);
  #ifdef WEIGHTTP_SPLICE
    worker->pipefds[0] = -1;
    worker->pipefds[1] = -1;
    if (-1 != config->reqpipe) {
        if (0 != pipe2(worker->pipefds, O_NONBLOCK))
            perror("pipe()");
    }
  #endif
    const int num_clients = wconf->num_clients;
    worker->stats.req_todo = wconf->num_requests;
    worker->pfds = (struct pollfd *)calloc(num_clients, sizeof(struct pollfd));
    worker->clients = (Client *)calloc(num_clients, sizeof(Client));

    /*(note: if humongous benchmark, might mmap large file on disk rather than
     * allocating on heap, but then would want to init .connect = INT32_MAX on
     * demand for keepalive requests instead of pre-init here, and might mmap
     * single file and give workers offsets instead of allocating per worker)*/
    worker->wtimes = (Times *)calloc(wconf->num_requests, sizeof(Times));
    for (uint64_t n = 0, total = wconf->num_requests; n < total; ++n)
        worker->wtimes[n].connect = INT32_MAX;/*(initial value for keepalive)*/

    for (int i = 0; i < num_clients; ++i)
        client_init(worker, wconf->config, i);
}


__attribute_cold__
__attribute_nonnull__()
__attribute_noinline__
static void
worker_delete (Worker * const restrict worker,
               Worker_Config * const restrict wconf)
{
    int i;
    const int num_clients = wconf->num_clients;

    /* adjust bytes_total to discard count of excess responses
     * (> worker->stats.req_todo) */
    if (worker->clients[0].pipeline_max > 1) {
        for (i = 0; i < num_clients; ++i) {
            worker->stats.bytes_total -= ( worker->clients[i].buffer_offset
                                         - worker->clients[i].parser_offset );
        }
    }

    memcpy(&wconf->stats, &worker->stats, sizeof(Stats));
    wconf->wtimes = worker->wtimes;
    for (i = 0; i < num_clients; ++i)
        client_delete(worker->clients+i);
    free(worker->clients);
    free(worker->pfds);

  #ifdef WEIGHTTP_SPLICE
    if (-1 != worker->pipefds[0]) close(worker->pipefds[0]);
    if (-1 != worker->pipefds[1]) close(worker->pipefds[1]);
  #endif
}


__attribute_cold__
__attribute_noinline__
__attribute_nonnull__()
static void
wconfs_init (Config * const restrict config)
{
  #ifdef WEIGHTTP_SPLICE
    int pipefds[2];
    if (0 != pipe2(pipefds, O_NONBLOCK))
        perror("pipe2()");
    else {
        struct iovec iov = { config->request, config->request_size };
        const ssize_t vms = vmsplice(pipefds[1], &iov, 1, SPLICE_F_GIFT);
        if (vms == config->request_size)
            config->reqpipe = pipefds[0];
        else {
            config->reqpipe = -1;
            if (-1 == vms)
                perror("vmsplice()");
            close(pipefds[0]);
        }
        close(pipefds[1]);
    }
  #endif

    /* create Worker_Config data structures for each (future) thread */
    Worker_Config * const restrict wconfs =
      (Worker_Config *)calloc(config->thread_count, sizeof(Worker_Config));

    uint32_t rest_concur = config->concur_count % config->thread_count;
    uint32_t rest_req = config->req_count % config->thread_count;

    for (int i = 0; i < config->thread_count; ++i) {
        uint64_t reqs = config->req_count / config->thread_count;
        int concur = config->concur_count / config->thread_count;

        if (rest_concur) {
            concur += 1;
            rest_concur -= 1;
        }

        if (rest_req) {
            reqs += 1;
            rest_req -= 1;
        }

        if (!config->quiet)
            printf("spawning thread #%d: %d concurrent requests, "
                   "%"PRIu64" total requests\n", i+1, concur, reqs);

        wconfs[i].config = config;
        wconfs[i].id = i;
        wconfs[i].num_clients = concur;
        wconfs[i].num_requests = reqs;
    }

    config->wconfs = wconfs;
}


__attribute_cold__
__attribute_noinline__
__attribute_nonnull__()
static void
wconfs_delete (const Config * const restrict config)
{
    free(config->wconfs);
    if (config->request < config->buf
        || config->buf+sizeof(config->buf) <= config->request)
        free(config->request);

    if (config->laddrs.num > 0) {
        for (int i = 0; i < config->laddrs.num; ++i)
            freeaddrinfo(config->laddrs.addrs[i]);
        free(config->laddrs.addrs);
    }

  #ifdef WEIGHTTP_SPLICE
    if (-1 != config->reqpipe) close(config->reqpipe);
  #endif
}


__attribute_cold__
__attribute_nonnull__()
static void
client_buffer_shift (Client * const restrict client)
{
    /*(caller should check client->parser_offset != 0 prior to call)*/
    /*(+1 for trailing '\0' added to buffer by weighttp)*/
    memmove(client->buffer, client->buffer + client->parser_offset,
            (client->buffer_offset -= client->parser_offset) + 1);
    client->parser_offset = 0;
}


__attribute_noinline__
static uint64_t
client_gettime_uint64 (void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    /* encode struct timeval in 64 bits */
    return ((uint64_t)tv.tv_sec << 20) | tv.tv_usec;
}


static uint32_t
client_difftime (const uint64_t e, const uint64_t b)
{
    return (uint32_t)(((e >> 20) - (b >> 20)) * 1000000
                      + ((e & 0xFFFFF) - (b & 0xFFFFF)));
}


__attribute_hot__
__attribute_nonnull__()
static void
client_reset (Client * const restrict client, const int success)
{
    /* response time */
    const uint64_t t = client_gettime_uint64();
    Times * const restrict tstats = client->times;
    tstats->t = (uint64_t)client_difftime(t, tstats->t);

    /* update worker stats */
    Stats * const restrict stats = client->stats;
    if (__builtin_expect( (!client->req_redo), 1)) {
        ++stats->req_done;
        if (__builtin_expect( (0 != success), 1))
            ++stats->req_success;
        else
            ++stats->req_failed;
    }

    client->revents = (stats->req_started < stats->req_todo) ? POLLOUT : 0;
    if (client->revents && client->keepalive) {
        /*(assumes writable; will find out soon if not and register interest)*/
        client->times = client->wtimes + stats->req_started++;
        client->parser_state = PARSER_START;
        client->keptalive = 1;
        if (client->parser_offset == client->buffer_offset) {
            client->parser_offset = 0;
            client->buffer_offset = 0;
        }
      #if 0
        else if (client->parser_offset > (CLIENT_BUFFER_SIZE/2))
            client_buffer_shift(client);
        /* future: if we tracked size of headers for first successful response,
         * we might use that size to determine whether or not to memmove()
         * any remaining contents in client->buffer to the beginning of buffer,
         * e.g. if parser_offset + expected_response_len exceeds buffer size
         * On the size, if we expect to already have completed response fully
         * received in buffer, then skip the memmove(). */
      #endif
        if (--client->pipelined) {
            if (client->buffer_offset)
                client->revents |= POLLIN;
            /*(pipelined request; now about to wait for response)*/
            client->times->t = t;
            /* note: may lower pipelining perf by delaying new request batch */
            if ((   client->pipeline_max > 4
                 && client->pipeline_max - 4 < client->pipelined)
                || stats->req_todo - stats->req_started
                     == (uint64_t)client->pipelined) {
                client->revents &= ~POLLOUT;
                client->pfd->events &= ~POLLOUT;
            }
        }
    }
    else {
      #ifdef _WIN32
        closesocket(client->pfd->fd);
      #else
        close(client->pfd->fd);
      #endif
        client->pfd->fd = -1;
        client->pfd->events = 0;
        /*client->pfd->revents = 0;*//*(POLLOUT set above if more req_todo)*/
        client->parser_state = PARSER_CONNECT;
    }
}


__attribute_cold__
__attribute_noinline__
__attribute_nonnull__()
static int
client_error (Client * const restrict client)
{
    if (!client->req_redo)
        ++client->stats->req_error;
    if (client->parser_state != PARSER_BODY) {
        /*(might include subsequent responses to pipelined requests, but
         * some sort of invalid response received if client_error() called)*/
        client->stats->bytes_headers +=
          (client->buffer_offset - client->parser_offset);
        client->buffer_offset = 0;
        client->parser_offset = 0;
    }
    client->keepalive = 0;
    client_reset(client, 0);
    return 0;
}


__attribute_cold__
__attribute_noinline__
__attribute_nonnull__()
static int
client_perror (Client * const restrict client, const char * const restrict tag)
{
    const int errnum = errno;
    client->buffer[0] = '\0';
  #if defined(_GNU_SOURCE) && defined(__GLIBC__)
    const char * const errstr =
      strerror_r(errnum, client->buffer, sizeof(client->buffer));
  #else /* XSI-compliant strerror_r() */
    const char * const errstr = client->buffer;
    strerror_r(errnum, client->buffer, sizeof(client->buffer));
  #endif
    fprintf(stderr, "error: %s failed: (%d) %s\n", tag, errnum, errstr);
    return client_error(client);
}


__attribute_nonnull__()
static void
client_connected (Client * const restrict client)
{
    client->request_offset = 0;
    client->buffer_offset = 0;
    client->parser_offset = 0;
    client->parser_state = PARSER_START;
    client->pipelined = 0;
    client->keepalive = client->config_keepalive;
    client->keptalive = 0;
    /*client->success = 0;*/

    client->pfd->events |= POLLIN | POLLRDHUP;

    Times * const restrict tstats = client->times;
    const uint64_t t = client_gettime_uint64();
    tstats->connect = client_difftime(t, tstats->t);
    tstats->t = t; /* request sent time for initial request */
    /*(assume writable socket ready for sending entire initial request)*/
}


__attribute_noinline__
__attribute_nonnull__()
static int
client_connect (Client * const restrict client)
{
    const struct addrinfo * const restrict raddr = client->raddr;
    int fd = client->pfd->fd;
    int opt;

    if (-1 == fd) {
        if (__builtin_expect( (!client->req_redo), 1))
            client->times = client->wtimes + client->stats->req_started++;
        else
            client->req_redo = 0;

        do {
            fd = socket(raddr->ai_family,raddr->ai_socktype,raddr->ai_protocol);
        } while (__builtin_expect( (-1 == fd), 0) && errno == EINTR);
        if (fd < 0)
            return client_perror(client, "socket()");

      #if !SOCK_NONBLOCK
        if (0 != fcntl(fd, F_SETFL, O_NONBLOCK | O_RDWR)) /* set non-blocking */
            client_perror(client, "fcntl() O_NONBLOCK");
      #endif
        client->pfd->fd = fd;

        if (raddr->ai_family != AF_UNIX) {
            opt = 1;
            if (0 != setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt,sizeof(opt)))
                client_perror(client, "setsockopt() TCP_NODELAY");
        }

        if (0 != client->so_bufsz) {
            opt = client->so_bufsz;
            if (0 != setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &opt, sizeof(opt)))
                client_perror(client, "setsockopt() SO_SNDBUF");
            if (0 != setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &opt, sizeof(opt)))
                client_perror(client, "setsockopt() SO_RCVBUF");
        }

        if (raddr->ai_family != AF_UNIX) {
            /*(might not be correct for real clients, but ok for load test)*/
            struct linger l = { .l_onoff = 1, .l_linger = 0 };
            if (0 != setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof(l)))
                client_perror(client, "setsockopt() SO_LINGER");
        }

        if (NULL != client->laddr
            && 0 != bind(fd, client->laddr->ai_addr, client->laddr->ai_addrlen))
            return client_perror(client, "bind() (local addr)");

        /* connect() start time */
        client->times->t = client_gettime_uint64();

        int rc;
      #ifdef TCP_FASTOPEN
        ssize_t wr = 0;
        if (client->tcp_fastopen) {/*(disabled if config->proxy is AF_UNIX)*/
            wr = sendto(fd, client->request, client->request_size,
                        MSG_FASTOPEN | MSG_DONTWAIT | MSG_NOSIGNAL,
                        raddr->ai_addr, raddr->ai_addrlen);
            if (wr > 0) {
                client_connected(client);
                if (client->request_size == (uint32_t)wr) {
                    if (++client->pipelined == client->pipeline_max) {
                        client->revents &= ~POLLOUT;
                        client->pfd->events &= ~POLLOUT;
                    }
                }
                else
                    client->request_offset = (uint32_t)wr;
                return 1;
            }
            else if (-1 == wr && errno == EOPNOTSUPP)
                wr = 0;
            else {
                /*(0 == wr with sendto() should not happen
                 * with MSG_FASTOPEN and non-zero request_size)*/
                wr = -1;
                rc = -1;
            }
        }
        if (0 == wr)
      #endif
        do {
            rc = connect(fd, raddr->ai_addr, raddr->ai_addrlen);
        } while (__builtin_expect( (-1 == rc), 0) && errno == EINTR);

        if (0 != rc) {
            switch (errno) {
              case EINPROGRESS:
              case EALREADY:
                /* async connect now in progress */
                client->revents &= ~POLLOUT;
                client->pfd->events |= POLLOUT;
                return 0;
              default:
                return client_perror(client, "connect()");
            }
        }
    }
    else {
        opt = 0;
        socklen_t optlen = sizeof(opt);
        if (0 != getsockopt(fd,SOL_SOCKET,SO_ERROR,&opt,&optlen) || 0 != opt) {
            if (0 != opt) errno = opt; /* error connecting */
            return client_perror(client, "connect() getsockopt()");
        }
    }

    /* successfully connected */
    client_connected(client);
    return 1;
}


__attribute_cold__
__attribute_noinline__
__attribute_nonnull__()
static int
client_parse_pending_more_data (Client * const restrict client)
{
    /* check if buffer is full (sizeof(client->buffer)-1 for added '\0').
     * PARSER_BODY handling consumes data, so buffer full might happen
     * only when parsing response header line or chunked header line.
     * If buffer is full, then line is *way* too long. */
    if (__builtin_expect(
          (client->buffer_offset == sizeof(client->buffer)-1), 0)) {
        if (__builtin_expect( (0 == client->parser_offset), 0))
            return client_error(client); /* e.g. response header too big */
    }

    return 1;
}


__attribute_nonnull__()
static int
client_parse_chunks (Client * const restrict client)
{
    do {
        char *str = client->buffer+client->parser_offset;

        if (-1 == client->chunk_size) {
            /* read chunk size */
            /*char *end = strchr(str, '\n');*/
            char *end =
              memchr(str, '\n', client->buffer_offset - client->parser_offset);
            if (!end) /* partial line */
                return client_parse_pending_more_data(client);
            ++end;

            /* assume server sends valid chunked header
             * (not validating; (invalid) chunked header without any
             *  hex digits is treated as 0-chunk, ending input) */
            client->chunk_size = 0;
            do {
                int c = *str;
                client->chunk_size <<= 4;
                if (c >= '0' && c <= '9')
                    client->chunk_size |= (c - '0');
                else if ((c |= 0x20) >= 'a' && c <= 'f')
                    client->chunk_size |= (c - 'a' + 10);
                else {
                    if (c=='\r' || c=='\n' || c==' ' || c=='\t' || c==';')
                        break;
                    return client_error(client);
                }
            } while (*++str != '\r' && *str != '\n');

            if (0 == client->chunk_size) {
                /* chunk of size 0 marks end of content body
                 * check for final "\r\n" ending response
                 * (not handling trailers if user supplied -H "TE: trailers") */
                if (end + 2 > client->buffer + client->buffer_offset) {
                    /* final "\r\n" not yet received */
                    client->chunk_size = -1;
                    return client_parse_pending_more_data(client);
                }
                if (end[0] == '\r' && end[1] == '\n')
                    client->stats->bytes_headers += 2;
                else
                    client->keepalive = 0; /*(just close con if trailers)*/
                client->parser_offset = end - client->buffer + 2;
                client_reset(client, client->http_status_success);
                return 0; /*(trigger loop continue in caller)*/
            }

            client->parser_offset = end - client->buffer;
            client->chunk_received = 0;
            client->chunk_size += 2; /*(for chunk "\r\n" end)*/
        }

        /* consume chunk until chunk_size is reached */
        const int rd = client->buffer_offset - client->parser_offset;
        int chunk_remain = client->chunk_size - client->chunk_received;
        if (rd >= chunk_remain) {
            client->chunk_received += chunk_remain;
            client->parser_offset += chunk_remain;

            if (client->buffer[client->parser_offset-1] != '\n')
                return client_error(client);

            /* got whole chunk, next! */
            client->chunk_size = -1;
            client->chunk_received = 0;
        }
        else {
            client->chunk_received += rd;
            client->parser_offset += rd;
        }

    } while (client->parser_offset != client->buffer_offset);/* more to parse */

    client->parser_offset = 0;
    client->buffer_offset = 0;
    return 1;
}


__attribute_hot__
__attribute_nonnull__()
__attribute_pure__
static uint64_t
client_parse_uint64 (const char * const restrict str)
{
    /* quick-n-dirty conversion of numerical string to integral number
     * Note: not validating field and not checking for valid number
     * (weighttp not intended for use with requests > 2 GB, as transfer
     *  of body would take the majority of the time in that case)*/
    uint64_t x = 0;
    for (int i = 0; (unsigned int)(str[i] - '0') < 10u; ++i) {
        x *= 10;
        x += (unsigned int)(str[i] - '0');
    }
    return x;
}


__attribute_hot__
__attribute_noinline__
__attribute_nonnull__()
static int
client_parse (Client * const restrict client)
{
    char *end;
    uint32_t len;

    /* future: might combine PARSER_START and PARSER_HEADER states by
     * collecting entire set of headers (reading until "\r\n\r\n")
     * prior to parsing */

    switch (client->parser_state) {

      case PARSER_START:
        /* look for HTTP/1.1 200 OK (though also accept HTTP/1.0 200)
         * Note: does not support 1xx intermediate messages */
        /* Note: not validating response line; assume valid */
        /*end = strchr(client->buffer+client->parser_offset, '\n');*/
        end = memchr(client->buffer+client->parser_offset, '\n',
                     client->buffer_offset - client->parser_offset);
        if (NULL != end) {
            len = (uint32_t)(end - client->buffer - client->parser_offset + 1);
            if (len < sizeof("HTTP/1.1 200\r\n")-1)
                return client_error(client);
        }
        else /*(partial response line; incomplete)*/
            return client_parse_pending_more_data(client);

        client->content_length = -1;
        client->chunked = 0;
        client->http_status_success = 1;
        switch (client->buffer[client->parser_offset + sizeof("HTTP/1.1 ")-1]
                - '0') {
          case 2:
            ++client->stats->req_2xx;
            break;
          case 3:
            ++client->stats->req_3xx;
            break;
          case 4:
            client->http_status_success = 0;
            ++client->stats->req_4xx;
            break;
          case 5:
            client->http_status_success = 0;
            ++client->stats->req_5xx;
            break;
          default:
            /* invalid status code */
            return client_error(client);
        }
        client->stats->bytes_headers += len;
        client->parser_offset += len;
        client->parser_state = PARSER_HEADER;

        __attribute_fallthrough__

      case PARSER_HEADER:
        /* minimally peek at Content-Length, Connection, Transfer-Encoding */
        do {
            const char *str = client->buffer+client->parser_offset;
            /*end = strchr(str, '\n');*/
            end =
              memchr(str, '\n', client->buffer_offset - client->parser_offset);
            if (NULL == end)
                return client_parse_pending_more_data(client);
            len = (uint32_t)(end - str + 1);
            client->stats->bytes_headers += len;
            client->parser_offset += len;

            /* minimum lengths for us to check for ':' in the following:
             *   "Content-Length:0\r\n"
             *   "Connection:close\r\n"
             *   "Transfer-Encoding:chunked\r\n"*/
            if (end - str < 17)
                continue;

            if (str[14] == ':'
                && (0 == memcmp(str, "Content-Length",
                                sizeof("Content-Length")-1)
                    || 0 == strncasecmp(str, "Content-Length",
                                        sizeof("Content-Length")-1))) {
                str += sizeof("Content-Length:")-1;
                if (__builtin_expect( (*str == ' '), 1))
                    ++str;
                while (__builtin_expect( (*str == ' '), 0)
                       || __builtin_expect( (*str == '\t'), 0))
                    ++str;
                client->content_length = client_parse_uint64(str);
            }
            else if (str[10] == ':'
                     && (0 == memcmp(str, "Connection",
                                     sizeof("Connection")-1)
                         || 0 == strncasecmp(str, "Connection",
                                             sizeof("Connection")-1))) {
                str += sizeof("Connection:")-1;
                if (__builtin_expect( (*str == ' '), 1))
                    ++str;
                while (__builtin_expect( (*str == ' '), 0)
                       || __builtin_expect( (*str == '\t'), 0))
                    ++str;
                if ((*str | 0x20) == 'c')  /*(assume "close")*/
                    client->keepalive = 0;
            }
            else if (str[17] == ':'
                     && (0 == memcmp(str, "Transfer-Encoding",
                                     sizeof("Transfer-Encoding")-1)
                         || 0 == strncasecmp(str, "Transfer-Encoding",
                                             sizeof("Transfer-Encoding")-1))) {
                client->chunked = 1; /*(assume "chunked")*/
                client->chunk_size = -1;
                client->chunk_received = 0;
            }

        } while (end[1] != '\r' || end[2] != '\n');

        /* body reached */
        client->stats->bytes_headers += 2;
        client->parser_offset += 2;
        client->parser_state = PARSER_BODY;
        if (client->http_head)
            client->content_length = 0;
        else if (!client->chunked && -1 == client->content_length)
            client->keepalive = 0;

        /* response time to first byte does not include connect() time for
         * consistency with keep-alive and pipeline requests, even though
         * traditional TTFB includes DNS resolution and connect() time.
         * Also for simplicity, ttfb here includes time to receive complete
         * response headers rather than first byte of headers or first byte
         * of body (which might not have arrived yet) */
        client->times->ttfb =
          client_difftime(client_gettime_uint64(), client->times->t);

        __attribute_fallthrough__

      case PARSER_BODY:
        /* consume and discard response body */

        if (client->chunked)
            return client_parse_chunks(client);
        else {
            /* consume all data until content-length reached (or EOF) */
            if (-1 != client->content_length) {
                uint32_t rd = client->buffer_offset - client->parser_offset;
                if (client->content_length > rd)
                    client->content_length -= rd;
                else { /* full response received */
                    client->parser_offset += client->content_length;
                    client_reset(client, client->http_status_success);
                    return 0; /*(trigger loop continue in caller)*/
                }
            }

            client->buffer_offset = 0;
            client->parser_offset = 0;
            return 1;
        }

      case PARSER_CONNECT: /*(should not happen here)*/
        break;
    }

    return 1;
}


#ifdef WEIGHTTP_SPLICE
__attribute_cold__
__attribute_noinline__
static void
client_discard_from_pipe (const int fd)
{
    char buf[8192];
    ssize_t rd;
    do {
        rd = read(fd, buf, sizeof(buf));
    } while (__builtin_expect( (rd == sizeof(buf)), 0)
             || (__builtin_expect( (-1 == rd), 0) && errno == EINTR));
}
#endif


__attribute_nonnull__()
static void
client_revents (Client * const restrict client)
{
    int x = 0;

    while (client->revents & (POLLIN|POLLRDHUP)) {
        /* parse pipelined responses */
        if (client->buffer_offset && !client_parse(client))
            continue;

        if (++x > 2 && !(client->revents & POLLRDHUP))
            break;

        /* adjust client recv buffer if nearly full (e.g. pipelined responses)*/
        if (client->parser_offset
            && sizeof(client->buffer) - client->buffer_offset < 1024)
            client_buffer_shift(client);

        ssize_t r;
        do {
            r = recv(client->pfd->fd, client->buffer+client->buffer_offset,
                     sizeof(client->buffer) - client->buffer_offset - 1,
                     MSG_DONTWAIT);
        } while (__builtin_expect( (-1 == r), 0) && errno == EINTR);
        if (__builtin_expect( (r > 0), 1)) {
            if (r < (ssize_t)(sizeof(client->buffer)-client->buffer_offset-1))
                client->revents &= ~POLLIN;
            client->buffer[(client->buffer_offset += (uint32_t)r)] = '\0';
            client->stats->bytes_total += r;

            if (!client_parse(client))
                continue;
            /*(also, continue)*/
        }
        else {
            if (-1 == r) { /* error */
                if (errno == EAGAIN
                   #if EAGAIN != EWOULDBLOCK
                    || errno == EWOULDBLOCK
                   #endif
                   ) {
                    client->revents &= ~POLLIN;
                    break;
                }
                else
                    client_perror(client, "read()");
            }
            else { /* disconnect; evaluate if end-of-response or error */
                if (client->http_status_success
                    && client->parser_state == PARSER_BODY
                    && !client->chunked && -1 == client->content_length) {
                    client->keepalive = 0;
                    client_reset(client, 1);
                }
                else {
                    if (client->keptalive
                        && client->parser_state == PARSER_START
                        && 0 == client->buffer_offset) {
                        /* (server might still read and discard request,
                         *  but has initiated connection close) */
                        client->req_redo = 1;
                    }
                    client_error(client);
                }
            }
        }
    }

    if (__builtin_expect( (client->revents & (POLLERR|POLLHUP)), 0)) {
        client->keepalive = 0;
        client_reset(client, 0);
    }

    x = 0;

    while (client->revents & POLLOUT) {
        ssize_t r;
        if (client->parser_state == PARSER_CONNECT && !client_connect(client))
            continue;

      #ifdef WEIGHTTP_SPLICE
        r = -1;
        if (-1 != client->pipefds[2] && 0 == client->request_offset) {
            r = tee(client->pipefds[2],client->pipefds[1],client->request_size,
                    SPLICE_F_MOVE | SPLICE_F_NONBLOCK | SPLICE_F_GIFT);
            if (r == (ssize_t)client->request_size) {
                r = splice(client->pipefds[0], NULL, client->pfd->fd, NULL,
                           client->request_size,
                           SPLICE_F_MOVE | SPLICE_F_NONBLOCK | SPLICE_F_GIFT);
                if (r != (ssize_t)client->request_size) {
                    if (r > 0)
                        client->request_offset = (uint32_t)r;
                    /*(must empty pipes for other clients to reuse)*/
                    client_discard_from_pipe(client->pipefds[0]);
                }
            }
            else if (-1 != r) {
                /*(precautions taken so that partial copy should not happen)*/
                /*(still, must empty pipes for other clients to reuse)*/
                client_discard_from_pipe(client->pipefds[0]);
                r = -1;
            }
        }
        if (-1 == r) /*(fall through to write())*/
      #endif
        {
          #ifndef HAVE_SENDMMSG
          #ifdef __GLIBC__
          #define HAVE_SENDMMSG 1
          #endif
          #endif

          #if HAVE_SENDMMSG
            /* Attempt to batch requests in fewer syscalls by using sendmmsg().
             * pipelining may increase raw requests per second, but also
             * increases worst response latency due in part to weighttp being
             * busy, and that pipelined requests must be resent after server
             * closes keep-alive. */
            int n = client->pipeline_max - client->pipelined;
            uint64_t nmax = client->stats->req_todo
                          - client->stats->req_started
                          - client->pipelined;
            if ((uint64_t)n > nmax)
                n = (int)nmax;
            if (n > 1) {
                if (n > 8) n = 8;
                struct iovec iov[8];
                for (int i = 0; i < n; ++i) {
                    *(const void **)&iov[i].iov_base = client->request;
                    iov[i].iov_len = client->request_size;
                }
                iov[0].iov_base = (char *)iov[0].iov_base
                                + client->request_offset;
                iov[0].iov_len -= client->request_offset;
                struct mmsghdr msg;
                memset(&msg, 0, sizeof(msg));
                msg.msg_hdr.msg_iov = iov;
                msg.msg_hdr.msg_iovlen = n;
                do {
                    r = sendmmsg(client->pfd->fd, &msg, 1,
                                 MSG_DONTWAIT | MSG_NOSIGNAL);
                } while (__builtin_expect( (-1 == r), 0) && errno == EINTR);
                if (1 == r
                    && (size_t)(r = (ssize_t)msg.msg_len) > iov[0].iov_len) {
                    /* multiple requests sent; handle all but last one */
                    n = 1;
                    if ((r -= (ssize_t)iov[0].iov_len) > client->request_size) {
                        n += r / client->request_size;
                        r %= client->request_size;
                        if (0 == r) { /* leave last request for below */
                            --n;
                            r = client->request_size;
                        }
                    }
                    client->request_offset = 0;
                    client->pipelined += n;
                    if (client->pipelined == n && client->keptalive)
                        /*(update times; client->pipelined was 0)*/
                        client->times->t = client_gettime_uint64();
                }
            }
            else
          #endif
            do {
                r = send(client->pfd->fd,
                         client->request+client->request_offset,
                         client->request_size - client->request_offset,
                         MSG_DONTWAIT | MSG_NOSIGNAL);
            } while (__builtin_expect( (-1 == r), 0) && errno == EINTR);
        }
        if (__builtin_expect( (r > 0), 1)) {
            if (client->request_size == (uint32_t)r
                || client->request_size==(client->request_offset+=(uint32_t)r)){
                /* request sent */
                client->request_offset = 0;
                ++client->pipelined;
                if (client->pipelined == 1 && client->keptalive)
                    client->times->t = client_gettime_uint64();
                if (client->pipelined < client->pipeline_max && ++x < 8
                    && client->stats->req_todo
                     - client->stats->req_started
                     - client->pipelined)
                    continue;
                else {
                    client->revents &= ~POLLOUT; /*(trigger write() loop exit)*/
                    client->pfd->events &= ~POLLOUT;
                }
            }
            else {
                client->request_offset += (uint32_t)r;
                client->revents &= ~POLLOUT; /*(trigger write() loop exit)*/
                client->pfd->events |= POLLOUT;
            }
        }
        else {
            if (-1 == r) { /* error */
                if (errno == EAGAIN
                   #if EAGAIN != EWOULDBLOCK
                    || errno == EWOULDBLOCK
                   #endif
                   ) {
                    client->revents &= ~POLLOUT;
                    client->pfd->events |= POLLOUT;
                    break;
                }
                else
                    client_perror(client, "send()");
            }
            else { /* (0 == r); not expected; not attempting to write 0 bytes */
                client->keepalive = 0;
                client_reset(client, 0);
            }
        }
    }
}


__attribute_nonnull__()
static void *
worker_thread (void * const arg)
{
    Worker worker;
    int i, nready;
    Worker_Config * const restrict wconf = (Worker_Config *)arg;
    worker_init(&worker, wconf);

    const int num_clients = wconf->num_clients;
    const int progress =
      (0==wconf->id && !wconf->config->quiet); /* report only in first thread */
    const uint64_t progress_interval =         /* print every 10% done */
     (worker.stats.req_todo > 10) ? worker.stats.req_todo / 10 : 1;
    uint64_t progress_next = progress_interval;

    /* start all clients */
    for (i = 0; i < num_clients; ++i) {
        if (worker.stats.req_started < worker.stats.req_todo) {
            worker.clients[i].revents = POLLOUT;
            client_revents(worker.clients+i);
        }
    }

    while (worker.stats.req_done < worker.stats.req_todo) {
        do {                                 /*(infinite wait)*/
            nready = poll(worker.pfds, (nfds_t)num_clients, -1);
        } while (__builtin_expect( (-1 == nready), 0) && errno == EINTR);
        if (__builtin_expect( (-1 == nready), 0)) {
            /*(repurpose client_perror(); use client buffer for strerror_r())*/
            client_perror(worker.clients+0, "poll()"); /* fatal; ENOMEM */
            return NULL;
        }

        i = 0;
        do {
            while (0 == worker.pfds[i].revents)
                ++i;
            worker.clients[i].revents |= worker.pfds[i].revents;
            worker.pfds[i].revents = 0;
            client_revents(worker.clients+i);
        } while (--nready);

        if (progress) {
            /*(assume progress of one thread approximates that of all threads)*/
            /*(RFE: main thread could poll and report progress of all workers)*/
            while (__builtin_expect( worker.stats.req_done >= progress_next,0)){
                printf("progress: %3d%% done\n", (int)
                       (worker.stats.req_done * 100 / worker.stats.req_todo));
                if (progress_next == worker.stats.req_todo)
                    break;
                progress_next += progress_interval;
                if (__builtin_expect( progress_next > worker.stats.req_todo, 0))
                    progress_next = worker.stats.req_todo;
            }
        }
    }

    worker_delete(&worker, wconf);
    return NULL;
}


__attribute_cold__
__attribute_noinline__
__attribute_nonnull__()
static void
config_error_diagnostic (const char * const restrict errfmt,
                         const int perr, va_list ap)
{
    const int errnum = errno;
    show_version();
    show_help();
    fflush(stdout);

    fprintf(stderr, "\nerror: ");
    vfprintf(stderr, errfmt, ap);

    if (!perr)
        fprintf(stderr, "\n\n");
    else {
        char buf[1024];
        buf[0] = '\0';
      #if defined(_GNU_SOURCE) && defined(__GLIBC__)
        const char * const errstr = strerror_r(errnum, buf, sizeof(buf));
      #else /* XSI-compliant strerror_r() */
        const char * const errstr = buf;
        strerror_r(errnum, buf, sizeof(buf));
      #endif

        fprintf(stderr, ": (%d) %s\n\n", errnum, errstr);
    }
}


__attribute_cold__
__attribute_format__((__printf__, 1, 2))
__attribute_noinline__
__attribute_nonnull__()
__attribute_noreturn__
static void
config_error (const char * const restrict errfmt, ...)
{
    va_list ap;
    va_start(ap, errfmt);
    config_error_diagnostic(errfmt, 0, ap);
    va_end(ap);
    exit(1);
}


__attribute_cold__
__attribute_format__((__printf__, 1, 2))
__attribute_noinline__
__attribute_nonnull__()
__attribute_noreturn__
static void
config_perror (const char * const restrict errfmt, ...)
{
    va_list ap;
    va_start(ap, errfmt);
    config_error_diagnostic(errfmt, 1, ap);
    va_end(ap);
    exit(1);
}


typedef struct config_params {
  const char *method;
  const char *uri;
        char *laddrstr;
  int use_ipv6;
  int headers_num;
  int cookies_num;
  const char *headers[64];
  const char *cookies[64];
  const char *body_content_type;
  const char *body_filename;
  const char *authorization;
  const char *proxy_authorization;
} config_params;


__attribute_cold__
__attribute_nonnull__()
static int
config_laddr (Config * const restrict config,
              const char * const restrict laddrstr)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    /*hints.ai_flags |= AI_NUMERICHOST;*/
    hints.ai_family   = config->raddr.ai_family;
    hints.ai_socktype = SOCK_STREAM;

    if (0 != getaddrinfo(laddrstr, NULL, &hints, &res) || NULL == res)
        return 0;

    config->laddr.ai_family   = res->ai_family;
    config->laddr.ai_socktype = res->ai_socktype;
    config->laddr.ai_protocol = res->ai_protocol;
    config->laddr.ai_addrlen  = res->ai_addrlen;
    config->laddr.ai_addr     = (struct sockaddr *)
      memcpy(&config->laddr_storage, res->ai_addr, res->ai_addrlen);

    freeaddrinfo(res);
    return 1;
}


__attribute_cold__
__attribute_nonnull__()
static int
config_laddrs (Config * const restrict config,
               char * const restrict laddrstr)
{
    char *s;
    int num = 1;
    for (s = laddrstr; NULL != (s = strchr(s, ',')); s = s+1) ++num;
    if (1 == num) return config_laddr(config, laddrstr);

    struct addrinfo hints, **res;
    memset(&hints, 0, sizeof(hints));
    /*hints.ai_flags |= AI_NUMERICHOST;*/
    hints.ai_family   = config->raddr.ai_family;
    hints.ai_socktype = SOCK_STREAM;

    config->laddrs.num = num;
    config->laddrs.addrs = res =
      (struct addrinfo **)calloc((size_t)num, sizeof(struct addrinfo *));

    s = laddrstr;
    for (int i = 0; i < num; ++i, ++res) {
        char *e = strchr(s, ',');
        if (NULL != e) *e = '\0';

        *res = NULL;
        if (0 != getaddrinfo(s, NULL, &hints, res) || NULL == *res)
            return 0; /*(leave laddrstr modified so last addr is one w/ error)*/

        if (NULL == e) break;
        *e = ',';
        s = e+1;
    }

    return 1;
}


__attribute_cold__
__attribute_nonnull__()
static void
config_raddr (Config * const restrict config,
              const char * restrict hostname, uint16_t port, const int use_ipv6)
{
    if (config->proxy && config->proxy[0] == '/') {
        #ifndef UNIX_PATH_MAX
        #define UNIX_PATH_MAX 108
        #endif
        const size_t len = strlen(config->proxy);
        if (len >= UNIX_PATH_MAX)
            config_error("socket path too long: %s", config->proxy);

        config->raddr.ai_family   = AF_UNIX;
        config->raddr.ai_socktype = SOCK_STREAM | SOCK_NONBLOCK;
        config->raddr.ai_protocol = 0;
        /* calculate effective SUN_LEN(); macro not always available)*/
        config->raddr.ai_addrlen  =
          (socklen_t)((size_t)(((struct sockaddr_un *) 0)->sun_path) + len);
        config->raddr.ai_addr     = (struct sockaddr *)&config->raddr_storage;
        memset(&config->raddr_storage, 0, sizeof(config->raddr_storage));
        config->raddr_storage.ss_family = AF_UNIX;
        memcpy(((struct sockaddr_un *)&config->raddr_storage)->sun_path,
               config->proxy, len+1);
        return;
    }

    char host[1024]; /*(host should be < 256 chars)*/
    if (config->proxy) { /* (&& config->proxy[0] != '/') */
        char * const colon = strrchr(config->proxy, ':');
        if (colon) {
            char *endptr;
            unsigned long i = strtoul(colon+1, &endptr, 10);
            if (*endptr == '\0' && 0 != i && i <= USHRT_MAX)
                port = (unsigned short)i;
            else /*(might mis-parse IPv6 addr which omitted port)*/
                config_error("could not parse -X proxy: %s", config->proxy);

            const size_t len = (size_t)(colon - config->proxy);
            if (len >= sizeof(host))
                config_error("proxy host path too long: %s", config->proxy);
            memcpy(host, config->proxy, len);
            host[len] = '\0';
            hostname = host;
        }
        else {
            hostname = config->proxy;
            port = 80; /* default HTTP port */
        }
    }

    struct addrinfo hints, *res, *res_first;
    memset(&hints, 0, sizeof(hints));
    hints.ai_flags |= AI_NUMERICSERV;
    hints.ai_family = PF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%hu", port);

    if (0 != getaddrinfo(hostname, port_str, &hints, &res_first))
        config_error("could not resolve hostname: %s", hostname);

    for (res = res_first; res != NULL; res = res->ai_next) {
        if (res->ai_family == (use_ipv6 ? AF_INET6 : AF_INET)) {
            config->raddr.ai_family   = res->ai_family;
            config->raddr.ai_socktype = res->ai_socktype | SOCK_NONBLOCK;
            config->raddr.ai_protocol = res->ai_protocol;
            config->raddr.ai_addrlen  = res->ai_addrlen;
            config->raddr.ai_addr     = (struct sockaddr *)
              memcpy(&config->raddr_storage, res->ai_addr, res->ai_addrlen);
            break;
        }
    }

    freeaddrinfo(res_first);
    if (NULL == res)
        config_error("could not resolve hostname: %s", hostname);
}


__attribute_cold__
__attribute_nonnull__()
static int
config_base64_encode_pad (char * const restrict dst, const size_t dstsz,
                          const char * const restrict ssrc)
{
    static const char base64_table[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=";

    const size_t srclen = strlen(ssrc);
    const int rem    = (int)(srclen % 3);
    const int tuples = (int)(srclen / 3);
    const int tuplen = (int)(srclen - (size_t)rem);
    if (srclen > INT_MAX/2)  /*(ridiculous size; prevent integer overflow)*/
        return -1;
    if (dstsz < (size_t)(4*tuples + (rem ? 4 : 0) + 1))
        return -1;

    int s = 0, d = 0;
    unsigned int v;
    const unsigned char * const src = (const unsigned char *)ssrc;
    for (; s < tuplen; s += 3, d += 4) {
        v = (src[s+0] << 16) | (src[s+1] << 8) | src[s+2];
        dst[d+0] = base64_table[(v >> 18) & 0x3f];
        dst[d+1] = base64_table[(v >> 12) & 0x3f];
        dst[d+2] = base64_table[(v >>  6) & 0x3f];
        dst[d+3] = base64_table[(v      ) & 0x3f];
    }

    if (rem) {
        if (1 == rem) {
            v = (src[s+0] << 4);
            dst[d+2] = base64_table[64]; /* pad */
        }
        else { /*(2 == rem)*/
            v = (src[s+0] << 10) | (src[s+1] << 2);
            dst[d+2] = base64_table[v & 0x3f]; v >>= 6;
        }
        dst[d+0] = base64_table[(v >> 6) & 0x3f];
        dst[d+1] = base64_table[(v     ) & 0x3f];
        dst[d+3] = base64_table[64]; /* pad */
        d += 4;
    }

    dst[d] = '\0';
    return d; /*(base64-encoded string length; might be 0)*/
}


__attribute_cold__
__attribute_nonnull__()
static void
config_request (Config * const restrict config,
                const config_params * const restrict params)
{
    const char * restrict uri = params->uri;
    uint16_t port = 80;
    uint16_t default_port = 80;
    char host[1024]; /*(host should be < 256 chars)*/

    if (0 == strncmp(uri, "http://", sizeof("http://")-1))
        uri += 7;
    else if (0 == strncmp(uri, "https://", sizeof("https://")-1)) {
        uri += 8;
        port = default_port = 443;
        config_error("no ssl support yet");
    }

    /* XXX: note that this is not a fully proper URI parse */
    const char *c;
    if ((c = strchr(uri, ':'))) { /* found ':' => host:port */
        if (c - uri + 1 > (int)sizeof(host))
            config_error("host name in URI is too long");
        memcpy(host, uri, c - uri);
        host[c - uri] = '\0';

        char *endptr;
        unsigned long i = strtoul(c+1, &endptr, 10);
        if (0 != i && i <= USHRT_MAX) {
            port = (unsigned short)i;
            uri = endptr;
        }
        else
            config_error("could not parse URI");
    }
    else {
        if ((c = strchr(uri, '/'))) {
            if (c - uri + 1 > (int)sizeof(host))
                config_error("host name in URI is too long");
            memcpy(host, uri, c - uri);
            host[c - uri] = '\0';
            uri = c;
        }
        else {
            size_t len = strlen(uri);
            if (len + 1 > (int)sizeof(host))
                config_error("host name in URI is too long");
            memcpy(host, uri, len);
            host[len] = '\0';
            uri += len;
        }
    }

    /* resolve hostname to sockaddr */
    config_raddr(config, host, port, params->use_ipv6);

    int idx_host = -1;
    int idx_user_agent = -1;
    int idx_content_type = -1;
    int idx_content_length = -1;
    int idx_transfer_encoding = -1;
    const char * const * const restrict headers = params->headers;
    for (int i = 0; i < params->headers_num; i++) {
        if (0 == strncasecmp(headers[i],"Host:",sizeof("Host:")-1)) {
            if (-1 != idx_host)
                config_error("duplicate Host header");
            idx_host = i;
        }
        if (0 == strncasecmp(headers[i],"User-Agent:",sizeof("User-Agent:")-1))
            idx_user_agent = i;
        if (0 == strncasecmp(headers[i],"Connection:",sizeof("Connection:")-1))
            config_error("Connection request header not allowed; "
                         "use -k param to enable keep-alive");
        if (0 == strncasecmp(headers[i],"Content-Type:",
                                 sizeof("Content-Type:")-1))
            idx_content_type = i;
        if (0 == strncasecmp(headers[i],"Content-Length:",
                                 sizeof("Content-Length:")-1))
            idx_content_length = i;
        if (0 == strncasecmp(headers[i],"Transfer-Encoding:",
                                 sizeof("Transfer-Encoding:")-1))
            idx_transfer_encoding = i;
    }

    /*(simple 8k memaligned request buffer (part of struct Config))*/
    config->request =
      (char *)((uintptr_t)(config->buf + (8*1024-1)) & ~(uintptr_t)(8*1024-1));
    char * const restrict req = config->request;
    const size_t sz = sizeof(config->buf) >> 1;
    int offset = snprintf(req, sz, "%s %s HTTP/1.1\r\n", params->method,
                          config->proxy && config->proxy[0] != '/'
                            ? params->uri /*(provide full URI to proxy host)*/
                            : *uri != '\0' ? uri : "/");
    if (offset >= (int)sz)
        config_error("request too large");

    int len = (-1 != idx_host)
      ? snprintf(req+offset, sz-offset, "%s\r\n", headers[idx_host])
      : (port == default_port)
        ? snprintf(req+offset, sz-offset, "Host: %s\r\n", host)
        : snprintf(req+offset, sz-offset, "Host: %s:%hu\r\n", host, port);
    if (len >= (int)sz - offset)
        config_error("request too large");
    offset += len;

    if (!config->keep_alive) {
        len = sizeof("Connection: close\r\n")-1;
        if (len >= (int)sz - offset)
            config_error("request too large");
        memcpy(req+offset, "Connection: close\r\n", len);
        offset += len;
    }

    int fd = -1;
    off_t fsize = 0;
    if (params->body_filename) {
        #ifndef O_BINARY
        #define O_BINARY 0
        #endif
        #ifndef O_LARGEFILE
        #define O_LARGEFILE 0
        #endif
        #ifndef O_NOATIME
        #define O_NOATIME 0
        #endif
        fd = open(params->body_filename,
                  O_RDONLY|O_BINARY|O_LARGEFILE|O_NOATIME|O_NONBLOCK, 0);
        if (-1 == fd)
            config_perror("open(%s)", params->body_filename);
        struct stat st;
        if (0 != fstat(fd, &st))
            config_perror("fstat(%s)", params->body_filename);
        fsize = st.st_size;
        if (fsize > UINT32_MAX - (8*1024))
            config_error("file size too large (not supported > ~4GB) (%s)",
                         params->body_filename);

        /* If user specified Transfer-Encoding, trust that it is proper,
         * e.g. chunked, and that body_filename contains already-chunked data */
        if (-1 == idx_transfer_encoding) {
            if (-1 == idx_content_length) {
                len = snprintf(req+offset, sz-offset,
                               "Content-Length: %"PRId64"\r\n", (int64_t)fsize);
                if (len >= (int)sz - offset)
                    config_error("request too large");
                offset += len;
            } /*(else trust user specified length matching body_filename size)*/
        }
        else if (-1 != idx_content_length)
            config_error("Content-Length must be omitted "
                         "if Transfer-Encoding provided");

        if (params->body_content_type) {
            if (-1 == idx_content_type)
                config_error("Content-Type duplicated in -H and -T params");
            len = snprintf(req+offset, sz-offset,
                           "Content-Type: %s\r\n", params->body_content_type);
            if (len >= (int)sz - offset)
                config_error("request too large");
            offset += len;
        }
        else if (-1 == idx_content_type) {
            len = sizeof("Content-Type: text/plain\r\n")-1;
            if (len >= (int)sz - offset)
                config_error("request too large");
            memcpy(req+offset, "Content-Type: text/plain\r\n", len);
            offset += len;
        }
    }

    for (int i = 0; i < params->headers_num; ++i) {
        if (i == idx_host)
            continue;
        len = snprintf(req+offset, sz-offset, "%s\r\n", headers[i]);
        if (len >= (int)sz - offset)
            config_error("request too large");
        offset += len;
    }

    if (params->authorization) {
        len = snprintf(req+offset, sz-offset, "Authorization: Basic ");
        if (len >= (int)sz - offset)
            config_error("request too large");
        offset += len;

        len = config_base64_encode_pad(req+offset, sz-offset,
                                       params->authorization);
        if (len < 0)
            config_error("request too large");
        offset += len;

        if (2 >= (int)sz - offset)
            config_error("request too large");
        memcpy(req+offset, "\r\n", 3);
        offset += 2;
    }

    if (params->proxy_authorization) {
        len = snprintf(req+offset, sz-offset, "Proxy-Authorization: Basic ");
        if (len >= (int)sz - offset)
            config_error("request too large");
        offset += len;

        len = config_base64_encode_pad(req+offset, sz-offset,
                                       params->proxy_authorization);
        if (len < 0)
            config_error("request too large");
        offset += len;

        if (2 >= (int)sz - offset)
            config_error("request too large");
        memcpy(req+offset, "\r\n", 3);
        offset += 2;
    }

    if (-1 == idx_user_agent) {
        len = sizeof("User-Agent: weighttp/" PACKAGE_VERSION "\r\n")-1;
        if (len >= (int)sz - offset)
            config_error("request too large");
        memcpy(req+offset,
               "User-Agent: weighttp/" PACKAGE_VERSION "\r\n", len);
        offset += len;
    }

    const char * const * const restrict cookies = params->cookies;
    for (int i = 0; i < params->cookies_num; ++i) {
        len = snprintf(req+offset, sz-offset, "Cookie: %s\r\n",cookies[i]);
        if (len >= (int)sz - offset)
            config_error("request too large");
        offset += len;
    }

    if (3 > (int)sz - offset)
        config_error("request too large");
    memcpy(req+offset, "\r\n", 3); /*(including terminating '\0')*/
    offset += 2;               /*(not including terminating '\0')*/

    config->request_size = (uint32_t)(offset + fsize);

    if (-1 != fd && 0 != fsize) {
        /*(not checking if file changed between fstat() and read())*/
        /*(not using mmap() since we expect benchmark test file to be smallish
         * and able to fit in memory) */
        config->request = malloc(config->request_size);
        memcpy(config->request, req, (size_t)offset);
        off_t reqsz = offset;
        ssize_t rd;
        do {
            rd = read(fd, config->request+reqsz, config->request_size-reqsz);
        } while (rd > 0 ? (reqsz += rd) < config->request_size
                        : (rd < 0 && errno == EINTR));
        if (reqsz != config->request_size)
            config_perror("read(%s)", params->body_filename);
    }

    if (-1 != fd)
        close(fd);
}


__attribute_cold__
__attribute_noinline__
__attribute_nonnull__()
static void
weighttp_setup (Config * const restrict config, const int argc, char *argv[])
{
    int opt_show_help = 0;
    int opt_show_version = 0;
    config_params params;
    memset(&params, 0, sizeof(params));

    /* default settings */
    config->thread_count = 1;
    config->concur_count = 1;
    config->req_count = 0;
    config->keep_alive = 0;
    config->proxy = NULL;
    config->pipeline_max = 0;
    config->tcp_fastopen = 0;
    config->http_head = 0;
    config->so_bufsz = 0;
    config->quiet = 0;

    setlocale(LC_ALL, "C");
    signal(SIGPIPE, SIG_IGN);

    const char * const optstr = ":hVikqdlr6Fm:n:t:c:b:p:u:A:B:C:H:K:P:T:X:";
    int opt;
    while (-1 != (opt = getopt(argc, argv, optstr))) {
        switch (opt) {
          case '6':
            params.use_ipv6 = 1;
            break;
          case 'A':
            params.authorization = optarg;
            break;
          case 'B':
            params.laddrstr = optarg;
            break;
          case 'C':
            if (params.cookies_num == sizeof(params.cookies)/sizeof(char *))
                config_error("too many cookies");
            params.cookies[params.cookies_num++] = optarg;
            break;
          case 'F':
            config->tcp_fastopen = 1;
            break;
          case 'H':
            if (params.headers_num == sizeof(params.headers)/sizeof(char *))
                config_error("too many headers");
            params.headers[params.headers_num++] = optarg;
            break;
          case 'K':
            config->pipeline_max = (int)strtoul(optarg, NULL, 10);
            if (config->pipeline_max >= 2)
                config->keep_alive = 1;
            break;
          case 'P':
            params.proxy_authorization = optarg;
            break;
          case 'T':
            params.body_content_type = optarg;
            break;
          case 'X':
            config->proxy = optarg;
            break;
          case 'b':
            config->so_bufsz = (int)strtoul(optarg, NULL, 10);
            break;
          case 'c':
            config->concur_count = (int)strtoul(optarg, NULL, 10);
            break;
          case 'i':
            config->http_head = 1;
            break;
          case 'k':
            config->keep_alive = 1;
            break;
          case 'm':
            params.method = optarg;
            config->http_head = (0 == strcasecmp(optarg, "HEAD"));
            break;
          case 'n':
            config->req_count = strtoull(optarg, NULL, 10);
            break;
          case 'p':
            params.body_filename = optarg;
            params.method = "POST";
            config->http_head = 0;
            break;
          case 'q':
            config->quiet = 1;
            break;
          case 'd':
          case 'l':
          case 'r':
            /*(ignored; compatibility with Apache Bench (ab))*/
            break;
          case 't':
            config->thread_count = (int)strtoul(optarg, NULL, 10);
            break;
          case 'u':
            params.body_filename = optarg;
            params.method = "PUT";
            config->http_head = 0;
            break;
          case ':':
            config_error("option requires an argument: -%c", optopt);
          case '?':
            if ('?' != optopt)
                config_error("unknown option: -%c", optopt);
            __attribute_fallthrough__
          case 'h':
            opt_show_help = 1;
            __attribute_fallthrough__
          case 'V':
            opt_show_version = 1;
            break;
        }
    }

    if (opt_show_version || !config->quiet)
        show_version();

    if (opt_show_help)
        show_help();

    if (opt_show_version)
        exit(0);

    if ((argc - optind) < 1)
        config_error("missing URI argument");
    else if ((argc - optind) > 1)
        config_error("too many arguments");
    params.uri = argv[optind];

    /* check for sane arguments */
    if (!config->req_count)
        config_error("num of requests has to be > 0");
    if (config->req_count == UINT64_MAX)
        config_error("invalid req_count");
    if (!config->thread_count)
        config_error("thread count has to be > 0");
    if ((uint64_t)config->thread_count > config->req_count)
        config_error("thread_count > req_count");
    if (!config->concur_count)
        config_error("num of concurrent clients has to be > 0");
    if ((uint64_t)config->concur_count > config->req_count)
        config_error("concur_count > req_count");
    if (config->thread_count > config->concur_count)
        config_error("thread_count > concur_count");
    if (config->pipeline_max < 1)
        config->pipeline_max = 1;
    if (NULL == params.method)
        params.method = config->http_head ? "HEAD" : "GET";

    config_request(config, &params);

    config->laddr.ai_addrlen = 0;
    config->laddrs.addrs = NULL;
    config->laddrs.num = 0;
    if (params.laddrstr && !config_laddrs(config, params.laddrstr))
        config_error("could not resolve local bind address: %s",
                     params.laddrstr);

    if (config->concur_count > 32768 && config->raddr.ai_family != AF_UNIX) {
        int need = config->concur_count;
        int avail = 32768;
        int fd = open("/proc/sys/net/ipv4/ip_local_port_range",
                      O_RDONLY|O_BINARY|O_LARGEFILE|O_NONBLOCK, 0);
        if (fd >= 0) {
            char buf[32];
            ssize_t rd = read(fd, buf, sizeof(buf));
            if (rd >= 3 && rd < (ssize_t)sizeof(buf)) {
                long lb, ub;
                char *e;
                buf[rd] = '\0';
                lb = strtoul(buf, &e, 10);
                if (lb > 0 && lb < USHRT_MAX && *e) {
                    ub = strtoul(e, &e, 10);
                    if (ub > 0 && ub <= USHRT_MAX && (*e=='\0' || *e=='\n')) {
                        if (lb <= ub)
                            avail = ub - lb + 1;
                    }
                }
            }
            close(fd);
        }
        if (config->laddrs.num)
            need = (need + config->laddrs.num - 1) / config->laddrs.num;
        if (need > avail)
            config_error("not enough local ports for concurrency\n"
                         "Reduce concur or provide -B addr,addr,addr "
                         "to specify multiple local bind addrs");
    }

    /* (see [RFC7413] 4.1.3. Client Cookie Handling) */
    if ((config->proxy && config->proxy[0] == '/')
        || config->request_size > (params.use_ipv6 ? 1440 : 1460))
        config->tcp_fastopen = 0;
}


__attribute_pure__
__attribute_nonnull__()
static int
sort_connect (const Times *a, const Times *b)
{
    return (int)((int64_t)a->connect - (int64_t)b->connect);
}


__attribute_pure__
__attribute_nonnull__()
static int
sort_ttfb (const Times *a, const Times *b)
{
    return (int)((int64_t)a->ttfb - (int64_t)b->ttfb);
}


__attribute_pure__
__attribute_nonnull__()
static int
sort_response (const Times *a, const Times *b)
{
    return (int)((int64_t)a->t - (int64_t)b->t);
}


__attribute_cold__
__attribute_noinline__
__attribute_nonnull__()
static void
weighttp_report (const Config * const restrict config)
{
    /* collect worker stats */
    Stats stats;
    memset(&stats, 0, sizeof(stats));
    for (int i = 0; i < config->thread_count; ++i) {
        const Stats * const restrict wstats = &config->wconfs[i].stats;

        stats.req_started   += wstats->req_started;
        stats.req_done      += wstats->req_done;
        stats.req_success   += wstats->req_success;
        stats.req_failed    += wstats->req_failed;
        stats.bytes_total   += wstats->bytes_total;
        stats.bytes_headers += wstats->bytes_headers;
        stats.req_2xx       += wstats->req_2xx;
        stats.req_3xx       += wstats->req_3xx;
        stats.req_4xx       += wstats->req_4xx;
        stats.req_5xx       += wstats->req_5xx;
    }

    if (0 == stats.req_done) {
        fprintf(stderr, "error: request_count 0\n");
        return;
    }

    /* collect worker times */
    Times * const times = calloc(stats.req_done, sizeof(Times));
    Times *wtimes = times;
    for (int i = 0; i < config->thread_count; ++i) {
        const Stats * const restrict wstats = &config->wconfs[i].stats;
        memcpy(wtimes, config->wconfs[i].wtimes, wstats->req_done*sizeof(Times));
        wtimes += wstats->req_done;
        free(config->wconfs[i].wtimes);
    }

    typedef struct TimingStats {
        uint64_t mean; /* mean */
        uint64_t t0;   /* min */
        uint64_t t50;  /* median */
        uint64_t t66;
        uint64_t t75;
        uint64_t t80;
        uint64_t t90;
        uint64_t t95;
        uint64_t t98;
        uint64_t t99;
        uint64_t t100; /* max */
        double stddev; /* standard deviation */
    } TimingStats;

    TimingStats tc, ttfb, tr;
    tc.mean = 0;
    ttfb.mean = 0;
    tr.mean = 0;
    tc.t0 = UINT64_MAX;
    ttfb.t0 = UINT64_MAX;
    tr.t0 = UINT64_MAX;
    tc.t100 = 0;
    ttfb.t100 = 0;
    tr.t100 = 0;
    tc.stddev = 0.0;
    ttfb.stddev = 0.0;
    tr.stddev = 0.0;

    qsort(times, stats.req_done, sizeof(Times),
          (int(*)(const void *, const void *))sort_connect);
    uint32_t connected = 0;
    for (uint64_t i = 0; i < stats.req_done; ++i) {
        Times *t = times+i;
        if (t->connect == INT32_MAX) break; /* keepalive; not connect */
        ++connected;
        if (t->connect < tc.t0)   tc.t0   = t->connect;
        if (t->connect > tc.t100) tc.t100 = t->connect;
        tc.mean += t->connect;
    }
    tc.mean /= connected;
    /* adjust median of num connected to 0-indexed array
     * (more precise index for small number of connect()) */
    tc.t50 = times[(connected / 2) - !(connected & 1)].connect;
    tc.t66 = times[(connected * 66 / 100) - !(connected & 1)].connect;
    tc.t75 = times[(connected * 75 / 100) - !(connected & 1)].connect;
    tc.t80 = times[(connected * 80 / 100) - !(connected & 1)].connect;
    tc.t90 = times[(connected * 90 / 100) - !(connected & 1)].connect;
    tc.t95 = times[(connected * 95 / 100) - !(connected & 1)].connect;
    tc.t98 = times[(connected * 98 / 100) - !(connected & 1)].connect;
    tc.t99 = times[(connected * 99 / 100) - !(connected & 1)].connect;
    for (uint32_t i = 0; i < connected; ++i) {
        Times *t = times+i;
        double d = (double)(int32_t)(t->connect - tc.mean);
        tc.stddev += d * d;           /* sum of squares */
    }
    if (connected > 1)
        tc.stddev /= (connected - 1); /* variance */
    tc.stddev = sqrt(tc.stddev);      /* standard deviation */

    qsort(times, stats.req_done, sizeof(Times),
          (int(*)(const void *, const void *))sort_ttfb);
    for (uint64_t i = 0; i < stats.req_done; ++i) {
        Times *t = times+i;
        if (t->ttfb < ttfb.t0)   ttfb.t0   = t->ttfb;
        if (t->ttfb > ttfb.t100) ttfb.t100 = t->ttfb;
        ttfb.mean += t->ttfb;
    }
    ttfb.mean /= stats.req_done;
    ttfb.t50 = times[(stats.req_done / 2) - !(stats.req_done & 1)].ttfb;
    ttfb.t66 = times[(stats.req_done * 66 / 100)].ttfb;
    ttfb.t75 = times[(stats.req_done * 75 / 100)].ttfb;
    ttfb.t80 = times[(stats.req_done * 80 / 100)].ttfb;
    ttfb.t90 = times[(stats.req_done * 90 / 100)].ttfb;
    ttfb.t95 = times[(stats.req_done * 95 / 100)].ttfb;
    ttfb.t98 = times[(stats.req_done * 98 / 100)].ttfb;
    ttfb.t99 = times[(stats.req_done * 99 / 100)].ttfb;
    for (uint64_t i = 0; i < stats.req_done; ++i) {
        Times *t = times+i;
        double d = (double)(int32_t)(t->ttfb - ttfb.mean);
        ttfb.stddev += d * d;                /* sum of squares */
    }
    if (stats.req_done > 1)
        ttfb.stddev /= (stats.req_done - 1); /* variance */
    ttfb.stddev = sqrt(ttfb.stddev);         /* standard deviation */

    qsort(times, stats.req_done, sizeof(Times),
          (int(*)(const void *, const void *))sort_response);
    for (uint64_t i = 0; i < stats.req_done; ++i) {
        Times *t = times+i;
        if (t->t < tr.t0)   tr.t0   = t->t;
        if (t->t > tr.t100) tr.t100 = t->t;
        tr.mean += t->t;
    }
    tr.mean /= stats.req_done;
    tr.t50 = times[(stats.req_done / 2) - !(stats.req_done & 1)].t;
    tr.t66 = times[(stats.req_done * 66 / 100)].t;
    tr.t75 = times[(stats.req_done * 75 / 100)].t;
    tr.t80 = times[(stats.req_done * 80 / 100)].t;
    tr.t90 = times[(stats.req_done * 90 / 100)].t;
    tr.t95 = times[(stats.req_done * 95 / 100)].t;
    tr.t98 = times[(stats.req_done * 98 / 100)].t;
    tr.t99 = times[(stats.req_done * 99 / 100)].t;
    for (uint64_t i = 0; i < stats.req_done; ++i) {
        Times *t = times+i;
        double d = (double)(int64_t)(t->t - tr.mean);
        tr.stddev += d * d;                /* sum of squares */
    }
    if (stats.req_done > 1)
        tr.stddev /= (stats.req_done - 1); /* variance */
    tr.stddev = sqrt(tr.stddev);           /* standard deviation */

  #if 0 /*(might be useful to combine for tests without using keep-alive)*/
    for (uint64_t i = 0; i < stats.req_done; ++i)
        if (times[i].connect != INT32_MAX) times[i].t += times[i].connect;
    TimingStats tot;
    tot.mean = 0;
    tot.t0 = UINT64_MAX;
    tot.t100 = 0;
    tot.stddev = 0.0;
    qsort(times, stats.req_done, sizeof(Times),
          (int(*)(const void *, const void *))sort_response);
    for (uint64_t i = 0; i < stats.req_done; ++i) {
        Times *t = times+i;
        if (t->t < tot.t0)   tot.t0   = t->t;
        if (t->t > tot.t100) tot.t100 = t->t;
        tot.mean += t->t;
    }
    tot.mean /= stats.req_done;
    tot.t50 = times[(stats.req_done / 2) - !(stats.req_done & 1)].t;
    tot.t66 = times[(stats.req_done * 66 / 100)].t;
    tot.t75 = times[(stats.req_done * 75 / 100)].t;
    tot.t80 = times[(stats.req_done * 80 / 100)].t;
    tot.t90 = times[(stats.req_done * 90 / 100)].t;
    tot.t95 = times[(stats.req_done * 95 / 100)].t;
    tot.t98 = times[(stats.req_done * 98 / 100)].t;
    tot.t99 = times[(stats.req_done * 99 / 100)].t;
    for (uint64_t i = 0; i < stats.req_done; ++i) {
        Times *t = times+i;
        double d = (double)(int64_t)(t->t - tot.mean);
        tot.stddev += d * d;                /* sum of squares */
    }
    if (stats.req_done > 1)
        tot.stddev /= (stats.req_done - 1); /* variance */
    tot.stddev = sqrt(tot.stddev);          /* standard deviation */
  #endif

    free(times);

    /* report cumulative stats */
    struct timeval tdiff;
    tdiff.tv_sec  = config->ts_end.tv_sec  - config->ts_start.tv_sec;
    tdiff.tv_usec = config->ts_end.tv_usec - config->ts_start.tv_usec;
    if (tdiff.tv_usec < 0) {
        --tdiff.tv_sec;
        tdiff.tv_usec += 1000000;
    }
    const uint64_t total_usecs = tdiff.tv_sec * 1000000 + tdiff.tv_usec;
    const uint64_t rps = stats.req_done * 1000000 / total_usecs;
    const uint64_t kbps= stats.bytes_total / 1024 * 1000000 / total_usecs;
    /*const uint64_t time_per_req = total_usecs / stats.req_done;*/
    /* note: not currently tracking header/body bytes sent,
     * and would have to track pipelined requests which were retried */
  #if 1  /* JSON-style formatted output */
    printf("{\n"
           "  \"reqs_per_sec\": %"PRIu64",\n"
           "  \"kBps_per_sec\": %"PRIu64",\n"
           "  \"secs_elapsed\": %01d.%06ld,\n",
           rps, kbps, (int)tdiff.tv_sec, (long)tdiff.tv_usec);
    printf("  \"request_counts\": {\n"
           "    \"started\":    %"PRIu64",\n"
           "    \"retired\":    %"PRIu64",\n"
           "    \"total\":      %"PRIu64",\n"
           "    \"keep-alive\": %"PRIu64"\n"
           "  },\n",
           stats.req_started, stats.req_done, config->req_count,
           config->req_count - connected);
    printf("  \"response_counts\": {\n"
           "    \"pass\": %"PRIu64",\n"
           "    \"fail\": %"PRIu64",\n"
           "    \"errs\": %"PRIu64"\n"
           "  },\n",
           stats.req_success, stats.req_failed, stats.req_error);
    printf("  \"status_codes\": {\n"
           "    \"2xx\":  %"PRIu64",\n"
           "    \"3xx\":  %"PRIu64",\n"
           "    \"4xx\":  %"PRIu64",\n"
           "    \"5xx\":  %"PRIu64"\n"
           "  },\n",
           stats.req_2xx, stats.req_3xx, stats.req_4xx, stats.req_5xx);
    printf("  \"traffic\": {\n"
           "    \"bytes_total\":   %12.1"PRIu64",\n"
           "    \"bytes_headers\": %12.1"PRIu64",\n"
           "    \"bytes_body\":    %12.1"PRIu64"\n"
           "  },\n",
           stats.bytes_total, stats.bytes_headers,
           stats.bytes_total - stats.bytes_headers);
    printf("  \"connect_times\": {\n"
           "     \"num\": %9.1"PRIu32",\n"
           "     \"avg\": %9.1"PRIu64",\n"
           "     \"stddev\": %6.0f,\n"
           "     \"unit\": \"us\",\n"
           "      \"0%%\": %9.1"PRIu64",\n"
           "     \"50%%\": %9.1"PRIu64",\n"
           "     \"66%%\": %9.1"PRIu64",\n"
           "     \"75%%\": %9.1"PRIu64",\n"
           "     \"80%%\": %9.1"PRIu64",\n"
           "     \"90%%\": %9.1"PRIu64",\n"
           "     \"95%%\": %9.1"PRIu64",\n"
           "     \"98%%\": %9.1"PRIu64",\n"
           "     \"99%%\": %9.1"PRIu64",\n"
           "    \"100%%\": %9.1"PRIu64"\n"
           "  },\n",
           connected, tc.mean, tc.stddev,
           tc.t0,  tc.t50, tc.t66, tc.t75, tc.t80,
           tc.t90, tc.t95, tc.t98, tc.t99, tc.t100);
    printf("  \"time_to_first_byte\": {\n"
           "     \"num\": %9.1"PRIu64",\n"
           "     \"avg\": %9.1"PRIu64",\n"
           "     \"stddev\": %6.0f,\n"
           "     \"unit\": \"us\",\n"
           "      \"0%%\": %9.1"PRIu64",\n"
           "     \"50%%\": %9.1"PRIu64",\n"
           "     \"66%%\": %9.1"PRIu64",\n"
           "     \"75%%\": %9.1"PRIu64",\n"
           "     \"80%%\": %9.1"PRIu64",\n"
           "     \"90%%\": %9.1"PRIu64",\n"
           "     \"95%%\": %9.1"PRIu64",\n"
           "     \"98%%\": %9.1"PRIu64",\n"
           "     \"99%%\": %9.1"PRIu64",\n"
           "    \"100%%\": %9.1"PRIu64"\n"
           "  },\n",
           stats.req_done, ttfb.mean, ttfb.stddev,
           ttfb.t0,  ttfb.t50, ttfb.t66, ttfb.t75, ttfb.t80,
           ttfb.t90, ttfb.t95, ttfb.t98, ttfb.t99, ttfb.t100);
    printf("  \"response_times\": {\n"
           "     \"num\": %9.1"PRIu64",\n"
           "     \"avg\": %9.1"PRIu64",\n"
           "     \"stddev\": %6.0f,\n"
           "     \"unit\": \"us\",\n"
           "      \"0%%\": %9.1"PRIu64",\n"
           "     \"50%%\": %9.1"PRIu64",\n"
           "     \"66%%\": %9.1"PRIu64",\n"
           "     \"75%%\": %9.1"PRIu64",\n"
           "     \"80%%\": %9.1"PRIu64",\n"
           "     \"90%%\": %9.1"PRIu64",\n"
           "     \"95%%\": %9.1"PRIu64",\n"
           "     \"98%%\": %9.1"PRIu64",\n"
           "     \"99%%\": %9.1"PRIu64",\n"
           "    \"100%%\": %9.1"PRIu64"\n"
           "  }\n"
           "}\n",
           stats.req_done, tr.mean, tr.stddev,
           tr.t0,  tr.t50, tr.t66, tr.t75, tr.t80,
           tr.t90, tr.t95, tr.t98, tr.t99, tr.t100);
   #if 0 /*(might be useful for tests without using keep-alive)*/
         /*(note: if enabled, must adjust JSON in last two printf lines above)*/
    printf("  \"total_times\": {\n"
           "     \"num\": %9.1"PRIu64",\n"
           "     \"avg\": %9.1"PRIu64",\n"
           "     \"stddev\": %6.0f,\n"
           "     \"unit\": \"us\",\n"
           "      \"0%%\": %9.1"PRIu64",\n"
           "     \"50%%\": %9.1"PRIu64",\n"
           "     \"66%%\": %9.1"PRIu64",\n"
           "     \"75%%\": %9.1"PRIu64",\n"
           "     \"80%%\": %9.1"PRIu64",\n"
           "     \"90%%\": %9.1"PRIu64",\n"
           "     \"95%%\": %9.1"PRIu64",\n"
           "     \"98%%\": %9.1"PRIu64",\n"
           "     \"99%%\": %9.1"PRIu64",\n"
           "    \"100%%\": %9.1"PRIu64"\n"
           "  }\n"
           "}\n",
           stats.req_done, tot.mean, tot.stddev,
           tot.t0,  tot.t50, tot.t66, tot.t75, tot.t80,
           tot.t90, tot.t95, tot.t98, tot.t99, tot.t100);
   #endif
  #else
    printf("\nfinished in %01d.%06ld sec, %"PRIu64" req/s, %"PRIu64" kbyte/s\n",
           (int)tdiff.tv_sec, (long)tdiff.tv_usec, rps, kbps);

    printf("requests:  %"PRIu64" started, %"PRIu64" done, %"PRIu64" total\n"
           "responses: %"PRIu64" success, %"PRIu64" fail, %"PRIu64" error\n",
           stats.req_started, stats.req_done, config->req_count,
           stats.req_success, stats.req_failed, stats.req_error);

    printf("status codes: "
           "%"PRIu64" 2xx, %"PRIu64" 3xx, %"PRIu64" 4xx, %"PRIu64" 5xx\n",
           stats.req_2xx, stats.req_3xx, stats.req_4xx, stats.req_5xx);

    printf("traffic: %"PRIu64" bytes total, %"PRIu64" bytes headers, "
           "%"PRIu64" bytes body\n", stats.bytes_total,
           stats.bytes_headers, stats.bytes_total - stats.bytes_headers);
  #endif
}


int main (int argc, char *argv[])
{
    Config config;
    weighttp_setup(&config, argc, argv); /* exits if error (or done) */
    wconfs_init(&config);
  #if defined(__STDC_VERSION__) && __STDC_VERSION__-0 >= 199901L /* C99 */
    pthread_t threads[config.thread_count]; /*(C99 dynamic array)*/
  #else
    pthread_t * const restrict threads =
      (pthread_t *)calloc(config.thread_count, sizeof(pthread_t));
  #endif

    if (!config.quiet)
        puts("starting benchmark...");
    gettimeofday(&config.ts_start, NULL);

    for (int i = 0; i < config.thread_count; ++i) {
        int err = pthread_create(threads+i, NULL,
                                 worker_thread, (void*)(config.wconfs+i));
        if (__builtin_expect( (0 != err), 0)) {
            fprintf(stderr, "error: failed spawning thread (%d)\n", err);
            /*(XXX: leaks resources and does not attempt pthread_cancel())*/
            return 2; /* (unexpected) fatal error */
        }
    }

    for (int i = 0; i < config.thread_count; ++i) {
        int err = pthread_join(threads[i], NULL);
        if (__builtin_expect( (0 != err), 0)) {
            fprintf(stderr, "error: failed joining thread (%d)\n", err);
            /*(XXX: leaks resources and does not attempt pthread_cancel())*/
            return 3; /* (unexpected) fatal error */
        }
    }

    gettimeofday(&config.ts_end, NULL);

  #if !(defined(__STDC_VERSION__) && __STDC_VERSION__-0 >= 199901L) /* !C99 */
    free(threads);
  #endif
    weighttp_report(&config);
    wconfs_delete(&config);

    return 0;
}
