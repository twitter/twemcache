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

#ifndef _MC_ALLOC_H_
#define _MC_ALLOC_H_

#include <stdarg.h>
#include <stddef.h>

/*
 * Make data 'd' or pointer 'p', n-byte aligned, where n is a power of 2
 * of 2.
 */
#define MC_ALIGNMENT        sizeof(unsigned long) /* platform word */
#define MC_ALIGN(d, n)      ((size_t)(((d) + (n - 1)) & ~(n - 1)))
#define MC_ALIGN_PTR(p, n)  \
    (void *) (((uintptr_t) (p) + ((uintptr_t) n - 1)) & ~((uintptr_t) n - 1))

/*
 * Memory allocation and free wrappers.
 *
 * These wrappers enables us to loosely detect double free, dangling
 * pointer access and zero-byte alloc.
 */
#define mc_alloc(_s)                    \
    _mc_alloc((size_t)(_s), __FILE__, __LINE__)

#define mc_zalloc(_s)                   \
    _mc_zalloc((size_t)(_s), __FILE__, __LINE__)

#define mc_calloc(_n, _s)               \
    _mc_calloc((size_t)(_n), (size_t)(_s), __FILE__, __LINE__)

#define mc_realloc(_p, _s)              \
    _mc_realloc(_p, (size_t)(_s), __FILE__, __LINE__)

#define mc_free(_p) do {                \
    _mc_free(_p, __FILE__, __LINE__);   \
    (_p) = NULL;                        \
} while (0)

void *_mc_alloc(size_t size, const char *name, int line);
void *_mc_zalloc(size_t size, const char *name, int line);
void *_mc_calloc(size_t nmemb, size_t size, const char *name, int line);
void *_mc_realloc(void *ptr, size_t size, const char *name, int line);
void _mc_free(void *ptr, const char *name, int line);

#endif
