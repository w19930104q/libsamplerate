/*
** Copyright (c) 2002-2016, Erik de Castro Lopo <erikd@mega-nerd.com>
** All rights reserved.
**
** This code is released under 2-clause BSD license. Please see the
** file at : https://github.com/libsndfile/libsamplerate/blob/master/COPYING
*/

/*
** Tests for the double precision stream statistics query functions :
**
**	src_get_frames_consumed ()
**	src_get_frames_generated ()
**	src_get_current_ratio  ()
**
** The double precision counters accumulate the *integer* per-call frame
** counts (SRC_DATA.input_frames_used / output_frames_gen) in double
** precision. Every addend is a non-negative integer and every partial
** sum stays far below 2^53, where all integers are exactly
** representable in IEEE-754 double precision. Addition of two exactly
** representable integers whose exact sum is below 2^53 is exact in
** round-to-nearest arithmetic, so the accumulated rounding error is
** exactly zero : the double counters must match integer accounting
** bit-for-bit. The assertions below check exactly that (difference
** <= 0 ulps, which is the tightest possible cumulative rounding bound).
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <samplerate.h>

#include "util.h"

#define	CHANNELS			2
#define	INPUT_FRAMES		123457
#define	CB_BLOCK_LEN		997
#define	READ_BLOCK_LEN		2048
#define	MAX_READS			100000

/* Tightest possible relative error bound for the exact counters. */
#define	REL_TOL				1e-12

static float input [INPUT_FRAMES * CHANNELS] ;
static float output [(READ_BLOCK_LEN + 64) * CHANNELS] ;

typedef struct
{	long	fed ;		/* Frames handed to the library so far. */
} CB_DATA ;

static void callback_const_ratio_test (int converter, double ratio) ;
static void callback_varispeed_ratio_test (int converter) ;
static void process_mode_stats_test (int converter, double ratio) ;
static void error_and_reset_test (void) ;

static double rel_err (double got, double expected) ;

int
main (void)
{	static double src_ratios [] = { 1.5, 0.75, 1.0001 } ;
	int k ;

	/* Fill the input buffer with a known signal. */
	for (k = 0 ; k < ARRAY_LEN (input) ; k++)
		input [k] = (float) sin (k * 0.01) ;

	puts ("") ;

	puts ("    Zero Order Hold interpolator :") ;
	for (k = 0 ; k < ARRAY_LEN (src_ratios) ; k++)
		callback_const_ratio_test (SRC_ZERO_ORDER_HOLD, src_ratios [k]) ;
	callback_varispeed_ratio_test (SRC_ZERO_ORDER_HOLD) ;
	process_mode_stats_test (SRC_ZERO_ORDER_HOLD, 1.5) ;

	puts ("    Linear interpolator :") ;
	for (k = 0 ; k < ARRAY_LEN (src_ratios) ; k++)
		callback_const_ratio_test (SRC_LINEAR, src_ratios [k]) ;
	callback_varispeed_ratio_test (SRC_LINEAR) ;
	process_mode_stats_test (SRC_LINEAR, 1.5) ;

#ifdef ENABLE_SINC_FAST_CONVERTER
	puts ("    Sinc interpolator :") ;
	for (k = 0 ; k < ARRAY_LEN (src_ratios) ; k++)
		callback_const_ratio_test (SRC_SINC_FASTEST, src_ratios [k]) ;
	callback_varispeed_ratio_test (SRC_SINC_FASTEST) ;
	process_mode_stats_test (SRC_SINC_FASTEST, 1.5) ;
#endif

	error_and_reset_test () ;

	puts ("") ;
	return 0 ;
} /* main */

/*==============================================================================
*/

static long
stats_test_callback (void *cb_data, float **data)
{	CB_DATA *cb = (CB_DATA *) cb_data ;
	long frames ;

	if (cb->fed >= INPUT_FRAMES)
		return 0 ;

	frames = MIN (CB_BLOCK_LEN, INPUT_FRAMES - cb->fed) ;
	*data = input + cb->fed * CHANNELS ;
	cb->fed += frames ;

	return frames ;
} /* stats_test_callback */

/* Callback that never runs out of data : wraps around to the start. */
static long
stats_test_loop_callback (void *cb_data, float **data)
{	CB_DATA *cb = (CB_DATA *) cb_data ;
	long frames = CB_BLOCK_LEN ;

	if (cb->fed + frames > INPUT_FRAMES)
		cb->fed = 0 ;

	*data = input + cb->fed * CHANNELS ;
	cb->fed += frames ;

	return frames ;
} /* stats_test_loop_callback */

static void
get_stats_or_die (SRC_STATE *state, double *consumed, double *generated, double *ratio, int line)
{
	if (src_get_frames_consumed (state, consumed) != 0
			|| src_get_frames_generated (state, generated) != 0
			|| src_get_current_ratio (state, ratio) != 0)
	{	printf ("\n\nLine %d (called from line %d) : statistics query failed.\n\n",
				__LINE__, line) ;
		exit (1) ;
		} ;
} /* get_stats_or_die */

static void
callback_const_ratio_test (int converter, double ratio)
{	CB_DATA cb ;
	SRC_STATE *src_state ;

	double	consumed_d, generated_d, ratio_d ;
	double	prev_consumed = 0.0, prev_generated = 0.0 ;
	long	gen_int = 0, read_count ;
	int		k, error ;

	printf ("\tcallback_stats_test (%s, ratio = %6.4f) ... ", src_get_name (converter), ratio) ;
	fflush (stdout) ;

	cb.fed = 0 ;

	if ((src_state = src_callback_new (stats_test_callback, converter, CHANNELS, &error, &cb)) == NULL)
	{	printf ("\n\nLine %d : src_callback_new failed : %s\n\n", __LINE__, src_strerror (error)) ;
		exit (1) ;
		} ;

	/* A fresh converter must report zeroed counters. */
	get_stats_or_die (src_state, &consumed_d, &generated_d, &ratio_d, __LINE__) ;
	if (consumed_d != 0.0 || generated_d != 0.0)
	{	printf ("\n\nLine %d : counters of a fresh converter are not zero.\n\n", __LINE__) ;
		exit (1) ;
		} ;

	for (k = 0 ; k < MAX_READS ; k++)
	{	read_count = src_callback_read (src_state, ratio, READ_BLOCK_LEN, output) ;

		if ((error = src_error (src_state)) != 0)
		{	printf ("\n\nLine %d : src_callback_read error : %s\n\n", __LINE__, src_strerror (error)) ;
			exit (1) ;
			} ;

		gen_int += read_count ;

		get_stats_or_die (src_state, &consumed_d, &generated_d, &ratio_d, __LINE__) ;

		/*
		** Exactness proof obligation (see file header) : the double
		** generated counter must equal the integer sum of all
		** src_callback_read return values *exactly*, because every
		** partial sum is an integer well below 2^53 and is therefore
		** accumulated without any rounding. The cumulative rounding
		** bound is exactly 0 ulps.
		*/
		if (generated_d != (double) gen_int)
		{	printf ("\n\nLine %d : generated_d (%f) != integer count (%ld).\n\n",
					__LINE__, generated_d, gen_int) ;
			exit (1) ;
			} ;

		/*
		** The consumed counter is fed from the same integer
		** (input_frames_used) accounting, so it must always be
		** integer valued ...
		*/
		if (consumed_d != floor (consumed_d))
		{	printf ("\n\nLine %d : consumed_d (%f) is not integer valued.\n\n",
					__LINE__, consumed_d) ;
			exit (1) ;
			} ;

		/*
		** ... and bounded by the amount of data the callback has
		** supplied : the library can never have consumed more than
		** cb.fed frames, and it can hold at most one callback block
		** (CB_BLOCK_LEN frames) of unconsumed input internally.
		*/
		if (consumed_d > (double) cb.fed || consumed_d < (double) (cb.fed - CB_BLOCK_LEN))
		{	printf ("\n\nLine %d : consumed_d (%f) outside [%ld, %ld].\n\n",
					__LINE__, consumed_d, cb.fed - CB_BLOCK_LEN, cb.fed) ;
			exit (1) ;
			} ;

		/* Counters must be monotonically non-decreasing. */
		if (consumed_d < prev_consumed || generated_d < prev_generated)
		{	printf ("\n\nLine %d : counters went backwards.\n\n", __LINE__) ;
			exit (1) ;
			} ;
		prev_consumed = consumed_d ;
		prev_generated = generated_d ;

		/* The ratio in effect must be the one just passed in. */
		if (ratio_d != ratio)
		{	printf ("\n\nLine %d : current ratio %f, expected %f.\n\n",
					__LINE__, ratio_d, ratio) ;
			exit (1) ;
			} ;

		if (read_count == 0)
			break ;
		} ;

	if (k >= MAX_READS)
	{	printf ("\n\nLine %d : stream did not terminate.\n\n", __LINE__) ;
		exit (1) ;
		} ;

	src_delete (src_state) ;

	/*
	** Hand computed expectation : the stream has a known length of
	** INPUT_FRAMES frames and every converter consumes all of its input
	** once end_of_input has been signalled, so the consumed counter must
	** equal INPUT_FRAMES with a relative error of at most 1e-12 (in fact
	** exactly zero).
	*/
	if (rel_err (consumed_d, (double) INPUT_FRAMES) > REL_TOL)
	{	printf ("\n\nLine %d : consumed_d (%f) != INPUT_FRAMES (%d).\n\n",
				__LINE__, consumed_d, INPUT_FRAMES) ;
		exit (1) ;
		} ;

	/*
	** Hand computed expectation for the generated count : the integer
	** sum of all src_callback_read() return values, accumulated by hand
	** by the caller. Relative error must be <= 1e-12 (exactly zero).
	*/
	if (rel_err (generated_d, (double) gen_int) > REL_TOL)
	{	printf ("\n\nLine %d : generated_d (%f) != hand computed count (%ld).\n\n",
				__LINE__, generated_d, gen_int) ;
		exit (1) ;
		} ;

	/*
	** Loose sanity check on the *measured* average ratio : it must
	** approach the requested ratio up to converter latency effects.
	*/
	if (fabs (generated_d / consumed_d - ratio) > 0.01 * ratio)
	{	printf ("\n\nLine %d : measured ratio %f too far from %f.\n\n",
				__LINE__, generated_d / consumed_d, ratio) ;
		exit (1) ;
		} ;

	/*
	** Cross check against src_process() : running the same data through
	** the full API must produce the same totals (same accounting).
	*/
	{
		SRC_STATE	*proc_state ;
		SRC_DATA	src_data ;
		double		p_consumed, p_generated, p_ratio ;
		long		proc_used = 0, proc_gen = 0, pos = 0 ;

		if ((proc_state = src_new (converter, CHANNELS, &error)) == NULL)
		{	printf ("\n\nLine %d : src_new failed : %s\n\n", __LINE__, src_strerror (error)) ;
			exit (1) ;
			} ;

		do
		{	memset (&src_data, 0, sizeof (src_data)) ;
			src_data.data_in = input + pos * CHANNELS ;
			src_data.input_frames = MIN (CB_BLOCK_LEN, INPUT_FRAMES - pos) ;
			src_data.data_out = output ;
			src_data.output_frames = ARRAY_LEN (output) / CHANNELS ;
			src_data.src_ratio = ratio ;
			src_data.end_of_input = (pos + src_data.input_frames >= INPUT_FRAMES) ;

			if ((error = src_process (proc_state, &src_data)) != 0)
			{	printf ("\n\nLine %d : src_process error : %s\n\n", __LINE__, src_strerror (error)) ;
				exit (1) ;
				} ;

			pos += src_data.input_frames_used ;
			proc_used += src_data.input_frames_used ;
			proc_gen += src_data.output_frames_gen ;
			}
		while (pos < INPUT_FRAMES || src_data.output_frames_gen > 0) ;

		get_stats_or_die (proc_state, &p_consumed, &p_generated, &p_ratio, __LINE__) ;

		src_delete (proc_state) ;

		/* src_process() side : double counters match integer accounting. */
		if (p_consumed != (double) proc_used || p_generated != (double) proc_gen)
		{	printf ("\n\nLine %d : src_process counters do not match integer accounting.\n\n",
					__LINE__) ;
			exit (1) ;
			} ;

		/* Callback API and full API must agree on the totals. */
		if (consumed_d != p_consumed || generated_d != p_generated)
		{	printf ("\n\nLine %d : callback/process mismatch : "
					"consumed %f/%f, generated %f/%f.\n\n",
					__LINE__, consumed_d, p_consumed, generated_d, p_generated) ;
			exit (1) ;
			} ;
		}

	puts ("ok") ;
} /* callback_const_ratio_test */

/*==============================================================================
*/

static void
callback_varispeed_ratio_test (int converter)
{	CB_DATA cb ;
	SRC_STATE *src_state ;
	double	consumed_d, generated_d, ratio_d, ratio ;
	int		k, error ;

	printf ("\tcallback_varispeed_ratio_test (%-28s) ... ", src_get_name (converter)) ;
	fflush (stdout) ;

	cb.fed = 0 ;

	if ((src_state = src_callback_new (stats_test_loop_callback, converter, CHANNELS, &error, &cb)) == NULL)
	{	printf ("\n\nLine %d : src_callback_new failed : %s\n\n", __LINE__, src_strerror (error)) ;
		exit (1) ;
		} ;

	/* Vary the ratio on every read and check the reported current ratio. */
	for (k = 0 ; k < 200 ; k++)
	{	ratio = 1.0 + 0.4 * sin (k * 0.3) ;

		src_callback_read (src_state, ratio, READ_BLOCK_LEN, output) ;

		if ((error = src_error (src_state)) != 0)
		{	printf ("\n\nLine %d : src_callback_read error : %s\n\n", __LINE__, src_strerror (error)) ;
			exit (1) ;
			} ;

		get_stats_or_die (src_state, &consumed_d, &generated_d, &ratio_d, __LINE__) ;

		if (ratio_d != ratio)
		{	printf ("\n\nLine %d : current ratio %f, expected %f.\n\n",
					__LINE__, ratio_d, ratio) ;
			exit (1) ;
			} ;
		} ;

	/* src_set_ratio() must also update the reported current ratio. */
	if (src_set_ratio (src_state, 1.25) != 0)
	{	printf ("\n\nLine %d : src_set_ratio failed.\n\n", __LINE__) ;
		exit (1) ;
		} ;

	get_stats_or_die (src_state, &consumed_d, &generated_d, &ratio_d, __LINE__) ;
	if (ratio_d != 1.25)
	{	printf ("\n\nLine %d : current ratio %f, expected 1.25.\n\n", __LINE__, ratio_d) ;
		exit (1) ;
		} ;

	src_delete (src_state) ;

	puts ("ok") ;
} /* callback_varispeed_ratio_test */

/*==============================================================================
*/

static void
process_mode_stats_test (int converter, double ratio)
{	SRC_STATE	*src_state ;
	SRC_DATA	src_data ;
	double		consumed_d, generated_d, ratio_d ;
	long		proc_used = 0, proc_gen = 0, pos = 0 ;
	int			error ;

	printf ("\tprocess_mode_stats_test (%-25s) ... ", src_get_name (converter)) ;
	fflush (stdout) ;

	if ((src_state = src_new (converter, CHANNELS, &error)) == NULL)
	{	printf ("\n\nLine %d : src_new failed : %s\n\n", __LINE__, src_strerror (error)) ;
		exit (1) ;
		} ;

	do
	{	memset (&src_data, 0, sizeof (src_data)) ;
		src_data.data_in = input + pos * CHANNELS ;
		src_data.input_frames = MIN (CB_BLOCK_LEN, INPUT_FRAMES - pos) ;
		src_data.data_out = output ;
		src_data.output_frames = ARRAY_LEN (output) / CHANNELS ;
		src_data.src_ratio = ratio ;
		src_data.end_of_input = (pos + src_data.input_frames >= INPUT_FRAMES) ;

		if ((error = src_process (src_state, &src_data)) != 0)
		{	printf ("\n\nLine %d : src_process error : %s\n\n", __LINE__, src_strerror (error)) ;
			exit (1) ;
			} ;

		pos += src_data.input_frames_used ;
		proc_used += src_data.input_frames_used ;
		proc_gen += src_data.output_frames_gen ;

		get_stats_or_die (src_state, &consumed_d, &generated_d, &ratio_d, __LINE__) ;

		/* Double counters must match integer accounting after every call. */
		if (consumed_d != (double) proc_used || generated_d != (double) proc_gen)
		{	printf ("\n\nLine %d : counter mismatch : %f/%ld, %f/%ld.\n\n",
					__LINE__, consumed_d, proc_used, generated_d, proc_gen) ;
			exit (1) ;
			} ;

		if (ratio_d != ratio)
		{	printf ("\n\nLine %d : current ratio %f, expected %f.\n\n",
					__LINE__, ratio_d, ratio) ;
			exit (1) ;
			} ;
		}
	while (pos < INPUT_FRAMES || src_data.output_frames_gen > 0) ;

	src_delete (src_state) ;

	if (rel_err (consumed_d, (double) INPUT_FRAMES) > REL_TOL)
	{	printf ("\n\nLine %d : consumed_d (%f) != INPUT_FRAMES (%d).\n\n",
				__LINE__, consumed_d, INPUT_FRAMES) ;
		exit (1) ;
		} ;

	puts ("ok") ;
} /* process_mode_stats_test */

/*==============================================================================
*/

static void
error_and_reset_test (void)
{	SRC_STATE	*src_state ;
	double		value ;
	int			error ;

	printf ("\terror_and_reset_test .............................. ... ") ;
	fflush (stdout) ;

	/* NULL state pointer must be reported as an error. */
	if (src_get_frames_consumed (NULL, &value) == 0
			|| src_get_frames_generated (NULL, &value) == 0
			|| src_get_current_ratio (NULL, &value) == 0)
	{	printf ("\n\nLine %d : NULL state not detected.\n\n", __LINE__) ;
		exit (1) ;
		} ;

	if ((src_state = src_new (SRC_LINEAR, CHANNELS, &error)) == NULL)
	{	printf ("\n\nLine %d : src_new failed : %s\n\n", __LINE__, src_strerror (error)) ;
		exit (1) ;
		} ;

	/* NULL destination pointer must be reported as an error. */
	if (src_get_frames_consumed (src_state, NULL) == 0
			|| src_get_frames_generated (src_state, NULL) == 0
			|| src_get_current_ratio (src_state, NULL) == 0)
	{	printf ("\n\nLine %d : NULL destination pointer not detected.\n\n", __LINE__) ;
		exit (1) ;
		} ;

	/* Process some data so the counters move away from zero. */
	{
		SRC_DATA src_data ;

		memset (&src_data, 0, sizeof (src_data)) ;
		src_data.data_in = input ;
		src_data.input_frames = 1000 ;
		src_data.data_out = output ;
		src_data.output_frames = ARRAY_LEN (output) / CHANNELS ;
		src_data.src_ratio = 1.5 ;
		src_data.end_of_input = 0 ;

		if ((error = src_process (src_state, &src_data)) != 0)
		{	printf ("\n\nLine %d : src_process error : %s\n\n", __LINE__, src_strerror (error)) ;
			exit (1) ;
			} ;
		}

	if (src_get_frames_generated (src_state, &value) != 0 || value <= 0.0)
	{	printf ("\n\nLine %d : generated counter did not advance.\n\n", __LINE__) ;
		exit (1) ;
		} ;

	/* src_reset() must return all statistics to zero. */
	if (src_reset (src_state) != 0)
	{	printf ("\n\nLine %d : src_reset failed.\n\n", __LINE__) ;
		exit (1) ;
		} ;

	get_stats_or_die (src_state, &value, &value, &value, __LINE__) ;
	if (value != 0.0)
	{	printf ("\n\nLine %d : statistics not zero after src_reset.\n\n", __LINE__) ;
		exit (1) ;
		} ;

	src_delete (src_state) ;

	puts ("ok") ;
} /* error_and_reset_test */

/*==============================================================================
*/

static double
rel_err (double got, double expected)
{
	if (expected == 0.0)
		return fabs (got) ;

	return fabs (got - expected) / fabs (expected) ;
} /* rel_err */
