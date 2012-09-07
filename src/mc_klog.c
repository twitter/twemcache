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

#include <unistd.h>
#include <stdio.h>

#include <mc_core.h>

extern struct settings settings;
extern struct thread_worker *threads;
extern struct thread_key keys;

#define CIRCULAR_INCR(_i, _d, _s)   ((_i + _d) % (_s))

#define KLOG_W3C_TIMEFMT        "%d/%b/%Y:%T %z"
#define KLOG_GET_FMT            "%s - [%s] \"get %.*s\" %d %d\n"
#define KLOG_GETS_FMT           "%s - [%s] \"gets %.*s\" %d %d\n"
#define KLOG_FMT                "%s - [%s] \"%.*s\" %d %d\n"
#define KLOG_DISCARD_MSG_SIZE   30
#define KLOG_MAX_SIZE           GB


static int fd;  /* klogger file descriptor */
static int kfs; /* klogger file size */

bool
klog_enabled(void)
{
    if (MC_DISABLE_KLOG) {
        return false;
    }

    if (settings.klog_running == false) {
        return false;
    }

    return true;
}

/*
 * Returns the remaining space for any new bytes in kbuf.
 */
static int
klog_remain(struct kbuf *kbuf)
{
    int remain;

    /*
     * The collector thread updates r_idx without a lock and the worker
     * thread updates w_idx without a lock. In order to compute the
     * remaining space in kbuf we might end up reading stale r_idx and
     * w_idx values. But that is ok, because even with stale values
     * we might just underestimate the remaining space.
     *
     * Cases:
     *
     *  1. r_idx <= w_idx
     *     remain = (size - w_idx + r_idx) - 1
     *
     *  +--------------------------+
     *  ||||                   |||||
     *  +---|------------------|---+
     *      ^                  ^
     *      |                  |
     *      \                  \
     *      r_idx              w_idx
     *
     *
     *  2. r_idx > w_idx
     *     remain = (r_idx - w_idx) - 1
     *
     *  +--------------------------+
     *  |   ||||||||||||||||||     |
     *  +---|------------------|---+
     *      ^                  ^
     *      |                  |
     *      \                  \
     *      w_idx              r_idx
     *
     */
    remain = kbuf->r_idx - kbuf->w_idx;
    if (remain <= 0) {
        remain += kbuf->size;
    }

    return remain - 1;
}

void
klog_set_interval(long interval)
{
    settings.klog_intvl.tv_sec = interval / 1000000;
    settings.klog_intvl.tv_usec = interval % 1000000;
}

struct kbuf *
klog_buf_create(void)
{
    struct kbuf *kbuf;
    char *buf;
    int size;

    size = KLOG_ENTRY_SIZE * settings.klog_entry;

    buf = mc_alloc(sizeof(*kbuf) + size);
    if (buf == NULL) {
        return NULL;
    }

    /*
     * kbuf header is at the tail end of the kbuf. This enables us to catch
     * buffer overrun early by asserting on the magic value.
     *
     *   <-------kbuf->size--------->
     *   +--------------------------+----------------+
     *   |       kbuf buffer        |  kbuf header   |
     *   |                          | (struct kbuf)  |
     *   +--------------------------+----------------+
     *   ^                          ^
     *   |                          |
     *   \                          |
     *   kbuf->buf                  \
     *                               kbuf
     *
     */
    kbuf = (struct kbuf *)(buf + size);
    kbuf->magic = KBUF_MAGIC;
    kbuf->r_idx = 0;
    kbuf->w_idx = 0;

    kbuf->entries = 0;
    kbuf->errors = 0;

    kbuf->buf = buf;
    kbuf->size = size;

    log_debug(LOG_VVERB, "create kbuf %p", kbuf);

    return kbuf;
}

void
klog_buf_destroy(struct kbuf *kbuf)
{
    char *buf;

    log_debug(LOG_VVERB, "destroy kbuf %p", kbuf);

    buf = (char *)kbuf - kbuf->size;
    mc_free(buf);
}

rstatus_t
klog_init(void)
{
    fd = -1;

    if (settings.klog_name == NULL) {
        return MC_OK;
    }

    fd = open(settings.klog_name, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) {
        log_error("open klog file '%s' failed: %s", settings.klog_name,
                  strerror(errno));
        return MC_ERROR;
    }

    log_debug(LOG_VERB, "klog init with file '%s' on fd %d",
              settings.klog_name, fd);

    return MC_OK;
}

void
klog_deinit(void)
{
    log_debug(LOG_VERB, "klog deinit");
    close(fd);
}

/*
 * This is the poor man's version of log rotation- one backup file only,
 * no compression. Yet it is much more responsive and precise in size control
 * than logrotate, which is scheduled to run only once an hour.
 *
 * Close current klog file, rename it what is specified by settings.klog_backup
 * and reopen the klog file.
 */
static void
klog_reopen(void)
{
    int ret;

    if (fd < 0) {
        return;
    }

    ASSERT(settings.klog_name != NULL);
    ASSERT(settings.klog_backup != NULL);

    close(fd);

    ret = rename(settings.klog_name, settings.klog_backup);
    if (ret < 0) {
        log_error("rename old klog file '%s' to '%s' failed, ignored: %s",
                  settings.klog_name, settings.klog_backup,
                  strerror(errno));
    }

    fd = open(settings.klog_name, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) {
        log_error("reopen klog file '%s' failed, disabling klogger: %s",
                  settings.klog_name, strerror(errno));

        settings.klog_running = false;
    }
}

/*
 * Reads remaining message from klog buffer and writes them to the
 * klogger output. On success updates read index - r_idx.
 *
 * Returns bytes read and successfully written to klogger output.
 */
static int
klog_read(struct kbuf *kbuf)
{
    int w_idx, r_idx;
    ssize_t n;
    int ret;

    ASSERT(kbuf->magic == KBUF_MAGIC);

    /*
     * A worker thread can update w_idx without a lock. So we save
     * a local copy to have a consistent value of w_idx throughout
     * this function.
     */
    w_idx = kbuf->w_idx;
    r_idx = kbuf->r_idx;
    ret = 0;

    if (r_idx <= w_idx) {
        /* no wrapping around */
        if (r_idx < w_idx) {
            n = mc_write(fd, &kbuf->buf[r_idx], w_idx - r_idx);
            if (n < 0) {
                goto error;
            }

            ret += n;
        }
    } else {
        /* read from r_idx to end of buf */
        n = mc_write(fd, &kbuf->buf[r_idx], kbuf->size - r_idx);
        if (n < 0) {
            goto error;
        }

        ret += n;

        /* read from start to w_idx */
        if (n == kbuf->size - r_idx) {
            /* first part written in whole */
            n = mc_write(fd, kbuf->buf, w_idx);
            if (n < 0) {
                goto error;
            }

            ret += n;
        }
    }

    if (ret > 0) {
        log_debug(LOG_VVERB, "klog read %d bytes at offset %d", ret, r_idx);
        kbuf->r_idx = CIRCULAR_INCR(r_idx, ret, kbuf->size);
    }

    kfs += ret;
    if (kfs > KLOG_MAX_SIZE) {
        klog_reopen();
        kfs = 0;
    }
    return ret;

error:
    kbuf->errors++;
    log_debug(LOG_DEBUG, "klog read failed: %s", strerror(errno));
    return 0;
}

/*
 * Format klog message and copy to a static buffer
 */
static int
klog_fmt(struct kbuf *kbuf, char *msg, const char *peer, req_type_t rtype,
         const char *cmdkey, size_t cmdkey_len, int status, int res_len)
{
    int len;
    time_t now_abs = time_now_abs();
    char *fmt;

    ASSERT(peer != NULL && cmdkey != NULL);
    ASSERT(kbuf->magic == KBUF_MAGIC);

    len = strftime(kbuf->timestr, KLOG_TIMESTR_SIZE, KLOG_W3C_TIMEFMT,
                   localtime(&now_abs));
    if (len == 0) {
        kbuf->errors++;
        log_debug(LOG_DEBUG, "strftime ts %"PRIu64" failed: %s",
                  (uint64_t)now_abs, strerror(errno));
        return 0;
    }

    /*
     * Given the different command format of multi get and gets we need to
     * log get and gets differently from the rest of the commands.
     *
     * If request type is GET or GETS, the next argument is the key only.
     * For the other requests, the next argument is the entire request header.
     */
    log_debug(LOG_VERB, "klog fmt message of type %d with cmdkey '%.*s' "
              "from %s", rtype, cmdkey_len, cmdkey, peer);

    switch (rtype) {
    case REQ_GET:
        fmt = KLOG_GET_FMT;
        break;

    case REQ_GETS:
        fmt = KLOG_GETS_FMT;
        break;

    default:
        fmt = KLOG_FMT;
        break;
    }

    len = mc_scnprintf(msg, KLOG_ENTRY_SIZE, fmt, peer, kbuf->timestr,
                       cmdkey_len, cmdkey, status, res_len);
    if (len >= KLOG_ENTRY_SIZE) {
        kbuf->errors++;
        log_debug(LOG_DEBUG, "klog fmt message of %d bytes is too long", len);
        return 0;
    }

    return len;
}

/*
 * Write a message to the next write location in the log buffer
 */
void
_klog_write(const char *peer, req_type_t rtype, const char *cmdkey,
            int cmdkey_len, int status, int res_len)
{
    struct kbuf *kbuf;
    int remain, ret;

    if (!klog_enabled()) {
        return;
    }

    kbuf = thread_get(keys.kbuf);
    if (kbuf == NULL) {
        return;
    }

    ASSERT(kbuf->magic == KBUF_MAGIC);

    kbuf->entries++;
    if (kbuf->entries % settings.klog_sampling_rate != 0) {
        stats_thread_incr(klog_skipped);
        return;
    }
    kbuf->entries = 0;

    /*
     * The collector thread can update r_idx without a lock. So, the
     * worker thread always gets a conservative esimate on the
     * remaining space for new bytes in the kbuf before continuing
     */
    remain = klog_remain(kbuf);
    if (remain < KLOG_ENTRY_SIZE) {
        stats_thread_incr(klog_discarded);

        log_debug(LOG_DEBUG, "discard an entry to prevent overwriting "
                  "r_idx %d w_idx %d", kbuf->r_idx, kbuf->w_idx);
        return;
    }

    remain = kbuf->size - kbuf->w_idx;
    if (remain < KLOG_ENTRY_SIZE) {
        /*
         * Log may wrap around. So we compose log in entry to
         * prevent overrun
         */
        ret = klog_fmt(kbuf, kbuf->entry, peer, rtype, cmdkey,
                       cmdkey_len, status, res_len);
        if (ret > remain) {
            /* log actually wraps around */
            memcpy(&kbuf->buf[kbuf->w_idx], kbuf->entry, remain);
            memcpy(&kbuf->buf[0], &kbuf->entry[remain], ret - remain);
        } else {
            memcpy(&kbuf->buf[kbuf->w_idx], kbuf->entry, ret);
        }
    } else {
        /* composing log directly in the main log kbuf */
        ret = klog_fmt(kbuf, &kbuf->buf[kbuf->w_idx], peer, rtype, cmdkey,
                       cmdkey_len, status, res_len);
    }

    if (ret > 0) {
        stats_thread_incr(klog_logged);

        log_debug(LOG_VERB, "klog write %d bytes at offset %d", ret,
                  kbuf->w_idx);
        kbuf->w_idx = CIRCULAR_INCR(kbuf->w_idx, ret, kbuf->size);
    }
}

/*
 * Main klogger logic that reads bytes from all the thread local kbuf
 * writes them to klogger output
 */
void
_klog_collect(void)
{
    int i, sum;
    struct kbuf *kbuf;

    if (!klog_enabled()) {
        return;
    }

    for (sum = 0, i = 0; i < settings.num_workers; i++) {
        kbuf = threads[i].kbuf;
        sum += klog_read(kbuf);
    }

    log_debug(LOG_PVERB, "klog collect %d bytes at time %u", sum, time_now());
}
