MOSTLYCLEANFILES = *.gcov *.gcno *.gcda *.tcov

bin_PROGRAMS = twemcache

twemcache_CPPFLAGS = -D_GNU_SOURCE

twemcache_CFLAGS =
twemcache_CFLAGS += -Wall
twemcache_CFLAGS += -Wmissing-prototypes -Wmissing-declarations -Wredundant-decls
twemcache_CFLAGS += -fno-strict-aliasing
twemcache_CFLAGS += @GCOV_CFLAGS@

twemcache_LDFLAGS = @GCOV_LDFLAGS@
if RDYNAMIC
twemcache_LDFLAGS += -rdynamic
endif

twemcache_SOURCES =			\
	mc_core.c mc_core.h		\
	mc_connection.c mc_connection.h	\
	mc_ascii.c mc_ascii.h		\
	mc_slabs.c mc_slabs.h		\
	mc_items.c mc_items.h		\
	mc_thread.c mc_thread.h		\
	mc_assoc.c mc_assoc.h		\
	mc_stats.c mc_stats.h		\
	mc_signal.c mc_signal.h		\
	mc_log.c mc_log.h		\
	mc_hash.c mc_hash.h		\
	mc_util.c mc_util.h		\
	mc_time.c mc_time.h		\
	mc_queue.h			\
	mc_cache.c mc_cache.h		\
	mc_klog.c mc_klog.h		\
	mc_hotkey.c mc_hotkey.h         \
	mc_ring_array.c mc_ring_array.h \
	mc_key_window.c mc_key_window.h \
	mc_kc_map.c mc_kc_map.h	        \
	mc.c
