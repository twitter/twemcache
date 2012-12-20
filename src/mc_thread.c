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

#include <stdlib.h>

#include <mc_core.h>

extern struct settings settings;

struct thread_worker *threads;       /* worker threads */
struct thread_aggregator aggregator; /* aggregator thread */
struct thread_klogger klogger;       /* klogger thread */
struct thread_key keys;              /* thread-locak keys */
static int last_thread;              /* last thread we assigned connection to most recently */

static int init_count;               /* # worker threads inited */
static pthread_mutex_t init_lock;    /* init threads lock */
static pthread_cond_t init_cond;     /* init threads condvar */

/*
 * Get the pointer to a member of the current thread-local data.
 */
void *
thread_get(pthread_key_t key)
{
    void *ptr;

    ptr = pthread_getspecific(key);
    ASSERT(ptr != NULL);

    return ptr;
}

static rstatus_t
thread_create(thread_func_t func, void *arg)
{
    pthread_t thread;
    err_t err;

    err = pthread_create(&thread, NULL, func, arg);
    if (err != 0) {
        log_error("pthread create failed: %s", strerror(err));
        return MC_ERROR;
    }

    return MC_OK;
}

/*
 * Setup a thread's stats-related fields.
 *
 * Return success status as a boolean.
 */
static rstatus_t
thread_setup_stats(struct thread_worker *t)
{
    err_t err;

    t->stats_mutex = mc_alloc(sizeof(*t->stats_mutex));
    if (t->stats_mutex == NULL) {
        return MC_ENOMEM;
    }

    err = pthread_mutex_init(t->stats_mutex, NULL);
    if (err != 0) {
        log_error("pthread mutex init failed: %s", strerror(err));
        return MC_ERROR;
    }

    t->stats_thread = stats_thread_init();
    if (t->stats_thread == NULL) {
        log_error("stats thread init failed: %s", strerror(errno));
        pthread_mutex_destroy(t->stats_mutex);
        return MC_ERROR;
    }

    t->stats_slabs = stats_slabs_init();
    if (t->stats_slabs == NULL) {
        log_error("stats slabs init failed: %s", strerror(errno));
        pthread_mutex_destroy(t->stats_mutex);
        stats_thread_deinit(t->stats_thread);
        return MC_ERROR;
    }

    return MC_OK;
}

/*
 * Map keys to their location in thread's local data structure.
 * Call this after the thread local data have been properly set up.
 *
 * Set the thread member keys.
 *
 * Return success status as a boolean.
 */
static rstatus_t
thread_setkeys(struct thread_worker *t)
{
    err_t err;

    err = pthread_setspecific(keys.stats_mutex, t->stats_mutex);
    if (err != 0) {
        log_error("pthread setspecific failed: %s", strerror(err));
        return MC_ERROR;
    }

    err = pthread_setspecific(keys.stats_thread, t->stats_thread);
    if (err != 0) {
        log_error("pthread setspecific failed: %s", strerror(err));
        return MC_ERROR;
    }

    err = pthread_setspecific(keys.stats_slabs, t->stats_slabs);
    if (err != 0) {
        log_error("pthread setspecific failed: %s", strerror(err));
        return MC_ERROR;
    }

    err = pthread_setspecific(keys.kbuf, t->kbuf);
    if (err != 0) {
        log_error("pthread setspecific failed: %s", strerror(err));
        return MC_ERROR;
    }

    return MC_OK;
}

/*
 * Worker thread new connection event loop
 *
 * Processes an incoming "handle a new connection" item. This is called when
 * input arrives on the libevent wakeup pipe. Each libevent instance has a
 * wakeup pipe, which other threads (dispatcher thread) uses to signal that
 * they've put a new connection on its queue.
 */
static void
thread_libevent_process(int fd, short which, void *arg)
{
    struct thread_worker *t = arg;
    char buf[1];
    ssize_t n;
    struct conn *c;
    int status;

    n = read(fd, buf, 1);
    if (n < 0) {
        log_warn("read from notify pipe %d failed: %s", fd, strerror(errno));
    }

    c = conn_cq_pop(&t->new_cq);
    if (c == NULL) {
        return;
    }

    c->thread = t;

    status = conn_set_event(c, t->base);
    if (status != MC_OK) {
        close(c->sd);
        conn_put(c);
    }
}

/*
 * Worker thread main event loop
 */
static void *
thread_worker_main(void *arg)
{
    struct thread_worker *t = arg;
    rstatus_t status;

    /*
     * Any per-thread setup can happen here; thread_init() will block until
     * all threads have finished initializing.
     */

    status = thread_setkeys(t);
    if (status != MC_OK) {
        exit(1);
    }

    pthread_mutex_lock(&init_lock);
    t->tid = pthread_self();
    init_count++;
    pthread_cond_signal(&init_cond);
    pthread_mutex_unlock(&init_lock);

    event_base_loop(t->base, 0);

    return NULL;
}

/*
 * Setup a worker thread
 */
static rstatus_t
thread_setup(struct thread_worker *t)
{
    size_t suffix_size;
    rstatus_t status;

    t->base = event_base_new();
    if (t->base == NULL) {
        log_error("event init failed: %s", strerror(errno));
        return MC_ERROR;
    }

    /* listen for notifications from other threads */
    event_set(&t->notify_event, t->notify_receive_fd, EV_READ | EV_PERSIST,
              thread_libevent_process, t);
    event_base_set(t->base, &t->notify_event);

    status = event_add(&t->notify_event, 0);
    if (status < 0) {
        log_error("event add failed: %s", strerror(errno));
        return MC_ERROR;
    }

    status = thread_setup_stats(t);
    if (status != MC_OK) {
        return status;
    }

    t->kbuf = klog_buf_create();
    if (t->kbuf == NULL) {
        log_error("klog buf create failed: %s", strerror(errno));
        return MC_ENOMEM;
    }

    conn_cq_init(&t->new_cq);

    suffix_size = settings.use_cas ? (CAS_SUFFIX_SIZE + SUFFIX_SIZE + 1) :
                  (SUFFIX_SIZE + 1);
    t->suffix_cache = cache_create("suffix", suffix_size, sizeof(char *));
    if (t->suffix_cache == NULL) {
        log_error("cache create of suffix cache failed: %s", strerror(errno));
        return MC_ENOMEM;
    }

    return MC_OK;
}


/*
 * Aggregator thread event loop
 */
static void
thread_aggregate_stats(int fd, short ev, void *arg)
{
    if (settings.stats_agg_intvl.tv_sec >= 0) {
        evtimer_add(&aggregator.ev, &settings.stats_agg_intvl);
        stats_aggregate();
    } else {
        /* if aggregation is turned off, come back & check in 5 seconds */
        struct timeval sleep;
        sleep.tv_sec = 5;
        sleep.tv_usec = 0;
        evtimer_add(&aggregator.ev, &sleep);
    }
}

/*
 * Aggregator thread main
 */
static void *
thread_aggregator_main(void *arg)
{
    pthread_mutex_lock(&init_lock);
    aggregator.tid = pthread_self();
    init_count++;
    pthread_cond_signal(&init_cond);
    pthread_mutex_unlock(&init_lock);

    event_base_dispatch(aggregator.base);

    return NULL;
}

/*
 * Setup aggregator thread
 */
static rstatus_t
thread_setup_aggregator(void)
{
    int status;

    aggregator.base = event_base_new();
    if (NULL == aggregator.base) {
        log_error("event init failed: %s", strerror(errno));
        return MC_ERROR;
    }

    /* +1 because we also aggregate from dispatcher */
    sem_init(&aggregator.stats_sem, 0, settings.num_workers + 1);

    aggregator.stats_thread = stats_thread_init();
    if (aggregator.stats_thread == NULL) {
        sem_destroy(&aggregator.stats_sem);
        return MC_ERROR;
    }

    aggregator.stats_slabs = stats_slabs_init();
    if (aggregator.stats_slabs == NULL) {
        sem_destroy(&aggregator.stats_sem);
        stats_thread_deinit(aggregator.stats_thread);
        return MC_ERROR;
    }

    evtimer_set(&aggregator.ev, thread_aggregate_stats, NULL);
    event_base_set(aggregator.base, &aggregator.ev);

    status = evtimer_add(&aggregator.ev, &settings.stats_agg_intvl);
    if (status < 0) {
        log_error("evtimer add failed: %s", strerror(errno));
        return status;
    }

    return MC_OK;
}

/*
 * Klogger thread event loop
 */
static void
thread_klogger_collect(int fd, short ev, void *arg)
{
    if (klog_enabled()) {
        evtimer_add(&klogger.ev, &settings.klog_intvl);
        klog_collect();
    } else {
        /*
         * If logging is turned off, come back & check in 0.01 seconds. We
         * use a small value so that we don't overrun the buffer when
         * klogger is turned on
         */
        struct timeval sleep;
        sleep.tv_sec = 0;
        sleep.tv_usec = 10000;
        evtimer_add(&klogger.ev, &sleep);
    }
}

/*
 * Klogger thread main
 */
static void *
thread_klogger_main(void *arg)
{
    pthread_mutex_lock(&init_lock);
    klogger.tid = pthread_self();
    init_count++;
    pthread_cond_signal(&init_cond);
    pthread_mutex_unlock(&init_lock);

    event_base_dispatch(klogger.base);

    return NULL;
}

/*
 * Setup klogger thread
 */
static rstatus_t
thread_setup_klogger(void)
{
    int status;

    klogger.base = event_base_new();
    if (NULL == klogger.base) {
        log_error("event init failed: %s", strerror(errno));
        return MC_ERROR;
    }

    evtimer_set(&klogger.ev, thread_klogger_collect, NULL);
    event_base_set(klogger.base, &klogger.ev);

    status = evtimer_add(&klogger.ev, &settings.klog_intvl);
    if (status < 0) {
        log_error("evtimer add failed: %s", strerror(errno));
        return status;
    }

    return MC_OK;
}


/*
 * Dispatches a new connection to another thread. This is only ever called
 * from the main thread, either during initialization (for UDP) or because
 * of an incoming connection.
 */
rstatus_t
thread_dispatch(int sd, conn_state_t state, int ev_flags, int udp)
{
    int tid;
    struct thread_worker *t;
    ssize_t n;
    struct conn *c;
    int rsize;

    rsize = udp ? UDP_BUFFER_SIZE : TCP_BUFFER_SIZE;

    c = conn_get(sd, state, ev_flags, rsize, udp);
    if (c == NULL) {
        return MC_ENOMEM;
    }

    mc_resolve_peer(c->sd, c->peer, sizeof(c->peer));

    tid = (last_thread + 1) % settings.num_workers;
    t = threads + tid;
    last_thread = tid;

    conn_cq_push(&t->new_cq, c);

    n = write(t->notify_send_fd, "", 1);
    if (n != 1) {
        log_warn("write to notify pipe %d failed: %s", t->notify_send_fd,
                 strerror(errno));
        return MC_ERROR;
    }

    if (state == CONN_NEW_CMD) {
        log_debug(LOG_NOTICE, "accepted c %d from '%s' on tid %d", c->sd,
                  c->peer, tid);
    }

    return MC_OK;
}

rstatus_t
thread_init(struct event_base *main_base)
{
    rstatus_t status;
    err_t err;
    int nworkers = settings.num_workers;
    struct thread_worker *dispatcher;
    int i;

    init_count = 0;
    pthread_mutex_init(&init_lock, NULL);
    pthread_cond_init(&init_cond, NULL);

    last_thread = -1;

    /* dispatcher takes the extra (last) slice of thread descriptor */
    threads = mc_zalloc(sizeof(*threads) * (1 + nworkers));
    if (threads == NULL) {
        return MC_ENOMEM;
    }
    /* keep data of dispatcher close to worker threads for easy aggregation */
    dispatcher = &threads[nworkers];

    /* create keys for common members of thread_worker. */
    err = pthread_key_create(&keys.stats_mutex, NULL);
    if (err != 0) {
        log_error("pthread key create failed: %s", strerror(err));
        return MC_ERROR;
    }

    err = pthread_key_create(&keys.stats_thread, NULL);
    if (err != 0) {
        log_error("pthread key create failed: %s", strerror(err));
        return MC_ERROR;
    }

    err = pthread_key_create(&keys.stats_slabs, NULL);
    if (err != 0) {
        log_error("pthread key create failed: %s", strerror(err));
        return MC_ERROR;
    }

    err = pthread_key_create(&keys.kbuf, NULL);
    if (err != 0) {
        log_error("pthread key create failed: %s", strerror(err));
        return MC_ERROR;
    }

    dispatcher->base = main_base;
    dispatcher->tid = pthread_self();

    status = thread_setup_stats(dispatcher);
    if (status != MC_OK) {
        return status;
    }

    status = thread_setkeys(dispatcher);
    if (status != MC_OK) {
        return status;
    }

    for (i = 0; i < nworkers; i++) {
        int fds[2];
        status = pipe(fds);
        if (status < 0) {
            log_error("pipe failed: %s", strerror(errno));
            return status;
        }

        threads[i].notify_receive_fd = fds[0];
        threads[i].notify_send_fd = fds[1];

        status = thread_setup(&threads[i]);
        if (status != MC_OK) {
            return status;
        }
    }

    /* create worker threads after we've done all the libevent setup */
    for (i = 0; i < nworkers; i++) {
        status = thread_create(thread_worker_main, &threads[i]);
        if (status != MC_OK) {
            return status;
        }
    }

    /* wait for all the workers to set themselves up */
    pthread_mutex_lock(&init_lock);
    while (init_count < nworkers) {
        pthread_cond_wait(&init_cond, &init_lock);
    }
    pthread_mutex_unlock(&init_lock);

    /* for stats module */

    /* setup thread data structures */
    status = thread_setup_aggregator();
    if (status != MC_OK) {
        return status;
    }

    /* create thread */
    status = thread_create(thread_aggregator_main, NULL);
    if (status != MC_OK) {
        return status;
    }

    /* for klogger */

    /* Setup thread data structure */
    status = thread_setup_klogger();
    if (status != MC_OK) {
        return status;
    }

    /* create thread */
    status = thread_create(thread_klogger_main, NULL);
    if (status != MC_OK) {
        return status;
    }

    /* wait for all the workers and dispatcher to set themselves up */
    pthread_mutex_lock(&init_lock);
    while (init_count < nworkers + 2) { /* +2: aggregator & klogger */
        pthread_cond_wait(&init_cond, &init_lock);
    }
    pthread_mutex_unlock(&init_lock);

    return MC_OK;
}

void
thread_deinit(void)
{
}
