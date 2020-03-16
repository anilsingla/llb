/* BSD 2-Clause License
 *
 * Copyright (c) 2019, Andrea Giacomo Baldan All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include "ev.h"
#include "log.h"
#include "config.h"
#include "server.h"
#include "network.h"
#include "memorypool.h"
#include "npt_internal.h"

pthread_mutex_t mutex;

/*
 * Auxiliary structure to be used as init argument for eventloop, fd is the
 * listening socket we want to share between multiple instances, cronjobs is
 * just a flag to signal if we want to register cronjobs on that particular
 * instance or not (to not repeat useless cron jobs on multiple threads)
 */
struct listen_payload {
    int fd;
    bool cronjobs;
};

/* Broker global instance, contains the topic trie and the clients hashtable */
struct server server;

/*
 * TCP server, based on I/O multiplexing abstraction called ev_ctx. Each thread
 * (if any) should have his own ev_ctx and thus being responsible of a subset
 * of clients.
 * At the init of the server, the ev_ctx will be instructed to run some
 * periodic tasks and to run a callback on accept on new connections. From now
 * on start a simple juggling of callbacks to be scheduled on the event loop,
 * typically after being accepted a connection his handle (fd) will be added to
 * the backend of the loop (this case we're using EPOLL as a backend but also
 * KQUEUE or SELECT/POLL should be easy to plug-in) and read_callback will be
 * run every time there's new data incoming. If a complete packet is received
 * and correctly parsed it will be processed by calling the right handler from
 * the handler module, based on the command it carries and a response will be
 * fired back.
 *
 *                             MAIN THREAD
 *                              [EV_CTX]
 *
 *    ACCEPT_CALLBACK         READ_CALLBACK         WRITE_CALLBACK
 *  -------------------    ------------------    --------------------
 *        |                        |                       |
 *      ACCEPT                     |                       |
 *        | ---------------------> |                       |
 *        |                  READ AND DECODE               |
 *        |                        |                       |
 *        |                        |                       |
 *        |                     PROCESS                    |
 *        |                        |                       |
 *        |                        |                       |
 *        |                        | --------------------> |
 *        |                        |                     WRITE
 *      ACCEPT                     |                       |
 *        | ---------------------> | <-------------------- |
 *        |                        |                       |
 *
 * Right now we're using a single thread, but the whole method could be easily
 * distributed across a threadpool, by paying attention to the shared critical
 * parts on handler module.
 * The access to shared data strucures on the worker thread pool could be
 * guarded by a spinlock, and being generally fast operations it shouldn't
 * suffer high contentions by the threads and thus being really fast.
 */

static void http_transaction_init(struct http_transaction *);

static void http_transaction_deactivate(struct http_transaction *);

// CALLBACKS for the eventloop
static void accept_callback(struct ev_ctx *, void *);

static void read_callback(struct ev_ctx *, void *);

static void write_callback(struct ev_ctx *, void *);

/*
 * Processing message function, will be applied on fully formed mqtt packet
 * received on read_callback callback
 */
static void process_request(struct ev_ctx *, struct http_transaction *);

static void process_response(struct ev_ctx *, struct http_transaction *);

static inline void http_parse_header(struct http_transaction *);

#define CHUNKED_COMPLETE(http) \
    strcmp((char *) (http)->stream.buf + (http)->stream.size - 5, "0\r\n\r\n") == 0

#define PROCESS_STREAM(http) do {                    \
    if ((http)->status == WAITING_REQUEST) {         \
        process_request((http)->ctx, (http));        \
    } else if ((http)->status == WAITING_RESPONSE) { \
        process_response((http)->ctx, (http));       \
    }                                                \
} while (0);

// XXX Eyesore
static inline void http_parse_header(struct http_transaction *http) {
    const char *encoding =
        strstr((const char *) http->stream.buf, "Transfer-Encoding");
    if (encoding) {
        if (strstr((const char *) http->stream.buf, "chunked"))
            http->encoding = CHUNKED;
        else
            http->encoding = GENERIC;
    }
}

/* Simple error_code to string function, to be refined */
//static const char *npterr(int rc) {
//    switch (rc) {
//        case -ERRCLIENTDC:
//            return "Client disconnected";
//        case -ERRSOCKETERR:
//            return strerror(errno);
//        case -ERRPACKETERR:
//            return "Error reading packet";
//        case -ERRMAXREQSIZE:
//            return "Packet sent exceeds max size accepted";
//        case -ERREAGAIN:
//            return "Socket FD EAGAIN";
//        default:
//            return "Unknown error";
//    }
//}

/*
 * ====================================================
 *  Cron tasks, to be repeated at fixed time intervals
 * ====================================================
 */

/*
 * ======================================================
 *  Private functions and callbacks for server behaviour
 * ======================================================
 */

/*
 * All transactions are pre-allocated at the start of the server, but their buffers
 * (read and write) are not, they're lazily allocated with this function, meant
 * to be called on the accept callback
 */
static void http_transaction_init(struct http_transaction *http) {
    http->status = WAITING_REQUEST;
    http->encoding = UNSET;
    http->stream.size = 0;
    http->stream.capacity = 2048;
    if (!http->stream.buf)
        http->stream.buf = npt_calloc(2048, sizeof(unsigned char));
    pthread_mutex_init(&http->mutex, NULL);
}

/*
 * As we really don't want to completely de-allocate a client in favor of
 * making it reusable by another connection we simply deactivate it according
 * to its state (e.g. if it's a clean_session connected client or not) and we
 * allow the clients memory pool to reclaim it
 */
static void http_transaction_deactivate(struct http_transaction *http) {

#if THREADSNR > 0
    pthread_mutex_lock(&http->mutex);
#endif

    log_debug("Deactivate");
    http->stream.size = 0;
    http->encoding = UNSET;
    http->status = WAITING_REQUEST;
    memset(http->stream.buf, 0x00, http->stream.capacity);
    //close_connection(&http->conn);
    //HASH_DEL(server.clients, client);
    memorypool_free(server.pool, http);

#if THREADSNR > 0
    pthread_mutex_unlock(&http->mutex);
    pthread_mutex_destroy(&http->mutex);
#endif
}

/*
 * Parse packet header, it is required at least the Fixed Header of each
 * packed, which is contained in the first 2 bytes in order to read packet
 * type and total length that we need to recv to complete the packet.
 *
 * This function accept a socket fd, a buffer to read incoming streams of
 * bytes and a pointer to the decoded fixed header that will be set in the
 * final parsed packet.
 *
 * - c: A struct client pointer, contains the FD of the requesting client
 *      as well as his SSL context in case of TLS communication. Also it store
 *      the reading buffer to be used for incoming byte-streams, tracking
 *      read, to be read and reading position taking into account the bytes
 *      required to encode the packet length.
 */
static inline int http_transaction_read(struct http_transaction *http) {

    ssize_t nread = 0;

    /*
     * Last status, we have access to the length of the packet and we know for
     * sure that it's not a PINGREQ/PINGRESP/DISCONNECT packet.
     */
    if (http->status == WAITING_REQUEST)
        nread = recv_data(&http->pipe[CLIENT], &http->stream);
    else if (http->status == WAITING_RESPONSE)
        nread = recv_data(&http->pipe[BACKEND], &http->stream);

    if (errno != EAGAIN && errno != EWOULDBLOCK && nread <= 0)
        return nread == -1 ? -ERRSOCKETERR : -ERRCLIENTDC;

    if ((errno == EAGAIN || errno == EWOULDBLOCK))
        return -ERREAGAIN;

    return NPT_SUCCESS;
}

/*
 * Write stream of bytes to a client represented by a connection object, till
 * all bytes to be written is exhausted, tracked by towrite field or if an
 * EAGAIN (socket descriptor must be in non-blocking mode) error is raised,
 * meaning we cannot write anymore for the current cycle.
 */
static inline int http_transaction_write(struct http_transaction *http) {
    ssize_t wrote = 0;
#if THREADSNR > 0
    pthread_mutex_lock(&http->mutex);
#endif
    log_debug("Forwarding %d (%ld bytes)", http->status, http->stream.size);
    if (http->status == FORWARDING_REQUEST)
        wrote = send_data(&http->pipe[BACKEND], &http->stream);
    else if (http->status == FORWARDING_RESPONSE)
        wrote = send_data(&http->pipe[CLIENT], &http->stream);
    if (errno != EAGAIN && errno != EWOULDBLOCK && wrote < 0)
        goto clientdc;
#if THREADSNR > 0
    pthread_mutex_unlock(&http->mutex);
#endif
    return NPT_SUCCESS;

clientdc:
#if THREADSNR > 0
    pthread_mutex_unlock(&http->mutex);
#endif
    return -ERRSOCKETERR;

//eagain:
#if THREADSNR > 0
    pthread_mutex_unlock(&http->mutex);
#endif
    return -ERREAGAIN;
}

/*
 * ===========
 *  Callbacks
 * ===========
 */

/*
 * Callback dedicated to client replies, try to send as much data as possible
 * epmtying the client buffer and rearming the socket descriptor for reading
 * after
 */
static void write_callback(struct ev_ctx *ctx, void *arg) {
    struct http_transaction *http = arg;
    printf("Write CB: %s %ld\n", http->stream.buf, http->stream.size);
    int err = http_transaction_write(http);
    switch (err) {
        case NPT_SUCCESS: // OK
            /*
             * Rearm descriptor making it ready to receive input,
             * read_callback will be the callback to be used; also reset the
             * read buffer status for the client.
             */
            if (http->status == FORWARDING_REQUEST) {
                log_debug(">>> EV_READ");
                http->status = WAITING_RESPONSE;
                http->stream.size = 0;
                ev_fire_event(ctx, http->pipe[BACKEND].fd,
                              EV_READ, read_callback, http);
            } else if (http->status == FORWARDING_RESPONSE) {
                close_connection(&http->pipe[CLIENT]);
                close_connection(&http->pipe[BACKEND]);
                http_transaction_deactivate(http);
            }
            break;
        case -ERREAGAIN:
            enqueue_event_write(http);
            break;
        default:
            //log_info("Closing connection with %s: %s %i",
            //         client->conn.ip, npterr(client->rc), err);
            ev_del_fd(ctx, http->pipe[CLIENT].fd);
            http_transaction_deactivate(http);
            break;
    }
}

/*
 * Handle incoming connections, create a a fresh new struct client structure
 * and link it to the fd, ready to be set in EV_READ event, then schedule a
 * call to the read_callback to handle incoming streams of bytes
 */
static void accept_callback(struct ev_ctx *ctx, void *data) {
    int serverfd = *((int *) data);
    while (1) {

        /*
         * Accept a new incoming connection assigning ip address
         * and socket descriptor to the connection structure
         * pointer passed as argument
         */
        struct connection conn;
        connection_init(&conn, conf->tls ? server.ssl_ctx : NULL);
        int fd = accept_connection(&conn, serverfd);
        if (fd == 0)
            continue;
        if (fd < 0) {
            close_connection(&conn);
            break;
        }

        /*
         * Create a client structure to handle his context
         * connection
         */
#if THREADSNR > 0
        pthread_mutex_lock(&mutex);
#endif
        struct http_transaction *http = memorypool_alloc(server.pool);
#if THREADSNR > 0
        pthread_mutex_unlock(&mutex);
#endif
        http->pipe[CLIENT] = conn;
        http_transaction_init(http);
        http->ctx = ctx;

        /* Add it to the epoll loop */
        ev_register_event(ctx, fd, EV_READ, read_callback, http);

        log_info("[%p] Connection from %s", (void *) pthread_self(), conn.ip);
    }
}

/*
 * Reading packet callback, it's the main function that will be called every
 * time a connected client has some data to be read, notified by the eventloop
 * context.
 */
static void read_callback(struct ev_ctx *ctx, void *data) {
    struct http_transaction *http = data;
    /*
     * Received a bunch of data from a client, after the creation
     * of an IO event we need to read the bytes and encoding the
     * content according to the protocol
     */
    int rc = http_transaction_read(http);
    log_debug("Read %ld", http->stream.size);
    switch (rc) {
        case NPT_SUCCESS:
            /*
             * All is ok, raise an event to the worker poll EPOLL and
             * link it with the IO event containing the decode payload
             * ready to be processed
             */
            PROCESS_STREAM(http);
            break;
        case -ERRCLIENTDC:
        case -ERRSOCKETERR:
        case -ERRPACKETERR:
        case -ERRMAXREQSIZE:
            /*
             * We got an unexpected error or a disconnection from the
             * client side, remove client from the global map and
             * free resources allocated such as io_event structure and
             * paired payload
             */
            // TODO
//            log_error("Closing connection with %s (%s): %s",
//                      c->client_id, c->conn.ip, npterr(rc));
#if THREADSNR > 0
            pthread_mutex_lock(&mutex);
#endif
            // Clean resources
            ev_del_fd(ctx, http->pipe[CLIENT].fd);

#if THREADSNR > 0
            pthread_mutex_unlock(&mutex);
#endif
            http_transaction_deactivate(http);
            break;
        case -ERREAGAIN:
            if (http->encoding == UNSET)
                http_parse_header(http);
            if (http->encoding != CHUNKED) {
                log_debug("Not chunked");
                PROCESS_STREAM(http);
            } else {
                log_debug("EAGAIN, re-read %s", http->stream.buf);
                log_debug("Last char %c at %li", http->stream.buf[http->stream.size - 5], http->stream.size);
                if (CHUNKED_COMPLETE(http)) {
                    log_debug("Complete");
                    PROCESS_STREAM(http)
                } else {
                    if (http->status == WAITING_RESPONSE)
                        ev_fire_event(ctx, http->pipe[BACKEND].fd,
                                      EV_READ, read_callback, http);
                    else if (http->status == WAITING_REQUEST)
                        ev_fire_event(ctx, http->pipe[CLIENT].fd,
                                      EV_READ, read_callback, http);
                }
            }
            break;
    }
}

/*
 * This function is called only if the client has sent a full stream of bytes
 * consisting of a complete packet as expected by the MQTT protocol and by the
 * declared length of the packet.
 * It uses eventloop APIs to react accordingly to the packet type received,
 * validating it before proceed to call handlers. Depending on the handler
 * called and its outcome, it'll enqueue an event to write a reply or just
 * reset the client state to allow reading some more packets.
 */
static void process_request(struct ev_ctx *ctx, struct http_transaction *http) {
    log_debug("Processing %ld %s", http->stream.size, http->stream.buf);
    if (server.current_backend == 8)
        server.current_backend = 0;
    struct backend *backend = &server.backends[server.current_backend++];
    log_debug("Connecting to %s:%d", backend->host, backend->port);
    struct connection conn;
    connection_init(&conn, conf->tls ? server.ssl_ctx : NULL);
    int fd = open_connection(&conn, backend->host, backend->port);
    if (fd == 0)
        return;
    if (fd < 0) {
        close_connection(&conn);
        return;
    }

    /*
     * Create a client structure to handle his context
     * connection
     */
#if THREADSNR > 0
    pthread_mutex_lock(&mutex);
#endif
    http->pipe[BACKEND] = conn;
#if THREADSNR > 0
    pthread_mutex_unlock(&mutex);
#endif
    char *ptr = strstr((const char *) http->stream.buf, "8789");
    memcpy(ptr, "6090", 4);
    http->status = FORWARDING_REQUEST;
    log_debug("Payload: %s %ld", http->stream.buf, http->stream.size);

    /* Add it to the epoll loop */
    ev_register_event(ctx, fd, EV_WRITE, write_callback, http);
    //enqueue_event_write(http);

    //ev_fire_event(http->ctx, http->backend.fd, EV_WRITE, write_callback, http);
}

static void process_response(struct ev_ctx *ctx, struct http_transaction *http) {
    log_debug("Forwarding response");
    http->status = FORWARDING_RESPONSE;
    enqueue_event_write(http);
}

/*
 * Eventloop stop callback, will be triggered by an EV_CLOSEFD event and stop
 * the running loop, unblocking the call.
 */
static void stop_handler(struct ev_ctx *ctx, void *arg) {
    (void) arg;
    ev_stop(ctx);
}

/*
 * IO worker function, wait for events on a dedicated epoll descriptor which
 * is shared among multiple threads for input and output only, following the
 * normal EPOLL semantic, EPOLLIN for incoming bytes to be unpacked and
 * processed by a worker thread, EPOLLOUT for bytes incoming from a worker
 * thread, ready to be delivered out.
 */
static void eventloop_start(void *args) {
    struct listen_payload *loop_data = args;
    struct ev_ctx ctx;
    int sfd = loop_data->fd;
    ev_init(&ctx, EVENTLOOP_MAX_EVENTS);
    // Register stop event
    ev_register_event(&ctx, conf->run, EV_CLOSEFD|EV_READ, stop_handler, NULL);
    // Register listening FD with accept callback
    ev_register_event(&ctx, sfd, EV_READ, accept_callback, &sfd);
    // Register periodic tasks
    //if (loop_data->cronjobs == true) {
    //    ev_register_cron(&ctx, publish_stats, NULL, conf->stats_pub_interval, 0);
    //    ev_register_cron(&ctx, inflight_msg_check, NULL, 1, 0);
    //    ev_register_cron(&ctx, persist_session, NULL, 1, 0);
    //}
    // Start the loop, blocking call
    ev_run(&ctx);
    ev_destroy(&ctx);
}

/*
 * ===================
 *  Main APIs exposed
 * ===================
 */

/* Fire a write callback to reply after a client request */
void enqueue_event_write(const struct http_transaction *http) {
    if (http->status == FORWARDING_REQUEST)
        ev_fire_event(http->ctx, http->pipe[BACKEND].fd,
                      EV_WRITE, write_callback, (void *) http);
    else if (http->status == FORWARDING_RESPONSE)
        ev_fire_event(http->ctx, http->pipe[CLIENT].fd,
                      EV_WRITE, write_callback, (void *) http);
}

/*
 * Main entry point for the server, to be called with an address and a port
 * to start listening
 */
int start_server(const char *addr, const char *port) {

    /* Initialize global Npt instance */
    server.current_backend = 0;
    server.pool =
        memorypool_new(BASE_CLIENTS_NUM, sizeof(struct http_transaction));
    server.clients = NULL;
    server.backends = npt_calloc(8, sizeof(struct backend));
    for (int i = 0; i < 8; ++i) {
        strcpy(server.backends[i].host, "127.0.0.1");
        server.backends[i].port = 6090;
        server.backends[i].alive = true;
    }
    printf("%s\n", server.backends[0].host);
    pthread_mutex_init(&mutex, NULL);

    /* Start listening for new connections */
    int sfd = make_listen(addr, port);

    /* Setup SSL in case of flag true */
    if (conf->tls == true) {
        openssl_init();
        server.ssl_ctx = create_ssl_context();
        load_certificates(server.ssl_ctx, conf->cafile,
                          conf->certfile, conf->keyfile);
    }

    log_info("Server start");

    struct listen_payload loop_start = { sfd, false };

#if THREADSNR > 0
    pthread_t thrs[THREADSNR];
    for (int i = 0; i < THREADSNR; ++i) {
        pthread_create(&thrs[i], NULL, (void * (*) (void *)) &eventloop_start, &loop_start);
        usleep(1500);
    }
#endif
    loop_start.cronjobs = true;
    // start eventloop, could be spread on multiple threads
    eventloop_start(&loop_start);

#if THREADSNR > 0
    for (int i = 0; i < THREADSNR; ++i)
        pthread_join(thrs[i], NULL);
#endif

    close(sfd);

    /* Destroy SSL context, if any present */
    if (conf->tls == true) {
        SSL_CTX_free(server.ssl_ctx);
        openssl_cleanup();
    }
    pthread_mutex_destroy(&mutex);
    npt_free(server.backends);

    log_info("Npt v%s exiting", VERSION);

    return NPT_SUCCESS;
}

/*
 * Make the entire process a daemon
 */
void daemonize(void) {

    int fd;

    if (fork() != 0)
        exit(0);

    setsid();

    if ((fd = open("/dev/null", O_RDWR, 0)) != -1) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO) close(fd);
    }
}
