/*
** pqueue.c - FIFO queue management routines.
**
** Copyright (c) 1997-2002 Peter Eriksson <pen@lysator.liu.se>
**
** This program is free software; you can redistribute it and/or
** modify it as you wish - as long as you don't claim that you wrote
** it.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
*/


#include <stdlib.h>
#include <pthread.h>

#ifdef ENABLE_KLEE
#include "klee/klee.h"
//#define DEBUGME
#endif

#include "pqueue.h"

int
pqueue_init(PQUEUE *qp,
           int qsize)
{
    qp->buf = calloc(sizeof(void *), qsize);
    if (qp->buf == NULL)
        return NULL;

    qp->qsize = qsize;
    qp->occupied = 0;
    qp->nextin = 0;
    qp->nextout = 0;
    qp->closed = 0;

    pthread_mutex_init(&qp->mtx, NULL);
    pthread_cond_init(&qp->more, NULL);
    pthread_cond_init(&qp->less, NULL);

    return 0;
}



void
pqueue_close(PQUEUE *qp)
{
    klee_debug("PQUEUE.CLOSE\n");
    pthread_mutex_lock(&qp->mtx);

#ifdef DEBUGME
    klee_print_expr_concretizations("PQUEUE.CLOSED: ", qp->closed, 5);
    klee_print_expr_concretizations("PQUEUE.OCCUP: ", qp->occupied, 5);
    klee_print_expr_concretizations("PQUEUE.SIZE: ", qp->qsize, 5);
#endif

    qp->closed = 1;

    // XXX: FIXME: These used to be flipped .. bug I think?
    pthread_cond_broadcast(&qp->more);
    pthread_mutex_unlock(&qp->mtx);
}


int
pqueue_put(PQUEUE *qp,
          void *item)
{
    //klee_debug("PQUEUE.PUT\n");
    pthread_mutex_lock(&qp->mtx);

    klee_print_expr_concretizations("PQUEUE.CLOSED: ", qp->closed, 5);
    if (qp->closed) {
        // XXX: FIXME: This was not included: bug?
        pthread_mutex_unlock(&qp->mtx);
        return 0;
    }
    
#ifdef DEBUGME
    klee_print_expr_concretizations("PQUEUE.OCCUP: ", qp->occupied, 5);
    klee_print_expr_concretizations("PQUEUE.SIZE: ", qp->qsize, 5);
#endif
    while (qp->occupied >= qp->qsize) {
        //klee_debug("PQUEUE.PUT.WAIT\n");
        pthread_cond_wait(&qp->less, &qp->mtx);
    }

    printf("PQUEUE.PUT!\n");
#ifdef DEBUGME
    klee_debug("PQUEUE.PUT.READY\n");
    klee_print_expr_concretizations("PQUEUE.CLOSED: ", qp->closed, 5);
    klee_print_expr_concretizations("PQUEUE.OCCUP: ", qp->occupied, 5);
    klee_print_expr_concretizations("PQUEUE.SIZE: ", qp->qsize, 5);
#endif
    qp->buf[qp->nextin++] = item;

    qp->nextin %= qp->qsize;
    qp->occupied++;

    // XXX: FIXME: These used to be flipped .. bug I think?
    printf("PQUEUE.PUT.SIGNAL\n");
    pthread_cond_signal(&qp->more);
    printf("PQUEUE.PUT.UNLOCK\n");
    pthread_mutex_unlock(&qp->mtx);

    return 1;
}



int
pqueue_get(PQUEUE *qp,
           void **item)
{
    int got = 0;

    printf("PQUEUE.GET\n");
    pthread_mutex_lock(&qp->mtx);

#ifdef DEBUGME
    klee_print_expr_concretizations("PQUEUE.CLOSED: ", qp->closed, 5);
    klee_print_expr_concretizations("PQUEUE.OCCUP: ", qp->occupied, 5);
    klee_print_expr_concretizations("PQUEUE.SIZE: ", qp->qsize, 5);
#endif

    while (qp->occupied <= 0 && !qp->closed) {
        printf("PQUEUE.GET.WAIT\n");
        pthread_cond_wait(&qp->more, &qp->mtx);
    }

    printf("PQUEUE.GET.READY: %d\n", qp->occupied);
#ifdef DEBUGME
    klee_debug("PQUEUE.GET.READY\n");
    klee_print_expr_concretizations("PQUEUE.CLOSED: ", qp->closed, 5);
    klee_print_expr_concretizations("PQUEUE.OCCUP: ", qp->occupied, 5);
    klee_print_expr_concretizations("PQUEUE.SIZE: ", qp->qsize, 5);
#endif
    if (qp->occupied > 0)
    {
        //klee_print_expr_concretizations("PQUEUE.NEXTOUT: ", qp->nextout, 5);
        //klee_print_expr_concretizations("PQUEUE.SIZE: ", qp->qsize, 5);
        *item = qp->buf[qp->nextout++];
        printf("PQUEUE.GET.DEQUEUE: %s\n", *item);
        qp->nextout %= qp->qsize;
        qp->occupied--;
        got = 1;

        //klee_debug("PQUEUE.GET.NOTIFY\n");
        // XXX: FIXME: These used to be flipped .. bug I think?
        pthread_cond_signal(&qp->less);
        pthread_mutex_unlock(&qp->mtx);
    }
    else
        pthread_mutex_unlock(&qp->mtx);

    printf("PQUEUE.GOT: %d\n", got);
    return got;
}



void
pqueue_destroy(PQUEUE *qp)
{
    pthread_mutex_destroy(&qp->mtx);
    pthread_cond_destroy(&qp->more);
    pthread_cond_destroy(&qp->less);
    free(qp->buf);
}
