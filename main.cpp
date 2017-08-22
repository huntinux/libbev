/*
 * main.cpp
 *
 *  Created on: Aug 21, 2017
 *      Author: jinger
 */

#include <list>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>
#include <malloc.h>
#include <unistd.h>
#include <event2/event.h>
#include <event2/event_struct.h>
#include <netinet/tcp.h>

#include "thread.h"



/* An item in the connection queue. */
enum conn_queue_item_modes {
    queue_new_conn,   /* brand new connection. */
    queue_redispatch, /* redispatching from side thread */
};

enum network_transport {
    local_transport, /* Unix sockets*/
    tcp_transport,
    udp_transport
};

/**
 * Possible states of a connection.
 */
enum conn_states {
    conn_listening,  /**< the socket which listens for connections */
    conn_new_cmd,    /**< Prepare connection for next command */
    conn_waiting,    /**< waiting for a readable socket */
    conn_read,       /**< reading in a command line */
    conn_parse_cmd,  /**< try to parse a command from the input buffer */
    conn_write,      /**< writing out a simple response */
    conn_nread,      /**< reading in a fixed number of bytes */
    conn_swallow,    /**< swallowing unnecessary bytes w/o storing */
    conn_closing,    /**< closing this connection */
    conn_mwrite,     /**< writing out many items sequentially */
    conn_closed,     /**< connection is closed */
    conn_watch,      /**< held by the logger thread as a watcher */
    conn_max_state   /**< Max state value (used for assertion) */
};

/**
 * The structure representing a connection into memcached.
 */
typedef struct conn conn;
struct conn {
    int    sfd;
    enum conn_states  state;
    struct event event;
    short  ev_flags;
    short  which;   /** which events were just triggered */
    char recvbuffer[1024];
    char sendbuffer[1024];
    conn   *next;     /* Used for generating a list of conn structures */
    LIBEVENT_THREAD *thread; /* Pointer to the thread object serving this connection */
};

typedef struct conn_queue_item CQ_ITEM;
struct conn_queue_item {
    int               sfd;
    enum conn_states  init_state;
    int               event_flags;
    int               read_buffer_size;
    enum conn_queue_item_modes mode;
    conn *c;
};

struct conn_queue {
    std::list<conn_queue_item> q;
    pthread_mutex_t lock;
};

/*
 * Number of worker threads that have finished setting themselves up.
 */
const int kWorkerThreadNum = 10;
static int init_count = 0;
static pthread_mutex_t init_lock;
static pthread_cond_t init_cond;

LIBEVENT_THREAD *threads;



conn *conn_new(const int sfd, enum conn_states init_state,
                const int event_flags,
                const int read_buffer_size, enum network_transport transport,
                struct event_base *base);
/*
 * Processes an incoming "handle a new connection" item. This is called when
 * input arrives on the libevent wakeup pipe.
 */
static void thread_libevent_process(int fd, short which, void *arg) {
	printf("main thread choose me\n");
    LIBEVENT_THREAD *me = (LIBEVENT_THREAD*)arg;
    CQ_ITEM item;
    char buf[1];
    conn *c;

    if (read(fd, buf, 1) != 1) {
        fprintf(stderr, "Can't read from libevent pipe\n");
        return;
    }

    switch (buf[0]) {
    case 'c':
        pthread_mutex_lock(&me->new_conn_queue->lock);
        item = me->new_conn_queue->q.front();
        me->new_conn_queue->q.pop_front();
        pthread_mutex_unlock(&me->new_conn_queue->lock);

        switch (item.mode) {
            case queue_new_conn:
                c = conn_new(item.sfd, item.init_state, item.event_flags, 1024, tcp_transport, me->base);
                if (c == NULL) {
                    fprintf(stderr, "Can't listen for events on fd %d\n", item.sfd);
                    close(item.sfd);
                } else {
                    c->thread = me;
                }
                break;
            case queue_redispatch:
                //conn_worker_readd(item->c);
                break;
        }
        break;
    /* we were told to pause and report in */
    case 'p':
        break;
    /* a client socket timed out */
    case 't':
        break;
    }
}

/*
 * Set up a thread's information.
 */
static void setup_thread(LIBEVENT_THREAD *me) {
    me->base = event_base_new();
    if (! me->base) {
        fprintf(stderr, "Can't allocate event base\n");
        exit(1);
    }

    /* Listen for notifications from other threads */
    event_assign(&me->notify_event, me->base, me->notify_receive_fd,
    		EV_READ | EV_PERSIST, thread_libevent_process, me);

    if (event_add(&me->notify_event, 0) == -1) {
        fprintf(stderr, "Can't monitor libevent notify pipe\n");
        exit(1);
    }
}


/*
 * Creates a worker thread.
 */
static void create_worker(void *(*func)(void *), void *arg) {
    pthread_attr_t  attr;
    int             ret;

    pthread_attr_init(&attr);

    if ((ret = pthread_create(&((LIBEVENT_THREAD*)arg)->thread_id, &attr, func, arg)) != 0) {
        fprintf(stderr, "Can't create thread: %s\n",
                strerror(ret));
        exit(1);
    }
}

static void register_thread_initialized(void) {
    pthread_mutex_lock(&init_lock);
    init_count++;
    pthread_cond_signal(&init_cond);
    pthread_mutex_unlock(&init_lock);
}

/*
 * Worker thread: main event loop
 */
static void *worker_libevent(void *arg) {
    LIBEVENT_THREAD *me = (LIBEVENT_THREAD*)arg;

    register_thread_initialized();

    event_base_loop(me->base, 0);
    return NULL;
}

static void wait_for_thread_registration(int nthreads) {
    while (init_count < nthreads) {
        pthread_cond_wait(&init_cond, &init_lock);
    }
}

void thread_init(int nthreads)
{
    int i;

    pthread_mutex_init(&init_lock, NULL);
    pthread_cond_init(&init_cond, NULL);

    threads = (LIBEVENT_THREAD*)calloc(nthreads, sizeof(LIBEVENT_THREAD));
    if (! threads) {
        perror("Can't allocate thread descriptors");
        exit(1);
    }

    for (i = 0; i < nthreads; i++) {

    	threads[i].new_conn_queue = new conn_queue;
        pthread_mutex_init(&threads[i].new_conn_queue->lock, NULL);

        int fds[2];
        if (pipe(fds)) {
            perror("Can't create notify pipe");
            exit(1);
        }

        threads[i].notify_receive_fd = fds[0];
        threads[i].notify_send_fd = fds[1];

        setup_thread(&threads[i]);
    }

    /* Create threads after we've done all the libevent setup. */
    for (i = 0; i < nthreads; i++) {
        create_worker(worker_libevent, &threads[i]);
    }

    /* Wait for all the threads to set themselves up before returning. */
    pthread_mutex_lock(&init_lock);
    wait_for_thread_registration(nthreads);
    pthread_mutex_unlock(&init_lock);
}

static int new_socket(struct addrinfo *ai) {
    int sfd;
    int flags;

    if ((sfd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)) == -1) {
        return -1;
    }

    if ((flags = fcntl(sfd, F_GETFL, 0)) < 0 ||
        fcntl(sfd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("setting O_NONBLOCK");
        close(sfd);
        return -1;
    }
    return sfd;
}

static void conn_close(conn *c) {
    assert(c != NULL);

    /* delete the event, the socket and the conn */
    event_del(&c->event);
    fprintf(stderr, "<%d connection closed.\n", c->sfd);
    close(c->sfd);

    return;
}

static int last_thread = -1;

/*
 * Dispatches a new connection to another thread. This is only ever called
 * from the main thread, either during initialization (for UDP) or because
 * of an incoming connection.
 */
void dispatch_conn_new(int sfd, enum conn_states init_state, int event_flags,
                       int read_buffer_size)
{
    CQ_ITEM item;
    char buf[1];

    int tid = (last_thread + 1) % kWorkerThreadNum;

    LIBEVENT_THREAD *thread = threads + tid;

    last_thread = tid;

    item.sfd = sfd;
    item.init_state = init_state;
    item.event_flags = event_flags;
    item.read_buffer_size = read_buffer_size;
    item.mode = queue_new_conn;

    pthread_mutex_lock(&thread->new_conn_queue->lock);
    thread->new_conn_queue->q.push_front(item);
    pthread_mutex_unlock(&thread->new_conn_queue->lock);

    buf[0] = 'c';
    if (write(thread->notify_send_fd, buf, 1) != 1) {
        perror("Writing to thread notify pipe");
    }
}

static void drive_machine(conn *c)
{
    bool stop = false;
    int sfd;
    socklen_t addrlen;
    struct sockaddr_storage addr;

    assert(c != NULL);

    while (!stop) {

        switch(c->state)
        {
        case conn_listening:
            addrlen = sizeof(addr);
            sfd = accept(c->sfd, (struct sockaddr *)&addr, &addrlen);
            if (sfd == -1) {
                if (errno == ENOSYS) {
                    continue;
                }
                perror("accept()");
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    /* these are transient, so don't log anything */
                    stop = true;
                } else if (errno == EMFILE) {
                    fprintf(stderr, "Too many open connections\n");
                    stop = true;
                } else {
                    perror("accept()");
                    stop = true;
                }
                break;
            }
            if (fcntl(sfd, F_SETFL, fcntl(sfd, F_GETFL) | O_NONBLOCK) < 0) {
                perror("setting O_NONBLOCK");
                close(sfd);
                break;
            }
            dispatch_conn_new(sfd, conn_new_cmd, EV_READ | EV_PERSIST, 1024);

            stop = true;
            break;

        case conn_waiting:
        case conn_read:
        case conn_parse_cmd :
        case conn_new_cmd:
        	printf("conn readable\n");
        	break;
        case conn_nread:
        case conn_swallow:
        case conn_write:
        case conn_mwrite:
        case conn_closing:
        case conn_closed:
        case conn_watch:
        case conn_max_state:
            break;
        }
    }

    return;
}
void event_handler(const int fd, const short which, void *arg)
{
	printf("event_handler\n");

    conn *c;

    c = (conn *)arg;
    assert(c != NULL);

    c->which = which;

    /* sanity */
    if (fd != c->sfd) {
        fprintf(stderr, "Catastrophic: event fd doesn't match conn fd!\n");
        conn_close(c);
        return;
    }

    drive_machine(c);

    /* wait for next event */
    return;
}

conn *conn_new(const int sfd, enum conn_states init_state,
                const int event_flags,
                const int read_buffer_size, enum network_transport transport,
                struct event_base *base) {

    conn *c = NULL;

    if (NULL == c) {
        if (!(c = (conn *)calloc(1, sizeof(conn)))) {
            fprintf(stderr, "Failed to allocate connection object\n");
            return NULL;
        }
    }

    c->sfd   = sfd;
    c->state = init_state;

    event_assign(&c->event, base, sfd, event_flags, event_handler, (void *)c);
    c->ev_flags = event_flags;

    if (event_add(&c->event, 0) == -1) {
        perror("event_add");
        return NULL;
    }

    return c;
}

#define IS_UDP(t) ((t) == udp_transport)

struct event_base* main_base;

/**
 * Create a tcp socket and bind it to a specific port number
 * @param port the port number to bind to
 */
static int server_socket(int port)
{
    int sfd;
    struct linger ling = {0, 0};
    struct addrinfo *ai;
    struct addrinfo *next;
    struct addrinfo hints = { .ai_flags = AI_PASSIVE,
                              .ai_family = AF_UNSPEC };
    char port_buf[NI_MAXSERV];
    int error;
    int success = 0;
    int flags =1;

    enum network_transport transport = tcp_transport;
    hints.ai_socktype = SOCK_STREAM;

    if (port == -1) {
        port = 0;
    }
    snprintf(port_buf, sizeof(port_buf), "%d", port);
    error = getaddrinfo(NULL, port_buf, &hints, &ai);
    if (error != 0) {
        if (error != EAI_SYSTEM)
          fprintf(stderr, "getaddrinfo(): %s\n", gai_strerror(error));
        else
          perror("getaddrinfo()");
        return 1;
    }

    for (next = ai; next; next = next->ai_next) {
        conn *listen_conn_add;
        if ((sfd = new_socket(next)) == -1) {
            /* getaddrinfo can return "junk" addresses,
             * we make sure at least one works before erroring.
             */
            if (errno == EMFILE) {
                /* ...unless we're out of fds */
                perror("server_socket");
                exit(-1);
            }
            continue;
        }

#ifdef IPV6_V6ONLY
        if (next->ai_family == AF_INET6) {
            error = setsockopt(sfd, IPPROTO_IPV6, IPV6_V6ONLY, (char *) &flags, sizeof(flags));
            if (error != 0) {
                perror("setsockopt");
                close(sfd);
                continue;
            }
        }
#endif

        setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (void *)&flags, sizeof(flags));
        error = setsockopt(sfd, SOL_SOCKET, SO_KEEPALIVE, (void *)&flags, sizeof(flags));
        if (error != 0)
            perror("setsockopt");

        error = setsockopt(sfd, SOL_SOCKET, SO_LINGER, (void *)&ling, sizeof(ling));
        if (error != 0)
            perror("setsockopt");

        error = setsockopt(sfd, IPPROTO_TCP, TCP_NODELAY, (void *)&flags, sizeof(flags));
        if (error != 0)
            perror("setsockopt");

        if (bind(sfd, next->ai_addr, next->ai_addrlen) == -1) {
            if (errno != EADDRINUSE) {
                perror("bind()");
                close(sfd);
                freeaddrinfo(ai);
                return 1;
            }
            close(sfd);
            continue;
        } else {
            success++;
            if (listen(sfd, 100000) == -1) {
                perror("listen()");
                close(sfd);
                freeaddrinfo(ai);
                return 1;
            }
            if ( (next->ai_addr->sa_family == AF_INET ||
                 next->ai_addr->sa_family == AF_INET6)) {
                union {
                    struct sockaddr_in in;
                    struct sockaddr_in6 in6;
                } my_sockaddr;
                socklen_t len = sizeof(my_sockaddr);
                if (getsockname(sfd, (struct sockaddr*)&my_sockaddr, &len)==0) {
                    if (next->ai_addr->sa_family == AF_INET) {
                        fprintf(stdout, "%s INET: %u\n",
                                IS_UDP(transport) ? "UDP" : "TCP",
                                ntohs(my_sockaddr.in.sin_port));
                    } else {
                        fprintf(stdout, "%s INET6: %u\n",
                                IS_UDP(transport) ? "UDP" : "TCP",
                                ntohs(my_sockaddr.in6.sin6_port));
                    }
                }
            }
        }
        if (!(listen_conn_add = conn_new(sfd, conn_listening,
                                             EV_READ | EV_PERSIST, 1,
                                             transport, main_base))) {
            fprintf(stderr, "failed to create listening connection\n");
            exit(EXIT_FAILURE);
        }
        break;
    }

    freeaddrinfo(ai);

    /* Return zero iff we detected no errors in starting up connections */
    return success == 0;
}

int main()
{
	main_base = event_base_new();
	thread_init(10);
	server_socket(9877);
	event_base_loop(main_base, 0);
	return 0;
}


