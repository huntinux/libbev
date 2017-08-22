/*
 * thread.h
 *
 *  Created on: Aug 21, 2017
 *      Author: jinger
 */

#ifndef SRC_THREAD_H_
#define SRC_THREAD_H_

#include <event2/event.h>
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

#endif /* SRC_THREAD_H_ */
