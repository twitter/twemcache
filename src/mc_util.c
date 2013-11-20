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
#include <ctype.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <execinfo.h>

#include <mc_core.h>

int
mc_set_blocking(int sd)
{
    int flags;

    flags = fcntl(sd, F_GETFL, 0);
    if (flags < 0) {
        return flags;
    }

    return fcntl(sd, F_SETFL, flags & ~O_NONBLOCK);
}

int
mc_set_nonblocking(int sd)
{
    int flags;

    flags = fcntl(sd, F_GETFL, 0);
    if (flags < 0) {
        return flags;
    }

    return fcntl(sd, F_SETFL, flags | O_NONBLOCK);
}

int
mc_set_reuseaddr(int sd)
{
    int reuse;
    socklen_t len;

    reuse = 1;
    len = sizeof(reuse);

    return setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, &reuse, len);
}

/*
 * Disable Nagle algorithm on TCP socket.
 *
 * This option helps to minimize transmit latency by disabling coalescing
 * of data to fill up a TCP segment inside the kernel. Sockets with this
 * option must use readv() or writev() to do data transfer in bulk and
 * hence avoid the overhead of small packets.
 */
int
mc_set_tcpnodelay(int sd)
{
    int nodelay;
    socklen_t len;

    nodelay = 1;
    len = sizeof(nodelay);

    return setsockopt(sd, IPPROTO_TCP, TCP_NODELAY, &nodelay, len);
}

int
mc_set_keepalive(int sd)
{
    int keepalive;
    socklen_t len;

    keepalive = 1;
    len = sizeof(keepalive);

    return setsockopt(sd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, len);
}

int
mc_set_linger(int sd, int timeout)
{
    struct linger linger;
    socklen_t len;

    linger.l_onoff = 1;
    linger.l_linger = timeout;

    len = sizeof(linger);

    return setsockopt(sd, SOL_SOCKET, SO_LINGER, &linger, len);
}

int
mc_unset_linger(int sd)
{
    struct linger linger;
    socklen_t len;

    linger.l_onoff = 0;
    linger.l_linger = 0;

    len = sizeof(linger);

    return setsockopt(sd, SOL_SOCKET, SO_LINGER, &linger, len);
}

int
mc_set_sndbuf(int sd, int size)
{
    socklen_t len;

    len = sizeof(size);

    return setsockopt(sd, SOL_SOCKET, SO_SNDBUF, &size, len);
}

int
mc_set_rcvbuf(int sd, int size)
{
    socklen_t len;

    len = sizeof(size);

    return setsockopt(sd, SOL_SOCKET, SO_RCVBUF, &size, len);
}

int
mc_get_soerror(int sd)
{
    int status, err;
    socklen_t len;

    err = 0;
    len = sizeof(err);

    status = getsockopt(sd, SOL_SOCKET, SO_ERROR, &err, &len);
    if (status == 0) {
        errno = err;
    }

    return status;
}

int
mc_get_sndbuf(int sd)
{
    int status, size;
    socklen_t len;

    size = 0;
    len = sizeof(size);

    status = getsockopt(sd, SOL_SOCKET, SO_SNDBUF, &size, &len);
    if (status < 0) {
        return status;
    }

    return size;
}

int
mc_get_rcvbuf(int sd)
{
    int status, size;
    socklen_t len;

    size = 0;
    len = sizeof(size);

    status = getsockopt(sd, SOL_SOCKET, SO_RCVBUF, &size, &len);
    if (status < 0) {
        return status;
    }

    return size;
}

void
mc_maximize_sndbuf(int sd)
{
    int status, min, max, avg;

    /* start with the default size */
    min = mc_get_sndbuf(sd);
    if (min < 0) {
        return;
    }

    /* binary-search for the real maximum */
    max = 256 * MB;

    while (min <= max) {
        avg = (min + max) / 2;
        status = mc_set_sndbuf(sd, avg);
        if (status != 0) {
            max = avg - 1;
        } else {
            min = avg + 1;
        }
    }
}

int
_mc_atoi(uint8_t *line, size_t n)
{
    int value;

    if (n == 0) {
        return -1;
    }

    for (value = 0; n--; line++) {
        if (*line < '0' || *line > '9') {
            return -1;
        }

        value = value * 10 + (*line - '0');
    }

    if (value < 0) {
        return -1;
    }

    return value;
}

bool
mc_valid_port(int n)
{
    if (n < 1 || n > UINT16_MAX) {
        return false;
    }

    return true;
}

static void
mc_skip_space(const char **str, size_t *len)
{
    while (*len > 0 && isspace(**str)) {
        (*str)++;
        (*len)--;
    }
}

bool
mc_strtoull_len(const char *str, uint64_t *out, size_t len)
{
    *out = 0ULL;

    mc_skip_space(&str, &len);

    while (len > 0 && (*str) >= '0' && (*str) <= '9') {
        if (*out >= UINT64_MAX / 10) {
            /*
             * At this point the integer is considered out of range,
             * by doing so we convert integers up to (UINT64_MAX - 6)
             */
            return false;
        }
        *out = *out * 10 + *str - '0';
        str++;
        len--;
    }

    mc_skip_space(&str, &len);

    if (len == 0) {
        return true;
    } else {
        return false;
    }
}

bool
mc_strtoull(const char *str, uint64_t *out)
{
    char *endptr;
    unsigned long long ull;

    errno = 0;
    *out = 0ULL;

    ull = strtoull(str, &endptr, 10);

    if (errno == ERANGE) {
        return false;
    }

    if (isspace(*endptr) || (*endptr == '\0' && endptr != str)) {
        if ((long long) ull < 0) {
            /*
             * Only check for negative signs in the uncommon case when
             * the unsigned number is so big that it's negative is a
             * signed number
             */
            if (strchr(str, '-') != NULL) {
                return false;
            }
        }

        *out = ull;

        return true;
    }

    return false;
}

bool
mc_strtoll(const char *str, int64_t *out)
{
    char *endptr;
    long long ll;

    errno = 0;
    *out = 0LL;

    ll = strtoll(str, &endptr, 10);

    if (errno == ERANGE) {
        return false;
    }

    if (isspace(*endptr) || (*endptr == '\0' && endptr != str)) {
        *out = ll;
        return true;
    }

    return false;
}

bool
mc_strtoul(const char *str, uint32_t *out)
{
    char *endptr;
    unsigned long l;

    errno = 0;
    *out = 0UL;

    l = strtoul(str, &endptr, 10);

    if (errno == ERANGE) {
        return false;
    }

    if (isspace(*endptr) || (*endptr == '\0' && endptr != str)) {
        if ((long) l < 0) {
            /*
             * Only check for negative signs in the uncommon case when
             * the unsigned number is so big that it's negative as a
             * signed number
             */
            if (strchr(str, '-') != NULL) {
                return false;
            }
        }

        *out = l;

        return true;
    }

    return false;
}

bool
mc_strtol(const char *str, int32_t *out)
{
    char *endptr;
    long l;

    *out = 0L;
    errno = 0;

    l = strtol(str, &endptr, 10);

    if (errno == ERANGE) {
        return false;
    }

    if (isspace(*endptr) || (*endptr == '\0' && endptr != str)) {
        *out = l;
        return true;
    }

    return false;
}

bool
mc_str2oct(const char *str, int32_t *out)
{
    char *endptr;
    long l;

    *out = 0L;
    errno = 0;

    l = strtol(str, &endptr, 8);

    if (errno == ERANGE) {
        return false;
    }

    if (isspace(*endptr) || (*endptr == '\0' && endptr != str)) {
        *out = l;
        return true;
    }

    return false;
}

int
_vscnprintf(char *buf, size_t size, const char *fmt, va_list args)
{
    int i;

    i = vsnprintf(buf, size, fmt, args);

    /*
     * The return value is the number of characters which would be written
     * into buf not including the trailing '\0'. If size is == 0 the
     * function returns 0.
     *
     * On error, the function also returns 0. This is to allow idiom such
     * as len += _vscnprintf(...)
     *
     * See: http://lwn.net/Articles/69419/
     */
    if (i <= 0) {
        return 0;
    }

    if (i < size) {
        return i;
    }

    return size - 1;
}

int
_scnprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list args;
    int i;

    va_start(args, fmt);
    i = _vscnprintf(buf, size, fmt, args);
    va_end(args);

    return i;
}

void
mc_assert(const char *cond, const char *file, int line, int panic)
{
    log_error("assert '%s' failed @ (%s, %d)", cond, file, line);
    if (panic) {
        mc_stacktrace(1);
        abort();
    }
}

void
mc_stacktrace(int skip_count)
{
#ifdef MC_BACKTRACE
    void *stack[64];
    char **symbols;
    int size, i, j;

    size = backtrace(stack, 64);
    symbols = backtrace_symbols(stack, size);
    if (symbols == NULL) {
        return;
    }

    skip_count++; /* skip the current frame also */

    for (i = skip_count, j = 0; i < size; i++, j++) {
        loga("[%d] %s", j, symbols[i]);
    }

    mc_libc_free(symbols);
#endif
}

void
mc_resolve_peer(int sd, char *buf, int size)
{
    int status;
    struct sockinfo si;
    struct sockaddr *addr;
    socklen_t addrlen;
    struct sockaddr_in *in;
    struct sockaddr_in6 *in6;
    struct sockaddr_un *un;
    const char *p;
    int plen;
    uint16_t port;

    memset(&si, 0, sizeof(si));
    addr = (struct sockaddr *)&si.addr;
    addrlen = sizeof(si.addr);

    status = getpeername(sd, addr, &addrlen);
    if (status < 0) {
        goto error;
    }

    switch (addr->sa_family) {
    case AF_INET:
        in = &si.addr.in;

        p = inet_ntop(AF_INET, &in->sin_addr, buf, size);
        if (p == NULL) {
            goto error;
        }
        plen = mc_strlen(p);

        port = ntohs(in->sin_port);
        if (port == 0) {
            goto error;
        }

        mc_snprintf(p + plen, size - plen, ":%d", port);
        break;

    case AF_INET6:
        in6 = &si.addr.in6;

        p = inet_ntop(AF_INET6, &in6->sin6_addr, buf, size);
        if (p == NULL) {
            goto error;
        }
        plen = mc_strlen(p);

        port = ntohs(in6->sin6_port);
        if (port == 0) {
            goto error;
        }

        mc_snprintf(p + plen, size - plen, ":%d", port);
        break;

    case AF_UNIX:
        un = &si.addr.un;
        mc_snprintf(buf, size, "%s", un->sun_path);
        break;

    default:
        NOT_REACHED();
        break;
    }

    return;

error:
    mc_snprintf(buf, size, "%s", "-");
}
