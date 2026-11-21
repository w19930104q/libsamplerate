/*
** Copyright (c) 2002-2022, Erik de Castro Lopo <erikd@mega-nerd.com>
** All rights reserved.
**
** This code is released under 2-clause BSD license. Please see the
** file at : https://github.com/libsndfile/libsamplerate/blob/master/COPYING
*/

/*
** Boundary regression tests.
**
** Test matrix (all cases run for SRC_SINC_FASTEST, SRC_ZERO_ORDER_HOLD and
** SRC_LINEAR unless noted) :
**
**	| # | Boundary dimension        | Cases enumerated                                            |
**	|---|---------------------------|-------------------------------------------------------------|
**	| 1 | Illegal ratio rejection   | NaN, +Inf, -Inf, 0.0, negative, 1/512, 257, +/-1e300,      |
**	|   |                           | DBL_MIN via src_is_valid_ratio / src_process /             |
**	|   |                           | src_set_ratio / src_callback_read ; boundary ratios        |
**	|   |                           | 1/256 and 256 must stay valid.                             |
**	| 2 | Zero / negative frames    | input_frames = 0 / -N, output_frames = 0 / -N, NULL data   |
**	|   |                           | pointers where legal, state reusable afterwards.           |
**	| 3 | Single-frame varispeed    | 1-frame src_process calls with per-call ratio changes and  |
**	|   |                           | 1-frame src_simple (regression: linear read before buffer).|
**	| 4 | Channel counts            | 1..8 must work ; 0 and negative must be rejected with      |
**	|   |                           | SRC_ERR_BAD_CHANNEL_COUNT ; sinc rejects > MAX_CHANNELS.   |
**	| 5 | Ratio jump sequence       | Abrupt per-block ratio changes 1.0 -> 4 -> 1/4 -> 256 ->   |
**	|   |                           | 1/256 -> 2 -> 1/2 -> 1.0 while streaming.                  |
**	| 6 | Huge output frame count   | output_frames = 2^40 with small input : must terminate     |
**	|   |                           | promptly and only write what it generates.                 |
**	| 7 | Legal-path bit-exactness  | FNV-1a hashes of legal conversion output checked against   |
**	|   |                           | baseline constants (guards against behaviour changes).     |
**
** This test is designed to also run cleanly under AddressSanitizer and
** UndefinedBehaviorSanitizer (configure with -DLIBSAMPLERATE_ENABLE_SANITIZERS=ON).
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <float.h>
#include <math.h>

#include "samplerate.h"

#define ARRAY_LEN(x)	((int) (sizeof (x) / sizeof ((x) [0])))

static int fail_count = 0 ;

#define CHECK(cond, ...) \
	do { if (!(cond)) \
		{	printf ("    FAIL (%s:%d) : ", __func__, __LINE__) ; \
			printf (__VA_ARGS__) ; \
			putchar ('\n') ; \
			fail_count ++ ; \
			} } while (0)

static const int converters [] =
{	SRC_SINC_FASTEST,
	SRC_ZERO_ORDER_HOLD,
	SRC_LINEAR
} ;

/*------------------------------------------------------------------------------
** Deterministic input generator (no libm transcendentals, so results are
** identical across compilers, optimization levels and sanitizers).
*/

static uint32_t lcg_state ;

static void
lcg_seed (uint32_t seed)
{	lcg_state = seed ;
}

static float
lcg_float (void)
{	lcg_state = lcg_state * 1664525u + 1013904223u ;
	/* Divide by a power of two : exact, portable. */
	return (float) ((lcg_state >> 8) & 0xFFFFFFu) / 8388608.0f - 1.0f ;
}

static void
fill_input (float *buf, long frames, int channels)
{	for (long k = 0 ; k < frames * channels ; k++)
		buf [k] = lcg_float () ;
}

static int
all_finite (const float *buf, long len)
{	for (long k = 0 ; k < len ; k++)
		if (! isfinite (buf [k]))
			return 0 ;
	return 1 ;
}

/* Error codes are library-internal ; check identity via the public strings. */
static int
err_is_bad_ratio (int err)
{	const char *s = src_strerror (err) ;
	return s != NULL && strstr (s, "ratio outside") != NULL ;
}

static int
err_is_bad_channel_count (int err)
{	const char *s = src_strerror (err) ;
	return s != NULL && strstr (s, "Channel count") != NULL ;
}

/*------------------------------------------------------------------------------
** FNV-1a (64 bit) hash for the bit-exact legal-path baseline.
*/

static uint64_t
fnv1a_update (uint64_t h, const void *ptr, size_t len)
{	const unsigned char *p = (const unsigned char *) ptr ;
	for (size_t k = 0 ; k < len ; k++)
	{	h ^= p [k] ;
		h *= 1099511628211ULL ;
		} ;
	return h ;
}

/*==============================================================================
** 1. Illegal ratio rejection.
*/

static long
feed_callback (void *data, float **ptr)
{	static float cb_data [4096] ;
	long *pos = (long *) data ;

	if (*pos >= ARRAY_LEN (cb_data))
		return 0 ;

	if (*pos == 0)
	{	lcg_seed (7777u) ;
		fill_input (cb_data, ARRAY_LEN (cb_data), 1) ;
		}

	*ptr = cb_data + *pos ;
	*pos += 512 ;
	return 512 ;
}

static void
bad_ratio_rejection_test (void)
{	static const double bad_ratios [] =
	{	0.0, -1.0, -0.5,				/* zero and negative */
		1.0 / 512.0, 257.0,				/* just outside [1/256, 256] */
		1e300, -1e300,					/* huge */
		DBL_MIN,						/* denormal-adjacent, below 1/256 */
		0.0 / 0.0,						/* NaN */
		1.0 / 0.0, -1.0 / 0.0			/* +Inf, -Inf */
	} ;
	static const double good_ratios [] =
	{	1.0 / 256.0, 1.0 / 255.5, 0.5, 1.0, 2.0, 255.5, 256.0
	} ;
	float input [128], output [512] ;
	float cb_out [256] ;
	int error ;

	printf ("    bad_ratio_rejection_test\n") ;

	for (int k = 0 ; k < ARRAY_LEN (bad_ratios) ; k++)
		CHECK (src_is_valid_ratio (bad_ratios [k]) == 0,
				"src_is_valid_ratio (%g) should be 0", bad_ratios [k]) ;

	for (int k = 0 ; k < ARRAY_LEN (good_ratios) ; k++)
		CHECK (src_is_valid_ratio (good_ratios [k]) != 0,
				"src_is_valid_ratio (%g) should be 1", good_ratios [k]) ;

	lcg_seed (11u) ;
	fill_input (input, ARRAY_LEN (input), 1) ;

	for (int c = 0 ; c < ARRAY_LEN (converters) ; c++)
	{	const char *name = src_get_name (converters [c]) ;
		SRC_STATE *state = src_new (converters [c], 1, &error) ;
		CHECK (state != NULL, "%s : src_new failed", name) ;
		if (state == NULL)
			continue ;

		for (int k = 0 ; k < ARRAY_LEN (bad_ratios) ; k++)
		{	SRC_DATA data ;
			memset (&data, 0, sizeof (data)) ;
			data.data_in = input ;
			data.data_out = output ;
			data.input_frames = ARRAY_LEN (input) ;
			data.output_frames = ARRAY_LEN (output) ;
			data.src_ratio = bad_ratios [k] ;

			int err = src_process (state, &data) ;
			CHECK (err != 0 && err_is_bad_ratio (err),
					"%s : src_process (ratio = %g) returned %d (%s), expected bad-ratio error",
					name, bad_ratios [k], err, src_strerror (err)) ;
			CHECK (data.input_frames_used == 0 && data.output_frames_gen == 0,
					"%s : src_process (ratio = %g) claims used = %ld gen = %ld",
					name, bad_ratios [k], data.input_frames_used, data.output_frames_gen) ;

			err = src_set_ratio (state, bad_ratios [k]) ;
			CHECK (err != 0 && err_is_bad_ratio (err),
					"%s : src_set_ratio (%g) returned %d (%s), expected bad-ratio error",
					name, bad_ratios [k], err, src_strerror (err)) ;
			} ;

		/* The state must still be fully usable after rejected ratios. */
		SRC_DATA data ;
		memset (&data, 0, sizeof (data)) ;
		data.data_in = input ;
		data.data_out = output ;
		data.input_frames = ARRAY_LEN (input) ;
		data.output_frames = ARRAY_LEN (output) ;
		data.src_ratio = 1.25 ;
		data.end_of_input = 1 ;
		int err = src_process (state, &data) ;
		CHECK (err == 0, "%s : legal src_process after bad ratios failed (%s)",
				name, src_strerror (err)) ;
		CHECK (data.output_frames_gen > 0 && all_finite (output, data.output_frames_gen),
				"%s : bad output after rejected ratios", name) ;

		CHECK (src_set_ratio (state, 1.5) == 0, "%s : src_set_ratio (1.5) failed", name) ;

		src_delete (state) ;

		/* Callback API must reject a NaN ratio as well. */
		long cb_pos = 0 ;
		SRC_STATE *cb_state = src_callback_new (feed_callback, converters [c], 1, &error, &cb_pos) ;
		CHECK (cb_state != NULL, "%s : src_callback_new failed", name) ;
		if (cb_state)
		{	double nan_ratio = 0.0 / 0.0 ;
			long got = src_callback_read (cb_state, nan_ratio, ARRAY_LEN (cb_out), cb_out) ;
			CHECK (got == 0, "%s : src_callback_read (NaN) returned %ld", name, got) ;
			CHECK (err_is_bad_ratio (src_error (cb_state)),
					"%s : src_callback_read (NaN) error %d (%s), expected bad-ratio error",
					name, src_error (cb_state), src_strerror (src_error (cb_state))) ;

			got = src_callback_read (cb_state, 1.0, ARRAY_LEN (cb_out), cb_out) ;
			CHECK (got > 0, "%s : legal src_callback_read after NaN returned %ld", name, got) ;

			src_delete (cb_state) ;
			}
		}
} /* bad_ratio_rejection_test */

/*==============================================================================
** 2. Zero and negative frame counts.
*/

static void
zero_negative_frames_test (void)
{	float input [256], output [256] ;

	printf ("    zero_negative_frames_test\n") ;

	lcg_seed (22u) ;
	fill_input (input, ARRAY_LEN (input), 1) ;

	for (int c = 0 ; c < ARRAY_LEN (converters) ; c++)
	{	const char *name = src_get_name (converters [c]) ;
		int error ;
		SRC_STATE *state = src_new (converters [c], 1, &error) ;
		CHECK (state != NULL, "%s : src_new failed", name) ;
		if (state == NULL)
			continue ;

		SRC_DATA data ;

		/* Zero input frames, NULL input pointer (legal). */
		memset (&data, 0, sizeof (data)) ;
		data.data_out = output ;
		data.output_frames = ARRAY_LEN (output) ;
		data.src_ratio = 1.0 ;
		int err = src_process (state, &data) ;
		CHECK (err == 0 && data.input_frames_used == 0 && data.output_frames_gen == 0,
				"%s : zero input frames : err %d (%s) used %ld gen %ld",
				name, err, src_strerror (err), data.input_frames_used, data.output_frames_gen) ;

		/* Zero output frames, NULL output pointer (legal). */
		memset (&data, 0, sizeof (data)) ;
		data.data_in = input ;
		data.input_frames = ARRAY_LEN (input) ;
		data.src_ratio = 1.0 ;
		err = src_process (state, &data) ;
		CHECK (err == 0 && data.output_frames_gen == 0,
				"%s : zero output frames : err %d (%s) gen %ld",
				name, err, src_strerror (err), data.output_frames_gen) ;

		/* Both zero, both NULL. */
		memset (&data, 0, sizeof (data)) ;
		data.src_ratio = 1.0 ;
		err = src_process (state, &data) ;
		CHECK (err == 0, "%s : zero/zero frames : err %d (%s)", name, err, src_strerror (err)) ;

		/* Negative frame counts are clamped to zero. */
		memset (&data, 0, sizeof (data)) ;
		data.data_out = output ;
		data.input_frames = -100 ;
		data.output_frames = ARRAY_LEN (output) ;
		data.src_ratio = 1.0 ;
		err = src_process (state, &data) ;
		CHECK (err == 0 && data.output_frames_gen == 0,
				"%s : negative input frames : err %d (%s) gen %ld",
				name, err, src_strerror (err), data.output_frames_gen) ;

		memset (&data, 0, sizeof (data)) ;
		data.data_in = input ;
		data.input_frames = ARRAY_LEN (input) ;
		data.output_frames = -100 ;
		data.src_ratio = 1.0 ;
		err = src_process (state, &data) ;
		CHECK (err == 0 && data.output_frames_gen == 0,
				"%s : negative output frames : err %d (%s) gen %ld",
				name, err, src_strerror (err), data.output_frames_gen) ;

		/* State must still be usable afterwards. */
		memset (&data, 0, sizeof (data)) ;
		data.data_in = input ;
		data.data_out = output ;
		data.input_frames = ARRAY_LEN (input) ;
		data.output_frames = ARRAY_LEN (output) ;
		data.src_ratio = 1.0 ;
		data.end_of_input = 1 ;
		err = src_process (state, &data) ;
		CHECK (err == 0 && data.output_frames_gen > 0,
				"%s : legal conversion after zero/negative frames failed (%s)",
				name, src_strerror (err)) ;

		src_delete (state) ;
		}
} /* zero_negative_frames_test */

/*==============================================================================
** 3. Single-frame input with per-call ratio changes.
** Regression test : the linear converter used to read before the start of
** the input buffer whenever src_process was called with a single frame.
*/

static void
one_frame_varispeed_test (void)
{	static const double ratios [] = { 0.5, 1.0, 2.0, 3.7, 1.0 / 3.0 } ;
	float output [64 * 2] ;

	printf ("    one_frame_varispeed_test\n") ;

	for (int c = 0 ; c < ARRAY_LEN (converters) ; c++)
	{	const char *name = src_get_name (converters [c]) ;
		int error ;
		SRC_STATE *state = src_new (converters [c], 2, &error) ;
		CHECK (state != NULL, "%s : src_new failed", name) ;
		if (state == NULL)
			continue ;

		lcg_seed (33u + c) ;

		long total_gen = 0 ;
		for (int k = 0 ; k < 64 ; k++)
		{	float frame [2] ;
			frame [0] = lcg_float () ;
			frame [1] = lcg_float () ;

			SRC_DATA data ;
			memset (&data, 0, sizeof (data)) ;
			data.data_in = frame ;
			data.data_out = output ;
			data.input_frames = 1 ;
			data.output_frames = ARRAY_LEN (output) / 2 ;
			data.src_ratio = ratios [k % ARRAY_LEN (ratios)] ;
			data.end_of_input = (k == 63) ;

			int err = src_process (state, &data) ;
			CHECK (err == 0, "%s : 1-frame call %d : err %d (%s)",
					name, k, err, src_strerror (err)) ;
			CHECK (data.input_frames_used >= 0 && data.input_frames_used <= 1,
					"%s : 1-frame call %d : used %ld out of range", name, k, data.input_frames_used) ;
			CHECK (data.output_frames_gen >= 0 && data.output_frames_gen <= ARRAY_LEN (output) / 2,
					"%s : 1-frame call %d : gen %ld out of range", name, k, data.output_frames_gen) ;
			CHECK (all_finite (output, data.output_frames_gen * 2),
					"%s : 1-frame call %d : non-finite output", name, k) ;
			total_gen += data.output_frames_gen ;
			} ;

		CHECK (total_gen > 0, "%s : 64 one-frame calls produced no output", name) ;

		src_delete (state) ;

		/* Single frame through the simple (one-shot) API. */
		float in_frame [2] = { 0.25f, -0.25f } ;
		SRC_DATA data ;
		memset (&data, 0, sizeof (data)) ;
		data.data_in = in_frame ;
		data.data_out = output ;
		data.input_frames = 1 ;
		data.output_frames = ARRAY_LEN (output) / 2 ;
		data.src_ratio = 2.0 ;
		int err = src_simple (&data, converters [c], 2) ;
		CHECK (err == 0, "%s : src_simple with 1 frame : err %d (%s)",
				name, err, src_strerror (err)) ;
		CHECK (data.output_frames_gen >= 0 && data.output_frames_gen <= ARRAY_LEN (output) / 2,
				"%s : src_simple with 1 frame : gen %ld out of range", name, data.output_frames_gen) ;
		}
} /* one_frame_varispeed_test */

/*==============================================================================
** 4. Channel counts : 1..8 must work, 0 and negative must be rejected.
*/

static void
channel_count_test (void)
{	float input [512 * 8], output [1024 * 8] ;

	printf ("    channel_count_test\n") ;

	for (int c = 0 ; c < ARRAY_LEN (converters) ; c++)
	{	const char *name = src_get_name (converters [c]) ;

		for (int ch = 1 ; ch <= 8 ; ch++)
		{	int error = 0 ;
			SRC_STATE *state = src_new (converters [c], ch, &error) ;
			CHECK (state != NULL, "%s : src_new (%d channels) failed : %s",
					name, ch, src_strerror (error)) ;
			if (state == NULL)
				continue ;

			lcg_seed (44u + 16 * c + ch) ;
			fill_input (input, 300, ch) ;

			SRC_DATA data ;
			memset (&data, 0, sizeof (data)) ;
			data.data_in = input ;
			data.data_out = output ;
			data.input_frames = 300 ;
			data.output_frames = 1024 ;
			data.src_ratio = 1.25 ;
			data.end_of_input = 1 ;

			int err = src_process (state, &data) ;
			CHECK (err == 0, "%s : %d channels : err %d (%s)", name, ch, err, src_strerror (err)) ;
			CHECK (data.input_frames_used >= 0 && data.input_frames_used <= 300,
					"%s : %d channels : used %ld out of range", name, ch, data.input_frames_used) ;
			CHECK (data.output_frames_gen > 0 && data.output_frames_gen <= 1024,
					"%s : %d channels : gen %ld out of range", name, ch, data.output_frames_gen) ;
			CHECK (all_finite (output, data.output_frames_gen * ch),
					"%s : %d channels : non-finite output", name, ch) ;

			src_delete (state) ;
			}

		/* Zero and negative channel counts must be rejected, not crash. */
		static const int bad_channels [] = { 0, -1, -100 } ;
		for (int k = 0 ; k < ARRAY_LEN (bad_channels) ; k++)
		{	int error = 0 ;
			SRC_STATE *state = src_new (converters [c], bad_channels [k], &error) ;
			CHECK (state == NULL, "%s : src_new (%d channels) unexpectedly succeeded",
					name, bad_channels [k]) ;
			CHECK (err_is_bad_channel_count (error),
					"%s : src_new (%d channels) error %d (%s), expected bad-channel-count error",
					name, bad_channels [k], error, src_strerror (error)) ;
			if (state != NULL)
				src_delete (state) ;
			}
		}

	/* The sinc converter has a hard channel limit (MAX_CHANNELS = 128). */
	int error = 0 ;
	SRC_STATE *state = src_new (SRC_SINC_FASTEST, 129, &error) ;
	CHECK (state == NULL && err_is_bad_channel_count (error),
			"sinc : src_new (129 channels) : state %p error %d (%s)",
			(void *) state, error, src_strerror (error)) ;
	if (state != NULL)
		src_delete (state) ;
} /* channel_count_test */

/*==============================================================================
** 5. Abrupt ratio changes mid-stream.
*/

static void
ratio_jump_test (void)
{	static const double ratios [] = { 1.0, 4.0, 0.25, 256.0, 1.0 / 256.0, 2.0, 0.5, 1.0 } ;
	const long block_frames = 256 ;
	const long out_cap = block_frames * 256 + 64 ; /* worst case : ratio 256 */
	const int channels = 2 ;

	printf ("    ratio_jump_test\n") ;

	float *input = (float *) malloc (sizeof (float) * block_frames * channels) ;
	float *output = (float *) malloc (sizeof (float) * out_cap * channels) ;
	CHECK (input != NULL && output != NULL, "malloc failed") ;
	if (input == NULL || output == NULL)
	{	free (input) ;
		free (output) ;
		return ;
		}

	for (int c = 0 ; c < ARRAY_LEN (converters) ; c++)
	{	const char *name = src_get_name (converters [c]) ;
		int error ;
		SRC_STATE *state = src_new (converters [c], channels, &error) ;
		CHECK (state != NULL, "%s : src_new failed", name) ;
		if (state == NULL)
			continue ;

		lcg_seed (55u + c) ;

		for (int k = 0 ; k < ARRAY_LEN (ratios) ; k++)
		{	fill_input (input, block_frames, channels) ;

			SRC_DATA data ;
			memset (&data, 0, sizeof (data)) ;
			data.data_in = input ;
			data.data_out = output ;
			data.input_frames = block_frames ;
			data.output_frames = out_cap ;
			data.src_ratio = ratios [k] ;
			data.end_of_input = (k == ARRAY_LEN (ratios) - 1) ;

			int err = src_process (state, &data) ;
			CHECK (err == 0, "%s : ratio jump to %g (block %d) : err %d (%s)",
					name, ratios [k], k, err, src_strerror (err)) ;
			CHECK (data.input_frames_used >= 0 && data.input_frames_used <= block_frames,
					"%s : ratio %g : used %ld out of range", name, ratios [k], data.input_frames_used) ;
			CHECK (data.output_frames_gen >= 0 && data.output_frames_gen <= out_cap,
					"%s : ratio %g : gen %ld out of range", name, ratios [k], data.output_frames_gen) ;
			CHECK (all_finite (output, data.output_frames_gen * channels),
					"%s : ratio %g : non-finite output", name, ratios [k]) ;
			} ;

		/* Reset and confirm normal operation after the jump sequence. */
		src_reset (state) ;
		fill_input (input, block_frames, channels) ;
		SRC_DATA data ;
		memset (&data, 0, sizeof (data)) ;
		data.data_in = input ;
		data.data_out = output ;
		data.input_frames = block_frames ;
		data.output_frames = out_cap ;
		data.src_ratio = 1.0 ;
		data.end_of_input = 1 ;
		int err = src_process (state, &data) ;
		CHECK (err == 0 && data.output_frames_gen > 0,
				"%s : conversion after ratio jumps failed (%s)", name, src_strerror (err)) ;

		src_delete (state) ;
		}

	free (input) ;
	free (output) ;
} /* ratio_jump_test */

/*==============================================================================
** 6. Huge declared output frame count : the converter must terminate once
** the input is exhausted and must not write more than it reports.
*/

static void
huge_output_frames_test (void)
{	const long in_frames = 1000 ;
	const long out_cap = 8000 ; /* actual backing store, much smaller than declared */
	const int channels = 2 ;
	const long huge_declared = 1L << 40 ;

	printf ("    huge_output_frames_test\n") ;

	/* Single arena with input first so the library's overlap check passes. */
	float *arena = (float *) malloc (sizeof (float) * (in_frames + out_cap) * channels) ;
	CHECK (arena != NULL, "malloc failed") ;
	if (arena == NULL)
		return ;

	float *input = arena ;
	float *output = arena + in_frames * channels ;

	for (int c = 0 ; c < ARRAY_LEN (converters) ; c++)
	{	const char *name = src_get_name (converters [c]) ;

		static const double ratios [] = { 1.0, 2.0, 0.5 } ;
		for (int r = 0 ; r < ARRAY_LEN (ratios) ; r++)
		{	int error ;
			SRC_STATE *state = src_new (converters [c], channels, &error) ;
			CHECK (state != NULL, "%s : src_new failed", name) ;
			if (state == NULL)
				continue ;

			lcg_seed (66u + 8 * c + r) ;
			fill_input (input, in_frames, channels) ;
			memset (output, 0, sizeof (float) * out_cap * channels) ;

			SRC_DATA data ;
			memset (&data, 0, sizeof (data)) ;
			data.data_in = input ;
			data.data_out = output ;
			data.input_frames = in_frames ;
			data.output_frames = huge_declared ;
			data.src_ratio = ratios [r] ;
			data.end_of_input = 1 ;

			int err = src_process (state, &data) ;
			CHECK (err == 0, "%s : huge output_frames, ratio %g : err %d (%s)",
					name, ratios [r], err, src_strerror (err)) ;

			long expect_max = (long) (in_frames * ratios [r]) + 512 ;
			CHECK (data.output_frames_gen > 0 && data.output_frames_gen <= expect_max,
					"%s : huge output_frames, ratio %g : gen %ld (expected <= %ld)",
					name, ratios [r], data.output_frames_gen, expect_max) ;
			CHECK (data.input_frames_used >= 0 && data.input_frames_used <= in_frames,
					"%s : huge output_frames, ratio %g : used %ld out of range",
					name, ratios [r], data.input_frames_used) ;
			CHECK (all_finite (output, data.output_frames_gen * channels),
					"%s : huge output_frames, ratio %g : non-finite output", name, ratios [r]) ;

			src_delete (state) ;
			}
		}

	free (arena) ;
} /* huge_output_frames_test */

/*==============================================================================
** 7. Legal-path bit-exact baseline. The expected hashes below were generated
** from the unmodified library ; any behaviour change on legal inputs breaks
** this test. Run with BOUNDARY_PRINT_HASHES=1 to print actual hashes.
*/

static uint64_t
bitexact_scenario (int converter, int scenario)
{	uint64_t h = 1469598103934665603ULL ;
	int error ;

	if (scenario == 0)
	{	/* One-shot, stereo, ratio 1.5. */
		const long in_frames = 4096, out_cap = 8192 ;
		const int ch = 2 ;
		float *in = (float *) malloc (sizeof (float) * in_frames * ch) ;
		float *out = (float *) malloc (sizeof (float) * out_cap * ch) ;

		lcg_seed (1234u + (uint32_t) converter) ;
		fill_input (in, in_frames, ch) ;

		SRC_STATE *state = src_new (converter, ch, &error) ;
		SRC_DATA data ;
		memset (&data, 0, sizeof (data)) ;
		data.data_in = in ;
		data.data_out = out ;
		data.input_frames = in_frames ;
		data.output_frames = out_cap ;
		data.src_ratio = 1.5 ;
		data.end_of_input = 1 ;
		if (src_process (state, &data) != 0)
			fail_count ++ ;

		h = fnv1a_update (h, out, (size_t) data.output_frames_gen * ch * sizeof (float)) ;
		h = fnv1a_update (h, &data.input_frames_used, sizeof (data.input_frames_used)) ;
		h = fnv1a_update (h, &data.output_frames_gen, sizeof (data.output_frames_gen)) ;

		src_delete (state) ;
		free (in) ;
		free (out) ;
		}
	else if (scenario == 1)
	{	/* Streaming varispeed ramp, 3 channels (sinc multichan path). */
		const long block = 257, out_cap = 2048 ;
		const int ch = 3 ;
		float *in = (float *) malloc (sizeof (float) * block * ch) ;
		float *out = (float *) malloc (sizeof (float) * out_cap * ch) ;

		SRC_STATE *state = src_new (converter, ch, &error) ;
		lcg_seed (9876u + (uint32_t) converter) ;

		for (int k = 0 ; k < 12 ; k++)
		{	fill_input (in, block, ch) ;

			SRC_DATA data ;
			memset (&data, 0, sizeof (data)) ;
			data.data_in = in ;
			data.data_out = out ;
			data.input_frames = block ;
			data.output_frames = out_cap ;
			data.src_ratio = 0.9 + 0.07 * k ;
			data.end_of_input = (k == 11) ;
			if (src_process (state, &data) != 0)
				fail_count ++ ;

			h = fnv1a_update (h, out, (size_t) data.output_frames_gen * ch * sizeof (float)) ;
			h = fnv1a_update (h, &data.input_frames_used, sizeof (data.input_frames_used)) ;
			h = fnv1a_update (h, &data.output_frames_gen, sizeof (data.output_frames_gen)) ;
			} ;

		src_delete (state) ;
		free (in) ;
		free (out) ;
		}
	else
	{	/* Extreme legal ratios, mono : 1/256 then 256. */
		const int ch = 1 ;

		lcg_seed (555u + (uint32_t) converter) ;

		const long in_a = 8192, out_a = 512 ;
		float *in = (float *) malloc (sizeof (float) * in_a) ;
		float *out = (float *) malloc (sizeof (float) * out_a) ;
		fill_input (in, in_a, ch) ;

		SRC_STATE *state = src_new (converter, ch, &error) ;
		SRC_DATA data ;
		memset (&data, 0, sizeof (data)) ;
		data.data_in = in ;
		data.data_out = out ;
		data.input_frames = in_a ;
		data.output_frames = out_a ;
		data.src_ratio = 1.0 / 256.0 ;
		data.end_of_input = 1 ;
		if (src_process (state, &data) != 0)
			fail_count ++ ;
		h = fnv1a_update (h, out, (size_t) data.output_frames_gen * sizeof (float)) ;
		h = fnv1a_update (h, &data.input_frames_used, sizeof (data.input_frames_used)) ;
		h = fnv1a_update (h, &data.output_frames_gen, sizeof (data.output_frames_gen)) ;
		src_delete (state) ;

		const long in_b = 64, out_b = 64 * 256 + 512 ;
		float *in2 = (float *) malloc (sizeof (float) * in_b) ;
		float *out2 = (float *) malloc (sizeof (float) * out_b) ;
		fill_input (in2, in_b, ch) ;

		state = src_new (converter, ch, &error) ;
		memset (&data, 0, sizeof (data)) ;
		data.data_in = in2 ;
		data.data_out = out2 ;
		data.input_frames = in_b ;
		data.output_frames = out_b ;
		data.src_ratio = 256.0 ;
		data.end_of_input = 1 ;
		if (src_process (state, &data) != 0)
			fail_count ++ ;
		h = fnv1a_update (h, out2, (size_t) data.output_frames_gen * sizeof (float)) ;
		h = fnv1a_update (h, &data.input_frames_used, sizeof (data.input_frames_used)) ;
		h = fnv1a_update (h, &data.output_frames_gen, sizeof (data.output_frames_gen)) ;
		src_delete (state) ;

		free (in) ;
		free (out) ;
		free (in2) ;
		free (out2) ;
		}

	return h ;
} /* bitexact_scenario */

static const uint64_t bitexact_expected [3] [3] =
{	/*		one-shot r=1.5			streaming ramp			extreme ratios	*/
	{ 0x2459d013d752e2afULL, 0x93eb2d24bd24a81eULL, 0xad43960b271d4155ULL },	/* sinc		*/
	{ 0xe16eb937a519fe33ULL, 0xdd6d68b418f6b315ULL, 0x525b0941bceb8f8cULL },	/* zoh		*/
	{ 0x5378a6027b8bd8f1ULL, 0x9ecbd5e229f04bbbULL, 0xaa2ff40efbeca946ULL }	/* linear	*/
} ;

static void
bitexact_baseline_test (void)
{	int print_mode = getenv ("BOUNDARY_PRINT_HASHES") != NULL ;

	printf ("    bitexact_baseline_test\n") ;

	for (int c = 0 ; c < ARRAY_LEN (converters) ; c++)
		for (int s = 0 ; s < 3 ; s++)
		{	uint64_t h = bitexact_scenario (converters [c], s) ;
			if (print_mode)
			{	printf ("    # hash converter %d scenario %d : 0x%016llxULL\n",
						c, s, (unsigned long long) h) ;
				continue ;
				}
			CHECK (h == bitexact_expected [c] [s],
					"%s scenario %d : hash 0x%016llx != baseline 0x%016llx",
					src_get_name (converters [c]), s,
					(unsigned long long) h, (unsigned long long) bitexact_expected [c] [s]) ;
			} ;
} /* bitexact_baseline_test */

/*==============================================================================
*/

int
main (void)
{	if (getenv ("BOUNDARY_PRINT_HASHES") != NULL)
	{	bitexact_baseline_test () ;
		return 0 ;
		}

	puts ("\nBoundary regression tests :") ;

	bad_ratio_rejection_test () ;
	zero_negative_frames_test () ;
	one_frame_varispeed_test () ;
	channel_count_test () ;
	ratio_jump_test () ;
	huge_output_frames_test () ;
	bitexact_baseline_test () ;

	if (fail_count > 0)
	{	printf ("\nboundary_test : %d check(s) FAILED.\n\n", fail_count) ;
		return 1 ;
		}

	puts ("boundary_test : all checks passed.\n") ;
	return 0 ;
} /* main */
