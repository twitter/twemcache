/*
 * twemcache - Twitter memcached.
 * Copyright (c) 2012, Twitter, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * * Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * * Neither the name of the Twitter nor the names of its contributors
 *   may be used to endorse or promote products derived from this software
 *   without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <mc_core.h>

#ifndef IOV_MAX
#define IOV_MAX 1024
#endif

extern struct settings settings;

static uint32_t nfree_connq;              /* # free conn q */
static struct conn_tqh free_connq;        /* free conn q */
static pthread_mutex_t free_connq_mutex;  /* free conn q mutex */

static size_t heap_conn = 0;              /* total struct conn heap usage */
static int mc_heap_conn_thread_safe = 1; 
pthread_mutex_t heap_conn_mutex = PTHREAD_MUTEX_INITIALIZER;/*heap_conn mutex*/

void
conn_cq_init(struct conn_q *cq)
{
    pthread_mutex_init(&cq->lock, NULL);
    STAILQ_INIT(&cq->hdr);
}

void
conn_cq_deinit(struct conn_q *cq)
{
}

void
conn_cq_push(struct conn_q *cq, struct conn *c)
{
    STAILQ_NEXT(c, c_tqe) = NULL;

    pthread_mutex_lock(&cq->lock);
    STAILQ_INSERT_TAIL(&cq->hdr, c, c_tqe);
    pthread_mutex_unlock(&cq->lock);
}

struct conn *
conn_cq_pop(struct conn_q *cq)
{
    struct conn *c;

    pthread_mutex_lock(&cq->lock);
    c = STAILQ_FIRST(&cq->hdr);
    if (c != NULL) {
        STAILQ_REMOVE(&cq->hdr, c, conn, c_tqe);
    }
    pthread_mutex_unlock(&cq->lock);

    return c;
}

void
conn_init(void)
{
    log_debug(LOG_DEBUG, "conn size %d", sizeof(struct conn));
    nfree_connq = 0;
    STAILQ_INIT(&free_connq);
    pthread_mutex_init(&free_connq_mutex, NULL);
}

void
conn_deinit(void)
{
}

static void
conn_free(struct conn *c)
{
    log_debug(LOG_VVERB, "free conn %p c %d", c, c->sd);

    if (c->udp_hbuf != NULL) {
        mc_free(c->udp_hbuf);
    }

    if (c->msg != NULL) {
        mc_free(c->msg);
    }

    if (c->rbuf != NULL) {
        mc_free(c->rbuf);
    }

    if (c->wbuf != NULL) {
        mc_free(c->wbuf);
    }

    if (c->ilist != NULL) {
        mc_free(c->ilist);
    }

    if (c->slist != NULL) {
        mc_free(c->slist);
    }

    if (c->iov != NULL) {
        mc_free(c->iov);
    }

    pthread_mutex_lock(&heap_conn_mutex);
    heap_conn -= (sizeof(*c) + c->rsize + c->wsize);
    pthread_mutex_unlock(&heap_conn_mutex);
    
    mc_free(c);
}

void
conn_put(struct conn *c)
{
    log_debug(LOG_VVERB, "put conn %p c %d", c, c->sd);

    if (c->rsize > RSIZE_HIGHWAT) {
        conn_free(c);
        return;
    }

    pthread_mutex_lock(&free_connq_mutex);
    nfree_connq++;
    STAILQ_INSERT_TAIL(&free_connq, c, c_tqe);
    pthread_mutex_unlock(&free_connq_mutex);
}

static struct conn *
_conn_get(void)
{
    struct conn *c;

    pthread_mutex_lock(&free_connq_mutex);

    if (!STAILQ_EMPTY(&free_connq)) {
        ASSERT(nfree_connq > 0);

        c = STAILQ_FIRST(&free_connq);
        nfree_connq--;
        STAILQ_REMOVE(&free_connq, c, conn, c_tqe);
    } else {
        c = NULL;
    }

    pthread_mutex_unlock(&free_connq_mutex);

    return c;
}

struct conn *
conn_get(int sd, conn_state_t state, int ev_flags, int rsize, int udp)
{
    struct conn *c;

    ASSERT(state >= CONN_LISTEN && state < CONN_SENTINEL);
    ASSERT(rsize > 0);

    c = _conn_get();
    if (c == NULL) {
        c = mc_zalloc(sizeof(*c));
        if (c == NULL) {
            return NULL;
        }

        c->rsize = rsize;
        c->rbuf = mc_alloc(c->rsize);

        c->wsize = TCP_BUFFER_SIZE;
        c->wbuf = mc_alloc(c->wsize);

        c->isize = ILIST_SIZE;
        c->ilist = mc_alloc(sizeof(*c->ilist) * c->isize);

        c->ssize = SLIST_SIZE;
        c->slist = mc_alloc(sizeof(*c->slist) * c->ssize);

        c->iov_size = IOV_SIZE;
        c->iov = mc_alloc(sizeof(*c->iov) * c->iov_size);

        c->msg_size = MSG_SIZE;
        c->msg = mc_alloc(sizeof(*c->msg) * c->msg_size);

        if (c->rbuf == NULL || c->wbuf == NULL || c->ilist == NULL ||
            c->iov == NULL || c->msg == NULL || c->slist == NULL) {
            conn_free(c);
            return NULL;
        }

        stats_thread_incr(conn_struct);

        pthread_mutex_lock(&heap_conn_mutex);
        heap_conn += (sizeof(*c) + c->rsize + c->wsize);
        pthread_mutex_unlock(&heap_conn_mutex);
    }

    STAILQ_NEXT(c, c_tqe) = NULL;
    c->thread = NULL;

    c->sd = sd;
    c->state = state;
    /* c->event is initialized later */
    c->ev_flags = ev_flags;
    c->which = 0;

    ASSERT(c->rbuf != NULL && c->rsize > 0);
    c->rcurr = c->rbuf;
    c->rbytes = 0;

    ASSERT(c->wbuf != NULL && c->wsize > 0);
    c->wcurr = c->wbuf;
    c->wbytes = 0;

    c->write_and_go = state;
    c->write_and_free = NULL;

    c->ritem = NULL;
    c->rlbytes = 0;

    c->item = NULL;
    c->sbytes = 0;

    ASSERT(c->iov != NULL && c->iov_size > 0);
    c->iov_used = 0;

    ASSERT(c->msg != NULL && c->msg_size > 0);
    c->msg_used = 0;
    c->msg_curr = 0;
    c->msg_bytes = 0;

    ASSERT(c->ilist != NULL && c->isize > 0);
    c->icurr = c->ilist;
    c->ileft = 0;

    ASSERT(c->slist != NULL && c->ssize > 0);
    c->scurr = c->slist;
    c->sleft = 0;

    c->stats.buffer = NULL;
    c->stats.size = 0;
    c->stats.offset = 0;

    c->req_type = REQ_UNKNOWN;
    c->req = NULL;
    c->req_len = 0;

    c->udp = udp;
    c->udp_rid = 0;
    c->udp_hbuf = NULL;
    c->udp_hsize = 0;

    c->noreply = 0;

    stats_thread_incr(conn_total);
    stats_thread_incr(conn_curr);

    log_debug(LOG_VVERB, "get conn %p c %d", c, c->sd);

    return c;
}

rstatus_t
conn_set_event(struct conn *c, struct event_base *base)
{
    int status;

    event_set(&c->event, c->sd, c->ev_flags, core_event_handler, c);
    event_base_set(base, &c->event);

    status = event_add(&c->event, 0);
    if (status < 0) {
        return MC_ERROR;
    }

    return MC_OK;
}

void
conn_cleanup(struct conn *c)
{
    ASSERT(c != NULL);

    if (c->item != NULL) {
        item_remove(c->item);
        c->item = NULL;
    }

    while (c->ileft > 0) {
        item_remove(*(c->icurr));
        c->ileft--;
        c->icurr++;
    }

    while (c->sleft > 0) {
        cache_free(c->thread->suffix_cache, *(c->scurr));
        c->sleft--;
        c->scurr++;
    }

    if (c->write_and_free != NULL) {
        mc_free(c->write_and_free);
    }

    if (c->udp) {
        conn_set_state(c, CONN_READ);
    }
}

void
conn_close(struct conn *c)
{
    /* delete the event, the socket and the conn */
    event_del(&c->event);

    log_debug(LOG_VVERB, "<%d connection closed", c->sd);

    close(c->sd);
    core_accept_conns(true);
    conn_cleanup(c);

    conn_put(c);

    stats_thread_decr(conn_curr);

    return;
}

/*
 * Shrinks a connection's buffers if they're too big.  This prevents
 * periodic large "get" requests from permanently chewing lots of server
 * memory.
 *
 * This should only be called in between requests since it can wipe output
 * buffers!
 */
void
conn_shrink(struct conn *c)
{
    ASSERT(c != NULL);

    if (c->udp) {
        return;
    }

    if (c->rsize > RSIZE_HIGHWAT && c->rbytes < TCP_BUFFER_SIZE) {
        char *newbuf;

        if (c->rcurr != c->rbuf) {
            memmove(c->rbuf, c->rcurr, (size_t)c->rbytes);
        }

        newbuf = mc_realloc(c->rbuf, TCP_BUFFER_SIZE);
        if (newbuf != NULL) {
            c->rbuf = newbuf;
            c->rsize = TCP_BUFFER_SIZE;
        }
        /* TODO check other branch... */
        c->rcurr = c->rbuf;
    }

    if (c->isize > ILIST_HIGHWAT) {
        struct item **newbuf;

        newbuf = mc_realloc(c->ilist, ILIST_SIZE * sizeof(c->ilist[0]));
        if (newbuf != NULL) {
            c->ilist = newbuf;
            c->isize = ILIST_SIZE;
        }
        /* TODO check error condition? */
    }

    if (c->msg_size > MSG_HIGHWAT) {
        struct msghdr *newbuf;

        newbuf = mc_realloc(c->msg, MSG_SIZE * sizeof(c->msg[0]));
        if (newbuf != NULL) {
            c->msg = newbuf;
            c->msg_size = MSG_SIZE;
        }
        /* TODO check error condition? */
    }

    if (c->iov_size > IOV_HIGHWAT) {
        struct iovec *newbuf;

        newbuf = mc_realloc(c->iov, IOV_SIZE * sizeof(c->iov[0]));
        if (newbuf != NULL) {
            c->iov = newbuf;
            c->iov_size = IOV_SIZE;
        }
        /* TODO check return value */
    }
}

/*
 * Sets a connection's current state in the state machine. Any special
 * processing that needs to happen on certain state transitions can
 * happen here.
 */
void
conn_set_state(struct conn *c, conn_state_t state)
{
    ASSERT(state >= CONN_LISTEN && state < CONN_SENTINEL);
    ASSERT(c->state >= CONN_LISTEN && c->state < CONN_SENTINEL);

    if (state == c->state) {
        return;
    }

    log_debug(LOG_VVERB, "c %d going from state %d to %d", c->sd,
              c->state, state);
    c->state = state;
}

/*
 * Ensures that there is room for another struct iovec in a connection's
 * iov list.
 *
 * Returns 0 on success, -1 on out-of-memory.
 */
static rstatus_t
conn_ensure_iov_space(struct conn *c)
{
    ASSERT(c != NULL);

    if (c->iov_used >= c->iov_size) {
        int i, iovnum;
        struct iovec *new_iov;

        new_iov = mc_realloc(c->iov, (c->iov_size * 2) * sizeof(*c->iov));
        if (new_iov == NULL) {
            return MC_ENOMEM;
        }
        c->iov = new_iov;
        c->iov_size *= 2;

        /* point all the msghdr structures at the new list */
        for (i = 0, iovnum = 0; i < c->msg_used; i++) {
            c->msg[i].msg_iov = &c->iov[iovnum];
            iovnum += c->msg[i].msg_iovlen;
        }
    }

    return MC_OK;
}

/*
 * Adds data to the list of pending data that will be written out to a
 * connection.
 *
 * Returns 0 on success, -1 on out-of-memory.
 */
rstatus_t
conn_add_iov(struct conn *c, const void *buf, int len)
{
    struct msghdr *m;
    int leftover;
    bool limit_to_mtu;

    ASSERT(c != NULL);

    do {
        rstatus_t status;

        ASSERT(c->msg_used >= 1);

        m = &c->msg[c->msg_used - 1];

        /*
         * Limit UDP packets, and the first payloads of TCP replies, to
         * UDP_MAX_PAYLOAD_SIZE bytes.
         */
        limit_to_mtu = c->udp || (1 == c->msg_used);

        /* We may need to start a new msghdr if this one is full. */
        if (m->msg_iovlen == IOV_MAX ||
            (limit_to_mtu && c->msg_bytes >= UDP_MAX_PAYLOAD_SIZE)) {
            status = conn_add_msghdr(c);
            if (status != MC_OK) {
                return status;
            }
        }

        status = conn_ensure_iov_space(c);
        if (status != MC_OK) {
            return status;
        }

        /* if the fragment is too big to fit in the datagram, split it up */
        if (limit_to_mtu && len + c->msg_bytes > UDP_MAX_PAYLOAD_SIZE) {
            leftover = len + c->msg_bytes - UDP_MAX_PAYLOAD_SIZE;
            len -= leftover;
        } else {
            leftover = 0;
        }

        ASSERT(c->msg_used >= 1);

        m = &c->msg[c->msg_used - 1];
        m->msg_iov[m->msg_iovlen].iov_base = (void *)buf;
        m->msg_iov[m->msg_iovlen].iov_len = len;

        c->msg_bytes += len;
        c->iov_used++;
        m->msg_iovlen++;

        buf = ((char *)buf) + len;
        len = leftover;
    } while (leftover > 0);

    return MC_OK;
}

/*
 * Adds a message header to a connection.
 *
 * Returns 0 on success, -1 on out-of-memory.
 */
rstatus_t
conn_add_msghdr(struct conn *c)
{
    struct msghdr *msg;

    ASSERT(c != NULL);

    if (c->msg_size == c->msg_used) {
        msg = mc_realloc(c->msg, c->msg_size * 2 * sizeof(*c->msg));
        if (msg == NULL) {
            return MC_ENOMEM;
        }
        c->msg = msg;
        c->msg_size *= 2;
    }

    msg = c->msg + c->msg_used;

    memset(msg, 0, sizeof(*msg));

    msg->msg_iov = &c->iov[c->iov_used];

    if (c->udp) {
        msg->msg_name = &c->udp_raddr;
        msg->msg_namelen = c->udp_raddr_size;
    }

    c->msg_bytes = 0;
    c->msg_used++;

    if (c->udp) {
        /* leave room for the UDP header, which we'll fill in later */
        return conn_add_iov(c, NULL, UDP_HEADER_SIZE);
    }

    return MC_OK;
}

/*
 * Constructs a set of UDP headers and attaches them to the outgoing
 * messages.
 */
rstatus_t
conn_build_udp_headers(struct conn *c)
{
    int i;
    unsigned char *hdr;

    ASSERT(c != NULL);

    if (c->msg_used > c->udp_hsize) {
        void *new_udp_hbuf;

        if (c->udp_hbuf != NULL) {
            new_udp_hbuf = mc_realloc(c->udp_hbuf, c->msg_used * 2 * UDP_HEADER_SIZE);
        } else {
            new_udp_hbuf = mc_alloc(c->msg_used * 2 * UDP_HEADER_SIZE);
        }
        if (new_udp_hbuf == NULL) {
            return MC_ENOMEM;
        }
        c->udp_hbuf = (unsigned char *)new_udp_hbuf;
        c->udp_hsize = c->msg_used * 2;
    }

    hdr = c->udp_hbuf;
    for (i = 0; i < c->msg_used; i++) {
        c->msg[i].msg_iov[0].iov_base = (void*)hdr;
        c->msg[i].msg_iov[0].iov_len = UDP_HEADER_SIZE;
        *hdr++ = c->udp_rid / 256;
        *hdr++ = c->udp_rid % 256;
        *hdr++ = i / 256;
        *hdr++ = i % 256;
        *hdr++ = c->msg_used / 256;
        *hdr++ = c->msg_used % 256;
        *hdr++ = 0;
        *hdr++ = 0;
        ASSERT((void *) hdr == (caddr_t)c->msg[i].msg_iov[0].iov_base + UDP_HEADER_SIZE);
    }

    return MC_OK;
}

size_t
mc_get_heap_conn(void)
{
    size_t hc = 0;

    if (mc_heap_conn_thread_safe) {
        pthread_mutex_lock(&heap_conn_mutex);
        hc = heap_conn;
        pthread_mutex_unlock(&heap_conn_mutex);
    } else {
        hc = heap_conn;
    }

    return hc;
}
