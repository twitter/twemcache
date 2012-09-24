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

/*
 *  memcached - memory caching daemon
 *
 *       http://www.danga.com/memcached/
 *
 *  Copyright 2003 Danga Interactive, Inc.  All rights reserved.
 *
 *  Use and distribution licensed under the BSD license.  See
 *  the LICENSE file for full text.
 *
 *  Authors:
 *      Anatoly Vorobey <mellon@pobox.com>
 *      Brad Fitzpatrick <brad@danga.com>
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/uio.h>
#include <ctype.h>
#include <pwd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <getopt.h>

#include <mc_core.h>

#define MC_CHUNK_SIZE       ITEM_CHUNK_SIZE
#define MC_SLAB_SIZE        SLAB_SIZE

#define MC_SLAB_PREALLOC    false
#define MC_LOCK_PAGES       false
#define MC_DAEMONIZE        false
#define MC_MAXIMIZE_CORE    false
#define MC_DISABLE_CAS      false

#define MC_LOG_FILE         NULL
#define MC_LOG_DEFAULT      LOG_NOTICE
#define MC_LOG_MIN          LOG_EMERG
#define MC_LOG_MAX          LOG_PVERB

#define MC_STATS_MIN_INTVL  STATS_MIN_INTVL
#define MC_STATS_MAX_INTVL  STATS_MAX_INTVL
#define MC_STATS_INTVL      STATS_DEFAULT_INTVL

#define MC_HASH_MAX_POWER   HASH_MAX_POWER

#define MC_KLOG_INTVL       KLOG_DEFAULT_INTVL
#define MC_KLOG_SMP_RATE    KLOG_DEFAULT_SMP_RATE
#define MC_KLOG_ENTRY       KLOG_DEFAULT_ENTRY
#define MC_KLOG_FILE        NULL
#define MC_KLOG_BACKUP      NULL
#define MC_KLOG_BACKUP_SUF  ".old"

#define MC_WORKERS          4
#define MC_PID_FILE         NULL
#define MC_USER             NULL

#define MC_REQ_PER_EVENT    20
#define MC_MAX_CONNS        1024
#define MC_BACKLOG          1024

#define MC_TCP_PORT         11211
#define MC_UDP_PORT         11211
#define MC_INTERFACE        NULL
#define MC_UNIX_PATH        NULL
#define MC_ACCESS_MASK      0700

#define MC_EVICT            EVICT_RS
#define MC_EVICT_STR        "random"
#define MC_FACTOR           1.25
#define MC_MAXBYTES         (64 * MB)

struct settings settings;          /* twemcache settings */
static int show_help;              /* show twemcache help? */
static int show_version;           /* show twemcache version? */
static int show_stats_description; /* show twemcache stats description? */
static int show_sizes;             /* show twemcache struct sizes? */
static int parse_profile;          /* parse profile? */
static char *profile_optarg;       /* profile optarg */

static struct option long_options[] = {
    { "help",                 no_argument,        NULL,   'h' }, /* help */
    { "version",              no_argument,        NULL,   'V' }, /* version */
    { "prealloc",             no_argument,        NULL,   'E' }, /* preallocate slabs */
    { "use-large-pages",      no_argument,        NULL,   'L' }, /* use large memory pages */
    { "lock-pages",           no_argument,        NULL,   'k' }, /* lock all pages */
    { "daemonize",            no_argument,        NULL,   'd' }, /* daemon mode */
    { "maximize-core-limit",  no_argument,        NULL,   'r' }, /* maximize corefile limit */
    { "disable-cas",          no_argument,        NULL,   'C' }, /* disable cas */
    { "describe-stats",       no_argument,        NULL,   'D' }, /* print stats description and exit */
    { "show-sizes",           no_argument,        NULL,   'S' }, /* print slab & item struct sizes and exit */
    { "output",               required_argument,  NULL,   'o' }, /* output logfile */
    { "verbosity",            required_argument,  NULL,   'v' }, /* log verbosity level */
    { "stats-aggr-interval",  required_argument,  NULL,   'A' }, /* stats aggregation interval in usec */
    { "klog-entry",           required_argument,  NULL,   'x' }, /* command logging entry number */
    { "klog-file",            required_argument,  NULL,   'X' }, /* command logging file */
    { "klog-sample-rate",     required_argument,  NULL,   'y' }, /* command logging sampling rate */
    { "threads",              required_argument,  NULL,   't' }, /* # of threads */
    { "pidfile",              required_argument,  NULL,   'P' }, /* pid file */
    { "user",                 required_argument,  NULL,   'u' }, /* user identity to run as */
    { "max-requests",         required_argument,  NULL,   'R' }, /* max request per event */
    { "max-conns",            required_argument,  NULL,   'c' }, /* max simultaneous connections */
    { "backlog",              required_argument,  NULL,   'b' }, /* tcp backlog queue limit */
    { "port",                 required_argument,  NULL,   'p' }, /* tcp port number to listen on */
    { "udp-port",             required_argument,  NULL,   'U' }, /* udp port number to listen on */
    { "interface",            required_argument,  NULL,   'l' }, /* interface to listen on */
    { "unix-path",            required_argument,  NULL,   's' }, /* unix socket path to listen on */
    { "access-mask",          required_argument,  NULL,   'a' }, /* access mask for unix socket */
    { "eviction-strategy",    required_argument,  NULL,   'M' }, /* eviction strategy on OOM */
    { "factor",               required_argument,  NULL,   'f' }, /* growth factor for slab items */
    { "max-memory",           required_argument,  NULL,   'm' }, /* max memory for all items in MB */
    { "min-item-chunk-size",  required_argument,  NULL,   'n' }, /* min item chunk size */
    { "slab-size",            required_argument,  NULL,   'I' }, /* slab size */
    { "slab-profile",         required_argument,  NULL,   'z' }, /* profile of slab item sizes */
    { NULL,                   0,                  NULL,    0  }
};

static char short_options[] =
    "h"  /* help */
    "V"  /* version */
    "E"  /* preallocate slabs */
    "L"  /* use large memory pages */
    "k"  /* lock all pages */
    "d"  /* daemon mode */
    "r"  /* maximize corefile limit */
    "C"  /* disable cas */
    "D"  /* print stats description and exit */
    "S"  /* print slab & item struct sizes and exit */
    "o:" /* output logfile */
    "v:" /* log verbosity level */
    "A:" /* stats aggregation interval in msec */
    "x:" /* command logging entry number */
    "X:" /* command logging file */
    "y:" /* command logging sample rate */
    "t:" /* # of threads */
    "P:" /* pid file */
    "u:" /* user identity to run as */
    "R:" /* max request per event */
    "c:" /* max simultaneous connections */
    "b:" /* tcp backlog queue limit */
    "p:" /* tcp port number to listen on */
    "U:" /* udp port number to listen on */
    "l:" /* interface to listen on */
    "s:" /* unix socket path to listen on */
    "a:" /* access mask for unix socket */
    "M:" /* eviction strategy on OOM */
    "f:" /* growth factor for slab items */
    "m:" /* max memory for all items in MB */
    "n:" /* min item size */
    "I:" /* max item size */
    "z:" /* profile of slab item sizes */
    ;

static void
mc_show_usage(void)
{
    log_stderr(
        "Usage: twemcache [-?hVCELdkrDS] [-o output file] [-v verbosity level]" CRLF
        "           [-A stats aggr interval] [-e hash power]" CRLF
        "           [-t threads] [-P pid file] [-u user]" CRLF
        "           [-x command logging entry] [-X command logging file] [-y command logging sample rate]" CRLF
        "           [-R max requests] [-c max conns] [-b backlog] [-p port] [-U udp port]" CRLF
        "           [-l interface] [-s unix path] [-a access mask] [-M eviction strategy]" CRLF
        "           [-f factor] [-m max memory] [-n min item chunk size] [-I slab size]" CRLF
        "           [-z slab profile]" CRLF
        "");
    log_stderr(
        "Options:" CRLF
        "  -h, --help                  : this help" CRLF
        "  -V, --version               : show version and exit" CRLF
        "  -E, --prealloc              : preallocate memory for all slabs" CRLF
        "  -L, --use-large-pages       : use large pages if available" CRLF
        "  -k, --lock-pages            : lock all pages and preallocate slab memory" CRLF
        "  -d, --daemonize             : run as a daemon" CRLF
        "  -r, --maximize-core-limit   : maximize core file limit" CRLF
        "  -C, --disable-cas           : disable use of cas" CRLF
        "  -D, --describe-stats        : print stats description and exit" CRLF
        "  -S, --show-sizes            : print slab and item struct sizes and exit"
        " ");

    log_stderr(
        "  -o, --output=S              : set the logging file (default: %s)" CRLF
        "  -v, --verbosity=N           : set the logging level (default: %d, min: %d, max: %d)" CRLF
        "  -A, --stats-aggr-interval=N : set the stats aggregation interval in usec (default: %d usec)" CRLF
        "  -e, --hash-power=N          : set the hash table size as a power of 2 (default: 0, adjustable)" CRLF
        "  -t, --threads=N             : set number of threads to use (default: %d)" CRLF
        "  -P, --pidfile=S             : set the pid file (default: %s)" CRLF
        "  -u, --user=S                : set user identity when run as root (default: %s)"
        " ",
        MC_LOG_FILE != NULL ? MC_LOG_FILE : "stderr", MC_LOG_DEFAULT, MC_LOG_MIN, MC_LOG_MAX,
        MC_STATS_INTVL,
        MC_WORKERS,
        MC_PID_FILE != NULL ? MC_PID_FILE : "off",
        MC_USER != NULL ? MC_USER : "off"
        );

    log_stderr(
        "  -x, --klog-entry=N          : set the command logging entry number per thread (default: %d)" CRLF
        "  -X, --klog-file=S           : set the command logging file (default: %s)" CRLF
        "  -y, --klog-sample-rate=N    : set the command logging sample rate (default: %d)"
        " ",
        MC_KLOG_ENTRY,
        MC_KLOG_FILE != NULL ? MC_KLOG_FILE : "off",
        MC_KLOG_SMP_RATE
        );

    log_stderr(
        "  -R, --max-requests=N        : set the maximum number of requests per event (default: %d)" CRLF
        "  -c, --max-conns=N           : set the maximum simultaneous connections (default: %d)" CRLF
        "  -b, --backlog=N             : set the backlog queue limit (default %d)" CRLF
        "  -p, --port=N                : set the tcp port to listen on (default: %d)" CRLF
        "  -U, --udp-port=N            : set the udp port to listen on (default: %d)" CRLF
        "  -l, --interface=S           : set the interface to listen on (default: %s)" CRLF
        "  -s, --unix-path=S           : set the unix socket path to listen on (default: %s)" CRLF
        "  -a, --access-mask=O         : set the access mask for unix socket in octal (default: %04o)"
        " ",
        MC_REQ_PER_EVENT, MC_MAX_CONNS, MC_BACKLOG,
        MC_TCP_PORT, MC_UDP_PORT,
        MC_INTERFACE != NULL ? MC_INTERFACE : "all",
        MC_UNIX_PATH != NULL ? MC_UNIX_PATH : "off", MC_ACCESS_MASK
        );

    log_stderr(
        "  -M, --eviction-strategy=N   : set the eviction strategy on OOM (default: %d, %s)" CRLF
        "  -f, --factor=D              : set the growth factor of slab item sizes (default: %g)" CRLF
        "  -m, --max-memory=N          : set the maximum memory to use for all items in MB (default: %d MB)" CRLF
        "  -n, --min-item-chunk-size=N : set the minimum item chunk size in bytes (default: %d bytes)" CRLF
        "  -I, --slab-size=N           : set slab size in bytes (default: %d bytes)" CRLF
        "  -z, --slab-profile=S        : set the profile of slab item chunk sizes (default: off)" CRLF
        " ",
        MC_EVICT, MC_EVICT_STR,
        MC_FACTOR, MC_MAXBYTES / MB,
        MC_CHUNK_SIZE,
        SLAB_SIZE
        );
}

static rstatus_t
mc_create_pidfile(void)
{
    rstatus_t status;
    char pid[MC_UINTMAX_MAXLEN];
    int fd, pid_len;
    ssize_t n;

    fd = open(settings.pid_filename, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        log_error("opening pid file '%s' failed: %s", settings.pid_filename,
                  strerror(errno));
        return MC_ERROR;
    }
    settings.pid_file = 1;

    pid_len = mc_snprintf(pid, MC_UINTMAX_MAXLEN, "%d", settings.pid);

    n = mc_write(fd, pid, pid_len);
    if (n < 0) {
        log_error("write to pid file '%s' failed: %s", settings.pid_filename,
                  strerror(errno));
        return MC_ERROR;
    }

    status = close(fd);
    if (status < 0) {
        log_error("close pid file '%s' failed: %s", settings.pid_filename,
                  strerror(errno));
        return MC_ERROR;
    }

    return MC_OK;
}

static void
mc_remove_pidfile(void)
{
    int status;

    if (!settings.pid_file) {
        return;
    }

    status = unlink(settings.pid_filename);
    if (status < 0) {
        log_error("unlink of pid file '%s' failed, ignored: %s",
                  settings.pid_filename, strerror(errno));
    }
}

static rstatus_t
mc_daemonize(int dump_core)
{
    rstatus_t status;
    pid_t pid, sid;
    int fd;

    /* 1st fork detaches child from terminal */
    pid = fork();
    switch (pid) {
    case -1:
        log_error("fork() failed: %s", strerror(errno));
        return MC_ERROR;

    case 0:
        break;

    default:
        /* parent terminates */
        _exit(0);
    }

    /* 1st child continues and becomes the session and process group leader */
    sid = setsid();
    if (sid < 0) {
        return MC_ERROR;
    }

    if (signal(SIGHUP, SIG_IGN) == SIG_ERR) {
        log_error("signal(SIGHUP, SIG_IGN) failed: %s", strerror(errno));
        return MC_ERROR;
    }

    /* 2nd fork turns child into a non-session leader: cannot acquire terminal */
    pid = fork();
    switch (pid) {
    case -1:
        log_error("fork() failed: %s", strerror(errno));
        return MC_ERROR;

    case 0:
        break;

    default:
        /* 1st child terminates */
        _exit(0);
    }

    /* change working directory */
    if (dump_core == 0) {
        status = chdir("/");
        if (status < 0) {
            log_error("chdir(\"/\") failed: %s", strerror(errno));
            return MC_ERROR;
        }
    }

    /* clear file mode creation mask */
    umask(0);

    /* redirect stdin, stdout and stderr to "/dev/null" */

    fd = open("/dev/null", O_RDWR);
    if (fd < 0) {
        log_error("open(\"/dev/null\") failed: %s", strerror(errno));
        return MC_ERROR;
    }

    status = dup2(fd, STDIN_FILENO);
    if (status < 0) {
        log_error("dup2(%d, STDIN) failed: %s", fd, strerror(errno));
        close(fd);
        return MC_ERROR;
    }

    status = dup2(fd, STDOUT_FILENO);
    if (status < 0) {
        log_error("dup2(%d, STDOUT) failed: %s", fd, strerror(errno));
        close(fd);
        return MC_ERROR;
    }

    status = dup2(fd, STDERR_FILENO);
    if (status < 0) {
        log_error("dup2(%d, STDERR) failed: %s", fd, strerror(errno));
        close(fd);
        return MC_ERROR;
    }

    if (fd > STDERR_FILENO) {
        status = close(fd);
        if (status < 0) {
            log_error("close(%d) failed: %s", fd, strerror(errno));
            return MC_ERROR;
        }
    }

    return MC_OK;
}

/*
 * On systems that supports multiple page sizes we may reduce the
 * number of TLB-misses by using the biggest available page size
 */
static int
mc_enable_large_pages(void)
{
#ifdef MC_LARGE_PAGES
    int ret, i, avail;
    size_t sizes[32];
    size_t max = sizes[0];
    struct memcntl_mha arg = {0};

    avail = getpagesizes(sizes, 32);
    if (avail < 0) {
        log_error("getpagesizes failed: %s, using default page size",
                  strerror(errno));
        return -1;
    }

    for (i = 1; i < avail; i++) {
        if (max < sizes[i]) {
            max = sizes[i];
        }
    }

    arg.mha_flags = 0;
    arg.mha_pagesize = max;
    arg.mha_cmd = MHA_MAPSIZE_BSSBRK;

    ret = memcntl(0, 0, MC_HAT_ADVISE, (caddr_t)&arg, 0, 0);
    if (ret < 0) {
        log_error("memctl to to set large pages failed: %s, using default "
                  "page size", strerror(errno));
        return ret;
    }
#endif

    return 0;
}

static rstatus_t
mc_lock_page(void)
{
#ifdef MC_MLOCKALL
    int status;

    status = mlockall(MCL_CURRENT | MCL_FUTURE);
    if (status < 0) {
        log_stderr("twemcache: -k option to mlockall failed: %s",
                   strerror(errno));
        return MC_ERROR;
    }
    return MC_OK;
#else
    log_stderr("twemcache: -k option to mlockall not supported");
    return MC_ERROR;
#endif
}

static void
mc_set_default_options(void)
{
    settings.prealloc = MC_SLAB_PREALLOC;
    settings.lock_page = MC_LOCK_PAGES;
    settings.daemonize = MC_DAEMONIZE;
    settings.max_corefile = MC_MAXIMIZE_CORE;
    settings.use_cas = MC_DISABLE_CAS ? false : true;

    settings.log_filename = MC_LOG_FILE;
    settings.verbose = MC_LOG_DEFAULT;

    stats_set_interval(MC_STATS_INTVL);

    settings.klog_name = MC_KLOG_FILE;
    settings.klog_backup = MC_KLOG_BACKUP;
    settings.klog_sampling_rate = MC_KLOG_SMP_RATE;
    settings.klog_entry = MC_KLOG_ENTRY;
    klog_set_interval(MC_KLOG_INTVL);
    settings.klog_running = false;

    settings.num_workers = MC_WORKERS;
    settings.username = MC_USER;

    settings.reqs_per_event = MC_REQ_PER_EVENT;
    settings.maxconns = MC_MAX_CONNS;
    settings.backlog = MC_BACKLOG;
    settings.port = MC_TCP_PORT;
    settings.udpport = MC_UDP_PORT;
    settings.interface = MC_INTERFACE;
    settings.socketpath = MC_UNIX_PATH;
    settings.access = MC_ACCESS_MASK;

    settings.evict_opt = MC_EVICT;
    settings.use_freeq = true;
    settings.use_lruq = true;
    settings.factor = MC_FACTOR;
    settings.maxbytes = MC_MAXBYTES;
    settings.chunk_size = MC_CHUNK_SIZE;
    settings.slab_size = MC_SLAB_SIZE;
    settings.hash_power = 0;

    settings.accepting_conns = true;
    settings.oldest_live = 0;

    settings.pid = 0;
    settings.pid_filename = MC_PID_FILE;
    settings.pid_file = 0;

    memset(settings.profile, 0, sizeof(settings.profile));
    settings.profile_last_id = SLABCLASS_MAX_ID;
}

static rstatus_t
mc_get_options(int argc, char **argv)
{
    int c, value, factor;
    size_t len;
    bool tcp_specified, udp_specified;

    tcp_specified = false;
    udp_specified = false;
    opterr = 0;

    for (;;) {
        c = getopt_long(argc, argv, short_options, long_options, NULL);
        if (c == -1) {
            /* no more options */
            break;
        }

        switch (c) {
        case 'h':
            show_version = 1;
            show_help = 1;
            break;

        case 'V':
            show_version = 1;
            break;

        case 'E':
            settings.prealloc = true;
            break;

        case 'L':
            if (mc_enable_large_pages() == 0) {
                settings.prealloc = true;
            }
            break;

        case 'k':
            settings.lock_page = true;
            settings.prealloc = true;
            break;

        case 'd':
            settings.daemonize = true;
            break;

        case 'r':
            settings.max_corefile = true;
            break;

        case 'C':
            settings.use_cas = false;
            break;

        case 'D':
            show_stats_description = 1;
            show_version = 1;
            break;

        case 'S':
            show_sizes = 1;
            show_version = 1;
            break;

        case 'o':
            settings.log_filename = optarg;
            break;

        case 'v':
            value = mc_atoi(optarg, strlen(optarg));
            if (value < 0) {
                log_stderr("twemcache: option -v requires a number");
                return MC_ERROR;
            }

            settings.verbose = value;
            break;

        case 'A':
            value = mc_atoi(optarg, strlen(optarg));
            if (value <= 0) {
                log_stderr("twemcache: option -A requires a non zero number");
                return MC_ERROR;
            }

            if (value < MC_STATS_MIN_INTVL) {
                log_stderr("twemcache: stats aggregation interval cannot be "
                           "less than %d usec",  MC_STATS_MIN_INTVL);
                return MC_ERROR;
            }

            if (value > MC_STATS_MAX_INTVL) {
                log_stderr("twemcache: stats aggregation interval cannot exceed"
                           " %d usec",  MC_STATS_MAX_INTVL);
                return MC_ERROR;
            }

            stats_set_interval(value);

            break;

        case 'e':
            value = mc_atoi(optarg, strlen(optarg));
            if (value <= 0) {
                log_stderr("twemcache: option -e requires a positive number");
                return MC_ERROR;
            }

            if (value > MC_HASH_MAX_POWER) {
                log_stderr("twemcache: hash power cannot be greater than %d",
                           MC_HASH_MAX_POWER);
                return MC_ERROR;
            }

            settings.hash_power = value;
            break;

        case 'x':
            value = mc_atoi(optarg, strlen(optarg));
            if (value <= 0) {
                log_stderr("twemcache: option -x requires a positive number");
                return MC_ERROR;
            }
            settings.klog_entry = value;
            break;

        case 'X':
            settings.klog_name = optarg;
            len = strlen(optarg) + sizeof(MC_KLOG_BACKUP_SUF);
            settings.klog_backup = mc_alloc(len);
            if (settings.klog_backup == NULL) {
                log_stderr("twemcache: cannot generate klog backup filename");
                return MC_ERROR;
            }
            value = mc_snprintf(settings.klog_backup, len, "%s"MC_KLOG_BACKUP_SUF,
                        settings.klog_name);
            ASSERT(value < len);
            settings.klog_running = true;
            break;

        case 'y':
            value = mc_atoi(optarg, strlen(optarg));
            if (value <= 0) {
                log_stderr("twemcache: option -y requires a positive number");
                return MC_ERROR;
            }
            settings.klog_sampling_rate = value;
            break;

        case 't':
            value = mc_atoi(optarg, strlen(optarg));
            if (value <= 0) {
                log_stderr("twemcache: option -t requires a non zero number");
                return MC_ERROR;
            }

            settings.num_workers = value;
            break;

        case 'P':
            settings.pid_filename = optarg;
            break;

        case 'u':
            settings.username = optarg;
            break;

        case 'R':
            value = mc_atoi(optarg, strlen(optarg));
            if (value <= 0) {
                log_stderr("twemcache: option -R requires a non zero number");
                return MC_ERROR;
            }

            settings.reqs_per_event = value;
            break;

        case 'c':
            value = mc_atoi(optarg, strlen(optarg));
            if (value <= 0) {
                log_stderr("twemcache: option -c requires a non zero number");
                return MC_ERROR;
            }

            settings.maxconns = value;
            break;

        case 'b':
            value = mc_atoi(optarg, strlen(optarg));
            if (value <= 0) {
                log_stderr("twemcache: option -b requires a non zero number");
                return MC_ERROR;
            }

            settings.backlog = value;
            break;

        case 'p':
            value = mc_atoi(optarg, strlen(optarg));
            if (value <= 0) {
                log_stderr("twemcache: option -p requires a non zero number");
                return MC_ERROR;
            }

            if (!mc_valid_port(value)) {
                log_stderr("twemcache: option -p value %d is not a valid "
                           "port", value);
            }

            settings.port = value;
            tcp_specified = true;
            break;

        case 'U':
            value = mc_atoi(optarg, strlen(optarg));
            if (value <= 0) {
                log_stderr("twemcache: option -U requires a non number");
                return MC_ERROR;
            }

            if (!mc_valid_port(value)) {
                log_stderr("twemcache: option -U value %d is not a valid "
                           "port", value);
            }

            settings.udpport = value;
            udp_specified = true;
            break;

        case 'l':
            settings.interface = optarg;
            break;

        case 's':
            settings.socketpath = optarg;
            break;

        case 'a':
            if (!mc_str2oct(optarg, &value)) {
                log_stderr("twemcache: option -a requires an octal number");
                return MC_ERROR;
            }
            settings.access = value;
            break;

        case 'M':
            value = mc_atoi(optarg, strlen(optarg));
            if (value < 0) {
                log_stderr("twemcache: option -M requires a number");
                return MC_ERROR;
            }
            if (value >= EVICT_INVALID || value < EVICT_NONE) {
                log_stderr("twemcache: option -M value %d is not a valid "
                           "eviction strategy", value);
                return MC_ERROR;
            }
            settings.evict_opt = value;
            if (value == EVICT_CS) {
                settings.use_freeq = false;
                settings.use_lruq = false;
            }
            break;

        case 'f':
            settings.factor = atof(optarg);
            if (settings.factor <= 1.0) {
                log_stderr("twemcache: factor must be greater than 1.0");
                return MC_ERROR;
            }
            break;

        case 'm':
            value = mc_atoi(optarg, strlen(optarg));
            if (value <= 0) {
                log_stderr("twemcache: option -m requires a non zero number");
                return MC_ERROR;
            }

            settings.maxbytes = (size_t)value * 1024 * 1024;
            break;

        case 'n':
            value = mc_atoi(optarg, strlen(optarg));
            if (value <= 0) {
                log_stderr("twemcache: option -n requires a non zero number");
                return MC_ERROR;
            }

            if (value < ITEM_MIN_CHUNK_SIZE) {
                log_stderr("twemcache: minimum item chunk size cannot be less "
                           "than %zu", ITEM_MIN_CHUNK_SIZE);
                return MC_ERROR;
            }

            if (value % MC_ALIGNMENT != 0) {
                log_stderr("twemcache: minimum item chunk size must be %zu "
                           "bytes aligned", MC_ALIGNMENT);
                return MC_ERROR;
            }

            settings.chunk_size = value;
            break;

        case 'I':
            len = strlen(optarg);
            switch (optarg[len - 1]) {
            case 'k':
                len--;
                factor = KB;
                break;

            case 'm':
            case 'M':
                len--;
                factor = MB;
                break;

            default:
                factor = 1;
            }

            value = mc_atoi(optarg, len);
            if (value <= 0) {
                log_stderr("twemcache: option -I requires a non zero number");
                return MC_ERROR;
            }

            settings.slab_size = (size_t)value * factor;

            if (value % MC_ALIGNMENT != 0) {
                log_stderr("twemcache: value of option -I must be %zu bytes "
                           "aligned", MC_ALIGNMENT);
                return MC_ERROR;
            }

            if (settings.slab_size < SLAB_MIN_SIZE) {
                log_stderr("twemcache: slab size must be at least %zu bytes",
                           SLAB_MIN_SIZE);
                return MC_ERROR;
            }

            if (settings.slab_size > SLAB_MAX_SIZE) {
                log_stderr("twemcache: slab size cannot be larger than %zu "
                           "bytes", SLAB_MAX_SIZE);
                return MC_ERROR;
            }

            break;

        case 'z':
            parse_profile = 1;
            profile_optarg = optarg;
            break;

        case '?':
            switch (optopt) {
            case 'o':
            case 'P':
                log_stderr("twemcache: option -%c requires a file name", optopt);
                break;

            case 's':
                log_stderr("twemcache: option -%c requires a path name", optopt);
                break;

            case 'v':
            case 'A':
            case 'e':
            case 't':
            case 'R':
            case 'c':
            case 'b':
            case 'p':
            case 'U':
            case 'a':
            case 'M':
            case 'f':
            case 'm':
            case 'n':
                log_stderr("twemcache: option -%c requires a number", optopt);
                break;

            case 'u':
            case 'l':
            case 'I':
            case 'z':
                log_stderr("twemcache: option -%c requires a string", optopt);
                break;

            default:
                log_stderr("twemcache: invalid option -- '%c'", optopt);
                break;
            }

            return MC_ERROR;

        default:
            log_stderr("twemcache: invalid option -- '%c'", optopt);
            return MC_ERROR;
        }
    }

    if (tcp_specified && !udp_specified) {
        settings.udpport = settings.port;
    } else if (udp_specified && !tcp_specified) {
        settings.port = settings.udpport;
    }

    return MC_OK;
}

static rstatus_t
mc_maximize_core(void)
{
    int status;
    struct rlimit rlim, new_rlim; /* current and new rlimit */

    status = getrlimit(RLIMIT_CORE, &rlim);
    if (status < 0) {
        log_stderr("twemcache: getrlimit(RLIMIT_CORE) failed: %s",
                   strerror(errno));
        return MC_ERROR;
    }

    /*
     * First try raising to infinity. If that fails, try bringing
     * the soft limit to the current hard limit.
     */
    new_rlim.rlim_cur = RLIM_INFINITY;
    new_rlim.rlim_max = RLIM_INFINITY;

    status = setrlimit(RLIMIT_CORE, &new_rlim);
    if (status < 0) {
        new_rlim.rlim_cur = rlim.rlim_max;
        new_rlim.rlim_max = rlim.rlim_max;

        status = setrlimit(RLIMIT_CORE, &new_rlim);
        if (status == 0) {
            return MC_OK;
        }

        log_stderr("twemcache: setrlimit(RLIMIT_CORE, %"PRIu64") failed: %s",
                   (uint64_t)rlim.rlim_cur, strerror(errno));
        return MC_ERROR;
    }

    return MC_OK;
}

static rstatus_t
mc_set_maxconns(void)
{
    int status, maxfiles;
    struct rlimit rlim;

    status = getrlimit(RLIMIT_NOFILE, &rlim);
    if (status != 0) {
        log_stderr("twemcache: getrlimit(RLIMIT_NOFILE) failed: %s",
                   strerror(errno));
        return MC_ERROR;
    }

    maxfiles = settings.maxconns;

    rlim.rlim_cur = MAX(rlim.rlim_cur, maxfiles);
    rlim.rlim_max = MAX(rlim.rlim_max, rlim.rlim_cur);

    status = setrlimit(RLIMIT_NOFILE, &rlim);
    if (status != 0) {
        log_stderr("twemcache: setting open files limit to %d failed: %s",
                   maxfiles, strerror(errno));
        log_stderr("twemcache: try running as root or request smaller "
                   "--max-conns value");
        return MC_ERROR;
    }

    return MC_OK;
}

static rstatus_t
mc_set_user(void)
{
    int status;
    struct passwd *pw;
    uid_t uid, euid;
    char *uname;

    uname = settings.username;

    uid = getuid();
    euid = geteuid();

    if (uid != 0 && euid != 0) {
        if (uname != NULL) {
            log_stderr("twemcache: -u option is only effective when run "
                       "as root");
            return MC_ERROR;
        }
        return MC_OK;
    }

    if (uname == NULL) {
        log_stderr("twemcache: cannot run as root without the -u option");
        return MC_ERROR;
    }

    pw = getpwnam(settings.username);
    if (pw == NULL) {
        log_stderr("twemcache: cannot find user '%s' to switch to", uname);
        return MC_ERROR;
    }

    status = setgid(pw->pw_gid);
    if (status < 0) {
        log_stderr("twemcache: setting group id to user '%s' failed: %s",
                   uname, strerror(errno));
    }

    status = setuid(pw->pw_uid);
    if (status < 0) {
        log_stderr("twemcache: setting user id to user '%s' failed: %s",
                   uname, strerror(errno));
        return MC_ERROR;
    }

    return MC_OK;
}

/*
 * Generate slab class sizes from a geometric sequence with the initial
 * term equal to minimum item chunk size (--min-item-chunk-size) and
 * the common ratio equal to factor (--factor)
 */
static rstatus_t
mc_generate_profile(void)
{
    size_t *profile = settings.profile; /* slab profile */
    uint8_t id;                         /* slab class id */
    size_t item_sz, last_item_sz;       /* current and last item chunk size */
    size_t min_item_sz, max_item_sz;    /* min and max item chunk size */

    ASSERT(settings.chunk_size % MC_ALIGNMENT == 0);
    ASSERT(settings.chunk_size <= slab_size());

    min_item_sz = settings.chunk_size;
    max_item_sz = slab_size();
    id = SLABCLASS_MIN_ID;
    item_sz = min_item_sz;

    while (id < SLABCLASS_MAX_ID && item_sz < max_item_sz) {
        /* save the cur item chunk size */
        last_item_sz = item_sz;
        profile[id] = item_sz;
        id++;

        /* get the next item chunk size */
        item_sz *= settings.factor;
        if (item_sz == last_item_sz) {
            item_sz++;
        }
        item_sz = MC_ALIGN(item_sz, MC_ALIGNMENT);
    }

    /* last profile entry always has a 1 item/slab of maximum size */
    profile[id] = max_item_sz;
    settings.profile_last_id = id;
    settings.max_chunk_size = max_item_sz;

    return MC_OK;
}

/*
 * Generate slab class sizes based on the sequence specified by the input
 * profile string (--slab-profile)
 */
static rstatus_t
mc_parse_profile(void)
{
    size_t *profile;
    uint8_t id;
    char *ptr;
    bool eos;

    profile = settings.profile;
    ptr = profile_optarg;
    eos = false;
    id = SLABCLASS_MIN_ID - 1;

    while (id < SLABCLASS_MAX_ID && !eos) {
        char buf[MC_UINT32_MAXLEN], *comma;
        int len;
        uint32_t item_sz;

        comma = strchr(ptr, ',');
        if (comma != NULL) {
            len = comma - ptr;
        } else {
            len = strlen(ptr);
            eos = true;
        }

        if (len >= MC_UINT32_MAXLEN) {
            log_stderr("twemcache: profile value in '%s' is out of range",
                       profile_optarg);
            return MC_ERROR;
        }

        memcpy(buf, ptr, len);
        buf[len] = '\0';

        if (!mc_strtoul(buf, &item_sz)) {
            log_stderr("twemcache: %s is not a valid number: %s", buf,
                       strerror(errno));
            return MC_ERROR;
        }

        if (item_sz % MC_ALIGNMENT != 0) {
            log_stderr("twemcache: item chunk size must be %zu bytes aligned",
                       MC_ALIGNMENT);
            return MC_ERROR;
        }

        if (item_sz < ITEM_MIN_CHUNK_SIZE) {
            log_stderr("twemcache: item chunk size cannot be less than %d "
                       "bytes", ITEM_MIN_CHUNK_SIZE);
            return MC_ERROR;
        }

        if (item_sz > slab_size()) {
            log_stderr("twemcache: item chunk size cannot be more than %zu "
                       "bytes", slab_size());
            return MC_ERROR;
        }

        if (id >= SLABCLASS_MIN_ID && item_sz <= profile[id]) {
            log_stderr("twemcache: item chunk sizes must be ascending and "
                       "> %zu bytes apart", MC_ALIGNMENT);
            return MC_ERROR;
        }

        id++;
        profile[id] = (size_t)item_sz;
        ptr = comma + 1;
    }

    if (!eos) {
        log_stderr("twemcache: too many sizes, keep it under %d",
                   SLABCLASS_MAX_IDS);
        return MC_ERROR;
    }

    settings.chunk_size = profile[SLABCLASS_MIN_ID];
    settings.profile_last_id = id;
    settings.max_chunk_size = profile[id];

    return MC_OK;
}

/*
 * Set the slab profile in settings.profile. The last slab id is returned
 * in settings.last_slab_id
 */
static rstatus_t
mc_set_profile(void)
{
    /*
     * There are two ways to create a slab size profile:
     *
     * - Natually Grown:
     *   The lowest slab class will start with settings.chunk_size and
     *   grow by the expansion factor for the next slab class, until maximum
     *   number of slab classes or maximum item size is reached. Size of
     *   the last slab class will always be that of the largest item.
     *
     * - User specified:
     *   Users provide the data sizes they expect to store in twemcache through
     *   command line (--slab-profile). Slab classes will be tailored to host
     *   only those data sizes.
     *
     * User specified profile supercedes naturally grown profile if provided.
     * This means ---slab-profile option supercedes options --factor, and
     * --min-item-chunk-size when present.
     */

    if (parse_profile) {
        return mc_parse_profile();
    }

    return mc_generate_profile();
}

static void
mc_print_sizes(void)
{
    log_stderr("item_hdr_size %zu", ITEM_HDR_SIZE);
    log_stderr("item_chunk_size %zu", settings.chunk_size);
    log_stderr("slab_hdr_size %zu", SLAB_HDR_SIZE);
    log_stderr("slab_size %zu", settings.slab_size);
}

static void
mc_print(void)
{
    loga("%s-%s started on pid %d with %d worker threads", PACKAGE,
         MC_VERSION_STRING, settings.pid, settings.num_workers);

    loga("configured with debug logs %s, asserts %s, panic %s, stats %s, "
         "klog %s", MC_DEBUG_LOG ? "enabled" : "disabled",
         MC_ASSERT_LOG ? "enabled" : "disabled",
         MC_ASSERT_PANIC ? "enabled" : "disabled",
         MC_DISABLE_STATS ? "disabled" : "enabled",
         MC_DISABLE_KLOG ? "disabled" : "enabled");

    slab_print();
}

int
main(int argc, char **argv)
{
    rstatus_t status;

    mc_set_default_options();

    status = mc_get_options(argc, argv);
    if (status != MC_OK) {
        mc_show_usage();
        exit(1);
    }

    if (show_version) {
        log_stderr("This is %s-%s" CRLF, PACKAGE, MC_VERSION_STRING);

        if (show_help) {
            mc_show_usage();
        }

        if (show_stats_description) {
            stats_describe();
        }

        if (show_sizes) {
            mc_print_sizes();
        }

        exit(0);
    }

    if (settings.max_corefile) {
        status = mc_maximize_core();
        if (status != MC_OK) {
            exit(1);
        }
    }

    status = mc_set_maxconns();
    if (status != MC_OK) {
        exit(1);
    }

    status = mc_set_user();
    if (status != MC_OK) {
        exit(1);
    }

    if (settings.daemonize) {
        status = mc_daemonize(settings.max_corefile);
        if (status != MC_OK) {
            exit(1);
        }
    }

    settings.pid = getpid();

    if (settings.pid_filename != NULL) {
        status = mc_create_pidfile();
        if (status != MC_OK) {
            exit(1);
        }
    }

    if (settings.lock_page) {
        status = mc_lock_page();
        if (status != MC_OK) {
            exit(1);
        }
    }

    status = mc_set_profile();
    if (status != MC_OK) {
        exit(1);
    }

    status = core_init();
    if (status != MC_OK) {
        exit(1);
    }

    mc_print();

    status = core_loop();
    if (status != MC_OK) {
        exit(1);
    }

    mc_remove_pidfile();

    return 0;
}
