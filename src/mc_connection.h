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

#ifndef _MC_CONNECTION_H_
#define _MC_CONNECTION_H_

/*
 * Default and high watermark sizes of various connection
 * related buffers
 */
#define UDP_HEADER_SIZE      8
#define UDP_BUFFER_SIZE      65536
#define UDP_MAX_PAYLOAD_SIZE 1400

#define TCP_BUFFER_SIZE      2048

#define RSIZE_HIGHWAT        8192

#define ILIST_SIZE           200
#define ILIST_HIGHWAT        400

#define SLIST_SIZE           20

#define IOV_SIZE             400
#define IOV_HIGHWAT          600

#define MSG_SIZE             10
#define MSG_HIGHWAT          100

typedef enum conn_state {
    CONN_LISTEN,        /* socket which listens for connections */
    CONN_NEW_CMD,       /* prepare connection for next command */
    CONN_WAIT,          /* waiting for a readable socket */
    CONN_READ,          /* reading in a command line */
    CONN_PARSE,         /* try to parse a command from the input buffer */
    CONN_NREAD,         /* reading in a fixed number of bytes */
    CONN_WRITE,         /* writing out a simple response */
    CONN_MWRITE,        /* writing out many items sequentially */
    CONN_SWALLOW,       /* swallowing unnecessary bytes w/o storing */
    CONN_CLOSE,         /* closing this connection */
    CONN_SENTINEL       /* max state value (used for assertion) */
} conn_state_t;

struct conn {
    STAILQ_ENTRY(conn)   c_tqe;            /* link in thread / listen / free q */
    struct thread_worker *thread;          /* owner thread */

    int                  sd;               /* socket descriptor */
    conn_state_t         state;            /* connection state */
    struct event         event;            /* libevent */
    short                ev_flags;         /* event flags */
    short                which;            /* triggered event */

    char                 *rbuf;            /* read buffer */
    int                  rsize;            /* read buffer size */
    char                 *rcurr;           /* read parsing marker */
    int                  rbytes;           /* read unparsed bytes */

    char                 *wbuf;            /* write buffer */
    int                  wsize;            /* write buffer size */
    char                 *wcurr;           /* write marker */
    int                  wbytes;           /* write bytes */

    conn_state_t         write_and_go;     /* which state to go into after finishing current write */
    void                 *write_and_free;  /* free this memory after finishing writing */

    char                 *ritem;           /* when we read in an item's value, it goes here */
    int                  rlbytes;

    void                 *item;            /* for commands set / add / replace */
    int                  sbytes;           /* how many bytes to swallow in CONN_SWALLOW state*/

    struct iovec         *iov;             /* scatter/gather iov */
    int                  iov_size;         /* # iov */
    int                  iov_used;         /* # used iov */

    struct msghdr        *msg;             /* scatter/gather msg */
    int                  msg_size;         /* # msg */
    int                  msg_used;         /* # used msg */
    int                  msg_curr;         /* current msg being transmitted */
    int                  msg_bytes;        /* current msg bytes to transmit */

    struct item          **ilist;          /* item list */
    int                  isize;            /* # item list */
    struct item          **icurr;          /* current item list */
    int                  ileft;            /* # remaining in item list */

    char                 **slist;          /* suffix list */
    int                  ssize;            /* # suffix list */
    char                 **scurr;          /* current suffix list */
    int                  sleft;            /* # remaining in suffix list */

    struct {
        char             *buffer;          /* stats buffer */
        size_t           size;             /* stats buffer size */
        size_t           offset;           /* stats buffer offset */
    } stats;

    req_type_t           req_type;         /* request type */
    char                 *req;             /* request header */
    int                  req_len;          /* request header length */

    char                 peer[32];         /* printable host:port, possibly truncated */

    int                  udp_rid;          /* udp request id */
    struct sockaddr      udp_raddr;        /* udp request address */
    socklen_t            udp_raddr_size;   /* udp request address size */
    unsigned char        *udp_hbuf;        /* udp header */
    int                  udp_hsize;        /* udp header size */

    unsigned             noreply:1;        /* noreply? */
    unsigned             udp:1;            /* udp? */
};

STAILQ_HEAD(conn_tqh, conn);

struct conn_q {
    struct conn_tqh hdr;  /* conn queue header */
    pthread_mutex_t lock; /* conn queue lock */
};

void conn_cq_init(struct conn_q *cq);
void conn_cq_deinit(struct conn_q *cq);
void conn_cq_push(struct conn_q *cq, struct conn *c);
struct conn *conn_cq_pop(struct conn_q *cq);

void conn_init(void);
void conn_deinit(void);

struct conn *conn_get(int sd, conn_state_t state, int ev_flags, int rsize, int udp);
void conn_put(struct conn *c);

void conn_cleanup(struct conn *c);
void conn_close(struct conn *c);
void conn_shrink(struct conn *c);

rstatus_t conn_add_iov(struct conn *c, const void *buf, int len);
rstatus_t conn_add_msghdr(struct conn *c);

rstatus_t conn_build_udp_headers(struct conn *c);

void conn_set_state(struct conn *c, conn_state_t state);

rstatus_t conn_set_event(struct conn *conn, struct event_base *base);

#endif
