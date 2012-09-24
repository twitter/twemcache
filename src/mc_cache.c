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

#include <stdlib.h>
#include <string.h>

#include <mc_cache.h>
#include <mc_core.h>

const int initial_pool_size = 64;

/*
 * Create an object cache.
 *
 * The object cache will let you allocate objects of the same size. It is
 * fully MT safe, so you may allocate objects from multiple threads without
 * having to do any syncrhonization in the application code.
 *
 * @param name the name of the object cache. This name may be used for debug
 *             purposes and may help you track down what kind of object you
 *             have problems with (buffer overruns, leakage etc)
 * @param bufsize the size of each object in the cache
 * @param align the alignment requirements of the objects in the cache.
 * @return a handle to an object cache if successful, NULL otherwise.
 */
cache_t *
cache_create(const char *name, size_t bufsize, size_t align)
{
    cache_t *ret;
    char *name_new;
    void **ptr;

    ret = mc_calloc(1, sizeof(cache_t));
    name_new = mc_alloc(strlen(name) + 1);
    ptr = mc_calloc(initial_pool_size, bufsize);

    if (ret == NULL || name_new == NULL || ptr == NULL ||
        pthread_mutex_init(&ret->mutex, NULL) == -1) {
        mc_free(ret);
        mc_free(name_new);
        mc_free(ptr);
        return NULL;
    }

    strncpy(name_new, name, strlen(name) + 1);
    ret->name = name_new;
    ret->ptr = ptr;
    ret->freetotal = initial_pool_size;

    ret->bufsize = bufsize;

    return ret;
}

/*
 * Destroy an object cache.
 *
 * Destroy and invalidate an object cache. You should return all buffers
 * allocated with cache_alloc by using cache_free before calling this
 * function. Not doing so results in undefined behavior (the buffers may
 * or may not be invalidated)
 *
 * @param handle the handle to the object cache to destroy.
 */
void
cache_destroy(cache_t *cache)
{
    while (cache->freecurr > 0) {
        void *buf = cache->ptr[--cache->freecurr];
        mc_free(buf);
    }
    mc_free(cache->name);
    mc_free(cache->ptr);
    pthread_mutex_destroy(&cache->mutex);
    mc_free(cache);
}

/*
 * Allocate an object from the cache.
 *
 * @param handle the handle to the object cache to allocate from
 * @return a pointer to an initialized object from the cache, or NULL if
 *         the allocation cannot be satisfied.
 */
void *
cache_alloc(cache_t *cache)
{
    void *object;

    pthread_mutex_lock(&cache->mutex);
    if (cache->freecurr > 0) {
        object = cache->ptr[--cache->freecurr];
    } else {
        object = mc_alloc(cache->bufsize);
    }
    pthread_mutex_unlock(&cache->mutex);

    return object;
}

/*
 * Return an object back to the cache.
 *
 * The caller should return the object in an initialized state so that
 * the object may be returned in an expected state from cache_alloc.
 *
 * @param handle handle to the object cache to return the object to
 * @param ptr pointer to the object to return.
 */
void
cache_free(cache_t *cache, void *buf)
{
    pthread_mutex_lock(&cache->mutex);

    if (cache->freecurr < cache->freetotal) {
        cache->ptr[cache->freecurr++] = buf;
    } else {
        /* try to enlarge free connections array */
        size_t newtotal = cache->freetotal * 2;
        void **new_free = mc_realloc(cache->ptr, sizeof(char *) * newtotal);
        if (new_free != NULL) {
            cache->freetotal = newtotal;
            cache->ptr = new_free;
            cache->ptr[cache->freecurr++] = buf;
        } else {
            mc_free(buf);

        }
    }
    pthread_mutex_unlock(&cache->mutex);
}
