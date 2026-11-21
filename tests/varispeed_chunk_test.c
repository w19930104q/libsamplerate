/*
** Copyright (c) 2002-2016, Erik de Castro Lopo <erikd@mega-nerd.com>
** All rights reserved.
**
** This code is released under 2-clause BSD license. Please see the
** file at : https://github.com/libsndfile/libsamplerate/blob/master/COPYING
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#include <samplerate.h>

#include "util.h"

/*
** Regression test for varispeed conversion fed one input frame at a time.
**
** The public API couples input delivery and the ratio anchor in a single
** src_process call, so the only chunk-independent statement it can express
** is a fixed per-output-frame ratio trajectory: every call requests exactly
** one output frame and hard-sets its ratio with src_set_ratio(). Under that
** schedule the ratio applied to global output frame k depends only on k, so
** splitting the *input* buffer differently must not change the result.
**
** Each converter/channel/ratio scenario is converted three times, differing
** only in how input frames are queued:
**   mode 0: one input frame per call  (the reported failing pattern)
**   mode 1: pseudo-random input chunk sizes
**   mode 2: all input up front, then zero-input drain calls
**
** The three runs must consume all input frames, produce the same number of
** output frames (no early stop on the single-frame path) and emit
** bit-identical output PCM.
*/

#define	INPUT_FRAMES		400
#define	MAX_OUTPUT_FRAMES	(64 * INPUT_FRAMES + 128)
#define	MAX_CALLS			(200 * INPUT_FRAMES)

#define	CHUNK_FEED_ONE		0
#define	CHUNK_FEED_RANDOM	1
#define	CHUNK_FEED_ALL		2
#define	CHUNK_MODE_COUNT	3

typedef struct
{	long	frames_used ;
	long	frames_gen ;
	long	calls ;
	float	*output ;
} RunResult ;

typedef double (*ratio_fn_t) (long output_frame) ;

static double
ratio_triangle (long k)
{	/* Triangle wave between 0.35 and 4.0, crossing unity. */
	double lo = 0.35, hi = 4.0, period = 300.0 ;
	double phase = fmod ((double) k, 2.0 * period) / period ;
	double t = phase <= 1.0 ? phase : 2.0 - phase ;

	return lo + t * (hi - lo) ;
} /* ratio_triangle */

static double
ratio_sweep (long k)
{	/* Monotonic sweep from above unity down below unity. Crossing ratio 1.0
	** grows the SINC half-filter length while the end-of-input zero padding
	** stays fixed, which is what exposed the chunk-dependent termination.
	*/
	double hi = 16.0, lo = 1.0 / 16.0, span = 1500.0 ;
	double t = (double) k / span ;

	if (t > 1.0)
		t = 1.0 ;

	return hi + t * (lo - hi) ;
} /* ratio_sweep */

static double
ratio_alternate (long k)
{	/* Hard step between extreme ratios on every output frame. */
	return (k & 1) ? (1.0 / 64.0) : 64.0 ;
} /* ratio_alternate */

static double
ratio_constant (long k)
{	(void) k ;
	return 2.5 ;
} /* ratio_constant */

static void
fill_input (float *input, int channels, long frame_count)
{
	for (long frame = 0 ; frame < frame_count ; frame++)
		for (int ch = 0 ; ch < channels ; ch++)
		{
			double value = 0.6 * sin (2.0 * M_PI * 0.013 * frame + 0.7 * ch)
					+ 0.25 * sin (2.0 * M_PI * 0.047 * frame + 1.3 * ch) ;

			input [frame * channels + ch] = (float) value ;
			} ;
} /* fill_input */

static int
run_conversion (int converter, int channels, int feed_mode, ratio_fn_t ratio_at,
		const float *input, unsigned seed, RunResult *result)
{
	SRC_STATE *state ;
	SRC_DATA data ;
	int error ;
	long in_pos = 0, out_pos = 0, queued = 0 ;

	state = src_new (converter, channels, &error) ;
	if (state == NULL)
	{	printf ("\n\nLine %d : src_new() failed : %s\n\n", __LINE__, src_strerror (error)) ;
		exit (1) ;
		} ;

	result->output = calloc ((size_t) MAX_OUTPUT_FRAMES * channels, sizeof (float)) ;
	if (result->output == NULL)
	{	printf ("\n\nLine %d : calloc failed.\n\n", __LINE__) ;
		exit (1) ;
		} ;
	result->calls = 0 ;

	while (1)
	{
		if (result->calls > MAX_CALLS)
		{	printf ("\n\nLine %d : run did not terminate (mode %d).\n\n", __LINE__, feed_mode) ;
			exit (1) ;
			} ;

		/* Queue the next piece of input unless everything has been fed. */
		if (queued == 0 && in_pos < INPUT_FRAMES)
		{
			if (feed_mode == CHUNK_FEED_ONE)
				queued = 1 ;
			else if (feed_mode == CHUNK_FEED_RANDOM)
			{	seed = seed * 1103515245u + 12345u ;
				queued = 1 + (seed >> 16) % 31 ;
				queued = MIN (queued, INPUT_FRAMES - in_pos) ;
				}
			else
				queued = INPUT_FRAMES - in_pos ;
			} ;

		int end_of_input = (in_pos + queued >= INPUT_FRAMES) ;
		double ratio = ratio_at (out_pos) ;

		memset (&data, 0, sizeof (data)) ;
		data.data_in = queued ? input + in_pos * channels : NULL ;
		data.input_frames = queued ;
		data.data_out = result->output + out_pos * channels ;
		data.output_frames = 1 ;
		data.end_of_input = end_of_input ;
		data.src_ratio = ratio ;

		/* Hard ratio step: identical ratio for the same global output frame
		** in every feed mode, with no in-call smoothing dependence.
		*/
		if ((error = src_set_ratio (state, ratio)) != 0)
		{	printf ("\n\nLine %d : src_set_ratio() failed : %s\n\n", __LINE__, src_strerror (error)) ;
			exit (1) ;
			} ;

		if ((error = src_process (state, &data)) != 0)
		{	printf ("\n\nLine %d : src_process() failed : %s\n\n", __LINE__, src_strerror (error)) ;
			exit (1) ;
			} ;

		result->calls++ ;
		in_pos += data.input_frames_used ;
		queued -= data.input_frames_used ;
		out_pos += data.output_frames_gen ;

		if (end_of_input && data.output_frames_gen == 0)
			break ;

		if (out_pos >= MAX_OUTPUT_FRAMES)
		{	printf ("\n\nLine %d : output buffer exhausted (mode %d).\n\n", __LINE__, feed_mode) ;
			exit (1) ;
			} ;
		} ;

	src_delete (state) ;

	result->frames_used = in_pos ;
	result->frames_gen = out_pos ;

	return 0 ;
} /* run_conversion */

static void
check_scenario (int converter, const char *converter_name, int channels,
		const char *scenario_name, ratio_fn_t ratio_at, const float *input)
{
	RunResult results [CHUNK_MODE_COUNT] ;
	unsigned base_seed = (unsigned) (converter * 131 + channels * 17) ;

	printf ("\t%-12s ch=%d %-18s .......... ", converter_name, channels, scenario_name) ;
	fflush (stdout) ;

	for (int mode = 0 ; mode < CHUNK_MODE_COUNT ; mode++)
		run_conversion (converter, channels, mode, ratio_at, input,
				base_seed + (unsigned) mode, &results [mode]) ;

	long reference_gen = results [CHUNK_FEED_ONE].frames_gen ;

	ASSERT (reference_gen > 0) ;

	for (int mode = 0 ; mode < CHUNK_MODE_COUNT ; mode++)
	{
		ASSERT (results [mode].frames_used == INPUT_FRAMES) ;
		ASSERT (results [mode].frames_gen == reference_gen) ;

		for (long sample = 0 ; sample < reference_gen * channels ; sample++)
			if (results [mode].output [sample] != results [CHUNK_FEED_ONE].output [sample])
			{	printf ("\n  DIFF mode=%d sample=%ld frame=%ld: %.8g vs %.8g gens=%ld/%ld used=%ld/%ld\n",
					mode, sample, sample / channels,
					results [mode].output [sample], results [CHUNK_FEED_ONE].output [sample],
					results [mode].frames_gen, reference_gen,
					results [mode].frames_used, results [CHUNK_FEED_ONE].frames_used) ;
				exit (1) ;
				} ;
		} ;

	for (int mode = 0 ; mode < CHUNK_MODE_COUNT ; mode++)
		free (results [mode].output) ;

	puts ("ok") ;
} /* check_scenario */

static void
check_converter (int converter, const char *converter_name)
{
	static ratio_fn_t ratio_functions [] =
	{	ratio_triangle, ratio_sweep, ratio_alternate, ratio_constant
		} ;
	static const char *ratio_names [] =
	{	"triangle 0.35..4", "sweep 16..1/16", "alternate 64..1/64", "constant 2.5"
		} ;

	for (int channels = 1 ; channels <= 2 ; channels++)
	{
		float *input = calloc ((size_t) INPUT_FRAMES * channels, sizeof (float)) ;

		ASSERT (input != NULL) ;
		fill_input (input, channels, INPUT_FRAMES) ;

		for (int scenario = 0 ; scenario < ARRAY_LEN (ratio_functions) ; scenario++)
			check_scenario (converter, converter_name, channels,
					ratio_names [scenario], ratio_functions [scenario], input) ;

		free (input) ;
		} ;
} /* check_converter */

int
main (void)
{
	puts ("") ;

	check_converter (SRC_ZERO_ORDER_HOLD, "ZOH") ;
	check_converter (SRC_LINEAR, "LINEAR") ;

#ifdef ENABLE_SINC_FAST_CONVERTER
	check_converter (SRC_SINC_FASTEST, "SINC_FASTEST") ;
#endif

#ifdef ENABLE_SINC_MEDIUM_CONVERTER
	check_converter (SRC_SINC_MEDIUM_QUALITY, "SINC_MEDIUM") ;
#endif

#ifdef ENABLE_SINC_BEST_CONVERTER
	check_converter (SRC_SINC_BEST_QUALITY, "SINC_BEST") ;
#endif

	puts ("") ;
	return 0 ;
} /* main */
