/* 
 *  Squeezelite - lightweight headless squeezebox emulator
 *
 *  (c) Adrian Smith 2012-2015, triode1@btinternet.com
 *      Ralph Irving 2015-2026, ralph_irving@hotmail.com
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

// Portaudio output

#include "squeezelite.h"

#if PORTAUDIO

#include <portaudio.h>

#if WIN
#ifndef PA18API
#include <pa_win_wasapi.h>
#endif
#define snprintf _snprintf
#endif

#if OSX && !defined(OSXPPC)
#include <pa_mac_core.h>
#endif

#if PA18API
typedef int PaDeviceIndex;
typedef double PaTime;

typedef struct PaStreamParameters
{
    PaDeviceIndex device;
    int channelCount;
    PaSampleFormat sampleFormat;
    PaTime suggestedLatency;

} PaStreamParameters;

static int paContinue=0; /* Signal that the stream should continue invoking the callback and processing audio. */
static int paComplete=1; /* Signal that the stream should stop invoking the callback and finish once all output */
			 /* samples have played. */

static unsigned paFramesPerBuffer = 4096;
static unsigned paNumberOfBuffers = 4;
#endif /* PA18API */

#if defined(ANDROID) && DSD
#define ANDROID_DSD 1
#else
#define ANDROID_DSD 0
#endif

// ouput device
static struct {
	unsigned rate;
	PaStream *stream;
#if ANDROID_DSD
	u8_t fmt;        // output.outfmt the open stream or DSD track was opened for
	bool dsd;        // a DSD writer thread owns the output instead of pa.stream
#endif
} pa;

static log_level loglevel;

static bool running = true;
static bool fixed_volume = false;

extern struct outputstate output;
extern struct buffer *outputbuf;

#define LOCK   mutex_lock(outputbuf->mutex)
#define UNLOCK mutex_unlock(outputbuf->mutex)

extern u8_t *silencebuf;
#if DSD
extern u8_t *silencebuf_dsd;
#endif

void list_devices(void) {
	PaError err;
	int i;

	if ((err = Pa_Initialize()) != paNoError) {
		LOG_WARN("error initialising port audio: %s", Pa_GetErrorText(err));
		return;
	}
	
	printf("Output devices:\n");
#ifndef PA18API
	for (i = 0; i < Pa_GetDeviceCount(); ++i) {
		if (Pa_GetDeviceInfo(i)->maxOutputChannels > 1) {
			printf("  %i - %s [%s]\n", i, Pa_GetDeviceInfo(i)->name, Pa_GetHostApiInfo(Pa_GetDeviceInfo(i)->hostApi)->name);
		}
#else
	for (i = 0; i < Pa_CountDevices(); ++i) {
		printf("  %i - %s\n", i, Pa_GetDeviceInfo(i)->name);
#endif
	}
	printf("\n");
	
	if ((err = Pa_Terminate()) != paNoError) {
		LOG_WARN("error closing port audio: %s", Pa_GetErrorText(err));
	}
}

void set_volume(unsigned left, unsigned right) {
	if (fixed_volume) {
		LOCK;
		output.gainL = FIXED_ONE;
		output.gainR = FIXED_ONE;
		UNLOCK;
	} else {
		LOG_DEBUG("setting internal gain left: %u right: %u", left, right);
		LOCK;
		output.gainL = left;
		output.gainR = right;
		UNLOCK;
	}
}

static int pa_device_id(const char *device) {
	int len = strlen(device);
	int i;

	if (!strncmp(device, "default", 7)) {
#ifndef PA18API
		return Pa_GetDefaultOutputDevice();
#else
		return Pa_GetDefaultOutputDeviceID();
#endif
	}
	if (len >= 1 && len <= 2 && device[0] >= '0' && device[0] <= '9') {
		return atoi(device);
	}

#ifndef PA18API
#define DEVICE_ID_MAXLEN 256
	for (i = 0; i < Pa_GetDeviceCount(); ++i) {
		char tmp[DEVICE_ID_MAXLEN];
		snprintf(tmp, DEVICE_ID_MAXLEN, "%s [%s]", Pa_GetDeviceInfo(i)->name, Pa_GetHostApiInfo(Pa_GetDeviceInfo(i)->hostApi)->name);
		if (!strncmp(tmp, device, len)) {
#else
	for (i = 0; i < Pa_CountDevices(); ++i) {
		if (!strncmp(Pa_GetDeviceInfo(i)->name, device, len)) {
#endif
			return i;
		}
	}

	return -1;
}

#ifndef PA18API
static int pa_callback(const void *pa_input, void *pa_output, unsigned long pa_frames_wanted, 
					   const PaStreamCallbackTimeInfo *timeInfo, PaStreamCallbackFlags statusFlags, void *userData);

#else
static int pa_callback(void *pa_input, void *pa_output, unsigned long pa_frames_wanted, 
			   PaTimestamp outTime, void *userData);
#endif
bool test_open(const char *device, unsigned rates[], bool userdef_rates) {
	PaStreamParameters outputParameters;
	PaError err;
	unsigned ref[] TEST_RATES;
	int device_id, i, ind;
#if WIN
	PaWasapiStreamInfo wasapiInfo;
	const PaDeviceInfo * paDeviceInfo;
	const PaHostApiInfo *paHostApiInfo;

#endif
	if ((device_id = pa_device_id(device)) == -1) {
		LOG_INFO("device %s not found", device);
		return false;
	}

	outputParameters.device = device_id;
	outputParameters.channelCount = 2;
	outputParameters.sampleFormat = paInt32;
#ifndef PA18API
	outputParameters.suggestedLatency =
		output.latency ? (double)output.latency/(double)1000 : Pa_GetDeviceInfo(outputParameters.device)->defaultHighOutputLatency;
	outputParameters.hostApiSpecificStreamInfo = NULL;
#if WIN
	paDeviceInfo = Pa_GetDeviceInfo( outputParameters.device );
	paHostApiInfo = Pa_GetHostApiInfo ( paDeviceInfo->hostApi );

	if ( paHostApiInfo != NULL )
	{
		if ( paHostApiInfo->type == paWASAPI )
		{
			/* Use exclusive mode for WasApi device, default is shared */
			if (output.pa_hostapi_option == 1)
			{
				wasapiInfo.size = sizeof(PaWasapiStreamInfo);
				wasapiInfo.hostApiType = paWASAPI;
				wasapiInfo.version = 1;
				wasapiInfo.flags = paWinWasapiExclusive;
				outputParameters.hostApiSpecificStreamInfo = &wasapiInfo;
				LOG_INFO("opening WASAPI device in exclusive mode");
			}
		}
	}
#endif /* WIN */
#endif

	// check supported sample rates
	// Note use Pa_OpenStream as it appears more reliable than Pa_IsFormatSupported on some windows apis
	for (i = 0, ind = 0; ref[i]; ++i) {
#ifndef PA18API
		err = Pa_OpenStream(&pa.stream, NULL, &outputParameters, (double)ref[i],
			paFramesPerBufferUnspecified, paNoFlag, pa_callback, NULL);
#else
		err = Pa_OpenStream(&pa.stream, paNoDevice, 0, 0, NULL, outputParameters.device,
			outputParameters.channelCount, outputParameters.sampleFormat, NULL, (double)ref[i],
			paFramesPerBuffer, paNumberOfBuffers, paNoFlag, pa_callback, NULL);
#endif
		switch (err) {
			case paInvalidSampleRate:
				LOG_DEBUG("sample rate %u not supported", ref[i]);
				continue;
#if WIN && !defined (PA18API)
			/* Ignore these errors for device probe */
			case paUnanticipatedHostError:
				continue;

			case paInvalidDevice:
				continue;
#endif
#if SUN && defined (PA18API)
			/* Ignore this error during device probe */
			case paHostError:
				continue;
#endif
			case paNoError:
				Pa_CloseStream(pa.stream);
				if (!userdef_rates) {
					rates[ind++] = ref[i];
				}
				continue;

			default:	
				/* Any other error is a failure */
				LOG_WARN("error opening portaudio stream: %s (%d)", Pa_GetErrorText(err), err);
				return false;
		}
	}

	if (!rates[0] && !userdef_rates) {
		LOG_WARN("no available rate found");
		return false;
	}

	pa.stream = NULL;
	return true;
}

static void pa_stream_finished(void *userdata) {
	if (running) {
		LOG_INFO("stream finished");
		LOCK;
		output.pa_reopen = true;
		wake_controller();
		UNLOCK;
	}
}

#if ANDROID_DSD
static void dsd_writer_start(bool hold);
static void dsd_writer_stop(void);

// guarded by the output lock
static bool dsd_refused = false;     // the DAC refused DSD: PCM until the player restarts
static bool dsd_route_ok = true;     // the output route takes native DSD (output_route_pa)
static unsigned dsd_route_gen = 0;   // bumped at each route change, followed by the writer
static unsigned own_rates[MAX_SUPPORTED_SAMPLERATES]; // the player's rates, for the FiiO's own DAC
#endif

static thread_type monitor_thread;
bool monitor_thread_running = false;

static void *pa_monitor() {
	bool output_off;

	LOCK;

	if (monitor_thread_running) {
		LOG_DEBUG("monitor thread already running");
		UNLOCK;
		return 0;
	}

	LOG_DEBUG("start monitor thread");

	monitor_thread_running = true;
	output_off = (output.state == OUTPUT_OFF);

	while (monitor_thread_running) {
		if (output_off) {
			if (output.state != OUTPUT_OFF) {
				LOG_INFO("output on");
				break;
			}
		} else {
			// this is a hack to partially support hot plugging of devices
			// we rely on terminating and reinitalising PA to get an updated list of devices and use name for output.device
#ifndef ANDROID
			LOG_INFO("probing device %s", output.device);
			Pa_Terminate();
			Pa_Initialize();
			pa.stream = NULL;
#endif
			if (pa_device_id(output.device) != -1) {
				LOG_INFO("device reopen");
				break;
			}
		}

		UNLOCK;
		sleep(output_off ? 1 : 5);
		LOCK;
	}

	LOG_DEBUG("end monitor thread");

	monitor_thread_running = false;
	pa.stream = NULL;

	_pa_open();

	UNLOCK;

	return 0;
}

void _pa_open(void) {
	PaStreamParameters outputParameters;
	PaError err = paNoError;
	int device_id;
#if WIN
	PaWasapiStreamInfo wasapiInfo;
	const PaDeviceInfo * paDeviceInfo;
	const PaHostApiInfo *paHostApiInfo;

#endif
	if (pa.stream) {
		PaStream *old_stream = pa.stream;
		pa.stream = NULL;

		// Pa_CloseStream can wait for an in-flight data callback, which needs this
		// mutex - so never close the stream while holding it
		UNLOCK;
		if ((err = Pa_CloseStream(old_stream)) != paNoError) {
			LOG_WARN("error closing stream: %s", Pa_GetErrorText(err));
		}
		LOCK;
	}

#if ANDROID_DSD
	if (pa.dsd) {
		dsd_writer_stop();
	}

	// After a stop the output comes on with the last track's format and rate still set, and the
	// next track's first frame at the head of the buffer: open for that track. Otherwise a FLAC after
	// a DSD track opens a native DSD track first, and the reverse opens PCM for a DSD track. The rate
	// goes with it, as a DSD frame rate opened as PCM (705.6k after DSD256) hangs the FiiO's DAC. The
	// track start then has nothing to change. A PCM rate change alone is left as upstream has it.
	if (output.state != OUTPUT_OFF && output.track_start && output.track_start == outputbuf->readp &&
		output.outfmt != output.next_fmt) {
		LOG_INFO("output opens for the next track: %s at %u", output.next_fmt == DSD_U16_LE ? "native DSD" : "PCM",
				 output.next_sample_rate);
		output.outfmt = output.next_fmt;
		output.current_sample_rate = output.next_sample_rate;
	}
#endif

	if (output.state == OUTPUT_OFF) {
		// we get called when transitioning to OUTPUT_OFF to create the probe thread
		// set err to avoid opening device and logging messages
		err = 1;

#if ANDROID_DSD
	} else if (output.outfmt == DSD_U16_LE) {
		// native DSD goes to an AudioTrack, not portaudio
		dsd_writer_start(false);
		output.error_opening = false;
		return;
#endif
	} else if ((device_id = pa_device_id(output.device)) == -1) {
		LOG_INFO("device %s not found", output.device);
		err = 1;

	} else {

		outputParameters.device = device_id;
		outputParameters.channelCount = 2;
		outputParameters.sampleFormat = paInt32;
#ifndef PA18API
		outputParameters.suggestedLatency =
			output.latency ? (double)output.latency/(double)1000 : Pa_GetDeviceInfo(outputParameters.device)->defaultHighOutputLatency;
		outputParameters.hostApiSpecificStreamInfo = NULL;
		
#endif
#if OSX && !defined(OSXPPC)
		/* enable pro mode which aims to avoid resampling if possible */
		/* command line controls pa_hostapi_option which is -1 if not specified, 0 or 1 - choose playnice if -1 or 1 */
		PaMacCoreStreamInfo macInfo;
		unsigned long streamInfoFlags;
	 	if (output.pa_hostapi_option) {
			LOG_INFO("opening device in PlayNice mode");
			streamInfoFlags = paMacCorePlayNice;
		} else {
			LOG_INFO("opening device in Pro mode");
			streamInfoFlags = paMacCorePro;
		}
		PaMacCore_SetupStreamInfo(&macInfo, streamInfoFlags);
		outputParameters.hostApiSpecificStreamInfo = &macInfo;
#endif
#if WIN
		paDeviceInfo = Pa_GetDeviceInfo( outputParameters.device );
		paHostApiInfo = Pa_GetHostApiInfo ( paDeviceInfo->hostApi );

		if ( paHostApiInfo != NULL )
		{
			if ( paHostApiInfo->type == paWASAPI )
			{
				/* Use exclusive mode for WasApi device, default is shared */
				if (output.pa_hostapi_option == 1)
				{
					wasapiInfo.size = sizeof(PaWasapiStreamInfo);
					wasapiInfo.hostApiType = paWASAPI;
					wasapiInfo.version = 1;
					wasapiInfo.flags = paWinWasapiExclusive;
					outputParameters.hostApiSpecificStreamInfo = &wasapiInfo;
					LOG_INFO("opening WASAPI device in exclusive mode");
				}
			}
		}
#endif
	}

	if (!err &&
#ifndef PA18API
		(err = Pa_OpenStream(&pa.stream, NULL, &outputParameters, (double)output.current_sample_rate, paFramesPerBufferUnspecified,
							 paPrimeOutputBuffersUsingStreamCallback | paDitherOff, pa_callback, NULL)) != paNoError) {
		LOG_WARN("error opening device %i - %s [%s] : %s", outputParameters.device, Pa_GetDeviceInfo(outputParameters.device)->name, 
				  Pa_GetHostApiInfo(Pa_GetDeviceInfo(outputParameters.device)->hostApi)->name, Pa_GetErrorText(err));
#else
		(err = Pa_OpenStream(&pa.stream, paNoDevice, 0, 0, NULL, outputParameters.device, outputParameters.channelCount,
							outputParameters.sampleFormat, NULL, (double)output.current_sample_rate, paFramesPerBuffer,
							paNumberOfBuffers, paDitherOff, pa_callback, NULL)) != paNoError) {
		LOG_WARN("error opening device %i - %s : %s", outputParameters.device, Pa_GetDeviceInfo(outputParameters.device)->name, 
				 Pa_GetErrorText(err));
#endif
	}

	if (!err) {
#ifndef PA18API
		LOG_INFO("opened device %i - %s [%s] at %u latency %u ms", outputParameters.device, Pa_GetDeviceInfo(outputParameters.device)->name,
				 Pa_GetHostApiInfo(Pa_GetDeviceInfo(outputParameters.device)->hostApi)->name,
				 (unsigned int)Pa_GetStreamInfo(pa.stream)->sampleRate, (unsigned int)(Pa_GetStreamInfo(pa.stream)->outputLatency * 1000));
#else
		LOG_INFO("opened device %i - %s at %u fpb %u nbf %u", outputParameters.device, Pa_GetDeviceInfo(outputParameters.device)->name,
				 (unsigned int)output.current_sample_rate, paFramesPerBuffer, paNumberOfBuffers);

#endif
		pa.rate = output.current_sample_rate;
#if ANDROID_DSD
		pa.fmt = output.outfmt;
#endif

#ifndef PA18API
		if ((err = Pa_SetStreamFinishedCallback(pa.stream, pa_stream_finished)) != paNoError) {
			LOG_WARN("error setting finish callback: %s", Pa_GetErrorText(err));
		}
	
		UNLOCK; // StartStream can call pa_callback in a sychronised thread on freebsd, remove lock while it is called

#endif
		if ((err = Pa_StartStream(pa.stream)) != paNoError) {
			LOG_WARN("error starting stream: %s", Pa_GetErrorText(err));
		}
#if ANDROID_DSD
		else {
			send_output_format_to_app(false, pa.rate);
		}
#endif

#ifndef PA18API
		LOCK;
#endif
	}

#if ANDROID_DSD
	if (err == paInvalidSampleRate && output.state != OUTPUT_OFF) {
		// AAudio refuses this rate outright (DSD converted to PCM above what the device takes, with no
		// resampler to bring it down), so no reopen can succeed and the monitor thread would retry it
		// without pause, starving the player. Hold the output instead: this track plays silent, the
		// next opens. On a FiiO the resampler brings such rates down first, so this is a safety net.
		LOG_WARN("rate %u cannot be opened: holding the output silent until the next track", output.current_sample_rate);
		pa.stream = NULL;
		dsd_writer_start(true);
		output.error_opening = false;
		return;
	}
#endif

	if (err && !monitor_thread_running) {
		vis_stop();

		// create a thread to check for output state change or device return
#if LINUX || OSX || FREEBSD
		pthread_create(&monitor_thread, NULL, pa_monitor, NULL);
#endif
#if WIN
		monitor_thread = CreateThread(NULL, OUTPUT_THREAD_STACK_SIZE, (LPTHREAD_START_ROUTINE)&pa_monitor, NULL, 0, NULL);
#endif
	}

	output.error_opening = !!err;
}

static u8_t *optr;

static int _write_frames(frames_t out_frames, bool silence, s32_t gainL, s32_t gainR, u8_t flags,
						 s32_t cross_gain_in, s32_t cross_gain_out, s32_t **cross_ptr) {
	
	if (!silence) {
		
		if (output.fade == FADE_ACTIVE && output.fade_dir == FADE_CROSS && *cross_ptr) {
			_apply_cross(outputbuf, out_frames, cross_gain_in, cross_gain_out, cross_ptr);
		}
		
		if (gainL != FIXED_ONE || gainR != FIXED_ONE || (flags & (MONO_LEFT | MONO_RIGHT))) {
			_apply_gain(outputbuf, out_frames, gainL, gainR, flags);
		}

		IF_DSD(
			if (output.outfmt == DOP) {
				update_dop((u32_t *) outputbuf->readp, out_frames, output.invert);
			} else if (output.outfmt != PCM && output.invert)
				dsd_invert((u32_t *) outputbuf->readp, out_frames);
		)

		memcpy(optr, outputbuf->readp, out_frames * BYTES_PER_FRAME);

	} else {

		u8_t *buf = silencebuf;

		IF_DSD(
			if (output.outfmt != PCM) {
				buf = silencebuf_dsd;
				if (output.outfmt == DOP) update_dop((u32_t *) buf, out_frames, false); // don't invert silence
			}
		)

		memcpy(optr, buf, out_frames * BYTES_PER_FRAME);
	}
	
	optr += out_frames * BYTES_PER_FRAME;

	return (int)out_frames;
}

#ifndef PA18API
static int pa_callback(const void *pa_input, void *pa_output, unsigned long pa_frames_wanted, 
					   const PaStreamCallbackTimeInfo *time_info, PaStreamCallbackFlags statusFlags, void *userData) {
#else
static int pa_callback(void *pa_input, void *pa_output, unsigned long pa_frames_wanted,PaTimestamp outTime, void *userData) {
#endif
	int ret;
	frames_t frames;

	optr = (u8_t *)pa_output;

	LOCK;

#ifndef PA18API
	if (time_info->outputBufferDacTime > time_info->currentTime) {
		// workaround for wdm-ks which can return outputBufferDacTime with a different epoch
		output.device_frames = (unsigned)((time_info->outputBufferDacTime - time_info->currentTime) * output.current_sample_rate);
	} else {
		output.device_frames = 0;
	}

#else
	output.device_frames = 0;
#endif
	output.updated = gettime_ms();
	output.frames_played_dmp = output.frames_played;

	do {
		frames = _output_frames(pa_frames_wanted);
		pa_frames_wanted -= frames;
#if ANDROID_DSD
		// the next track needs a different output: leave its frames for that one
		if (pa.fmt != output.outfmt || pa.rate != output.current_sample_rate) break;
#endif
	} while (pa_frames_wanted > 0 && frames != 0);

	if (pa_frames_wanted > 0) {
		LOG_DEBUG("pad with silence");
		memset(optr, 0, pa_frames_wanted * BYTES_PER_FRAME);
	}

	if (output.state == OUTPUT_OFF) {
		LOG_INFO("output off");
		ret = paComplete;
	} else if (pa.rate != output.current_sample_rate) {
		ret = paComplete;
#if ANDROID_DSD
	} else if (pa.fmt != output.outfmt) {
		ret = paComplete;
#endif
	} else {
		ret = paContinue;
	}

	UNLOCK;

#ifdef PA18API
	if ( ret == paComplete )
		pa_stream_finished (userData);
#endif
	return ret;
}

#if ANDROID_DSD
/*
 Native DSD output for Android players whose firmware has a DSD AudioTrack encoding (FiiO).
 A writer thread stands in for the portaudio callback: it pulls DSD_U16_LE frames with
 _output_frames(), packs them to the byte layout the DAC expects and writes them to the
 AudioTrack (android.c), which blocks and so paces the thread. A format, rate or state change
 ends the thread and asks for a reopen, as pa_stream_finished does for portaudio.
 */
extern bool dsd_track_open(unsigned rate, unsigned buffer_bytes, unsigned chunk_bytes);
extern bool dsd_track_write(const u8_t *buf, unsigned len);
extern unsigned dsd_track_position(void);
extern void dsd_track_close(void);

#define DSD_CHUNK_FRAMES 4096 // squeezelite frames per write

static thread_type dsd_thread;
static volatile bool dsd_running = false;
static bool dsd_hold = false;       // the writer only holds the output, see _pa_open

// DSD_U16_LE frames hold rev(b0) << 24 | rev(b1) << 16 per channel, b0 the earliest byte.
// The DAC takes a 16-bit L/R interleave, each 16-bit pair little-endian, at every DSD rate
// (verified by ear on an M11 Plus at DSD64/128/256; a 32-bit interleave plays with hiss).
static void dsd_pack(const u32_t *in, u8_t *out, frames_t frames) {
	frames_t i;
	for (i = 0; i < frames; i++) {
		u32_t l = in[2 * i], r = in[2 * i + 1];
		*out++ = l >> 16; *out++ = l >> 24;
		*out++ = r >> 16; *out++ = r >> 24;
	}
}

// called with the lock held. The DAC refused DSD: later tracks decode to PCM (dsd.c reads
// output.dsdfmt at each track start), and the writer plays the rest of this one out as silence
// so the timeline and LMS stay in step. Reopening DSD instead would fail the same way.
static void dsd_fall_back(void) {
	if (!dsd_refused) {
		LOG_ERROR("native DSD output failed: DSD plays as PCM until the player restarts");
		dsd_refused = true;
		output.dsdfmt = PCM;
	}
}

static bool route_takes(const unsigned rates[], unsigned rate) {
	unsigned i;
	for (i = 0; i < MAX_SUPPORTED_SAMPLERATES && rates[i]; i++) {
		if (rates[i] == rate) return true;
	}
	return false;
}

// android.c, from the Java audio device callback: whether the route takes native DSD, and the
// PCM rates it takes. The FiiO's own DAC takes DSD; a USB DAC the FiiO routes to does not, and
// its HAL hides that: the DSD track writes fine into a USB output that never opens (silence), and
// at the unplug is moved back to the FiiO's DAC as PCM (static). While the route cannot take DSD,
// later tracks decode to PCM (dsd.c reads output.dsdfmt at each track start) and the writer drops
// its track; when it can again, the writer opens a fresh one.
// The FiiO also hands a PCM stream's rate to a USB DAC as it is, and a rate the DAC cannot take
// plays silent (the HAL retries the open). So while one is attached, dac_rates (highest first, 0
// terminated) replace the player's own, and later tracks are resampled to one of them (decoders
// read output.supported_rates at each track start). NULL restores the player's own.
// Returns whether the PCM track playing runs at a rate the new route would not give it, so that
// Library has LMS stream it again: a rate the route cannot take (a USB DAC plugged in), or one it
// was resampled to although the route takes its own rate (the DAC unplugged). A DSD track is
// Library's to judge (it restarts every one), so native DSD output returns false.
bool output_route_pa(bool dsd_ok, const unsigned dac_rates[]) {
	const unsigned *rates = dac_rates ? dac_rates : own_rates;
	unsigned raw;
	bool pcm_stale;
	LOCK;
	if (dsd_route_ok != dsd_ok) {
		LOG_INFO("output route %s native DSD", dsd_ok ? "takes" : "does not take");
		dsd_route_ok = dsd_ok;
		dsd_route_gen++;
	}
	output.dsdfmt = dsd_ok && !dsd_refused ? DSD_U16_LE : PCM;
	if (memcmp(output.supported_rates, rates, sizeof(output.supported_rates))) {
		char list[MAX_SUPPORTED_SAMPLERATES * 8] = "";
		unsigned i;
		memcpy(output.supported_rates, rates, sizeof(output.supported_rates));
		for (i = 0; i < MAX_SUPPORTED_SAMPLERATES && rates[i]; i++) {
			sprintf(list + strlen(list), " %u", rates[i]);
		}
		LOG_INFO("output rates (%s):%s", dac_rates ? "USB DAC" : "player", list);
	}
	raw = output.current_raw_rate;
	pcm_stale = output.outfmt == PCM && output.current_sample_rate &&
				(!route_takes(rates, output.current_sample_rate) ||
				 (raw && raw != output.current_sample_rate && route_takes(rates, raw)));
	if (pcm_stale) {
		LOG_INFO("PCM track at %u (own rate %u) is not at the rate the new route gives it", output.current_sample_rate, raw);
	}
	UNLOCK;
	return pcm_stale;
}

static bool dsd_open_track(unsigned rate) {
	if (dsd_track_open(rate / 2, rate / 2 * 8 / 4, DSD_CHUNK_FRAMES * 4)) { // track rate = DSD / 32; 0.25s buffer
		send_output_format_to_app(true, rate * 16);
		return true;
	}
	LOG_ERROR("native DSD output failed to open at %u", rate);
	LOCK;
	dsd_fall_back();
	UNLOCK;
	return false;
}

// The writer's track, opened when asked: returns whether it has one. Without one nothing reaches
// the DAC, so the app is told rate 0 rather than left on the last output's format
static bool dsd_writer_track(bool open, unsigned rate) {
	if (open && dsd_open_track(rate)) {
		return true;
	}
	send_output_format_to_app(false, 0);
	return false;
}

// called with the output lock held: the output needs reopening for what is now in the buffer.
// A hold ends at the next track as well, whatever its format.
static bool dsd_writer_done(bool hold, u32_t start_time) {
	return pa.fmt != output.outfmt || pa.rate != output.current_sample_rate ||
		   (hold && output.track_start_time != start_time);
}

static void *dsd_writer(void *arg) {
	static u32_t frames_buf[DSD_CHUNK_FRAMES * 2]; // one writer at a time: dsd_writer_stop joins it
	static u8_t packed[DSD_CHUNK_FRAMES * 4];
	unsigned rate = pa.rate;                   // squeezelite frame rate: DSD bit rate / 16, or the PCM rate on hold
	bool hold = dsd_hold;                      // consume the frames silently, never open a track
	u64_t written = 0;                         // squeezelite frames handed to the track
	u64_t track_base = 0;                      // written when the current track opened
	bool reopen = false;
	bool track = false;
	unsigned route_gen;
	bool route_ok;
	u32_t start_time;

	LOCK;
	route_gen = dsd_route_gen;
	route_ok = dsd_route_ok;
	start_time = output.track_start_time;
	UNLOCK;

	if (hold) {
		LOG_INFO("holding the output silent until the next track");
	} else if (!route_ok) {
		LOG_INFO("output route does not take native DSD: this track plays silent");
	}
	track = dsd_writer_track(!hold && route_ok, rate);

	while (dsd_running) {
		frames_t wanted = DSD_CHUNK_FRAMES, got;
		u64_t played = track ? track_base + (u64_t)dsd_track_position() * 2 : written;
		bool moved = false;

		LOCK;
		output.device_frames = written > played ? (unsigned)(written - played) : 0;
		output.updated = gettime_ms();
		output.frames_played_dmp = output.frames_played;
		optr = (u8_t *)frames_buf;
		do {
			got = _output_frames(wanted);
			wanted -= got;
			if (dsd_writer_done(hold, start_time)) break;
		} while (wanted > 0 && got != 0);
		if (wanted > 0) {
			memset(optr, 0x69, wanted * BYTES_PER_FRAME); // DSD silence
		}
		if (output.state == OUTPUT_OFF || dsd_writer_done(hold, start_time)) {
			reopen = true;
		}
		if (!hold && dsd_route_gen != route_gen) {
			route_gen = dsd_route_gen;
			route_ok = dsd_route_ok;
			moved = true;
		}
		UNLOCK;

		if (moved) {
			// the HAL moved the track with the route and cannot be trusted with it: drop it, and
			// open a fresh one where DSD plays. Frames already in the old track are lost
			if (track) {
				dsd_track_close();
			}
			if (route_ok) {
				LOG_INFO("output route takes native DSD again: reopening at %u", rate);
			} else {
				LOG_INFO("output route does not take native DSD: rest of this track plays silent");
			}
			track = dsd_writer_track(route_ok, rate);
			track_base = written;
		}

		if (track) {
			dsd_pack(frames_buf, packed, DSD_CHUNK_FRAMES);
			if (!dsd_track_write(packed, DSD_CHUNK_FRAMES * 4)) {
				LOCK;
				dsd_fall_back();
				UNLOCK;
				dsd_track_close(); // a later route change opens a fresh one, never on top of this
				track = dsd_writer_track(false, rate);
			}
		} else {
			usleep((u64_t)DSD_CHUNK_FRAMES * 1000000 / rate); // the time the chunk would have played
		}
		written += DSD_CHUNK_FRAMES;
		if (reopen) break;
	}

	dsd_track_close();

	LOCK;
	if (reopen && dsd_running) {
		// ended itself: hand the output back as portaudio does when its stream finishes
		LOG_INFO("%s finished", hold ? "output hold" : "DSD output");
		output.pa_reopen = true;
		wake_controller();
	}
	dsd_running = false;
	UNLOCK;
	return NULL;
}

// called with the output lock held. hold: no output at all, the writer only paces the buffer
static void dsd_writer_start(bool hold) {
	pa.rate = output.current_sample_rate;
	pa.fmt = output.outfmt;
	pa.dsd = true;
	dsd_hold = hold;
	dsd_running = true;
	if (hold) {
		LOG_INFO("output hold at %u", pa.rate);
	} else {
		LOG_INFO("native DSD output at %u (DSD%u)", pa.rate, pa.rate * 16 / 44100);
	}
	pthread_create(&dsd_thread, NULL, dsd_writer, NULL);
}

// called with the output lock held; the writer needs the lock to finish
static void dsd_writer_stop(void) {
	dsd_running = false;
	UNLOCK;
	pthread_join(dsd_thread, NULL);
	LOCK;
	pa.dsd = false;
}
#endif

void output_init_pa(log_level level, const char *device, unsigned output_buf_size, char *params, unsigned rates[], unsigned rate_delay,
                    unsigned idle, bool use_fixed_volume) {
	PaError err;
#ifndef PA18API
	unsigned latency = 0;
	int pa_hostapi_option = -1;

#else
	unsigned pa_frames = 0;
	unsigned pa_nbufs = 0;
#endif /* PA18API */
#ifndef PA18API
	char *l = next_param(params, ':');
	char *p = next_param(NULL, ':');

	if (l) latency = (unsigned)atoi(l);
	if (p) pa_hostapi_option = atoi(p);
#else
	char *t = next_param(params, ':');
	char *c = next_param(NULL, ':');
	if (t) pa_frames  = atoi(t);
	if (c) pa_nbufs = atoi(c);
#endif

	loglevel = level;
	fixed_volume = use_fixed_volume;

	LOG_INFO("init output");

	memset(&output, 0, sizeof(output));
#if ANDROID_DSD
	dsd_refused = false;
#endif

#ifndef PA18API
	output.latency = latency;
	output.pa_hostapi_option = pa_hostapi_option;
#else
	if ( pa_frames != 0 )
		paFramesPerBuffer = pa_frames;
	if ( pa_nbufs != 0 )
		paNumberOfBuffers = pa_nbufs;
#endif /* PA18API */
	output.format = 0;
	output.start_frames = 0;
	output.write_cb = &_write_frames;
	output.rate_delay = rate_delay;
	pa.stream = NULL;

#ifndef PA18API
	LOG_INFO("requested latency: %u", output.latency);
#endif

	if ((err = Pa_Initialize()) != paNoError) {
		LOG_WARN("error initialising port audio: %s", Pa_GetErrorText(err));
		exit(0);
	}

	output_init_common(level, device, output_buf_size, rates, idle);
#if ANDROID_DSD
	memcpy(own_rates, output.supported_rates, sizeof(own_rates));
#endif

	LOCK;

	_pa_open();

	UNLOCK;
}

void output_close_pa(void) {
	PaError err;

	LOG_INFO("close output");

	LOCK;

	running = false;
	monitor_thread_running = false;

#if ANDROID_DSD
	if (pa.dsd) {
		dsd_writer_stop();
	}
#endif

	if (pa.stream) {
		if ((err = Pa_AbortStream(pa.stream)) != paNoError) {
			LOG_WARN("error closing stream: %s", Pa_GetErrorText(err));
		}
	}

	if ((err = Pa_Terminate()) != paNoError) {
		LOG_WARN("error closing port audio: %s", Pa_GetErrorText(err));
	}

	UNLOCK;

	output_close_common();
}

#endif // PORTAUDIO
