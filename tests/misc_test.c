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
#include <string.h>

#include <samplerate.h>

#include "util.h"
#include "common.h"

static void name_test (void) ;
static void error_test (void) ;
static void src_ratio_test (void) ;
static void zero_input_test (int converter) ;
static void get_channels_test (int converter);
static void bad_channel_count_test (int converter) ;
static void overlap_test (int converter) ;

int
main (void)
{
	puts ("") ;

	printf ("    version : %s\n\n", src_get_version ()) ;

	/* Current max converter is SRC_LINEAR. */
	name_test () ;

	error_test () ;

	src_ratio_test () ;

	zero_input_test (SRC_ZERO_ORDER_HOLD) ;
	zero_input_test (SRC_LINEAR) ;
#ifdef ENABLE_SINC_FAST_CONVERTER
	zero_input_test (SRC_SINC_FASTEST) ;
#endif
	get_channels_test (SRC_ZERO_ORDER_HOLD) ;
	get_channels_test (SRC_LINEAR) ;
#ifdef ENABLE_SINC_FAST_CONVERTER
	get_channels_test (SRC_SINC_FASTEST) ;
#endif

	bad_channel_count_test (SRC_ZERO_ORDER_HOLD) ;
	bad_channel_count_test (SRC_LINEAR) ;
#ifdef ENABLE_SINC_FAST_CONVERTER
	bad_channel_count_test (SRC_SINC_FASTEST) ;
#endif

	overlap_test (SRC_ZERO_ORDER_HOLD) ;
	overlap_test (SRC_LINEAR) ;
#ifdef ENABLE_SINC_FAST_CONVERTER
	overlap_test (SRC_SINC_FASTEST) ;
#endif
	puts ("") ;
	return 0 ;
} /* main */

static void
name_test (void)
{	const char	*name ;
	int	k = 0 ;

	puts ("    name_test :") ;

	while (1)
	{	name = src_get_name (k) ;
		if (name == NULL)
			break ;
		printf ("\tName %d : %s\n", k, name) ;
		printf ("\tDesc %d : %s\n", k, src_get_description (k)) ;
		k ++ ;
		} ;

	puts ("") ;

	return ;
} /* name_test */

/*------------------------------------------------------------------------------
*/

typedef struct
{	double	ratio ;
	int		should_pass ;
} RATIO_TEST ;

static RATIO_TEST ratio_test [] =
{	{	1.0 / 256.1,	0 },
	{	1.0 / 256.0,	1 },
	{	1.0,			1 },
	{	256.0, 			1 },
	{	256.1,			0 },
	{	-1.0,			0 }
} ;

static void
src_ratio_test (void)
{	int k ;

	puts ("    src_ratio_test (SRC ratio must be in range [1/256, 256]):" ) ;


	for (k = 0 ; k < ARRAY_LEN (ratio_test) ; k++)
	{	if (ratio_test [k].should_pass && src_is_valid_ratio (ratio_test [k].ratio) == 0)
		{	printf ("\n\nLine %d : SRC ratio %f should have passed.\n\n", __LINE__, ratio_test [k].ratio) ;
			exit (1) ;
			} ;
		if (! ratio_test [k].should_pass && src_is_valid_ratio (ratio_test [k].ratio) != 0)
		{	printf ("\n\nLine %d : SRC ratio %f should not have passed.\n\n", __LINE__, ratio_test [k].ratio) ;
			exit (1) ;
			} ;
		printf ("\t SRC ratio (%9.5f) : %s ................... ok\n", ratio_test [k].ratio,
			(ratio_test [k].should_pass ? "pass" : "fail")) ;
		} ;

	puts ("") ;

	return ;
} /* src_ratio_test */

static void
error_test (void)
{	const char *errorstr ;
	int		k, errors = 0 ;

	puts ("    error_test :") ;

	for (k = 0 ; 1 ; k++)
	{	errorstr = src_strerror (k) ;
		printf ("\t%-2d : %s\n", k, errorstr) ;
		if (errorstr == NULL)
		{	errors ++ ;
			continue ;
			} ;
		if (strstr (errorstr, "Placeholder.") == errorstr)
			break ;
		} ;

	if (errors != 0)
	{	printf ("\n\nLine %d : Missing error numbers above.\n\n", __LINE__) ;
		exit (1) ;
		} ;

	puts ("") ;

	return ;
} /* error_test */

static void
zero_input_test (int converter)
{	SRC_DATA data ;
	SRC_STATE *state ;
	float out [100] ;
	int error ;

	printf ("    %s (%-26s) ........ ", __func__, src_get_name (converter)) ;
	fflush (stdout) ;

	if ((state = src_new (converter, 1, &error)) == NULL)
	{	printf ("\n\nLine %d : src_new failed : %s.\n\n", __LINE__, src_strerror (error)) ;
		exit (1) ;
		} ;

	data.data_in = (float *) (size_t) 0xdeadbeef ;
	data.input_frames = 0 ;
	data.data_out = out ;
	data.output_frames = ARRAY_LEN (out) ;
	data.end_of_input = 0 ;
	data.src_ratio = 1.0 ;

	if ((error = src_process (state, &data)))
	{	printf ("\n\nLine %d : src_new failed : %s.\n\n", __LINE__, src_strerror (error)) ;
		exit (1) ;
		} ;

	state = src_delete (state) ;

	puts ("ok") ;
} /* zero_input_test */

static void get_channels_test(int converter)
{
	SRC_STATE *state;
	int channels_in, channels_out;
	const char *errorstr;

	state = NULL;
	if ((channels_out = src_get_channels(state)) >= 0)
	{
		printf ("\n\nLine %d : Return value should be negative, was : %d.\n\n", __LINE__, channels_out) ;
		exit (1) ;
	}
	errorstr = src_strerror(-channels_out);
	if (!strstr(errorstr, "NULL"))
	{
		printf ("\n\nLine %d : Inverted output should be valid error code mentioning a NULL pointer, was : %s.\n\n", __LINE__, errorstr) ;
		exit (1) ;
	}

	for (channels_in = 1; channels_in <= 8; channels_in++)
	{
		int error;
		state = src_new(converter, channels_in, &error);
		if ((channels_out = src_get_channels(state)) != channels_in)
		{
			printf ("\n\nLine %d : Return value should be %d, was : %d.\n\n", __LINE__, channels_in, channels_out) ;
			exit (1) ;
		}
		state = src_delete(state);
	}
}

static void
bad_channel_count_test (int converter)
{	SRC_STATE *state ;
	SRC_DATA data ;
	float input [10], output [10] ;
	int error, k ;
	static const int bad_channels [] = { 0, -1, -100, MAX_CHANNELS + 1, MAX_CHANNELS + 1000 } ;

	printf ("    %s (%-26s) ........ ", __func__, src_get_name (converter)) ;
	fflush (stdout) ;

	for (k = 0 ; k < ARRAY_LEN (bad_channels) ; k++)
	{	/* src_new must fail with an error code, not an assert or UB. */
		error = SRC_ERR_NO_ERROR ;
		if ((state = src_new (converter, bad_channels [k], &error)) != NULL)
		{	printf ("\n\nLine %d : src_new (channels = %d) should have failed.\n\n", __LINE__, bad_channels [k]) ;
			exit (1) ;
			} ;
		if (error != SRC_ERR_BAD_CHANNEL_COUNT)
		{	printf ("\n\nLine %d : expected SRC_ERR_BAD_CHANNEL_COUNT (%d), got %d.\n\n",
					__LINE__, SRC_ERR_BAD_CHANNEL_COUNT, error) ;
			exit (1) ;
			} ;

		/* Same via the simple interface. */
		memset (&data, 0, sizeof (data)) ;
		data.data_in = input ;
		data.input_frames = ARRAY_LEN (input) ;
		data.data_out = output ;
		data.output_frames = ARRAY_LEN (output) ;
		data.src_ratio = 1.0 ;

		if ((error = src_simple (&data, converter, bad_channels [k])) != SRC_ERR_BAD_CHANNEL_COUNT)
		{	printf ("\n\nLine %d : src_simple (channels = %d) should return SRC_ERR_BAD_CHANNEL_COUNT (%d), got %d.\n\n",
					__LINE__, bad_channels [k], SRC_ERR_BAD_CHANNEL_COUNT, error) ;
			exit (1) ;
			} ;
		} ;

	puts ("ok") ;
} /* bad_channel_count_test */

#define OVERLAP_TEST_LEN	256

static void
overlap_test (int converter)
{	SRC_DATA data ;
	SRC_STATE *state ;
	float input [OVERLAP_TEST_LEN], baseline [OVERLAP_TEST_LEN] ;
	/* Input and output buffers placed back to back in a single array. */
	float pair [2 * OVERLAP_TEST_LEN] ;
	long output_frames_gen ;
	int error, k ;

	printf ("    %s (%-26s) ........ ", __func__, src_get_name (converter)) ;
	fflush (stdout) ;

	for (k = 0 ; k < ARRAY_LEN (input) ; k++)
		input [k] = (float) (0.8 * ((k % 31) / 31.0 - 0.5)) ;

	/*----------------------------------------------------------------------
	** Baseline : fully separate input and output buffers must work.
	*/
	memset (&data, 0, sizeof (data)) ;
	data.data_in = input ;
	data.input_frames = ARRAY_LEN (input) ;
	data.data_out = baseline ;
	data.output_frames = ARRAY_LEN (baseline) ;
	data.end_of_input = 1 ;
	data.src_ratio = 1.0 ;

	if ((state = src_new (converter, 1, &error)) == NULL)
	{	printf ("\n\nLine %d : src_new failed : %s.\n\n", __LINE__, src_strerror (error)) ;
		exit (1) ;
		} ;

	if ((error = src_process (state, &data)) != SRC_ERR_NO_ERROR)
	{	printf ("\n\nLine %d : src_process on separate buffers failed : %s.\n\n", __LINE__, src_strerror (error)) ;
		exit (1) ;
		} ;

	output_frames_gen = data.output_frames_gen ;
	if (output_frames_gen <= 0)
	{	printf ("\n\nLine %d : expected output frames, got %ld.\n\n", __LINE__, output_frames_gen) ;
		exit (1) ;
		} ;

	state = src_delete (state) ;

	/*----------------------------------------------------------------------
	** Adjacent buffers : the output buffer starts exactly where the input
	** buffer ends. This is NOT an overlap and must produce bit-identical
	** output to the baseline.
	*/
	memcpy (pair, input, sizeof (input)) ;
	memset (pair + ARRAY_LEN (input), 0, sizeof (input)) ;

	memset (&data, 0, sizeof (data)) ;
	data.data_in = pair ;
	data.input_frames = ARRAY_LEN (input) ;
	data.data_out = pair + ARRAY_LEN (input) ;
	data.output_frames = ARRAY_LEN (input) ;
	data.end_of_input = 1 ;
	data.src_ratio = 1.0 ;

	if ((state = src_new (converter, 1, &error)) == NULL)
	{	printf ("\n\nLine %d : src_new failed : %s.\n\n", __LINE__, src_strerror (error)) ;
		exit (1) ;
		} ;

	if ((error = src_process (state, &data)) != SRC_ERR_NO_ERROR)
	{	printf ("\n\nLine %d : adjacent buffers wrongly rejected : %s.\n\n", __LINE__, src_strerror (error)) ;
		exit (1) ;
		} ;

	if (data.output_frames_gen != output_frames_gen
			|| memcmp (pair + ARRAY_LEN (input), baseline, output_frames_gen * sizeof (float)) != 0)
	{	printf ("\n\nLine %d : adjacent buffer output differs from separate buffer baseline.\n\n", __LINE__) ;
		exit (1) ;
		} ;

	state = src_delete (state) ;

	/*----------------------------------------------------------------------
	** Overlapping buffers must be rejected with SRC_ERR_DATA_OVERLAP,
	** whichever way around the buffers lie.
	*/
	for (k = 0 ; k < 3 ; k++)
	{	memset (&data, 0, sizeof (data)) ;
		switch (k)
		{	case 0 : /* Output buffer starts inside the input buffer. */
				data.data_in = pair ;
				data.data_out = pair + 10 ;
				break ;
			case 1 : /* Input buffer starts inside the output buffer. */
				data.data_in = pair + 10 ;
				data.data_out = pair ;
				break ;
			default : /* Identical buffers. */
				data.data_in = pair ;
				data.data_out = pair ;
				break ;
			} ;
		data.input_frames = 100 ;
		data.output_frames = 100 ;
		data.end_of_input = 1 ;
		data.src_ratio = 1.0 ;

		if ((state = src_new (converter, 1, &error)) == NULL)
		{	printf ("\n\nLine %d : src_new failed : %s.\n\n", __LINE__, src_strerror (error)) ;
			exit (1) ;
			} ;

		if ((error = src_process (state, &data)) != SRC_ERR_DATA_OVERLAP)
		{	printf ("\n\nLine %d : expected SRC_ERR_DATA_OVERLAP (%d), got %d.\n\n",
					__LINE__, SRC_ERR_DATA_OVERLAP, error) ;
			exit (1) ;
			} ;

		state = src_delete (state) ;

		/* src_simple must apply the same check. */
		if ((error = src_simple (&data, converter, 1)) != SRC_ERR_DATA_OVERLAP)
		{	printf ("\n\nLine %d : src_simple : expected SRC_ERR_DATA_OVERLAP (%d), got %d.\n\n",
					__LINE__, SRC_ERR_DATA_OVERLAP, error) ;
			exit (1) ;
			} ;
		} ;

	/*----------------------------------------------------------------------
	** Adjacent buffers via src_simple must also succeed and match the
	** baseline bit for bit.
	*/
	memcpy (pair, input, sizeof (input)) ;
	memset (pair + ARRAY_LEN (input), 0, sizeof (input)) ;

	memset (&data, 0, sizeof (data)) ;
	data.data_in = pair ;
	data.input_frames = ARRAY_LEN (input) ;
	data.data_out = pair + ARRAY_LEN (input) ;
	data.output_frames = ARRAY_LEN (input) ;
	data.src_ratio = 1.0 ;

	if ((error = src_simple (&data, converter, 1)) != SRC_ERR_NO_ERROR)
	{	printf ("\n\nLine %d : src_simple on adjacent buffers failed : %s.\n\n", __LINE__, src_strerror (error)) ;
		exit (1) ;
		} ;

	if (data.output_frames_gen != output_frames_gen
			|| memcmp (pair + ARRAY_LEN (input), baseline, output_frames_gen * sizeof (float)) != 0)
	{	printf ("\n\nLine %d : src_simple adjacent buffer output differs from baseline.\n\n", __LINE__) ;
		exit (1) ;
		} ;

	puts ("ok") ;
} /* overlap_test */
