#include <mc_core.h>

#include <stdint.h>

extern struct settings settings;

bool hotkey_realloc = false;
static uint64_t hotkey_counter;

#define HOTKEY_WINDOW_SIZE HOTKEY_REDLINE_QPS * HOTKEY_TIMEFRAME / 1000 / HOTKEY_SAMPLE_RATE

static size_t hotkey_redline_qps = HOTKEY_REDLINE_QPS; /* above this qps, hotkey signalling kicks in */
static size_t hotkey_sample_rate = HOTKEY_SAMPLE_RATE; /* sampling rate, one in every hotkey_sample_rate
                                                          gets is sampled */
static size_t hotkey_timeframe = HOTKEY_TIMEFRAME;     /* timeframe for window at redline_qps, default
                                                          1000 msec */
static size_t hotkey_window_size = HOTKEY_WINDOW_SIZE; /* number of samples to keep, calculated as
                                                          redline qps * timeframe / sample rate */
static size_t hotkey_threshold = HOTKEY_QPS_THRESHOLD * HOTKEY_WINDOW_SIZE; /* if key count in the
                                                          window >= hotkey_threshold, signal is given */
static size_t hotkey_bw_threshold = HOTKEY_BW_THRESHOLD; /* if key consumes >= hotkey_bw_threshold
                                                          bytes/sec, signal is given */
static uint64_t hotkey_qps_numerator =
    (uint64_t)HOTKEY_WINDOW_SIZE * (uint64_t)HOTKEY_SAMPLE_RATE * 1000000ULL;
                                                       /* this figure is precalculated as part of the
                                                          calculation for determining realtime qps */

rstatus_t
hotkey_init(void)
{
    rstatus_t status;

    hotkey_redline_qps = settings.hotkey_redline_qps;
    hotkey_sample_rate = settings.hotkey_sample_rate;
    hotkey_window_size = hotkey_redline_qps * hotkey_timeframe / 1000 / hotkey_sample_rate;
    hotkey_threshold = (size_t)(settings.hotkey_qps_threshold * hotkey_window_size);
    hotkey_bw_threshold = settings.hotkey_bw_threshold;
    hotkey_qps_numerator = hotkey_window_size * hotkey_sample_rate * 1000000;
    hotkey_counter = 0;

    if ((status = key_window_init(hotkey_window_size)) != MC_OK) {
        return status;
    }

    if ((status = kc_map_init(hotkey_window_size)) != MC_OK) {
        return status;
    }

    return MC_OK;
}

void
hotkey_deinit(void)
{
    key_window_deinit();
    kc_map_deinit();
}

/* given key count in window, key/val size, duration in usec, calculate bandwidth */
static inline size_t
_get_bandwidth(size_t count, size_t size, size_t usec)
{
    return count * size * hotkey_sample_rate * 1000000 / usec;
}

item_control_flags_t
hotkey_sample(const char *key, size_t klen, size_t vlen)
{
    if (++hotkey_counter % hotkey_sample_rate == 0) {
        /* sample this key */
        size_t count;
        uint64_t qps, bw;
        uint64_t oldest_time, cur_time, time_diff; /* timestamps in usec */

        /* get current time, push key into window with timestamp, then obtain
           the count of that key */
        ASSERT(!key_window_full());
        cur_time = (time_now() * 1000000) + time_now_usec();
        count = key_window_push(key, klen, cur_time);
        stats_thread_incr(hotkey_sampled);

        if (key_window_full()) {
            /* calculate qps using window size, and the amount of time it took to
               fill the window. */
            oldest_time = key_window_pop();
            /* the ternary conditional statement is here to prevent any divide by 0 errs */
            time_diff = (cur_time - oldest_time > 0) ? cur_time - oldest_time : 1;
            qps = hotkey_qps_numerator / time_diff;
            bw = _get_bandwidth(count, klen + vlen, time_diff);

            log_debug(LOG_DEBUG, "count of key %.*s: %d qps: %d bandwidth: %d",
                    klen, key, count, qps, bw);

            /* signal QPS hotkey if qps >= redline and key count >= threshold */
            if (qps >= hotkey_redline_qps && count >= __atomic_load_n(&hotkey_threshold,
                            __ATOMIC_RELAXED)) {
                log_debug(LOG_INFO, "frequency hotkey detected: %.*s", klen, key);
                stats_thread_incr(hotkey_qps);
                return ITEM_HOT_QPS;
            }

            /* signal bandwidth hotkey if bw consumption >= threshold */
            if (bw >= __atomic_load_n(&hotkey_bw_threshold, __ATOMIC_RELAXED)) {
                log_debug(LOG_INFO, "bandwidth hotkey detected: %.*s", klen, key);
                stats_thread_incr(hotkey_bw);
                return ITEM_HOT_BW;
            }
        }
    }

    return 0;
}

static inline rstatus_t
_hotkey_realloc(void)
{
    rstatus_t status;

    hotkey_window_size = hotkey_redline_qps * hotkey_timeframe / 1000 / hotkey_sample_rate;
    hotkey_qps_numerator = hotkey_window_size * hotkey_sample_rate * 1000000;

    key_window_deinit();
    if ((status = key_window_init(hotkey_window_size)) != MC_OK) {
        return status;
    }

    kc_map_deinit();
    if ((status = kc_map_init(hotkey_window_size)) != MC_OK) {
        return status;
    }

    return MC_OK;
}

rstatus_t
hotkey_update_redline(size_t redline)
{
    rstatus_t status;

    ASSERT(!settings.hotkey_enable);

    if (hotkey_redline_qps == redline) {
        return MC_OK;
    }

    __atomic_store_n(&hotkey_realloc, true, __ATOMIC_RELAXED);
    hotkey_redline_qps = settings.hotkey_redline_qps = redline;
    if ((status = _hotkey_realloc()) != MC_OK) {
        return status;
    }
    __atomic_store_n(&hotkey_realloc, false, __ATOMIC_RELAXED);

    return MC_OK;
}

rstatus_t
hotkey_update_sample_rate(size_t sample_rate)
{
    rstatus_t status;

    ASSERT(!settings.hotkey_enable);

    if (hotkey_sample_rate == sample_rate) {
        return MC_OK;
    }

    __atomic_store_n(&hotkey_realloc, true, __ATOMIC_RELAXED);
    hotkey_sample_rate = settings.hotkey_sample_rate = sample_rate;
    if ((status = _hotkey_realloc()) != MC_OK) {
        return status;
    }
    __atomic_store_n(&hotkey_realloc, false, __ATOMIC_RELAXED);

    return MC_OK;
}

void
hotkey_update_qps_threshold(double qps_threshold)
{
    if (settings.hotkey_qps_threshold == qps_threshold) {
        return;
    }
    settings.hotkey_qps_threshold = qps_threshold;
    __atomic_store_n(&hotkey_threshold, (size_t)(settings.hotkey_qps_threshold *
                    hotkey_window_size), __ATOMIC_RELAXED);
}

void
hotkey_update_bw_threshold(size_t bw_threshold)
{
    if (settings.hotkey_bw_threshold == bw_threshold) {
        return;
    }
    settings.hotkey_bw_threshold = bw_threshold;
    __atomic_store_n(&hotkey_bw_threshold, bw_threshold, __ATOMIC_RELAXED);
}
