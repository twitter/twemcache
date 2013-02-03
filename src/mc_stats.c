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
#include <sys/resource.h>

#include <mc_core.h>

extern struct settings settings;
extern struct thread_worker *threads;
extern struct thread_aggregator aggregator;
extern struct thread_key keys;
extern struct item_tqh item_lruq[];
extern uint8_t slabclass_max_id;
extern struct slabclass slabclass[];
extern pthread_mutex_t cache_lock;

#define STATS_KEY_LEN       128
#define STATS_VAL_LEN       128

#define STATS_BUCKET_SIZE   32

#define MAKEARRAY(_name, _type, _desc)  \
    { .type = _type, .value = { .counter = 0LL }, .name = #_name },
static struct stats_metric stats_tmetrics[] = {
    STATS_THREAD_METRICS(MAKEARRAY)
};

static struct stats_metric stats_smetrics[] = {
    STATS_SLAB_METRICS(MAKEARRAY)
};
#undef MAKEARRAY

static int num_updaters; /* # threads that update stats */

struct stats_desc {
    char *name; /* stats name */
    char *desc; /* stats description */
};

#define MAKEARRAY(_name, _type, _desc)  \
    { .name = #_name, .desc = _desc },
static struct stats_desc stats_tdesc[] = {
    STATS_THREAD_METRICS(MAKEARRAY)
};

static struct stats_desc stats_sdesc[] = {
    STATS_SLAB_METRICS(MAKEARRAY)
};
#undef MAKEARRAY

void
stats_describe(void)
{
    int i;

    log_stderr("per thread stats:");
    for (i = 0; i < NELEMS(stats_tdesc); i++) {
        log_stderr("  %-20s\"%s\"", stats_tdesc[i].name, stats_tdesc[i].desc);
    }

    log_stderr("");

    log_stderr("per slab, per thread stats:");
    for (i = 0; i < NELEMS(stats_sdesc); i++) {
        log_stderr("  %-20s\"%s\"", stats_sdesc[i].name, stats_sdesc[i].desc);
    }
}

bool
stats_enabled(void)
{
    if (MC_DISABLE_STATS) {
        return false;
    }

    /* aggregation has been disabled? */
    if (settings.stats_agg_intvl.tv_sec < 0) {
        return false;
    }

    return true;
}

void
stats_set_interval(long interval)
{
    settings.stats_agg_intvl.tv_sec = interval / 1000000;
    settings.stats_agg_intvl.tv_usec = interval % 1000000;
}

static void
stats_print(struct conn *c, const char *name, const char *fmt, ...)
{
    char val_str[STATS_VAL_LEN];
    int vlen;
    va_list ap;

    ASSERT(c != NULL);
    ASSERT(name != NULL);
    ASSERT(fmt != NULL);

    va_start(ap, fmt);
    vlen = vsnprintf(val_str, sizeof(val_str) - 1, fmt, ap);
    va_end(ap);

    stats_append(c, name, strlen(name), val_str, vlen);
}

static void
stats_metric_init(struct stats_metric *metric)
{
    switch (metric->type) {
    case STATS_TIMESTAMP:
        metric->value.timestamp = 0;
        break;

    case STATS_COUNTER:
        metric->value.counter = 0LL;
        break;

    case STATS_GAUGE:
        metric->value.gauge.t = 0LL;
        metric->value.gauge.b = 0LL;
        break;

    default:
        NOT_REACHED();
    }
}

static void
stats_template_init(void)
{
    uint32_t i;

    for (i = 0; i < STATS_THREAD_LEN; ++i) {
        stats_metric_init(&stats_tmetrics[i]);
    }

    for (i = 0; i < STATS_SLAB_LEN; ++i) {
        stats_metric_init(&stats_smetrics[i]);
    }
}

void
_stats_thread_incr(stats_tmetric_t name)
{
    _stats_thread_incr_by(name, 1);
}

void
_stats_thread_decr(stats_tmetric_t name)
{
    _stats_thread_decr_by(name, 1);
}

void
_stats_thread_incr_by(stats_tmetric_t name, int64_t delta)
{
    pthread_mutex_t *stats_mutex = thread_get(keys.stats_mutex);
    struct stats_metric *stats_thread = thread_get(keys.stats_thread);
    struct stats_metric *metric = &stats_thread[name];

    pthread_mutex_lock(stats_mutex);

    switch (metric->type) {
    case STATS_COUNTER:
        metric->value.counter += delta;
        break;

    case STATS_GAUGE:
        metric->value.gauge.t += delta;
        break;

    default:
        NOT_REACHED();
        break;
    }

    pthread_mutex_unlock(stats_mutex);
}

void
_stats_thread_decr_by(stats_tmetric_t name, int64_t delta)
{
    pthread_mutex_t *stats_mutex = thread_get(keys.stats_mutex);
    struct stats_metric *stats_thread = thread_get(keys.stats_thread);
    struct stats_metric *metric = &stats_thread[name];

    ASSERT(metric->type == STATS_GAUGE);

    pthread_mutex_lock(stats_mutex);
    metric->value.gauge.b += delta;
    pthread_mutex_unlock(stats_mutex);
}

void
_stats_slab_settime(uint8_t cls_id, stats_smetric_t name, rel_time_t val)
{
    pthread_mutex_t *stats_mutex = thread_get(keys.stats_mutex);
    struct stats_metric **stats_slabs = thread_get(keys.stats_slabs);
    struct stats_metric *metric = &stats_slabs[cls_id][name];

    ASSERT(metric != NULL);
    ASSERT(cls_id >= SLABCLASS_MIN_ID && cls_id <= slabclass_max_id);
    ASSERT(metric->type == STATS_TIMESTAMP);

    pthread_mutex_lock(stats_mutex);
    metric->value.timestamp = val;
    pthread_mutex_unlock(stats_mutex);
}

void
_stats_slab_incr(uint8_t cls_id, stats_smetric_t name)
{
    _stats_slab_incr_by(cls_id, name, 1);
}

void
_stats_slab_decr(uint8_t cls_id, stats_smetric_t name)
{
    _stats_slab_decr_by(cls_id, name, 1);
}

void
_stats_slab_incr_by(uint8_t cls_id, stats_smetric_t name, int64_t delta)
{
    pthread_mutex_t *stats_mutex = thread_get(keys.stats_mutex);
    struct stats_metric **stats_slabs = thread_get(keys.stats_slabs);
    struct stats_metric *metric = &stats_slabs[cls_id][name];

    pthread_mutex_lock(stats_mutex);

    switch (metric->type) {
    case STATS_COUNTER:
        metric->value.counter += delta;
        break;

    case STATS_GAUGE:
        metric->value.gauge.t += delta;
        break;

    default:
        NOT_REACHED();
    }

    pthread_mutex_unlock(stats_mutex);
}

void
_stats_slab_decr_by(uint8_t cls_id, stats_smetric_t name, int64_t delta)
{
    pthread_mutex_t *stats_mutex = thread_get(keys.stats_mutex);
    struct stats_metric **stats_slabs = thread_get(keys.stats_slabs);
    struct stats_metric *metric = &stats_slabs[cls_id][name];

    ASSERT(metric->type == STATS_GAUGE);

    pthread_mutex_lock(stats_mutex);
    metric->value.gauge.b += delta;
    pthread_mutex_unlock(stats_mutex);
}

static int64_t
stats_metric_val(struct stats_metric *metric)
{
    int64_t delta;

    switch (metric->type) {
    case STATS_COUNTER:
        return metric->value.counter;

    case STATS_GAUGE:
        /*
         * Note on negative values: in the aggregator-worker stats collection
         * model, aggregator walks through worker threads to gather local
         * metrics, locking thread struct one at a time. For gauges (both incr
         * and decr are allowed), it could happen that aggregator misses the
         * "incr" activity in thread A but was in time to catch "decr" in
         * thread B, getting a negative snapshot value for the gauge metric.
         * In theory gauge value is non-negative. The reason that we may see
         * them is due to an inaccuracy (in exchange of efficiency) by design.
         *
         * When we see a negative value, 0 should be reported instead.
         */

        delta = metric->value.gauge.t - metric->value.gauge.b;
        return delta >= 0 ? delta : 0;

    case STATS_TIMESTAMP:
        if (metric->value.timestamp > 0) {
            return (int64_t)time_now() - metric->value.timestamp;
        } else {
            /* time_now() always positive, 0 means ts hasn't been updated */
            return -1;
        }

    default:
        NOT_REACHED();
        return -1;
    }
}

/*
 * Update the first stats metric1 with the second stats metric2
 * depending on the metric type.
 */
static void
stats_metric_update(struct stats_metric *metric1,
                    const struct stats_metric *metric2)
{
    ASSERT(metric1 != NULL);
    ASSERT(metric2 != NULL);
    ASSERT(metric1->type == metric2->type);

    switch (metric1->type) {
    case STATS_TIMESTAMP:
        if (metric1->value.timestamp < metric2->value.timestamp) {
            /* newer timestamp wins */
            metric1->value.timestamp = metric2->value.timestamp;
        }
        return;

    case STATS_COUNTER:
        metric1->value.counter += metric2->value.counter;
        return;

    case STATS_GAUGE:
        metric1->value.gauge.t += metric2->value.gauge.t;
        metric1->value.gauge.b += metric2->value.gauge.b;
        return;

    default:
        NOT_REACHED();
    }
}

/*
 * Reset thread metrics by copying the template again.
 */
static void
stats_thread_reset(struct stats_metric *stats_thread)
{
    ASSERT(stats_thread != NULL);
    memcpy(stats_thread, stats_tmetrics, sizeof(stats_tmetrics));
}

/*
 * Reset slab metrics by copying the template again.
 */
static void
stats_slab_reset(struct stats_metric *stats_slab)
{
    ASSERT(stats_slab != NULL);
    memcpy(stats_slab, stats_smetrics, sizeof(stats_smetrics));
}

/*
 * Initialize static slab stats (and be done with it).
 */
static void
stats_slab_getstatic(struct stats_slab_const *stats)
{
    uint8_t i;

    ASSERT(stats != NULL);

    for (i = SLABCLASS_MIN_ID; i <= slabclass_max_id; i++) {
        struct slabclass *p = &slabclass[i];
        stats[i].chunk_size = p->size;
        stats[i].items_perslab = p->nitem;
    }
}

/*
 * Initiate slab stats - alloc & initialize metrics for each
 * slab class
 */
struct stats_metric **
stats_slabs_init(void)
{
    struct stats_metric **stats_slabs;
    struct stats_metric *metric_array2; /* for all slabs, 2-D array */
    uint32_t i;

    stats_slabs = mc_alloc(SLABCLASS_MAX_IDS *
                           sizeof(struct stats_metric *));
    if (stats_slabs == NULL) {
        return NULL;
    }

    metric_array2 = mc_alloc(SLABCLASS_MAX_IDS * sizeof(stats_smetrics));
    if (metric_array2 == NULL) {
        mc_free(stats_slabs);
        return NULL;
    }

    for (i = 0; i <= SLABCLASS_MAX_ID; ++i) { /* cid 0: aggregated */
        /* slicing the 2D array */
        stats_slabs[i] = &metric_array2[STATS_SLAB_LEN * i];
        /* initialize each slab */
        memcpy(stats_slabs[i], stats_smetrics, sizeof(stats_smetrics));
    }

    return stats_slabs;
}

/*
 * Initiate thread stats - alloc & initialize thread local
 * metrics
 */
struct stats_metric *
stats_thread_init(void)
{
    struct stats_metric *stats_thread;

    stats_thread = mc_alloc(sizeof(stats_tmetrics));
    if (stats_thread == NULL) {
        return NULL;
    }

    memcpy(stats_thread, stats_tmetrics, sizeof(stats_tmetrics));

    return stats_thread;
}

void
stats_thread_deinit(struct stats_metric *stats_thread)
{
    mc_free(stats_thread);
}

/*
 * Initialize the stats subsystem (other than thread & slab)
 */
void
stats_init(void)
{
    stats_template_init();
    stats_slab_getstatic(aggregator.stats_slabs_const);
    num_updaters = settings.num_workers + 1; /* +1 to include dispatcher */
}

void
stats_deinit(void)
{
}

/*
 * Aggregate thread-local metrics - thread and slab stats, over all
 * threads
 */
void
_stats_aggregate(void)
{
    uint32_t i, j;
    uint8_t cid; /* slab class id */

    log_debug(LOG_PVERB, "aggregating stats at time %u", time_now());

    /* grab semaphore counts from all workers and dispatcher as well */
    for (i = 0; i < num_updaters; ++i) {
        sem_wait(&aggregator.stats_sem);
    }
    gettimeofday(&aggregator.stats_ts, NULL);

    /* reset aggregated metrics */
    stats_thread_reset(aggregator.stats_thread);

    /* this is to reset connection related stats before aggregation */
    memcpy(aggregator.stats_thread, stats_tmetrics,
           5 * sizeof(struct stats_metric));
    for (cid = 0; cid <= SLABCLASS_MAX_ID; ++cid) { /* cid 0: aggregated */
        stats_slab_reset(aggregator.stats_slabs[cid]);
    }

    /* aggregate over workers and dispatcher */
    for (i = 0; i < num_updaters; ++i) {
        struct stats_metric *stats_thread = threads[i].stats_thread;
        pthread_mutex_lock(threads[i].stats_mutex);

        /* thread level */
        for (j = 0; j < STATS_THREAD_LEN; ++j) {
            stats_metric_update(&aggregator.stats_thread[j], &stats_thread[j]);
        }

        /* slab level */
        for (cid = SLABCLASS_MIN_ID; cid <= SLABCLASS_MAX_ID; ++cid) {
            struct stats_metric *stats_slab = threads[i].stats_slabs[cid];

            for (j = 0; j < STATS_SLAB_LEN; ++j) {
                stats_metric_update(&aggregator.stats_slabs[cid][j],
                                    &stats_slab[j]);
            }
        }

        pthread_mutex_unlock(threads[i].stats_mutex);
    }

    /* sum slab level stats over all slab classes and store in slab class 0 */
    for (j = 0; j < STATS_SLAB_LEN; ++j) {
        for (cid = SLABCLASS_MIN_ID; cid < SLABCLASS_MAX_ID; ++cid) {
            stats_metric_update(&aggregator.stats_slabs[0][j],
                                &aggregator.stats_slabs[cid][j]);
        }
    }

    /* restore all semaphore counts */
    for (i = 0; i < num_updaters; ++i) {
        sem_post(&aggregator.stats_sem);
    }
}

/*
 * Process command "stats slabs\r\n"
 */
void
stats_slabs(struct conn *c)
{
    uint32_t i;
    uint8_t cid;

    /* Get the per-thread stats which contain some interesting aggregates */

    sem_wait(&aggregator.stats_sem);

    for (cid = SLABCLASS_MIN_ID; cid <= slabclass_max_id; ++cid) {
        struct stats_slab_const *slabconst = &aggregator.stats_slabs_const[cid];
        struct stats_metric *slab = aggregator.stats_slabs[cid];
        char key_str[STATS_KEY_LEN];
        char val_str[STATS_VAL_LEN];
        uint32_t klen = 0, vlen = 0;

        klen = snprintf(key_str, STATS_KEY_LEN, "%d:%s", cid, "chunk_size");
        vlen = snprintf(val_str, STATS_VAL_LEN, "%"PRIu64, slabconst->chunk_size);
        stats_append(c, key_str, klen, val_str, vlen);

        klen = snprintf(key_str, STATS_KEY_LEN, "%d:%s", cid, "chunks_per_page");
        vlen = snprintf(val_str, STATS_VAL_LEN, "%"PRIu64, slabconst->items_perslab);
        stats_append(c, key_str, klen, val_str, vlen);

        for (i = 0; i < STATS_SLAB_LEN; ++i) {
            klen = snprintf(key_str, STATS_KEY_LEN, "%d:%s", cid, slab[i].name);
            vlen = snprintf(val_str, STATS_VAL_LEN, "%"PRId64,
                            stats_metric_val(&slab[i]));
            stats_append(c, key_str, klen, val_str, vlen);
        }
    }

    sem_post(&aggregator.stats_sem);
    stats_append(c, NULL, 0, NULL, 0);
}

/*
 * Process command "stats sizes\r\n". Dumps a list of objects of each size
 * in 32-byte increments
 */
void
stats_sizes(void *c)
{
    uint32_t *histogram;
    int num_buckets;

    num_buckets = settings.slab_size / STATS_BUCKET_SIZE + 1;
    histogram = mc_zalloc(sizeof(int) * num_buckets);

    pthread_mutex_lock(&cache_lock);
    if (histogram != NULL) {
        uint32_t i;

        /* build the histogram */
        for (i = SLABCLASS_MIN_ID; i <= slabclass_max_id; i++) {
            struct item *iter;

            TAILQ_FOREACH(iter, &item_lruq[i], i_tqe) {
                int ntotal = item_size(iter);
                int bucket = (ntotal - 1) / STATS_BUCKET_SIZE + 1;
                ASSERT(bucket < num_buckets);
                histogram[bucket]++;
            }
        }

        /* write the buffer */
        for (i = 0; i < num_buckets; i++) {
            if (histogram[i] != 0) {
                char key[8];
                ASSERT(snprintf(key, sizeof(key), "%d", i * 32) < sizeof(key));
                stats_print(c, key, "%u", histogram[i]);
            }
        }
        mc_free(histogram);
    }
    stats_append(c, NULL, 0, NULL, 0);
    pthread_mutex_unlock(&cache_lock);
}

/*
 * Process command "stats settings\r\n".
 */
void
stats_settings(void *c)
{
    stats_print(c, "prealloc", "%u", (unsigned int)settings.prealloc);
    stats_print(c, "lock_page", "%u", (unsigned int)settings.lock_page);
    stats_print(c, "accepting_conns", "%u", (unsigned int)settings.accepting_conns);
    stats_print(c, "daemonize", "%u", (unsigned int)settings.daemonize);
    stats_print(c, "max_corefile", "%u", (unsigned int)settings.max_corefile);
    stats_print(c, "cas_enabled", "%u", (unsigned int)settings.use_cas);
    stats_print(c, "num_workers", "%d", settings.num_workers);
    stats_print(c, "reqs_per_event", "%d", settings.reqs_per_event);
    stats_print(c, "oldest", "%u", settings.oldest_live);
    stats_print(c, "log_filename", "%s", settings.log_filename);
    stats_print(c, "verbosity", "%d", settings.verbose);
    stats_print(c, "maxconns", "%d", settings.maxconns);
    stats_print(c, "tcpport", "%d", settings.port);
    stats_print(c, "udpport", "%d", settings.udpport);
    stats_print(c, "interface", "%s", settings.interface ? settings.interface : "");
    stats_print(c, "domain_socket", "%s",
                settings.socketpath ? settings.socketpath : "NULL");
    stats_print(c, "umask", "%o", settings.access);
    stats_print(c, "tcp_backlog", "%d", settings.backlog);
    stats_print(c, "evictions", "%d", settings.evict_opt);
    stats_print(c, "growth_factor", "%.2f", settings.factor);
    stats_print(c, "maxbytes", "%zu", settings.maxbytes);
    stats_print(c, "chunk_size", "%d", settings.chunk_size);
    stats_print(c, "slab_size", "%d", settings.slab_size);
    stats_print(c, "username", "%s", settings.username);
    stats_print(c, "stats_agg_intvl", "%10.6f", settings.stats_agg_intvl.tv_sec +
                1.0 * settings.stats_agg_intvl.tv_usec / 1000000);
    stats_print(c, "hash_power", "%d", settings.hash_power);
    stats_print(c, "klog_name", "%s", settings.klog_name);
    stats_print(c, "klog_sampling_rate", "%d", settings.klog_sampling_rate);
    stats_print(c, "klog_entry", "%d", settings.klog_entry);
    stats_print(c, "klog_intvl", "%10.6f", settings.klog_intvl.tv_sec +
                1.0 * settings.klog_intvl.tv_usec / 1000000);
}

/*
 * Process command "stats\r\n".
 */
void
stats_default(struct conn *c)
{
    struct stats_metric *slab;
    struct stats_metric *thread;
    uint32_t i;
    struct rusage usage;
    rel_time_t uptime;
    long int abstime;

    uptime = time_now();
    abstime = (long int)time_started() + time_now();
    slab = aggregator.stats_slabs[0];
    thread = aggregator.stats_thread;

    getrusage(RUSAGE_SELF, &usage);

    stats_print(c, "pid", "%d", (int)settings.pid);
    stats_print(c, "uptime", "%u", uptime);
    stats_print(c, "time", "%ld", abstime);
    stats_print(c, "aggregate_ts", "%ld.%06ld", aggregator.stats_ts.tv_sec,
                aggregator.stats_ts.tv_usec);
    stats_print(c, "version", "%02d%02d%02d", MC_VERSION_MAJOR,
                MC_VERSION_MINOR, MC_VERSION_PATCH);
    stats_print(c, "pointer_size", "%zu", 8 * sizeof(void *));
    stats_print(c, "rusage_user", "%ld.%06ld", usage.ru_utime.tv_sec,
                usage.ru_utime.tv_usec);
    stats_print(c, "rusage_system", "%ld.%06ld", usage.ru_stime.tv_sec,
                usage.ru_stime.tv_usec);
    stats_print(c, "heap_curr", "%ld", mc_malloc_used_memory());

    sem_wait(&aggregator.stats_sem);

    /* thread-level metrics */
    for (i = 0; i < STATS_THREAD_LEN; ++i) {
        stats_print(c, thread[i].name, "%"PRId64, stats_metric_val(&thread[i]));
    }

    /* slab-level metrics, summary */
    for (i = 0; i < STATS_SLAB_LEN; ++i) {
        stats_print(c, slab[i].name, "%"PRId64, stats_metric_val(&slab[i]));
    }

    sem_post(&aggregator.stats_sem);
}

static bool
stats_buf_grow(struct conn *c, size_t needed)
{
    size_t nsize = c->stats.size;
    size_t available = nsize - c->stats.offset;
    bool rv = true;

    /* special case: No buffer -- need to allocate fresh */
    if (c->stats.buffer == NULL) {
        nsize = 1024;
        available = c->stats.size = c->stats.offset = 0;
    }

    while (needed > available) {
        ASSERT(nsize > 0);
        nsize = nsize << 1;
        available = nsize - c->stats.offset;
    }

    if (nsize != c->stats.size) {
        char *ptr = mc_realloc(c->stats.buffer, nsize);
        if (ptr) {
            c->stats.buffer = ptr;
            c->stats.size = nsize;
        } else {
            rv = false;
        }
    }

    return rv;
}

void
stats_append(struct conn *c, const char *key, uint16_t klen,
             char *val, uint32_t vlen)
{
    size_t needed;

    /* value without a key is invalid */
    if (klen == 0 && vlen > 0) {
        return ;
    }

    needed = vlen + klen + 10; /* 10 == "STAT = \r\n" */
    if (!stats_buf_grow(c, needed)) {
        return;
    }
    asc_append_stats(c, key, klen, val, vlen);

    ASSERT(c->stats.offset <= c->stats.size);
}
