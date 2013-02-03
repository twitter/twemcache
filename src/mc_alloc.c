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
#include <errno.h>
#include <string.h>
#include <stdlib.h>

#if __GNUC__ >= 4 && __GNUC_MINOR__ >= 2
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

/* This function provide us access to the original libc free(). This is useful
 * for instance to free results obtained by backtrace_symbols(). We need
 * to define this function before including mc_alloc.h that may shadow the
 * free implementation if we use jemalloc or another non standard allocator. */
void
mc_libc_free(void *ptr)
{
    free(ptr);
}
#if __GNUC__ >= 4 && __GNUC_MINOR__ >= 6
#pragma GCC diagnostic pop
#endif

#include <mc_alloc.h>
#include <mc_core.h>

#ifdef HAVE_MALLOC_SIZE
#define PREFIX_SIZE (0)
#else
#if defined(__sun) || defined(__sparc) || defined(__sparc__)
#define PREFIX_SIZE (sizeof(long long))
#else
#define PREFIX_SIZE (sizeof(size_t))
#endif
#endif

#if defined(USE_TCMALLOC)
#define malloc(size)        tc_malloc(size)
#define calloc(count, size) tc_calloc(count, size)
#define realloc(ptr, size)  tc_realloc(ptr, size)
#define free(ptr)           tc_free(ptr);
#elif defined(USE_JEMALLOC)
#define malloc(size)        jemalloc(size)
#define calloc(count, size) jecalloc(count, size)
#define realloc(ptr, size)  jerealloc(ptr, size)
#define free(ptr)           jefree(ptr);
#endif

#ifdef HAVE_ATOMIC
#define update_mcmalloc_stat_add(__n) __sync_add_and_fetch(&heap_curr, (__n))
#define update_mcmalloc_stat_sub(__n) __sync_sub_and_fetch(&heap_curr, (__n))
#else
#define update_mcmalloc_stat_add(__n) do {  \
    pthread_mutex_lock(&heap_curr_mutex);   \
    heap_curr += (__n);                     \
    pthread_mutex_unlock(&heap_curr_mutex); \
} while(0)

#define update_mcmalloc_stat_sub(__n) do {  \
    pthread_mutex_lock(&heap_curr_mutex);   \
    heap_curr -= (__n);                     \
    pthread_mutex_unlock(&heap_curr_mutex); \
} while(0)

#endif

#define update_mcmalloc_stat_alloc(__n) do {            \
    size_t _n = (__n);                                  \
    if (_n & (sizeof(long) - 1))                        \
        _n += sizeof(long)-(_n & (sizeof(long)-1));     \
    if (mc_malloc_thread_safe) {                        \
        update_mcmalloc_stat_add(_n);                   \
    } else {                                            \
        heap_curr += _n;                                \
    }                                                   \
} while(0)

#define update_mcmalloc_stat_free(__n) do {             \
    size_t _n = (__n);                                  \
    if (_n & (sizeof(long)-1))                          \
        _n += sizeof(long) - (_n & (sizeof(long)-1));   \
    if (mc_malloc_thread_safe) {                        \
        update_mcmalloc_stat_sub(_n);                   \
    } else {                                            \
        heap_curr -= _n;                                \
    }                                                   \
} while(0)

static size_t heap_curr = 0;
static int mc_malloc_thread_safe = 1;
pthread_mutex_t heap_curr_mutex = PTHREAD_MUTEX_INITIALIZER;

void *
_mc_alloc(size_t size, const char *name, int line)
{
    void *p;

    ASSERT(size != 0);

    p = malloc(size + PREFIX_SIZE);
    if (p == NULL) {
        log_error("malloc(%zu) failed @ %s:%d", size, name, line);
        return p;
    } else {
        log_debug(LOG_VVERB, "malloc(%zu) at %p @ %s:%d", 
            size + PREFIX_SIZE, p, name, line);
    }

#ifdef HAVE_MALLOC_SIZE
    update_mcmalloc_stat_alloc(size + PREFIX_SIZE);
    return p;
#else
    *((size_t*)p) = size;
    update_mcmalloc_stat_alloc(size + PREFIX_SIZE);
    return (char *)p + PREFIX_SIZE;
#endif
}

void *
_mc_zalloc(size_t size, const char *name, int line)
{
    void *p;

    p = _mc_alloc(size, name, line);
    if (p != NULL) {
        memset(p, 0, size);
    }

    return p;
}

void *
_mc_calloc(size_t nmemb, size_t size, const char *name, int line)
{
    return _mc_zalloc(nmemb * size, name, line);
}

void *
_mc_realloc(void *ptr, size_t size, const char *name, int line)
{
#ifndef HAVE_MALLOC_SIZE
    void *realptr;
#endif
    size_t oldsize;
    void *p;

    ASSERT(size != 0);

    if (ptr == NULL)
        return _mc_alloc(size, name, line);

#ifdef HAVE_MALLOC_SIZE
    oldsize = mc_alloc_size(ptr);
    p = realloc(ptr, size);
#else
    realptr = (char*)ptr - PREFIX_SIZE;
    oldsize = *((size_t*)realptr);

    p = realloc(realptr, size + PREFIX_SIZE);
#endif

    if (p == NULL) {
        log_error("realloc(%zu) failed @ %s:%d", size, name, line);
        return p;
    } else {
        log_debug(LOG_VVERB, "realloc(%zu) at %p @ %s:%d", size, p, name, line);
    }

#ifdef HAVE_MALLOC_SIZE
    update_mcmalloc_stat_free(oldsize);
    update_mcmalloc_stat_alloc(mc_alloc_size(p));
#else
    *((size_t*)p) = size;
    update_mcmalloc_stat_free(oldsize);
    update_mcmalloc_stat_alloc(size);
#endif

    return (char*)p + PREFIX_SIZE;
}

/* Provide mc_alloc_size() for systems where this function is not provided by
 * malloc itself, given that in that case we store an header with this
 * information as the first bytes of every allocation. */
#ifndef HAVE_MALLOC_SIZE
size_t
mc_alloc_size(void *ptr)
{
    void *realptr = (char*)ptr - PREFIX_SIZE;
    size_t size = *((size_t*)realptr);
    
    /* Assume at least that all the allocations are padded at sizeof(long) by
     * the underlying allocator. */
    if (size & (sizeof(long) - 1)) 
        size += sizeof(long) - (size & (sizeof(long) - 1));

    return size + PREFIX_SIZE;
}
#endif

void
_mc_free(void *ptr, const char *name, int line)
{
    ASSERT(ptr != NULL);
    
#ifndef HAVE_MALLOC_SIZE
    int oldsize;
    ptr = (char*)ptr - PREFIX_SIZE;
#endif

    log_debug(LOG_VVERB, "free(%p) @ %s:%d", ptr, name, line);
#ifdef HAVE_MALLOC_SIZE
    update_mcmalloc_stat_free(mc_alloc_size(ptr));
#else
    oldsize = *((size_t*)ptr);
    update_mcmalloc_stat_free(oldsize + PREFIX_SIZE);
#endif

    free(ptr);
}

size_t
mc_malloc_used_memory(void)
{
    size_t hc = 0;

    if (mc_malloc_thread_safe) {
#ifdef HAVE_ATOMIC
        hc = __sync_add_and_fetch(&heap_curr, 0);
#else
        pthread_mutex_lock(&heap_curr_mutex);
        hc = heap_curr;
        pthread_mutex_unlock(&heap_curr_mutex);
#endif
    } else {
        hc = heap_curr;
    }

    return hc;
}
