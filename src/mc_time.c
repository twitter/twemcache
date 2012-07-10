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

#include <mc_core.h>

extern struct event_base *main_base;
extern struct settings settings;

/*
 * From memcache protocol specification:
 *
 * Some commands involve a client sending some kind of expiration time
 * (relative to an item or to an operation requested by the client) to
 * the server. In all such cases, the actual value sent may either be
 * Unix time (number of seconds since January 1, 1970, as a 32-bit
 * value), or a number of seconds starting from current time. In the
 * latter case, this number of seconds may not exceed 60*60*24*30 (number
 * of seconds in 30 days); if the number sent by a client is larger than
 * that, the server will consider it to be real Unix time value rather
 * than an offset from current time.
 */
#define TIME_MAXDELTA   (time_t)(60 * 60 * 24 * 30)

/*
 * Time when process was started expressed as absolute unix timestamp
 * with a time_t type
 */
static time_t process_started;

/*
 * We keep a cache of the current time of day in a global variable now
 * that is updated periodically by a timer event every second. This
 * saves us a bunch of time() system calls because we really only need
 * to get the time once a second, whereas there can be tens of thosands
 * of requests a second.
 *
 * Also keeping track of time as relative to server-start timestamp
 * instead of absolute unix timestamps gives us a space savings on
 * systems where sizeof(time_t) > sizeof(unsigned int)
 *
 * So, now actually holds 32-bit seconds since the server start time.
 */
static volatile rel_time_t now;

void
time_update(void)
{
    int status;
    struct timeval timer;

    status = gettimeofday(&timer, NULL);
    if (status < 0) {
        log_error("gettimeofday failed: %s", strerror(errno));
    }
    now = (rel_time_t) (timer.tv_sec - process_started);

    log_debug(LOG_PVERB, "time updated to %u", now);
}

rel_time_t
time_now(void)
{
    return now;
}

time_t
time_now_abs(void)
{
    return process_started + (time_t)now;
}

time_t
time_started(void)
{
    return process_started;
}

/*
 * Given time value that's either unix time or delta from current unix
 * time, return the time relative to process start.
 */
rel_time_t
time_reltime(time_t exptime)
{
    if (exptime == 0) { /* 0 means never expire */
        return 0;
    }

    if (exptime > TIME_MAXDELTA) {
        /*
         * If item expiration is at or before the server_started, give
         * it an expiration time of 1 second after the server started
         * becasue because 0 means don't expire.  Without this, we would
         * underflow and wrap around to some large value way in the
         * future, effectively making items expiring in the past
         * really expiring never
         */
        if (exptime <= process_started) {
            return (rel_time_t)1;
        }

        return (rel_time_t)(exptime - process_started);
    } else {
        return (rel_time_t)(exptime + now);
    }
}

static void
time_clock_handler(int fd, short which, void *arg)
{
    static struct event clockevent;
    static bool initialized = false;
    struct timeval t = {.tv_sec = 1, .tv_usec = 0};

    if (initialized) {
        /* only delete the event if it's actually there. */
        evtimer_del(&clockevent);
    } else {
        initialized = true;
    }

    evtimer_set(&clockevent, time_clock_handler, 0);
    event_base_set(main_base, &clockevent);
    evtimer_add(&clockevent, &t);

    time_update();
}

void
time_init(void)
{
    /*
     * Make the time we started always be 2 seconds before we really
     * did, so time_now(0) - time.started is never zero. If so, things
     * like 'settings.oldest_live' which act as booleans as well as
     * values are now false in boolean context.
     */
    process_started = time(NULL) - 2;

    time_clock_handler(0, 0, NULL);

    log_debug(LOG_DEBUG, "process started at %"PRId64, (int64_t)process_started);
}
