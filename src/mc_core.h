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
#ifndef _MC_CORE_H_
#define _MC_CORE_H_

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#ifdef HAVE_DEBUG_LOG
# define MC_DEBUG_LOG 1
#else
# define MC_DEBUG_LOG 0
#endif

#ifdef HAVE_ASSERT_PANIC
# define MC_ASSERT_PANIC 1
#else
# define MC_ASSERT_PANIC 0
#endif

#ifdef HAVE_ASSERT_LOG
# define MC_ASSERT_LOG 1
#else
# define MC_ASSERT_LOG 0
#endif

#ifdef HAVE_MEM_SCRUB
# define MC_MEM_SCRUB 1
#else
# define MC_MEM_SCRUB 0
#endif

#ifdef DISABLE_STATS
# define MC_DISABLE_STATS 1
#else
# define MC_DISABLE_STATS 0
#endif

#ifdef DISABLE_KLOG
# define MC_DISABLE_KLOG 1
#else
# define MC_DISABLE_KLOG 0
#endif

#ifdef HAVE_LITTLE_ENDIAN
# define MC_LITTLE_ENDIAN 1
#endif

#ifdef HAVE_BACKTRACE
#define MC_BACKTRACE 1
#endif

#ifdef HAVE_MLOCKALL
#define MC_MLOCKALL 1
#endif

#if defined(HAVE_GETPAGESIZES) && defined(HAVE_MEMCNTL)
#define MC_LARGE_PAGES 1
#endif

#define MC_OK        0
#define MC_ERROR    -1
#define MC_EAGAIN   -2
#define MC_ENOMEM   -3

typedef int rstatus_t; /* return type */
typedef int err_t;     /* error type */

struct conn;
struct conn_q;
struct thread_key;
struct thread_worker;
struct thread_aggregator;
struct item;
struct slab;
struct slabclass;

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include <unistd.h>
#include <string.h>
#include <event.h>
#include <netdb.h>
#include <pthread.h>
#include <limits.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <limits.h>
#include <pthread.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include <mc_cache.h>
#include <mc_queue.h>
#include <mc_log.h>
#include <mc_time.h>
#include <mc_util.h>
#include <mc_hash.h>
#include <mc_alloc.h>

/*
 *          request    min          max     noreply   noreply
 *           type                            min        max
 */
#define REQ_CODEC(ACTION)                                       \
    ACTION( UNKNOWN,    0,          0,        0,        0   )   \
    ACTION( SET,        6,          6,        7,        7   )   \
    ACTION( ADD,        6,          6,        7,        7   )   \
    ACTION( REPLACE,    6,          6,        7,        7   )   \
    ACTION( APPEND,     6,          6,        7,        7   )   \
    ACTION( PREPEND,    6,          6,        7,        7   )   \
    ACTION( APPENDRL,   6,          6,        7,        7   )   \
    ACTION( PREPENDRL,  6,          6,        7,        7   )   \
    ACTION( CAS,        7,          7,        8,        8   )   \
    ACTION( GET,        3,    INT_MAX,        3,  INT_MAX   )   \
    ACTION( GETS,       3,    INT_MAX,        3,  INT_MAX   )   \
    ACTION( INCR,       4,          4,        5,        5   )   \
    ACTION( DECR,       4,          4,        5,        5   )   \
    ACTION( DELETE,     3,          3,        4,        4   )   \
    ACTION( QUIT,       2,          2,        2,        2   )   \
    ACTION( STATS,      2,          5,        2,        5   )   \
    ACTION( CONFIG,     3,          5,        3,        5   )   \
    ACTION( VERSION,    2,          2,        2,        2   )   \
    ACTION( FLUSHALL,   2,          3,        3,        4   )   \
    ACTION( VERBOSITY,  3,          4,        3,        4   )   \

/*
 *          response type
 */
#define RSP_CODEC(ACTION)   \
    ACTION( NOT_STORED    ) \
    ACTION( STORED        ) \
    ACTION( EXISTS        ) \
    ACTION( NOT_FOUND     ) \
    ACTION( DELETED       ) \
    ACTION( CLIENT_ERROR  ) \
    ACTION( SERVER_ERROR  ) \
    ACTION( OK            ) \

#define KEY_MAX_LEN     250 /* max key length */

/*
 * suffix includes data flags, data length, and cas value (optional)
 * The format follows " <flags> <length> <cas>"
 * line terminator (CRLF) is not part of the suffix in this implementation
 *
 * data flags and length are both 4 bytes, 10 bytes each to print;
 * cas is 8 bytes, 20 bytes to print;
 * each field needs another byte for the delimiter, which is space;
 * suffix is also given an extra byte to store '\0'
 */
#define CAS_SUFFIX_SIZE 21 /* of the cas field only */
#define SUFFIX_SIZE     22 /* of the other fields */

/* size of an incr buf */
#define INCR_MAX_STORAGE_LEN 24

#define HOSTNAME_SIZE 256 /* hostname length, we follow the UNIX convention */

#define EVICT_NONE    0x00 /* throw OOM, no eviction */
#define EVICT_LRU     0x01 /* per-slab lru eviction */
#define EVICT_RS      0x02 /* random slab eviction */
#define EVICT_AS      0x04 /* lra (least recently accessed) slab eviction */
#define EVICT_CS      0x08 /* lrc (least recently created) slab eviction */
#define EVICT_INVALID 0x10 /* go no further! */

#define DEFINE_ACTION(_type, _min, _max, _nmin, _nmax) REQ_##_type,
typedef enum req_type {
    REQ_CODEC( DEFINE_ACTION )
    REQ_SENTINEL
} req_type_t;
#undef DEFINE_ACTION

#define DEFINE_ACTION(_type) RSP_##_type,
typedef enum rsp_type {
    RSP_CODEC( DEFINE_ACTION )
    RSP_SENTINEL
} rsp_type_t;
#undef DEFINE_ACTION

#include <mc_thread.h>
#include <mc_slabs.h>
#include <mc_stats.h>
#include <mc_klog.h>
#include <mc_assoc.h>
#include <mc_items.h>
#include <mc_signal.h>
#include <mc_ascii.h>
#include <mc_connection.h>

struct settings {
                                                  /* options with no argument */

    bool            prealloc;                     /* memory  : whether we preallocate for slabs */
    bool            lock_page;                    /* memory  : whether to lock allcoated pages */
    bool            daemonize;                    /* process : daemonized or not */
    bool            max_corefile;                 /* process : maximize core core file limit */
    bool            use_cas;                      /* protocol: whether cas is supported */

                                                  /* options with required argument */

    char            *log_filename;                /* debug   : log filename */
    int             verbose;                      /* debug   : log verbosity level */

    struct timeval  stats_agg_intvl;              /* stats   : how often we aggregate stats */
    char            *klog_name;                   /* klog    : name of the command log */
    char            *klog_backup;                 /* klog    : name of the backup after log rotation */
    int             klog_sampling_rate;           /* klog    : log every klog_smp_rate messages */
    int             klog_entry;                   /* klog    : number of entry to buffer per thread */
    struct timeval  klog_intvl;                   /* klog    : how often the command logger collector thread runs */
    bool            klog_running;                 /* klog    : klog running? apply to both read and write */

    int             num_workers;                  /* process : number of workers driven by libevent */
    char            *username;                    /* process : run as another user */

    int             reqs_per_event;               /* network : max # of requests to process per io event */
    int             maxconns;                     /* network : max connections */
    int             backlog;                      /* network : tcp backlog */
    int             port;                         /* network : tcp listening port */
    int             udpport;                      /* network : udp listening port */
    char            *interface;                   /* network : listening interface */
    char            *socketpath;                  /* network : path to unix socket if used */
    int             access;                       /* network : access mask for unix socket */

    int             evict_opt;                    /* memory  : eviction */
    bool            use_freeq;                    /* memory  : whether use items in freeq or not */
    bool            use_lruq;                     /* memory  : whether use items in freeq or not */
    double          factor;                       /* memory  : chunk size growth factor */
    size_t          maxbytes;                     /* memory  : maximum bytes allowed for slabs */
    size_t          chunk_size;                   /* memory  : minimum item chunk size */
    size_t          max_chunk_size;               /* memory  : maximum item chunk size */
    size_t          slab_size;                    /* memory  : slab size */
    int             hash_power;                   /* memory  : hash table size, 0 for autotune */

                                                  /* global state */

    bool            accepting_conns;              /* network : whether we accept new connections */
    rel_time_t      oldest_live;                  /* data    : ignore existing items older than this */

    pid_t           pid;                          /* process : pid */
    char            *pid_filename;                /* process : pid file */
    unsigned        pid_file:1;                   /* process : pid file created? */

    size_t          profile[SLABCLASS_MAX_IDS];   /* memory  : slab profile */
    uint8_t         profile_last_id;              /* memory  : last id in slab profile */
};

void core_write_and_free(struct conn *c, char *buf, int bytes);
void core_event_handler(int fd, short which, void *arg);
void core_accept_conns(bool do_accept);

rstatus_t core_init(void);
void core_deinit(void);
rstatus_t core_loop(void);

#endif
