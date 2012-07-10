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

#ifndef _MC_STATS_H_
#define _MC_STATS_H_

/*
 * Each metric is described by a triplet (NAME, TYPE, DESCRIPTION). Members
 * of each list (thread, slab), when expanded, are prefixed with THREAD_ or
 * SLAB_. However, for the sake of uniformity we should still make sure that
 * shorter names are unique across different lists.
 *
 *          NAME                   TYPE                       DESCRIPTION
 */
#define STATS_THREAD_METRICS(ACTION)                                                                        \
    ACTION( conn_disabled,      STATS_COUNTER,      "# times accepting connections was disabled")           \
    ACTION( conn_total,         STATS_COUNTER,      "# connections created until now")                      \
    ACTION( conn_struct,        STATS_COUNTER,      "# new connection objects created")                     \
    ACTION( conn_yield,         STATS_COUNTER,      "# times we yielded from an active connection")         \
    ACTION( conn_curr,          STATS_GAUGE,        "# active connections")                                 \
    ACTION( data_read,          STATS_COUNTER,      "# bytes read")                                         \
    ACTION( data_written,       STATS_COUNTER,      "# bytes written")                                      \
    ACTION( add,                STATS_COUNTER,      "# add requests")                                       \
    ACTION( add_exist,          STATS_COUNTER,      "# add requests that was a hit")                        \
    ACTION( set,                STATS_COUNTER,      "# set requests")                                       \
    ACTION( replace,            STATS_COUNTER,      "# replace requests")                                   \
    ACTION( replace_miss,       STATS_COUNTER,      "# replace requests that was a miss")                   \
    ACTION( append,             STATS_COUNTER,      "# append requests")                                    \
    ACTION( append_miss,        STATS_COUNTER,      "# append requests that was a miss")                    \
    ACTION( prepend,            STATS_COUNTER,      "# prepend requests")                                   \
    ACTION( prepend_miss,       STATS_COUNTER,      "# prepend requests that was a miss")                   \
    ACTION( delete,             STATS_COUNTER,      "# delete requests")                                    \
    ACTION( delete_miss,        STATS_COUNTER,      "# delete requests that was a miss")                    \
    ACTION( incr,               STATS_COUNTER,      "# incr requests")                                      \
    ACTION( incr_miss,          STATS_COUNTER,      "# incr requests that was a miss")                      \
    ACTION( decr,               STATS_COUNTER,      "# decr requests")                                      \
    ACTION( decr_miss,          STATS_COUNTER,      "# decr requests that was a miss")                      \
    ACTION( cas,                STATS_COUNTER,      "# cas requests")                                       \
    ACTION( cas_miss,           STATS_COUNTER,      "# cas requests that was a miss")                       \
    ACTION( get,                STATS_COUNTER,      "# get requests")                                       \
    ACTION( get_miss,           STATS_COUNTER,      "# get requests that was a miss")                       \
    ACTION( gets,               STATS_COUNTER,      "# gets requests")                                      \
    ACTION( gets_miss,          STATS_COUNTER,      "# gets requests that was a miss")                      \
    ACTION( flush,              STATS_COUNTER,      "# flush requests")                                     \
    ACTION( stats,              STATS_COUNTER,      "# stats requests")                                     \
    ACTION( cmd_error,          STATS_COUNTER,      "# invalid requests")                                   \
    ACTION( server_error,       STATS_COUNTER,      "# requests that resulted in server errors")            \
    ACTION( klog_logged,        STATS_COUNTER,      "# commands logged in buffer when klog is turned on")   \
    ACTION( klog_discarded,     STATS_COUNTER,      "# commands discarded when klog is turned on")          \
    ACTION( klog_skipped,       STATS_COUNTER,      "# commands skipped by sampling when klog is turned on")\


#define STATS_SLAB_METRICS(ACTION)                                                                          \
    ACTION( data_curr,          STATS_GAUGE,        "# current item bytes including overhead")              \
    ACTION( data_value_curr,    STATS_GAUGE,        "# current data bytes")                                 \
    ACTION( item_curr,          STATS_GAUGE,        "# current items")                                      \
    ACTION( item_acquire,       STATS_COUNTER,      "# items acquired (allocated or reused)")               \
    ACTION( item_remove,        STATS_COUNTER,      "# items removed")                                      \
    ACTION( item_expire,        STATS_COUNTER,      "# items expired")                                      \
    ACTION( item_evict,         STATS_COUNTER,      "# items evicted")                                      \
    ACTION( item_free,          STATS_GAUGE,        "# items in free q")                                    \
    ACTION( item_expire_ts,     STATS_TIMESTAMP,    "last item expired timestamp")                          \
    ACTION( item_reclaim_ts,    STATS_TIMESTAMP,    "last item reclaimed timestamp")                        \
    ACTION( item_evict_ts,      STATS_TIMESTAMP,    "last item evicted timestamp")                          \
    ACTION( slab_req,           STATS_COUNTER,      "# slab allocation requests")                           \
    ACTION( slab_error,         STATS_COUNTER,      "# slabs allocation failures")                          \
    ACTION( slab_alloc,         STATS_COUNTER,      "# allocated slabs until now")                          \
    ACTION( slab_curr,          STATS_GAUGE,        "# current slabs")                                      \
    ACTION( slab_evict,         STATS_COUNTER,      "# slabs evicted")                                      \
    ACTION( slab_req_ts,        STATS_TIMESTAMP,    "the last slab requested timestamp")                    \
    ACTION( slab_error_ts,      STATS_TIMESTAMP,    "the last slab allocation failure timestamp")           \
    ACTION( slab_alloc_ts,      STATS_TIMESTAMP,    "the last allocated slab timestamp")                    \
    ACTION( slab_new_ts,        STATS_TIMESTAMP,    "the last newly allocated slab timestamp")              \
    ACTION( slab_evict_ts,      STATS_TIMESTAMP,    "the last slab evicted timestamp")                      \
    ACTION( set_success,        STATS_COUNTER,      "# set requests tht was a success")                     \
    ACTION( add_success,        STATS_COUNTER,      "# add requests that was a success")                    \
    ACTION( replace_hit,        STATS_COUNTER,      "# replace requests that was a hit")                    \
    ACTION( replace_success,    STATS_COUNTER,      "# replace requests that was a success")                \
    ACTION( append_hit,         STATS_COUNTER,      "# append requests that was a hit")                     \
    ACTION( append_success,     STATS_COUNTER,      "# append requests that was a success")                 \
    ACTION( prepend_hit,        STATS_COUNTER,      "# prepend requests that was a hit")                    \
    ACTION( prepend_success,    STATS_COUNTER,      "# prepend requests that was a success")                \
    ACTION( delete_hit,         STATS_COUNTER,      "# delete requests that was a hit")                     \
    ACTION( incr_hit,           STATS_COUNTER,      "# incr requests that was a hit")                       \
    ACTION( incr_success,       STATS_COUNTER,      "# incr requests that was a success")                   \
    ACTION( decr_hit,           STATS_COUNTER,      "# decr requests that was a hit")                       \
    ACTION( decr_success,       STATS_COUNTER,      "# decr requests that was a success")                   \
    ACTION( cas_badval,         STATS_COUNTER,      "# cas requests that resulted in exists")               \
    ACTION( cas_hit,            STATS_COUNTER,      "# cas requests that was a hit")                        \
    ACTION( cas_success,        STATS_COUNTER,      "# cas requests that was a success")                    \
    ACTION( get_hit,            STATS_COUNTER,      "# get requests that was a hit")                        \
    ACTION( gets_hit,           STATS_COUNTER,      "# gets requests that was a hit")                       \

#define STATS_MIN_INTVL     10000    /* min aggregation interval in usec */
#define STATS_MAX_INTVL     60000000 /* max aggregation interval in usec */
#define STATS_DEFAULT_INTVL 100000   /* aggregation interval in usec */

typedef enum metric_type {
    STATS_INVALID,   /* invalid or uninitialized */
    STATS_TIMESTAMP, /* monotonic timestamp as rel_time_t (in secs) */
    STATS_COUNTER,   /* monotonic accumulator */
    STATS_GAUGE      /* non-monotonic accumulator */
} metric_type_t;

struct stats_metric {
    metric_type_t   type;       /* type */
    char            *name;      /* display name */
    union {                     /* actual value */
        rel_time_t  timestamp;  /* monotonic timestamp */
        int64_t     counter;    /* accumulating counter */
        struct {                /* gauge */
            int64_t t;          /* incr counter */
            int64_t b;          /* decr counter */
        } gauge;
    } value;
};

/* Since it's constant after initialization we don't use the metric struct */
struct stats_slab_const {
    uint64_t chunk_size;    /* item size + metadata + alignment padding */
    uint64_t items_perslab; /* maximum # items per slab */
};

#define MAKELIST(_name, _type, _desc) THREAD_##_name,
typedef enum stats_tmetric {
    STATS_THREAD_METRICS(MAKELIST)
    STATS_THREAD_LEN
} stats_tmetric_t;
#undef MAKELIST

#define MAKELIST(_name, _type, _desc) SLAB_##_name,
typedef enum stats_smetric {
    STATS_SLAB_METRICS(MAKELIST)
    STATS_SLAB_LEN
} stats_smetric_t;
#undef MAKELIST

/*
 * We need two different call interfaces to thread & slab stats because
 * all that callers know is the name into the enum.  It's difficult to
 * combine the two without burdening the caller to pass the pointer into
 * the actual storage of those metrics, which defeats our intention to
 * keep the routine simple for callers.
 */

#if defined MC_DISABLE_STATS && MC_DISABLE_STATS == 1

/* aggregation */
#define stats_aggregate()

/* thread level metrics */
#define stats_thread_incr(_name)
#define stats_thread_decr(_name)
#define stats_thread_incr_by(_name, _delta)
#define stats_thread_decr_by(_name, _delta)

/* slab level metrics */
#define stats_slab_settime(_cls_id, _name, _val)
#define stats_slab_incr(_cls_id, _name)
#define stats_slab_decr(_cls_id, _name)
#define stats_slab_incr_by(_cls_id, _name, _delta)
#define stats_slab_decr_by(_cls_id, _name, _delta)

#else

#define stats_aggregate()                                       \
    _stats_aggregate()

#define stats_thread_incr(_name)                                \
    _stats_thread_incr(THREAD_##_name)
#define stats_thread_decr(_name)                                \
    _stats_thread_decr(THREAD_##_name)
#define stats_thread_incr_by(_name, _delta)                     \
    _stats_thread_incr_by(THREAD_##_name, (int64_t)_delta)
#define stats_thread_decr_by(_name, _delta)                     \
    _stats_thread_decr_by(THREAD_##_name, (int64_t)_delta)

#define stats_slab_settime(_cls_id, _name, _val)                \
    _stats_slab_settime(_cls_id, SLAB_##_name, (rel_time_t)_val)
#define stats_slab_incr(_cls_id, _name)                         \
    _stats_slab_incr(_cls_id, SLAB_##_name)
#define stats_slab_decr(_cls_id, _name)                         \
    _stats_slab_decr(_cls_id, SLAB_##_name)
#define stats_slab_incr_by(_cls_id, _name, _delta)              \
    _stats_slab_incr_by(_cls_id, SLAB_##_name, (int64_t)_delta)
#define stats_slab_decr_by(_cls_id, _name, _delta)              \
    _stats_slab_decr_by(_cls_id, SLAB_##_name, (int64_t)_delta)

#endif

void stats_describe(void);
bool stats_enabled(void);
void stats_set_interval(long interval);

void stats_init(void);
void stats_deinit(void);

struct stats_metric *stats_thread_init(void);
void stats_thread_deinit(struct stats_metric *stats_thread);
struct stats_metric **stats_slabs_init(void);

void _stats_aggregate(void);

void _stats_thread_incr(stats_tmetric_t name);
void _stats_thread_decr(stats_tmetric_t name);
void _stats_thread_incr_by(stats_tmetric_t name, int64_t delta);
void _stats_thread_decr_by(stats_tmetric_t name, int64_t delta);

void _stats_slab_settime(uint8_t cls_id, stats_smetric_t name, rel_time_t val);
void _stats_slab_incr(uint8_t cls_id, stats_smetric_t name);
void _stats_slab_decr(uint8_t cls_id, stats_smetric_t name);
void _stats_slab_incr_by(uint8_t cls_id, stats_smetric_t name, int64_t delta);
void _stats_slab_decr_by(uint8_t cls_id, stats_smetric_t name, int64_t delta);

void stats_default(struct conn *c);
void stats_settings(void *c);
void stats_slabs(struct conn *c);
void stats_sizes(void *c);
void stats_append(struct conn *c, const char *key, uint16_t klen, char *val, uint32_t vlen);

#endif
