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

#ifndef _MC_KLOG_H_
#define _MC_KLOG_H_

#define KLOG_DEFAULT_INTVL    1000 /* logging interval in msec */
#define KLOG_MIN_INTVL        100  /* do not allow shorter intervals to be set */
#define KLOG_DEFAULT_SMP_RATE 100  /* log 1 out of 100 commands by default */
#define KLOG_DEFAULT_HOST     "-"
#define KLOG_DEFAULT_ENTRY    512

/*
 * Max width for different fields
 *   sockaddr: 46 (peer name in string format)
 *   cmd: 7 ("prepend" which is the longest command)
 *   key: 250 (KEY_MAX_LENTH)
 *   time: 27 ("[01/Jan/2000:00:00:00 -0700]"
 *   other: 6
 *  Total: 336
 */
#define KLOG_TIMESTR_SIZE 32    /* sizeof(""[01/Jan/2000:00:00:00 -0700]"" - 1 */
#define KLOG_ENTRY_SIZE   384   /* max allowed length of a command log */

/*
 * Thread-local circular buffer:
 *   r_idx : read pointer, collector uses it to determine where to read
 *   w_idx : write pointer, worker uses it to determine where to write
 *   buf   : hold logs contiguously
 *
 * Special condition:
 *   (r_idx == w_idx) : empty buffer
 *
 * Note:
 *   We assume read can always catch up with write to make all operations
 *   lockless: worker only moves w_idx, and klogger moves r_idx. If this
 *   is not true, then we will be losing entries which gets tracked by
 *   the discarded counter.
 */
struct kbuf {
    uint32_t     magic;                       /* magic (const) */
    volatile int r_idx;                       /* read index */
    volatile int w_idx;                       /* write index */

    int          entries;                     /* # entries */
    int          errors;                      /* # errors */

    char         *buf;                        /* buffer */
    int          size;                        /* buffer size */

    char         entry[KLOG_ENTRY_SIZE];      /* klog entry */
    char         timestr[KLOG_TIMESTR_SIZE];  /* formatted timestamp */
};

#define KBUF_MAGIC  0xdeadf00d

#if defined MC_DISABLE_KLOG && MC_DISABLE_KLOG == 1

#define klog_write(_peer, _rtype, _cmdkey, _cmdkey_len, _status, _res_len)

#define klog_collect()

#else

#define klog_write(_peer, _rtype, _cmdkey, _cmdkey_len, _status, _res_len) \
    _klog_write(_peer, _rtype, _cmdkey, _cmdkey_len, _status, _res_len)

#define klog_collect()                      \
    _klog_collect()

#endif

bool klog_enabled(void);
void klog_set_interval(long interval);

struct kbuf *klog_buf_create(void);
void klog_buf_destroy(struct kbuf *kbuf);

rstatus_t klog_init(void);
void klog_deinit(void);

void _klog_write(const char *peer, req_type_t rtype, const char *cmdkey,
    int cmdkey_len, int status, int res_len);
void _klog_collect(void);

#endif
