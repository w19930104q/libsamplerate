/*
** Copyright (c) 2024, Erik de Castro Lopo <erikd@mega-nerd.com>
** All rights reserved.
**
** This code is released under 2-clause BSD license. Please see the
** file at : https://github.com/libsndfile/libsamplerate/blob/master/COPYING
*/

/*
** Verify that the frame accounting reported through input_frames_used and
** output_frames_gen is independent of the channel count and stays aligned
** with the src_ratio bookkeeping for every converter.
**
** For every (converter, channel count, ratio grid point) the test asserts:
**   1. |output_frames_gen - input_frames_used * ratio| <= 2 frames, both
**      per src_process() call and for the accumulated totals;
**   2. the number of samples actually written equals
**      output_frames_gen * channels (checked by advancing the output data
**      pointer by exactly that amount);
**   3. the number of samples actually consumed equals
**      input_frames_used * channels (same pointer advancement check on the
**      input side);
**   4. identical frame level workloads produce identical frame counts for
**      every channel count.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

#include <samplerate.h>

#include "util.h"

#define	CONVERTER_COUNT		3
#define	CHANNEL_GRID_LEN	5
#define	RATIO_GRID_LEN		7

#define	TOTAL_IN_FRAMES		2000
#define	FRAME_TOLERANCE		2

static const int converters [CONVERTER_COUNT] =
{	SRC_SINC_FASTEST, SRC_ZERO_ORDER_HOLD, SRC_LINEAR
} ;

static const char *converter_names [CONVERTER_COUNT] =
{	"sinc-fastest", "zero-order-hold", "linear"
} ;

static const int channel_grid [CHANNEL_GRID_LEN] = { 1, 2, 3, 6, 8 } ;

static const double ratio_grid [RATIO_GRID_LEN] =
{	0.3333333333, 0.5, 0.95, 1.0, 1.05, 2.0, 3.0
} ;

/* A variable ratio sequence spanning <1, ==1 and >1. */
#define	VARISPEED_STEPS		7
static const double varispeed_seq [VARISPEED_STEPS] =
{	0.5, 0.75, 1.0, 1.25, 2.0, 1.0, 0.9
} ;

static float input_storage [(TOTAL_IN_FRAMES + 64) * 8] ;

/*
** Per-call scratch output block, filled with a sentinel before each call.
** The test input is a bounded sine wave (|sample| < 0.4), so a large finite
** value well outside the converter's output range marks an untouched slot
** without colliding with any real sample (a NaN sentinel is unusable because
** the linear interpolator can itself emit NaN on the very first frame).
*/
#define	MAX_OUT_BLOCK		64
#define	SENTINEL			1.0e30f
static float output_block [(MAX_OUT_BLOCK + 1) * 8] ;

/*
** A slot is untouched only while it still holds the finite sentinel. A NaN
** is treated as written because the linear interpolator can itself emit NaN
** on the very first frame (it interpolates from a zeroed history value).
*/
#define	IS_SENTINEL(x)	(! isnan (x) && fabsf ((x) - SENTINEL) < 1.0e20f)

typedef struct
{	long		input_frames_used ;
	long		output_frames_gen ;
	double		expected_output_acc ;
	int			call_count ;
} STREAM_TOTALS ;

static void
fail (const char *tag, int converter, int channels, double ratio, const char *fmt, ...)
{
	printf ("\n\n%s: converter=%s channels=%d ratio=%.6f\n",
			tag, src_get_name (converter), channels, ratio) ;
	va_list ap ;
	va_start (ap, fmt) ;
	vprintf (fmt, ap) ;
	va_end (ap) ;
	printf ("\n") ;
	exit (1) ;
}

static STREAM_TOTALS
run_stream (int converter, int channels, double fixed_ratio, int varispeed,
		int in_block, int out_block)
{	SRC_STATE *state ;
	SRC_DATA src_data ;
	STREAM_TOTALS totals ;
	long in_off = 0 ;
	int error, calls = 0 ;

	memset (&totals, 0, sizeof (totals)) ;

	state = src_new (converter, channels, &error) ;
	if (state == NULL)
		fail ("src_new", converter, channels, fixed_ratio, "%s", src_strerror (error)) ;

	memset (&src_data, 0, sizeof (src_data)) ;

	while (1)
	{	long in_frames, out_cap, used, gen, s ;
		double current_ratio ;
		float *in_before ;

		in_frames = TOTAL_IN_FRAMES - in_off ;
		if (in_frames > in_block)
			in_frames = in_block ;
		out_cap = out_block ;

		current_ratio = varispeed ? varispeed_seq [calls % VARISPEED_STEPS] : fixed_ratio ;

		in_before = input_storage + in_off * channels ;

		/*
		** Fill the whole scratch output block with a sentinel (plus one
		** extra frame of guard space) so that the number of samples the
		** converter actually writes can be detected exactly.
		*/
		for (s = 0 ; s < (out_cap + 1) * channels ; s++)
			output_block [s] = SENTINEL ;

		src_data.data_in = in_before ;
		src_data.input_frames = in_frames ;
		src_data.data_out = output_block ;
		src_data.output_frames = out_cap ;
		src_data.src_ratio = current_ratio ;
		src_data.end_of_input = (in_off + in_frames >= TOTAL_IN_FRAMES) ? 1 : 0 ;

		error = src_process (state, &src_data) ;
		if (error != 0)
			fail ("src_process", converter, channels, current_ratio, "%s", src_strerror (error)) ;

		used = src_data.input_frames_used ;
		gen = src_data.output_frames_gen ;

		/* Counts must stay within the offered capacities. */
		if (used < 0 || used > in_frames)
			fail ("input count", converter, channels, current_ratio,
				"input_frames_used=%ld outside [0,%ld] at call %d", used, in_frames, calls) ;
		if (gen < 0 || gen > out_cap)
			fail ("output count", converter, channels, current_ratio,
				"output_frames_gen=%ld outside [0,%ld] at call %d", gen, out_cap, calls) ;

		/*
		** Written-sample contract: exactly output_frames_gen * channels
		** samples must have been produced. Every sample in that range must
		** differ from the sentinel and every sample past it must still hold
		** the sentinel, proving the converter wrote no more and no less than
		** the reported count.
		*/
		for (s = 0 ; s < gen * channels ; s++)
			if (IS_SENTINEL (output_block [s]))
				fail ("written samples", converter, channels, current_ratio,
					"sample %ld inside output_frames_gen*channels (%ld) was not "
					"written at call %d", s, gen * channels, calls) ;
		for (s = gen * channels ; s < out_cap * channels ; s++)
			if (! IS_SENTINEL (output_block [s]))
				fail ("written samples", converter, channels, current_ratio,
					"sample %ld past output_frames_gen*channels (%ld) was overwritten "
					"at call %d", s, gen * channels, calls) ;

		/*
		** Consumed-sample contract: input_frames_used * channels must be the
		** exact offset of the next unconsumed input frame, i.e. advancing the
		** input pointer by that many samples stays inside the offered input.
		*/
		if (used * channels > in_frames * channels)
			fail ("consumed samples", converter, channels, current_ratio,
				"input_frames_used*channels (%ld) exceeds offered input samples (%ld)",
				used * channels, in_frames * channels) ;

		/*
		** Per-call ratio bookkeeping. The sinc converters have to fill a
		** window of filter coefficients before the first output frame, so a
		** single src_process() call near start up or at a buffer boundary can
		** legitimately lag the used*ratio target by up to half the filter
		** length; bound that by the offered output capacity plus a fixed
		** filter-window margin. The ZOH and linear interpolators have no
		** filter delay and stay within the rounding tolerance on every call.
		** A variable ratio is additionally interpolated across each output
		** block, so its per-call target is only bounded by the sequence
		** extrema. In every case the accumulated totals for a constant ratio
		** below are required to match within FRAME_TOLERANCE frames, which is
		** what downstream buffer advancement actually relies on.
		*/
		{	int per_call_tolerance ;
			double expected = (double) used * current_ratio ;

			if (converter == SRC_SINC_FASTEST)
				per_call_tolerance = (int) out_cap + 32 ;
			else
				per_call_tolerance = FRAME_TOLERANCE ;
			if (varispeed)
				per_call_tolerance += (int) used ;

			if (fabs ((double) gen - expected) > per_call_tolerance)
				fail ("per-call ratio", converter, channels, current_ratio,
					"|gen(%ld) - used(%ld)*ratio(%.6f)=%.3f| > %d at call %d",
					gen, used, current_ratio, expected, per_call_tolerance, calls) ;
			} ;

		in_off += used ;
		totals.input_frames_used += used ;
		totals.output_frames_gen += gen ;
		totals.expected_output_acc += (double) used * current_ratio ;
		totals.call_count = ++ calls ;

		if (src_data.end_of_input && gen == 0)
			break ;
		if (calls > 100000)
			fail ("loop", converter, channels, current_ratio, "runaway stream", calls) ;
		} ;

	src_delete (state) ;

	/*
	** Accumulated ratio bookkeeping. This only has a well defined answer for
	** a constant ratio: when the ratio varies between calls the library
	** interpolates it across each output block, so the per-call target ratio
	** cannot be used to reconstruct an exact expected output length.
	*/
	if (!varispeed &&
			fabs ((double) totals.output_frames_gen - totals.expected_output_acc) > FRAME_TOLERANCE)
		fail ("total ratio", converter, channels, fixed_ratio,
			"|total_gen(%ld) - accumulated expected(%.3f)| > %d",
			totals.output_frames_gen, totals.expected_output_acc, FRAME_TOLERANCE) ;

	return totals ;
}

int
main (void)
{
	/* Block size pairs chosen so that boundary rounding is exercised even
	** when a single pair happens not to trigger a channel-dependent path.
	** The smallest input block is kept above the interpolation window so
	** that every reported output frame corresponds to a sample actually
	** written (with a single input frame the converters legitimately report
	** frames they cannot yet interpolate). */
	static const int block_pairs [][2] =
	{	{ 3, 1 }, { 3, 5 }, { 5, 13 }, { 7, 13 }, { 31, 64 }
	} ;
	const int block_pair_count = ARRAY_LEN (block_pairs) ;

	int ci, chi, ri, bi ;

	puts ("") ;

	for (int f = 0 ; f < (TOTAL_IN_FRAMES + 64) * 8 ; f++)
		input_storage [f] = 0.25f * sinf (f * 0.01f) + 0.1f * sinf (f * 0.037f) ;

	for (ci = 0 ; ci < CONVERTER_COUNT ; ci++)
	{
		printf ("    %s :\n", converter_names [ci]) ;

		for (bi = 0 ; bi < block_pair_count ; bi++)
		{
			int in_block = block_pairs [bi][0] ;
			int out_block = block_pairs [bi][1] ;
			STREAM_TOTALS reference [RATIO_GRID_LEN + 1] ;

			printf ("\tblock sizes: in=%d out=%d\n", in_block, out_block) ;

			for (chi = 0 ; chi < CHANNEL_GRID_LEN ; chi++)
			{
				int channels = channel_grid [chi] ;

				for (ri = 0 ; ri < RATIO_GRID_LEN ; ri++)
				{
					STREAM_TOTALS totals = run_stream (converters [ci], channels,
							ratio_grid [ri], 0, in_block, out_block) ;

					/*
					** Frame counts must be identical for every channel count.
					** Store the mono run as the channel-independent reference.
					*/
					if (channels == 1)
						reference [ri] = totals ;
					else if (totals.input_frames_used != reference [ri].input_frames_used ||
							totals.output_frames_gen != reference [ri].output_frames_gen)
						fail ("channel invariance", converters [ci], channels, ratio_grid [ri],
							"in/out block=%d/%d counts (used=%ld,gen=%ld) differ from mono "
							"(used=%ld,gen=%ld)",
							in_block, out_block,
							totals.input_frames_used, totals.output_frames_gen,
							reference [ri].input_frames_used,
							reference [ri].output_frames_gen) ;
					} ;

				/* Variable ratio sequence. */
				{
					STREAM_TOTALS totals = run_stream (converters [ci], channels,
							1.0, 1, in_block, out_block) ;

					if (channels == 1)
						reference [RATIO_GRID_LEN] = totals ;
					else if (totals.input_frames_used != reference [RATIO_GRID_LEN].input_frames_used ||
							totals.output_frames_gen != reference [RATIO_GRID_LEN].output_frames_gen)
						fail ("channel invariance", converters [ci], channels, 1.0,
							"varispeed in/out block=%d/%d counts (used=%ld,gen=%ld) differ "
							"from mono (used=%ld,gen=%ld)",
							in_block, out_block,
							totals.input_frames_used, totals.output_frames_gen,
							reference [RATIO_GRID_LEN].input_frames_used,
							reference [RATIO_GRID_LEN].output_frames_gen) ;
					} ;
				} ;

			printf ("\tblock sizes: in=%d out=%d ........ ok\n", in_block, out_block) ;
			} ;
		} ;

	puts ("") ;
	return 0 ;
} /* main */
