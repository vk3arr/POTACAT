#ifndef _INCLUDE_FT2_SYNC_H_
#define _INCLUDE_FT2_SYNC_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float freq_hz;      /* carrier frequency estimate (Hz) */
    float time_offset;  /* sample offset within input buffer */
    float score;        /* sync correlation score */
} ft2_candidate_t;

/*
 * Search for FT2 sync patterns in audio.
 * Input: signal of num_samples floats at 12000 Hz sample rate.
 * Output: up to max_candidates candidates sorted by descending score.
 * Returns number of candidates found.
 */
int ft2_find_candidates(const float* signal, int num_samples,
                        ft2_candidate_t* candidates, int max_candidates);

#ifdef __cplusplus
}
#endif

#endif /* _INCLUDE_FT2_SYNC_H_ */
