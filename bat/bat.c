/*
 * Copyright (C) 2013-2015 Intel Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <argp.h>
#include <error.h>
#include <math.h>
#include <limits.h>
#include <locale.h>

#include "aconfig.h"
#include "gettext.h"
#include "version.h"

#include "common.h"

#include "alsa.h"
#include "convert.h"
#include "analyze.h"

static int get_duration(struct bat *bat)
{
	float duration_f;
	long duration_i;
	char *ptrf, *ptri;

	duration_f = strtof(bat->narg, &ptrf);
	if (duration_f == HUGE_VALF || duration_f == -HUGE_VALF
			|| (duration_f == 0.0 && errno != 0))
		goto err_exit;

	duration_i = strtol(bat->narg, &ptri, 10);
	if (duration_i == LONG_MAX || duration_i == LONG_MIN)
		goto err_exit;

	if (*ptrf == 's')
		bat->frames = duration_f * bat->rate;
	else if (*ptri == 0)
		bat->frames = duration_i;
	else
		bat->frames = -1;

	if (bat->frames <= 0 || bat->frames > MAX_FRAMES) {
		fprintf(bat->err, _("Invalid duration. Range: (0, %d(%fs))\n"),
				MAX_FRAMES, (double)MAX_FRAMES / bat->rate);
		return -EINVAL;
	}

	return 0;

err_exit:
	fprintf(bat->err, _("Duration overflow/underflow: %d\n"), -errno);

	return -errno;
}

static void get_sine_frequencies(struct bat *bat, char *freq)
{
	char *tmp0, *tmp1;
	int ncolons;
	int nfreqs;
	int i;
	ncolons = 0;
	tmp1 = freq;
	while ((tmp1 = strchr(tmp1, ':'))) {
		tmp1++;
		ncolons ++;
	}
	nfreqs = ncolons+1;

	tmp0 = freq;
	tmp1 = strchr(tmp0, ':');
	for (i = 0; i < MAX_CHANNELS && i < nfreqs; i++) {
		if (i+1 < nfreqs)
			*tmp1 = '\0';
		bat->target_freq[i] = atof(tmp0);
		tmp0 = tmp1+1;
		if (i+1 < nfreqs)
			tmp1 = strchr(tmp0, ':');
	}
	for (i = nfreqs; i < MAX_CHANNELS; i++)
		bat->target_freq[i] = bat->target_freq[i % nfreqs];
}

static void get_format(struct bat *bat, char *optarg)
{
	if (strcasecmp(optarg, "cd") == 0) {
		bat->format = SND_PCM_FORMAT_S16_LE;
		bat->rate = 44100;
		bat->channels = 2;
	} else if (strcasecmp(optarg, "dat") == 0) {
		bat->format = SND_PCM_FORMAT_S16_LE;
		bat->rate = 48000;
		bat->channels = 2;
	} else {
		bat->format = snd_pcm_format_value(optarg);
		if (bat->format == SND_PCM_FORMAT_UNKNOWN) {
			fprintf(bat->err, _("wrong extended format '%s'\n"),
					optarg);
			exit(EXIT_FAILURE);
		}
	}

	switch (bat->format) {
	case SND_PCM_FORMAT_U8:
		bat->sample_size = 1;
		break;
	case SND_PCM_FORMAT_S16_LE:
		bat->sample_size = 2;
		break;
	case SND_PCM_FORMAT_S24_3LE:
		bat->sample_size = 3;
		break;
	case SND_PCM_FORMAT_S32_LE:
		bat->sample_size = 4;
		break;
	default:
		fprintf(bat->err, _("unsupported format: %d\n"), bat->format);
		exit(EXIT_FAILURE);
	}
}

static inline int thread_wait_completion(struct bat *bat,
		pthread_t id, int **val)
{
	int err;

	err = pthread_join(id, (void **) val);
	if (err)
		pthread_cancel(id);

	return err;
}

/* loopback test where we play sine wave and capture the same sine wave */
static void test_loopback(struct bat *bat)
{
	pthread_t capture_id, playback_id;
	int err;
	int *thread_result_capture, *thread_result_playback;

	/* start playback */
	err = pthread_create(&playback_id, NULL,
			(void *) bat->playback.fct, bat);
	if (err != 0) {
		fprintf(bat->err, _("Cannot create playback thread: %d\n"),
				err);
		exit(EXIT_FAILURE);
	}

	/* TODO: use a pipe to signal stream start etc - i.e. to sync threads */
	/* Let some time for playing something before capturing */
	usleep(CAPTURE_DELAY * 1000);

	/* start capture */
	err = pthread_create(&capture_id, NULL, (void *) bat->capture.fct, bat);
	if (err != 0) {
		fprintf(bat->err, _("Cannot create capture thread: %d\n"), err);
		pthread_cancel(playback_id);
		exit(EXIT_FAILURE);
	}

	/* wait for playback to complete */
	err = thread_wait_completion(bat, playback_id, &thread_result_playback);
	if (err != 0) {
		fprintf(bat->err, _("Cannot join playback thread: %d\n"), err);
		free(thread_result_playback);
		pthread_cancel(capture_id);
		exit(EXIT_FAILURE);
	}

	/* check playback status */
	if (*thread_result_playback != 0) {
		fprintf(bat->err, _("Exit playback thread fail: %d\n"),
				*thread_result_playback);
		pthread_cancel(capture_id);
		exit(EXIT_FAILURE);
	} else {
		fprintf(bat->log, _("Playback completed.\n"));
	}

	/* now stop and wait for capture to finish */
	pthread_cancel(capture_id);
	err = thread_wait_completion(bat, capture_id, &thread_result_capture);
	if (err != 0) {
		fprintf(bat->err, _("Cannot join capture thread: %d\n"), err);
		free(thread_result_capture);
		exit(EXIT_FAILURE);
	}

	/* check capture status */
	if (*thread_result_capture != 0) {
		fprintf(bat->err, _("Exit capture thread fail: %d\n"),
				*thread_result_capture);
		exit(EXIT_FAILURE);
	} else {
		fprintf(bat->log, _("Capture completed.\n"));
	}
}

/* single ended playback only test */
static void test_playback(struct bat *bat)
{
	pthread_t playback_id;
	int err;
	int *thread_result;

	/* start playback */
	err = pthread_create(&playback_id, NULL,
			(void *) bat->playback.fct, bat);
	if (err != 0) {
		fprintf(bat->err, _("Cannot create playback thread: %d\n"),
				err);
		exit(EXIT_FAILURE);
	}

	/* wait for playback to complete */
	err = thread_wait_completion(bat, playback_id, &thread_result);
	if (err != 0) {
		fprintf(bat->err, _("Cannot join playback thread: %d\n"), err);
		free(thread_result);
		exit(EXIT_FAILURE);
	}

	/* check playback status */
	if (*thread_result != 0) {
		fprintf(bat->err, _("Exit playback thread fail: %d\n"),
				*thread_result);
		exit(EXIT_FAILURE);
	} else {
		fprintf(bat->log, _("Playback completed.\n"));
	}
}

/* single ended capture only test */
static void test_capture(struct bat *bat)
{
	pthread_t capture_id;
	int err;
	int *thread_result;

	/* start capture */
	err = pthread_create(&capture_id, NULL, (void *) bat->capture.fct, bat);
	if (err != 0) {
		fprintf(bat->err, _("Cannot create capture thread: %d\n"), err);
		exit(EXIT_FAILURE);
	}

	/* TODO: stop capture */

	/* wait for capture to complete */
	err = thread_wait_completion(bat, capture_id, &thread_result);
	if (err != 0) {
		fprintf(bat->err, _("Cannot join capture thread: %d\n"), err);
		free(thread_result);
		exit(EXIT_FAILURE);
	}

	/* check playback status */
	if (*thread_result != 0) {
		fprintf(bat->err, _("Exit capture thread fail: %d\n"),
				*thread_result);
		exit(EXIT_FAILURE);
	} else {
		fprintf(bat->log, _("Capture completed.\n"));
	}
}

static void set_defaults(struct bat *bat)
{
	int i;
	memset(bat, 0, sizeof(struct bat));

	/* Set default values */
	bat->rate = 44100;
	bat->channels = 1;
	bat->frame_size = 2;
	bat->sample_size = 2;
	bat->format = SND_PCM_FORMAT_S16_LE;
	bat->convert_float_to_sample = convert_float_to_int16;
	bat->convert_sample_to_double = convert_int16_to_double;
	bat->frames = bat->rate * 2;
	for (i = 0; i < MAX_CHANNELS; i++)
		bat->target_freq[i] = 997.0;
	bat->sigma_k = 3.0;
	bat->playback.device = NULL;
	bat->capture.device = NULL;
	bat->buf = NULL;
	bat->local = false;
	bat->playback.fct = &playback_alsa;
	bat->capture.fct = &record_alsa;
	bat->playback.mode = MODE_LOOPBACK;
	bat->capture.mode = MODE_LOOPBACK;
	bat->period_is_limited = false;
	bat->log = stdout;
	bat->err = stderr;
}


static error_t parse_opt (int key, char *arg, struct argp_state *state)
{
	struct bat *bat = state->input;
	switch (key) {
	case OPT_LOG:
		bat->logarg = arg;
		break;
	case OPT_READFILE:
		bat->playback.file = arg;
		break;
	case OPT_SAVEPLAY:
		bat->debugplay = arg;
		break;
	case OPT_LOCAL:
		bat->local = true;
		break;
	case 'D':
		if (bat->playback.device == NULL)
			bat->playback.device = arg;
		if (bat->capture.device == NULL)
			bat->capture.device = arg;
		break;
	case 'P':
		if (bat->capture.mode == MODE_SINGLE)
			bat->capture.mode = MODE_LOOPBACK;
		else
			bat->playback.mode = MODE_SINGLE;
		bat->playback.device = arg;
		break;
	case 'C':
		if (bat->playback.mode == MODE_SINGLE)
			bat->playback.mode = MODE_LOOPBACK;
		else
			bat->capture.mode = MODE_SINGLE;
		bat->capture.device = arg;
		break;
	case 'n':
		bat->narg = arg;
		break;
	case 'F':
		get_sine_frequencies(bat, arg);
		break;
	case 'c':
		bat->channels = atoi(arg);
		break;
	case 'r':
		bat->rate = atoi(arg);
		break;
	case 'f':
		get_format(bat, arg);
		break;
	case 'k':
		bat->sigma_k = atof(arg);
		break;
	case 'p':
		bat->periods_total = atoi(arg);
		bat->period_is_limited = true;
		break;
	default:
		return ARGP_ERR_UNKNOWN;
		break;
	}
	return 0;
}
static void parse_arguments(struct bat *bat, int argc, char *argv[])
{

	const char *doc =
		_("Basic Audio Tester\n"
		  "\n"
		  "Uses a loopback configuration or 2 PC configuration "
		  "(play on one PC and record on the other) to test "
		  "if if audio is flowing smoothly."
		  "\n"
		  "Full documentation in the bat man page and at "
		  "https://github.com/01org/bat/wiki"
		  "\n"
		  "Recognized sample formats are: %s %s %s %s\n"
		  "The available format shotcuts are:\n"
		  "\t-f cd (16 bit little endian, 44100, stereo)\n"
		  "\t-f dat (16 bit little endian, 48000, stereo)\n");

	const char args_doc[] = "";

	struct argp_option options[] = {
		{"log", OPT_LOG, "FILENAME", 0,
		 _("file that both stdout and strerr redirecting to"), 0},
		{"file", OPT_READFILE, "FILENAME", 0,
		 _("file for playback"),0},
		{"saveplay", OPT_SAVEPLAY, "FILENAME", 0,
		 _("file for storing playback content, for debug"), 0},
		{"local", OPT_LOCAL, 0, 0,
		 _("internal loopback, set to bypass pcm hardware devices"),0},
		{"device-duplex", 'D', "DEVICE", 0,
		 _("pcm device for both playback and capture"), 0},
		{"device-playback", 'P', "DEVICE", 0,
		 _("pcm device for playback"), 0},
		{"device-capture", 'C', "DEVICE", 0,
		 _("pcm device for capture"), 0},
		{"sample_format" , 'f', "FORMAT", 0,
		 _("sample format"), 0},
		{"channels", 'c', "CHANNELS", 0,
		 _("number of channels"), 0},
		{"sample-rate", 'r', "SAMP/SEC", 0,
		 _("sampling rate"), 0},
		{"frames", 'n', "FRAMES", 0,
		 _("The number of frames for playback and/or capture"), 0},
		{"threshold", 'k', "THRESHOLD", 0,
		 _("parameter for frequency detecting threshold"), 0},
		{"target-frequency", 'F', "FREQUENCY", 0,
		 _("target frequency for sin test "), 0},
		{"periods", 'p', "PERIODS", 0,
		 _("total number of periods to play/capture"), 0},
		{ 0 }
	};

	char *doc_filled;
	const int num_formats = 4; // # formats supported below.
	const int doc_size = strlen(doc) + 40*num_formats;
	doc_filled = malloc(doc_size);
	snprintf(doc_filled, doc_size, doc,
		 snd_pcm_format_name(SND_PCM_FORMAT_U8),
		 snd_pcm_format_name(SND_PCM_FORMAT_S16_LE),
		 snd_pcm_format_name(SND_PCM_FORMAT_S24_3LE),
		 snd_pcm_format_name(SND_PCM_FORMAT_S32_LE));

	struct argp  argp = {options, parse_opt, args_doc, doc_filled};
	argp_parse(&argp, argc, argv, 0, 0, bat);
	free(doc_filled);
}

static int validate_options(struct bat *bat)
{
	int c;
	float freq_low, freq_high;

	/* check if we have an input file for local mode */
	if ((bat->local == true) && (bat->capture.file == NULL)) {
		fprintf(bat->err, _("no input file for local testing\n"));
		return -EINVAL;
	}

	/* check supported channels */
	if (bat->channels > MAX_CHANNELS || bat->channels < MIN_CHANNELS) {
		fprintf(bat->err, _("%d channels not supported\n"),
				bat->channels);
		return -EINVAL;
	}

	/* check single ended is in either playback or capture - not both */
	if ((bat->playback.mode == MODE_SINGLE)
			&& (bat->capture.mode == MODE_SINGLE)) {
		fprintf(bat->err, _("single ended mode is simplex\n"));
		return -EINVAL;
	}

	/* check sine wave frequency range */
	freq_low = DC_THRESHOLD;
	freq_high = bat->rate * RATE_FACTOR;
	for (c = 0; c < bat->channels; c++) {
		if (bat->target_freq[c] < freq_low
				|| bat->target_freq[c] > freq_high) {
			fprintf(bat->err, _("sine wave frequency out of"));
			fprintf(bat->err, _(" range: (%.1f, %.1f)\n"),
				freq_low, freq_high);
			return -EINVAL;
		}
	}

	return 0;
}

static int bat_init(struct bat *bat)
{
	int err = 0;
	char name[] = TEMP_RECORD_FILE_NAME;

	/* Determine logging to a file or stdout and stderr */
	if (bat->logarg) {
		bat->log = NULL;
		bat->log = fopen(bat->logarg, "wb");
		if (bat->log == NULL) {
			fprintf(bat->err, _("Cannot open file for capture:"));
			fprintf(bat->err, _(" %s %d\n"),
					bat->logarg, -errno);
			return -errno;
		}
		bat->err = bat->log;
	}

	/* Determine duration of playback and/or capture */
	if (bat->narg) {
		err = get_duration(bat);
		if (err < 0)
			return err;
	}

	/* Determine capture file */
	if (bat->local) {
		bat->capture.file = bat->playback.file;
	} else {
		/* create temp file for sound record and analysis */
		err = mkstemp(name);
		if (err == -1) {
			fprintf(bat->err, _("Fail to create record file: %d\n"),
					-errno);
			return -errno;

		}
		/* store file name which is dynamically created */
		bat->capture.file = strdup(name);
		if (bat->capture.file == NULL)
			return -errno;
		/* close temp file */
		close(err);
	}

	/* Initial for playback */
	if (bat->playback.file == NULL) {
		/* No input file so we will generate our own sine wave */
		if (bat->frames) {
			if (bat->playback.mode == MODE_SINGLE) {
				/* Play nb of frames given by -n argument */
				bat->sinus_duration = bat->frames;
			} else {
				/* Play CAPTURE_DELAY msec +
				 * 150% of the nb of frames to be analyzed */
				bat->sinus_duration = bat->rate *
						CAPTURE_DELAY / 1000;
				bat->sinus_duration +=
						(bat->frames + bat->frames / 2);
			}
		} else {
			/* Special case where we want to generate a sine wave
			 * endlessly without capturing */
			bat->sinus_duration = 0;
			bat->playback.mode = MODE_SINGLE;
		}
	} else {
		bat->fp = fopen(bat->playback.file, "rb");
		if (bat->fp == NULL) {
			fprintf(bat->err, _("Cannot open file for playback:"));
			fprintf(bat->err, _(" %s %d\n"),
					bat->playback.file, -errno);
			return -errno;
		}
		err = read_wav_header(bat, bat->playback.file, bat->fp, false);
		fclose(bat->fp);
		if (err != 0)
			return err;
	}

	bat->frame_size = bat->sample_size * bat->channels;

	/* Set conversion functions */
	switch (bat->sample_size) {
	case 1:
		bat->convert_float_to_sample = convert_float_to_uint8;
		bat->convert_sample_to_double = convert_uint8_to_double;
		break;
	case 2:
		bat->convert_float_to_sample = convert_float_to_int16;
		bat->convert_sample_to_double = convert_int16_to_double;
		break;
	case 3:
		bat->convert_float_to_sample = convert_float_to_int24;
		bat->convert_sample_to_double = convert_int24_to_double;
		break;
	case 4:
		bat->convert_float_to_sample = convert_float_to_int32;
		bat->convert_sample_to_double = convert_int32_to_double;
		break;
	default:
		fprintf(bat->err, _("Invalid PCM format: size=%d\n"),
				bat->sample_size);
		return -EINVAL;
	}

	return err;
}

int main(int argc, char *argv[])
{
	struct bat bat;
	int err = 0;

	set_defaults(&bat);

#ifdef ENABLE_NLS
	setlocale(LC_ALL, "");
	textdomain(PACKAGE);
#endif

	fprintf(bat.log, _("%s version %s\n\n"), PACKAGE_NAME, PACKAGE_VERSION);

	parse_arguments(&bat, argc, argv);

	err = bat_init(&bat);
	if (err < 0)
		goto out;

	err = validate_options(&bat);
	if (err < 0)
		goto out;

	/* single line playback thread: playback only, no capture */
	if (bat.playback.mode == MODE_SINGLE) {
		test_playback(&bat);
		goto out;
	}

	/* single line capture thread: capture only, no playback */
	if (bat.capture.mode == MODE_SINGLE) {
		test_capture(&bat);
		goto analyze;
	}

	/* loopback thread: playback and capture in a loop */
	if (bat.local == false)
		test_loopback(&bat);

analyze:
	err = analyze_capture(&bat);
out:
	fprintf(bat.log, _("\nReturn value is %d\n"), err);

	if (bat.logarg)
		fclose(bat.log);
	if (!bat.local)
		free(bat.capture.file);

	return err;
}
