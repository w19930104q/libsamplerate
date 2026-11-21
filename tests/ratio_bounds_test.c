/*
** Copyright (c) 2002-2021, Erik de Castro Lopo <erikd@mega-nerd.com>
** All rights reserved.
**
** This code is released under 2-clause BSD license. Please see the
** file at : https://github.com/libsndfile/libsamplerate/blob/master/COPYING
*/

/*
** Test that invalid conversion ratios (NaN, Inf, zero, out of range) are
** rejected by src_is_valid_ratio () and cause src_simple () to fail with
** SRC_ERR_BAD_SRC_RATIO instead of crashing or corrupting memory.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

#include <samplerate.h>

#include "util.h"

#define INPUT_FRAMES	64
#define OUTPUT_FRAMES	20000
#define CHANNELS		2

static void is_valid_ratio_test (void) ;
static void src_simple_ratio_test (void) ;
static void src_set_ratio_test (void) ;

static const struct
{	const char	*name ;
	double		ratio ;
	int			expect_valid ;
} ratio_tests [] =
{	{ "NaN",			NAN,							0 },
	{ "+Inf",			INFINITY,						0 },
	{ "-Inf",			-INFINITY,						0 },
	{ "+0.0",			0.0,							0 },
	{ "-0.0",			-0.0,							0 },
	{ "denormal",		4.9406564584124654e-324,		0 },
	{ "tiny",			1e-300,							0 },
	{ "below 1/256",	1.0 / 256.0 - 1e-12,			0 },
	{ "exactly 1/256",	1.0 / 256.0,					1 },
	{ "above 1/256",	1.0 / 256.0 + 1e-12,			1 },
	{ "1.0",			1.0,							1 },
	{ "below 256",		256.0 - 1e-9,					1 },
	{ "exactly 256",	256.0,							1 },
	{ "above 256",		256.0 + 1e-9,					0 },
	{ "-1.0",			-1.0,							0 },
	{ "huge",			1e300,							0 },
	{ "-huge",			-1e300,							0 },
	{ "DBL_MAX",		DBL_MAX,						0 },
} ;

int
main (void)
{
	puts ("") ;

	is_valid_ratio_test () ;
	src_simple_ratio_test () ;
	src_set_ratio_test () ;

	puts ("") ;

	return 0 ;
} /* main */

static int
expect_bad_ratio_error (int error)
{	const char *str ;

	if (error == 0)
		return 0 ;

	str = src_strerror (error) ;

	return str != NULL && strstr (str, "ratio outside") != NULL ;
} /* expect_bad_ratio_error */

static void
is_valid_ratio_test (void)
{	int k ;

	puts ("    src_is_valid_ratio test :") ;

	for (k = 0 ; k < ARRAY_LEN (ratio_tests) ; k++)
	{	int valid = src_is_valid_ratio (ratio_tests [k].ratio) ;

		printf ("\t%-16s (% -24g) : %s\n", ratio_tests [k].name, ratio_tests [k].ratio,
				valid ? "valid" : "invalid") ;

		if (!! valid != ratio_tests [k].expect_valid)
		{	printf ("\n\nLine %d : src_is_valid_ratio (%s) returned %d, expected %d.\n\n",
					__LINE__, ratio_tests [k].name, valid, ratio_tests [k].expect_valid) ;
			exit (1) ;
			} ;
		} ;
} /* is_valid_ratio_test */

static void
src_simple_ratio_test (void)
{	static const int converters [] =
	{	SRC_SINC_BEST_QUALITY, SRC_SINC_MEDIUM_QUALITY, SRC_SINC_FASTEST,
		SRC_ZERO_ORDER_HOLD, SRC_LINEAR
	} ;

	float *input, *output ;
	int k, c ;

	puts ("    src_simple bad ratio test :") ;

	input = malloc (INPUT_FRAMES * CHANNELS * sizeof (float)) ;
	output = malloc (OUTPUT_FRAMES * CHANNELS * sizeof (float)) ;
	ASSERT (input != NULL && output != NULL) ;

	for (k = 0 ; k < INPUT_FRAMES * CHANNELS ; k++)
		input [k] = 0.01f * k ;

	for (k = 0 ; k < ARRAY_LEN (ratio_tests) ; k++)
	for (c = 0 ; c < ARRAY_LEN (converters) ; c++)
	{	SRC_DATA src_data ;
		int error ;

		memset (&src_data, 0, sizeof (src_data)) ;
		src_data.data_in = input ;
		src_data.data_out = output ;
		src_data.input_frames = INPUT_FRAMES ;
		src_data.output_frames = OUTPUT_FRAMES ;
		src_data.src_ratio = ratio_tests [k].ratio ;
		src_data.end_of_input = 1 ;

		error = src_simple (&src_data, converters [c], CHANNELS) ;

		if (ratio_tests [k].expect_valid)
		{	if (error != 0)
			{	printf ("\n\nLine %d : src_simple (%s, converter %d) failed : %s\n\n",
						__LINE__, ratio_tests [k].name, converters [c], src_strerror (error)) ;
				exit (1) ;
				} ;
			}
		else if (! expect_bad_ratio_error (error))
		{	printf ("\n\nLine %d : src_simple (%s, converter %d) returned %d (%s), "
					"expected SRC_ERR_BAD_SRC_RATIO.\n\n",
					__LINE__, ratio_tests [k].name, converters [c], error, src_strerror (error)) ;
			exit (1) ;
			} ;
		} ;

	free (input) ;
	free (output) ;

	puts ("\tok") ;
} /* src_simple_ratio_test */

static void
src_set_ratio_test (void)
{	SRC_STATE *state ;
	int k, error ;

	puts ("    src_set_ratio bad ratio test :") ;

	state = src_new (SRC_LINEAR, 1, &error) ;
	ASSERT (state != NULL) ;

	for (k = 0 ; k < ARRAY_LEN (ratio_tests) ; k++)
	{	error = src_set_ratio (state, ratio_tests [k].ratio) ;

		if (ratio_tests [k].expect_valid)
		{	if (error != 0)
			{	printf ("\n\nLine %d : src_set_ratio (%s) failed : %s\n\n",
						__LINE__, ratio_tests [k].name, src_strerror (error)) ;
				exit (1) ;
				} ;
			}
		else if (! expect_bad_ratio_error (error))
		{	printf ("\n\nLine %d : src_set_ratio (%s) returned %d (%s), "
					"expected SRC_ERR_BAD_SRC_RATIO.\n\n",
					__LINE__, ratio_tests [k].name, error, src_strerror (error)) ;
			exit (1) ;
			} ;
		} ;

	src_delete (state) ;

	puts ("\tok") ;
} /* src_set_ratio_test */
