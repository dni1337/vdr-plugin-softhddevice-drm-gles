///
///	@file audio.c		@brief Audio module
///
///	Copyright (c) 2009 - 2014 by Johns.  All Rights Reserved.
///	Copyright (c) 2018 by zille.  All Rights Reserved.
///
///	Contributor(s):
///
///	License: AGPLv3
///
///	This program is free software: you can redistribute it and/or modify
///	it under the terms of the GNU Affero General Public License as
///	published by the Free Software Foundation, either version 3 of the
///	License.
///
///	This program is distributed in the hope that it will be useful,
///	but WITHOUT ANY WARRANTY; without even the implied warranty of
///	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
///	GNU Affero General Public License for more details.
///
///	$Id$
//////////////////////////////////////////////////////////////////////////////

///
///	@defgroup Audio The audio module.
///
///		This module contains all audio output functions.
///
///		ALSA PCM/Mixer api is supported.
///		@see http://www.alsa-project.org/alsa-doc/alsa-lib
///
///	@note alsa async playback is broken, don't use it!
///
///	@todo FIXME: there can be problems with little/big endian.
///

#define USE_AUDIO_THREAD		///< use thread for audio playback
#define USE_AUDIO_MIXER			///< use audio module mixer

#include <stdint.h>
#include <math.h>

#include <libintl.h>
#define _(str) gettext(str)		///< gettext shortcut
#define _N(str) str			///< gettext_noop shortcut

#include <alsa/asoundlib.h>

#ifdef USE_AUDIO_THREAD
#ifndef __USE_GNU
#define __USE_GNU
#endif
#include <pthread.h>
#endif

#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>

#include "iatomic.h"			// portable atomic_t

#include "ringbuffer.h"
#include "misc.h"
#include "audio.h"


//----------------------------------------------------------------------------
//	Variables
//----------------------------------------------------------------------------

char AudioAlsaDriverBroken;		///< disable broken driver message
char AudioAlsaNoCloseOpen;		///< disable alsa close/open fix
char AudioAlsaCloseOpenDelay;		///< enable alsa close/open delay fix

static const char *AudioPCMDevice;	///< PCM device name
static const char *AudioPassthroughDevice;	///< Passthrough device name
static char AudioAppendAES;		///< flag automatic append AES
static const char *AudioMixerDevice;	///< mixer device name
static const char *AudioMixerChannel;	///< mixer channel name
static char AudioDoingInit;		///> flag in init, reduce error
static volatile char AudioRunning;	///< thread running / stopped
static volatile char AudioPaused;	///< audio paused
static volatile char AudioVideoIsReady;	///< video ready start early
static int AudioSkip;			///< skip audio to sync to video

static const int AudioBytesProSample = 2;	///< number of bytes per sample

static int AudioBufferTime = 336;	///< audio buffer time in ms

#ifdef USE_AUDIO_THREAD
static pthread_t AudioThread;		///< audio play thread
static pthread_mutex_t AudioMutex;	///< audio condition mutex
static pthread_cond_t AudioStartCond;	///< condition variable
static char AudioThreadStop;		///< stop audio thread
#else
static const int AudioThread;		///< dummy audio thread
#endif

static char AudioSoftVolume;		///< flag use soft volume
static char AudioNormalize;		///< flag use volume normalize
static char AudioCompression;		///< flag use compress volume
static char AudioMute;			///< flag muted
static int AudioAmplifier;		///< software volume factor
static int AudioNormalizeFactor;	///< current normalize factor
static const int AudioMinNormalize = 100;	///< min. normalize factor
static int AudioMaxNormalize;		///< max. normalize factor
static int AudioCompressionFactor;	///< current compression factor
static int AudioMaxCompression;		///< max. compression factor
static int AudioStereoDescent;		///< volume descent for stereo
static int AudioVolume;			///< current volume (0 .. 1000)

extern int VideoAudioDelay;		///< import audio/video delay

    /// default ring buffer size ~2s 8ch 16bit (3 * 5 * 7 * 8)
static const unsigned AudioRingBufferSize = 3 * 5 * 7 * 8 * 2 * 1000;

static int AudioChannelsInHw[9];	///< table which channels are supported
enum _audio_rates
{					///< sample rates enumeration
    // HW: 32000 44100 48000 88200 96000 176400 192000
    //Audio32000,				///< 32.0Khz
    Audio44100,				///< 44.1Khz
    Audio48000,				///< 48.0Khz
    //Audio88200,				///< 88.2Khz
    //Audio96000,				///< 96.0Khz
    //Audio176400,				///< 176.4Khz
    Audio192000,			///< 192.0Khz
    AudioRatesMax			///< max index
};

    /// table which rates are supported
static int AudioRatesInHw[AudioRatesMax];

    /// input to hardware channel matrix
static int AudioChannelMatrix[AudioRatesMax][9];

    /// rates tables (must be sorted by frequency)
static const unsigned AudioRatesTable[AudioRatesMax] = {
    44100, 48000, 192000
};

//int AudioFilter;
AVFilterGraph *filter_graph;
AVFilterContext *abuffersrc_ctx, *abuffersink_ctx;
int FilterInit;
float AudioEqBand[18];
int AudioEq;
int Filterchanged;

//----------------------------------------------------------------------------
//	filter
//----------------------------------------------------------------------------

static const int AudioNormSamples = 4096;	///< number of samples

#define AudioNormMaxIndex 128		///< number of average values
    /// average of n last sample blocks
static uint32_t AudioNormAverage[AudioNormMaxIndex];
static int AudioNormIndex;		///< index into average table
static int AudioNormReady;		///< index counter
static int AudioNormCounter;		///< sample counter

/**
**	Audio normalizer.
**
**	@param samples	sample buffer
**	@param count	number of bytes in sample buffer
*/
static void AudioNormalizer(int16_t * samples, int count)
{
    int i;
    int l;
    int n;
    uint32_t avg;
    int factor;
    int16_t *data;

    // average samples
    l = count / AudioBytesProSample;
    data = samples;
    do {
	n = l;
	if (AudioNormCounter + n > AudioNormSamples) {
	    n = AudioNormSamples - AudioNormCounter;
	}
	avg = AudioNormAverage[AudioNormIndex];
	for (i = 0; i < n; ++i) {
	    int t;

	    t = data[i];
	    avg += (t * t) / AudioNormSamples;
	}
	AudioNormAverage[AudioNormIndex] = avg;
	AudioNormCounter += n;
	if (AudioNormCounter >= AudioNormSamples) {
	    if (AudioNormReady < AudioNormMaxIndex) {
		AudioNormReady++;
	    } else {
		avg = 0;
		for (i = 0; i < AudioNormMaxIndex; ++i) {
		    avg += AudioNormAverage[i] / AudioNormMaxIndex;
		}

		// calculate normalize factor
		if (avg > 0) {
		    factor = ((INT16_MAX / 8) * 1000U) / (uint32_t) sqrt(avg);
		    // smooth normalize
		    AudioNormalizeFactor =
			(AudioNormalizeFactor * 500 + factor * 500) / 1000;
		    if (AudioNormalizeFactor < AudioMinNormalize) {
			AudioNormalizeFactor = AudioMinNormalize;
		    }
		    if (AudioNormalizeFactor > AudioMaxNormalize) {
			AudioNormalizeFactor = AudioMaxNormalize;
		    }
		} else {
		    factor = 1000;
		}
		Debug(4, "audio/noramlize: avg %8d, fac=%6.3f, norm=%6.3f\n",
		    avg, factor / 1000.0, AudioNormalizeFactor / 1000.0);
	    }

	    AudioNormIndex = (AudioNormIndex + 1) % AudioNormMaxIndex;
	    AudioNormCounter = 0;
	    AudioNormAverage[AudioNormIndex] = 0U;
	}
	data += n;
	l -= n;
    } while (l > 0);

    // apply normalize factor
    for (i = 0; i < count / AudioBytesProSample; ++i) {
	int t;

	t = (samples[i] * AudioNormalizeFactor) / 1000;
	if (t < INT16_MIN) {
	    t = INT16_MIN;
	} else if (t > INT16_MAX) {
	    t = INT16_MAX;
	}
	samples[i] = t;
    }
}

/**
**	Reset normalizer.
*/
static void AudioResetNormalizer(void)
{
    int i;

    AudioNormCounter = 0;
    AudioNormReady = 0;
    for (i = 0; i < AudioNormMaxIndex; ++i) {
	AudioNormAverage[i] = 0U;
    }
    AudioNormalizeFactor = 1000;
}

/**
**	Audio compression.
**
**	@param samples	sample buffer
**	@param count	number of bytes in sample buffer
*/
static void AudioCompressor(int16_t * samples, int count)
{
    int max_sample;
    int i;
    int factor;

    // find loudest sample
    max_sample = 0;
    for (i = 0; i < count / AudioBytesProSample; ++i) {
		int t;

		t = abs(samples[i]);
		if (t > max_sample) {
			max_sample = t;
		}
    }

    // calculate compression factor
    if (max_sample > 0) {
		factor = (INT16_MAX * 1000) / max_sample;
		// smooth compression (FIXME: make configurable?)
		AudioCompressionFactor =
			(AudioCompressionFactor * 950 + factor * 50) / 1000;
		if (AudioCompressionFactor > factor) {
			AudioCompressionFactor = factor;	// no clipping
		}
		if (AudioCompressionFactor > AudioMaxCompression) {
			AudioCompressionFactor = AudioMaxCompression;
		}
    } else {
		return;				// silent nothing todo
    }

    Debug(4, "audio/compress: max %5d, fac=%6.3f, com=%6.3f\n", max_sample,
	factor / 1000.0, AudioCompressionFactor / 1000.0);

    // apply compression factor
    for (i = 0; i < count / AudioBytesProSample; ++i) {
		int t;

		t = (samples[i] * AudioCompressionFactor) / 1000;
		if (t < INT16_MIN) {
			t = INT16_MIN;
		} else if (t > INT16_MAX) {
			t = INT16_MAX;
		}
		samples[i] = t;
    }
}

/**
**	Reset compressor.
*/
static void AudioResetCompressor(void)
{
    AudioCompressionFactor = 2000;
    if (AudioCompressionFactor > AudioMaxCompression) {
		AudioCompressionFactor = AudioMaxCompression;
    }
}

/**
**	Audio software amplifier.
**
**	@param samples	sample buffer
**	@param count	number of bytes in sample buffer
**
**	@todo FIXME: this does hard clipping
*/
static void AudioSoftAmplifier(int16_t * samples, int count)
{
    int i;

    // silence
    if (AudioMute || !AudioAmplifier) {
	memset(samples, 0, count);
	return;
    }

    for (i = 0; i < count / AudioBytesProSample; ++i) {
	int t;

	t = (samples[i] * AudioAmplifier) / 1000;
	if (t < INT16_MIN) {
	    t = INT16_MIN;
	} else if (t > INT16_MAX) {
	    t = INT16_MAX;
	}
	samples[i] = t;
    }
}

#ifdef USE_AUDIO_MIXER

/**
**	Upmix mono to stereo.
**
**	@param in	input sample buffer
**	@param frames	number of frames in sample buffer
**	@param out	output sample buffer
*/
static void AudioMono2Stereo(const int16_t * in, int frames, int16_t * out)
{
    int i;

    for (i = 0; i < frames; ++i) {
	int t;

	t = in[i];
	out[i * 2 + 0] = t;
	out[i * 2 + 1] = t;
    }
}

/**
**	Downmix stereo to mono.
**
**	@param in	input sample buffer
**	@param frames	number of frames in sample buffer
**	@param out	output sample buffer
*/
static void AudioStereo2Mono(const int16_t * in, int frames, int16_t * out)
{
    int i;

    for (i = 0; i < frames; i += 2) {
	out[i / 2] = (in[i + 0] + in[i + 1]) / 2;
    }
}

/**
**	Downmix surround to stereo.
**
**	ffmpeg L  R  C	Ls Rs		-> alsa L R  Ls Rs C
**	ffmpeg L  R  C	LFE Ls Rs	-> alsa L R  Ls Rs C  LFE
**	ffmpeg L  R  C	LFE Ls Rs Rl Rr	-> alsa L R  Ls Rs C  LFE Rl Rr
**
**	@param in	input sample buffer
**	@param in_chan	nr. of input channels
**	@param frames	number of frames in sample buffer
**	@param out	output sample buffer
*/
static void AudioSurround2Stereo(const int16_t * in, int in_chan, int frames,
    int16_t * out)
{
    while (frames--) {
	int l;
	int r;

	switch (in_chan) {
	    case 3:			// stereo or surround? =>stereo
		l = in[0] * 600;	// L
		r = in[1] * 600;	// R
		l += in[2] * 400;	// C
		r += in[2] * 400;
		break;
	    case 4:			// quad or surround? =>quad
		l = in[0] * 600;	// L
		r = in[1] * 600;	// R
		l += in[2] * 400;	// Ls
		r += in[3] * 400;	// Rs
		break;
	    case 5:			// 5.0
		l = in[0] * 500;	// L
		r = in[1] * 500;	// R
		l += in[2] * 200;	// Ls
		r += in[3] * 200;	// Rs
		l += in[4] * 300;	// C
		r += in[4] * 300;
		break;
	    case 6:			// 5.1
		l = in[0] * 400;	// L
		r = in[1] * 400;	// R
		l += in[2] * 200;	// Ls
		r += in[3] * 200;	// Rs
		l += in[4] * 300;	// C
		r += in[4] * 300;
		l += in[5] * 100;	// LFE
		r += in[5] * 100;
		break;
	    case 7:			// 7.0
		l = in[0] * 400;	// L
		r = in[1] * 400;	// R
		l += in[2] * 200;	// Ls
		r += in[3] * 200;	// Rs
		l += in[4] * 300;	// C
		r += in[4] * 300;
		l += in[5] * 100;	// RL
		r += in[6] * 100;	// RR
		break;
	    case 8:			// 7.1
		l = in[0] * 400;	// L
		r = in[1] * 400;	// R
		l += in[2] * 150;	// Ls
		r += in[3] * 150;	// Rs
		l += in[4] * 250;	// C
		r += in[4] * 250;
		l += in[5] * 100;	// LFE
		r += in[5] * 100;
		l += in[6] * 100;	// RL
		r += in[7] * 100;	// RR
		break;
	    default:
		abort();
	}
	in += in_chan;

	out[0] = l / 1000;
	out[1] = r / 1000;
	out += 2;
    }
}

/**
**	Upmix @a in_chan channels to @a out_chan.
**
**	@param in	input sample buffer
**	@param in_chan	nr. of input channels
**	@param frames	number of frames in sample buffer
**	@param out	output sample buffer
**	@param out_chan	nr. of output channels
*/
static void AudioUpmix(const int16_t * in, int in_chan, int frames,
    int16_t * out, int out_chan)
{
    while (frames--) {
	int i;

	for (i = 0; i < in_chan; ++i) {	// copy existing channels
	    *out++ = *in++;
	}
	for (; i < out_chan; ++i) {	// silents missing channels
	    *out++ = 0;
	}
    }
}

/**
**	Resample ffmpeg sample format to hardware format.
**
**	FIXME: use libswresample for this and move it to codec.
**	FIXME: ffmpeg to alsa conversion is already done in codec.c.
**
**	ffmpeg L  R  C	Ls Rs		-> alsa L R  Ls Rs C
**	ffmpeg L  R  C	LFE Ls Rs	-> alsa L R  Ls Rs C  LFE
**	ffmpeg L  R  C	LFE Ls Rs Rl Rr	-> alsa L R  Ls Rs C  LFE Rl Rr
**
**	@param in	input sample buffer
**	@param in_chan	nr. of input channels
**	@param frames	number of frames in sample buffer
**	@param out	output sample buffer
**	@param out_chan	nr. of output channels
*/
static void AudioResample(const int16_t * in, int in_chan, int frames,
    int16_t * out, int out_chan)
{
    switch (in_chan * 8 + out_chan) {
	case 1 * 8 + 1:
	case 2 * 8 + 2:
	case 3 * 8 + 3:
	case 4 * 8 + 4:
	case 5 * 8 + 5:
	case 6 * 8 + 6:
	case 7 * 8 + 7:
	case 8 * 8 + 8:		// input = output channels
	    memcpy(out, in, frames * in_chan * AudioBytesProSample);
	    break;
	case 2 * 8 + 1:
	    AudioStereo2Mono(in, frames, out);
	    break;
	case 1 * 8 + 2:
	    AudioMono2Stereo(in, frames, out);
	    break;
	case 3 * 8 + 2:
	case 4 * 8 + 2:
	case 5 * 8 + 2:
	case 6 * 8 + 2:
	case 7 * 8 + 2:
	case 8 * 8 + 2:
	    AudioSurround2Stereo(in, in_chan, frames, out);
	    break;
	case 5 * 8 + 6:
	case 3 * 8 + 8:
	case 5 * 8 + 8:
	case 6 * 8 + 8:
	    AudioUpmix(in, in_chan, frames, out, out_chan);
	    break;

	default:
	    Error("audio: unsupported %d -> %d channels resample\n", in_chan,
		out_chan);
	    // play silence
	    memset(out, 0, frames * out_chan * AudioBytesProSample);
	    break;
    }
}

#endif

/**
**	Set filter bands.
**
**	@param band		setting frequenz bands
*/
void AudioSetEq(int band[17], int onoff)
{
	int i;

/*	fprintf(stderr, "AudioSetEq %i %i %i %i %i %i %i %i %i %i %i %i %i %i %i %i %i %i onoff %d\n",
		band[0], band[1], band[2], band[3], band[4], band[5], band[6], band[7],
		band[8], band[9], band[10], band[11], band[12], band[13], band[14],
		band[15], band[16], band[17], onoff);
*/
    for (i = 0; i < 18; i++) {
		switch (band[i]) {
			case 10:
				AudioEqBand[i] = 6;
				break;
			case 9:
				AudioEqBand[i] = 5.5;
				break;
			case 8:
				AudioEqBand[i] = 5;
				break;
			case 7:
				AudioEqBand[i] = 4.5;
				break;
			case 6:
				AudioEqBand[i] = 4;
				break;
			case 5:
				AudioEqBand[i] = 3.5;
				break;
			case 4:
				AudioEqBand[i] = 3;
				break;
			case 3:
				AudioEqBand[i] = 2.5;
				break;
			case 2:
				AudioEqBand[i] = 2;
				break;
			case 1:
				AudioEqBand[i] = 1.5;
				break;
			case 0:
				AudioEqBand[i] = 1;
				break;
			case -1:
				AudioEqBand[i] = 0.95;
				break;
			case -2:
				AudioEqBand[i] = 0.9;
				break;
			case -3:
				AudioEqBand[i] = 0.85;
				break;
			case -4:
				AudioEqBand[i] = 0.8;
				break;
			case -5:
				AudioEqBand[i] = 0.75;
				break;
			case -6:
				AudioEqBand[i] = 0.7;
				break;
			case -7:
				AudioEqBand[i] = 0.65;
				break;
			case -8:
				AudioEqBand[i] = 0.6;
				break;
			case -9:
				AudioEqBand[i] = 0.55;
				break;
			case -10:
				AudioEqBand[i] = 0.5;
				break;
		}
	}

/*		fprintf(stderr, "1b=%.2f 2b=%.2f 3b=%.2f 4b=%.2f 5b=%.2f 6b=%.2f"
			" 7b=%.2f 8b=%.2f 9b=%.2f 10b=%.2f 11b=%.2f 12b=%.2f 13b=%.2f"
			" 14b=%.2f 15b=%.2f 16b=%.2f 17b=%.2f 18b=%.2f\n",
			AudioEqBand[0], AudioEqBand[1], AudioEqBand[2], AudioEqBand[3],
			AudioEqBand[4], AudioEqBand[5], AudioEqBand[6], AudioEqBand[7],
			AudioEqBand[8], AudioEqBand[9], AudioEqBand[10], AudioEqBand[11],
			AudioEqBand[12], AudioEqBand[13], AudioEqBand[14], AudioEqBand[15],
			AudioEqBand[16], AudioEqBand[17]);
*/
	if (AudioEq && onoff)
		Filterchanged = 1;
	AudioEq = onoff;
}

/**
**	Filter init.
*/
void AudioFilterInit(AVFrame *frame)
{
	const AVFilter  *abuffer;
	AVFilterContext *filter_ctx[3];
	const AVFilter *eq;
	const AVFilter *aformat;
	const AVFilter *abuffersink;
	char ch_layout[64];
	char options_str[1024];
	int err, i, n_filter = 0;

#if LIBAVFILTER_VERSION_INT < AV_VERSION_INT(7,16,100)
	avfilter_register_all();
#endif

	if (!(filter_graph = avfilter_graph_alloc()))
		fprintf(stderr, "Unable to create filter graph.\n");

	// input buffer
	if (!(abuffer = avfilter_get_by_name("abuffer")))
		fprintf(stderr, "Could not find the abuffer filter.\n");
	if (!(abuffersrc_ctx = avfilter_graph_alloc_filter(filter_graph, abuffer, "src")))
		fprintf(stderr, "Could not allocate the abuffersrc_ctx instance.\n");
	av_get_channel_layout_string(ch_layout, sizeof(ch_layout), frame->channels, frame->channel_layout);
//	fprintf(stderr, "CodecAudioFilterInit: ch_layout %s sample_fmt %s sample_rate %d\n",
//		ch_layout, av_get_sample_fmt_name(frame->format), frame->sample_rate);
	av_opt_set    (abuffersrc_ctx, "channel_layout", ch_layout,                             AV_OPT_SEARCH_CHILDREN);
	av_opt_set    (abuffersrc_ctx, "sample_fmt",     av_get_sample_fmt_name(frame->format), AV_OPT_SEARCH_CHILDREN);
	av_opt_set_q  (abuffersrc_ctx, "time_base",      (AVRational){ 1, frame->sample_rate }, AV_OPT_SEARCH_CHILDREN);
	av_opt_set_int(abuffersrc_ctx, "sample_rate",    frame->sample_rate,                    AV_OPT_SEARCH_CHILDREN);
	// initialize the filter with NULL options, set all options above.
	if (avfilter_init_str(abuffersrc_ctx, NULL) < 0)
		fprintf(stderr, "Could not initialize the abuffer filter.\n");

	if (AudioEq) {
		// superequalizer
		if (!(eq = avfilter_get_by_name("superequalizer")))
			fprintf(stderr, "Could not find the superequalizer filter.\n");
		if (!(filter_ctx[n_filter] = avfilter_graph_alloc_filter(filter_graph, eq, "superequalizer")))
			fprintf(stderr, "Could not allocate the superequalizer instance.\n");
		snprintf(options_str, sizeof(options_str),"1b=%.2f:2b=%.2f:3b=%.2f:4b=%.2f:5b=%.2f"
			":6b=%.2f:7b=%.2f:8b=%.2f:9b=%.2f:10b=%.2f:11b=%.2f:12b=%.2f:13b=%.2f:14b=%.2f:"
			"15b=%.2f:16b=%.2f:17b=%.2f:18b=%.2f", AudioEqBand[0], AudioEqBand[1],
			AudioEqBand[2], AudioEqBand[3], AudioEqBand[4], AudioEqBand[5],
			AudioEqBand[6], AudioEqBand[7], AudioEqBand[8], AudioEqBand[9],
			AudioEqBand[10], AudioEqBand[11], AudioEqBand[12], AudioEqBand[13],
			AudioEqBand[14], AudioEqBand[15], AudioEqBand[16], AudioEqBand[17]);
		if (avfilter_init_str(filter_ctx[n_filter], options_str) < 0)
			fprintf(stderr, "Could not initialize the superequalizer filter.\n");
		n_filter++;
	}
	// aformat
	if (!(aformat = avfilter_get_by_name("aformat")))
		fprintf(stderr, "Could not find the aformat filter.\n");
	if (!(filter_ctx[n_filter] = avfilter_graph_alloc_filter(filter_graph, aformat, "aformat")))
		fprintf(stderr, "Could not allocate the aformat instance.\n");
	snprintf(options_str, sizeof(options_str),
		"sample_fmts=%s:sample_rates=%d:channel_layouts=%s",
		av_get_sample_fmt_name(AV_SAMPLE_FMT_S16),
		frame->sample_rate, ch_layout);
	if (avfilter_init_str(filter_ctx[n_filter], options_str) < 0)
		fprintf(stderr, "Could not initialize the aformat filter.\n");
	n_filter++;

	// abuffersink
	if (!(abuffersink = avfilter_get_by_name("abuffersink")))
		fprintf(stderr, "Could not find the abuffersink filter.\n");
	if (!(filter_ctx[n_filter] = avfilter_graph_alloc_filter(filter_graph, abuffersink, "sink")))
		fprintf(stderr, "Could not allocate the abuffersink instance.\n");
	if (avfilter_init_str(filter_ctx[n_filter], NULL) < 0)
		fprintf(stderr, "Could not initialize the abuffersink instance.\n");
	n_filter++;

	// Connect the filters
	for (i = 0; i < n_filter; i++) {
		if (i == 0) {
			err = avfilter_link(abuffersrc_ctx, 0, filter_ctx[i], 0);
		} else {
			err = avfilter_link(filter_ctx[i - 1], 0, filter_ctx[i], 0);
		}
	}
	if (err < 0)
		fprintf(stderr, "Error connecting audio filters\n");

	// Configure the graph.
	if (avfilter_graph_config(filter_graph, NULL) < 0)
		fprintf(stderr, "Error configuring the audio filter graph\n");

	abuffersink_ctx = filter_ctx[n_filter - 1];
	Filterchanged = 0;
	FilterInit = 1;
}

//----------------------------------------------------------------------------
//	ring buffer
//----------------------------------------------------------------------------

#define AUDIO_RING_MAX 8		///< number of audio ring buffers

/**
**	Audio ring buffer.
*/
typedef struct _audio_ring_ring_
{
    char FlushBuffers;			///< flag: flush buffers
    char Passthrough;			///< flag: use pass-through (AC-3, ...)
    int16_t PacketSize;			///< packet size
    unsigned HwSampleRate;		///< hardware sample rate in Hz
    unsigned HwChannels;		///< hardware number of channels
    unsigned InSampleRate;		///< input sample rate in Hz
    unsigned InChannels;		///< input number of channels
    int64_t PTS;			///< pts clock
    RingBuffer *RingBuffer;		///< sample ring buffer
} AudioRingRing;

    /// ring of audio ring buffers
static AudioRingRing AudioRing[AUDIO_RING_MAX];
static int AudioRingWrite;		///< audio ring write pointer
static int AudioRingRead;		///< audio ring read pointer
static atomic_t AudioRingFilled;	///< how many of the ring is used
static unsigned AudioStartThreshold;	///< start play, if filled

/**
**	Add sample-rate, number of channels change to ring.
**
**	@param sample_rate	sample-rate frequency
**	@param channels		number of channels
**	@param passthrough	use /pass-through (AC-3, ...) device
**
**	@retval -1	error
**	@retval 0	okay
**
**	@note this function shouldn't fail.  Checks are done during AudoInit.
*/
static int AudioRingAdd(unsigned sample_rate, int channels, int passthrough)
{
    unsigned u;

    // search supported sample-rates
    for (u = 0; u < AudioRatesMax; ++u) {
	if (AudioRatesTable[u] == sample_rate) {
	    goto found;
	}
	if (AudioRatesTable[u] > sample_rate) {
	    break;
	}
    }
    Error(_("audio: %dHz sample-rate unsupported\n"), sample_rate);
    return -1;				// unsupported sample-rate

  found:
    if (!AudioChannelMatrix[u][channels]) {
	Error(_("audio: %d channels unsupported\n"), channels);
	return -1;			// unsupported nr. of channels
    }

    if (atomic_read(&AudioRingFilled) == AUDIO_RING_MAX) {	// no free slot
	// FIXME: can wait for ring buffer empty
	Error(_("audio: out of ring buffers\n"));
	return -1;
    }
    AudioRingWrite = (AudioRingWrite + 1) % AUDIO_RING_MAX;

    AudioRing[AudioRingWrite].FlushBuffers = 0;
    AudioRing[AudioRingWrite].Passthrough = passthrough;
    AudioRing[AudioRingWrite].PacketSize = 0;
    AudioRing[AudioRingWrite].InSampleRate = sample_rate;
    AudioRing[AudioRingWrite].InChannels = channels;
    AudioRing[AudioRingWrite].HwSampleRate = sample_rate;
    AudioRing[AudioRingWrite].HwChannels = AudioChannelMatrix[u][channels];
    AudioRing[AudioRingWrite].PTS = INT64_C(0x8000000000000000);
    RingBufferReset(AudioRing[AudioRingWrite].RingBuffer);

    Debug(3, "audio: %d ring buffer prepared\n",
	atomic_read(&AudioRingFilled) + 1);

    atomic_inc(&AudioRingFilled);

#ifdef USE_AUDIO_THREAD
    if (AudioThread) {
	// tell thread, that there is something todo
	AudioRunning = 1;
	pthread_cond_signal(&AudioStartCond);
    }
#endif

    return 0;
}

/**
**	Setup audio ring.
*/
static void AudioRingInit(void)
{
    int i;

    for (i = 0; i < AUDIO_RING_MAX; ++i) {
	// ~2s 8ch 16bit
	AudioRing[i].RingBuffer = RingBufferNew(AudioRingBufferSize);
    }
    atomic_set(&AudioRingFilled, 0);
}

/**
**	Cleanup audio ring.
*/
static void AudioRingExit(void)
{
    int i;

    for (i = 0; i < AUDIO_RING_MAX; ++i) {
	if (AudioRing[i].RingBuffer) {
	    RingBufferDel(AudioRing[i].RingBuffer);
	    AudioRing[i].RingBuffer = NULL;
	}
	AudioRing[i].HwSampleRate = 0;	// checked for valid setup
	AudioRing[i].InSampleRate = 0;
    }
    AudioRingRead = 0;
    AudioRingWrite = 0;
}


//============================================================================
//	A L S A
//============================================================================

//----------------------------------------------------------------------------
//	Alsa variables
//----------------------------------------------------------------------------

static snd_pcm_t *AlsaPCMHandle;	///< alsa pcm handle
static char AlsaCanPause;		///< hw supports pause
static int AlsaUseMmap;			///< use mmap

static snd_mixer_t *AlsaMixer;		///< alsa mixer handle
static snd_mixer_elem_t *AlsaMixerElem;	///< alsa pcm mixer element
static int AlsaRatio;			///< internal -> mixer ratio * 1000

//----------------------------------------------------------------------------
//	alsa pcm
//----------------------------------------------------------------------------

/**
**	Play samples from ringbuffer.
**
**	Fill the kernel buffer, as much as possible.
**
**	@retval	0	ok
**	@retval 1	ring buffer empty
**	@retval -1	underrun error
*/
static int AlsaPlayRingbuffer(void)
{
    int first;

    first = 1;
    for (;;) {				// loop for ring buffer wrap
	int avail;
	int n;
	int err;
	int frames;
	const void *p;

	// how many bytes can be written?
	n = snd_pcm_avail_update(AlsaPCMHandle);
	if (n < 0) {
	    if (n == -EAGAIN) {
		continue;
	    }
	    Warning(_("audio/alsa: avail underrun error? '%s'\n"),
		snd_strerror(n));
	    err = snd_pcm_recover(AlsaPCMHandle, n, 0);
	    if (err >= 0) {
		continue;
	    }
	    Error(_("audio/alsa: snd_pcm_avail_update(): %s\n"),
		snd_strerror(n));
	    return -1;
	}
	avail = snd_pcm_frames_to_bytes(AlsaPCMHandle, n);
	if (avail < 256) {		// too much overhead
	    if (first) {
		// happens with broken alsa drivers
		if (AudioThread) {
		    if (!AudioAlsaDriverBroken) {
			Error(_("audio/alsa: broken driver %d state '%s'\n"),
			    avail,
			    snd_pcm_state_name(snd_pcm_state(AlsaPCMHandle)));
		    }
		    // try to recover
		    if (snd_pcm_state(AlsaPCMHandle)
			== SND_PCM_STATE_PREPARED) {
			if ((err = snd_pcm_start(AlsaPCMHandle)) < 0) {
			    Error(_("audio/alsa: snd_pcm_start(): %s\n"),
				snd_strerror(err));
			}
		    }
		    usleep(5 * 1000);
		}
	    }
	    Debug(4, "audio/alsa: break state '%s'\n",
		snd_pcm_state_name(snd_pcm_state(AlsaPCMHandle)));
	    break;
	}

	n = RingBufferGetReadPointer(AudioRing[AudioRingRead].RingBuffer, &p);
	if (!n) {			// ring buffer empty
	    if (first) {		// only error on first loop
		Debug(4, "audio/alsa: empty buffers %d\n", avail);
		// ring buffer empty
		// AlsaLowWaterMark = 1;
		return 1;
	    }
	    return 0;
	}
	if (n < avail) {		// not enough bytes in ring buffer
	    avail = n;
	}
	if (!avail) {			// full or buffer empty
	    break;
	}
	// muting pass-through AC-3, can produce disturbance
	if (AudioMute || (AudioSoftVolume
		&& !AudioRing[AudioRingRead].Passthrough)) {
	    // FIXME: quick&dirty cast
	    AudioSoftAmplifier((int16_t *) p, avail);
	    // FIXME: if not all are written, we double amplify them
	}
	frames = snd_pcm_bytes_to_frames(AlsaPCMHandle, avail);
#ifdef DEBUG
	if (avail != snd_pcm_frames_to_bytes(AlsaPCMHandle, frames)) {
	    Error(_("audio/alsa: bytes lost -> out of sync\n"));
	}
#endif

	for (;;) {
	    if (AlsaUseMmap) {
		err = snd_pcm_mmap_writei(AlsaPCMHandle, p, frames);
	    } else {
		err = snd_pcm_writei(AlsaPCMHandle, p, frames);
	    }
	    //Debug(3, "audio/alsa: wrote %d/%d frames\n", err, frames);
	    if (err != frames) {
		if (err < 0) {
		    if (err == -EAGAIN) {
			continue;
		    }
		    /*
		       if (err == -EBADFD) {
		       goto again;
		       }
		     */
		    Warning(_("audio/alsa: writei underrun error? '%s'\n"),
			snd_strerror(err));
		    err = snd_pcm_recover(AlsaPCMHandle, err, 0);
		    if (err >= 0) {
			continue;
		    }
		    Error(_("audio/alsa: snd_pcm_writei failed: %s\n"),
			snd_strerror(err));
		    return -1;
		}
		// this could happen, if underrun happened
		Warning(_("audio/alsa: not all frames written\n"));
		avail = snd_pcm_frames_to_bytes(AlsaPCMHandle, err);
	    }
	    break;
	}
	RingBufferReadAdvance(AudioRing[AudioRingRead].RingBuffer, avail);
	first = 0;
    }

    return 0;
}

/**
**	Flush alsa buffers.
*/
static void AlsaFlushBuffers(void)
{
    if (AlsaPCMHandle) {
	int err;
	snd_pcm_state_t state;

	state = snd_pcm_state(AlsaPCMHandle);
	Debug(3, "audio/alsa: flush state %s\n", snd_pcm_state_name(state));
	if (state != SND_PCM_STATE_OPEN) {
	    if ((err = snd_pcm_drop(AlsaPCMHandle)) < 0) {
		Error(_("audio: snd_pcm_drop(): %s\n"), snd_strerror(err));
	    }
	    // ****ing alsa crash, when in open state here
	    if ((err = snd_pcm_prepare(AlsaPCMHandle)) < 0) {
		Error(_("audio: snd_pcm_prepare(): %s\n"), snd_strerror(err));
	    }
	}
    }
}

#ifdef USE_AUDIO_THREAD

//----------------------------------------------------------------------------
//	thread playback
//----------------------------------------------------------------------------

/**
**	Alsa thread
**
**	Play some samples and return.
**
**	@retval	-1	error
**	@retval 0	underrun
**	@retval	1	running
*/
static int AlsaThread(void)
{
    int err;

    if (!AlsaPCMHandle) {
	usleep(24 * 1000);
	return -1;
    }
    for (;;) {
	if (AudioPaused) {
	    return 1;
	}
	// wait for space in kernel buffers
	if ((err = snd_pcm_wait(AlsaPCMHandle, 24)) < 0) {
	    Warning(_("audio/alsa: wait underrun error? '%s'\n"),
		snd_strerror(err));
	    err = snd_pcm_recover(AlsaPCMHandle, err, 0);
	    if (err >= 0) {
		continue;
	    }
	    Error(_("audio/alsa: snd_pcm_wait(): %s\n"), snd_strerror(err));
	    usleep(24 * 1000);
	    return -1;
	}
	break;
    }
    if (!err || AudioPaused) {		// timeout or some commands
	return 1;
    }

    if ((err = AlsaPlayRingbuffer())) {	// empty or error
	snd_pcm_state_t state;

	if (err < 0) {			// underrun error
	    return -1;
	}

	state = snd_pcm_state(AlsaPCMHandle);
	if (state != SND_PCM_STATE_RUNNING) {
	    Debug(3, "audio/alsa: stopping play '%s'\n",
		snd_pcm_state_name(state));
	    return 0;
	}

	usleep(24 * 1000);		// let fill/empty the buffers
    }
    return 1;
}

#endif

//----------------------------------------------------------------------------

/**
**	Open alsa pcm device.
**
**	@param passthrough	use pass-through (AC-3, ...) device
*/
static snd_pcm_t *AlsaOpenPCM(int passthrough)
{
    const char *device;
    snd_pcm_t *handle;
    int err;

    // &&|| hell
    if (!(passthrough && ((device = AudioPassthroughDevice)
		|| (device = getenv("ALSA_PASSTHROUGH_DEVICE"))))
	&& !(device = AudioPCMDevice) && !(device = getenv("ALSA_DEVICE"))) {
	device = "default";
    }
    if (!AudioDoingInit) {		// reduce blabla during init
	Info(_("audio/alsa: using %sdevice '%s'\n"),
	    passthrough ? "pass-through " : "", device);
    }
    //
    // for AC3 pass-through try to set the non-audio bit, use AES0=6
    //
    if (passthrough && AudioAppendAES) {
#if 0
	// FIXME: not yet finished
	char *buf;
	const char *s;
	int n;

	n = strlen(device);
	buf = alloca(n + sizeof(":AES0=6") + 1);
	strcpy(buf, device);
	if (!(s = strchr(buf, ':'))) {
	    // no alsa parameters
	    strcpy(buf + n, ":AES=6");
	}
	Debug(3, "audio/alsa: try '%s'\n", buf);
#endif
    }
    // open none blocking; if device is already used, we don't want wait
    if ((err =
	    snd_pcm_open(&handle, device, SND_PCM_STREAM_PLAYBACK,
		SND_PCM_NONBLOCK)) < 0) {
	Error(_("audio/alsa: playback open '%s' error: %s\n"), device,
	    snd_strerror(err));
	return NULL;
    }

    if ((err = snd_pcm_nonblock(handle, 0)) < 0) {
	Error(_("audio/alsa: can't set block mode: %s\n"), snd_strerror(err));
    }
    return handle;
}

/**
**	Initialize alsa pcm device.
**
**	@see AudioPCMDevice
*/
static void AlsaInitPCM(void)
{
    snd_pcm_t *handle;
    snd_pcm_hw_params_t *hw_params;
    int err;

    if (!(handle = AlsaOpenPCM(0))) {
	return;
    }
    // FIXME: pass-through and pcm out can support different features
    snd_pcm_hw_params_alloca(&hw_params);
    // choose all parameters
    if ((err = snd_pcm_hw_params_any(handle, hw_params)) < 0) {
	Error(_
	    ("audio: snd_pcm_hw_params_any: no configurations available: %s\n"),
	    snd_strerror(err));
    }
    AlsaCanPause = snd_pcm_hw_params_can_pause(hw_params);
    Info(_("audio/alsa: supports pause: %s\n"), AlsaCanPause ? "yes" : "no");

    AlsaPCMHandle = handle;
}

//----------------------------------------------------------------------------
//	Alsa Mixer
//----------------------------------------------------------------------------

/**
**	Set alsa mixer volume (0-1000)
**
**	@param volume	volume (0 .. 1000)
*/
static void AlsaSetVolume(int volume)
{
    int v;

    if (AlsaMixer && AlsaMixerElem) {
	v = (volume * AlsaRatio) / (1000 * 1000);
	snd_mixer_selem_set_playback_volume(AlsaMixerElem, 0, v);
	snd_mixer_selem_set_playback_volume(AlsaMixerElem, 1, v);
    }
}

/**
**	Initialize alsa mixer.
*/
static void AlsaInitMixer(void)
{
    const char *device;
    const char *channel;
    snd_mixer_t *alsa_mixer;
    snd_mixer_elem_t *alsa_mixer_elem;
    long alsa_mixer_elem_min;
    long alsa_mixer_elem_max;

    if (!(device = AudioMixerDevice)) {
	if (!(device = getenv("ALSA_MIXER"))) {
	    device = "default";
	}
    }
    if (!(channel = AudioMixerChannel)) {
	if (!(channel = getenv("ALSA_MIXER_CHANNEL"))) {
	    channel = "PCM";
	}
    }
    Debug(3, "audio/alsa: mixer %s - %s open\n", device, channel);
    snd_mixer_open(&alsa_mixer, 0);
    if (alsa_mixer && snd_mixer_attach(alsa_mixer, device) >= 0
	&& snd_mixer_selem_register(alsa_mixer, NULL, NULL) >= 0
	&& snd_mixer_load(alsa_mixer) >= 0) {

	const char *const alsa_mixer_elem_name = channel;

	alsa_mixer_elem = snd_mixer_first_elem(alsa_mixer);
	while (alsa_mixer_elem) {
	    const char *name;

	    name = snd_mixer_selem_get_name(alsa_mixer_elem);
	    if (!strcasecmp(name, alsa_mixer_elem_name)) {
		snd_mixer_selem_get_playback_volume_range(alsa_mixer_elem,
		    &alsa_mixer_elem_min, &alsa_mixer_elem_max);
		AlsaRatio = 1000 * (alsa_mixer_elem_max - alsa_mixer_elem_min);
		Debug(3, "audio/alsa: PCM mixer found %ld - %ld ratio %d\n",
		    alsa_mixer_elem_min, alsa_mixer_elem_max, AlsaRatio);
		break;
	    }

	    alsa_mixer_elem = snd_mixer_elem_next(alsa_mixer_elem);
	}

	AlsaMixer = alsa_mixer;
	AlsaMixerElem = alsa_mixer_elem;
    } else {
	Error(_("audio/alsa: can't open mixer '%s'\n"), device);
    }
}

//----------------------------------------------------------------------------
//	Alsa API
//----------------------------------------------------------------------------

/**
**	Get alsa audio delay in time-stamps.
**
**	@returns audio delay in time-stamps.
**
**	@todo FIXME: handle the case no audio running
*/
static int64_t AlsaGetDelay(void)
{
    int err;
    snd_pcm_sframes_t delay;
    int64_t pts;

    // setup error
    if (!AlsaPCMHandle || !AudioRing[AudioRingRead].HwSampleRate) {
	return 0L;
    }
    // delay in frames in alsa + kernel buffers
    if ((err = snd_pcm_delay(AlsaPCMHandle, &delay)) < 0) {
	//Debug(3, "audio/alsa: no hw delay\n");
	delay = 0L;
#ifdef DEBUG
    } else if (snd_pcm_state(AlsaPCMHandle) != SND_PCM_STATE_RUNNING) {
	//Debug(3, "audio/alsa: %ld frames delay ok, but not running\n", delay);
#endif
    }
    //Debug(3, "audio/alsa: %ld frames hw delay\n", delay);

    // delay can be negative, when underrun occur
    if (delay < 0) {
	delay = 0L;
    }

    pts =
	((int64_t) delay * 90 * 1000) / AudioRing[AudioRingRead].HwSampleRate;

    return pts;
}

/**
**	Setup alsa audio for requested format.
**
**	@param freq		sample frequency
**	@param channels		number of channels
**	@param passthrough	use pass-through (AC-3, ...) device
**
**	@retval 0	everything ok
**	@retval 1	didn't support frequency/channels combination
**	@retval -1	something gone wrong
**
**	@todo FIXME: remove pointer for freq + channels
*/
static int AlsaSetup(int *freq, int *channels, int passthrough)
{
    snd_pcm_uframes_t buffer_size;
    snd_pcm_uframes_t period_size;
    int err;
    int delay;

    if (!AlsaPCMHandle) {		// alsa not running yet
		// FIXME: if open fails for fe. pass-through, we never recover
		return -1;
    }
    if (!AudioAlsaNoCloseOpen) {	// close+open to fix HDMI no sound bug
		snd_pcm_t *handle;
		handle = AlsaPCMHandle;

		// no lock needed, thread exit in main loop only
		//Debug(3, "audio: %s [\n", __FUNCTION__);
		AlsaPCMHandle = NULL;		// other threads should check handle
		snd_pcm_close(handle);
		if (AudioAlsaCloseOpenDelay) {
			usleep(50 * 1000);		// 50ms delay for alsa recovery
		}
		// FIXME: can use multiple retries
		if (!(handle = AlsaOpenPCM(passthrough))) {
			return -1;
		}
		AlsaPCMHandle = handle;
		//Debug(3, "audio: %s ]\n", __FUNCTION__);
	}

    for (;;) {
		if ((err =
			snd_pcm_set_params(AlsaPCMHandle, SND_PCM_FORMAT_S16,
				AlsaUseMmap ? SND_PCM_ACCESS_MMAP_INTERLEAVED :
				SND_PCM_ACCESS_RW_INTERLEAVED, *channels, *freq, 1,
				96 * 1000))) {
			// try reduced buffer size (needed for sunxi)
			// FIXME: alternativ make this configurable
			if ((err =
				snd_pcm_set_params(AlsaPCMHandle, SND_PCM_FORMAT_S16,
				AlsaUseMmap ? SND_PCM_ACCESS_MMAP_INTERLEAVED :
				SND_PCM_ACCESS_RW_INTERLEAVED, *channels, *freq, 1,
				72 * 1000))) {

			/*
			if ( err == -EBADFD ) {
			snd_pcm_close(AlsaPCMHandle);
			AlsaPCMHandle = NULL;
			continue;
			}
			*/

		if (!AudioDoingInit) {
				Error(_("audio/alsa: set params error: %s\n"),
					snd_strerror(err));
				fprintf(stderr, "audio/AlsaSetup: set params error: %s\n",
					snd_strerror(err));
			}
			// FIXME: must stop sound, AudioChannels ... invalid
			return -1;
			}
		}
		break;
    }
//    fprintf(stderr, "audio/AlsaSetup: AlsaUseMmap: %i channels %i freq %i\n",
//           AlsaUseMmap, *channels, *freq);

    // this is disabled, no advantages!
    if (0) {				// no underruns allowed, play silence
	snd_pcm_sw_params_t *sw_params;
	snd_pcm_uframes_t boundary;

	snd_pcm_sw_params_alloca(&sw_params);
	err = snd_pcm_sw_params_current(AlsaPCMHandle, sw_params);
	if (err < 0) {
	    Error(_("audio: snd_pcm_sw_params_current failed: %s\n"),
		snd_strerror(err));
	}
	if ((err = snd_pcm_sw_params_get_boundary(sw_params, &boundary)) < 0) {
	    Error(_("audio: snd_pcm_sw_params_get_boundary failed: %s\n"),
		snd_strerror(err));
	}
	Debug(4, "audio/alsa: boundary %lu frames\n", boundary);
	if ((err =
		snd_pcm_sw_params_set_stop_threshold(AlsaPCMHandle, sw_params,
		    boundary)) < 0) {
	    Error(_("audio: snd_pcm_sw_params_set_silence_size failed: %s\n"),
		snd_strerror(err));
	}
	if ((err =
		snd_pcm_sw_params_set_silence_size(AlsaPCMHandle, sw_params,
		    boundary)) < 0) {
	    Error(_("audio: snd_pcm_sw_params_set_silence_size failed: %s\n"),
		snd_strerror(err));
	}
	if ((err = snd_pcm_sw_params(AlsaPCMHandle, sw_params)) < 0) {
	    Error(_("audio: snd_pcm_sw_params failed: %s\n"),
		snd_strerror(err));
	}
    }
    // update buffer

    snd_pcm_get_params(AlsaPCMHandle, &buffer_size, &period_size);
    Debug(3, "audio/alsa: buffer size %lu %zdms, period size %lu %zdms\n",
	buffer_size, snd_pcm_frames_to_bytes(AlsaPCMHandle,
	    buffer_size) * 1000 / (*freq * *channels * AudioBytesProSample),
	period_size, snd_pcm_frames_to_bytes(AlsaPCMHandle,
	    period_size) * 1000 / (*freq * *channels * AudioBytesProSample));
    Debug(3, "audio/alsa: state %s\n",
	snd_pcm_state_name(snd_pcm_state(AlsaPCMHandle)));

    AudioStartThreshold = snd_pcm_frames_to_bytes(AlsaPCMHandle, period_size);
    // buffer time/delay in ms
    delay = AudioBufferTime;
    if (VideoAudioDelay > 0) {
		delay += VideoAudioDelay / 90;
    }
    if (AudioStartThreshold <
	(*freq * *channels * AudioBytesProSample * delay) / 1000U) {
		AudioStartThreshold =
			(*freq * *channels * AudioBytesProSample * delay) / 1000U;
    }
    // no bigger, than 1/3 the buffer
    if (AudioStartThreshold > AudioRingBufferSize / 3) {
		AudioStartThreshold = AudioRingBufferSize / 3;
    }
    if (!AudioDoingInit) {
		Info(_("audio/alsa: start delay %ums\n"), (AudioStartThreshold * 1000)
			/ (*freq * *channels * AudioBytesProSample));
    }

    return 0;
}

/**
**	Play audio.
*/
/*static void AlsaPlay(void)
{
    int err;

    if (AlsaCanPause) {
	if ((err = snd_pcm_pause(AlsaPCMHandle, 0))) {
	    Error(_("audio/alsa: snd_pcm_pause(): %s\n"), snd_strerror(err));
	}
    } else {
	if ((err = snd_pcm_prepare(AlsaPCMHandle)) < 0) {
	    Error(_("audio/alsa: snd_pcm_prepare(): %s\n"), snd_strerror(err));
	}
    }
#ifdef DEBUG
    if (snd_pcm_state(AlsaPCMHandle) == SND_PCM_STATE_PAUSED) {
	Error(_("audio/alsa: still paused\n"));
    }
#endif
}*/

/**
**	Pause audio.
*/
/*static void AlsaPause(void)
{
    int err;

    if (AlsaCanPause) {
	if ((err = snd_pcm_pause(AlsaPCMHandle, 1))) {
	    Error(_("snd_pcm_pause(): %s\n"), snd_strerror(err));
	}
    } else {
	if ((err = snd_pcm_drop(AlsaPCMHandle)) < 0) {
	    Error(_("snd_pcm_drop(): %s\n"), snd_strerror(err));
	}
    }
}*/

/**
**	Empty log callback
*/
static void AlsaNoopCallback( __attribute__ ((unused))
    const char *file, __attribute__ ((unused))
    int line, __attribute__ ((unused))
    const char *function, __attribute__ ((unused))
    int err, __attribute__ ((unused))
    const char *fmt, ...)
{
}

/**
**	Initialize alsa audio output module.
*/
static void AlsaInit(void)
{
#ifdef DEBUG
    (void)AlsaNoopCallback;
#else
    // disable display of alsa error messages
    snd_lib_error_set_handler(AlsaNoopCallback);
#endif

    AlsaInitPCM();
    AlsaInitMixer();
}

/**
**	Cleanup alsa audio output module.
*/
static void AlsaExit(void)
{
    if (AlsaPCMHandle) {
	snd_pcm_close(AlsaPCMHandle);
	AlsaPCMHandle = NULL;
    }
    if (AlsaMixer) {
	snd_mixer_close(AlsaMixer);
	AlsaMixer = NULL;
	AlsaMixerElem = NULL;
    }
}


//----------------------------------------------------------------------------
//	thread playback
//----------------------------------------------------------------------------

#ifdef USE_AUDIO_THREAD

/**
**	Prepare next ring buffer.
*/
static int AudioNextRing(void)
{
    int passthrough;
    int sample_rate;
    int channels;
    size_t used;

    // update audio format
    // not always needed, but check if needed is too complex
    passthrough = AudioRing[AudioRingRead].Passthrough;
    sample_rate = AudioRing[AudioRingRead].HwSampleRate;
    channels = AudioRing[AudioRingRead].HwChannels;
    if (AlsaSetup(&sample_rate, &channels, passthrough)) {
		Error(_("audio: can't set channels %d sample-rate %dHz\n"), channels,
			sample_rate);
		fprintf(stderr, "audio: can't set channels %d sample-rate %dHz\n",
		channels, sample_rate);
		// FIXME: handle error
		AudioRing[AudioRingRead].HwSampleRate = 0;
		AudioRing[AudioRingRead].InSampleRate = 0;
		return -1;
    }

    AudioSetVolume(AudioVolume);	// update channel delta
    AudioResetCompressor();
    AudioResetNormalizer();

    Debug(3, "audio: a/v next buf(%d,%4zdms)\n", atomic_read(&AudioRingFilled),
	(RingBufferUsedBytes(AudioRing[AudioRingRead].RingBuffer) * 1000)
	/ (AudioRing[AudioRingWrite].HwSampleRate *
	    AudioRing[AudioRingWrite].HwChannels * AudioBytesProSample));

    // stop, if not enough in next buffer
    used = RingBufferUsedBytes(AudioRing[AudioRingRead].RingBuffer);
    if (AudioStartThreshold * 4 < used || (AudioVideoIsReady
	    && AudioStartThreshold < used)) {
	return 0;
    }
    return 1;
}

/**
**	Audio play thread.
**
**	@param dummy	unused thread argument
*/
static void *AudioPlayHandlerThread(void *dummy)
{
    Debug(3, "audio: play thread started\n");
    for (;;) {
	// check if we should stop the thread
	if (AudioThreadStop) {
	    Debug(3, "audio: play thread stopped\n");
	    return PTHREAD_CANCELED;
	}

	Debug(3, "audio: wait on start condition\n");
	pthread_mutex_lock(&AudioMutex);
	AudioRunning = 0;
	do {
	    pthread_cond_wait(&AudioStartCond, &AudioMutex);
	    // cond_wait can return, without signal!
	} while (!AudioRunning);
	pthread_mutex_unlock(&AudioMutex);

	Debug(3, "audio: ----> %dms start\n", (AudioUsedBytes() * 1000)
	    / (!AudioRing[AudioRingWrite].HwSampleRate +
		!AudioRing[AudioRingWrite].HwChannels +
		AudioRing[AudioRingWrite].HwSampleRate *
		AudioRing[AudioRingWrite].HwChannels * AudioBytesProSample));

	do {
	    int filled;
	    int read;
	    int flush;
	    int err;
	    int i;

	    // check if we should stop the thread
	    if (AudioThreadStop) {
		Debug(3, "audio: play thread stopped\n");
		return PTHREAD_CANCELED;
	    }
	    // look if there is a flush command in the queue
	    flush = 0;
	    filled = atomic_read(&AudioRingFilled);
	    read = AudioRingRead;
	    i = filled;
	    while (i--) {
		read = (read + 1) % AUDIO_RING_MAX;
		if (AudioRing[read].FlushBuffers) {
		    AudioRing[read].FlushBuffers = 0;
		    AudioRingRead = read;
		    // handle all flush in queue
		    flush = filled - i;
		}
	    }

	    if (flush) {
		Debug(3, "audio: flush %d ring buffer(s)\n", flush);
		AlsaFlushBuffers();
		atomic_sub(flush, &AudioRingFilled);
		if (AudioNextRing()) {
		    Debug(3, "audio: break after flush\n");
		    break;
		}
		Debug(3, "audio: continue after flush\n");
	    }
	    // try to play some samples
	    err = 0;
	    if (RingBufferUsedBytes(AudioRing[AudioRingRead].RingBuffer)) {
		err = AlsaThread();
	    }
	    // underrun, check if new ring buffer is available
	    if (!err) {
		int passthrough;
		int sample_rate;
		int channels;
		int old_passthrough;
		int old_sample_rate;
		int old_channels;

		// underrun, and no new ring buffer, goto sleep.
		if (!atomic_read(&AudioRingFilled)) {
		    break;
		}

		Debug(3, "audio: next ring buffer\n");
		old_passthrough = AudioRing[AudioRingRead].Passthrough;
		old_sample_rate = AudioRing[AudioRingRead].HwSampleRate;
		old_channels = AudioRing[AudioRingRead].HwChannels;

		atomic_dec(&AudioRingFilled);
		AudioRingRead = (AudioRingRead + 1) % AUDIO_RING_MAX;

		passthrough = AudioRing[AudioRingRead].Passthrough;
		sample_rate = AudioRing[AudioRingRead].HwSampleRate;
		channels = AudioRing[AudioRingRead].HwChannels;
		Debug(3, "audio: thread channels %d frequency %dHz %s\n",
		    channels, sample_rate, passthrough ? "pass-through" : "");
		// audio config changed?
		if (old_passthrough != passthrough
		    || old_sample_rate != sample_rate
		    || old_channels != channels) {
		    // FIXME: wait for buffer drain
		    if (AudioNextRing()) {
			break;
		    }
		} else {
		    AudioResetCompressor();
		    AudioResetNormalizer();
		}
	    }
	    // FIXME: check AudioPaused ...Thread()
	    if (AudioPaused) {
		break;
	    }
	} while (AudioRing[AudioRingRead].HwSampleRate);
    }
    return dummy;
}

/**
**	Initialize audio thread.
*/
static void AudioInitThread(void)
{
    AudioThreadStop = 0;
    pthread_mutex_init(&AudioMutex, NULL);
    pthread_cond_init(&AudioStartCond, NULL);
    pthread_create(&AudioThread, NULL, AudioPlayHandlerThread, NULL);
    pthread_setname_np(AudioThread, "softhddev audio");
}

/**
**	Cleanup audio thread.
*/
static void AudioExitThread(void)
{
    void *retval;

    Debug(3, "audio: %s\n", __FUNCTION__);

    if (AudioThread) {
	AudioThreadStop = 1;
	AudioRunning = 1;		// wakeup thread, if needed
	pthread_cond_signal(&AudioStartCond);
	if (pthread_join(AudioThread, &retval) || retval != PTHREAD_CANCELED) {
	    Error(_("audio: can't cancel play thread\n"));
	}
	pthread_cond_destroy(&AudioStartCond);
	pthread_mutex_destroy(&AudioMutex);
	AudioThread = 0;
    }
}

#endif

//----------------------------------------------------------------------------
//----------------------------------------------------------------------------

/**
**	Place samples in audio output queue.
**
**	@param samples	sample buffer
**	@param count	number of bytes in sample buffer
*/
void AudioEnqueue(__attribute__ ((unused)) const void *samples, int count, AVFrame *inframe)
{
    size_t n;
    int16_t *buffer;
	AVFrame *outframe = NULL;
	int err;

	outframe  = av_frame_alloc();

	if (inframe) {
	if (inframe->sample_rate != (int)AudioRing[AudioRingWrite].HwSampleRate) {
//		fprintf(stderr, "audio: AudioEnqueue sample_rate changed!!!\n");
		AudioSetup(&inframe->sample_rate, &inframe->channels, 0);
	}

	if (FilterInit && (inframe->sample_rate != filter_graph->sink_links[0]->sample_rate
		|| Filterchanged)) {
		avfilter_graph_free(&filter_graph);
		FilterInit = 0;
//		fprintf(stderr, "audio: AudioEnqueue Free the filter graph.\n");
	}

	if (!FilterInit) {
		AudioFilterInit(inframe);
//		fprintf(stderr, "audio: AudioFilterInit\n");
	}
	if (av_buffersrc_add_frame(abuffersrc_ctx, inframe) < 0)
		fprintf(stderr, "audio: Error submitting the frame to the filtergraph\n");
	while ((err = av_buffersink_get_frame(abuffersink_ctx, outframe)) >= 0) {
		if (err < 0)
			fprintf(stderr, "audio: Error processing the filtered frame\n");
	}
/*	if (err == AVERROR(EAGAIN)) {
		fprintf(stderr, "audio: Error filtering AVERROR(EAGAIN)\n");
	} else if (err == AVERROR_EOF) {
		fprintf(stderr, "audio: Error filtering AVERROR_EOF\n");
	} else if (err < 0) {
		fprintf(stderr, "audio: Error filtering the data\n");
	}*/
	} // Hack if (inframe)

	if (outframe->channels) {
/*		fprintf(stderr, "codec/audio in %s %dHz *%d NS %i linesize %d out %s %dHz *%d NS %i linesize %d count %d\n",
			av_get_sample_fmt_name(inframe->format), inframe->sample_rate,
			inframe->channels, inframe->nb_samples, inframe->linesize[0],
			av_get_sample_fmt_name(outframe->format), outframe->sample_rate,
			outframe->channels, outframe->nb_samples, outframe->linesize[0],
			count);*/

#ifdef noDEBUG
		static uint32_t last_tick;
		uint32_t tick;

		tick = GetMsTicks();
		if (tick - last_tick > 101) {
			Debug(3, "audio: enqueue %4d %dms\n", count, tick - last_tick);
		}
		last_tick = tick;
#endif

		void * data = (void *)outframe->data[0];
		count = outframe->nb_samples * outframe->channels * 2;

		if (!AudioRing[AudioRingWrite].HwSampleRate) {
			Debug(3, "audio: enqueue not ready\n");
			return;				// no setup yet
		}
		// save packet size
		if (!AudioRing[AudioRingWrite].PacketSize) {
			AudioRing[AudioRingWrite].PacketSize = count;
			Debug(3, "audio: a/v packet size %d bytes\n", count);
//			fprintf(stderr, "audio: AudioEnqueue a/v packet size %d bytes\n", count);
		}
		buffer = (void *)data;
		// audio sample modification allowed and needed?
		if (!AudioRing[AudioRingWrite].Passthrough && (AudioCompression
			|| AudioNormalize
			|| AudioRing[AudioRingWrite].InChannels !=
			AudioRing[AudioRingWrite].HwChannels)) {
			int frames;
			fprintf(stderr, "audio: AudioEnqueue audio sample modification allowed and needed\n");

			// resample into ring-buffer is too complex in the case of a roundabout
			// just use a temporary buffer
			frames =
				count / (AudioRing[AudioRingWrite].InChannels *
				AudioBytesProSample);
			buffer =
				alloca(frames * AudioRing[AudioRingWrite].HwChannels *
				AudioBytesProSample);
#ifdef USE_AUDIO_MIXER
			// Convert / resample input to hardware format
			AudioResample(samples, AudioRing[AudioRingWrite].InChannels, frames,
				buffer, AudioRing[AudioRingWrite].HwChannels);
#else
#ifdef DEBUG
			if (AudioRing[AudioRingWrite].InChannels !=
				AudioRing[AudioRingWrite].HwChannels) {
				Debug(3, "audio: internal failure channels mismatch\n");
				fprintf(stderr, "audio: AudioEnqueue internal failure channels mismatch\n");
				return;
			}
#endif
			memcpy(buffer, samples, count);
#endif
			count =
				frames * AudioRing[AudioRingWrite].HwChannels *
				AudioBytesProSample;

			if (AudioCompression) {		// in place operation
				AudioCompressor(buffer, count);
			}
			if (AudioNormalize) {		// in place operation
				AudioNormalizer(buffer, count);
			}
		}

/*		fprintf(stderr, "audio: RingBufferWrite count %d PacketSize %d "
			"RingBufferUsedBytes %6zu RingBufferFreeBytes %zu\n",
			count, AudioRing[AudioRingWrite].PacketSize,
			RingBufferUsedBytes(AudioRing[AudioRingWrite].RingBuffer),
			RingBufferFreeBytes(AudioRing[AudioRingWrite].RingBuffer));*/

		n = RingBufferWrite(AudioRing[AudioRingWrite].RingBuffer, buffer, count);
		if (n != (size_t) count) {
			Error(_("audio: can't place %d samples in ring buffer\n"), count);
			fprintf(stderr, "audio: AudioEnqueue can't place %d samples in ring buffer\n", count);
			// too many bytes are lost
			// FIXME: caller checks buffer full.
			// FIXME: should skip more, longer skip, but less often?
			// FIXME: round to channel + sample border
		}

		if (!AudioRunning) {		// check, if we can start the thread
			int skip;

			n = RingBufferUsedBytes(AudioRing[AudioRingWrite].RingBuffer);
			skip = AudioSkip;
			// FIXME: round to packet size

			Debug(3, "audio: start? %4zdms skip %dms\n", (n * 1000)
				/ (AudioRing[AudioRingWrite].HwSampleRate *
				AudioRing[AudioRingWrite].HwChannels * AudioBytesProSample),
				(skip * 1000)
				/ (AudioRing[AudioRingWrite].HwSampleRate *
				AudioRing[AudioRingWrite].HwChannels * AudioBytesProSample));
/*			fprintf(stderr, "audio: AudioEnqueue start? %4zdms skip %dms\n", (n * 1000)
				/ (AudioRing[AudioRingWrite].HwSampleRate *
				AudioRing[AudioRingWrite].HwChannels * AudioBytesProSample),
				(skip * 1000)
				/ (AudioRing[AudioRingWrite].HwSampleRate *
				AudioRing[AudioRingWrite].HwChannels * AudioBytesProSample));
*/
			if (skip) {
				if (n < (unsigned)skip) {
					skip = n;
				}
				AudioSkip -= skip;
				RingBufferReadAdvance(AudioRing[AudioRingWrite].RingBuffer, skip);
				n = RingBufferUsedBytes(AudioRing[AudioRingWrite].RingBuffer);
			}
			// forced start or enough video + audio buffered
			// for some exotic channels * 4 too small
			if (AudioStartThreshold * 4 < n || (AudioVideoIsReady
				&& AudioStartThreshold < n)) {
				// restart play-back
				// no lock needed, can wakeup next time
				AudioRunning = 1;
				pthread_cond_signal(&AudioStartCond);
			}
		}
		AudioRing[AudioRingWrite].PTS = outframe->pts;
	}
	av_frame_unref(outframe);
}

/**
**	Video is ready.
**
**	@param pts	video presentation timestamp
*/
void AudioVideoReady(int64_t pts)
{
    int64_t audio_pts;
    size_t used;

    if (pts == (int64_t) INT64_C(0x8000000000000000)) {
	Debug(3, "audio: a/v start, no valid video\n");
	return;
    }
    // no valid audio known
    if (!AudioRing[AudioRingWrite].HwSampleRate
	|| !AudioRing[AudioRingWrite].HwChannels
	|| AudioRing[AudioRingWrite].PTS ==
	(int64_t) INT64_C(0x8000000000000000)) {
	Debug(3, "audio: a/v start, no valid audio\n");
	AudioVideoIsReady = 1;
	return;
    }
    // Audio.PTS = next written sample time stamp

    used = RingBufferUsedBytes(AudioRing[AudioRingWrite].RingBuffer);
    audio_pts =
	AudioRing[AudioRingWrite].PTS -
	(used * 90 * 1000) / (AudioRing[AudioRingWrite].HwSampleRate *
	AudioRing[AudioRingWrite].HwChannels * AudioBytesProSample);

    Debug(3, "audio: a/v sync buf(%d,%4zdms) %s|%s = %dms %s\n",
	atomic_read(&AudioRingFilled),
	(used * 1000) / (AudioRing[AudioRingWrite].HwSampleRate *
	    AudioRing[AudioRingWrite].HwChannels * AudioBytesProSample),
	Timestamp2String(pts), Timestamp2String(audio_pts),
	(int)(pts - audio_pts) / 90, AudioRunning ? "running" : "ready");

    if (!AudioRunning) {
	int skip;

	// buffer ~15 video frames
	// FIXME: HDTV can use smaller video buffer
	skip =
	    pts - 15 * 20 * 90 - AudioBufferTime * 90 - audio_pts +
	    VideoAudioDelay;
#ifdef DEBUG
	fprintf(stderr, "%dms %dms %dms\n", (int)(pts - audio_pts) / 90,
	    VideoAudioDelay / 90, skip / 90);
#endif
	// guard against old PTS
	if (skip > 0 && skip < 2000 * 90) {
	    skip = (((int64_t) skip * AudioRing[AudioRingWrite].HwSampleRate)
		/ (1000 * 90))
		* AudioRing[AudioRingWrite].HwChannels * AudioBytesProSample;
	    // FIXME: round to packet size
	    if ((unsigned)skip > used) {
		AudioSkip = skip - used;
		skip = used;
	    }
	    Debug(3, "audio: sync advance %dms %d/%zd\n",
		(skip * 1000) / (AudioRing[AudioRingWrite].HwSampleRate *
		    AudioRing[AudioRingWrite].HwChannels *
		    AudioBytesProSample), skip, used);
	    RingBufferReadAdvance(AudioRing[AudioRingWrite].RingBuffer, skip);

	    used = RingBufferUsedBytes(AudioRing[AudioRingWrite].RingBuffer);
	}
	// FIXME: skip<0 we need bigger audio buffer

	// enough video + audio buffered
	if (AudioStartThreshold < used) {
	    AudioRunning = 1;
	    pthread_cond_signal(&AudioStartCond);
	}
    }

    AudioVideoIsReady = 1;
#if 0
    if (AudioRing[AudioRingWrite].HwSampleRate
	&& AudioRing[AudioRingWrite].HwChannels) {
	if (pts != (int64_t) INT64_C(0x8000000000000000)
	    && AudioRing[AudioRingWrite].PTS !=
	    (int64_t) INT64_C(0x8000000000000000)) {
	    Debug(3, "audio: a/v %d %s\n",
		(int)(pts - AudioRing[AudioRingWrite].PTS) / 90,
		AudioRunning ? "running" : "stopped");
	}
	Debug(3, "audio: start %4zdms %s|%s video ready\n",
	    (RingBufferUsedBytes(AudioRing[AudioRingWrite].RingBuffer) * 1000)
	    / (AudioRing[AudioRingWrite].HwSampleRate *
		AudioRing[AudioRingWrite].HwChannels * AudioBytesProSample),
	    Timestamp2String(pts),
	    Timestamp2String(AudioRing[AudioRingWrite].PTS));

	if (!AudioRunning) {
	    size_t used;

	    used = RingBufferUsedBytes(AudioRing[AudioRingWrite].RingBuffer);
	    // enough video + audio buffered
	    if (AudioStartThreshold < used) {
		// too much audio buffered, skip it
		if (AudioStartThreshold < used) {
		    Debug(3, "audio: start %4zdms skip video ready\n",
			((used - AudioStartThreshold) * 1000)
			/ (AudioRing[AudioRingWrite].HwSampleRate *
			    AudioRing[AudioRingWrite].HwChannels *
			    AudioBytesProSample));
		    RingBufferReadAdvance(AudioRing[AudioRingWrite].RingBuffer,
			used - AudioStartThreshold);
		}
		AudioRunning = 1;
		pthread_cond_signal(&AudioStartCond);
	    }
	}
    }
    AudioVideoIsReady = 1;
#endif
}

/**
**	Flush audio buffers.
*/
void AudioFlushBuffers(void)
{
    int old;
    int i;

//	fprintf(stderr, "AudioFlushBuffers: AudioFlushBuffers\n");
	if (FilterInit)
		Filterchanged = 1;

    if (atomic_read(&AudioRingFilled) >= AUDIO_RING_MAX) {
	// wait for space in ring buffer, should never happen
	for (i = 0; i < 24 * 2; ++i) {
	    if (atomic_read(&AudioRingFilled) < AUDIO_RING_MAX) {
		break;
	    }
	    Debug(3, "audio: flush out of ring buffers\n");
	    usleep(1 * 1000);		// avoid hot polling
	}
	if (atomic_read(&AudioRingFilled) >= AUDIO_RING_MAX) {
	    // FIXME: We can set the flush flag in the last wrote ring buffer
	    Error(_("audio: flush out of ring buffers\n"));
	    return;
	}
    }

    old = AudioRingWrite;
    AudioRingWrite = (AudioRingWrite + 1) % AUDIO_RING_MAX;
    AudioRing[AudioRingWrite].FlushBuffers = 1;
    AudioRing[AudioRingWrite].Passthrough = AudioRing[old].Passthrough;
    AudioRing[AudioRingWrite].HwSampleRate = AudioRing[old].HwSampleRate;
    AudioRing[AudioRingWrite].HwChannels = AudioRing[old].HwChannels;
    AudioRing[AudioRingWrite].InSampleRate = AudioRing[old].InSampleRate;
    AudioRing[AudioRingWrite].InChannels = AudioRing[old].InChannels;
    AudioRing[AudioRingWrite].PTS = INT64_C(0x8000000000000000);
    RingBufferReadAdvance(AudioRing[AudioRingWrite].RingBuffer,
	RingBufferUsedBytes(AudioRing[AudioRingWrite].RingBuffer));
    Debug(3, "audio: reset video ready\n");
    AudioVideoIsReady = 0;
    AudioSkip = 0;

    atomic_inc(&AudioRingFilled);

    // FIXME: wait for flush complete needed?
    for (i = 0; i < 24 * 2; ++i) {
	if (!AudioRunning) {		// wakeup thread to flush buffers
	    AudioRunning = 1;
	    pthread_cond_signal(&AudioStartCond);
	}
	// FIXME: waiting on zero isn't correct, but currently works
	if (!atomic_read(&AudioRingFilled)) {
	    break;
	}
	usleep(1 * 1000);		// avoid hot polling
    }
    Debug(3, "audio: audio flush %dms\n", i);
}

/**
**	Call back to play audio polled.
*/
void AudioPoller(void)
{
    // FIXME: write poller
	fprintf(stderr, "FIXME: write audio poller!\n");
}

/**
**	Get free bytes in audio output.
*/
int AudioFreeBytes(void)
{
    return AudioRing[AudioRingWrite].RingBuffer ?
	RingBufferFreeBytes(AudioRing[AudioRingWrite].RingBuffer)
	: INT32_MAX;
}

/**
**	Get used bytes in audio output.
*/
int AudioUsedBytes(void)
{
    // FIXME: not correct, if multiple buffer are in use
    return AudioRing[AudioRingWrite].RingBuffer ?
	RingBufferUsedBytes(AudioRing[AudioRingWrite].RingBuffer) : 0;
}

/**
**	Get audio delay in time stamps.
**
**	@returns audio delay in time stamps.
*/
int64_t AudioGetDelay(void)
{
    int64_t pts;

    if (!AudioRunning) {
	return 0L;			// audio not running
    }
    if (!AudioRing[AudioRingRead].HwSampleRate) {
	return 0L;			// audio not setup
    }
    if (atomic_read(&AudioRingFilled)) {
	return 0L;			// multiple buffers, invalid delay
    }
    pts = AlsaGetDelay();
    pts += ((int64_t) RingBufferUsedBytes(AudioRing[AudioRingRead].RingBuffer)
	* 90 * 1000) / (AudioRing[AudioRingRead].HwSampleRate *
	AudioRing[AudioRingRead].HwChannels * AudioBytesProSample);
    Debug(4, "audio: hw+sw delay %zd %" PRId64 "ms\n",
	RingBufferUsedBytes(AudioRing[AudioRingRead].RingBuffer), pts / 90);

    return pts;
}

/**
**	Get current audio clock.
**
**	@returns the audio clock in time stamps.
*/
int64_t AudioGetClock(void)
{
    // (cast) needed for the evil gcc
    if (AudioRing[AudioRingRead].PTS != (int64_t) INT64_C(0x8000000000000000)) {
	int64_t delay;

	// delay zero, if no valid time stamp
	if ((delay = AudioGetDelay())) {
	    if (AudioRing[AudioRingRead].Passthrough) {
		return AudioRing[AudioRingRead].PTS + 0 * 90 - delay;
	    }
	    return AudioRing[AudioRingRead].PTS + 0 * 90 - delay;
	}
    }
    return INT64_C(0x8000000000000000);
}

/**
**	Set mixer volume (0-1000)
**
**	@param volume	volume (0 .. 1000)
*/
void AudioSetVolume(int volume)
{
    AudioVolume = volume;
    AudioMute = !volume;
    // reduce loudness for stereo output
    if (AudioStereoDescent && AudioRing[AudioRingRead].InChannels == 2
	&& !AudioRing[AudioRingRead].Passthrough) {
	volume -= AudioStereoDescent;
	if (volume < 0) {
	    volume = 0;
	} else if (volume > 1000) {
	    volume = 1000;
	}
    }
    AudioAmplifier = volume;
    if (!AudioSoftVolume) {
	AlsaSetVolume(volume);
    }
}

/**
**	Setup audio for requested format.
**
**	@param freq		sample frequency
**	@param channels		number of channels
**	@param passthrough	use pass-through (AC-3, ...) device
**
**	@retval 0	everything ok
**	@retval 1	didn't support frequency/channels combination
**	@retval -1	something gone wrong
**
**	@todo add support to report best fitting format.
*/
int AudioSetup(int *freq, int *channels, int passthrough)
{
    Debug(3, "audio: setup channels %d frequency %dHz %s\n", *channels, *freq,
	passthrough ? "pass-through" : "");

    // invalid parameter
    if (!freq || !channels || !*freq || !*channels) {
	Debug(3, "audio: bad channels or frequency parameters\n");
	// FIXME: set flag invalid setup
	return -1;
    }
    return AudioRingAdd(*freq, *channels, passthrough);
}

/**
**	Play audio.
*/
void AudioPlay(void)
{
    if (!AudioPaused) {
	Debug(3, "audio: not paused, check the code\n");
	return;
    }
    Debug(3, "audio: resumed\n");
    AudioPaused = 0;
    AudioEnqueue(NULL, 0, NULL);		// wakeup thread
}

/**
**	Pause audio.
*/
void AudioPause(void)
{
    if (AudioPaused) {
	Debug(3, "audio: already paused, check the code\n");
	return;
    }
    Debug(3, "audio: paused\n");
    AudioPaused = 1;
}

/**
**	Set audio buffer time.
**
**	PES audio packets have a max distance of 300 ms.
**	TS audio packet have a max distance of 100 ms.
**	The period size of the audio buffer is 24 ms.
**	With streamdev sometimes extra +100ms are needed.
*/
void AudioSetBufferTime(int delay)
{
    if (!delay) {
	delay = 336;
    }
    AudioBufferTime = delay;
}

/**
**	Enable/disable software volume.
**
**	@param onoff	-1 toggle, true turn on, false turn off
*/
void AudioSetSoftvol(int onoff)
{
    if (onoff < 0) {
	AudioSoftVolume ^= 1;
    } else {
	AudioSoftVolume = onoff;
    }
}

/**
**	Set normalize volume parameters.
**
**	@param onoff	-1 toggle, true turn on, false turn off
**	@param maxfac	max. factor of normalize /1000
*/
void AudioSetNormalize(int onoff, int maxfac)
{
    if (onoff < 0) {
	AudioNormalize ^= 1;
    } else {
	AudioNormalize = onoff;
    }
    AudioMaxNormalize = maxfac;
}

/**
**	Set volume compression parameters.
**
**	@param onoff	-1 toggle, true turn on, false turn off
**	@param maxfac	max. factor of compression /1000
*/
void AudioSetCompression(int onoff, int maxfac)
{
    if (onoff < 0) {
		AudioCompression ^= 1;
    } else {
		AudioCompression = onoff;
    }
    AudioMaxCompression = maxfac;
    if (!AudioCompressionFactor) {
		AudioCompressionFactor = 1000;
    }
    if (AudioCompressionFactor > AudioMaxCompression) {
		AudioCompressionFactor = AudioMaxCompression;
    }
}

/**
**	Set stereo loudness descent.
**
**	@param delta	value (/1000) to reduce stereo volume
*/
void AudioSetStereoDescent(int delta)
{
    AudioStereoDescent = delta;
    AudioSetVolume(AudioVolume);	// update channel delta
}

/**
**	Set pcm audio device.
**
**	@param device	name of pcm device (fe. "hw:0,9")
**
**	@note this is currently used to select alsa output module.
*/
void AudioSetDevice(const char *device)
{
    AudioPCMDevice = device;
}

/**
**	Set pass-through audio device.
**
**	@param device	name of pass-through device (fe. "hw:0,1")
**
**	@note this is currently usable with alsa only.
*/
void AudioSetPassthroughDevice(const char *device)
{
    AudioPassthroughDevice = device;
}

/**
**	Set pcm audio mixer channel.
**
**	@param channel	name of the mixer channel (fe. PCM or Master)
**
**	@note this is currently used to select alsa output module.
*/
void AudioSetChannel(const char *channel)
{
    AudioMixerChannel = channel;
}

/**
**	Set automatic AES flag handling.
**
**	@param onoff	turn setting AES flag on or off
*/
void AudioSetAutoAES(int onoff)
{
    if (onoff < 0) {
	AudioAppendAES ^= 1;
    } else {
	AudioAppendAES = onoff;
    }
}

/**
**	Initialize audio output module.
**
**	@todo FIXME: make audio output module selectable.
*/
void AudioInit(void)
{
    unsigned u;
    int freq;
    int chan;

    AudioDoingInit = 1;
    AudioRingInit();
    AlsaInit();
    //
    //	Check which channels/rates/formats are supported
    //	FIXME: we force 44.1Khz and 48Khz must be supported equal
    //	FIXME: should use bitmap of channels supported in RatesInHw
    //	FIXME: use loop over sample-rates
    freq = 44100;
    AudioRatesInHw[Audio44100] = 0;
    for (chan = 1; chan < 9; ++chan) {
		int tchan;
		int tfreq;

		tchan = chan;
		tfreq = freq;
		if (AlsaSetup(&tfreq, &tchan, 0)) {
			AudioChannelsInHw[chan] = 0;
		} else {
			AudioChannelsInHw[chan] = chan;
			AudioRatesInHw[Audio44100] |= (1 << chan);
		}
    }
    freq = 48000;
    AudioRatesInHw[Audio48000] = 0;
    for (chan = 1; chan < 9; ++chan) {
		int tchan;
		int tfreq;

		if (!AudioChannelsInHw[chan]) {
			continue;
		}
		tchan = chan;
		tfreq = freq;
		if (AlsaSetup(&tfreq, &tchan, 0)) {
			//AudioChannelsInHw[chan] = 0;
		} else {
			AudioChannelsInHw[chan] = chan;
			AudioRatesInHw[Audio48000] |= (1 << chan);
		}
    }
    freq = 192000;
    AudioRatesInHw[Audio192000] = 0;
	for (chan = 1; chan < 9; ++chan) {
		int tchan;
		int tfreq;

		if (!AudioChannelsInHw[chan]) {
			continue;
		}
		tchan = chan;
		tfreq = freq;
		if (AlsaSetup(&tfreq, &tchan, 0)) {
			//AudioChannelsInHw[chan] = 0;
		} else {
			AudioChannelsInHw[chan] = chan;
			AudioRatesInHw[Audio192000] |= (1 << chan);
		}
    }
    //	build channel support and conversion table
    for (u = 0; u < AudioRatesMax; ++u) {
	for (chan = 1; chan < 9; ++chan) {
	    AudioChannelMatrix[u][chan] = 0;
	    if (!AudioRatesInHw[u]) {	// rate unsupported
		continue;
	    }
	    if (AudioChannelsInHw[chan]) {
		AudioChannelMatrix[u][chan] = chan;
	    } else {
		switch (chan) {
		    case 1:
			if (AudioChannelsInHw[2]) {
			    AudioChannelMatrix[u][chan] = 2;
			}
			break;
		    case 2:
		    case 3:
			if (AudioChannelsInHw[4]) {
			    AudioChannelMatrix[u][chan] = 4;
			    break;
			}
		    case 4:
			if (AudioChannelsInHw[5]) {
			    AudioChannelMatrix[u][chan] = 5;
			    break;
			}
		    case 5:
			if (AudioChannelsInHw[6]) {
			    AudioChannelMatrix[u][chan] = 6;
			    break;
			}
		    case 6:
			if (AudioChannelsInHw[7]) {
			    AudioChannelMatrix[u][chan] = 7;
			    break;
			}
		    case 7:
			if (AudioChannelsInHw[8]) {
			    AudioChannelMatrix[u][chan] = 8;
			    break;
			}
		    case 8:
			if (AudioChannelsInHw[6]) {
			    AudioChannelMatrix[u][chan] = 6;
			    break;
			}
			if (AudioChannelsInHw[2]) {
			    AudioChannelMatrix[u][chan] = 2;
			    break;
			}
			if (AudioChannelsInHw[1]) {
			    AudioChannelMatrix[u][chan] = 1;
			    break;
			}
			break;
		}
	    }
	}
    }
    for (u = 0; u < AudioRatesMax; ++u) {
	Info(_("audio: %6dHz supports %d %d %d %d %d %d %d %d channels\n"),
	    AudioRatesTable[u], AudioChannelMatrix[u][1],
	    AudioChannelMatrix[u][2], AudioChannelMatrix[u][3],
	    AudioChannelMatrix[u][4], AudioChannelMatrix[u][5],
	    AudioChannelMatrix[u][6], AudioChannelMatrix[u][7],
	    AudioChannelMatrix[u][8]);
    }
#ifdef USE_AUDIO_THREAD
	AudioInitThread();
#endif
    AudioDoingInit = 0;
}

/**
**	Cleanup audio output module.
*/
void AudioExit(void)
{
    Debug(3, "audio: %s\n", __FUNCTION__);

#ifdef USE_AUDIO_THREAD
	AudioExitThread();
#endif
    AlsaExit();
    AudioRingExit();
    AudioRunning = 0;
    AudioPaused = 0;
}
