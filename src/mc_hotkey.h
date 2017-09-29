#ifndef _MC_HOTKEY_H_
#define _MC_HOTKEY_H_

#include <stdlib.h>

#include <mc_core.h>

#define MAX_KEY_LEN 255

#define HOTKEY_REDLINE_QPS     80000  /* begin signalling hotkey if observed qps >= HOTKEY_REDLINE_QPS */
#define HOTKEY_SAMPLE_RATE     100    /* sample one in every HOTKEY_SAMPLE_RATE keys */
#define HOTKEY_TIMEFRAME       1000   /* in milliseconds. we calculate window size based on timeframe and
                                         sample rate at the redline qps */
#define HOTKEY_QPS_THRESHOLD   0.01   /* signal hotkey if key is observed taking >= HOTKEY_QPS_THRESHOLD
                                         of the traffic in the window */
#define HOTKEY_BW_THRESHOLD    200000 /* signal hotkey if key is observed taking >= HOTKEY_BW_THRESHOLD
                                         bytes/second of bandwidth */

extern bool hotkey_realloc;

rstatus_t hotkey_init(void);
void hotkey_deinit(void);
item_control_flags_t hotkey_sample(const char *key, size_t klen, size_t vlen);

rstatus_t hotkey_update_redline(size_t redline);
rstatus_t hotkey_update_sample_rate(size_t sample_rate);
void hotkey_update_qps_threshold(double qps_threshold);
void hotkey_update_bw_threshold(size_t bw_threshold);

#endif
