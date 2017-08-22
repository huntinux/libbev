/*
 * net.h
 *
 *  Created on: Aug 22, 2017
 *      Author: jinger
 */

#ifndef SRC_NET_H_
#define SRC_NET_H_

#include <event2/event.h>
#include <event2/bufferevent.h>
#include <event2/bufferevent_struct.h>
#include <event2/event_struct.h>
#include <pthread.h>

struct conn_quque;
typedef struct {
    pthread_t thread_id;        /* unique ID of this thread */
    struct event_base *base;    /* libevent handle this thread uses */
    struct event notify_event;  /* listen event for notify pipe */
    int notify_receive_fd;      /* receiving end of notify pipe */
    int notify_send_fd;         /* sending end of notify pipe */
    struct conn_queue * new_conn_queue;  /* queue of new connections to handle */
} LIBEVENT_THREAD;

extern struct event_base* main_base;

void thread_init(int nthreads);

int server_socket(int port);

typedef void (*readcb)(struct bufferevent *bev, void *ctx);
void set_worker_thread_readcb(readcb rcb);

void start_work(int worker_thread_num, short port, readcb rcb);
#endif /* SRC_NET_H_ */
