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

#include <stdio.h>
#include <stdlib.h>

#include <mc_core.h>

extern struct settings settings;

static struct conn_tqh listen_connq; /* listening conn q */
struct event_base *main_base;        /* main base */
static pthread_mutex_t accept_lock;  /* connection accept lock */

typedef enum read_result {
    READ_DATA_RECEIVED,
    READ_NO_DATA_RECEIVED,
    READ_ERROR,
    READ_MEMORY_ERROR
} read_result_t;

typedef enum transmit_result {
    TRANSMIT_COMPLETE,   /* all done writing */
    TRANSMIT_INCOMPLETE, /* more data remaining to write */
    TRANSMIT_SOFT_ERROR, /* can't write any more right now */
    TRANSMIT_HARD_ERROR  /* can't write (c->state is set to CONN_CLOSE) */
} transmit_result_t;

static void
core_reset_cmd_handler(struct conn *c)
{
    c->req_type = REQ_UNKNOWN;
    c->req = NULL;
    c->req_len = 0;

    if (c->item != NULL) {
        item_remove(c->item);
        c->item = NULL;
    }

    conn_shrink(c);

    if (c->rbytes > 0) {
        conn_set_state(c, CONN_PARSE);
    } else {
        conn_set_state(c, CONN_WAIT);
    }
}

static void
core_complete_nread(struct conn *c)
{
    asc_complete_nread(c);
}

/*
 * Set up a connection to write a buffer then free it
 *
 * Used by the stats module
 */
void
core_write_and_free(struct conn *c, char *buf, int bytes)
{
    if (buf != NULL) {
        c->write_and_free = buf;
        c->wcurr = buf;
        c->wbytes = bytes;
        conn_set_state(c, CONN_WRITE);
        c->write_and_go = CONN_NEW_CMD;
    } else {
        log_warn("server error on c %d for req of type %d because message "
                  "buffer is NULL", c->sd, c->req_type);

        asc_write_server_error(c);
    }
}

static void
core_parse(struct conn *c)
{
    rstatus_t status;

    status = asc_parse(c);
    switch (status) {
    case MC_EAGAIN:
        conn_set_state(c, CONN_WAIT);

    default:
        break;
    }
}

/*
 * Read a UDP request
 */
static read_result_t
core_read_udp(struct conn *c)
{
    int res;

    c->udp_raddr_size = sizeof(c->udp_raddr);
    res = recvfrom(c->sd, c->rbuf, c->rsize,
                   0, &c->udp_raddr, &c->udp_raddr_size);
    if (res > 8) {
        unsigned char *buf = (unsigned char *)c->rbuf;

        stats_thread_incr_by(data_read, res);

        /* beginning of UDP packet is the request ID; save it */
        c->udp_rid = buf[0] * 256 + buf[1];

        /* if this is a multi-packet request, drop it */
        if (buf[4] != 0 || buf[5] != 1) {
            log_warn("server error: multipacket req not supported");

            asc_write_server_error(c);
            return READ_NO_DATA_RECEIVED;
        }

        /* don't care about any of the rest of the header */
        res -= 8;
        memmove(c->rbuf, c->rbuf + 8, res);

        c->rbytes = res;
        c->rcurr = c->rbuf;

        return READ_DATA_RECEIVED;
    }

    return READ_NO_DATA_RECEIVED;
}

/*
 * Read from network as much as we can, handle buffer overflow and connection
 * close. Before reading, move the remaining incomplete fragment of a command
 * (if any) to the beginning of the buffer.
 *
 * To protect us from someone flooding a connection with bogus data causing
 * the connection to eat up all available memory, break out and start looking
 * at the data I've got after a number of reallocs...
 *
 * @return read_result_t
 */
static read_result_t
core_read_tcp(struct conn *c)
{
    ssize_t n;
    size_t size;
    read_result_t gotdata = READ_NO_DATA_RECEIVED;
    int num_allocs = 0;

    ASSERT(c->rcurr <= c->rbuf + c->rsize);

    if (c->rcurr != c->rbuf) {
        if (c->rbytes != 0) {
            memmove(c->rbuf, c->rcurr, c->rbytes);
        }
        c->rcurr = c->rbuf;
    }

    for (;;) {

        if (c->rbytes >= c->rsize) {
            char *new_rbuf;

            if (num_allocs == 4) {
                return gotdata;
            }
            ++num_allocs;

            new_rbuf = mc_realloc(c->rbuf, c->rsize * 2);
            if (new_rbuf == NULL) {
                log_warn("server error on c %d for req of type %d because of "
                         "oom alloc buf for new req", c->sd, c->req_type);

                c->rbytes = 0; /* ignore what we read */
                asc_write_server_error(c);
                c->write_and_go = CONN_CLOSE;
                return READ_MEMORY_ERROR;
            }
            c->rcurr = c->rbuf = new_rbuf;
            c->rsize *= 2;
        }

        size = c->rsize - c->rbytes;
        n = read(c->sd, c->rbuf + c->rbytes, size);

        log_debug(LOG_VERB, "recv on c %d %zd of %zu", c->sd, n, size);

        if (n > 0) {
            stats_thread_incr_by(data_read, n);
            gotdata = READ_DATA_RECEIVED;
            c->rbytes += n;
            if (n == size) {
                continue;
            } else {
                break;
            }
        }

        if (n == 0) {
            log_debug(LOG_INFO, "recv on c %d eof", c->sd);
            return READ_ERROR;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            log_debug(LOG_VERB, "recv on c %d not ready - eagain", c->sd);
            break;
        }

        return READ_ERROR;
    }

    return gotdata;
}

static void
core_read(struct conn *c)
{
    read_result_t result;

    result = c->udp ? core_read_udp(c) : core_read_tcp(c);
    switch (result) {
    case READ_NO_DATA_RECEIVED:
        conn_set_state(c, CONN_WAIT);
        break;

    case READ_DATA_RECEIVED:
        conn_set_state(c, CONN_PARSE);
        break;

    case READ_ERROR:
        conn_set_state(c, CONN_CLOSE);
        break;

    case READ_MEMORY_ERROR:
        /* state already set by core_read_tcp */
        break;

    default:
        NOT_REACHED();
        break;
    }
}

static rstatus_t
core_update(struct conn *c, int new_flags)
{
    int status;
    struct event_base *base;

    base = c->event.ev_base;

    if (c->ev_flags == new_flags) {
        return MC_OK;
    }

    status = event_del(&c->event);
    if (status < 0) {
        return MC_ERROR;
    }

    event_set(&c->event, c->sd, new_flags, core_event_handler, c);
    event_base_set(base, &c->event);
    c->ev_flags = new_flags;

    status = event_add(&c->event, 0);
    if (status < 0) {
        return MC_ERROR;
    }

    return MC_OK;
}

/*
 * Sets up whether or not to accept new connections
 */
static void
_core_accept_conns(bool do_accept)
{
    rstatus_t status;
    struct conn *c;

    STAILQ_FOREACH(c, &listen_connq, c_tqe) {
        if (do_accept) {
            status = core_update(c, EV_READ | EV_PERSIST);
            if (status != MC_OK) {
                log_warn("update on c %d failed, ignored: %s", c->sd,
                         strerror(errno));
            }

            status = listen(c->sd, settings.backlog);
            if (status != MC_OK) {
                log_warn("listen on c %d failed, ignored: %s", c->sd,
                         strerror(errno));
            }
        } else {
            status = core_update(c, 0);
            if (status != MC_OK) {
                log_warn("update on c %d failed, ignored: %s", c->sd,
                         strerror(errno));
            }

            status = listen(c->sd, 0);
            if (status != MC_OK) {
                log_warn("listen on c %d failed, ignored: %s", c->sd,
                         strerror(errno));
            }
        }
    }

    if (do_accept) {
        settings.accepting_conns = true;
    } else {
        settings.accepting_conns = false;
        stats_thread_incr(conn_disabled);
    }
}

void
core_accept_conns(bool do_accept)
{
    pthread_mutex_lock(&accept_lock);
    _core_accept_conns(do_accept);
    pthread_mutex_unlock(&accept_lock);
}

/*
 * Transmit the next chunk of data from our list of msgbuf structures
 *
 * Returns:
 *   TRANSMIT_COMPLETE   All done writing.
 *   TRANSMIT_INCOMPLETE More data remaining to write.
 *   TRANSMIT_SOFT_ERROR Can't write any more right now.
 *   TRANSMIT_HARD_ERROR Can't write (c->state is set to CONN_CLOSE)
 */
static transmit_result_t
core_transmit(struct conn *c)
{
    rstatus_t status;

    if (c->msg_curr < c->msg_used &&
        c->msg[c->msg_curr].msg_iovlen == 0) {
        /* finished writing the current msg; advance to the next */
        c->msg_curr++;
    }
    if (c->msg_curr < c->msg_used) {
        ssize_t res;
        struct msghdr *m = &c->msg[c->msg_curr];

        res = sendmsg(c->sd, m, 0);
        if (res > 0) {
            stats_thread_incr_by(data_written, res);

            /*
             * We've written some of the data; remove the completed
             * iovec entries from the list of pending writes
             */
            while (m->msg_iovlen > 0 && res >= m->msg_iov->iov_len) {
                res -= m->msg_iov->iov_len;
                m->msg_iovlen--;
                m->msg_iov++;
            }

            /*
             * Might have written just part of the last iovec entry; adjust
             * it so the next write will do the rest
             */
            if (res > 0) {
                m->msg_iov->iov_base = (caddr_t)m->msg_iov->iov_base + res;
                m->msg_iov->iov_len -= res;
            }
            return TRANSMIT_INCOMPLETE;
        }

        if (res == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            status = core_update(c, EV_WRITE | EV_PERSIST);
            if (status != MC_OK) {
                log_error("update on c %d failed: %s", c->sd, strerror(errno));
                conn_set_state(c, CONN_CLOSE);
                return TRANSMIT_HARD_ERROR;
            }

            return TRANSMIT_SOFT_ERROR;
        }

        /*
         * if res == 0 or res == -1 and error is not EAGAIN or EWOULDBLOCK,
         * we have a real error, on which we close the connection
         */
        log_debug(LOG_ERR, "failed to write, and not due to blocking: %s",
                  strerror(errno));

        if (c->udp) {
            conn_set_state(c, CONN_READ);
        } else {
            conn_set_state(c, CONN_CLOSE);
        }

        return TRANSMIT_HARD_ERROR;
    } else {
        return TRANSMIT_COMPLETE;
    }
}

static void
core_close(struct conn *c)
{
    log_debug(LOG_NOTICE, "close c %d", c->sd);

    if (c->udp) {
        conn_cleanup(c);
    } else {
        conn_close(c);
    }
}

static void
core_accept(struct conn *c)
{
    rstatus_t status;
    int sd;

    ASSERT(c->state == CONN_LISTEN);

    for (;;) {
        sd = accept(c->sd, NULL, NULL);
        if (sd < 0) {
            if (errno == EINTR) {
                log_debug(LOG_VERB, "accept on s %d not ready - eintr", c->sd);
                continue;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                log_debug(LOG_VERB, "accept on s %d not ready - eagain", c->sd);
                return;
            }

            if (errno == EMFILE || errno == ENFILE) {
                log_debug(LOG_VERB, "accept on s %d not ready - emfile", c->sd);
                core_accept_conns(false);
                return;
            }

            log_error("accept on s %d failed: %s", c->sd, strerror(errno));
            return;
        }

        break;
    }

    status = mc_set_nonblocking(sd);
    if (status != MC_OK) {
        log_error("set nonblock on c %d from s %d failed: %s", sd, c->sd,
                  strerror(errno));
        close(sd);
        return;
    }

    status = mc_set_keepalive(sd);
    if (status != MC_OK) {
        log_warn("set keepalive on c %d from s %d failed, ignored: %s", sd,
                 c->sd, strerror(errno));
    }

    status = mc_set_tcpnodelay(sd);
    if (status != MC_OK) {
        log_warn("set tcp nodelay on c %d from s %d failed, ignored: %s", sd,
                 c->sd, strerror(errno));
    }

    status = thread_dispatch(sd, CONN_NEW_CMD, EV_READ | EV_PERSIST, 0);
    if (status != MC_OK) {
        log_error("dispatch c %d from s %d failed: %s", sd, c->sd,
                  strerror(errno));
        close(sd);
        return;
    }
}

static void
core_drive_machine(struct conn *c)
{
    rstatus_t status;
    ssize_t n;
    bool stop = false;
    int nreqs = settings.reqs_per_event;

    while (!stop) {

        switch (c->state) {

        case CONN_LISTEN:
            core_accept(c);
            stop = true;
            break;

        case CONN_WAIT:
            status = core_update(c, EV_READ | EV_PERSIST);
            if (status != MC_OK) {
                log_error("update on c %d failed: %s", c->sd, strerror(errno));
                conn_set_state(c, CONN_CLOSE);
                break;
            }

            conn_set_state(c, CONN_READ);
            stop = true;
            break;

        case CONN_READ:
            core_read(c);
            break;

        case CONN_PARSE:
            core_parse(c);
            break;

        case CONN_NEW_CMD:
            /*
             * Only process nreqs at a time to avoid starving other
             * connection
             */
            --nreqs;
            if (nreqs >= 0) {
                core_reset_cmd_handler(c);
            } else {
                stats_thread_incr(conn_yield);

                if (c->rbytes > 0) {
                    /*
                     * We have already read in data into the input buffer,
                     * so libevent will most likely not signal read events
                     * on the socket (unless more data is available. As a
                     * hack we should just put in a request to write data,
                     * because that should be possible ;-)
                     */
                    status = core_update(c, EV_WRITE | EV_PERSIST);
                    if (status != MC_OK) {
                        log_error("update on c %d failed: %s", c->sd, strerror(errno));
                        conn_set_state(c, CONN_CLOSE);
                    }
                }
                stop = true;
            }
            break;

        case CONN_NREAD:
            if (c->rlbytes == 0) {
                core_complete_nread(c);
                break;
            }

            /* first check if we have leftovers in the CONN_READ buffer */
            if (c->rbytes > 0) {
                int tocopy = c->rbytes > c->rlbytes ? c->rlbytes : c->rbytes;

                if (c->ritem != c->rcurr) {
                    memmove(c->ritem, c->rcurr, tocopy);
                }
                c->ritem += tocopy;
                c->rlbytes -= tocopy;
                c->rcurr += tocopy;
                c->rbytes -= tocopy;
                if (c->rlbytes == 0) {
                    break;
                }
            }

            /* now try reading from the socket */
            n = read(c->sd, c->ritem, c->rlbytes);
            if (n > 0) {
                stats_thread_incr_by(data_read, n);

                if (c->rcurr == c->ritem) {
                    c->rcurr += n;
                }
                c->ritem += n;
                c->rlbytes -= n;
                break;
            }

            if (n == 0) {
                /* end of stream */
                conn_set_state(c, CONN_CLOSE);
                break;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                status = core_update(c, EV_READ | EV_PERSIST);
                if (status != MC_OK) {
                    log_error("update on c %d failed: %s", c->sd, strerror(errno));
                    conn_set_state(c, CONN_CLOSE);
                    break;
                }
                stop = true;
                break;
            }

            /* otherwise we have a real error, on which we close the connection */
            log_debug(LOG_INFO, "failed to read, and not due to blocking:\n"
                      "errno: %d %s\nrcurr=%lx ritem=%lx rbuf=%lx rlbytes=%d "
                      "rsize=%d", errno, strerror(errno), (long)c->rcurr,
                      (long)c->ritem, (long)c->rbuf, (int)c->rlbytes,
                      (int)c->rsize);
            conn_set_state(c, CONN_CLOSE);
            break;

        case CONN_SWALLOW:
            /* we are reading sbytes and throwing them away */
            if (c->sbytes == 0) {
                conn_set_state(c, CONN_NEW_CMD);
                break;
            }

            /* first check if we have leftovers in the CONN_READ buffer */
            if (c->rbytes > 0) {
                int tocopy = c->rbytes > c->sbytes ? c->sbytes : c->rbytes;

                c->sbytes -= tocopy;
                c->rcurr += tocopy;
                c->rbytes -= tocopy;
                break;
            }

            /* now try reading from the socket */
            n = read(c->sd, c->rbuf, c->rsize > c->sbytes ? c->sbytes : c->rsize);
            if (n > 0) {
                stats_thread_incr_by(data_read, n);

                c->sbytes -= n;
                break;
            }

            if (n == 0) {
                /* end of stream */
                conn_set_state(c, CONN_CLOSE);
                break;
            }

            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                status = core_update(c, EV_READ | EV_PERSIST);
                if (status != MC_OK) {
                    log_error("update on c %d failed: %s", c->sd, strerror(errno));
                    conn_set_state(c, CONN_CLOSE);
                    break;
                }
                stop = true;
                break;
            }

            /* otherwise we have a real error, on which we close the connection */
            log_debug(LOG_INFO, "failed to read, and not due to blocking: %s",
                      strerror(errno));
            conn_set_state(c, CONN_CLOSE);
            break;

        case CONN_WRITE:
            /*
             * We want to write out a simple response. If we haven't already,
             * assemble it into a msgbuf list (this will be a single-entry
             * list for TCP or a two-entry list for UDP).
             */
            if (c->iov_used == 0 || (c->udp && c->iov_used == 1)) {
                status = conn_add_iov(c, c->wcurr, c->wbytes);
                if (status != MC_OK) {
                    log_debug(LOG_INFO, "couldn't build response: %s",
                              strerror(errno));
                    conn_set_state(c, CONN_CLOSE);
                    break;
                }
            }

            /* fall through */

        case CONN_MWRITE:
            if (c->udp && c->msg_curr == 0 && conn_build_udp_headers(c) != MC_OK) {
                log_debug(LOG_INFO, "failed to build UDP headers: %s",
                      strerror(errno));
                conn_set_state(c, CONN_CLOSE);
                break;
            }

            switch (core_transmit(c)) {
            case TRANSMIT_COMPLETE:
                if (c->state == CONN_MWRITE) {
                    while (c->ileft > 0) {
                        struct item *it = *(c->icurr);

                        ASSERT((it->flags & ITEM_SLABBED) == 0);
                        item_remove(it);
                        c->icurr++;
                        c->ileft--;
                    }
                    while (c->sleft > 0) {
                        char *suffix = *(c->scurr);

                        cache_free(c->thread->suffix_cache, suffix);
                        c->scurr++;
                        c->sleft--;
                    }

                    conn_set_state(c, CONN_NEW_CMD);
                } else if (c->state == CONN_WRITE) {
                    if (c->write_and_free) {
                        mc_free(c->write_and_free);
                        c->write_and_free = 0;
                    }
                    conn_set_state(c, c->write_and_go);
                } else {
                    log_debug(LOG_INFO, "unexpected state %d", c->state);
                    conn_set_state(c, CONN_CLOSE);
                }
                break;

            case TRANSMIT_INCOMPLETE:
            case TRANSMIT_HARD_ERROR:
                /* continue in state machine */
                break;

            case TRANSMIT_SOFT_ERROR:
                stop = true;
                break;
            }
            break;

        case CONN_CLOSE:
            core_close(c);
            stop = true;
            break;

        default:
            NOT_REACHED();
            break;
        }
    }
}

void
core_event_handler(int sd, short which, void *arg)
{
    struct conn *c = arg;

    c->which = which;

    if (c->sd != sd) {
        log_error("c %d does not match sd %d on event %d", c->sd, sd, which);
        conn_close(c);
        return;
    }

    core_drive_machine(c);
}

static rstatus_t
core_create_inet_socket(int port, int udp)
{
    rstatus_t status;
    int sd;
    struct addrinfo *ai;
    struct addrinfo *next;
    struct addrinfo hints = { .ai_flags = AI_PASSIVE, .ai_family = AF_UNSPEC };
    char port_buf[NI_MAXSERV];
    int error;
    int success = 0;

    hints.ai_socktype = udp ? SOCK_DGRAM : SOCK_STREAM;

    if (port == -1) {
        port = 0;
    }
    snprintf(port_buf, sizeof(port_buf), "%d", port);
    error = getaddrinfo(settings.interface, port_buf, &hints, &ai);
    if (error != 0) {
        log_error("getaddrinfo() failed: %s", gai_strerror(error));
        return MC_ERROR;
    }

    for (next = ai; next != NULL; next = next->ai_next) {
        struct conn *conn;

        sd = socket(next->ai_family, next->ai_socktype, next->ai_protocol);
        if (sd < 0) {
            /*
             * getaddrinfo can return "junk" addresses, we make sure at
             * least one works before erroring.
             */
            continue;
        }

        status = mc_set_nonblocking(sd);
        if (status != MC_OK) {
            log_error("set nonblock on sd %d failed: %s", sd, strerror(errno));
            close(sd);
            continue;
        }

#ifdef IPV6_V6ONLY
        if (next->ai_family == AF_INET6) {
            int flags = 1;
            error = setsockopt(sd, IPPROTO_IPV6, IPV6_V6ONLY, (char *) &flags, sizeof(flags));
            if (error != 0) {
                log_error("set ipv6 on sd %d failed: %s", sd,
                          strerror(errno));
                close(sd);
                continue;
            }
        }
#endif
        status = mc_set_reuseaddr(sd);
        if (status != MC_OK) {
            log_warn("set reuse addr on sd %d failed, ignored: %s", sd,
                      strerror(errno));
        }

        if (udp) {
            mc_maximize_sndbuf(sd);
        }

        status = bind(sd, next->ai_addr, next->ai_addrlen);
        if (status != MC_OK) {
            if (errno != EADDRINUSE) {
                log_error("bind on sd %d failed: %s", sd, strerror(errno));
                close(sd);
                freeaddrinfo(ai);
                return MC_ERROR;
            }
            close(sd);
            continue;
        }

        success++;

        if (!udp && listen(sd, settings.backlog) == -1) {
            log_error("listen on sd %d failed: %s", sd, strerror(errno));
            close(sd);
            freeaddrinfo(ai);
            return MC_ERROR;
        }

        if (udp) {
            int c;

            for (c = 0; c < settings.num_workers; c++) {
                /* this is guaranteed to hit all threads because we round-robin */
                status = thread_dispatch(sd, CONN_READ, EV_READ | EV_PERSIST, 1);
                if (status != MC_OK) {
                    return status;
                }
            }
        } else {
            conn = conn_get(sd, CONN_LISTEN, EV_READ | EV_PERSIST, 1, 0);
            if (conn == NULL) {
                log_error("listen on sd %d failed: %s", sd, strerror(errno));
                return MC_ERROR;
            }
            STAILQ_INSERT_HEAD(&listen_connq, conn, c_tqe);

            status = conn_set_event(conn, main_base);
            if (status != MC_OK) {
                return status;
            }

            log_debug(LOG_NOTICE, "s %d listening", conn->sd);
        }
   }

    freeaddrinfo(ai);

    return success != 0 ? MC_OK : MC_ERROR;
}

static rstatus_t
core_create_unix_socket(char *path, int mask)
{
    rstatus_t status;
    int sd, old_mask;
    struct sockaddr_un addr;
    struct conn *c;

    ASSERT(path != NULL);

    sd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sd < 0) {
        log_error("socket failed: %s", strerror(errno));
        return MC_ERROR;
    }

    status = mc_set_nonblocking(sd);
    if (status != MC_OK) {
        log_error("set noblock on sd %d failed: %s", sd, strerror(errno));
        close(sd);
        return status;
    }

    /*
     * bind() will fail if the path already exist. So, we call unlink()
     * to delete the path, in case it already exists. If it does not
     * exist, unlink() returns error, which we ignore
     */
    unlink(path);

    status = mc_set_reuseaddr(sd);
    if (status != MC_OK) {
        log_warn("set reuse addr on sd %d failed, ignored: %s", sd,
                 strerror(errno));
    }

    status = mc_set_keepalive(sd);
    if (status != MC_OK) {
        log_warn("set keepalive on sd %d failed, ignored: %s", sd,
                 strerror(errno));
    }

    status = mc_unset_linger(sd);
    if (status != MC_OK) {
        log_warn("unset linger on sd %d failed, ignored: %s", sd,
                 strerror(errno));
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    ASSERT(strcmp(addr.sun_path, path) == 0);

    old_mask = umask(~(mask & 0777));

    status = bind(sd, (struct sockaddr *)&addr, sizeof(addr));
    if (status != MC_OK) {
        log_error("bind on sd %d failed: %s", sd, strerror(errno));
        close(sd);
        umask(old_mask);
        return status;
    }

    umask(old_mask);

    status = listen(sd, settings.backlog);
    if (status != MC_OK) {
        log_error("listen on sd %d failed: %s", sd, strerror(errno));
        close(sd);
        return status;
    }

    c = conn_get(sd, CONN_LISTEN, EV_READ | EV_PERSIST, 1, 0);
    if (c == NULL) {
        log_error("listen on sd %d failed: %s", sd, strerror(errno));
        return MC_ERROR;
    }

    status = conn_set_event(c, main_base);
    if (status != MC_OK) {
        conn_put(c);
        return status;
    }

    STAILQ_INSERT_HEAD(&listen_connq, c, c_tqe);

    return MC_OK;
}

static rstatus_t
core_create_socket(void)
{
    rstatus_t status;

    if (settings.socketpath != NULL) {
        return core_create_unix_socket(settings.socketpath, settings.access);
    }

    if (settings.port) {
        status = core_create_inet_socket(settings.port, 0);
        if (status != MC_OK) {
            return status;
        }
    }

    if (settings.udpport) {
        status = core_create_inet_socket(settings.udpport, 1);
        if (status != MC_OK) {
            return status;
        }
    }

    return MC_OK;
}

rstatus_t
core_init(void)
{
    rstatus_t status;

    status = log_init(settings.verbose, settings.log_filename);
    if (status != MC_OK) {
        return status;
    }

    status = signal_init();
    if (status != MC_OK) {
        return status;
    }

    pthread_mutex_init(&accept_lock, NULL);
    STAILQ_INIT(&listen_connq);

    /* initialize main thread libevent instance */
    main_base = event_base_new();
    if (main_base == NULL) {
        return MC_ERROR;
    }

    status = assoc_init();
    if (status != MC_OK) {
        return status;
    }

    conn_init();

    item_init();

    status = slab_init();
    if (status != MC_OK) {
        return status;
    }

    stats_init();

    status = klog_init();
    if (status != MC_OK) {
        return status;
    }

    time_init();

    /* start up worker, dispatcher and aggregator threads */
    status = thread_init(main_base);
    if (status != MC_OK) {
        return status;
    }

    return MC_OK;
}

void
core_deinit(void)
{
    klog_deinit();
}

rstatus_t
core_loop(void)
{
    rstatus_t status;

    status = core_create_socket();
    if (status != MC_OK) {
        return status;
    }

    /* enter the event loop */
    event_base_loop(main_base, 0);

    return MC_OK;
}
