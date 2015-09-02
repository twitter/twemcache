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

#include <mc_core.h>

extern struct settings settings;

/*
 * Parsing tokens:
 *
 * COMMAND   KEY   FLAGS   EXPIRY   VLEN
 * set       <key> <flags> <expiry> <datalen> [noreply]\r\n<data>\r\n
 * add       <key> <flags> <expiry> <datalen> [noreply]\r\n<data>\r\n
 * replace   <key> <flags> <expiry> <datalen> [noreply]\r\n<data>\r\n
 * append    <key> <flags> <expiry> <datalen> [noreply]\r\n<data>\r\n
 * prepend   <key> <flags> <expiry> <datalen> [noreply]\r\n<data>\r\n
 * appendrl  <key> <flags> <expiry> <datalen> [noreply]\r\n<data>\r\n
 * prependrl <key> <flags> <expiry> <datalen> [noreply]\r\n<data>\r\n
 *
 * COMMAND   KEY   FLAGS   EXPIRY   VLEN      CAS
 * cas       <key> <flags> <expiry> <datalen> <cas> [noreply]\r\n<data>\r\n
 *
 * COMMAND   KEY
 * get       <key>\r\n
 * get       <key> [<key>]+\r\n
 * gets      <key>\r\n
 * gets      <key> [<key>]+\r\n
 * delete    <key> [noreply]\r\n
 *
 * COMMAND   KEY    DELTA
 * incr      <key> <value> [noreply]\r\n
 * decr      <key> <value> [noreply]\r\n
 *
 * COMMAND   SUBCOMMAND
 * quit\r\n
 * flush_all [<delay>] [noreply]\r\n
 * version\r\n
 * verbosity <num> [noreply]\r\n
 *
 * COMMAND   SUBCOMMAND CACHEDUMP_ID CACHEDUMP_LIMIT
 * stats\r\n
 * stats     <args>\r\n
 * stats     cachedump   <id>         <limit>\r\n
 *
 * COMMAND   SUBCOMMAND  AGGR_COMMAND
 * config    aggregate   <num>\r\n
 *
 * COMMAND   SUBCOMMAND  EVICT_COMMAND
 * config    evict       <num>\r\n
 *
 * COMMAND   SUBCOMMAND  NEW_LIMIT
 * config    maxbytes    <num>\r\n
 *
 * COMMAND   SUBCOMMAND  KLOG_COMMAND  KLOG_SUBCOMMAND
 * config    klog        run           start\r\n
 * config    klog        run           stop\r\n
 * config    klog        interval      reset\r\n
 * config    klog        interval      <val>\r\n
 * config    klog        sampling      reset\r\n
 * config    klog        sampling      <val>\r\n
 */

#define TOKEN_COMMAND           0
#define TOKEN_KEY               1
#define TOKEN_FLAGS             2
#define TOKEN_EXPIRY            3
#define TOKEN_VLEN              4
#define TOKEN_CAS               5
#define TOKEN_DELTA             2
#define TOKEN_SUBCOMMAND        1
#define TOKEN_CACHEDUMP_ID      2
#define TOKEN_CACHEDUMP_LIMIT   3
#define TOKEN_AGGR_COMMAND      2
#define TOKEN_EVICT_COMMAND     2
#define TOKEN_MAXBYTES_COMMAND  2
#define TOKEN_KLOG_COMMAND      2
#define TOKEN_KLOG_SUBCOMMAND   3
#define TOKEN_MAX               8

#define SUFFIX_MAX_LEN 44 /* =11+11+21+1 enough to hold " <uint32_t> <uint32_t> <uint64_t>\0" */

struct token {
    char   *val; /* token value */
    size_t len;  /* token length */
};

struct bound {
    struct {
        int min; /* min # token */
        int max; /* max # token */
    } b[2];      /* bound without and with noreply */
};

#define DEFINE_ACTION(_t, _min, _max, _nmin, _nmax) \
    { {{ _min, _max }, { _nmin, _nmax }} },
static struct bound ntoken_bound[] = {
    REQ_CODEC( DEFINE_ACTION )
};
#undef DEFINE_ACTION

#define strcrlf(m)                                                          \
    (*(m) == '\r' && *((m) + 1) == '\n')

#ifdef MC_LITTLE_ENDIAN

#define str4cmp(m, c0, c1, c2, c3)                                          \
    (*(uint32_t *) m == ((c3 << 24) | (c2 << 16) | (c1 << 8) | c0))

#define str5cmp(m, c0, c1, c2, c3, c4)                                      \
    (str4cmp(m, c0, c1, c2, c3) && (m[4] == c4))

#define str6cmp(m, c0, c1, c2, c3, c4, c5)                                  \
    (str4cmp(m, c0, c1, c2, c3) &&                                          \
        (((uint32_t *) m)[1] & 0xffff) == ((c5 << 8) | c4))

#define str7cmp(m, c0, c1, c2, c3, c4, c5, c6)                              \
    (str6cmp(m, c0, c1, c2, c3, c4, c5) && (m[6] == c6))

#define str8cmp(m, c0, c1, c2, c3, c4, c5, c6, c7)                          \
    (str4cmp(m, c0, c1, c2, c3) &&                                          \
        (((uint32_t *) m)[1] == ((c7 << 24) | (c6 << 16) | (c5 << 8) | c4)))

#define str9cmp(m, c0, c1, c2, c3, c4, c5, c6, c7, c8)                      \
    (str8cmp(m, c0, c1, c2, c3, c4, c5, c6, c7) && m[8] == c8)

#define str10cmp(m, c0, c1, c2, c3, c4, c5, c6, c7, c8, c9)                 \
    (str8cmp(m, c0, c1, c2, c3, c4, c5, c6, c7) &&                          \
        (((uint32_t *) m)[2] & 0xffff) == ((c9 << 8) | c8))

#define str11cmp(m, c0, c1, c2, c3, c4, c5, c6, c7, c8, c9, c10)            \
    (str10cmp(m, c0, c1, c2, c3, c4, c5, c6, c7, c8, c9) && (m[10] == c10))

#define str12cmp(m, c0, c1, c2, c3, c4, c5, c6, c7, c8, c9, c10, c11)       \
    (str8cmp(m, c0, c1, c2, c3, c4, c5, c6, c7) &&                          \
        (((uint32_t *) m)[2] == ((c11 << 24) | (c10 << 16) | (c9 << 8) | c8)))

#else

#define str4cmp(m, c0, c1, c2, c3)                                          \
    (m[0] == c0 && m[1] == c1 && m[2] == c2 && m[3] == c3)

#define str5cmp(m, c0, c1, c2, c3, c4)                                      \
    (str4cmp(m, c0, c1, c2, c3) && (m[4] == c4))

#define str6cmp(m, c0, c1, c2, c3, c4, c5)                                  \
    (str5cmp(m, c0, c1, c2, c3, c4) && m[5] == c5)

#define str7cmp(m, c0, c1, c2, c3, c4, c5, c6)                              \
    (str6cmp(m, c0, c1, c2, c3, c4, c5) && m[6] == c6)

#define str8cmp(m, c0, c1, c2, c3, c4, c5, c6, c7)                          \
    (str7cmp(m, c0, c1, c2, c3, c4, c5, c6) && m[7] == c7)

#define str9cmp(m, c0, c1, c2, c3, c4, c5, c6, c7, c8)                      \
    (str8cmp(m, c0, c1, c2, c3, c4, c5, c6, c7) && m[8] == c8)

#define str10cmp(m, c0, c1, c2, c3, c4, c5, c6, c7, c8, c9)                 \
    (str9cmp(m, c0, c1, c2, c3, c4, c5, c6, c7, c8) && m[9] == c9)

#define str11cmp(m, c0, c1, c2, c3, c4, c5, c6, c7, c8, c9, c10)            \
    (str10cmp(m, c0, c1, c2, c3, c4, c5, c6, c7, c8, c9) && m[10] == c10)

#define str12cmp(m, c0, c1, c2, c3, c4, c5, c6, c7, c8, c9, c10, c11)       \
    (str11cmp(m, c0, c1, c2, c3, c4, c5, c6, c7, c8, c9, c10) && m[11] == c11)

#endif

/*
 * Tokenize the request header and update the token array token with
 * pointer to start of each token and length. Note that tokens are
 * not null terminated.
 *
 * Returns total number of tokens. The last valid token is the terminal
 * token (value points to the first unprocessed character of the string
 * and length zero).
 */
static size_t
asc_tokenize(char *command, struct token *token, int ntoken_max)
{
    char *s, *e; /* start and end marker */
    int ntoken;  /* # tokens */

    ASSERT(command != NULL);
    ASSERT(token != NULL);
    ASSERT(ntoken_max > 1);

    for (s = e = command, ntoken = 0; ntoken < ntoken_max - 1; e++) {
        if (*e == ' ') {
            if (s != e) {
                /* save token */
                token[ntoken].val = s;
                token[ntoken].len = e - s;
                ntoken++;
            }
            s = e + 1;
        } else if (*e == '\0') {
            if (s != e) {
                /* save final token */
                token[ntoken].val = s;
                token[ntoken].len = e - s;
                ntoken++;
            }
            break;
        }
    }

    /*
     * If we scanned the whole string, the terminal value pointer is NULL,
     * otherwise it is the first unprocessed character.
     */
    token[ntoken].val = (*e == '\0') ? NULL : e;
    token[ntoken].len = 0;
    ntoken++;

    return ntoken;
}

static void
asc_write_string(struct conn *c, const char *str, size_t len)
{
    log_debug(LOG_VVERB, "write on c %d noreply %d str '%.*s'", c->sd,
              c->noreply, len, str);

    if (c->noreply) {
        c->noreply = 0;
        conn_set_state(c, CONN_NEW_CMD);
        return;
    }

    if ((len + CRLF_LEN) > c->wsize) {
        log_warn("server error on c %d for str '%.*s' because wbuf is not big "
                 "enough", c->sd, len, str);

        stats_thread_incr(server_error);
        str = "SERVER_ERROR";
        len = sizeof("SERVER_ERROR") - 1;
    }

    memcpy(c->wbuf, str, len);
    memcpy(c->wbuf + len, CRLF, CRLF_LEN);
    c->wbytes = len + CRLF_LEN;
    c->wcurr = c->wbuf;

    conn_set_state(c, CONN_WRITE);
    c->write_and_go = CONN_NEW_CMD;
}

static size_t
asc_rsp_stored(struct conn *c)
{
    const char *str = "STORED";
    size_t len = sizeof("STORED") - 1;

    asc_write_string(c, str, len);

    return len;
}

static size_t
asc_rsp_exists(struct conn *c)
{
    const char *str = "EXISTS";
    size_t len = sizeof("EXISTS") - 1;

    asc_write_string(c, str, len);

    return len;
}

static size_t
asc_rsp_not_found(struct conn *c)
{
    const char *str = "NOT_FOUND";
    size_t len = sizeof("NOT_FOUND") - 1;

    asc_write_string(c, str, len);

    return len;
}

static size_t
asc_rsp_not_stored(struct conn *c)
{
    const char *str = "NOT_STORED";
    size_t len = sizeof("NOT_STORED") - 1;

    asc_write_string(c, str, len);

    return len;
}

static size_t
asc_rsp_deleted(struct conn *c)
{
    const char *str = "DELETED";
    size_t len = sizeof("DELETED") - 1;

    asc_write_string(c, str, len);

    return len;
}

static size_t
asc_rsp_client_error(struct conn *c)
{
    const char *str = "CLIENT_ERROR";
    size_t len = sizeof("CLIENT_ERROR") - 1;

    stats_thread_incr(cmd_error);
    asc_write_string(c, str, len);

    return len;
}

size_t
asc_rsp_server_error(struct conn *c)
{
    const char *str = "SERVER_ERROR";
    size_t len = sizeof("SERVER_ERROR") - 1;

    stats_thread_incr(server_error);
    asc_write_string(c, str, len);

    return len;
}

static size_t
asc_rsp_ok(struct conn *c)
{
    const char *str = "OK";
    size_t len = sizeof("OK") - 1;

    asc_write_string(c, str, len);

    return len;
}

static size_t
asc_rsp_version(struct conn *c)
{
    const char *str = "VERSION " MC_VERSION_STRING;
    size_t len = sizeof("VERSION " MC_VERSION_STRING) - 1;

    asc_write_string(c, str, len);

    return len;
}

static size_t
asc_rsp_int(struct conn *c, uint64_t num)
{
    int len;
    char temp[INCR_MAX_STORAGE_LEN];

    len = snprintf(temp, INCR_MAX_STORAGE_LEN, "%"PRIu64, num);
    ASSERT(len < INCR_MAX_STORAGE_LEN);
    if (len < 0) {
        log_warn("server error on c %d for value %"PRIu64" during printing: %s",
                 c->sd, num, strerror(errno));
        len = asc_rsp_server_error(c);
    } else {
        asc_write_string(c, temp, strlen(temp));
    }

    return (size_t)len;
}

/*
 * Returns true if ntoken is within the bounds for a given request
 * type, false otherwise.
 */
static bool
asc_validate_ntoken(struct conn *c, int ntoken)
{
    struct bound *t;
    int min, max;

    ASSERT(c->req_type > REQ_UNKNOWN && c->req_type < REQ_SENTINEL);

    t = &ntoken_bound[c->req_type];
    min = t->b[c->noreply].min;
    max = t->b[c->noreply].max;

    if (ntoken >= min && ntoken <= max) {
        return true;
    } else {
        log_hexdump(LOG_NOTICE, c->req, c->req_len, "client error on c %d for "
                    "req of type %d with %d invalid tokens", c->sd,
                    c->req_type, ntoken);

        asc_rsp_client_error(c);
        return false;
    }
}

/*
 * Returns true if slabid is within the bounds for a given request
 * type, false otherwise.
 */
static bool
asc_get_slabid(uint8_t *id, struct conn *c, uint8_t nkey, uint32_t vlen)
{
    *id = item_slabid(nkey, vlen);
    if (*id == SLABCLASS_INVALID_ID) {
        log_debug(LOG_NOTICE, "client error on c %d for req of type %d and "
                  "slab id out of range for key size %"PRIu8" and value size "
                  "%"PRIu32, c->sd, c->req_type, nkey, vlen);

        asc_rsp_client_error(c);
        return false;
    } else {
        return true;
    }
}

/*
 * Returns true if nkey is within the bounds for a given request
 * type, and update the key, nkey values; false otherwise.
 */
static bool
asc_get_key(char **key, uint8_t *nkey, struct conn *c, struct token *token)
{
    size_t keylen;

    *key = token[TOKEN_KEY].val;
    keylen = token[TOKEN_KEY].len;

    if (keylen > KEY_MAX_LEN) {
        log_debug(LOG_NOTICE, "client error on c %d for req of type %d and %d "
                  "length key", c->sd, c->req_type, keylen);

        asc_rsp_client_error(c);
        return false;
    } else {
        *nkey = (uint8_t)keylen;
        return true;
    }
}

/*
 * Returns true if flags is valid and update the flags value; false otherwise.
 */
static bool
asc_get_flags(uint32_t *flags, struct conn *c, struct token *token)
{
    if (!mc_strtoul(token[TOKEN_FLAGS].val, flags)) {
        log_debug(LOG_NOTICE, "client error on c %d for req of type %d and "
                  "invalid flags '%.*s'", c->sd, c->req_type,
                  token[TOKEN_FLAGS].len, token[TOKEN_FLAGS].val);

        asc_rsp_client_error(c);
        return false;
    } else {
        return true;
    }
}

/*
 * Returns true if expiry is valid and update the expiry value; false otherwise.
 */
static bool
asc_get_expiry(int32_t *exptime_int, struct conn *c, struct token *token)
{
    if (!mc_strtol(token[TOKEN_EXPIRY].val, exptime_int)) {
        log_debug(LOG_NOTICE, "client error on c %d for req of type %d and "
                  "invalid expiry '%.*s'", c->sd, c->req_type,
                  token[TOKEN_EXPIRY].len, token[TOKEN_EXPIRY].val);

        asc_rsp_client_error(c);
        return false;
    } else {
        return true;
    }
}

/*
 * Returns true if vlen is valid and update the vlen value; false otherwise.
 */
static bool
asc_get_vlen(uint32_t *vlen, struct conn *c, struct token *token)
{
    if (!mc_strtoul(token[TOKEN_VLEN].val, vlen)) {
        log_debug(LOG_NOTICE, "client error on c %d for req of type %d and "
                  "invalid vlen '%.*s'", c->sd, c->req_type,
                  token[TOKEN_VLEN].len, token[TOKEN_VLEN].val);

        asc_rsp_client_error(c);
        return false;
    } else {
        return true;
    }
}

/*
 * Returns true if delta is valid and update the delta value; false otherwise.
 */
static bool
asc_get_delta(uint64_t *delta, struct conn *c, struct token *token)
{
    if (!mc_strtoull(token[TOKEN_DELTA].val, delta)) {
        log_debug(LOG_NOTICE, "client error on c %d for req of type %d with "
                  "invalid delta '%.*s'", c->sd, c->req_type,
                  token[TOKEN_DELTA].len, token[TOKEN_DELTA].val);

        asc_rsp_client_error(c);
        return false;
    } else {
        return true;
    }
}

static void
asc_complete_set(struct conn *c)
{
    struct item *it = c->item;
    size_t rsplen;

    item_set(c);
    stats_slab_incr(it->id, set_success);
    rsplen = asc_rsp_stored(c);
    klog_write(c->peer, c->req_type, c->req, c->req_len, SET_OK, rsplen);
}

static void
asc_complete_cas(struct conn *c)
{
    item_cas_result_t res;
    struct item *it = c->item;
    size_t rsplen;

    res = item_cas(c);
    switch (res) {
    case CAS_OK:
        stats_slab_incr(it->id, cas_success);
        rsplen = asc_rsp_stored(c);
        klog_write(c->peer, c->req_type, c->req, c->req_len, res, rsplen);
        break;

    case CAS_EXISTS:
        stats_thread_incr(cas_badval);
        rsplen = asc_rsp_exists(c);
        klog_write(c->peer, c->req_type, c->req, c->req_len, res, rsplen);
        break;

    case CAS_NOT_FOUND:
        stats_thread_incr(cas_miss);
        rsplen = asc_rsp_not_found(c);
        klog_write(c->peer, c->req_type, c->req, c->req_len, res, rsplen);
        break;

    default:
        NOT_REACHED();
        break;
    }
}

static void
asc_complete_add(struct conn *c)
{
    item_add_result_t res;
    struct item *it = c->item;
    size_t rsplen;

    res = item_add(c);
    switch (res) {
    case ADD_OK:
        stats_slab_incr(it->id, add_success);
        rsplen = asc_rsp_stored(c);
        klog_write(c->peer, c->req_type, c->req, c->req_len, res, rsplen);
        break;

    case ADD_EXISTS:
        stats_thread_incr(add_exist);
        rsplen = asc_rsp_not_stored(c);
        klog_write(c->peer, c->req_type, c->req, c->req_len, res, rsplen);
        break;

    default:
        NOT_REACHED();
        break;
    }
}

static void
asc_complete_replace(struct conn *c)
{
    item_replace_result_t res;
    struct item *it = c->item;
    size_t rsplen;

    res = item_replace(c);
    switch (res) {
    case REPLACE_OK:
        stats_slab_incr(it->id, replace_success);
        rsplen = asc_rsp_stored(c);
        klog_write(c->peer, c->req_type, c->req, c->req_len, res, rsplen);
        break;

    case REPLACE_NOT_FOUND:
        stats_thread_incr(replace_miss);
        rsplen = asc_rsp_not_stored(c);
        klog_write(c->peer, c->req_type, c->req, c->req_len, res, rsplen);
        break;

    default:
        NOT_REACHED();
        break;
    }
}

static void
asc_complete_annex(struct conn *c)
{
    item_annex_result_t res;
    uint32_t nbyte;
    uint8_t oid, nid;
    size_t rsplen;
    bool rl;

    rl = (c->req_type == REQ_APPENDRL) || (c->req_type == REQ_PREPENDRL);
    res = item_annex(&nbyte, &oid, &nid, c);
    switch (res) {
    case ANNEX_OK:
        switch (c->req_type) {
        case REQ_APPEND:
            stats_slab_incr(oid, append_hit);
            stats_slab_incr(nid, append_success);
            break;

        case REQ_PREPEND:
            stats_slab_incr(oid, prepend_hit);
            stats_slab_incr(nid, prepend_success);
            break;

        case REQ_APPENDRL:
            stats_slab_incr(oid, appendrl_hit);
            stats_slab_incr(nid, appendrl_success);
            break;

        case REQ_PREPENDRL:
            stats_slab_incr(oid, prependrl_hit);
            stats_slab_incr(nid, prependrl_success);
            break;

        default:
            NOT_REACHED();
            break;
        }
        if (rl) {
            rsplen = asc_rsp_int(c, nbyte);
        } else {
            rsplen = asc_rsp_stored(c);
        }
        klog_write(c->peer, c->req_type, c->req, c->req_len, res, rsplen);
        break;

    case ANNEX_NOT_FOUND:
        switch (c->req_type) {
        case REQ_APPEND:
            stats_thread_incr(append_miss);
            break;

        case REQ_PREPEND:
            stats_thread_incr(prepend_miss);
            break;

        case REQ_APPENDRL:
            stats_thread_incr(appendrl_miss);
            break;

        case REQ_PREPENDRL:
            stats_thread_incr(prependrl_miss);
            break;

        default:
            NOT_REACHED();
            break;
        }
        rsplen = asc_rsp_not_stored(c);
        klog_write(c->peer, c->req_type, c->req, c->req_len, res, rsplen);
        break;

    case ANNEX_OVERSIZED:
        switch (c->req_type) {
        case REQ_APPEND:
            stats_slab_incr(oid, append_hit);
            break;

        case REQ_PREPEND:
            stats_slab_incr(oid, prepend_hit);
            break;

        case REQ_APPENDRL:
            stats_slab_incr(oid, appendrl_hit);
            break;

        case REQ_PREPENDRL:
            stats_slab_incr(oid, prependrl_hit);
            break;

        default:
            NOT_REACHED();
            break;
        }
        rsplen = asc_rsp_client_error(c);
        klog_write(c->peer, c->req_type, c->req, c->req_len, res, rsplen);
        break;

    case ANNEX_EOM:
        log_warn("server error on c %d for req of type %d with store "
                 "status %d", c->sd, c->req_type, res);

        rsplen = asc_rsp_server_error(c);
        klog_write(c->peer, c->req_type, c->req, c->req_len, res, rsplen);
        break;

    default:
        NOT_REACHED();
        break;
    }
}

/*
 * We get here after reading the value in update commands. The command
 * is stored in c->req_type, and the item is ready in c->item.
 */
void
asc_complete_nread(struct conn *c)
{
    struct item *it;
    char *end;

    it = c->item;
    end = item_data(it) + it->nbyte;

    if (!strcrlf(end)) {
        log_hexdump(LOG_NOTICE, c->req, c->req_len, "client error on c %d for "
                    "req of type %d with missing crlf", c->sd, c->req_type);

        asc_rsp_client_error(c);

        goto nread_done;
    }

    switch (c->req_type) {
    case REQ_SET:
        asc_complete_set(c);
        break;

    case REQ_CAS:
        asc_complete_cas(c);
        break;

    case REQ_ADD:
        asc_complete_add(c);
        break;

    case REQ_REPLACE:
        asc_complete_replace(c);
        break;

    case REQ_APPEND:
    case REQ_PREPEND:
    case REQ_APPENDRL:
    case REQ_PREPENDRL:
        asc_complete_annex(c);
        break;

    default:
        NOT_REACHED();
        break;
    }

nread_done:

    item_remove(it);
    c->item = NULL;
}

static void
asc_set_noreply_maybe(struct conn *c, struct token *token, int ntoken)
{
    struct token *t;

    if (ntoken < 2) {
        return;
    }

    t = &token[ntoken - 2];

    if ((t->len == sizeof("noreply") - 1) &&
        str7cmp(t->val, 'n', 'o', 'r', 'e', 'p', 'l', 'y')) {
        c->noreply = 1;
    }
}

static rstatus_t
asc_create_suffix(struct conn *c, unsigned valid_key_iter, char **suffix)
{
    if (valid_key_iter >= c->ssize) {
        char **new_suffix_list;

        new_suffix_list = mc_realloc(c->slist, sizeof(char *) * c->ssize * 2);
        if (new_suffix_list == NULL) {
            return MC_ENOMEM;
        }
        stats_thread_incr_by(mem_slist_curr, sizeof(char *) * c->ssize);
        c->ssize *= 2;
        c->slist  = new_suffix_list;
    }

    *suffix = cache_alloc(c->thread->suffix_cache);
    if (*suffix == NULL) {
        log_warn("server error on c %d for req of type %d with enomem on "
                 "suffix cache", c->sd, c->req_type);

        asc_rsp_server_error(c);
        return MC_ENOMEM;
    }

    *(c->slist + valid_key_iter) = *suffix;
    return MC_OK;
}

static void
asc_destroy_suffix(struct conn *c, char *suffix)
{
    cache_free(c->thread->suffix_cache, suffix);
}

/*
 * Build the response. Each hit adds three elements to the outgoing
 * reponse vector, viz:
 *   "VALUE "
 *   key
 *   " " + flags + " " + data length + "\r\n" + data (with \r\n)
 */
static rstatus_t
asc_respond_get(struct conn *c, unsigned valid_key_iter, struct item *it,
                bool return_cas)
{
    rstatus_t status;
    char *suffix = NULL;
    int sz;
    int total_len = 0;
    uint32_t nbyte = it->nbyte;
    char *data = item_data(it);

    status = conn_add_iov(c, VALUE, VALUE_LEN);
    if (status != MC_OK) {
        return status;
    }
    total_len += VALUE_LEN;

    status = conn_add_iov(c, item_key(it), it->nkey);
    if (status != MC_OK) {
        return status;
    }
    total_len += it->nkey;

    status = asc_create_suffix(c, valid_key_iter, &suffix);
    if (status != MC_OK) {
        return status;
    }
    if (return_cas) {
        sz = mc_snprintf(suffix, SUFFIX_MAX_LEN, " %"PRIu32" %"PRIu32" %"PRIu64,
                      it->dataflags, nbyte, item_get_cas(it));
        ASSERT(sz <= SUFFIX_SIZE + CAS_SUFFIX_SIZE);
     } else {
        sz = mc_snprintf(suffix, SUFFIX_MAX_LEN, " %"PRIu32" %"PRIu32,
                      it->dataflags, nbyte);
        ASSERT(sz <= SUFFIX_SIZE);
    }
    if (sz < 0) {
        status = MC_ERROR;
        goto get_done;
    }

    status = conn_add_iov(c, suffix, sz);
    if (status != MC_OK) {
        goto get_done;
    }
    total_len += sz;

    status = conn_add_iov(c, CRLF, CRLF_LEN);
    if (status != MC_OK) {
        goto get_done;
    }
    total_len += CRLF_LEN;

    status = conn_add_iov(c, data, nbyte);
    if (status != MC_OK) {
        goto get_done;
    }
    total_len += nbyte;

    status = conn_add_iov(c, CRLF, CRLF_LEN);
    if (status != MC_OK) {
        goto get_done;
    }
    total_len += CRLF_LEN;

    klog_write(c->peer, c->req_type, item_key(it), it->nkey, 0, total_len);

get_done:
    if (status != MC_OK) {
        /*
         * an abnormal status will warrant an error, so suffix won't be
         * used/freed later
         */
        asc_destroy_suffix(c, suffix);
    }

    return status;
}

static inline void
asc_process_read(struct conn *c, struct token *token, int ntoken)
{
    rstatus_t status;
    char *key;
    size_t keylen;
    uint8_t nkey;
    unsigned valid_key_iter = 0;
    struct item *it;
    struct token *key_token;
    bool return_cas;

    if (!asc_validate_ntoken(c, ntoken)) {
        return;
    }

    return_cas = (c->req_type == REQ_GETS) ? true : false;
    key_token = &token[TOKEN_KEY];

    do {
        while (key_token->len != 0) {
            key = key_token->val;
            keylen = key_token->len;

            if (keylen > KEY_MAX_LEN) {
                log_debug(LOG_NOTICE, "client error on c %d for req of type %d and %d "
                          "length key", c->sd, c->req_type, keylen);

                asc_rsp_client_error(c);
                return;
            } else {
                nkey = (uint8_t)keylen;
            }

            if (return_cas) {
                stats_thread_incr(gets_key);
            } else {
                stats_thread_incr(get_key);
            }

            it = item_get(key, nkey);
            if (it != NULL) {
                /* item found */
                if (return_cas) {
                    stats_slab_incr(it->id, gets_key_hit);
                } else {
                    stats_slab_incr(it->id, get_key_hit);
                }

                if (valid_key_iter >= c->isize) {
                    struct item **new_list;

                    new_list = mc_realloc(c->ilist, sizeof(struct item *) * c->isize * 2);
                    if (new_list != NULL) {
                        stats_thread_incr_by(mem_ilist_curr, sizeof(struct item *) * c->isize);
                        c->isize *= 2;
                        c->ilist = new_list;
                    } else {
                        item_remove(it);
                        break;
                    }
                }

                status = asc_respond_get(c, valid_key_iter, it, return_cas);
                if (status != MC_OK) {
                    log_warn("server error on c %d for req of type %d with %d "
                             "tokens", c->sd, c->req_type, ntoken);

                    asc_rsp_server_error(c);
                    item_remove(it);
                    break;
                }

                log_debug(LOG_VVERB, ">%d sending key %.*s", c->sd, it->nkey,
                          item_key(it));

                item_touch(it);
                *(c->ilist + valid_key_iter) = it;
                valid_key_iter++;
            } else {
                /* item not found */
                if (return_cas) {
                    stats_thread_incr(gets_key_miss);
                } else {
                    stats_thread_incr(get_key_miss);
                }
                klog_write(c->peer, c->req_type, key, nkey, 1, 0);
            }

            key_token++;
        }

        /*
         * If the command string hasn't been fully processed, get the next set
         * of token.
         */
        if (key_token->val != NULL) {
            ntoken = asc_tokenize(key_token->val, token, TOKEN_MAX);
            /* ntoken is unused */
            key_token = token;
        }

    } while (key_token->val != NULL);

    c->icurr = c->ilist;
    c->ileft = valid_key_iter;
    c->scurr = c->slist;
    c->sleft = valid_key_iter;

    log_debug(LOG_VVERB, ">%d END", c->sd);

    /*
     * If the loop was terminated because of out-of-memory, it is not
     * reliable to add END\r\n to the buffer, because it might not end
     * in \r\n. So we send SERVER_ERROR instead.
     */
    if (key_token->val != NULL || conn_add_iov(c, "END\r\n", 5) != MC_OK ||
        (c->udp && conn_build_udp_headers(c) != MC_OK)) {
        log_warn("server error on c %d for req of type %d with enomem", c->sd,
                 c->req_type);

        asc_rsp_server_error(c);
    } else {
        conn_set_state(c, CONN_MWRITE);
        c->msg_curr = 0;
    }
}

static void
asc_process_update(struct conn *c, struct token *token, int ntoken)
{
    char *key;
    uint8_t nkey;
    uint32_t flags, vlen;
    int32_t exptime_int;
    time_t exptime;
    uint64_t req_cas_id;
    struct item *it;
    bool handle_cas;
    uint8_t id;

    asc_set_noreply_maybe(c, token, ntoken);

    if (!asc_validate_ntoken(c, ntoken)) {
        return;
    }

    if (!asc_get_key(&key, &nkey, c, token)) {
        return;
    }

    if (!asc_get_flags(&flags, c, token)) {
        return;
    }

    if (!asc_get_expiry(&exptime_int, c, token)) {
        return;
    }

    if (!asc_get_vlen(&vlen, c, token)) {
        return;
    }

    if (!asc_get_slabid(&id, c, nkey, vlen)) {
        return;
    }

    exptime = (time_t)exptime_int;

    handle_cas = (c->req_type == REQ_CAS) ? true : false;
    if (handle_cas && !mc_strtoull(token[TOKEN_CAS].val, &req_cas_id)) {
        log_debug(LOG_NOTICE, "client error on c %d for req of type %d and "
                  "invalid cas '%.*s'", c->sd, c->req_type,
                  token[TOKEN_CAS].len, token[TOKEN_CAS].val);

        asc_rsp_client_error(c);
        return;
    }

    it = item_alloc(id, key, nkey, flags, time_reltime(exptime), vlen);
    if (it == NULL) {
        log_warn("server error on c %d for req of type %d because of oom in "
                 "storing item", c->sd, c->req_type);

        asc_rsp_server_error(c);

        /* swallow the data line */
        c->write_and_go = CONN_SWALLOW;
        c->sbytes = vlen + CRLF_LEN;

        item_delete(key, nkey);
        return;
    }

    if (handle_cas) {
        item_set_cas(it, req_cas_id);
    }
    c->item = it;
    c->ritem = item_data(it);
    c->rlbytes = it->nbyte + CRLF_LEN;
    conn_set_state(c, CONN_NREAD);
}

static void
asc_process_annex(struct conn *c, struct token *token, int ntoken)
{
    char *key;
    uint8_t nkey;
    uint32_t vlen;
    struct item *it;
    uint8_t id;

    asc_set_noreply_maybe(c, token, ntoken);

    if (!asc_validate_ntoken(c, ntoken)) {
        return;
    }

    if (!asc_get_key(&key, &nkey, c, token)) {
        return;
    }

    if (!asc_get_vlen(&vlen, c, token)) {
        return;
    }

    if (!asc_get_slabid(&id, c, nkey, vlen)) {
        return;
    }

    /* flags and exptime are both set to 0 as they have no effect later */
    it = item_alloc(id, key, nkey, 0, 0, vlen);
    if (it == NULL) {
        log_warn("server error on c %d for req of type %d because of oom in "
                 "allocing item", c->sd, c->req_type);

        asc_rsp_server_error(c);

        /* swallow the data line */
        c->write_and_go = CONN_SWALLOW;
        c->sbytes = vlen + CRLF_LEN;

        item_delete(key, nkey);
        return;
    }

    c->item = it;
    c->ritem = item_data(it);
    c->rlbytes = it->nbyte + CRLF_LEN;
    conn_set_state(c, CONN_NREAD);
}


static void
asc_process_delta(struct conn *c, struct token *token, int ntoken)
{
    item_delta_result_t res;
    uint64_t delta;
    char *key;
    uint8_t nkey;
    bool incr;
    uint64_t value;
    size_t rsplen;

    asc_set_noreply_maybe(c, token, ntoken);

    if (!asc_validate_ntoken(c, ntoken)) {
        return;
    }

    if (!asc_get_key(&key, &nkey, c, token)) {
        return;
    }

    if (!asc_get_delta(&delta, c, token)) {
        return;
    }

    incr = (c->req_type == REQ_INCR) ? true : false;
    res = item_delta(&value, key, nkey, incr, delta);
    switch (res) {
    case DELTA_OK:
        if (incr) {
            stats_thread_incr(incr_success);
        } else {
            stats_thread_incr(decr_success);
        }
        rsplen = asc_rsp_int(c, value);
        klog_write(c->peer, c->req_type, c->req, c->req_len, res, rsplen);
        break;

    case DELTA_NOT_FOUND:
        if (incr) {
            stats_thread_incr(incr_miss);
        } else {
            stats_thread_incr(decr_miss);
        }
        rsplen = asc_rsp_not_found(c);
        klog_write(c->peer, c->req_type, c->req, c->req_len, res, rsplen);
        break;

    case DELTA_NON_NUMERIC:
        log_debug(LOG_NOTICE, "client error on c %d for req of type %d with "
                  "non-numeric value", c->sd, c->req_type);

        rsplen = asc_rsp_client_error(c);
        klog_write(c->peer, c->req_type, c->req, c->req_len, res, rsplen);
        break;

        rsplen = asc_rsp_client_error(c);
        klog_write(c->peer, c->req_type, c->req, c->req_len, res, rsplen);
        break;

    case DELTA_EOM:
        log_warn("server error on c %d for req of type %d because of oom",
                 c->sd, c->req_type);

        rsplen = asc_rsp_server_error(c);
        klog_write(c->peer, c->req_type, c->req, c->req_len, res, rsplen);
        break;

    default:
        NOT_REACHED();
        break;
    }
}

static void
asc_process_delete(struct conn *c, struct token *token, int ntoken)
{
    item_delete_result_t res;
    char *key;       /* key to be deleted */
    uint8_t nkey;     /* # key bytes */
    size_t rsplen;

    asc_set_noreply_maybe(c, token, ntoken);

    if (!asc_validate_ntoken(c, ntoken)) {
        return;
    }

    if (!asc_get_key(&key, &nkey, c, token)) {
        return;
    }

    res  = item_delete(key, nkey);
    switch (res) {
    case DELETE_OK:
        stats_thread_incr(delete_hit);
        rsplen = asc_rsp_deleted(c);
        klog_write(c->peer, c->req_type, c->req, c->req_len, DELETE_OK, rsplen);
        break;

    case DELETE_NOT_FOUND:
        stats_thread_incr(delete_miss);
        rsplen = asc_rsp_not_found(c);
        klog_write(c->peer, c->req_type, c->req, c->req_len, DELETE_OK, rsplen);
        break;

    default:
        NOT_REACHED();
        break;
    }
}

static void
asc_process_stats(struct conn *c, struct token *token, int ntoken)
{
    struct token *t = &token[TOKEN_SUBCOMMAND];

    if (!stats_enabled()) {
        log_warn("server error on c %d for req of type %d because stats is "
                 "disabled", c->sd, c->req_type);

        asc_rsp_server_error(c);
        return;
    }

    if (!asc_validate_ntoken(c, ntoken)) {
        return;
    }

    if (ntoken == 2) {
        stats_default(c);
    } else if (strncmp(t->val, "reset", t->len) == 0) {
        log_warn("server error on c %d for req of type %d because stats reset "
                 "is not supported", c->sd, c->req_type);
        asc_rsp_server_error(c);
        return;
    } else if (strncmp(t->val, "settings", t->len) == 0) {
        stats_settings(c);
    } else if (strncmp(t->val, "cachedump", t->len) == 0) {
        char *buf;
        unsigned int bytes, id, limit = 0;

        if (ntoken < 5) {
            log_hexdump(LOG_NOTICE, c->req, c->req_len, "client error on c %d "
                        "for req of type %d with %d invalid tokens", c->sd,
                        c->req_type, ntoken);

            asc_rsp_client_error(c);
            return;
        }

        if (!mc_strtoul(token[TOKEN_CACHEDUMP_ID].val, &id) ||
            !mc_strtoul(token[TOKEN_CACHEDUMP_LIMIT].val, &limit)) {
            log_debug(LOG_NOTICE, "client error on c %d for req of type %d "
                      "because either id '%.*s' or limit '%.*s' is invalid",
                      c->sd, c->req_type, token[TOKEN_CACHEDUMP_ID].len,
                      token[TOKEN_CACHEDUMP_ID].val, token[TOKEN_CACHEDUMP_LIMIT].len,
                      token[TOKEN_CACHEDUMP_LIMIT].val);

            asc_rsp_client_error(c);
            return;
        }

        if (id < SLABCLASS_MIN_ID || id > SLABCLASS_MAX_ID) {
            log_debug(LOG_NOTICE, "client error on c %d for req of type %d "
                      "because %d is an illegal slab id", c->sd, c->req_type,
                      id);

            asc_rsp_client_error(c);
            return;
        }

        buf = item_cache_dump(id, limit, &bytes);
        core_write_and_free(c, buf, bytes);
        return;
    } else {
        /*
         * Getting here means that the sub command is either engine specific
         * or is invalid. query the engine and see
         */
        if (strncmp(t->val, "slabs", t->len) == 0) {
            stats_slabs(c);
        } else if (strncmp(t->val, "sizes", t->len) == 0) {
            stats_sizes(c);
        } else {
            log_debug(LOG_NOTICE, "client error on c %d for req of type %d with "
                      "invalid stats subcommand '%.*s", c->sd, c->req_type,
                      t->len, t->val);

            asc_rsp_client_error(c);
            return;
        }

        if (c->stats.buffer == NULL) {
            log_warn("server error on c %d for req of type %d because of oom "
                     "writing stats", c->sd, c->req_type);

            asc_rsp_server_error(c);
        } else {
            core_write_and_free(c, c->stats.buffer, c->stats.offset);
            c->stats.buffer = NULL;
        }

        return;
    }

    /* append terminator and start the transfer */
    stats_append(c, NULL, 0, NULL, 0);

    if (c->stats.buffer == NULL) {
        log_warn("server error on c %d for req of type %d because of oom "
                 "writing stats", c->sd, c->req_type);

        asc_rsp_server_error(c);
    } else {
        core_write_and_free(c, c->stats.buffer, c->stats.offset);
        c->stats.buffer = NULL;
    }
}

static void
asc_process_klog(struct conn *c, struct token *token, int ntoken)
{
    struct token *t;

    if (!asc_validate_ntoken(c, ntoken)) {
        return;
    }

    t = &token[TOKEN_KLOG_COMMAND];

    if (strncmp(t->val, "run", t->len) == 0) {
        if (settings.klog_name == NULL) {
            log_debug(LOG_NOTICE, "client error on c %d for req of type %d "
                      "with klog filename not set", c->sd, c->req_type);

            asc_rsp_client_error(c);
            return;
        }

        t = &token[TOKEN_KLOG_SUBCOMMAND];
        if (strncmp(t->val, "start", t->len) == 0) {
            log_debug(LOG_NOTICE, "klog start at epoch %u", time_now());
            settings.klog_running = true;
            asc_rsp_ok(c);
        } else if (strncmp(t->val, "stop", t->len) == 0) {
            log_debug(LOG_NOTICE, "klog stops at epoch %u", time_now());
            settings.klog_running = false;
            asc_rsp_ok(c);
        } else {
            log_debug(LOG_NOTICE, "client error on c %d for req of type %d "
                      "with invalid klog run subcommand '%.*s'", c->sd,
                      c->req_type, t->len, t->val);

            asc_rsp_client_error(c);
        }
    } else if (strncmp(t->val, "interval", t->len) == 0) {
        t = &token[TOKEN_KLOG_SUBCOMMAND];
        if (strncmp(t->val, "reset", t->len) == 0) {
            stats_set_interval(STATS_DEFAULT_INTVL);
            asc_rsp_ok(c);
        } else {
            int32_t interval;

            if (!mc_strtol(t->val, &interval)) {
                log_debug(LOG_NOTICE, "client error on c %d for req of type %d "
                          "with invalid klog interval '%.*s'", c->sd,
                          c->req_type, t->len, t->val);

                asc_rsp_client_error(c);
            } else if (interval < KLOG_MIN_INTVL) {
                log_debug(LOG_NOTICE, "client error on c %d for req of type %d "
                          "with invalid klog interval %"PRId32"", c->sd,
                          c->req_type, interval);

                asc_rsp_client_error(c);
            } else {
                stats_set_interval(interval);
                asc_rsp_ok(c);
            }
        }
    } else if (strncmp(t->val, "sampling", t->len) == 0) {
        t = &token[TOKEN_KLOG_SUBCOMMAND];
        if (strncmp(t->val, "reset", t->len) == 0) {
            settings.klog_sampling_rate = KLOG_DEFAULT_SMP_RATE;
            asc_rsp_ok(c);
        } else {
            int32_t sampling;

            if (!mc_strtol(t->val, &sampling)) {
                log_debug(LOG_NOTICE, "client error on c %d for req of type %d "
                          "with invalid klog sampling '%.*s'", c->sd,
                          c->req_type, t->len, t->val);

                asc_rsp_client_error(c);
            } else if (sampling <= 0) {
                log_debug(LOG_NOTICE, "client error on c %d for req of type %d "
                          "with invalid klog sampling %"PRId32"", c->sd,
                          c->req_type, sampling);

                asc_rsp_client_error(c);
            } else {
                settings.klog_sampling_rate = sampling;
                asc_rsp_ok(c);
            }
        }
    } else {
        log_debug(LOG_NOTICE, "client error on c %d for req of type %d with "
                  "invalid klog subcommand '%.*s'", c->sd, c->req_type,
                  t->len, t->val);

        asc_rsp_client_error(c);
    }
}

static void
asc_process_verbosity(struct conn *c, struct token *token, int ntoken)
{
    uint32_t level;

    asc_set_noreply_maybe(c, token, ntoken);

    if (ntoken != 3 && ntoken != 4) {
        log_hexdump(LOG_NOTICE, c->req, c->req_len, "client error on c %d for "
                    "req of type %d with %d invalid tokens", c->sd,
                    c->req_type, ntoken);

        asc_rsp_client_error(c);
        return;
    }

    if (!mc_strtoul(token[TOKEN_SUBCOMMAND].val, &level)) {
        log_debug(LOG_NOTICE, "client error on c %d for req of type %d with "
                  "invalid level '%.*s'", c->sd, c->req_type,
                  token[TOKEN_SUBCOMMAND].len, token[TOKEN_SUBCOMMAND].val);

        asc_rsp_client_error(c);
        return;
    }

    log_level_set(level);

    asc_rsp_ok(c);
}

static void
asc_process_aggregate(struct conn *c, struct token *token, int ntoken)
{
    int32_t interval;

    if (ntoken != 4) {
        log_hexdump(LOG_NOTICE, c->req, c->req_len, "client error on c %d for "
                    "req of type %d with %d invalid tokens", c->sd,
                    c->req_type, ntoken);

        asc_rsp_client_error(c);
        return;
    }

    if (!mc_strtol(token[TOKEN_AGGR_COMMAND].val, &interval)) {
        log_debug(LOG_NOTICE, "client error on c %d for req of type %d with "
                  "invalid option '%.*s'", c->sd, c->req_type,
                  token[TOKEN_AGGR_COMMAND].len, token[TOKEN_AGGR_COMMAND].val);

        asc_rsp_client_error(c);
        return;
    }

    if (interval > 0) {
        stats_set_interval(interval);
        asc_rsp_ok(c);
    } else if (interval == 0) {
        stats_set_interval(STATS_DEFAULT_INTVL);
        asc_rsp_ok(c);
    } else {
        stats_set_interval(-1000000);
        asc_rsp_ok(c);
    }
}

static void
asc_process_evict(struct conn *c, struct token *token, int ntoken)
{
    int32_t option;

    if (ntoken != 4) {
        log_hexdump(LOG_NOTICE, c->req, c->req_len, "client error on c %d for "
                    "req of type %d with %d invalid tokens", c->sd,
                    c->req_type, ntoken);

        asc_rsp_client_error(c);
        return;
    }

    if (!mc_strtol(token[TOKEN_EVICT_COMMAND].val, &option)) {
        log_debug(LOG_NOTICE, "client error on c %d for req of type %d with "
                  "invalid option '%.*s'", c->sd, c->req_type,
                  token[TOKEN_EVICT_COMMAND].len,
                  token[TOKEN_EVICT_COMMAND].val);

        asc_rsp_client_error(c);
        return;
    }

    if (option >= EVICT_NONE && option < EVICT_INVALID) {
        settings.evict_opt = option;
        asc_rsp_ok(c);
        return;
    }

    log_debug(LOG_NOTICE, "client error on c %d for req of type %d with "
              "invalid option %"PRId32"", c->sd, c->req_type, option);

    asc_rsp_client_error(c);
}

static void
asc_process_maxbytes(struct conn *c, struct token *token, int ntoken)
{
    uint64_t option;

    if (ntoken != 4) {
        log_hexdump(LOG_NOTICE, c->req, c->req_len, "client error on c %d for "
                    "req of type %d with %d invalid tokens", c->sd,
                    c->req_type, ntoken);

        asc_rsp_client_error(c);
        return;
    }

    if (!mc_strtoull(token[TOKEN_MAXBYTES_COMMAND].val, &option)) {
        log_debug(LOG_NOTICE, "client error on c %d for req of type %d with "
                  "invalid option '%.*s'", c->sd, c->req_type,
                  token[TOKEN_MAXBYTES_COMMAND].len,
                  token[TOKEN_MAXBYTES_COMMAND].val);

        asc_rsp_client_error(c);
        return;
    }

    if (option < SIZE_MAX && option >= settings.maxbytes) {
        settings.maxbytes = (size_t)option;
        asc_rsp_ok(c);
        return;
    }

    log_debug(LOG_NOTICE, "client error on c %d for req of type %d with "
              "invalid option %"PRId64"", c->sd, c->req_type, option);

    asc_rsp_client_error(c);
}

static void
asc_process_config(struct conn *c, struct token *token, int ntoken)
{
    struct token *t = &token[TOKEN_SUBCOMMAND];

    if (strncmp(t->val, "aggregate", t->len) == 0) {
        asc_process_aggregate(c, token, ntoken);
    } else if (strncmp(t->val, "klog", t->len) == 0) {
        asc_process_klog(c, token, ntoken);
    } else if (strncmp(t->val, "evict", t->len) == 0) {
        asc_process_evict(c, token, ntoken);
    } else if (strncmp(t->val, "maxbytes", t->len) == 0) {
        asc_process_maxbytes(c, token, ntoken);
    }
}

static void
asc_process_flushall(struct conn *c, struct token *token, int ntoken)
{
    struct bound *t = &ntoken_bound[REQ_FLUSHALL];
    int32_t exptime_int;
    time_t exptime;

    time_update();

    asc_set_noreply_maybe(c, token, ntoken);

    if (!asc_validate_ntoken(c, ntoken)) {
        return;
    }

    if (ntoken == t->b[c->noreply].min) {
        settings.oldest_live = time_now() - 1;
        item_flush_expired();
        asc_rsp_ok(c);
        return;
    }

    if (!mc_strtol(token[TOKEN_SUBCOMMAND].val, &exptime_int)) {
        log_debug(LOG_NOTICE, "client error on c %d for req of type %d with "
                  "invalid numeric value '%.*s'", c->sd, c->req_type,
                  token[TOKEN_SUBCOMMAND].len, token[TOKEN_SUBCOMMAND].val);

        asc_rsp_client_error(c);
        return;
    }

    exptime = (time_t)exptime_int;

    /*
     * If exptime is zero time_reltime() would return zero too, and
     * time_reltime(exptime) - 1 would overflow to the max unsigned value.
     * So we process exptime == 0 the same way we do when no delay is
     * given at all.
     */
    if (exptime > 0) {
        settings.oldest_live = time_reltime(exptime) - 1;
    } else {
        /* exptime == 0 */
        settings.oldest_live = time_now() - 1;
    }

    item_flush_expired();
    asc_rsp_ok(c);
}

static req_type_t
asc_parse_type(struct conn *c, struct token *token, int ntoken)
{
    char *tval;      /* token value */
    size_t tlen;     /* token length */
    req_type_t type; /* request type */

    if (ntoken < 2) {
        return REQ_UNKNOWN;
    }

    tval = token[TOKEN_COMMAND].val;
    tlen = token[TOKEN_COMMAND].len;

    type = REQ_UNKNOWN;

    switch (tlen) {
    case 3:
        if (str4cmp(tval, 'g', 'e', 't', ' ')) {
            type = REQ_GET;
        } else if (str4cmp(tval, 's', 'e', 't', ' ')) {
            type = REQ_SET;
        } else if (str4cmp(tval, 'a', 'd', 'd', ' ')) {
            type = REQ_ADD;
        } else if (str4cmp(tval, 'c', 'a', 's', ' ')) {
            type = REQ_CAS;
        }

        break;

    case 4:
        if (str4cmp(tval, 'g', 'e', 't', 's')) {
            type = REQ_GETS;
        } else if (str4cmp(tval, 'i', 'n', 'c', 'r')) {
            type = REQ_INCR;
        } else if (str4cmp(tval, 'd', 'e', 'c', 'r')) {
            type = REQ_DECR;
        } else if (str4cmp(tval, 'q', 'u', 'i', 't')) {
            type = REQ_QUIT;
        }

        break;

    case 5:
        if (str5cmp(tval, 's', 't', 'a', 't', 's')) {
            type = REQ_STATS;
        }

        break;

    case 6:
        if (str6cmp(tval, 'a', 'p', 'p', 'e', 'n', 'd')) {
            type = REQ_APPEND;
        } else if (str6cmp(tval, 'd', 'e', 'l', 'e', 't', 'e')) {
            type = REQ_DELETE;
        } else if (str6cmp(tval, 'c', 'o', 'n', 'f', 'i', 'g')) {
            type = REQ_CONFIG;
        }

        break;

    case 7:
        if (str7cmp(tval, 'r', 'e', 'p', 'l', 'a', 'c', 'e')) {
            type = REQ_REPLACE;
        } else if (str7cmp(tval, 'p', 'r', 'e', 'p', 'e', 'n', 'd')) {
            type = REQ_PREPEND;
        } else if (str7cmp(tval, 'v', 'e', 'r', 's', 'i', 'o', 'n')) {
            type = REQ_VERSION;
        }

        break;

    case 8:
        if (str8cmp(tval, 'a', 'p', 'p', 'e', 'n', 'd', 'r', 'l')) {
            type = REQ_APPENDRL;
        }

        break;

    case 9:
        if (str9cmp(tval, 'f', 'l', 'u', 's', 'h', '_', 'a', 'l', 'l')) {
            type = REQ_FLUSHALL;
        } else if (str9cmp(tval, 'p', 'r', 'e', 'p', 'e', 'n', 'd', 'r', 'l')) {
            type = REQ_PREPENDRL;
        } else if (str9cmp(tval, 'v', 'e', 'r', 'b', 'o', 's', 'i', 't', 'y')) {
            type = REQ_VERBOSITY;
        }

        break;

    default:
        type = REQ_UNKNOWN;
        break;
    }

    return type;
}

static void
asc_dispatch(struct conn *c)
{
    rstatus_t status;
    struct token token[TOKEN_MAX];
    int ntoken;

    c->msg_curr = 0;
    c->msg_used = 0;
    c->iov_used = 0;
    status = conn_add_msghdr(c);
    if (status != MC_OK) {
        log_warn("server error on c %d for req of type %d because of oom in "
                 "preparing response", c->sd, c->req_type);

        asc_rsp_server_error(c);
        return;
    }

    ntoken = asc_tokenize(c->req, token, TOKEN_MAX);

    c->req_type = asc_parse_type(c, token, ntoken);
    switch (c->req_type) {
    case REQ_GET:
        stats_thread_incr(cmd_total);
        stats_thread_incr(get);
        asc_process_read(c, token, ntoken);
        break;

    case REQ_GETS:
        stats_thread_incr(cmd_total);
        stats_thread_incr(gets);
        asc_process_read(c, token, ntoken);
        break;

    case REQ_SET:
        stats_thread_incr(cmd_total);
        stats_thread_incr(set);
        asc_process_update(c, token, ntoken);
        break;

    case REQ_CAS:
        stats_thread_incr(cmd_total);
        stats_thread_incr(cas);
        asc_process_update(c, token, ntoken);
        break;

    case REQ_ADD:
        stats_thread_incr(cmd_total);
        stats_thread_incr(add);
        asc_process_update(c, token, ntoken);
        break;

    case REQ_REPLACE:
        stats_thread_incr(cmd_total);
        stats_thread_incr(replace);
        asc_process_update(c, token, ntoken);
        break;

    case REQ_APPEND:
        stats_thread_incr(cmd_total);
        stats_thread_incr(append);
        asc_process_annex(c, token, ntoken);
        break;

    case REQ_PREPEND:
        stats_thread_incr(cmd_total);
        stats_thread_incr(prepend);
        asc_process_annex(c, token, ntoken);
        break;

    case REQ_APPENDRL:
        stats_thread_incr(cmd_total);
        stats_thread_incr(appendrl);
        asc_process_annex(c, token, ntoken);
        break;

    case REQ_PREPENDRL:
        stats_thread_incr(cmd_total);
        stats_thread_incr(prependrl);
        asc_process_annex(c, token, ntoken);
        break;

    case REQ_INCR:
        stats_thread_incr(cmd_total);
        stats_thread_incr(incr);
        asc_process_delta(c, token, ntoken);
        break;

    case REQ_DECR:
        stats_thread_incr(cmd_total);
        stats_thread_incr(decr);
        asc_process_delta(c, token, ntoken);
        break;

    case REQ_DELETE:
        stats_thread_incr(cmd_total);
        stats_thread_incr(delete);
        asc_process_delete(c, token, ntoken);
        break;

    case REQ_STATS:
        asc_process_stats(c, token, ntoken);
        break;

    case REQ_FLUSHALL:
        asc_process_flushall(c, token, ntoken);
        break;

    case REQ_VERSION:
        asc_rsp_version(c);
        break;

    case REQ_QUIT:
        conn_set_state(c, CONN_CLOSE);
        break;

    case REQ_VERBOSITY:
        asc_process_verbosity(c, token, ntoken);
        break;

    case REQ_CONFIG:
        asc_process_config(c, token, ntoken);
        break;

    case REQ_UNKNOWN:
    default:
        log_hexdump(LOG_INFO, c->req, c->req_len, "req on c %d with %d "
                    "invalid tokens", c->sd, ntoken);
        asc_rsp_client_error(c);
        break;
    }
}

rstatus_t
asc_parse(struct conn *c)
{
    char *el, *cont; /* eol marker, continue marker */

    ASSERT(c->rcurr <= c->rbuf + c->rsize);

    if (c->rbytes == 0) {
        return MC_EAGAIN;
    }

    el = memchr(c->rcurr, '\n', c->rbytes);
    if (el == NULL) {
        if (c->rbytes > 1024) {
            char *ptr = c->rcurr;

            /*
             * We didn't have a '\n' in the first k. This _has_ to be a
             * large multiget, if not we should just nuke the connection.
             */

            /* ignore leading whitespaces */
            while (*ptr == ' ') {
                ++ptr;
            }

            if (ptr - c->rcurr > 100 ||
                (strncmp(ptr, "get ", 4) && strncmp(ptr, "gets ", 5))) {

                conn_set_state(c, CONN_CLOSE);
                return MC_ERROR;
            }
        }

        return MC_EAGAIN;
    }

    cont = el + 1;
    if ((el - c->rcurr) > 1 && *(el - 1) == '\r') {
        el--;
    }
    *el = '\0';

    log_hexdump(LOG_VERB, c->rcurr, el - c->rcurr, "recv on c %d req with "
                "%d bytes", c->sd, el - c->rcurr);

    ASSERT(cont <= c->rbuf + c->rsize);
    ASSERT(cont <= c->rcurr + c->rbytes);

    c->req = c->rcurr;
    c->req_len = (uint16_t)(el - c->rcurr);

    asc_dispatch(c);

    /* update the read marker to point to continue marker */
    c->rbytes -= (cont - c->rcurr);
    c->rcurr = cont;

    return MC_OK;
}

void
asc_append_stats(struct conn *c, const char *key, uint16_t klen,
                 const char *val, uint32_t vlen)
{
    char *pos;
    uint32_t nbyte;
    int remaining, room;

    pos = c->stats.buffer + c->stats.offset;
    remaining = c->stats.size - c->stats.offset;
    room = remaining - 1;

    if (klen == 0 && vlen == 0) {
        nbyte = snprintf(pos, room, "END\r\n");
    } else if (vlen == 0) {
        nbyte = snprintf(pos, room, "STAT %s\r\n", key);
    } else {
        nbyte = snprintf(pos, room, "STAT %s %s\r\n", key, val);
    }

    c->stats.offset += nbyte;
}
