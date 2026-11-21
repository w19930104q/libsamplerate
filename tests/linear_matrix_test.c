/*
** Copyright (c) 2002-2021, Erik de Castro Lopo <erikd@mega-nerd.com>
** All rights reserved.
**
** This code is released under 2-clause BSD license. Please see the
** file at : https://github.com/libsndfile/libsamplerate/blob/master/COPYING
*/

/*
** Exhaustive small-input matrix test for the linear interpolator :
** channels 1-8 x input frames 1-8 x a grid of conversion ratios
** (including ratios < 1, > 1 and irrational ones).
**
** Every output sample is compared against a double precision reference
** implementation of the same algorithm. This test is primarily meant to
** be run under a memory error detector (AddressSanitizer, valgrind) :
** previous versions read before the start of the input buffer for
** single frame input.
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

#define MAX_CHANNELS	8
#define MAX_FRAMES		8
#define MAX_OUT_FRAMES	(MAX_FRAMES * 256 + 64)

#define ABS_TOLERANCE	1e-3

static double
fmod_one (double x)
{	double res = x - lrint (x) ;
	return (res < 0.0) ? res + 1.0 : res ;
} /* fmod_one */

/* Double precision reference mirroring the library's linear algorithm. */
static long
linear_reference (const float *data_in, long input_frames, int channels,
		double src_ratio, double *data_out, long output_frames)
{	double	last_value [MAX_CHANNELS] ;
	double	input_index = 0.0, rem ;
	long	in_count = input_frames * channels ;
	long	out_count = output_frames * channels ;
	long	in_used = 0, out_gen = 0 ;
	int		ch ;

	for (ch = 0 ; ch < channels ; ch++)
		last_value [ch] = data_in [ch] ;

	/* Samples before the first sample of the input array. */
	while (input_index < 1.0 && out_gen < out_count)
	{	for (ch = 0 ; ch < channels ; ch++)
			data_out [out_gen ++] = last_value [ch]
					+ input_index * ((double) data_in [ch] - last_value [ch]) ;
		input_index += 1.0 / src_ratio ;
		} ;

	rem = fmod_one (input_index) ;
	in_used += channels * lrint (input_index - rem) ;
	input_index = rem ;

	/* Main processing loop. */
	while (out_gen < out_count && in_used >= channels
			&& in_used + channels * input_index < in_count)
	{	for (ch = 0 ; ch < channels ; ch++)
			data_out [out_gen ++] = data_in [in_used - channels + ch]
					+ input_index * ((double) data_in [in_used + ch]
							- data_in [in_used - channels + ch]) ;

		input_index += 1.0 / src_ratio ;
		rem = fmod_one (input_index) ;
		in_used += channels * lrint (input_index - rem) ;
		input_index = rem ;
		} ;

	return out_gen / channels ;
} /* linear_reference */

static void
linear_matrix_test (int channels, long input_frames, double src_ratio)
{	static float input [MAX_CHANNELS * MAX_FRAMES] ;
	static float output [MAX_CHANNELS * MAX_OUT_FRAMES] ;
	static double reference [MAX_CHANNELS * MAX_OUT_FRAMES] ;

	SRC_DATA	src_data ;
	long		output_frames, ref_frames, k ;
	int			error ;

	output_frames = (long) (input_frames * src_ratio) + 16 ;
	ASSERT (output_frames <= MAX_OUT_FRAMES) ;

	for (k = 0 ; k < input_frames * channels ; k++)
		input [k] = (float) (0.9 * sin (k * 0.7 + channels)) ;

	memset (&src_data, 0, sizeof (src_data)) ;
	src_data.data_in = input ;
	src_data.data_out = output ;
	src_data.input_frames = input_frames ;
	src_data.output_frames = output_frames ;
	src_data.src_ratio = src_ratio ;
	src_data.end_of_input = 1 ;

	if ((error = src_simple (&src_data, SRC_LINEAR, channels)))
	{	printf ("\n\nLine %d : src_simple (channels %d, frames %ld, ratio %g) failed : %s\n\n",
				__LINE__, channels, input_frames, src_ratio, src_strerror (error)) ;
		exit (1) ;
		} ;

	ref_frames = linear_reference (input, input_frames, channels, src_ratio,
				reference, output_frames) ;

	if (src_data.output_frames_gen != ref_frames)
	{	printf ("\n\nLine %d : channels %d, frames %ld, ratio %.17g : "
				"output_frames_gen %ld != reference %ld.\n\n",
				__LINE__, channels, input_frames, src_ratio,
				src_data.output_frames_gen, ref_frames) ;
		exit (1) ;
		} ;

	for (k = 0 ; k < ref_frames * channels ; k++)
		if (fabs (output [k] - reference [k]) > ABS_TOLERANCE)
		{	printf ("\n\nLine %d : channels %d, frames %ld, ratio %.17g, sample %ld : "
					"output %.9g != reference %.9g.\n\n",
					__LINE__, channels, input_frames, src_ratio, k,
					(double) output [k], reference [k]) ;
			exit (1) ;
			} ;
} /* linear_matrix_test */

int
main (void)
{	static const double ratios [] =
	{	1.0 / 256.0, 1.0 / 255.3, 0.05, 0.25, 1.0 / 3.0, 0.5, 0.75, 0.9, 0.99,
		1.0, 1.01, 1.1, 1.4142135623730951, 2.0, 2.718281828459045,
		3.141592653589793, 10.0, 100.0, 255.9, 256.0
	} ;

	int		channels, k ;
	long	frames ;

	puts ("") ;
	puts ("    Linear interpolator small input matrix test :") ;

	for (channels = 1 ; channels <= MAX_CHANNELS ; channels++)
	for (frames = 1 ; frames <= MAX_FRAMES ; frames++)
	for (k = 0 ; k < ARRAY_LEN (ratios) ; k++)
		linear_matrix_test (channels, frames, ratios [k]) ;

	printf ("\tok (%d channel counts x %d frame counts x %d ratios)\n",
			MAX_CHANNELS, MAX_FRAMES, ARRAY_LEN (ratios)) ;
	puts ("") ;

	return 0 ;
} /* main */
