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

#ifndef _MC_THREAD_H_
#define _MC_THREAD_H_

#include <semaphore.h>
#include <pthread.h>
#include <mc_slabs.h>
#include <mc_stats.h>
#include <mc_connection.h>

/* A mapping from a member to its key value in pthread */
struct thread_key {
    pthread_key_t stats_mutex;  /* stats lock */
    pthread_key_t stats_thread; /* thread stats */
    pthread_key_t stats_slabs;  /* slab stats */
    pthread_key_t kbuf;         /* klog buffer */
};

typedef void * (*thread_func_t)(void *);

struct thread_worker {
    pthread_t           tid;               /* thread id */

    struct event_base   *base;             /* libevent handle this thread uses */
    struct event        notify_event;      /* listen event for notify pipe */
    int                 notify_receive_fd; /* receiving end of notify pipe */
    int                 notify_send_fd;    /* sending end of notify pipe */

    struct conn_q       new_cq;            /* new connection q */
    cache_t             *suffix_cache;     /* suffix cache */

    pthread_mutex_t     *stats_mutex;      /* lock for stats update/aggregation */
    struct stats_metric *stats_thread;     /* per-thread thread-level stats */
    struct stats_metric **stats_slabs;     /* per-thread slab-level stats */
    struct kbuf         *kbuf;             /* per-thread klog buffer */
};

/*
 * Aggregator model:
 *
 * The stats aggregator thread periodically wakes itself up and collects
 * collects stats from all worker threads.
 *
 * Clients can collect stats by sending 'stats' command to a twemcache
 * server and get replies in pretty much the same format as before. But
 * data are obtained from the aggregator rather than collected on-the-fly.
 *
 * Note that update granularity is the same as aggregation interval. This
 * may break some tests which rely on instant stats update, our in-house
 * python tests factor this in, and should pass on all future versions.
 *
 * Semaphore:
 *
 * Because now workers can process stats commands while aggregator is updating,
 * a semaphore is utilized to prevent workers from getting inconsistent data
 * (imagine the aggregator has flushed half of its metrics before aggregation
 * when a stats query command comes in). On the other hand, we don't want to
 * discourage concurrent queries if they are all reads, therefore a semaphore
 * instead of a mutex.
 *
 * Configuration:
 *
 * aggregation interval: settings.stats_interval
 * - default: 0.1s, not much overhead with this value
 *   (twctop.rb pulls data every 1 or 2 seconds)
 */
struct thread_aggregator {
    pthread_t               tid;            /* thread id */

    struct event_base       *base;          /* libevent handle for this thread */
    struct event            ev;             /* event object */

    sem_t                   stats_sem;      /* rw sempahore */
    struct timeval          stats_ts;       /* aggregation timestamp */
    struct stats_metric     *stats_thread;  /* aggregated thread-level stats */
    struct stats_metric     **stats_slabs;  /* aggregated slab-level stats */
    struct stats_slab_const stats_slabs_const[SLABCLASS_MAX_IDS];
};

/*
 * The command logger (klogger) model:
 *
 * klogger periodically wakes itself up through libevent,
 * and collects/logs command logs from all worker threads.
 *
 * Configuration:
 * collect interval: settings.klog_intvl
 * - default: 1ms, the structure is lockless so we can afford to wake up often
 * local log file: settings.klog_name
 * - default: NULL
 */
struct thread_klogger {
    pthread_t           tid;        /* unique ID of this thread */
    struct event_base   *base;      /* libevent handle for this thread */
    struct event        ev;         /* event object */
};

void *thread_get(pthread_key_t key);

rstatus_t thread_init(struct event_base *main_base);
void thread_deinit(void);
rstatus_t thread_dispatch(int sd, conn_state_t state, int ev_flags, int udp);

#endif
